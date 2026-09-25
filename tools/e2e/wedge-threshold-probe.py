#!/usr/bin/env python3
"""Stage-2 threshold probe for #14 (the wedge), from the 2026-09-25 forensic reconstruction.

The wedge was the second stage of a recovery that left occupancy owned by nothing: a request above
what the remaining capacity could hold was still classed *feasible* (feasibility is checked against
full capacity), so it blocked forever instead of being rejected. This probe tests the claim the
diagnosis rests on, on the test server, without needing to reproduce a recovery first:

  * **Just below the line** -- one request of ~257k tokens, about 4016 of the device's 4096 main-KV
    pages. On a server with no residual occupancy it must succeed. If it does not, the server is
    carrying occupancy it cannot free *before* any recovery, which is a stronger finding than the
    incident itself.
  * **Above the line** -- ~263k tokens. It must fail *promptly and visibly* (context-length or
    overloaded), not hang: a hang is the unsatisfiable-block condition, i.e. the wedge's stage two, and
    it would appear as `admission stalled` / `admission rejected` in the log.

  **What the first real run established, and what it cannot:** below the line FIT (1 request, 110 s,
  `ok=1 failed=0`) and above the line was a clean `HTTP 400` in 0.23 s -- so a fresh server carries no
  residual occupancy and refuses an impossible request promptly. But on a server with no leak the
  context limit (262,144) and the page capacity (4096 x 64 = 262,144) are *the same number*, so the
  upper arm brackets the context limit, not the capacity line the wedge lives on. That line separates
  only once a leak has consumed capacity, which is why the discriminating experiment is the
  rewind-replay arm (`L2` in `results/HANDOFF.md`), not this probe alone.

Run it through the swap (it needs the test port; against prod it would consume real capacity and, if
a leak were present, stall real traffic):

  CTX=prod4 E2E_TIMEOUT=420 E2E_SUITE=/tmp/wedge-threshold-probe.py bash tools/e2e/e2e-swap.sh

It deliberately uses `tools/load/prod-load.py` rather than re-implementing the client, so the prompts
and the request shape are the ones every other measurement on this host used.
"""

import subprocess
import sys
import time

# Absolute, because the swap runs a suite from its own directory (`cd $(dirname $E2E_SUITE)`), so a
# relative path silently resolves elsewhere: the first version of this probe ran with CWD=/tmp, failed
# to find the client, and reported rc=2 in 0s -- which its own verdict logic then read as a refused
# request and printed a conclusion ("residual occupancy exists with no recovery") from two runs that
# never sent anything. Hence also the `requests in` requirement below.
CLIENT = "/home/zenz/ninfer/tools/load/prod-load.py"
PROBE = ["python3", CLIENT, "--port", "8085", "--sessions", "1",
         "--rounds", "1", "--system-tokens", "0", "--max-tokens", "32"]


def run(label: str, tokens: int, expect_fit: bool) -> bool:
    started = time.time()
    result = subprocess.run(PROBE + ["--tokens", str(tokens)], capture_output=True, text=True,
                            timeout=600)
    elapsed = time.time() - started
    out = (result.stdout or "") + (result.stderr or "")
    # The client must have actually issued the request before any verdict is drawn: `N requests in` is
    # printed only after the run, so its absence means the client never got that far (bad path, bad
    # args, import error) and the arm is INCONCLUSIVE rather than refused.
    made_request = "requests in" in out
    ok = "ok=1" in out and "failed=0" in out
    verdict = "INCONCLUSIVE" if not made_request else ("FIT" if ok else "REFUSED")
    agrees = made_request and ((verdict == "FIT") == expect_fit)
    print(f"[probe] {label}: tokens≈{int(tokens * 1.133)} ({tokens} raw) -> {verdict} "
          f"in {elapsed:.0f}s, rc={result.returncode}, "
          f"{'as expected' if agrees else 'NOT AS EXPECTED'}")
    for line in out.splitlines():
        if any(k in line for k in ("requests in", "FAILED", "error", "Error", "refused", "ok=")):
            print(f"[probe]   {line.strip()[:160]}")
    return agrees


def main() -> int:
    # 4096 pages x 64 tokens = 262,144; ~4016 pages is ~257k tokens, ~4110 pages is ~263k.
    below = run("below the line", 227000, expect_fit=True)
    above = run("above the line", 233000, expect_fit=False)
    print(f"[probe] verdict: below={'as-expected' if below else 'UNEXPECTED'} "
          f"above={'as-expected' if above else 'UNEXPECTED'}")
    if not below and not above:
        print("[probe] READ: neither arm produced a request -- the probe measured nothing; fix it "
              "before drawing any conclusion from this run.")
    elif not below:
        print("[probe] READ: a request that fits on an empty server did not fit -- residual "
              "occupancy may exist without a recovery, i.e. a leak with no trigger. Confirm the "
              "client's own output shows a real refusal (HTTP status), not a harness failure.")
    return 0 if (below and above) else 1


if __name__ == "__main__":
    sys.exit(main())
