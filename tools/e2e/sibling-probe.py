#!/usr/bin/env python3
"""Sibling-consumption capture probe.

Captures the condition `resource_manager.h:602` counts as `sibling_candidates`: a private source whose
own ENDPOINT lies BEYOND this request's prompt, so consuming it (`retain=false` -> plan `Replace`)
destroys a checkpoint this request can never re-establish.

WHY A SIBLING IS POSSIBLE AT ALL. The session key is `derive_session_key` (`src/serve/request.h:153`) --
FNV-1a over the SYSTEM text plus the FIRST user message, and nothing else. Two requests therefore share a
session key, and so count as "the same session" for the `retain` test, while having tails of different
length. A request whose prompt is the same conversation PLUS a small delta gets `retain=false` (own
session) and replaces the entry's endpoint, even though it never reaches it.

THE SHAPE, AND THE PRECONDITION IT MUST SATISFY. The condition is
`request.prompt_tokens < entry.endpoint.frontier`, and the entry's endpoint frontier is the prompt PLUS the
generated response. So a sibling needs a LONG first response:

  turn 1 (deep)    : prompt ~D, long answer        -> endpoint frontier F = D + response
  turn 2 (sibling) : same conversation, tail swapped for a SHORT different question
                                                   -> prompt ~D-ish, and D < F  => the condition holds
  turn 3 (real)    : turn 1 + its answer + a follow-up, so its prompt CONTAINS F

Turn 3's `prefix_cache_hit_tokens` is the reading: with `NINFER_SIBLING_RETAIN` OFF the sibling consumed
the endpoint and turn 3 should be depressed; with it ON the endpoint survives.

The probe CHECKS THE PRECONDITION ITSELF and reports it, because the whole reading is void if turn 1's
response was too short for F to clear turn 2's prompt -- a probe that silently measures nothing is the
failure mode this repo keeps recording. It also reads the ARM from the test server's own environ rather
than trusting a label.

  PORT=8085 python3 sibling-probe.py
  E2E_SUITE_ARGS='--json /tmp/sib-probe-on.json' E2E_SIBLING_RETAIN=1 E2E_SUITE=.../sibling-probe.py \
    bash tools/e2e/e2e-swap.sh
"""
import argparse
import json
import os
import random
import sys
import time
import urllib.request

CHARS_PER_TOKEN = 5.9  # calibrated 2026-09-18 (longctx_recall_probe.py)
REQUEST_LOG = os.path.expanduser("~/ninfer-requests.jsonl")
PID_FILE = os.path.expanduser("~/ninfer-test.pid")


def build_doc(tokens, seed):
    rng = random.Random(seed)
    words = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda "
             "mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega "
             "anchor beacon compass estuary harbor jetty lighthouse mooring "
             "nadir orbit pier quay reef shoal tide vortex wave").split()
    chars = int(tokens * CHARS_PER_TOKEN)
    parts, n = [], 0
    while n < chars:
        w = rng.choice(words)
        parts.append(w)
        n += len(w) + 1
    return " ".join(parts)


def post(args, payload):
    t0 = time.monotonic()
    req = urllib.request.Request(
        f"http://{args.host}:{args.port}/v1/messages",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json",
                 "x-api-key": "test", "anthropic-version": "2023-06-01"},
        method="POST")
    with urllib.request.urlopen(req, timeout=args.timeout) as r:
        out = json.load(r)
    text = "".join(b.get("text", "") for b in (out.get("content") or [])
                   if b.get("type") == "text")
    return out, text, time.monotonic() - t0


def prompt_size(usage):
    """The request's PROMPT SIZE from the response usage, which is not `input_tokens`.

    ON A CACHE HIT `input_tokens` IS ONLY THE UNCACHED REMAINDER: measured on QA, a turn whose record
    read `prompt_tokens=15472` returned `input_tokens=1, cache_read_input_tokens=15471`. Matching a turn
    on `input_tokens` alone therefore finds nothing as soon as the cache is warm -- which is how the
    first version of this probe reported all three turns MISSING on its second run and nothing on its
    first. The record's `prompt_tokens` is the total, so the total is what has to be reconstructed."""
    if not usage:
        return None
    total = usage.get("input_tokens")
    if total is None:
        return None
    total += usage.get("cache_read_input_tokens") or 0
    total += usage.get("cache_creation_input_tokens") or 0
    return total


def new_records(since_line):
    recs = []
    try:
        with open(REQUEST_LOG, "r", errors="replace") as f:
            for i, line in enumerate(f):
                if i < since_line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if rec.get("event") == "request_done":
                    recs.append(rec)
    except OSError:
        pass
    return recs


def log_lines():
    try:
        with open(REQUEST_LOG, "r", errors="replace") as f:
            return sum(1 for _ in f)
    except OSError:
        return 0


