#!/usr/bin/env python3
"""Long-idle probe: does an idle session survive, or get evicted -> re-prefill?

Warms one session A, then re-touches it after:
  (a) a PURE idle (no traffic) — baseline: no pressure, should be retained;
  (b) idle UNDER CHURN — N concurrent sessions generate pressure while A sits
      idle; re-touch A and see if it was evicted (re-prefill) or retained
      (warm restore).

Verdict per re-touch by wall time vs a cold reference (a fresh session's first
turn = what a re-prefill costs) and A's own warm turn:
  re-touch wall < 0.6 x cold  -> RESTORE  (retained)
  re-touch wall > 0.8 x cold  -> RE-PREFILL (evicted)
  else                        -> AMBIGUOUS

Also records the /stats occupancy trace (device KV pages + host KV bytes) so we
can see whether A's data actually left the arena during the idle window.

Run inside e2e-swap-cmp.sh (prod stopped, test server on :8080). Env-tunable:
  IDLE_SEED IDLE_TURN PURE_IDLE_S CHURN_SESSIONS CHURN_SEED CHURN_TURN
  CHURN_ROUNDS CHURN_OUT
"""

import importlib.util
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("cmp_e2e", os.path.join(HERE, "cmp-e2e.py"))
cmp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cmp)
suite = cmp.suite


def verdict(wall, cold):
    if wall is None:
        return "error"
    if cold is None or cold <= 0:
        return "unknown"
    if wall < 0.6 * cold:
        return "restore"
    if wall > 0.8 * cold:
        return "re-prefill"
    return "ambiguous"


def safe_turn(session, index):
    """Run a turn; on error return a sentinel record so the probe survives."""
    try:
        return session.turn(index)
    except Exception as exc:
        rec = {"session": session.name, "turn": index, "wall_s": None, "error": repr(exc)}
        session.turns.append(rec)
        print(f"  TURN-ERROR {session.name} t{index}: {exc}")
        return rec


