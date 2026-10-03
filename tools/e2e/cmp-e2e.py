#!/usr/bin/env python3
"""Build-agnostic A/B comparison driver (ours vs upstream v3).

Drives IDENTICAL workloads with the repo suite's session classes (workload
parity with the real e2e suite), then records build-agnostic metrics:

  - per-turn wall time (client-side: queue + prefill + decode + relief)
  - cold-vs-warm ratio (first turn vs later turns per session)
  - reuse evidence: request-log prefix_cache_hit_tokens (upstream) and/or
    serve-log reuse= lines (ours)
  - occupancy/pressure: /stats polling (ours) and/or periodic stats records
    in the request log (upstream)
  - crash/abort markers in the serve log

Usage:
  python3 cmp-e2e.py --profile focused|p13|all [--tag ours-v2] [--json out.json]

Run inside e2e-swap-cmp.sh (prod stopped, test server on :8080).
"""

import argparse
import importlib.util
import json
import os
import re
import statistics
import sys
import threading
import time
import urllib.request

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO_SUITE = os.path.join(_HERE, "ninfer-e2e.py")   # vendored with this driver
if not os.path.exists(REPO_SUITE):
    REPO_SUITE = os.path.expanduser("~/ninfer/tools/e2e/ninfer-e2e.py")
SERVE_LOG = os.path.expanduser("~/ninfer-serve.log")
REQUEST_LOG = os.path.expanduser("~/ninfer-requests.jsonl")

CRASH_RE = re.compile(
    r"bad_alloc|terminate called|Segmentation|core dumped|Killed \d|NINFER_EXIT=[1-9]"
)
REUSE_LINE_RE = re.compile(r"reuse=([a-z_]+)")


def load_suite():
    spec = importlib.util.spec_from_file_location("ninfer_e2e_suite", REPO_SUITE)
    assert spec is not None
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


suite = load_suite()


class Args:
    """Mimics the repo suite's argparse namespace (what the session classes use)."""

    def __init__(self, host, port, model, max_output_tokens, timeout):
        self.host = host
        self.port = port
        self.model = model
        self.max_output_tokens = max_output_tokens
        self.serve_log = SERVE_LOG
        self.request_log = REQUEST_LOG
        self.timeout = timeout
        self.thinking_mode = False


def _build_id():
    """sha256 (first 12) + mtime of the served binary, or a reason why it is unavailable."""
    import hashlib
    path = os.path.expanduser(os.environ.get("E2E_BIN", "~/ninfer/build/apps/ninfer-serve"))
    try:
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()[:12]
        return f"{digest}@{int(os.path.getmtime(path))}"
    except OSError as exc:
        return f"unknown ({exc})"


def _poll_max(samples, key):
    """Largest value of `key` across samples, or None if no sample carried a value."""
    vals = [s.get(key) for s in samples if s.get(key) is not None]
    return max(vals) if vals else None


class StatsPoller(threading.Thread):
    """Polls /stats (ours) every 5s; upstream has no /stats -> no data."""

    def __init__(self, host, port):
        super().__init__(daemon=True)
        self.host, self.port = host, port
        self.samples = []
        self.polls = 0
        self.skipped = 0
        # `_halt`, not `_stop`: `threading.Thread` already defines a private `_stop()` method, and
        # shadowing it makes `Thread.join()` raise TypeError('Event' object is not callable).
        self._halt = threading.Event()

    def run(self):
        while not self._halt.is_set():
            try:
                with urllib.request.urlopen(
                    f"http://{self.host}:{self.port}/stats", timeout=5
                ) as r:
                    d = json.load(r)
                self.polls += 1
                self.samples.append({
                    "t": time.time(),
                    "device_main_kv_pages": d.get("pressure", {}).get("device_main_kv_occupied_pages"),
                    "host_kv_bytes": d.get("memory", {}).get("host_kv_occupied_bytes"),
                    "dead_bytes": (d.get("host_kv", {}).get("tier_census", {}) or {}).get("dead", {}).get("bytes"),
                    "waiting": d.get("scheduler", {}).get("waiting"),
                    "running": d.get("scheduler", {}).get("running"),
                    # Cumulative planner counters, from /stats. They matter because the same names in
                    # the request log (`request_log.cpp`) are per-reporting-window MONOTONIC DELTAS,
                    # so a reader of the result JSON cannot tell a run total from a window. Whether
                    # the materialization search actually ran ("the search never began") is the
                    # load-bearing claim of the D1 fix, so it needs a number that means what it says.
                    "searches": d.get("pressure", {}).get("searches"),
                    "search_budget_exhaustions": d.get("pressure", {}).get("search_budget_exhaustions"),
                })
            except Exception:
                # Counted, not swallowed: samples are dropped exactly while the server is saturated,
                # which is the window `stats_poll_max` exists to observe (the acceptance run collected
                # 10 of ~45 possible samples). The count is reported so `poll_max` can be read as a
                # lower bound rather than the run's peak.
                self.skipped += 1
            self._halt.wait(5)

    def stop(self):
        self._halt.set()


