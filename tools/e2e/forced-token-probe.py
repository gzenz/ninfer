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

**What the three swap runs established (2026-09-26 13:53-13:59), so they are not repeated:** the multi-row case has NOT been
reached by concurrency in this configuration. The server prints `[forced] run_control_batch
membership=1 control_ready_lanes=1` -- by that line's own criterion the separation is timing, not
structure, since only one lane was control-ready at each control boundary. Two concurrency shapes were
tried: two byte-identical 3,804-token requests with budget 1024, and two DISTINCT equal-length ones (the
byte-identical pair reuses a prefix and cannot step together). The construction that should force
coincidence -- a short prompt so both prefills finish in one chunk, plus a tiny budget so both exhaust in
the same step -- is blocked by the API: `thinking.budget_tokens` must be at least 1024
(`anthropic_messages_request.cpp:866`), and a smaller value is a 400. The next step here is READING the
scheduler's admission path (one lane per boundary, so a readiness that is a one-shot per-lane event may
never coincide), not another run of this probe.

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
def body(pad_tokens: int, lane: int = 0) -> dict:
    """The request. `pad_tokens` of filler are prepended when > 0, which is what makes the lanes overlap:
    with tiny prompts the engine runs them one at a time (`running 1` in the throughput lines), so the
    forced steps never share a step -- and the membership that `append_forced_tokens` is handed, which
    accumulates rows, therefore holds one. Long prompts keep several lanes resident at once."""
    filler = ("The following is filler context, read it but do not act on it. " * max(1, pad_tokens // 12))
    # `lane` makes each request DISTINCT while keeping the prompts the same length. Byte-identical prompts
    # are the reason the first multi-row attempt saw one row per step: the second request reuses the
    # first's prefix and the two never advance in lockstep, so each forced step holds a single row. A
    # single differing digit is one token either way, so the lengths stay equal -- which is what the
    # multi-row case needs (equal-length lanes stepping together), and what the printed
    # `prompt_tokens` per request lets the reader check rather than assume.
    content = (filler + f"\n\n(lane {lane})\n\n" + PROMPT) if pad_tokens else PROMPT
    return {
        "model": "qwen3.8-27b",
        "max_tokens": 2048,
        "thinking": {"type": "enabled", "budget_tokens": BUDGET_TOKENS},
        "messages": [{"role": "user", "content": content}],
    }


PAD_TOKENS = 0
BUDGET_TOKENS = 1024


def send(label: str) -> dict:
    request = urllib.request.Request(
        URL, data=json.dumps(body(PAD_TOKENS, int(label.split()[-1].split('/')[0]) - 1)).encode(),
        headers={"content-type": "application/json"})
    started = time.time()
    with urllib.request.urlopen(request, timeout=300) as response:
        payload = json.loads(response.read().decode())
    usage = payload.get("usage", {})
    thinking = usage.get("output_tokens_details", {}).get("thinking_tokens")
    print(f"[probe] {label}: prompt_tokens={usage.get('input_tokens')} "
          f"stop_reason={payload.get('stop_reason')} "
          f"thinking_tokens={thinking} output={usage.get('output_tokens')} "
          f"elapsed={time.time() - started:.0f}s", flush=True)
    return payload


def main() -> int:
    import threading

    global PAD_TOKENS, BUDGET_TOKENS
    if "--pad-tokens" in sys.argv:
        PAD_TOKENS = int(sys.argv[sys.argv.index("--pad-tokens") + 1])
    # A SHORT prompt would be the construction that makes two lanes control-ready at the same worker
    # boundary -- with a single prefill chunk the one-boundary admission offset cannot accumulate -- but
    # the BUDGET half of that idea is unavailable: the API floors `thinking.budget_tokens` at 1024
    # (`anthropic_messages_request.cpp:866`), so a smaller value is a 400 and cannot force two lanes to
    # exhaust in the same step. Refused here rather than sent, because a 400 run reads like a failed
    # experiment and is really an unusable one.
    if "--budget" in sys.argv:
        BUDGET_TOKENS = int(sys.argv[sys.argv.index("--budget") + 1])
        if BUDGET_TOKENS < 1024:
            print(f"[probe] refusing --budget {BUDGET_TOKENS}: the API requires >= 1024 "
                  f"(anthropic_messages_request.cpp:866); a smaller value returns HTTP 400 and the run "
                  f"measures nothing", flush=True)
            return 2
    if count > 1 and PAD_TOKENS == 0:
        print("[probe] warning: --concurrent > 1 without --pad-tokens -- `lane` is ignored when "
              "pad_tokens is 0, so the requests are byte-identical and cannot step together",
              flush=True)

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
