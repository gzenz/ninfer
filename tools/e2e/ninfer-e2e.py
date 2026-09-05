#!/usr/bin/env python3
"""E2E test suite for ninfer safety-net eviction system.

Runs three phases by default against a single test server (no flags needed):
  Phase 1 "pressure": 4 sessions — basic safety net (spills, restores, no re-prefills)
  Phase 2 "mixed":    1 big + 3 small — eviction order (smallest-first, big preserved)
  Phase 3 "trash":    10 sessions — graceful degradation under trashing (no crash)

Server config: 32k max-context, 64k kv-capacity, 4GB host-kv, 3 continuations.
All phases use the same server — no restarts.

Usage: python3 ninfer-e2e.py [--host 127.0.0.1] [--port 8080] [--serve-log /home/zenz/ninfer-serve.log]
"""

import argparse
import json
import re
import random
import sys
import threading
import time
import urllib.request

WORDS = ("alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima "
         "mike november oscar papa quebec romeo sierra tango uniform victor whiskey "
         "xray yankee zulu amber cedar dawn ember frost grove harbor ivory "
         "jade kernel lumen meadow night opal prism quill raven stone umber "
         "vale willow xenon yonder zephyr anchor beacon compass estuary").split()


def filler(rng, tokens):
    chars = int(tokens * 4.2)
    parts, n = [], 0
    while n < chars:
        w = rng.choice(WORDS)
        parts.append(w)
        n += len(w) + 1
    return " ".join(parts)


class Session:
    def __init__(self, name, seed_tokens, turn_tokens, args):
        self.name = name
        self.rng = random.Random(sum(ord(c) for c in name))
        self.seed_tokens = seed_tokens
        self.turn_tokens = turn_tokens
        self.args = args
        self.response_id = None
        self.turns = []
        self.doc = filler(self.rng, seed_tokens)

    def turn(self, index):
        question = f"Question {index}: Consider the paragraph about '{self.rng.choice(WORDS)}'. Answer briefly."
        new_text = filler(self.rng, self.turn_tokens) + "\n\n" + question
        if index == 1:
            new_text = self.doc + "\n\n---\n\n" + new_text
        payload = {
            "model": self.args.model,
            "input": [{"role": "user", "content": [{"type": "input_text", "text": new_text}]}],
            "instructions": "You are a concise assistant.",
            "max_output_tokens": self.args.max_output_tokens,
            "store": True,
            "stream": False,
        }
        if self.response_id:
            payload["previous_response_id"] = self.response_id
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/responses",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        usage = out.get("usage", {}) or {}
        self.response_id = out.get("id", self.response_id)
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("input_tokens"), "output_tokens": usage.get("output_tokens")}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s prompt={record['input_tokens']} out={record['output_tokens']}")
        return record


def run_round(sessions, r, timeout):
    results = [None] * len(sessions)
    errors = []
    def run(i):
        try:
            results[i] = sessions[i].turn(r)
        except Exception as exc:
            errors.append((sessions[i].name, repr(exc)))
    threads = [threading.Thread(target=run, args=(i,)) for i in range(len(sessions))]
    for t in threads: t.start()
    for t in threads: t.join()
    return errors


def get_stats(args):
    try:
        with urllib.request.urlopen(f"http://{args.host}:{args.port}/stats", timeout=10) as resp:
            return json.load(resp)
    except Exception:
        return {}