def count_file_lines(path):
    """Line count of a log file, for per-profile windowing (see run_profile)."""
    try:
        with open(path, "r", errors="replace") as f:
            return sum(1 for _ in f)
    except OSError:
        return 0


def read_request_log(path, skip_lines=0):
    """Parse the request log JSONL.

    Upstream v3 records carry an `event` field:
      request_start / request_done / request_rejected / throughput / server_start
    Ours (v2 tree) has no `event` field — fall back to key sniffing.
    `skip_lines` drops records logged before this profile started: the log is
    append-only for the life of the server, so without it every profile in a
    multi-profile run reports cumulative metrics (all four profiles reported the
    same 248.9 s max queue wait and monotonically increasing hit counts).
    Returns (requests, stats_records, rejections).
    """
    requests, stats_records, rejections = [], [], []
    try:
        with open(path, "r", errors="replace") as f:
            for index, line in enumerate(f):
                if index < skip_lines:
                    continue
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                ev = rec.get("event")
                if ev == "request_done":
                    requests.append(rec)
                elif ev == "request_rejected":
                    rejections.append(rec)
                elif ev == "throughput" or "occupancy" in rec or "pressure" in rec or "scheduler" in rec:
                    stats_records.append(rec)
                elif any(k in rec for k in ("prefix_cache_hit_tokens", "prompt_n",
                                            "request_id", "timings", "completion")):
                    requests.append(rec)
    except OSError:
        pass
    return requests, stats_records, rejections


def request_metrics(rec):
    """Normalize a request_done record (upstream) or legacy record (ours)."""
    if "result" in rec and isinstance(rec.get("result"), dict):
        res = rec["result"]
        timings = rec.get("timings_seconds", {}) or {}
        eng = rec.get("engine_timing", {}) or {}
        return {
            "prefix_cache_hit_tokens": res.get("prefix_cache_hit_tokens", 0) or 0,
            "prefix_reuse_path": res.get("prefix_reuse_path"),
            "computed_prefill_tokens": res.get("computed_prefill_tokens"),
            "prompt_tokens": res.get("prompt_tokens"),
            "ttft_s": timings.get("ttft"),
            "queue_wait_s": eng.get("queue_wait_seconds"),
            "materialization_stop_reason": (rec.get("materialization", {}) or {}).get("stop_reason"),
        }
    return {
        "prefix_cache_hit_tokens": rec.get("prefix_cache_hit_tokens", 0) or 0,
        "prefix_reuse_path": rec.get("prefix_reuse_path"),
        "computed_prefill_tokens": None,
        "prompt_tokens": rec.get("prompt_tokens"),
        "ttft_s": None,
        "queue_wait_s": None,
        "materialization_stop_reason": None,
    }


def serve_log_slice(path, start_line):
    """Serve-log lines since start_line (crash markers + reuse lines)."""
    crashes, reuse_counts = [], {}
    try:
        with open(path, "r", errors="replace") as f:
            for i, line in enumerate(f):
                if i < start_line:
                    continue
                if CRASH_RE.search(line):
                    crashes.append(line.strip()[:200])
                for m in REUSE_LINE_RE.finditer(line):
                    reuse_counts[m.group(1)] = reuse_counts.get(m.group(1), 0) + 1
    except OSError:
        pass
    return crashes, reuse_counts


