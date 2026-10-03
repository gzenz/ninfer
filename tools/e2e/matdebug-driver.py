#!/usr/bin/env python3
"""Run a FOCUSED slice of the e2e suite, for a run that has the planner's own ASSESS lines on.

`NINFER_MAT_DEBUG=1` makes `materialization_planner.h` print one line per assessment
(`phys/now/fut/total/reused_tok/bytes` and `incumbent_now ->inc=`), which is the only way to see WHAT the
election compared -- the request log's `candidates[]` rows record the per-candidate best, and whether that
is the cost the election ranked is not determinable from them (see `plan.md`, the 2 domination hits).

WHY A DRIVER AND NOT THE SUITE. The debug stream is large and it lands on the paths the suite times, so a
full 14-phase run with it on is both slow and perturbed. The phase that produced the shape is `phase_1`
(pressure: four 14000-token sessions, 8 rounds), so that is what this runs by default.

  PORT=8085 python3 matdebug-driver.py --phases 1
  NINFER_MAT_DEBUG=1 E2E_SUITE=.../matdebug-driver.py E2E_SUITE_ARGS='--phases 1' E2E_TIMEOUT=0 \
    bash tools/e2e/e2e-swap.sh
"""
import argparse
import importlib.util
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("e2esuite", os.path.join(HERE, "ninfer-e2e.py"))
suite = importlib.util.module_from_spec(_spec)
try:
    _spec.loader.exec_module(suite)
except SystemExit:  # the suite's `if __name__ == "__main__"` guard should prevent this; be safe
    pass


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=int(os.environ.get("PORT", 8085)))
    p.add_argument("--phases", default="1",
                   help="comma-separated phase numbers, e.g. '1' or '1,2'")
    p.add_argument("--timeout", type=int, default=180)
    args, _ = p.parse_known_args()  # the swap appends --start-phase to any tools/e2e/ suite

    sa = types.SimpleNamespace(
        host=args.host, port=args.port, model="qwen3.8-27b", max_output_tokens=48,
        serve_log=os.path.expanduser("~/ninfer-serve.log"),
        request_log=os.path.expanduser("~/ninfer-requests.jsonl"),
        timeout=args.timeout, start_phase=1,
    )
    sa._request_log_start = suite.count_log_lines(sa.request_log)
    suite._ARGS = sa

    print(f"matdebug-driver: phases={args.phases} port={args.port} "
          f"request_log_start={sa._request_log_start}", flush=True)

    rc = 0
    for token in args.phases.split(","):
        token = token.strip()
        if not token:
            continue
        fn = getattr(suite, f"phase_{token}", None)
        if fn is None:
            print(f"ERROR: no phase_{token}", flush=True)
            return 2
        result = fn(sa)
        if result is None:
            print(f"ABORT: phase {token} returned None (a request failed)", flush=True)
            return 1
        for pn, v in result:
            print(f"  [{pn}] {v}", flush=True)

    # The two log-reading gates, so this run reports them on ITS OWN window.
    for name in ("phase_planner_latency", "phase_reuse_paths"):
        for pn, v in getattr(suite, name)(sa):
            print(f"  [{pn}] {v}", flush=True)

    print(f"matdebug-driver done rc={rc}", flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
