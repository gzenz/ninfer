#!/usr/bin/env python3
"""C1 concurrent-thrash probe (the 08-27 #98 repro).

Two CONCURRENT anthropic_messages sessions, ~200k tokens each, with
cache_control breakpoints (system + last assistant message), and the
original entitlement demands (max_tokens 32000 / 64000). Two alternation
rounds: round 1 = cold (both sessions prefill from scratch, concurrently);
round 2 = warm (each session re-touches its own ~200k prefix while the
other session is active — the exact pattern that thrashed the old
architecture: fresh continuations evicted instead of parked to host).

Pass criterion (the 08-28 benchmark bar): round-2 turns show prefix reuse
and wall time well below a full re-prefill. A round-2 turn with wall >=
0.6 x its round-1 wall is flagged RE-PREFILL.

Run inside e2e-swap-cmp.sh with CTX=200k (prod-parity sizing:
--host-kv-mib 30720 --host-state-slots 112 --max-concurrency 2).
"""

import argparse
import json
import os
import random
import re
import sys
import threading
import time
import urllib.request

CHARS_PER_TOKEN = 5.9  # calibrated 2026-09-18 (longctx_recall_probe.py)
SERVE_LOG = os.path.expanduser("~/ninfer-serve.log")
REQUEST_LOG = os.path.expanduser("~/ninfer-requests.jsonl")
REUSE_LINE_RE = re.compile(r"reuse=([a-z_]+)")
CRASH_RE = re.compile(r"bad_alloc|terminate called|Segmentation|core dumped|Killed \d|NINFER_EXIT=[1-9]")


def build_doc(tokens, rng):
    """Deterministic filler document of ~`tokens` tokens (same style as the
    longctx probe: distinct-ish sentences, not trivially compressible)."""
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


class AnthropicSession:
    def __init__(self, name, doc_tokens, max_tokens, args):
        self.name = name
        self.max_tokens = max_tokens
        self.args = args
        self.rng = random.Random(sum(ord(c) for c in name) + 7)
        self.doc = build_doc(doc_tokens, self.rng)
        self.system = [
            {"type": "text",
             "text": ("You are a coding assistant. Answer questions about the "
                      "document provided. Keep every answer to at most two "
                      "sentences."),
             "cache_control": {"type": "ephemeral"}}
        ]
        self.messages = []
        self.turns = []

    def _post(self, payload):
        t0 = time.monotonic()
        try:
            with urllib.request.urlopen(urllib.request.Request(
                    f"http://{self.args.host}:{self.args.port}/v1/messages",
                    data=json.dumps(payload).encode(),
                    headers={"Content-Type": "application/json",
                             "x-api-key": "test", "anthropic-version": "2023-06-01"},
                    method="POST"), timeout=self.args.timeout) as r:
                out = json.load(r)
            wall = time.monotonic() - t0
            usage = out.get("usage", {}) or {}
            rec = {"session": self.name, "wall_s": round(wall, 1),
                   "input_tokens": usage.get("input_tokens"),
                   "cache_read_input_tokens": usage.get("cache_read_input_tokens"),
                   "output_tokens": usage.get("output_tokens")}
            self.turns.append(rec)
            print(f"  {self.name}: wall={rec['wall_s']}s in={rec['input_tokens']} "
                  f"cache_read={rec['cache_read_input_tokens']} out={rec['output_tokens']}")
            # persist the assistant reply so the next turn is a real
            # multi-turn conversation (and has an assistant message to
            # carry the cache breakpoint)
            content = out.get("content") or []
            text = "".join(b.get("text", "") for b in content if b.get("type") == "text")
            self.messages.append({"role": "assistant", "content": text or "(no text)"})
            return rec
        except Exception as exc:
            rec = {"session": self.name, "wall_s": round(time.monotonic() - t0, 1),
                   "error": repr(exc)[:200]}
            self.turns.append(rec)
            print(f"  {self.name}: ERROR {rec['error']}")
            return rec

    def turn(self, round_idx):
        if round_idx == 1:
            user = self.doc + "\n\n---\n\nWhat is the overall theme of this document? Answer briefly."
            self.messages.append({"role": "user", "content": user})
        else:
            # short follow-up turn (+~2k filler) — the 08-27 repro's "+2 turns"
            user = build_doc(2000, self.rng) + f"\n\nQuestion {round_idx}: one more brief question about the document."
            self.messages.append({"role": "user", "content": user})
            # cache breakpoint on the last assistant message (the growing
            # shared prefix) — mirrors Claude Code's per-turn breakpoints
            for m in reversed(self.messages):
                if m["role"] == "assistant":
                    m["cache_control"] = {"type": "ephemeral"}
                    break
        payload = {
            "model": self.args.model,
            "system": self.system,
            "messages": self.messages,
            "max_tokens": self.max_tokens,
            "temperature": 1.0,
            "top_p": 0.95,
        }
        return self._post(payload)


