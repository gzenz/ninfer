#!/usr/bin/env python3
"""Neutral load generator for the live server. NOT an e2e test, and deliberately not in tools/e2e/.

The distinction this file exists to keep: an e2e *suite* runs against a swapped-in test server on the
isolated port (8085) with prod stopped, so its numbers mean something. This tool does the opposite job
-- it drives the live server on its own port to put the system under load and let the operator read
/stats and the request log afterwards. It asserts nothing and prints no verdicts; if it were run as a
gate it would be a lie, and if the e2e harness were pointed at the live port it would measure session
traffic it never issued.

Usage:
  python3 tools/load/prod-load.py --port 8080 --sessions 4 --rounds 2 --tokens 50000
"""

import argparse
import json
import random
import sys
import threading
import time
import urllib.request

WORDS = ("alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november "
         "oscar papa quebec romeo sierra tango uniform victor whiskey xray yankee zulu anchor beacon "
         "cinder dormant ember fathom grove hollow ivory kernel lumen meadow nightfall opal quarry "
         "ridge summit tundra umber vale willow yonder zenith").split()


def filler(rng, tokens):
    """Roughly `tokens` tokens of prose: ~1.3 tokens per word at this vocabulary."""
    return " ".join(rng.choice(WORDS) for _ in range(int(tokens * 0.75)))