def run_profile(name, sessions, rounds, args, poller):
    print(f"\n=== {name}: {len(sessions)} sessions x {rounds} rounds ===")
    log_start = suite.count_log_lines(SERVE_LOG)
    request_log_start = count_file_lines(REQUEST_LOG)
    poller.start()
    t0 = time.time()
    round_errors = 0
    round_reuse_paths = []
    round_queue_waits = []
    for r in range(1, rounds + 1):
        print(f"Round {r}:")
        round_request_start = count_file_lines(REQUEST_LOG)
        errors = suite.run_round(sessions, r, args.timeout)
        round_errors += len(errors)
        round_rows, _, _ = read_request_log(REQUEST_LOG, round_request_start)
        round_paths = {}
        round_waits = []
        for rec in (request_metrics(x) for x in round_rows):
            rp = rec["prefix_reuse_path"]
            if rp:
                round_paths[rp] = round_paths.get(rp, 0) + 1
            if rec["queue_wait_s"] is not None:
                round_waits.append(rec["queue_wait_s"])
        round_reuse_paths.append(round_paths)
        round_queue_waits.append(round_waits)
        if errors:
            for n, e in errors:
                print(f"  ERROR {n}: {e}")
            time.sleep(3)
            failed = [s for s in sessions if s.name in [n for n, _ in errors]]
            errors2 = suite.run_round(failed, r, args.timeout)
            # Count them: a turn that fails twice was invisible to the gate before.
            round_errors += len(errors2)
            for n, e in errors2:
                print(f"  RETRY-ERROR {n}: {e}")
    wall_total = time.time() - t0
    poller.stop()
    crashes, reuse_counts = serve_log_slice(SERVE_LOG, log_start)
    reqs, stats_recs, rejections = read_request_log(REQUEST_LOG, request_log_start)

    # per-request metrics (upstream request_done records; ours: legacy shape)
    metrics = [request_metrics(r) for r in reqs]
    prefix_hits = sum(m["prefix_cache_hit_tokens"] for m in metrics)
    reuse_paths = {}
    stop_reasons = {}
    for m in metrics:
        p = m["prefix_reuse_path"]
        if p:
            reuse_paths[p] = reuse_paths.get(p, 0) + 1
        sr = m["materialization_stop_reason"]
        if sr:
            stop_reasons[sr] = stop_reasons.get(sr, 0) + 1
    ttfts = [m["ttft_s"] for m in metrics if m["ttft_s"] is not None]
    queue_waits = [m["queue_wait_s"] for m in metrics if m["queue_wait_s"] is not None]

    # upstream periodic stats: last throughput record's occupancy/pressure
    last_stats = stats_recs[-1] if stats_recs else {}
    cc = last_stats.get("context_cache", {}) or {}
    occ = cc.get("occupancy", {}) or last_stats.get("occupancy", {}) or {}
    pres = cc.get("pressure", {}) or last_stats.get("pressure", {}) or {}

    # per-session cold/warm
    per_session = []
    for s in sessions:
        walls = [t["wall_s"] for t in s.turns]
        if not walls:
            continue
        per_session.append({
            "session": s.name,
            "turns": len(walls),
            "first_s": round(walls[0], 1),
            "last_s": round(walls[-1], 1),
            "warm_ratio": round(walls[-1] / walls[0], 2) if walls[0] > 0 else None,
            "mean_s": round(statistics.mean(walls), 1),
        })
    all_walls = [t["wall_s"] for s in sessions for t in s.turns]
    result = {
        "profile": name,
        "wall_total_s": round(wall_total, 1),
        "n_turns": len(all_walls),
        # From the round loop's error list, not from turn records: those never carry an "error" key,
        # so this field used to be structurally always 0 and the gate's check on it was inert.
        "n_errors": round_errors,
        "round_reuse_paths": round_reuse_paths,
        "round_queue_waits": round_queue_waits,
        "turn_wall_s": {
            "mean": round(statistics.mean(all_walls), 1) if all_walls else None,
            "p95": round(sorted(all_walls)[int(0.95 * len(all_walls))], 1) if all_walls else None,
            "max": round(max(all_walls), 1) if all_walls else None,
        },
        "per_session": per_session,
        "reuse_serve_log": reuse_counts,
        "prefix_cache_hit_tokens": prefix_hits,
        "prefix_reuse_paths": reuse_paths,
        "materialization_stop_reasons": stop_reasons,
        "ttft_s": {
            "mean": round(statistics.mean(ttfts), 2) if ttfts else None,
            "p95": round(sorted(ttfts)[int(0.95 * len(ttfts))], 2) if ttfts else None,
        } if ttfts else None,
        "queue_wait_s": {
            "mean": round(statistics.mean(queue_waits), 3) if queue_waits else None,
            "max": round(max(queue_waits), 3) if queue_waits else None,
        } if queue_waits else None,
        "request_rejections": len(rejections),
        "request_log_records": len(reqs),
        "crash_markers": crashes[:10],
        "stats_last": {
            "occupancy": occ,
            "pressure": {k: v for k, v in pres.items() if isinstance(v, (int, float))},
        } if last_stats else None,
        # Every key goes through _poll_max: None when no sample carried it, never a literal 0. The
        # first version applied that to the two new counters and left the other four on
        # `max((s[k] or 0) ...)`, which puts "measured nothing" and "measured zero" in the same
        # artifact for exactly the fields a reader uses to judge pressure.
        "stats_poll_max": {
            "device_main_kv_pages": _poll_max(poller.samples, "device_main_kv_pages"),
            "host_kv_bytes": _poll_max(poller.samples, "host_kv_bytes"),
            "dead_bytes": _poll_max(poller.samples, "dead_bytes"),
            "waiting": _poll_max(poller.samples, "waiting"),
            # These two matter most: "the search never ran" is the load-bearing claim of the D1 fix.
            "searches": _poll_max(poller.samples, "searches"),
            "search_budget_exhaustions": _poll_max(poller.samples, "search_budget_exhaustions"),
            "stats_poll_samples": len(poller.samples),
            "stats_poll_skipped": poller.skipped,   # dropped polls: poll_max is a lower bound
            # attempts = successes + failures; `polls` alone counts only the successes, so a
            # "attempts/skipped" rate computed from the pair would be backwards.
            "stats_poll_attempts": poller.polls + poller.skipped,
        } if poller.samples else None,
    }
    print(json.dumps(result, indent=1))
    return result