def read_reuse_evidence():
    """Reuse evidence from both sources (whichever the build emits)."""
    reuse_counts, prefix_hits = {}, 0
    try:
        with open(SERVE_LOG, "r", errors="replace") as f:
            for line in f:
                for m in REUSE_LINE_RE.finditer(line):
                    reuse_counts[m.group(1)] = reuse_counts.get(m.group(1), 0) + 1
    except OSError:
        pass
    try:
        with open(REQUEST_LOG, "r", errors="replace") as f:
            for line in f:
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                prefix_hits += rec.get("prefix_cache_hit_tokens", 0) or 0
    except OSError:
        pass
    return reuse_counts, prefix_hits


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--model", default="qwen3.8-27b")
    p.add_argument("--tokens-a", type=int, default=180000)
    p.add_argument("--tokens-b", type=int, default=185000)
    p.add_argument("--timeout", type=int, default=900)
    p.add_argument("--json", dest="json_out", default="/tmp/ninfer-c1-probe.json")
    args = p.parse_args()

    print(f"C1 probe: A~{args.tokens_a}k (max_tokens=32000) + B~{args.tokens_b}k (max_tokens=64000), 2 rounds")
    a = AnthropicSession("A", args.tokens_a, 32000, args)
    b = AnthropicSession("B", args.tokens_b, 64000, args)

    for rnd in (1, 2):
        print(f"\n=== Round {rnd} (concurrent) ===")
        t0 = time.time()
        ta, tb = threading.Thread(target=a.turn, args=(rnd,)), threading.Thread(target=b.turn, args=(rnd,))
        ta.start(); tb.start()
        ta.join(); tb.join()
        print(f"  round wall: {time.time() - t0:.0f}s")

    # verdict
    verdicts = []
    for s in (a, b):
        t1, t2 = s.turns[0], s.turns[1]
        if t1.get("error") or t2.get("error"):
            verdicts.append({"session": s.name, "verdict": "ERROR",
                             "t1": t1, "t2": t2})
            continue
        ratio = t2["wall_s"] / t1["wall_s"] if t1["wall_s"] > 0 else None
        re_prefill = ratio is not None and ratio >= 0.6
        verdicts.append({
            "session": s.name,
            "t1_wall_s": t1["wall_s"], "t2_wall_s": t2["wall_s"],
            "warm_ratio": round(ratio, 2) if ratio else None,
            "cache_read_t2": t2.get("cache_read_input_tokens"),
            "verdict": "RE-PREFILL" if re_prefill else "WARM",
        })
    reuse_counts, prefix_hits = read_reuse_evidence()
    out = {
        "args": vars(args),
        "verdicts": verdicts,
        "reuse_serve_log": reuse_counts,
        "prefix_cache_hit_tokens_total": prefix_hits,
        "pass": all(v["verdict"] == "WARM" for v in verdicts),
    }
    with open(args.json_out, "w") as f:
        json.dump(out, f, indent=1)
    print(f"\nSUMMARY: " + " | ".join(f"{v['session']}={v['verdict']}" for v in verdicts)
          + f"  prefix_hits={prefix_hits}  reuse_log={reuse_counts}")
    print(f"PASS={out['pass']} -> {args.json_out}")
    return 0 if out["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