def find_turn(before_line, want_prompt, timeout_s=20.0):
    """The request_done whose prompt_tokens is this turn's, in the window that opened at `before_line`.

    Matching by ORDINAL is wrong wherever the port carries traffic the probe did not send -- on QA
    (:8080) a smoke run had a foreign record land first and every row after it was attributed to the
    wrong turn. `usage.input_tokens` from the response is the turn's own prompt size, so it is the key."""
    deadline = time.monotonic() + timeout_s
    while True:
        for r in new_records(before_line):
            if (r.get("result") or {}).get("prompt_tokens") == want_prompt:
                return r
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.5)


def summarise(r):
    if r is None:
        return None
    m, res = r.get("materialization") or {}, r.get("result") or {}
    return {"prompt_tokens": res.get("prompt_tokens"),
            "completion_tokens": res.get("completion_tokens"),
            "hit": res.get("prefix_cache_hit_tokens"),
            "path": res.get("prefix_reuse_path"),
            "sibling_candidates": m.get("sibling_candidates"),
            "consumed_sources": m.get("consumed_sources"),
            "retained_sources": m.get("retained_sources"),
            "chosen_restorable_evictions": m.get("chosen_restorable_evictions")}


def read_arm():
    """The arm as the SERVER sees it, not as the caller labelled it.

    `NINFER_SIBLING_RETAIN` is read by PRESENCE (`std::getenv(...) != nullptr`), so an inherited EMPTY
    value enables it -- which is why this reports the value as well as the presence."""
    try:
        with open(PID_FILE) as f:
            pid = f.read().strip()
        raw = open(f"/proc/{pid}/environ", "rb").read().decode("utf-8", "replace")
    except OSError:
        return {"known": False, "reason": "no pid file or /proc entry"}
    val = None
    for kv in raw.split("\0"):
        if kv.startswith("NINFER_SIBLING_RETAIN="):
            val = kv.split("=", 1)[1]
    return {"known": True, "pid": pid, "sibling_retain_present": val is not None,
            "sibling_retain_value": val,
            "enabled": val is not None and val != ""}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=int(os.environ.get("PORT", 8085)))
    p.add_argument("--model", default="qwen3.8-27b")
    p.add_argument("--deep-tokens", type=int, default=14000,
                   help="turn 1 prompt size; must stay under the 32k test profile")
    p.add_argument("--answer-tokens", type=int, default=3000,
                   help="turn 1 max_tokens; the endpoint frontier is prompt + answer, so this is what "
                        "must clear turn 2's prompt")
    p.add_argument("--timeout", type=int, default=900)
    p.add_argument("--json", dest="json_out", default="/tmp/ninfer-sibling-probe.json")
    # The swap appends `--start-phase` to any suite under tools/e2e/, which this probe does not take.
    args, _ = p.parse_known_args()

    system = [{"type": "text",
               "text": "You are a coding assistant. Answer questions about the document provided. "
                       "When asked for a long answer, write at least several hundred words and stay on "
                       "the document.",
               "cache_control": {"type": "ephemeral"}}]
    # The FIRST user message fixes the session key, so every turn below must share it verbatim.
    u0 = build_doc(args.deep_tokens, 4242) + "\n\n---\n\nDescribe the structure of this document at length."
    # THE LENGTH DEMAND HAS TO BE IN THE TURN, NOT THE SYSTEM PROMPT: with thinking disabled the model
    # answered "3 tokens" to a system-level "write several hundred words", which left F only 10 tokens
    # above the sibling's prompt -- a precondition that holds by accident and would flake run to run.
    u_tail = ("Finally, write a detailed 1,200-word analysis of this document's structure, section by "
              "section. Keep writing until you have covered every part; do not summarise or stop early.")

    deep = [{"role": "user", "content": u0},
            {"role": "assistant", "content": "Understood. I will stay on the document."},
            {"role": "user", "content": u_tail}]

    print(f"sibling-probe: port={args.port} deep={args.deep_tokens} answer_max={args.answer_tokens}",
          flush=True)

    # --- turn 1: the deep turn, whose long answer sets the endpoint frontier F.
    mark = log_lines()
    # THINKING OFF, because the endpoint frontier is prompt + the ASSISTANT TEXT, and a thinking-only
    # answer renders as no text at all -- measured: `max_tokens=2500` came back with 2500 thinking tokens
    # and an empty text block, so turn 1's answer was "" and turn 3's prompt never reached F. The probe
    # then reads a good reuse for a reason unrelated to the sibling, which is the silent-nothing failure
    # this repo keeps recording.
    out1, text1, wall1 = post(args, {"model": args.model, "system": system, "messages": deep,
                                     "max_tokens": args.answer_tokens, "thinking": {"type": "disabled"},
                                     "temperature": 1.0, "top_p": 0.95})
    r1 = find_turn(mark, prompt_size(out1.get("usage")))
    print(f"  t1 deep     wall={wall1:.1f}s out={out1.get('usage', {}).get('output_tokens')}", flush=True)

    # --- turn 2: the SIBLING. Same conversation, the tail swapped for a short different question, so the
    # prompt stays SHORTER than turn 1's endpoint frontier. Name matches, tail does not.
    # THE SIBLING MUST CONTAIN TURN 1'S PROMPT AS A STRICT PREFIX. A sibling that REPLACES the tail has a
    # prompt shorter than turn 1's own prefix, so it can only resume a SHALLOWER entry -- which is what the
    # first version did, and why turn 3 still found its endpoint and the probe measured nothing. Appending
    # a small delta after turn 1's last message is "the same prompt plus a small delta" the source comment
    # names: the sibling resumes turn 1's entry (destroying the endpoint beyond it) while still sitting
    # under the frontier.
    sibling = deep + [{"role": "assistant", "content": "Understood."},
                      {"role": "user", "content": "Actually, just reply with OK."}]
    mark = log_lines()
    out2, _, wall2 = post(args, {"model": args.model, "system": system, "messages": sibling,
                                 "max_tokens": 32, "temperature": 1.0, "top_p": 0.95})
    r2 = find_turn(mark, prompt_size(out2.get("usage")))
    print(f"  t2 sibling  wall={wall2:.1f}s", flush=True)

    # --- turn 3: the REAL next turn. Its prompt contains turn 1's answer, so it reaches F.
    real = deep + [{"role": "assistant", "content": text1 or "(no text)"},
                   {"role": "user", "content": "Thanks. Answer one more: summarise the document in two sentences."}]
    mark = log_lines()
    out3, _, wall3 = post(args, {"model": args.model, "system": system, "messages": real,
                                 "max_tokens": 200, "temperature": 1.0, "top_p": 0.95})
    r3 = find_turn(mark, prompt_size(out3.get("usage")))
    print(f"  t3 real     wall={wall3:.1f}s", flush=True)

    arm = read_arm()
    t1, t2, t3 = summarise(r1), summarise(r2), summarise(r3)

    # THE PRECONDITION, CHECKED RATHER THAN ASSUMED: turn 2's prompt must be shorter than turn 1's
    # endpoint frontier, and that frontier is prompt + answer.
    f1 = None
    if t1:
        p1, c1 = t1.get("prompt_tokens"), t1.get("completion_tokens")
        if p1 is not None and c1 is not None:
            f1 = p1 + c1
    precondition = (f1 is not None and t2 is not None and t2.get("prompt_tokens") is not None
                    and t2["prompt_tokens"] < f1)

    captured = bool(t2 and (t2.get("sibling_candidates") or 0) > 0)
    # THE SECOND HALF OF THE PRECONDITION: turn 3 must actually NEED the endpoint, i.e. its prompt must
    # reach the frontier the sibling was in a position to destroy. Without this the probe can read a
    # healthy reuse path simply because the endpoint was never on turn 3's route.
    reaches = bool(t3 and f1 is not None and t3.get("prompt_tokens") is not None
                   and t3["prompt_tokens"] >= f1)

    out = {
        "arm": arm,
        "t1_deep": t1, "t2_sibling": t2, "t3_real": t3,
        "t1_endpoint_frontier_est": f1,
        "precondition_holds": precondition,
        "turn3_reaches_frontier": reaches,
        "captured": captured,
        "pass": captured and precondition and reaches,
    }
    print("\n--- reading ---")
    print(f"  arm: NINFER_SIBLING_RETAIN present={arm.get('sibling_retain_present')} "
          f"value={arm.get('sibling_retain_value')!r} enabled={arm.get('enabled')}")
    for name, t in (("t1", t1), ("t2", t2), ("t3", t3)):
        if t:
            print(f"  {name}: prompt={t['prompt_tokens']} completion={t['completion_tokens']} "
                  f"hit={t['hit']} path={t['path']} sibling={t['sibling_candidates']} "
                  f"consumed={t['consumed_sources']} retained={t['retained_sources']}")
        else:
            print(f"  {name}: MISSING from the request log")
    print(f"  precondition (t2.prompt < t1.prompt+t1.completion, est F={f1}): {precondition}")
    print(f"  CAPTURED (t2.sibling_candidates > 0): {captured}")
    print(f"  t3 reaches F (t3.prompt >= F): {reaches}")
    print(f"  t3 reuse in tokens reused: {t3.get('hit') if t3 else None}")
    print(f"PASS={out['pass']} -> {args.json_out}")
    with open(args.json_out, "w") as f:
        json.dump(out, f, indent=1)
    if not precondition:
        print("WARNING: t2.prompt was not below F, so the sibling condition could not arise -- raise "
              "--answer-tokens (a longer turn-1 answer pushes F past the sibling's prompt).")
    if not reaches:
        print("WARNING: t3.prompt did not reach F, so turn 3's reuse cannot speak to the sibling at all "
              "-- its reading is VOID whatever it says.")
    return 0 if out["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
