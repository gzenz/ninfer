#!/usr/bin/env python3
"""Faithful replay of process 407363 (L1): the seven-run sequence that preceded the 2026-09-25 wedge.

Every earlier attempt to reproduce the wedge drove *one* load shape against a fresh process. The
forensic reconstruction says the incident's state came from a **sequence**: five runs of different sizes
plus the operator's own streamed traffic accumulated sixteen host states and a catalog of cached
continuations, and the underflow fired in the second round of a seventh run that was byte-identical to
the sixth. A fresh process cannot have that state, which is why "the identical load ran 160/160 clean"
was never a fair control.

This arm replays the sequence (minus the operator's traffic, which cannot be replayed) in **one process**,
with prod's own host KV:

  1. 4x1 `--tokens 25000`, killed after ~30 s                  (the first, cancelled run)
  2. `--rounds 2  --tokens 50000`
  3. `--rounds 2  --tokens 150000`
  4. `--rounds 8  --tokens 60000  --turn-tokens 12000`
  5. `--rounds 40 --tokens 40000  --turn-tokens 5000`          (its over-budget 400s are part of the state)
  6. `--rounds 40 --tokens 30000  --turn-tokens 4000`, killed during round 6
  7. the same as 6, run to completion                          (expect the underflow near round 2)

Watch for the three signatures: `resource subtraction underflow`, any `WORKER RECOVER`, and a **non-zero**
`post-recovery residual` line -- that last one is the leak, named in the units the incident used. The
counters `[capture] recycled-checkpoint DROPPED on abort (was 'restored' before the #11(a) disposal)` and `fail-all cleanup: ... skipped=...` name
the site if it fires.

Run through the swap, ~50 minutes, `E2E_TIMEOUT` above 3300 (the whole sequence plus startup):

  E2E_HOST_KV_MIB=30720 CTX=prod4 SPEC=dflash2 E2E_TIMEOUT=3600 E2E_SUITE=/tmp/wedge-faithful-replay.py \\
    bash tools/e2e/e2e-swap.sh

Two things it cannot do: replay the operator's streamed traffic from 09:30-09:39, and run inside a
single tool call -- it holds prod down for its duration, because the swap stops prod.
"""

import signal
import subprocess
import sys
import time

CLIENT = "/home/zenz/ninfer/tools/load/prod-load.py"
BASE = ["--port", "8085", "--sessions", "4"]

# The incident's sequence, in order. `kill_after` is None where the run completed.
SEQUENCE = [
    ("first-cancelled", ["--rounds", "1", "--tokens", "25000"], 30.0),
    ("small", ["--rounds", "2", "--tokens", "50000", "--max-tokens", "32"], None),
    ("large", ["--rounds", "2", "--tokens", "150000", "--max-tokens", "32"], None),
    ("eight", ["--rounds", "8", "--tokens", "60000", "--turn-tokens", "12000", "--max-tokens", "32"], None),
    ("forty-heavy", ["--rounds", "40", "--tokens", "40000", "--turn-tokens", "5000", "--max-tokens", "32"], None),
    ("forty-killed", ["--rounds", "40", "--tokens", "30000", "--turn-tokens", "4000"], 330.0),
    ("forty-full", ["--rounds", "40", "--tokens", "30000", "--turn-tokens", "4000"], None),
]


def run(label: str, args: list[str], kill_after: float | None) -> None:
    started = time.time()
    proc = subprocess.Popen(["python3", CLIENT] + BASE + args, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if kill_after is not None:
        # SIGINT the client, not the server: abandoned mid-prefill is what leaves the server holding
        # partly-built continuations, which is the state the sequence exists to build.
        try:
            time.sleep(kill_after)
            proc.send_signal(signal.SIGINT)
        except Exception:
            pass
    try:
        out, _ = proc.communicate(timeout=3000)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        label += " (timeout)"
    print(f"[l1] {label}: rc={proc.returncode} elapsed={time.time() - started:.0f}s", flush=True)
    for line in out.splitlines():
        if "requests in" in line or "FAILED" in line:
            print(f"[l1]   {line.strip()[:140]}", flush=True)


def main() -> int:
    for label, args, kill_after in SEQUENCE:
        run(label, args, kill_after)
    print("[l1] sequence complete -- read the server log for 'resource subtraction underflow', "
          "'WORKER RECOVER', and a non-zero 'post-recovery residual'", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