def post(args, payload, timeout=900):
    req = urllib.request.Request(
        f"http://{args.host}:{args.port}/v1/messages",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json", "anthropic-version": "2023-06-01"},
        method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.load(resp)


class Client(threading.Thread):
    def __init__(self, index, args, barrier, results):
        super().__init__(daemon=True)
        self.index, self.args, self.barrier, self.results = index, args, barrier, results
        self.rng = random.Random(4242 + index)
        # One session per client, with its own large document, so the engine sees distinct prompts
        # that share a prefix rather than four copies of one request.
        self.system = ("You are a careful engineering assistant. Reference material follows.\n"
                       + filler(self.rng, args.system_tokens))
        self.marker = "MARKER-S%d-%016x" % (index, self.rng.getrandbits(64))
        self.messages = []

    def run(self):
        try:
            time.sleep(self.index * self.args.stagger)
            body = filler(self.rng, self.args.tokens)
            # The marker instruction must make the model REPRODUCE its marker, or the check is inert:
            # the first version asked for a summary and merely forbade other sessions' markers, so no
            # reply ever contained one -- `own_marker True: 0 of 32` -- and a foreign marker could not
            # have appeared either. `own_marker` is the control: it must be ~always true for a foreign
            # match to mean anything.
            # Two ways this instruction was inert before, both now closed. (1) It asked for a summary
            # *and then* the marker, while `--max-tokens` was 32 -- every reply was exactly 32 tokens
            # and never reached the marker (`own_marker True: 0 of 140`). (2) The earlier version merely
            # forbade other sessions' markers, so none ever appeared. Reply with the marker alone, and
            # keep the default budget above it.
            self.messages.append({"role": "user", "content":
                                  f"{body}\n\nReply with only this token, nothing else: "
                                  f"{self.marker}"})
            for rnd in range(1, self.args.rounds + 1):
                t0 = time.time()
                try:
                    out = post(self.args, {"model": self.args.model, "max_tokens": self.args.max_tokens,
                                           "temperature": 0.0, "system": self.system,
                                           "messages": self.messages})
                    usage = out.get("usage") or {}
                    text = "".join(b.get("text", "") for b in (out.get("content") or [])
                                   if b.get("type") == "text")
                    # Match the FULL marker (name + 64 random bits), never the `MARKER-S<i>-` prefix.
                    # The prefix version reported 27 foreign hits in a run whose control was 2 of 125
                    # -- impossible as contamination, and explained by the model inventing the marker
                    # *shape*: it has seen its own marker, so "MARKER-S2-<anything>" is easy to
                    # fabricate. A 64-bit exact match is the only sound needle, which is the same
                    # lesson the canary harness learned at 32 hex.
                    foreign = [f"MARKER-S{i}" for i in range(self.args.sessions) if i != self.index
                               and any(m in text for m in self.args._peer_markers.get(i, []))]
                    rec = {"client": self.index, "round": rnd, "ok": True,
                           "wall": round(time.time() - t0, 2),
                           "input_tokens": usage.get("input_tokens"),
                           "output_tokens": usage.get("output_tokens"),
                           "own_marker": self.marker in text}
                    if self.args.marker_check:
                        rec["foreign_markers"] = foreign
                    self.results.append(rec)
                except Exception as exc:  # noqa: BLE001 - reported, not raised
                    self.results.append({"client": self.index, "round": rnd, "ok": False,
                                         "wall": round(time.time() - t0, 2), "error": str(exc)})
                if rnd < self.args.rounds:
                    self.barrier.wait(timeout=600)
                    self.messages.append({"role": "assistant", "content": "noted"})
                    growth = filler(self.rng, self.args.turn_tokens) if self.args.turn_tokens else ""
                    # The marker instruction must ride EVERY turn: with it only in the first message the
                    # control read 0 of 24, because later turns never asked for it.
                    self.messages.append({"role": "user",
                                          "content": f"{growth}\n\nReply with only this token, "
                                                     f"nothing else: {self.marker}"})
        except Exception as exc:  # noqa: BLE001
            self.results.append({"client": self.index, "round": "?", "ok": False, "error": str(exc)})


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--model", default="qwen3.8-27b")
    p.add_argument("--sessions", type=int, default=4)
    p.add_argument("--rounds", type=int, default=2)
    p.add_argument("--turn-tokens", type=int, default=0,
                   help="tokens appended to the session on each later turn. A session whose context "
                        "GROWS is the shape the production incidents had -- the original report needed "
                        "more turns than a burst provides for the working set to cross what fits, and "
                        "for demote/restore churn to start. 0 keeps every turn a no-op append.")
    p.add_argument("--marker-check", action="store_true",
                   help="give each session a unique marker and report, with a denominator, whether any "
                        "response carries another session's marker. An observation, not a gate: this "
                        "tool prints no verdicts.")
    p.add_argument("--tokens", type=int, default=150000,
                   help="tokens per private document. Sizing note: four lanes must exceed the device "
                        "KV capacity (`--kv-capacity`, 262144 tokens in prod) to create pressure at "
                        "all -- 4 x ~50k+20k is only ~1.2x that and barely moves the counters, while "
                        "4 x ~170k is ~2.6x and forces spill/demotion")
    p.add_argument("--system-tokens", type=int, default=20000)
    p.add_argument("--max-tokens", type=int, default=64,
                   help="must exceed the reply the marker instruction asks for, or the control is "
                        "inert: every reply is truncated at this bound and the marker never appears")
    p.add_argument("--stagger", type=float, default=3.0)
    p.add_argument("--serial", action="store_true",
                   help="run the clients one after another instead of concurrently: the CONTROL arm. "
                        "With one lane occupied the engine must return each session its own marker "
                        "(own_marker ~100%%, foreign 0); a control that does not pass invalidates the "
                        "concurrent arm rather than confirming it.")
    p.add_argument("--expect-clean", action="store_true",
                   help="assert the arm's expectation and exit nonzero otherwise: with --serial, require "
                        "own_marker >= 90%% and foreign == 0; without it, require foreign == 0.")
    p.add_argument("--json", dest="json_out", default=None)
    args = p.parse_args()

    # Context budget, printed because exceeding it is silent from the client's side until the last
    # turns 400: prod's --max-context is 262144, and 4x40000 + 40x5000 + 20000 system was ~260k, which
    # is why 20 of 160 turns failed with HTTP 400 in the run of 2026-09-25.
    final_ctx = args.system_tokens + args.tokens + max(0, args.rounds - 1) * args.turn_tokens
    args._peer_markers = {}
    print(f"context budget: final per-session ~{final_ctx} tokens "
          f"({100.0 * final_ctx / 262144:.0f}% of prod's 262144 cap)"
          + ("  <-- WILL 400 on the last turns" if final_ctx > 250000 else ""))
    print(f"prod load: {args.sessions} sessions x {args.rounds} rounds, "
          f"~{args.system_tokens} shared + ~{args.tokens} private tokens, temp=0, "
          f"target http://{args.host}:{args.port}")
    results, threads = [], []
    barrier = threading.Barrier(max(1, args.sessions), timeout=600)
    for i in range(args.sessions):
        threads.append(Client(i, args, barrier, results))
    args._peer_markers = {i: [t.marker] for i, t in enumerate(threads)}
    # Harness hygiene, printed with a denominator: does any client's own prompt text contain another
    # client's marker? If it does, a "foreign" hit in a reply proves nothing about the engine -- which
    # is the check that has to pass before any contamination claim, given how often this one check has
    # been inert today (0 of 32, 0 of 140, then 27 phantom hits from a prefix match).
    dirty = 0
    for i, t in enumerate(threads):
        blob = " ".join(str(m.get("content", "")) for m in t.messages)
        peers = [m for j, ms in args._peer_markers.items() if j != i for m in ms]
        hits = [p for p in peers if p in blob]
        if hits:
            dirty += 1
            print(f"  HARNESS-DIRTY client {i}: its own prompt contains {hits}")
    print(f"harness hygiene: {dirty} of {len(threads)} clients carry a peer marker in their own prompt")
    t0 = time.time()
    if args.serial:
        for t in threads:   # one lane at a time: the control arm
            t.start()
            t.join()
    else:
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    wall = time.time() - t0

    ok = [r for r in results if r.get("ok")]
    bad = [r for r in results if not r.get("ok")]
    print(f"\n{len(results)} requests in {wall:.0f}s; ok={len(ok)} failed={len(bad)}")
    if args.marker_check:
        f = [r for r in ok if r.get("foreign_markers")]
        print(f"cross-session marker matches: {len(f)} of {len(ok)} completed turns"
              f"{'  <-- ' + str([(r['client'], r['round'], r['foreign_markers']) for r in f]) if f else ''}")
    for r in sorted(results, key=lambda r: (str(r.get('round')), r.get('client'))):
        if r.get("ok"):
            extra = ""
            if args.marker_check:
                extra = (f" own_marker={r.get('own_marker')} foreign={r.get('foreign_markers')}"
                         if r.get("foreign_markers") else f" own_marker={r.get('own_marker')}")
            print(f"  client {r['client']} round {r['round']}: {r['wall']:6.2f}s "
                  f"in={r.get('input_tokens')} out={r.get('output_tokens')}{extra}")
        else:
            print(f"  client {r['client']} round {r['round']}: FAILED after {r['wall']}s "
                  f":: {r.get('error')}")
    verdict = 0
    if args.marker_check and args.expect_clean:
        own = sum(1 for x in ok if x.get("own_marker"))
        fo = [x for x in ok if x.get("foreign_markers")]
        if args.serial:
            if not ok or own < 0.9 * len(ok):
                print(f"CONTROL INVALID: own_marker {own} of {len(ok)} (<90%) -- the arm cannot "
                      f"confirm or refute anything until the model reproduces its own marker")
                verdict = 2
            elif fo:
                print(f"CONTROL INVALID: {len(fo)} foreign match(es) while serialized -- the harness "
                      f"or the client is the problem, not concurrency")
                verdict = 2
            else:
                print(f"CONTROL OK: own_marker {own}/{len(ok)}, foreign 0")
        else:
            if fo:
                print(f"REPRODUCED: {len(fo)} of {len(ok)} turns carried another session's marker")
                verdict = 1
            else:
                print(f"no foreign marker in {len(ok)} concurrent turns (control own_marker {own}/{len(ok)})")
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump({"config": vars(args), "results": results}, f, indent=1)
    return 1 if bad else verdict


if __name__ == "__main__":
    sys.exit(main())