def main():
    host = os.environ.get("HOST", "127.0.0.1")
    port = int(os.environ.get("PORT", "8080"))
    model = os.environ.get("MODEL", "qwen3.8-27b")
    tag = os.environ.get("TAG", "build")

    idle_seed   = int(os.environ.get("IDLE_SEED", "40000"))
    idle_turn   = int(os.environ.get("IDLE_TURN", "1500"))
    pure_idle_s = int(os.environ.get("PURE_IDLE_S", "60"))
    churn_n     = int(os.environ.get("CHURN_SESSIONS", "8"))
    churn_seed  = int(os.environ.get("CHURN_SEED", "48000"))
    churn_turn  = int(os.environ.get("CHURN_TURN", "1500"))
    churn_rounds = int(os.environ.get("CHURN_ROUNDS", "2"))

    sargs = cmp.Args(host, port, model, 48, 600)
    A = suite.Session("IDLE_A", idle_seed, idle_turn, sargs)
    churn = [suite.Session(f"CH{i}", churn_seed, churn_turn, sargs) for i in range(churn_n)]

    poller = cmp.StatsPoller(host, port)
    log_start = suite.count_log_lines(cmp.SERVE_LOG)
    poller.start()
    t0 = time.time()

    print(f"[long-idle] A: {idle_seed} seed + {idle_turn}/turn; "
          f"churn: {churn_n}x{churn_seed} x{churn_rounds}r; pure_idle={pure_idle_s}s")

    # warm A (turn 1 = cold-ish incl. seed prefill; turn 2 = warm)
    safe_turn(A, 1)
    safe_turn(A, 2)
    warm_ref = A.turns[-1]["wall_s"]
    print(f"[long-idle] A warm: t1={A.turns[0]['wall_s']}s t2={warm_ref}s")

    # (a) pure idle, no traffic
    print(f"[long-idle] pure idle {pure_idle_s}s ...")
    time.sleep(pure_idle_s)
    rec_pure = safe_turn(A, 3)
    print(f"[long-idle] re-touch (pure): {rec_pure['wall_s']}s")

    # (b) churn under pressure while A idle
    print(f"[long-idle] churn {churn_n} sessions x {churn_rounds} rounds (A idle) ...")
    churn_errors = 0
    for r in range(1, churn_rounds + 1):
        errs = suite.run_round(churn, r, sargs.timeout)
        churn_errors += len(errs)
        for n, e in errs:
            print(f"  CHURN-ERROR {n}: {e}")
    rec_churn = safe_turn(A, 4)
    print(f"[long-idle] re-touch (churn): {rec_churn['wall_s']}s")

    # cold reference: a brand-new session's first turn (what a re-prefill costs)
    C = suite.Session("IDLE_COLD", churn_seed, churn_turn, sargs)
    rec_cold = safe_turn(C, 1)
    cold_ref = rec_cold["wall_s"]
    print(f"[long-idle] cold ref (fresh {churn_seed}-seed session): {cold_ref}s")

    poller.stop()
    wall_total = time.time() - t0

    crashes, reuse_counts = cmp.serve_log_slice(cmp.SERVE_LOG, log_start)
    reqs, _, _ = cmp.read_request_log(cmp.REQUEST_LOG)
    metrics = [cmp.request_metrics(r) for r in reqs]
    prefix_hits = sum(m["prefix_cache_hit_tokens"] for m in metrics)
    reuse_paths = {}
    for m in metrics:
        p = m["prefix_reuse_path"]
        if p:
            reuse_paths[p] = reuse_paths.get(p, 0) + 1

    # occupancy trace (downsampled to ~40 points)
    samples = poller.samples
    step = max(1, len(samples) // 40)
    trace = [
        {"t": round(s["t"] - t0, 1),
         "dev_kv_pages": s["device_main_kv_pages"],
         "host_kv_bytes": s["host_kv_bytes"],
         "waiting": s["waiting"], "running": s["running"]}
        for s in samples[::step]
    ]
    dev_max = max((s["device_main_kv_pages"] or 0) for s in samples) if samples else None
    host_max = max((s["host_kv_bytes"] or 0) for s in samples) if samples else None
    host_min_after_warm = min((s["host_kv_bytes"] or 0) for s in samples) if samples else None

    result = {
        "tag": tag, "host": host, "port": port,
        "config": {
            "idle_seed": idle_seed, "idle_turn": idle_turn, "pure_idle_s": pure_idle_s,
            "churn_sessions": churn_n, "churn_seed": churn_seed, "churn_turn": churn_turn,
            "churn_rounds": churn_rounds,
        },
        "wall_total_s": round(wall_total, 1),
        "warm_ref_s": warm_ref,
        "cold_ref_s": cold_ref,
        "re_touch_pure": {"wall_s": rec_pure["wall_s"],
                          "verdict": verdict(rec_pure["wall_s"], cold_ref)},
        "re_touch_churn": {"wall_s": rec_churn["wall_s"],
                           "verdict": verdict(rec_churn["wall_s"], cold_ref)},
        "churn_errors": churn_errors,
        "prefix_cache_hit_tokens": prefix_hits,
        "prefix_reuse_paths": reuse_paths,
        "reuse_serve_log": reuse_counts,
        "occupancy": {
            "device_kv_pages_max": dev_max,
            "host_kv_bytes_max": host_max,
            "host_kv_bytes_min": host_min_after_warm,
            "trace": trace,
        },
        "crash_markers": crashes[:10],
    }
    out_path = f"/tmp/ninfer-longidle-{tag}.json"
    with open(out_path, "w") as f:
        json.dump(result, f, indent=1)
    print(f"\n[long-idle] SUMMARY tag={tag} -> {out_path}")
    print(f"  warm={warm_ref}s cold={cold_ref}s | "
          f"re-touch pure={rec_pure['wall_s']}s ({result['re_touch_pure']['verdict']}) "
          f"churn={rec_churn['wall_s']}s ({result['re_touch_churn']['verdict']})")
    print(f"  host_kv max={host_max} min={host_min_after_warm} "
          f"dev_kv_pages_max={dev_max} crashes={len(crashes)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