def gate_result(r, expect_trash=False):
    """Hard failure conditions (W4b) — a run must be able to FAIL.

    v3 evidence only: request-log reuse paths / hits / queue waits, turn errors,
    crash markers, request rejections. A profile whose re-touches all re-prefill
    from root, or whose clients waited behind re-prefills, is a failure even
    though nothing crashed.
    """
    fails = []
    # No data is a failure, never a pass: an empty request log, a format change, or an over-wide
    # window would otherwise silently convert "nothing measured" into "measured clean".
    if not r.get("n_turns"):
        fails.append("no completed turns")
    if not r.get("request_log_records"):
        fails.append("no request-log records in this profile's window")
    # Root share is per-round from round 2 on: round 1 legitimately prefills from root, and an
    # aggregate denominator lets a real collapse dilute below threshold (382/1950 = 19.6% while
    # rounds 2+ re-prefilled from root). Falls back to the aggregate only if rounds are unavailable.
    round_paths = r.get("round_reuse_paths") or []
    if not expect_trash:
        # Prefer per-round accounting (round 1 legitimately prefills from root). If a result predates
        # that field, fall back to the aggregate -- and if neither exists, FAIL: skipping the check
        # on missing data is how a gate silently converts "nothing measured" into "measured clean".
        if round_paths:
            scoped = round_paths[1:] if len(round_paths) > 1 else round_paths
            paths = {}
            for rp in scoped:
                for k, v in (rp or {}).items():
                    paths[k] = paths.get(k, 0) + v
            scope = "rounds 2+" if len(round_paths) > 1 else "all rounds"
        else:
            paths = dict(r.get("prefix_reuse_paths") or {})
            scope = "all rounds (aggregate; no per-round data in this result)"
        n = sum(paths.values())
        if not n:
            fails.append("no prefix-reuse accounting at all (cannot judge root share)")
        else:
            root_share = paths.get("root", 0) / n
            if root_share > 0.25:
                fails.append(f"root-prefill share {root_share:.0%} > 25% ({scope}, {paths})")
            if not r.get("prefix_cache_hit_tokens"):
                fails.append(f"zero prefix-cache hits over {n} requests ({paths})")
    # Queue waits are scoped like root share: round 1 is a cold cache (four 150k-token prompts
    # arriving together and prefilled one lane at a time, ~75-150 s of unavoidable queueing), which
    # no cache policy can remove. The collapse's signature is queueing AFTER the first round.
    waits = r.get("round_queue_waits") or []
    if len(waits) > 1:
        later = [x for per_round in waits[1:] for x in (per_round or [])]
        q = max(later) if later else None
        scope = "rounds 2+"
        # The exempted first round still gets a loose bound: a cold start is allowed to queue once
        # per session, but a regression there (e.g. 400 s) must not pass silently. 240 s is a
        # deliberately loose regression guard -- 1.65x the highest round-1 maximum in any surviving
        # prod4 artifact (145.3 s in the collapse-with-fix run, 143.9 s in the acceptance run,
        # 143.5 s in the narrowed-window arm). An earlier version of this comment claimed ~1.25x of
        # a ~192 s run; no artifact contains a round-1 wait above 145.3 s, so that calibration
        # story was wrong. Nothing tighter can be justified while the cold start's spread is
        # measured at only 143-145 s in three runs -- but a bound nothing can reach is a guard
        # whose only value is against gross regression, and it should be read that way.
        first_round = [x for x in (waits[0] or [])]
        if first_round and max(first_round) > 240:
            fails.append(f"round-1 queue wait {max(first_round):.0f}s > 240s (cold start exceeded "
                         f"its loose bound)")
    else:
        q = (r.get("queue_wait_s") or {}).get("max")
        scope = "all rounds (no per-round data)"
    if q is not None and q > 60:
        fails.append(f"max queue wait {q:.0f}s > 60s ({scope}, requests waited behind re-prefills)")
    if r.get("n_errors"):
        fails.append(f"{r['n_errors']} turn errors")
    if r.get("crash_markers"):
        fails.append(f"{len(r['crash_markers'])} crash markers")
    if r.get("request_rejections"):
        fails.append(f"{r['request_rejections']} request rejections")
    return fails


