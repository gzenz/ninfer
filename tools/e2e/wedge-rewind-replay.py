#!/usr/bin/env python3
"""Rewind-replay arm (L2) for #14: reproduce the state the 10:13 underflow needed.

The forensic reconstruction says the incident's first stage needed three things at once:
  * a **saturated host tier** (host state slots 16/16, host KV ~90% full),
  * **four-way concurrency**, and
  * requests that **fork or truncate from the middle of longer cached continuations** left by a prior
    run -- which happens because `prod-load.py` seeds each client from fixed values, so repeated runs
    send byte-identical prompts and later runs rewind into earlier runs' cached sequences
    (`historical_fork_hits`, `partial_tail_cow_pages`). A fresh process can never have this, which is
    why every earlier "identical load, clean run" control was not a fair comparison.

This arm builds exactly that: fill under a small host KV, then run the identical command three times,
each SIGINT'd mid-prefill, then one full run. It watches for the three signatures that say it worked --
`resource subtraction underflow`, any `WORKER RECOVER`, and a non-zero post-recovery residual.

Run through the swap, with the host KV small so it saturates quickly:

  E2E_HOST_KV_MIB=6144 CTX=prod4 E2E_TIMEOUT=2100 E2E_SUITE=/tmp/wedge-rewind-replay.py \\
    bash tools/e2e/e2e-swap.sh

It is long (~20 min) and it holds prod down for the duration, because the swap stops prod. There is no
way to drive the test port without that.
"""

import random
import signal
import subprocess
import sys
import time

CLIENT = "/home/zenz/ninfer/tools/load/prod-load.py"
SHAPE = ["--port", "8085", "--sessions", "4", "--tokens", "30000", "--turn-tokens", "4000",
         "--max-tokens", "64"]
# The shape the incident's later runs used, so the prompts are byte-identical across runs and later
# runs rewind into earlier ones (the property this arm exists to exploit).


def run(label: str, rounds: int, kill_after: float | None = None) -> None:
    cmd = ["python3", CLIENT] + SHAPE + ["--rounds", str(rounds)]
    started = time.time()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if kill_after is not None:
        # SIGINT the *client*, not the server: the requests in flight are abandoned mid-prefill,
        # which is what leaves the server holding partly-built continuations.
        try:
            time.sleep(kill_after)
            proc.send_signal(signal.SIGINT)
        except Exception:
            pass
    out, _ = proc.communicate(timeout=1800)
    print(f"[l2] {label}: rounds={rounds} kill_after={kill_after} rc={proc.returncode} "
          f"elapsed={time.time() - started:.0f}s")
    for line in out.splitlines():
        if "requests in" in line or "FAILED" in line or "error" in line.lower():
            print(f"[l2]   {line.strip()[:150]}")


def main() -> int:
    print("[l2] phase 1: fill under a small host KV")
    run("fill", rounds=12)
    print("[l2] phase 2: rewind -- identical command, killed mid-prefill")
    for i in range(3):
        run(f"rewind-{i + 1}", rounds=40, kill_after=random.uniform(45.0, 90.0))
    print("[l2] phase 3: the identical command, run to completion")
    run("full", rounds=40)
    print("[l2] done -- check the server log for 'resource subtraction underflow', 'WORKER RECOVER', "
          "and the post-recovery residual line")
    return 0


if __name__ == "__main__":
    sys.exit(main())