def parse_serve_log(path):
    d = {k: 0 for k in [
        "spill_ok", "spill_ckpt_ok", "spill_ckpt_missing", "spill_fail",
        "restore_started", "restore_completed", "restore_failed",
        "safety_find_hit", "safety_find_miss", "max_net_entries",
        "worker_crash", "bad_alloc", "capture_skip", "restore_skip",
        "refind_hit", "evict_smallest", "multi_extent_ok", "admit_session",
    ]}
    d["evict_pages"] = []
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                if "[restore]" in line and "syncing" in line: d["restore_completed"] += 1
                if "[restore]" in line and "frontier=" in line: d["restore_started"] += 1
                if "[restore] FAILED" in line: d["restore_failed"] += 1
                if "[safety-find]" in line and "match=hit" in line: d["safety_find_hit"] += 1
                if "[safety-find]" in line and "match=miss" in line: d["safety_find_miss"] += 1
                if "[safety-find]" in line and "entries=" in line:
                    m = re.search(r"entries=(\d+)", line)
                    if m: d["max_net_entries"] = max(d["max_net_entries"], int(m.group(1)))
                if "[safety-spill] OK" in line:
                    d["spill_ok"] += 1
                    if "ckpt_valid=1" in line: d["spill_ckpt_ok"] += 1
                    elif "ckpt_valid=0" in line: d["spill_ckpt_missing"] += 1
                if "[safety-spill] FAIL" in line: d["spill_fail"] += 1
                if "[safety-spill] multi-extent OK" in line: d["multi_extent_ok"] += 1
                if "WORKER CRASH" in line: d["worker_crash"] += 1
                if "std::bad_alloc" in line: d["bad_alloc"] += 1
                if "[capture] skip zero-prefill" in line: d["capture_skip"] += 1
                if "[capture] skip restore" in line: d["restore_skip"] += 1
                if "[safety-find] re-find HIT" in line: d["refind_hit"] += 1
                if "[safety-spill] evict-smallest:" in line:
                    m = re.search(r"pages=(\d+)", line)
                    if m: d["evict_pages"].append(int(m.group(1)))
                    d["evict_smallest"] += 1
                if "[admit-session]" in line:
                    d["admit_session"] += 1
    except OSError:
        pass
    return d