def main():
    p = argparse.ArgumentParser()
    # CMP_PROFILE lets the swap driver select a single profile without extra CLI plumbing (the
    # driver only forwards --start-phase).
    p.add_argument("--profile", default=os.environ.get("CMP_PROFILE", "all"),
                   choices=["focused", "p13", "p14", "trash", "prod4", "all"])
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--model", default="qwen3.8-27b")
    p.add_argument("--tag", default="build")
    p.add_argument("--json", dest="json_out", default=None)
    p.add_argument("--no-gates", action="store_true",
                   help="report metrics without failing the run")
    # Accepted and ignored: the swap driver passes --start-phase to every suite under tools/e2e,
    # and rejecting it is why the W4b prod4 profile had never actually been run.
    p.add_argument("--start-phase", type=int, default=1)
    args = p.parse_args()

    sargs = Args(args.host, args.port, args.model, 48, 600)
    results = []
    if args.profile in ("focused", "all"):
        # focused suite shape: 3 sessions, 14k seed + 2k/turn, 8 rounds, 48 out
        ss = [suite.Session(f"AB{i}", 14000, 2000, sargs) for i in range(3)]
        results.append(run_profile("focused", ss, 8, sargs, StatsPoller(args.host, args.port)))
    if args.profile in ("p13", "all"):
        # phase 13 shape: 6 tool-calling sessions, 10k seed + 1.5k/turn, 8 rounds
        s13args = Args(args.host, args.port, args.model, 512, 600)
        ss = [suite.ChatSession(f"SS{i}", 10000, 1500, s13args) for i in range(6)]
        rounds = int(os.environ.get("PHASE13_ROUNDS", "8"))
        results.append(run_profile("p13-state-saturation", ss, rounds, s13args, StatsPoller(args.host, args.port)))
    if args.profile in ("p14", "all"):
        # phase 14 shape: 4 sessions, 24k seed + 1k/turn, 3 rounds
        s14args = Args(args.host, args.port, args.model, 512, 600)
        ss = [suite.Session(f"QR{i}", 24000, 1000, s14args) for i in range(4)]
        results.append(run_profile("p14-queued-relief", ss, 3, s14args, StatsPoller(args.host, args.port)))
    if args.profile in ("trash", "all"):
        # phase 3 (trash) shape: 10 sessions, 15k seed + 1.5k/turn, 6 rounds.
        # Env-tunable for the big-session retention test (bigger seed/turn).
        t_sessions = int(os.environ.get("TRASH_SESSIONS", "10"))
        t_seed     = int(os.environ.get("TRASH_SEED", "15000"))
        t_turn     = int(os.environ.get("TRASH_TURN", "1500"))
        t_rounds   = int(os.environ.get("TRASH_ROUNDS", "6"))
        t_out      = int(os.environ.get("TRASH_OUT", "48"))
        s3args = Args(args.host, args.port, args.model, t_out, 600)
        ss = [suite.Session(f"S{i}", t_seed, t_turn, s3args) for i in range(t_sessions)]
        results.append(run_profile(
            f"trash-{t_sessions}x{t_seed}", ss, t_rounds, s3args,
            StatsPoller(args.host, args.port)))

    if args.profile == "prod4":
        # W4b: prod SLOT SHAPE (c=4, device-state-slots=4, host-state-slots=16) at
        # 4 concurrent heavy sessions, LIGHT host KV (server: CTX=prod4). This is
        # the configuration that collapsed prod: every re-touch re-prefilled.
        p4_sessions = int(os.environ.get("PROD4_SESSIONS", "4"))
        p4_seed = int(os.environ.get("PROD4_SEED", "150000"))
        p4_turn = int(os.environ.get("PROD4_TURN", "2000"))
        p4_rounds = int(os.environ.get("PROD4_ROUNDS", "4"))
        p4args = Args(args.host, args.port, args.model, 512, 900)
        ss = [suite.Session(f"P4_{i}", p4_seed, p4_turn, p4args) for i in range(p4_sessions)]
        results.append(run_profile(f"prod4-{p4_sessions}x{p4_seed}", ss, p4_rounds,
                                   p4args, StatsPoller(args.host, args.port)))

    # Build identity: every artifact used to carry only `tag` (always "build"), so two runs of the
    # same profile against different binaries were indistinguishable -- which is how a 32 ms arm's
    # result file ended up identical to an 800 ms arm's with nothing to tell them apart.
    out = {"tag": args.tag, "build_id": _build_id(), "host": args.host, "port": args.port,
           # The arm, self-described: the binary is not enough (two arms of one binary differ only by
           # this), and a reader of the artifact should not have to reconstruct the env from the log.
           "arm": {"search_ms": os.environ.get("NINFER_SEARCH_MS", "default"),
                   "ctx": os.environ.get("CTX", "unset"),
                   "spec": os.environ.get("SPEC", "unset"),
                   "hint": os.environ.get("CMP_PROFILE", "unset")},
           "results": results}
    # The arm is part of the file name when the arm is a knob: two arms of ONE binary both wrote
    # `/tmp/ninfer-cmp-build.json`, which is how a 32 ms arm's file ended up indistinguishable from an
    # 800 ms arm's. `build_id` alone cannot separate them -- same build, different window.
    arm = os.environ.get("NINFER_SEARCH_MS")
    suffix = f"-search{arm}" if arm else ""
    out_path = args.json_out or f"/tmp/ninfer-cmp-{args.tag}{suffix}.json"
    with open(out_path, "w") as f:
        json.dump(out, f, indent=1)
    print(f"\nSUMMARY tag={args.tag} -> {out_path}")
    for r in results:
        tw = r["turn_wall_s"]
        print(f"  {r['profile']}: turns={r['n_turns']} mean={tw['mean']}s p95={tw['p95']}s "
              f"crashes={len(r['crash_markers'])} prefix_hits={r['prefix_cache_hit_tokens']} "
              f"reuse_log={r['reuse_serve_log']}")

    fails = []
    for r in results:
        fails += gate_result(r, expect_trash=r["profile"].startswith("trash"))
    if fails and not args.no_gates:
        print("\nGATES FAILED:")
        for f in fails:
            print(f"  FAIL: {f}")
        return 1
    if fails:
        print("\n(gates reported, --no-gates: exit 0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
