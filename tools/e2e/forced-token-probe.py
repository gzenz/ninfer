#!/usr/bin/env python3
"""Reach the forced-token path and report what it was given.

`append_forced_tokens` injects a thinking turn's closing tokens when the thinking budget is exhausted.
Nothing has ever observed that function running: the only claim about it in the tree was a comment that
read the request log's `units.control` as evidence -- which is false, that field is a control-plane work
counter that is non-zero in most records.

This suite creates the condition and reads the probe inside the function:

  * one request, `thinking: {enabled, budget_tokens: 1024}`, `max_tokens` above it, and a prompt that
    MUST exceed the budget (enumerate 1..400 with a justification and no summarising) -- a milder prompt
    finishes thinking naturally (`stop_reason: end_turn`) and never forces anything;
  * the server must run with `NINFER_FORCED_PROBE=1`, which makes `append_forced_tokens` print
    `[forced] append_forced_tokens rows=… stride=… tokens=… distinct_lanes=…`.

  E2E_TIMEOUT=420 NINFER_FORCED_PROBE=1 CTX=prod4 E2E_SUITE=/tmp/forced-token-probe.py \\
    bash tools/e2e/e2e-swap.sh
  then: grep '\\[forced\\]' ~/ninfer-serve.log

`distinct_lanes=1` means the call saw more than one row -- the case the per-row KV bind exists for. A
run of this suite with one row proves the path executes; a run with two rows is the one that tests the
fix, and needs two sessions constrained the same way at the same time (send both requests before reading
either response).
"""

import json
import sys
import time
import urllib.request

URL = "http://127.0.0.1:8085/v1/messages"
PROMPT = ("For every integer from 1 to 400, state whether it is prime and give a one-line "
          "justification for each. Do not skip any number and do not summarise.")
BODY = {
    "model": "qwen3.8-27b",
    "max_tokens": 2048,
    "thinking": {"type": "enabled", "budget_tokens": 1024},
    "messages": [{"role": "user", "content": PROMPT}],
}


def send(label: str) -> dict:
    request = urllib.request.Request(
        URL, data=json.dumps(BODY).encode(), headers={"content-type": "application/json"})
    started = time.time()
    with urllib.request.urlopen(request, timeout=300) as response:
        payload = json.loads(response.read().decode())
    usage = payload.get("usage", {})
    thinking = usage.get("output_tokens_details", {}).get("thinking_tokens")
    print(f"[probe] {label}: stop_reason={payload.get('stop_reason')} "
          f"thinking_tokens={thinking} output={usage.get('output_tokens')} "
          f"elapsed={time.time() - started:.0f}s", flush=True)
    return payload


def main() -> int:
    import threading

    # Concurrent, not sequential: `multi_row=1` is the case the per-row KV bind exists for, and it needs
    # two thinking-constrained lanes in flight *at the same time* so they can land in one batch. Both
    # requests are launched before either response is read.
    count = int(sys.argv[sys.argv.index("--concurrent") + 1]) if "--concurrent" in sys.argv else 1
    errors: list[str] = []

    def worker(index: int) -> None:
        try:
            send(f"budget-exhausting request {index + 1}/{count}")
        except Exception as error:  # noqa: BLE001 - reported, not swallowed
            errors.append(f"{index}: {error}")

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(count)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for error in errors:
        print(f"[probe] ERROR {error}", flush=True)
    print(f"[probe] {count - len(errors)}/{count} requests completed; now grep the server log for "
          "'[forced]' -- a line means append_forced_tokens ran, and multi_row=1 means it saw the "
          "multi-row case the per-row bind exists for", flush=True)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