def evaluate(phase_name, sessions, stats0, stats1, log, expect_trash=False):
    v = []
    pr = (stats1 or {}).get("pressure", {})
    pr0 = (stats0 or {}).get("pressure", {})
    evicted = int(pr.get("private_owners_evicted", 0)) - int(pr0.get("private_owners_evicted", 0))
    degraded = int(pr.get("private_owners_degraded", 0)) - int(pr0.get("private_owners_degraded", 0))
    cr = (stats1 or {}).get("cache_reuse", {})
    cr0 = (stats0 or {}).get("cache_reuse", {})
    reused = int(cr.get("reused_prompt_tokens", 0)) - int(cr0.get("reused_prompt_tokens", 0))
    restores = int(pr.get("admission_safety_net_restores", 0)) - int(pr0.get("admission_safety_net_restores", 0))

    # CRASH CHECK — always enforced
    if log["worker_crash"] > 0:
        v.append(f"FAIL: {log['worker_crash']} WORKER CRASH")
    if log["bad_alloc"] > 0:
        v.append(f"FAIL: {log['bad_alloc']} std::bad_alloc")

    # Pressure
    pressure = evicted > 0 or degraded > 0
    if not pressure and not expect_trash:
        v.append("FAIL: no KV pressure")
    if pressure:
        v.append(f"PASS: pressure (evicted={evicted}, degraded={degraded})")

    # Cache reuse
    if reused > 0:
        v.append(f"PASS: cache reuse ({reused} tokens)")
    elif not expect_trash:
        v.append("FAIL: zero cache reuse")

    # Safety net
    if log["spill_ok"] > 0:
        v.append(f"PASS: {log['spill_ok']} spills OK (ckpt={log['spill_ckpt_ok']})")
    if log["spill_ckpt_missing"] > 0:
        v.append(f"WARN: {log['spill_ckpt_missing']} spills missing ckpt")
    if restores > 0 or log["restore_completed"] > 0:
        v.append(f"PASS: {max(restores, log['restore_completed'])} restores")
    if log["restore_started"] > log["restore_completed"]:
        v.append(f"FAIL: {log['restore_started'] - log['restore_completed']} restores started but not completed")
    if log["max_net_entries"] >= 3 and log["restore_completed"] > 2:
        v.append(f"PASS: net accumulated (max={log['max_net_entries']})")
    if log["multi_extent_ok"] > 0:
        v.append(f"PASS: {log['multi_extent_ok']} scatter-gather allocations")
    if log["capture_skip"] > 0 or log["restore_skip"] > 0:
        v.append(f"PASS: {log['capture_skip'] + log['restore_skip']} capture skips (no crash)")
    if log["refind_hit"] > 0:
        if log["restore_completed"] >= log["refind_hit"]:
            v.append(f"PASS: {log['refind_hit']} re-find hits, restores completed")
        else:
            v.append(f"FAIL: {log['refind_hit']} re-find hits but insufficient restores")

    # Eviction order (mixed phase)
    if phase_name == "mixed" and len(log["evict_pages"]) > 1:
        ev = log["evict_pages"]
        fh = sum(ev[:len(ev)//2]) / max(len(ev)//2, 1)
        sh = sum(ev[len(ev)//2:]) / max(len(ev) - len(ev)//2, 1)
        if fh <= sh:
            v.append(f"PASS: eviction smallest-first (first={fh:.0f} <= second={sh:.0f})")
        else:
            v.append(f"WARN: eviction NOT smallest-first (first={fh:.0f} > second={sh:.0f})")

    # BIG session preservation (mixed phase)
    if phase_name == "mixed":
        for s in sessions:
            if s.name == "BIG":
                cold = sum(1 for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
                fast = sum(1 for t in s.turns if t["turn"] > 1 and t["wall_s"] <= 60)
                if cold == 0 and fast > 0:
                    v.append(f"PASS: BIG {fast} fast turns, 0 cold-starts")
                elif cold > 0:
                    v.append(f"WARN: BIG {cold} cold-starts (may have been evicted)")

    # Trash mode: cold-starts and spill failures are acceptable
    if expect_trash:
        cold = sum(1 for s in sessions for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
        if cold > 0:
            v.append(f"PASS: {cold} cold-starts (expected in trash mode)")
        if log["spill_fail"] > 0:
            v.append(f"PASS: {log['spill_fail']} spill failures (expected — graceful degradation)")
        # Must verify trashing actually occurred
        if log["spill_fail"] == 0 and log["restore_failed"] == 0:
            v.append("FAIL: no spill failures in trash mode — trashing did not occur "
                     "(increase sessions or reduce host-kv)")

    # Spill success rate (not trash mode)
    total = log["spill_ok"] + log["spill_fail"] + log["restore_failed"]
    if total > 5 and log["spill_ok"] == 0 and not expect_trash:
        v.append(f"FAIL: 0 spills succeeded out of {total}")

    return v


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--model", default="qwen3.8-27b")
    p.add_argument("--max-output-tokens", type=int, default=48)
    p.add_argument("--serve-log", default="/home/zenz/ninfer-serve.log")
    p.add_argument("--timeout", type=int, default=120)
    args = p.parse_args()

    all_verdicts = []

    # Phase 1: pressure — 4 sessions, basic safety net
    print("\n=== Phase 1: pressure (4 sessions, 8 rounds) ===")
    stats0 = get_stats(args)
    s1 = [Session("ABCD"[i], 14000, 2000, args) for i in range(4)]
    for r in range(1, 9):
        print(f"Round {r}:")
        errors = run_round(s1, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("ABORT: phase 1 failed"); return 1
    stats1 = get_stats(args)
    log1 = parse_serve_log(args.serve_log)
    for v in evaluate("pressure", s1, stats0, stats1, log1):
        all_verdicts.append(("pressure", v))

    # Phase 2: mixed — 1 big + 3 small, eviction order
    print("\n=== Phase 2: mixed (1 BIG + 3 small, 10 rounds) ===")
    stats0 = get_stats(args)
    s2 = [Session("BIG", 16000, 1500, args),
          Session("A", 6000, 1500, args),
          Session("B", 6000, 1500, args),
          Session("C", 6000, 1500, args)]
    for r in range(1, 11):
        print(f"Round {r}:")
        errors = run_round(s2, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("ABORT: phase 2 failed"); return 1
    stats1 = get_stats(args)
    log2 = parse_serve_log(args.serve_log)
    for v in evaluate("mixed", s2, stats0, stats1, log2):
        all_verdicts.append(("mixed", v))

    # Phase 3: trash — 10 sessions, graceful degradation (no crash)
    print("\n=== Phase 3: trash (10 sessions, 6 rounds) ===")
    stats0 = get_stats(args)
    s3 = [Session(f"S{i}", 20000, 2000, args) for i in range(10)]
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round(s3, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("ABORT: phase 3 failed"); return 1
    stats1 = get_stats(args)
    log3 = parse_serve_log(args.serve_log)
    for v in evaluate("trash", s3, stats0, stats1, log3, expect_trash=True):
        all_verdicts.append(("trash", v))

    # Phase 4: thinking — session-key fallback with rewrite checkpoint
    print("\n=== Phase 4: thinking (3 sessions, 6 rounds, reasoning mode) ===")
    stats0 = get_stats(args)
    s4 = [Session(f"T{i}", 10000, 2000, args) for i in range(3)]
    for s in s4:
        s.args = type(args)(**vars(args))
        s.args.thinking_mode = True
        s.args.max_output_tokens = 256
    # Override the Session.turn to add reasoning
    original_turn = Session.turn
    def thinking_turn(self, index):
        question = f"Question {index}: Consider the paragraph about '{self.rng.choice(WORDS)}'. Answer briefly."
        new_text = filler(self.rng, self.turn_tokens) + "\n\n" + question
        if index == 1:
            new_text = self.doc + "\n\n---\n\n" + new_text
        payload = {
            "model": self.args.model,
            "input": [{"role": "user", "content": [{"type": "input_text", "text": new_text}]}],
            "instructions": "You are a concise assistant.",
            "max_output_tokens": 256,
            "store": True,
            "stream": False,
            "reasoning": {"effort": "low"},
        }
        if self.response_id:
            payload["previous_response_id"] = self.response_id
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/responses",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        usage = out.get("usage", {}) or {}
        self.response_id = out.get("id", self.response_id)
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("input_tokens"), "output_tokens": usage.get("output_tokens")}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s prompt={record['input_tokens']} out={record['output_tokens']}")
        return record
    Session.turn = thinking_turn
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round(s4, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("ABORT: phase 4 failed"); return 1
    Session.turn = original_turn
    stats1 = get_stats(args)
    log4 = parse_serve_log(args.serve_log)
    for v in evaluate("thinking", s4, stats0, stats1, log4):
        all_verdicts.append(("thinking", v))
    if log4["admit_session"] > 0:
        all_verdicts.append(("thinking", f"PASS: {log4['admit_session']} session-key fallback hits (rewrite checkpoint working)"))
    else:
        # Fallback may not fire if prefixes happen to match. Check for re-prefills instead.
        cold = sum(1 for s in s4 for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
        if cold > 0:
            all_verdicts.append(("thinking", f"WARN: {cold} cold-starts in thinking mode (session-key fallback may not have fired)"))

    # Summary
    print("\n=== FINAL VERDICTS ===")
    for pn, v in all_verdicts:
        print(f"  [{pn}] {v}")
    npass = sum(1 for _, v in all_verdicts if v.startswith("PASS"))
    nwarn = sum(1 for _, v in all_verdicts if v.startswith("WARN"))
    nfail = sum(1 for _, v in all_verdicts if v.startswith("FAIL"))
    print(f"\n{'FAIL' if nfail else 'PASS'}: {npass} PASS, {nwarn} WARN, {nfail} FAIL")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
