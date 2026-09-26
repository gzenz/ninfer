#!/usr/bin/env python3
"""Cross-session canary: can one session's decode ever surface another session's content?

This is the repro for the production contamination (D2): with N concurrent sessions
sharing a large common prefix, each session plants a unique canary token in its own
document and is asked to repeat it. A correct engine returns each session its own
canary. A physical-KV / state mismatch returns the other session's content, which is
the observed failure: correct tokens admitted (98.6% self-prefix hit), foreign
activations.

Shape (prod parity where it matters):
  - N sessions (default 4 = prod max-concurrency), same ~20k-token system prompt and
    the same tool list, so the shared-prefix / fork paths engage.
  - each session appends its own ~40k-token document with CANARY-<session>-<uuid>
    planted at three depths,
  - staggered arrivals and >=4 rounds, so turns overlap other sessions' prefill,
  - temperature 0, "reply with only the canary".

Usage (inside the swap, test server up):
  python3 tools/e2e/canary-e2e.py [--sessions 4] [--rounds 4] [--seed 40000]
Exit code 0 = no cross-session bleed; 1 = bleed (with the offending reply printed).
"""

import argparse
import hashlib
import json
import os
import random
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid

import importlib.util

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("ninfer_e2e_suite", os.path.join(_HERE, "ninfer-e2e.py"))
_suite = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_suite)

# Canary width in hex digits. The default 12 (48 bits) is what every earlier run used. A WIDER
# canary tests the one question the D2 record cannot settle: a foreign exact match proves content
# flowed between lanes, but a confabulated/recombined canary-shaped string could also match a short
# canary by accident (fabrications and recombinations are common in these logs). At 32 hex digits an
# accidental or fabricated match is not credible, so a bleed that survives the wider canary is real
# cross-lane content; a bleed count that collapses to zero at 32 hex says the short-canary matches
# were recombination, not contamination.
CANARY_HEX = int(os.environ.get("CANARY_HEX", "12"))

TOOLS = [
    {"name": "read_file", "description": "Read a file",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}},
                      "required": ["path"]}},
    {"name": "write_file", "description": "Write a file",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"},
                                                       "content": {"type": "string"}},
                      "required": ["path", "content"]}},
]


