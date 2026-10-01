#!/usr/bin/env python3
"""Gate-probe: minimal driver to reproduce the retention gate on a resident
re-touch, with the instrumented serve binary (NINFER_MAT_DEBUG=1).

Needs >= ceil(device_pool / seed) sessions so the device KV pool is oversubscribed
and a re-touch must restore from the host arena (with neighbor demotions) -- the
exact regime where `insufficient_expected_gain` fires.

  PORT=8085 N=3 SEED=120000 python3 gate-probe.py
"""
import importlib.util
import os
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("cmpe2e", os.path.join(HERE, "cmp-e2e.py"))
_m = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_m)
suite = _m.suite
Args = _m.Args

PORT = int(os.environ.get("PORT", os.environ.get("TEST_PORT", "8085")))
N = int(os.environ.get("N", "3"))
SEED = int(os.environ.get("SEED", "120000"))
TURN = int(os.environ.get("TURN", "1500"))
ROUNDS = int(os.environ.get("ROUNDS", "2"))

args = Args("127.0.0.1", PORT, "qwen3.8-27b", 48, 600)
ss = [suite.Session(f"P{i}", SEED, TURN, args) for i in range(N)]

for r in range(1, ROUNDS + 1):
    print(f"\n=== Round {r} ({'establish' if r == 1 else 're-touch'}) ===", flush=True)
    t0 = time.monotonic()
    errs = suite.run_round(ss, r, args.timeout)
    print(f"    round wall={time.monotonic() - t0:.1f}s errs={errs}", flush=True)
    if errs:
        time.sleep(3)
        failed = [s for s in ss if s.name in [n for n, _ in errs]]
        for n, e in suite.run_round(failed, r, args.timeout):
            print(f"    RETRY-ERROR {n}: {e}", flush=True)

print("\nprobe done", flush=True)