def post_messages(args, payload, timeout=600):
    req = urllib.request.Request(
        f"http://{args.host}:{args.port}/v1/messages",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json", "anthropic-version": "2023-06-01"},
        method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.load(resp)


def reply_text(out):
    parts = []
    for block in out.get("content", []) or []:
        if block.get("type") == "text":
            parts.append(block.get("text", ""))
    return "".join(parts)


def reply_shape(out):
    """What KIND of reply came back, which `len(text)` alone cannot say.

    `len=0` was recorded as an unexplained reproducible observation ("S2 r2 replies empty") because the
    record kept only the concatenated TEXT. A turn whose reply is a tool call has no text and therefore
    measures zero, so "empty reply" and "tool-only reply" were indistinguishable -- and one of them is an
    engine finding while the other is this harness counting the wrong thing. The shape is what separates
    them: the content-block kinds, the tool-call count and the stop reason.
    """
    blocks = out.get("content", []) or []
    return {
        "block_kinds": [b.get("type") for b in blocks],
        "tool_calls": sum(1 for b in blocks if b.get("type") == "tool_use"),
        "stop_reason": out.get("stop_reason"),
    }


def filler(rng, tokens):
    return _suite.filler(rng, tokens)


def document(rng, tokens, canary):
    """A per-session document with the canary planted at three depths."""
    third = tokens // 3
    return (f"[doc-start] {filler(rng, third)} [canary-a] {canary} {filler(rng, third)} "
            f"[canary-b] {canary} {filler(rng, third)} [canary-c] {canary} [doc-end]")


class CanarySession(threading.Thread):
    def __init__(self, index, args, barrier, results, shared_system):
        super().__init__(daemon=True)
        self.index = index
        self.name = f"S{index}"
        if os.environ.get("CANARY_DETERMINISTIC") == "1":
            # Byte-identical prompts across runs: the canary is derived from a fixed key and the
            # document rng from the session index, so two runs with the same settings present the
            # engine the same prompts. That makes the per-lane first-token trace comparable
            # across runs (concurrent vs serialized) -- a content-equivalence oracle that needs
            # no engine instrumentation.
            key = os.environ.get("CANARY_KEY", "deterministic")
            dig = hashlib.sha256(f"{key}-{index}".encode()).hexdigest()
            self.canary = f"CANARY-{self.name}-" + dig[:CANARY_HEX]
            self.rng = random.Random(1000 + index)
        else:
            self.canary = f"CANARY-{self.name}-{uuid.uuid4().hex[:CANARY_HEX]}"
            self.rng = random.Random(hash(self.name) & 0xFFFFFFFF)
        self.args = args
        self.barrier = barrier
        self.results = results
        self.shared_system = shared_system
        self.messages = []
        self.errors = 0

    def ask(self, prompt):
        self.messages.append({"role": "user", "content": prompt})
        payload = {"model": self.args.model, "max_tokens": self.args.max_tokens,
                   "temperature": 0.0, "system": self.shared_system, "tools": TOOLS,
                   "messages": self.messages}
        out = post_messages(self.args, payload)
        text = reply_text(out)
        self.messages.append({"role": "assistant", "content": text or "(empty)"})
        # The SHAPE comes back with the text: `verdict` needs it to tell an empty reply from a
        # tool-only one, which the text alone cannot express.
        return text, reply_shape(out)

    def run(self):
        # Staggered arrival: session i starts i*STAGGER after the previous one, then joins at the
        # barrier each round so turns land on top of each other's prefill. CANARY_REVERSE=1 inverts
        # the arrival order, which separates "which session stays clean" as a function of *arrival
        # order* from *prompt length* (the two are confounded in the default order).
        order = (self.args.sessions - 1 - self.index
                 if os.environ.get("CANARY_REVERSE") == "1" else self.index)
        time.sleep(order * self.args.stagger)
        try:
            body = document(self.rng, self.args.seed, self.canary)
            first = (f"{body}\n\nYour canary code is {self.canary}. "
                     f"Reply with only the canary code, nothing else.")
            text, shape = self.ask(first)
            self.results.append(self.verdict("r1", text, shape))
            for r in range(2, self.args.rounds + 1):
                self.barrier.wait(timeout=900)
                text, shape = self.ask("Again: reply with only your canary code.")
                self.results.append(self.verdict(f"r{r}", text, shape))
        except Exception as exc:  # noqa: BLE001 - reported, not raised
            self.errors += 1
            self.results.append({"session": self.name, "round": "?", "error": str(exc)})

    def verdict(self, rnd, text, shape):
        own = self.canary in text
        foreign = [c for c in _ALL_CANARIES if c != self.canary and c in text]
        # A *partial* foreign match: another session's canary prefix appears without the full
        # canary. That is the recombination signature the record already contains (a canary-shaped
        # string rebuilt from a prefix seen in the lane's own context) and it must not be counted as
        # bleed -- bleed stays *exact* foreign only, so the numbers stay comparable with every
        # earlier run. Reported alongside so the widened-canary (CANARY_HEX=32) reading can tell
        # "the content flowed" from "the format was learned".
        # A *foreign prefix*: the start of another session's canary (its name plus at least two hex
        # digits) without the full canary. The first version required all but the last six hex, which
        # is 36 characters of a 32-hex canary -- far longer than the replies a corrupted turn emits
        # (6-25 characters in the 32-hex run), so it reported `partial=0` for turns that plainly
        # carried another session's first six hex digits. The threshold is four hex digits (16 bits)
        # -- the session name is guessable, but 16 bits of a sha256 prefix is not something the
        # format or the lane's own context can supply.
        partial = [c for c in _ALL_CANARIES
                   if c != self.canary and c not in text and len(c) > 14 and c[:14] in text]
        record = {"session": self.name, "round": rnd, "own": own,
                  "foreign": foreign, "foreign_partial": partial, "len": len(text),
                  "text": text[:400], **(shape or {})}
        # Print as we go: a capped run still shows which turn stalled. The body hash lets the
        # serve-side provenance log be matched to what this client actually received.
        body_hash = hashlib.sha256(text.encode()).hexdigest()[:16]
        shape = shape or {}
        print(f"  {self.name} {rnd}: own={own} foreign={foreign} partial={partial} "
              f"len={len(text)} kinds={shape['block_kinds']} tools={shape['tool_calls']} "
              f"stop={shape['stop_reason']} sha={body_hash} :: {text[:60]!r}", flush=True)
        record["sha"] = body_hash
        return record


_ALL_CANARIES = []


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--model", default="qwen3.8-27b")
    p.add_argument("--sessions", type=int, default=int(os.environ.get("CANARY_SESSIONS", "4")))
    p.add_argument("--rounds", type=int, default=int(os.environ.get("CANARY_ROUNDS", "4")))
    p.add_argument("--seed", type=int, default=int(os.environ.get("CANARY_SEED", "40000")),
                   help="tokens in each session's private document")
    p.add_argument("--system-tokens", type=int, default=int(os.environ.get("CANARY_SYSTEM", "20000")),
                   help="tokens in the prefix shared by every session")
    p.add_argument("--max-tokens", type=int, default=64)
    p.add_argument("--stagger", type=float, default=float(os.environ.get("CANARY_STAGGER", "0.7")))
    p.add_argument("--json", dest="json_out", default=None)
    # Accepted and ignored so the e2e swap driver can pass its suite args.
    p.add_argument("--start-phase", type=int, default=1)
    args = p.parse_args()

    sys_rng = random.Random(4242)
    shared_system = ("You are a careful engineering assistant. Reference material follows.\n"
                     + filler(sys_rng, args.system_tokens))

    results, threads = [], []
    barrier = threading.Barrier(max(1, args.sessions), timeout=900)
    for i in range(args.sessions):
        s = CanarySession(i, args, barrier, results, shared_system)
        _ALL_CANARIES.append(s.canary)
        threads.append(s)

    # The arm, self-described. Nothing in an earlier run's artifacts recorded the knobs that decide
    # whether two runs are comparable (determinism, arrival order, key, stagger, concurrency, and the
    # gate's own version), so "identical knobs" was an assertion about the launch, not a property of
    # the record -- the same defect cmp-e2e.py's `arm` block was added to fix.
    arm = {
        "sessions": args.sessions, "rounds": args.rounds, "hex": CANARY_HEX,
        "shared_tokens": args.system_tokens, "private_tokens": args.seed,
        "stagger": args.stagger, "max_concurrency": os.environ.get("MAX_CONCURRENCY", "unset"),
        "deterministic": os.environ.get("CANARY_DETERMINISTIC", "0"),
        "reverse": os.environ.get("CANARY_REVERSE", "0"),
        "key": os.environ.get("CANARY_KEY", "deterministic"),
        "gate": "bleed-or-partial-v2",
    }
    print(f"arm: {json.dumps(arm, sort_keys=True)}")
    print(f"canary: {args.sessions} sessions x {args.rounds} rounds, hex={CANARY_HEX}, "
          f"shared={args.system_tokens} tok + private={args.seed} tok, temp=0")
    t0 = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    wall = time.time() - t0

    bleed = [r for r in results if r.get("foreign")]
    partial = [r for r in results if r.get("foreign_partial")]
    missed = [r for r in results if r.get("own") is False]
    errors = [r for r in results if r.get("error")]
    print(f"\n{len(results)} turns in {wall:.0f}s; "
          f"bleed={len(bleed)} partial_foreign={len(partial)} missed_own={len(missed)} "
          f"errors={len(errors)}")
    for r in results:
        if r.get("error"):
            print(f"  ERROR {r['session']} {r['round']}: {r['error']}")
        else:
            print(f"  {r['session']} {r['round']}: own={r['own']} foreign={r['foreign']} "
                  f"len={r['len']} :: {r['text'][:80]!r}")
    for r in bleed:
        print(f"\nBLEED: {r['session']} {r['round']} returned {r['foreign']}\n  {r['text']}")

    # The gate fails on an exact foreign match OR on a foreign *prefix*: at 32 hex a
    # `foreign_partial` is four or more hex digits of another session's sha256, which the format and
    # the lane's own context cannot supply. Version 1 exited 0 on those runs -- 1 of 4 identical-knob
    # concurrent runs failed the gate while 4 carried the same signature (the denominator is five
    # runs, not four: 22:04, 22:22, 22:48, 22:50, 22:58), so a reader would have concluded the defect
    # was absent.
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump({"arm": arm, "sessions": args.sessions, "rounds": args.rounds,
                       "results": results},
                      f, indent=1)
    return 1 if (bleed or partial or errors) else 0


if __name__ == "__main__":
    sys.exit(main())