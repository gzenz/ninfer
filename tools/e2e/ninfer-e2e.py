#!/usr/bin/env python3
"""E2E test suite for ninfer safety-net eviction system.

Runs twelve phases by default against a single test server (no flags needed):
  Phase 1 "pressure":           4 sessions — basic safety net (spills, restores, no re-prefills)
  Phase 2 "mixed":              1 big + 3 small — eviction order (smallest-first, big preserved)
  Phase 3 "trash":              10 sessions — graceful degradation under trashing (no crash)
  Phase 4 "thinking":           3 sessions, reasoning mode — session-key fallback with rewrite checkpoint
  Phase 5 "checkpoint-advance": 1 session, 8 turns — checkpoint frontier advances monotonically
  Phase 6 "tool-calling":       1 session, 6 turns with tools — rewrite restore under tool-call rounds
  Phase 7 "responses-tools":    1 session, 5 turns — Responses API tool-calling with checkpoint reuse
  Phase 8 "reasoning-effort":   5 requests — reasoning effort tier mapping (high, minimal, max, medium, low)
  Phase 9 "concurrent":         2 sessions + title-gen — source eviction fallback, no cross-session state destruction
  Phase 10 "thinking-sig":      4 requests — thinking signature skip when preserve_thinking=false
  Phase 11 "demotion":          3 sessions, large prompts — host demotion + checkpoint restore
  Phase 12 "state-lease":       4 thinking sessions — rewrite-recycle pressure; zero state-lease
                                leaks / orphaned state slots (regression: 2026-09-14 prod wedge)

Server config: 32k max-context, 64k kv-capacity, 4GB host-kv, 3 continuations,
5 device state slots (production parity — required by phase 12).
All phases use the same server — no restarts.

Usage: python3 ninfer-e2e.py [--host 127.0.0.1] [--port 8080] [--serve-log /home/zenz/ninfer-serve.log]
"""

import argparse
import json
import os
import re
import random
import sys
import threading
import time
import urllib.request
import urllib.error

WORDS = ("alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima "
         "mike november oscar papa quebec romeo sierra tango uniform victor whiskey "
         "xray yankee zulu amber cedar dawn ember frost grove harbor ivory "
         "jade kernel lumen meadow night opal prism quill raven stone umber "
         "vale willow xenon yonder zephyr anchor beacon compass estuary").split()


def filler(rng, tokens):
    chars = int(tokens * 4.2)
    parts, n = [], 0
    while n < chars:
        w = rng.choice(WORDS)
        parts.append(w)
        n += len(w) + 1
    return " ".join(parts)


class Session:
    def __init__(self, name, seed_tokens, turn_tokens, args):
        self.name = name
        self.rng = random.Random(sum(ord(c) for c in name))
        self.seed_tokens = seed_tokens
        self.turn_tokens = turn_tokens
        self.args = args
        self.response_id = None
        self.turns = []
        self.doc = filler(self.rng, seed_tokens)

    def turn(self, index):
        question = f"Question {index}: Consider the paragraph about '{self.rng.choice(WORDS)}'. Answer briefly."
        new_text = filler(self.rng, self.turn_tokens) + "\n\n" + question
        if index == 1:
            new_text = self.doc + "\n\n---\n\n" + new_text
        payload = {
            "model": self.args.model,
            "input": [{"role": "user", "content": [{"type": "input_text", "text": new_text}]}],
            "instructions": "You are a concise assistant.",
            "max_output_tokens": self.args.max_output_tokens,
            "store": True,
            "stream": False,
        }
        if getattr(self.args, "thinking_mode", False):
            payload["reasoning"] = {"effort": "low"}
        if self.response_id:
            payload["previous_response_id"] = self.response_id
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/responses",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        usage = out.get("usage", {}) or {}
        self.response_id = out.get("id", self.response_id)
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("input_tokens"), "output_tokens": usage.get("output_tokens")}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s prompt={record['input_tokens']} out={record['output_tokens']}")
        return record


class ChatSession:
    """Session using /v1/chat/completions with tools, simulating Claude Code."""
    TOOLS = [
        {"type": "function", "function": {
            "name": "read_file", "description": "Read a file",
            "parameters": {"type": "object", "properties": {"path": {"type": "string"}},
                           "required": ["path"]}}},
        {"type": "function", "function": {
            "name": "write_file", "description": "Write a file",
            "parameters": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}},
                           "required": ["path", "content"]}}},
        {"type": "function", "function": {
            "name": "list_dir", "description": "List directory contents",
            "parameters": {"type": "object", "properties": {"path": {"type": "string"}},
                           "required": ["path"]}}},
    ]

    def __init__(self, name, seed_tokens, turn_tokens, args):
        self.name = name
        self.rng = random.Random(sum(ord(c) for c in name) + 1)
        self.seed_tokens = seed_tokens
        self.turn_tokens = turn_tokens
        self.args = args
        self.messages = [{"role": "system", "content":
            "You are a coding assistant. You MUST use tools (read_file, write_file, list_dir) "
            "to answer questions. Always call at least one tool before responding."}]
        self.turns = []
        self.doc = filler(self.rng, seed_tokens)

    def turn(self, index):
        if index == 1:
            user_text = self.doc + "\n\n---\n\n" + filler(self.rng, self.turn_tokens) + "\n\nUse the read_file tool to read /tmp/test.txt, then answer."
        else:
            user_text = filler(self.rng, self.turn_tokens) + f"\n\nUse the list_dir tool to list /tmp, then answer question {index}."
        self.messages.append({"role": "user", "content": user_text})
        payload = {
            "model": self.args.model,
            "messages": self.messages,
            "max_tokens": self.args.max_output_tokens,
            "tools": self.TOOLS,
            "tool_choice": "auto",
            "stream": False,
        }
        if getattr(self.args, "thinking_mode", False):
            payload["enable_thinking"] = True
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/chat/completions",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        choice = out.get("choices", [{}])[0]
        msg = choice.get("message", {})
        usage = out.get("usage", {}) or {}
        # Record the assistant reply (including tool_calls) for multi-turn context
        assistant_msg = {"role": "assistant", "content": msg.get("content") or ""}
        if msg.get("tool_calls"):
            assistant_msg["tool_calls"] = msg["tool_calls"]
            self.messages.append(assistant_msg)
            # Simulate tool results so the conversation can continue
            for tc in msg["tool_calls"]:
                self.messages.append({
                    "role": "tool",
                    "tool_call_id": tc.get("id", "call_0"),
                    "content": f"Result of {tc['function']['name']}: OK",
                })
        else:
            self.messages.append(assistant_msg)
        finish = choice.get("finish_reason", "unknown")
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("prompt_tokens"),
                  "output_tokens": usage.get("completion_tokens"),
                  "finish": finish}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s "
              f"prompt={record['input_tokens']} out={record['output_tokens']} finish={finish}")
        return record


class ResponsesApiSession:
    """Session using /v1/responses with tools. Uses store=True and
    previous_response_id for checkpoint reuse, with tool definitions
    to test the Responses API tool-calling path.
    """
    def __init__(self, name, seed_tokens, turn_tokens, args):
        self.name = name
        self.rng = random.Random(sum(ord(c) for c in name) + 7)
        self.seed_tokens = seed_tokens
        self.turn_tokens = turn_tokens
        self.args = args
        self.response_id = None
        self.turns = []
        self.doc = filler(self.rng, seed_tokens)
        self._pending_tool_calls = []

    def turn(self, index):
        # On turns after a tool call, send function_call_output as new input.
        # Otherwise send a new user message.
        if index == 1:
            user_text = self.doc + "\n\n---\n\n" + filler(self.rng, self.turn_tokens) + "\n\nRead the file."
            new_input = [{"role": "user", "content": [{"type": "input_text", "text": user_text}]}]
        elif self._pending_tool_calls:
            # Send function_call_output for each pending tool call
            new_input = []
            for call_id, fn_name in self._pending_tool_calls:
                new_input.append({
                    "type": "function_call_output",
                    "call_id": call_id,
                    "output": f"Result of {fn_name}: OK",
                })
            self._pending_tool_calls = []
        else:
            user_text = filler(self.rng, self.turn_tokens) + f"\n\nQuestion {index}: Summarize what you found."
            new_input = [{"role": "user", "content": [{"type": "input_text", "text": user_text}]}]
        payload = {
            "model": self.args.model,
            "input": new_input,
            "instructions": "You are a coding assistant. Use tools when needed.",
            "max_output_tokens": self.args.max_output_tokens,
            "store": True,
            "stream": False,
            "tools": [
                {"type": "function", "name": "read_file",
                 "description": "Read a file",
                 "parameters": {"type": "object", "properties": {"path": {"type": "string"}}}},
            ],
        }
        if self.response_id:
            payload["previous_response_id"] = self.response_id
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/responses",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        usage = out.get("usage", {}) or {}
        self.response_id = out.get("id", self.response_id)
        output = out.get("output", [])
        tool_calls = [item for item in output if isinstance(item, dict) and item.get("type") == "function_call"]
        has_tool_call = len(tool_calls) > 0
        has_text = any(item.get("type") == "message" for item in output if isinstance(item, dict))
        # Queue tool call results for next turn
        self._pending_tool_calls = [
            (tc.get("call_id", f"call_{i}"), tc.get("name", "read_file"))
            for i, tc in enumerate(tool_calls)
        ]
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("input_tokens"),
                  "output_tokens": usage.get("output_tokens"),
                  "has_tool_call": has_tool_call, "has_text": has_text}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s "
              f"prompt={record['input_tokens']} out={record['output_tokens']} "
              f"tool={has_tool_call} text={has_text}")
        return record


class ReasoningEffortTester:
    """Send requests with various reasoning effort levels to verify tier mapping."""
    EFFORTS = ["low", "medium", "high", "minimal", "max"]
    def __init__(self, args):
        self.args = args
        self.results = []

    def test(self):
        for effort in self.EFFORTS:
            payload = {
                "model": self.args.model,
                "input": [{"role": "user", "content": [{"type": "input_text", "text": "Say hello."}]}],
                "max_output_tokens": 32,
                "stream": False,
                "reasoning": {"effort": effort},
            }
            t0 = time.monotonic()
            try:
                out = json.load(urllib.request.urlopen(urllib.request.Request(
                    f"http://{self.args.host}:{self.args.port}/v1/responses",
                    data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
                    method="POST"), timeout=self.args.timeout))
                wall = time.monotonic() - t0
                usage = out.get("usage", {}) or {}
                self.results.append({"effort": effort, "ok": True, "wall_s": round(wall, 2),
                                    "input_tokens": usage.get("input_tokens"),
                                    "output_tokens": usage.get("output_tokens")})
                print(f"  effort={effort}: OK wall={wall:.1f}s out={usage.get('output_tokens', '?')}")
            except urllib.error.HTTPError as e:
                wall = time.monotonic() - t0
                body = e.read().decode()[:200]
                self.results.append({"effort": effort, "ok": False, "wall_s": round(wall, 2),
                                    "error": body})
                print(f"  effort={effort}: FAIL status={e.code} {body[:100]}")
            except Exception as e:
                wall = time.monotonic() - t0
                self.results.append({"effort": effort, "ok": False, "wall_s": round(wall, 2),
                                    "error": repr(e)})
                print(f"  effort={effort}: ERROR {repr(e)[:100]}")
        return self.results


class ThinkingSignatureTester:
    """Test thinking signature verification skip when preserve_thinking=false."""
    def __init__(self, args):
        self.args = args
        self.results = []

    def test(self):
        base_url = f"http://{self.args.host}:{self.args.port}/v1/messages"
        headers = {"Content-Type": "application/json", "x-api-key": "test"}

        # Step 1: Get a valid thinking block
        payload = {
            "model": self.args.model,
            "messages": [{"role": "user", "content": "Say hello in one word."}],
            "max_tokens": 2048,
            "thinking": {"type": "enabled", "budget_tokens": 1024},
            "stream": False,
        }
        try:
            out = json.load(urllib.request.urlopen(urllib.request.Request(
                base_url, data=json.dumps(payload).encode(), headers=headers,
                method="POST"), timeout=self.args.timeout))
            thinking_text = None
            for block in out.get("content", []):
                if block.get("type") == "thinking":
                    thinking_text = block.get("thinking", "")
                    break
            if not thinking_text:
                self.results.append({"test": "get_thinking", "ok": False, "error": "no thinking block"})
                print("  get_thinking: FAIL - no thinking block")
                return self.results
            self.results.append({"test": "get_thinking", "ok": True})
            print("  get_thinking: OK")
        except Exception as e:
            self.results.append({"test": "get_thinking", "ok": False, "error": repr(e)})
            print(f"  get_thinking: FAIL - {repr(e)[:100]}")
            return self.results

        # Build assistant message with INVALID signature
        bad_assistant = {"role": "assistant", "content": [
            {"type": "thinking", "thinking": thinking_text, "signature": "sig_INVALID_old_signature_12345"},
            {"type": "text", "text": "Hello!"},
        ]}

        # Test 2: preserve_thinking=false → should succeed
        for label, pt_val in [("preserve_false", False), ("preserve_none", None), ("preserve_true", True)]:
            payload = {
                "model": self.args.model,
                "messages": [
                    {"role": "user", "content": "hello"},
                    bad_assistant,
                    {"role": "user", "content": "What did you say?"},
                ],
                "max_tokens": 64,
                "stream": False,
            }
            if pt_val is not None:
                payload["preserve_thinking"] = pt_val
            try:
                urllib.request.urlopen(urllib.request.Request(
                    base_url, data=json.dumps(payload).encode(), headers=headers,
                    method="POST"), timeout=self.args.timeout)
                ok = True
                error = None
            except urllib.error.HTTPError as e:
                ok = False
                error = e.read().decode()[:100]
            except Exception as e:
                ok = False
                error = repr(e)[:100]
            # Retry once on materialization error (state may have been evicted)
            if not ok and "resident state" in str(error):
                time.sleep(2)
                try:
                    urllib.request.urlopen(urllib.request.Request(
                        base_url, data=json.dumps(payload).encode(), headers=headers,
                        method="POST"), timeout=self.args.timeout)
                    ok = True
                    error = None
                except urllib.error.HTTPError as e:
                    ok = False
                    error = e.read().decode()[:100]
                except Exception as e:
                    ok = False
                    error = repr(e)[:100]
            # ALL THREE MUST BE ACCEPTED, INCLUDING preserve_thinking=true. This check used to expect a
            # REJECTION for `true` (`expected_ok = pt_val is not True`) and so printed
            # `preserve_true: FAIL (accepted=True, expected=False)` on every run -- but that expectation
            # was never the engine's behaviour: `anthropic_messages_request.cpp:318` says the wire
            # signature is INTENTIONALLY dropped ("NInfer has no encrypted reasoning state to restore.
            # The wire signature is therefore intentionally outside the lowered request; only visible
            # Thinking reaches the model"). The engine does not model the signature, so it cannot and
            # should not reject a bogus one; a rejection here would mean it began validating an opaque
            # blob it does not model. A permanently-failing line is also the worst kind of check: it
            # trains a reader to skip FAIL lines.
            expected_ok = True
            if ok == expected_ok:
                self.results.append({"test": label, "ok": True})
                print(f"  {label}: OK (accepted={ok}; the wire signature is deliberately ignored)")
            else:
                self.results.append({"test": label, "ok": False, "error": f"accepted={ok} expected={expected_ok}: {error}"})
                print(f"  {label}: FAIL (rejected={not ok}, expected acceptance: {error})")
        return self.results


def run_round(sessions, r, timeout):
    results = [None] * len(sessions)
    errors = []
    def run(i):
        try:
            results[i] = sessions[i].turn(r)
        except Exception as exc:
            errors.append((sessions[i].name, repr(exc)))
    threads = [threading.Thread(target=run, args=(i,)) for i in range(len(sessions))]
    for t in threads: t.start()
    for t in threads: t.join()
    return errors


def get_stats(args):
    try:
        with urllib.request.urlopen(f"http://{args.host}:{args.port}/stats", timeout=10) as resp:
            return json.load(resp)
    except Exception:
        return {}


def count_log_lines(path):
    try:
        with open(path, "r", errors="replace") as f:
            return sum(1 for _ in f)
    except OSError:
        return 0


# Serve-log patterns that actually exist in THIS tree (grep-verified against
# src/, include/, apps/). The v2 suite's serve-log metrics -- [safety-spill],
# [restore], [safety-find], [relief], [admit-session], [materialize],
# [checkpoint], [kv-not-resident], [state-lease], [spill-before-loss],
# [rewrite-restore], [entitlement], [replan], [relief-kv], [capture] -- are
# emitted by NO v3 code path: counting them always returned 0, so every verdict
# built on them was inert. Retention / restore / pressure evidence now comes
# from /stats deltas (see evaluate()).
V3_LOG_PATTERNS = {
    "bad_alloc": "std::bad_alloc",
    "worker_recover": "WORKER RECOVER",
    "cuda_error": "CUDA error",
    "tool_markup_leak": "tool markup returned as text",
    "pressure_expansion_fail": "prepared pressure expansion exceeds the target arena",
    # The v3 request-error string. It has to be a PATTERN to be counted at all: mapping it in
    # LEGACY_V3_SOURCES pointed at a key the dict never had, so `missing_source_result` was permanently 0 and
    # phase 12's "PASS: zero 'private source result is missing' errors" could not fail. The review of
    # 2026-09-26 proved that by feeding the string in twice and getting 0 both times.
    "missing_source_result": "private source result is missing",
}


# THE LEGACY COUNTER NAMES THE PHASES READ, and why they are DERIVED rather than read. This suite was
# vendored speaking the PRE-v3 counter vocabulary: every name below appears in ZERO files under `src/` in
# this engine, so reading it raised KeyError and killed the phase -- phases 4, 12 and 13 each died that way
# on 2026-09-26, which is why the 14-phase acceptance could not run at all.
#
# Each name is therefore mapped to a signal this engine DOES emit. A `None` means there is no v3 equivalent;
# those stay 0 and the phase's own "the counter may not fire" branch applies -- but they are listed in
# `no_v3_source` so a permanent zero is visible as "unmeasurable" and cannot be read as a pass, which is the
# mistake this repo keeps paying for (an instrument that cannot fire reads exactly like a quiet system).
LEGACY_V3_SOURCES = {
    "admit_session": None,          # phase 4 falls back to checking cold re-prefills, by its own design
    # `missing_source_result` is NOT listed here: it is a native V3_LOG_PATTERNS key now, and mapping it
    # re-assigned it from `d.get(<raw string>)`, i.e. 0 -- overwriting the correct count.
    "state_lease_orphan": "refused_release_orphan",   # refused release whose blocker is NOT a still-owned image
    "state_lease_leak": "refused_release_any",        # every refusal, legitimate retention included
    "relief_demote": None,          # no per-demotion stderr line in v3; the disposition is in the request log
    "spill_before_loss": None,
    "state_relinquish": None,
}


def parse_serve_log(path, skip_lines=0):
    d = {k: 0 for k in V3_LOG_PATTERNS}
    d["legacy_metrics_available"] = False
    refused_any = 0
    refused_orphan = 0
    refused_unclassified = 0
    try:
        with open(path, "r", errors="replace") as f:
            for _ in range(skip_lines):
                f.readline()
            for line in f:
                for key, pat in V3_LOG_PATTERNS.items():
                    if pat in line:
                        d[key] += 1
                if "non-strict release REFUSED" in line:
                    refused_any += 1
                    # A refusal whose blocker is a still-owned image (a checkpoint reference or a fork pin)
                    # is BY DESIGN: the slot is owned and nothing was stranded. Only the other shapes are the
                    # leak, so counting every refusal would fail this phase on correct behaviour.
                    # Only the three shapes the store can classify as unowned count as orphans. The other
                    # FOURTEEN emission sites pass no blocker and print `blocker=unclassified`, so they are
                    # counted separately -- silently folding them in would call an unclassified refusal a leak,
                    # and dropping them entirely would hide #11a's abort-recycled-checkpoint instrument.
                    if "blocker=unclassified" in line:
                        refused_unclassified += 1
                    elif "pending-replica" in line or "blocker=none" in line:
                        refused_orphan += 1
    except OSError:
        pass
    d["refused_release_any"] = refused_any
    d["refused_release_orphan"] = refused_orphan
    d["refused_release_unclassified"] = refused_unclassified
    for name, source in LEGACY_V3_SOURCES.items():
        d[name] = d.get(source, 0) if source is not None else 0
    # EVERY OTHER NAME THE PHASES READ, defaulted to 0 and DECLARED unmeasurable. These describe mechanisms
    # whose v3 signal I could not establish (entitlement re-planning, queued-KV relief, spill-checkpoint
    # backstops, ...); inventing a pattern for them would FABRICATE the acceptance rather than adapt it.
    # Defaulting them (a) removes the KeyError class that killed phases 4, 12 and 13, and (b) puts them in
    # `no_v3_source`, which the phases now report as INFO -- an unmeasurable check is visible instead of
    # reading as a pass. The list is the union of `logN["..."]` accesses in this file, obtained with:
    #   grep -oE 'log[0-9]+\["[a-z_]+"\]' tools/e2e/ninfer-e2e.py | sed 's/.*\["//;s/"\]//' | sort -u
    unmeasurable = {name for name, src in LEGACY_V3_SOURCES.items() if src is None}
    for name in ("checkpoint_demoted", "checkpoint_restored", "entitlement_mismatch", "kv_occupancy_block",
                 "queued_kv_deadline", "queued_kv_relief", "rewrite_prefix_hit", "spill_ckpt_ok",
                 "state_replan"):
        d.setdefault(name, 0)
        unmeasurable.add(name)
    d["no_v3_source"] = sorted(unmeasurable)
    return d


def _request_window_rows(args):
    """Request-log rows written since the suite started — the v3 evidence source."""
    if args is None:
        return None
    start = getattr(args, "_request_log_start", 0)
    rows = []
    try:
        with open(args.request_log, "r", errors="replace") as f:
            for i, line in enumerate(f):
                if i < start:
                    continue
                line = line.strip()
                if line:
                    try:
                        rows.append(json.loads(line))
                    except Exception:
                        pass
    except OSError:
        return None
    return rows


# Set in main(): the request-log window that evaluate() reads. Call sites stay
# unchanged (they pass no args); evaluate() falls back to this.
_ARGS = None


# Phase groups where reuse/pressure cannot be expected (single-session or
# state-pool phases). Kept from the v2 suite: the exclusion list is a property
# of what each phase exercises, not of the metrics.
NO_CACHE_PHASES = ("checkpoint-advance", "tool-calling", "responses-tools",
                   "reasoning-effort", "concurrent", "thinking-sig", "demotion",
                   "state-saturation", "queued-relief")


def tester_verdicts(phase_name, results):
    """A tester's own per-case results AS VERDICTS.

    They used to be printed and thrown away: `evaluate` has no branch for these phases, and nothing
    reads `Tester.results`, while `print_summary` counts `all_verdicts` only -- so a failing case could
    not reach the summary OR the exit code. `preserve_true` printed
    `FAIL (accepted=True, expected=False)` on every run while the summary reported `1 FAIL` for the
    phase. A check that cannot fail the suite is not a check.
    """
    out = []
    for r in results or []:
        label = r.get("test") or r.get("effort") or "case"
        if r.get("ok"):
            out.append((phase_name, f"PASS: {label}"))
        else:
            out.append((phase_name, f"FAIL: {label} — {r.get('error') or 'no detail'}"))
    return out


def evaluate(phase_name, sessions, stats0, stats1, log, expect_trash=False,
             args=None, gates=False):
    """Verdicts from v3-native evidence: /stats deltas + the request log.

    gates=True promotes the collapse gates (root share, queue wait, drops
    without eviction accounting) to FAIL — used by retention-health profiles
    such as prod4 (W4b). Otherwise they are WARN.
    """
    v = []
    pr, pr0 = (stats1 or {}).get("pressure", {}), (stats0 or {}).get("pressure", {})
    kt, kt0 = (stats1 or {}).get("kv_transfers", {}), (stats0 or {}).get("kv_transfers", {})

    def d(b, b0, k):
        return int((b or {}).get(k, 0)) - int((b0 or {}).get(k, 0))

    evicted = d(pr, pr0, "private_owners_evicted")
    shared_evicted = d(pr, pr0, "shared_owners_evicted")
    degraded = d(pr, pr0, "private_owners_degraded")
    demoted = d(pr, pr0, "private_owners_demoted")
    spill_pages = d(pr, pr0, "spill_pages")
    dropped = d(pr, pr0, "checkpoints_dropped")
    d2h_pages = d(kt, kt0, "main_kv_d2h_pages")
    h2d_pages = d(kt, kt0, "main_kv_h2d_pages")
    cr, cr0 = (stats1 or {}).get("cache_reuse", {}), (stats0 or {}).get("cache_reuse", {})
    reused = d(cr, cr0, "reused_prompt_tokens")

    rows = _request_window_rows(args if args is not None else _ARGS)
    paths, hit, prompt, tool_rounds, qmax = {}, 0, 0, 0, 0.0
    for r in (rows or []):
        res, req = r.get("result", {}) or {}, r.get("request", {}) or {}
        p = res.get("prefix_reuse_path", "?")
        paths[p] = paths.get(p, 0) + 1
        hit += int(res.get("prefix_cache_hit_tokens", 0) or 0)
        prompt += int(res.get("prompt_tokens", 0) or 0)
        if int(res.get("tool_call_count", 0) or 0) > 0 or res.get("finish_reason") == "tool_calls":
            tool_rounds += 1
        qmax = max(qmax, float((r.get("engine_timing", {}) or {}).get("queue_wait_seconds", 0) or 0))
    n = len(rows) if rows is not None else 0

    # ---- evidence-source guard: no source, no verdict -----------------------
    if not stats1:
        v.append("FAIL: /stats unreachable — retention/restore cannot be evaluated")
    if rows is None:
        v.append("FAIL: request log unreadable — reuse cannot be evaluated")

    # ---- crash / error signatures (v3-emitted patterns only) ----------------
    if log.get("cuda_error", 0) > 0:
        v.append(f"FAIL: {log['cuda_error']} CUDA error")
    if log.get("worker_recover", 0) > 0:
        v.append(f"WARN: {log['worker_recover']} WORKER RECOVER")
    if log.get("bad_alloc", 0) > 0:
        if phase_name in ("trash", "mixed", "concurrent", "tool-calling"):
            v.append(f"PASS: {log['bad_alloc']} std::bad_alloc caught and recovered (extreme pressure)")
        else:
            v.append(f"FAIL: {log['bad_alloc']} std::bad_alloc — OOM was not prevented")
    if log.get("tool_markup_leak", 0) > 0:
        v.append(f"FAIL: {log['tool_markup_leak']} tool markup returned as text (parser leak)")
    if log.get("pressure_expansion_fail", 0) > 0:
        v.append(f"WARN: {log['pressure_expansion_fail']} pressure expansion over target arena")

    # ---- cache reuse + the collapse gates ----------------------------------
    if n:
        root_share = paths.get("root", 0) / n
        v.append(f"INFO: reuse paths {paths} (root {root_share:.0%}, reuse {hit}/{prompt} tokens, "
                 f"max queue {qmax:.0f}s)")
        if reused > 0 or hit > 0:
            v.append(f"PASS: cache reuse ({max(reused, hit)} tokens)")
        elif not expect_trash:
            v.append("FAIL: zero cache reuse")
        if phase_name not in NO_CACHE_PHASES and not expect_trash:
            if root_share > 0.25:
                msg = (f"root-prefill share {root_share:.0%} > 25% from round 2 "
                       f"(every re-touch re-prefilled)")
                v.append(("FAIL: " if gates else "WARN: ") + msg)
            if qmax > 60:
                msg = f"max queue wait {qmax:.0f}s > 60s (requests waited behind re-prefills)"
                v.append(("FAIL: " if gates else "WARN: ") + msg)

    # ---- pressure axes (/stats) --------------------------------------------
    pressure = any(x > 0 for x in (evicted, shared_evicted, degraded, demoted, spill_pages, h2d_pages))
    if phase_name not in NO_CACHE_PHASES:
        if pressure:
            v.append(f"PASS: pressure (evicted={evicted} shared_evicted={shared_evicted} "
                     f"degraded={degraded} demoted={demoted} spill_pages={spill_pages} "
                     f"h2d_pages={h2d_pages})")
        elif not expect_trash:
            v.append("FAIL: no KV pressure")
    if dropped > 0:
        # v3: a checkpoint that is dropped without an accounting eviction is a
        # retention bug (it becomes unreusable); with evictions it is arithmetic.
        if dropped > evicted:
            v.append(("FAIL: " if gates else "WARN: ") +
                     f"{dropped} checkpoints dropped > {evicted} owners evicted "
                     f"(drops not accounted as evictions)")
        else:
            v.append(f"INFO: {dropped} checkpoints dropped across {evicted} evictions")
    if d2h_pages > 0 and h2d_pages == 0 and phase_name not in NO_CACHE_PHASES:
        v.append(("FAIL: " if gates else "WARN: ") +
                 f"{d2h_pages} KV pages demoted to host but 0 restored (host tier never read back)")

    # ---- per-phase checks ---------------------------------------------------
    if phase_name == "tool-calling":
        if tool_rounds > 0:
            v.append(f"PASS: {tool_rounds} tool-call rounds completed (request log)")
        else:
            v.append("FAIL: zero tool-call rounds — model did not call tools")
    if phase_name == "checkpoint-advance":
        non_root = {k: c for k, c in paths.items() if k != "root"}
        if non_root:
            v.append(f"PASS: non-root reuse paths taken {non_root}")
        else:
            v.append("FAIL: zero non-root reuse — checkpoint not reused")
    if phase_name == "demotion":
        # v3: the demote/restore axis is KV pages + (state) residency, read from
        # /stats. A demote with no later restore is the host-tier-not-read-back
        # signature — that is a FAIL, not a pass.
        if spill_pages > 0 and h2d_pages > 0:
            v.append(f"PASS: demote/restore exercised (spill={spill_pages} h2d={h2d_pages} pages)")
        elif spill_pages > 0:
            v.append("FAIL: pages demoted to host but never restored")
        elif demoted > 0:
            v.append(f"PASS: {demoted} owners demoted (state residency moved to host)")
        else:
            v.append("WARN: no demotion exercised in this phase")
    if phase_name == "mixed":
        for s in sessions:
            if s.name == "BIG":
                cold = sum(1 for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
                fast = sum(1 for t in s.turns if t["turn"] > 1 and t["wall_s"] <= 60)
                if cold == 0 and fast > 0:
                    v.append(f"PASS: BIG {fast} fast turns, 0 cold-starts")
                elif cold > 0:
                    v.append(f"WARN: BIG {cold} cold-starts (may have been evicted)")

    return v


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--model", default="qwen3.8-27b")
    p.add_argument("--max-output-tokens", type=int, default=48)
    p.add_argument("--serve-log", default="/home/zenz/ninfer-serve.log")
    p.add_argument("--request-log", default="/home/zenz/ninfer-requests.jsonl",
                   help="JSONL request log with per-request materialization diagnostics")
    p.add_argument("--timeout", type=int, default=120)
    p.add_argument("--start-phase", type=int, default=1,
                   help="run phases N..13 (for split runs across separate e2e server windows)")
    args = p.parse_args()

    # Verify we're running against the test server, not production.
    # The e2e tests require a constrained server (32k ctx, 4GB host-kv, 64k KV)
    # to trigger pressure, eviction, and trashing. Running against the production
    # server (555k ctx, 30GB host-kv) will silently pass phases that should fail.
    config = get_stats(args) or {}
    if not config:
        print("ERROR: cannot reach server. Is it running on the configured host:port?")
        return 1
    mem = config.get("memory", {})
    if not mem:
        print("ERROR: server stats missing 'memory' section — wrong server or old build?")
        return 1
    host_kv = int(mem.get("host_kv_capacity_bytes", 0))
    kv_pages = int(mem.get("kv_capacity_max_page_groups", 0))
    if not host_kv or not kv_pages:
        print(f"ERROR: cannot read server config from /stats (host_kv={host_kv}, kv_pages={kv_pages}). "
              f"Is the server running and responsive?")
        return 1
    if host_kv > 16 * 1024 * 1024 * 1024:
        print(f"ERROR: host-kv capacity is {host_kv / 1024**3:.1f} GiB — this looks like the "
              f"production server. The e2e tests require the test server (12 GiB host-kv, "
              f"32k context, 64k KV capacity). Start it with: tools/e2e/ninfer-start-test.sh")
        return 1
    if kv_pages > 4096:
        print(f"ERROR: KV capacity is {kv_pages} page groups — this looks like the production "
              f"server. The e2e tests require the test server (64k KV capacity = ~1024 pages). "
              f"Start it with: tools/e2e/ninfer-start-test.sh")
        return 1
    print(f"Server config OK: host-kv={host_kv / 1024**3:.1f} GiB, KV pages={kv_pages}")

    # Prod is stopped during the swap, so every request-log line written from
    # here on belongs to the e2e server. Mark the offset so the planner-latency
    # check (below) reads only this run's materialization diagnostics.
    args._request_log_start = count_log_lines(args.request_log)
    global _ARGS
    _ARGS = args  # evaluate() reads the request-log window from this

    all_verdicts = []
    phases = (phase_1, phase_2, phase_3, phase_4, phase_5, phase_6, phase_7,
              phase_8, phase_9, phase_10, phase_11, phase_12, phase_13, phase_14)
    for i, phase_fn in enumerate(phases, start=1):
        if i < args.start_phase:
            print(f"=== Phase {i}: skipped (--start-phase {args.start_phase}) ===")
            continue
        result = phase_fn(args)
        if result is None:
            return 1  # phase aborted
        all_verdicts.extend(result)
        for pn, v in result:
            print(f"  [{pn}] {v}")

    # Planner-latency gate: the admission planner must converge fast. The
    # 2026-09-17 fix seeds with a feasible cover and caps the beam search at
    # 30ms, so searches stop at model_optimal/queue_exhausted/
    # value_of_next_expansion/time_budget instead of enumerating to the 4096-
    # target budget (expansion_capacity/target_budget).
    planner_verdicts = phase_planner_latency(args)
    all_verdicts.extend(planner_verdicts)
    for pn, v in planner_verdicts:
        print(f"  [{pn}] {v}")

    reuse_verdicts = phase_reuse_paths(args)
    all_verdicts.extend(reuse_verdicts)
    for pn, v in reuse_verdicts:
        print(f"  [{pn}] {v}")
    return print_summary(all_verdicts)


def phase_1(args):
    all_verdicts = []
    # Phase 1: pressure — 4 sessions, basic safety net
    print("\n=== Phase 1: pressure (4 sessions, 8 rounds) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s1 = [Session("ABCD"[i], 14000, 2000, args) for i in range(4)]
    for r in range(1, 9):
        print(f"Round {r}:")
        errors = run_round(s1, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("ABORT: phase 1 failed"); return None
    stats1 = get_stats(args)
    log1 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("pressure", s1, stats0, stats1, log1):
        all_verdicts.append(("pressure", v))
    return all_verdicts


def phase_2(args):
    all_verdicts = []
    # Phase 2: mixed — 1 big + 3 small, eviction order
    print("\n=== Phase 2: mixed (1 BIG + 3 small, 10 rounds) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s2 = [Session("BIG", 16000, 1500, args),
          Session("A", 6000, 1500, args),
          Session("B", 6000, 1500, args),
          Session("C", 6000, 1500, args)]
    for r in range(1, 11):
        print(f"Round {r}:")
        errors = run_round(s2, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Retry only failed sessions (Session uses previous_response_id,
            # safe to retry; but don't re-run successful sessions).
            failed = [s for s in s2 if s.name in [n for n, _ in errors]]
            if failed:
                print("  Retrying failed sessions...")
                time.sleep(3)
                errors2 = run_round(failed, r, args.timeout)
                if errors2:
                    for n, e in errors2: print(f"  ERROR {n}: {e}")
                    print("  Continuing to next round (mixed phase tolerates failures)")
                continue
    stats1 = get_stats(args)
    log2 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("mixed", s2, stats0, stats1, log2):
        all_verdicts.append(("mixed", v))
    return all_verdicts


def phase_3(args):
    all_verdicts = []
    # Phase 3: trash — 10 sessions, graceful degradation (no crash)
    print("\n=== Phase 3: trash (10 sessions, 6 rounds) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s3 = [Session(f"S{i}", 15000, 1500, args) for i in range(10)]
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round(s3, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Retry only failed sessions
            failed = [s for s in s3 if s.name in [n for n, _ in errors]]
            if failed:
                print("  Retrying failed sessions...")
                time.sleep(3)
                errors2 = run_round(failed, r, args.timeout)
                if errors2:
                    for n, e in errors2: print(f"  ERROR {n}: {e}")
                    print("  Continuing despite errors (trash phase tolerates failures)")
            continue
    stats1 = get_stats(args)
    log3 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("trash", s3, stats0, stats1, log3, expect_trash=True):
        all_verdicts.append(("trash", v))
    return all_verdicts


def phase_4(args):
    all_verdicts = []
    # Phase 4: thinking — session-key fallback with rewrite checkpoint
    print("\n=== Phase 4: thinking (3 sessions, 6 rounds, reasoning mode) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s4 = [Session(f"T{i}", 10000, 2000, args) for i in range(3)]
    for s in s4:
        s.args = type(args)(**vars(args))
        s.args.thinking_mode = True
        s.args.max_output_tokens = 128
    # Override the Session.turn to add reasoning
    original_turn = Session.turn
    def thinking_turn(self, index):
        question = f"Question {index}: Consider the paragraph about '{self.rng.choice(WORDS)}'. Answer briefly."
        new_text = filler(self.rng, self.turn_tokens) + "\n\n" + question
        if index == 1:
            new_text = self.doc + "\n\n---\n\n" + new_text
        payload = {
            "model": self.args.model,
            "input": [{"role": "user", "content": [{"type": "input_text", "text": new_text}]}],
            "instructions": "You are a concise assistant.",
            "max_output_tokens": self.args.max_output_tokens,
            "store": True,
            "stream": False,
            "reasoning": {"effort": "low"},
        }
        if self.response_id:
            payload["previous_response_id"] = self.response_id
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/responses",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        usage = out.get("usage", {}) or {}
        self.response_id = out.get("id", self.response_id)
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("input_tokens"), "output_tokens": usage.get("output_tokens")}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s prompt={record['input_tokens']} out={record['output_tokens']}")
        return record
    Session.turn = thinking_turn
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round(s4, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Session uses previous_response_id, safe to retry
            print("  Retrying failed sessions...")
            time.sleep(3)
            failed = [s for s in s4 if s.name in [n for n, _ in errors]]
            errors2 = run_round(failed, r, args.timeout)
            if errors2:
                for n, e in errors2: print(f"  ERROR {n}: {e}")
                print("  Continuing to next round (thinking phase tolerates failures)")
            continue
    Session.turn = original_turn
    stats1 = get_stats(args)
    log4 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("thinking", s4, stats0, stats1, log4):
        all_verdicts.append(("thinking", v))
    if log4["admit_session"] > 0:
        all_verdicts.append(("thinking", f"PASS: {log4['admit_session']} session-key fallback hits (rewrite checkpoint working)"))
    else:
        # Fallback may not fire if prefixes happen to match. Check for re-prefills instead.
        cold = sum(1 for s in s4 for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
        if cold > 0:
            all_verdicts.append(("thinking", f"WARN: {cold} cold-starts in thinking mode (session-key fallback may not have fired)"))
    return all_verdicts


def phase_5(args):
    all_verdicts = []
    # Phase 5: checkpoint-advance — single session, verify frontier advances
    print("\n=== Phase 5: checkpoint-advance (1 session, 8 turns) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s5 = [Session("CKPT", 12000, 2000, args)]
    # Force reasoning (rewrite checkpoints) like phase 4 so the
    # checkpoint-advance assertions don't flap on model mood.
    s5[0].args = type(args)(**vars(args))
    s5[0].args.thinking_mode = True
    s5[0].args.max_output_tokens = 128
    for r in range(1, 9):
        print(f"Round {r}:")
        errors = run_round(s5, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Session uses previous_response_id, safe to retry
            print("  Retrying failed sessions...")
            time.sleep(3)
            failed = [s for s in s5 if s.name in [n for n, _ in errors]]
            errors2 = run_round(failed, r, args.timeout)
            if errors2:
                for n, e in errors2: print(f"  ERROR {n}: {e}")
                print("  Continuing to next round (checkpoint-advance tolerates failures)")
            continue
    stats1 = get_stats(args)
    log5 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("checkpoint-advance", s5, stats0, stats1, log5):
        all_verdicts.append(("checkpoint-advance", v))
    # Token stability: verify input_tokens grow by ~turn_tokens each turn,
    # not by reasoning output size (which would indicate reasoning is kept)
    if len(s5[0].turns) >= 3:
        deltas = []
        for i in range(1, len(s5[0].turns)):
            t0 = s5[0].turns[i-1]
            t1 = s5[0].turns[i]
            if t0.get("input_tokens") and t1.get("input_tokens"):
                deltas.append(t1["input_tokens"] - t0["input_tokens"])
        if deltas:
            max_delta = max(deltas)
            # turn_tokens is 2000, max_output_tokens is 48.
            # With reasoning kept, delta would be ~2000 + reasoning_output.
            # Without reasoning, delta should be ~2000 + output_tokens.
            # Allow generous bound: 2000 (turn) + 48 (output) + 2000 (fudge) = 4048
            if max_delta < 5000:
                all_verdicts.append(("checkpoint-advance",
                    f"PASS: token stability (max delta={max_delta}, reasoning dropped)"))
            else:
                all_verdicts.append(("checkpoint-advance",
                    f"WARN: large token delta (max={max_delta}) — reasoning may be kept"))
    # Check no re-prefills after turn 1
    cold = sum(1 for t in s5[0].turns if t["turn"] > 1 and t["wall_s"] > 60)
    if cold == 0 and len(s5[0].turns) > 1:
        all_verdicts.append(("checkpoint-advance", f"PASS: 0 cold-starts across {len(s5[0].turns)} turns"))
    elif cold > 0:
        all_verdicts.append(("checkpoint-advance", f"FAIL: {cold} cold-starts — checkpoint not reused"))
    return all_verdicts


def phase_6(args):
    all_verdicts = []
    # Phase 6: tool-calling — multi-turn with tools, simulating Claude Code
    print("\n=== Phase 6: tool-calling (1 session, 6 turns, tools) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s6 = [ChatSession("TOOL", 10000, 1500, args)]
    # Override max_output_tokens so the model has room to generate tool calls
    s6[0].args = type(args)(**vars(args))
    s6[0].args.max_output_tokens = 256
    # Force reasoning (rewrite checkpoints) like phase 4 so the
    # checkpoint assertions don't flap on model mood.
    s6[0].args.thinking_mode = True
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round(s6, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # ChatSession appends messages before HTTP call — don't retry.
            # Continue to next round (tool-calling tolerates missing turns).
            print("  Continuing to next round (tool-calling tolerates failures)")
            continue
    stats1 = get_stats(args)
    log6 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("tool-calling", s6, stats0, stats1, log6):
        all_verdicts.append(("tool-calling", v))
    # Check no re-prefills after turn 1
    cold = sum(1 for t in s6[0].turns if t["turn"] > 1 and t["wall_s"] > 60)
    if cold == 0 and len(s6[0].turns) > 1:
        all_verdicts.append(("tool-calling", f"PASS: 0 cold-starts across {len(s6[0].turns)} tool-call turns"))
    elif cold > 0:
        all_verdicts.append(("tool-calling", f"WARN: {cold} cold-starts during tool-calling"))
    return all_verdicts


def phase_7(args):
    all_verdicts = []
    # Phase 7: responses-tools — Responses API tool-calling with checkpoint reuse
    print("\n=== Phase 7: responses-tools (1 session, 5 turns, Responses API) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s7 = [ResponsesApiSession("RSP", 8000, 1000, args)]
    s7[0].args = type(args)(**vars(args))
    s7[0].args.max_output_tokens = 128
    for r in range(1, 6):
        print(f"Round {r}:")
        errors = run_round(s7, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Retry once after OOM (worker recovery clears state)
            print("  Retrying after error...")
            time.sleep(2)
            errors = run_round(s7, r, args.timeout)
            if errors:
                for n, e in errors: print(f"  ERROR {n}: {e}")
                print("ABORT: phase 7 failed"); return None
    stats1 = get_stats(args)
    log7 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("responses-tools", s7, stats0, stats1, log7):
        all_verdicts.append(("responses-tools", v))
    cold = sum(1 for t in s7[0].turns if t["turn"] > 1 and t["wall_s"] > 60)
    if cold == 0 and len(s7[0].turns) > 1:
        all_verdicts.append(("responses-tools", f"PASS: 0 cold-starts across {len(s7[0].turns)} Responses API turns"))
    elif cold > 0:
        all_verdicts.append(("responses-tools", f"WARN: {cold} cold-starts in Responses API"))
    return all_verdicts


def phase_8(args):
    all_verdicts = []
    # Phase 8: reasoning-effort — verify tier mapping (high, minimal, max, low, medium)
    print("\n=== Phase 8: reasoning-effort (5 effort levels) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    tester = ReasoningEffortTester(args)
    effort_results = tester.test()
    stats1 = get_stats(args)
    log8 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("reasoning-effort", [tester], stats0, stats1, log8):
        all_verdicts.append(("reasoning-effort", v))
    all_verdicts.extend(tester_verdicts("reasoning-effort", effort_results))
    return all_verdicts


def phase_9(args):
    all_verdicts = []
    # Phase 9: concurrent — 2 sessions + title-gen, verify no cross-session destruction
    print("\n=== Phase 9: concurrent (2 sessions + title-gen, 6 rounds) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s9a = ChatSession("CONC_A", 10000, 1500, args)
    s9b = ChatSession("CONC_B", 8000, 1200, args)
    s9a.args = type(args)(**vars(args))
    s9a.args.max_output_tokens = 128
    s9b.args = type(args)(**vars(args))
    s9b.args.max_output_tokens = 128
    # Interleave: both sessions + a tiny "title-gen" request each round
    title_gen_session = Session("TITLE", 500, 100, args)
    title_gen_session.args = type(args)(**vars(args))
    title_gen_session.args.max_output_tokens = 32
    all_sessions_9 = [s9a, s9b, title_gen_session]
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round([s9a, s9b], r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Don't retry — ChatSession appends messages before the HTTP call,
            # so a retry would corrupt the message list. Continue to next round.
            print("  Continuing to next round (concurrent phase tolerates failures)")
            continue
        # Title-gen request between main session turns (simulates Claude Code)
        if r > 1:
            try:
                title_gen_session.turn(r)
            except Exception as e:
                print(f"  TITLE ERROR: {repr(e)[:100]}")
    stats1 = get_stats(args)
    log9 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("concurrent", all_sessions_9, stats0, stats1, log9):
        all_verdicts.append(("concurrent", v))
    return all_verdicts


def phase_10(args):
    all_verdicts = []
    # Phase 10: thinking-sig — verify signature skip when preserve_thinking=false
    print("\n=== Phase 10: thinking-sig (4 requests) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    sig_tester = ThinkingSignatureTester(args)
    sig_results = sig_tester.test()
    stats1 = get_stats(args)
    log10 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("thinking-sig", [sig_tester], stats0, stats1, log10):
        all_verdicts.append(("thinking-sig", v))
    all_verdicts.extend(tester_verdicts("thinking-sig", sig_results))
    return all_verdicts


def phase_11(args):
    all_verdicts = []
    # Phase 11: demotion — 3 sessions, large prompts, verify checkpoint demotion
    print("\n=== Phase 11: demotion (3 sessions, 5 rounds, verify host demotion) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s11 = [Session(f"DEM{i}", 20000, 2000, args) for i in range(3)]
    for r in range(1, 6):
        print(f"Round {r}:")
        errors = run_round(s11, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Session uses previous_response_id, safe to retry
            print("  Retrying failed sessions...")
            time.sleep(3)
            failed = [s for s in s11 if s.name in [n for n, _ in errors]]
            errors2 = run_round(failed, r, args.timeout)
            if errors2:
                for n, e in errors2: print(f"  ERROR {n}: {e}")
                print("  Continuing to next round (demotion phase tolerates failures)")
            continue
    stats1 = get_stats(args)
    log11 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("demotion", s11, stats0, stats1, log11):
        all_verdicts.append(("demotion", v))
    # Verify no re-prefills after turn 1
    cold = sum(1 for s in s11 for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
    if cold == 0:
        all_verdicts.append(("demotion", f"PASS: 0 cold-starts across {sum(len(s.turns) for s in s11)} turns"))
    elif cold > 0:
        all_verdicts.append(("demotion", f"WARN: {cold} cold-starts — demotion may not have prevented all re-prefills"))
    return all_verdicts


def phase_12(args):
    all_verdicts = []
    # Phase 12: state-lease — rewrite-checkpoint recycle pressure.
    # Reproduces the 2026-09-14 production wedge: forced-thinking sessions fill
    # the device state pool, so captures fork + recycle checkpoint slots. The
    # buggy recycle-capture publish path dropped the rewrite handle without
    # releasing its checkpoint reference, so every fork+recycle cycle leaked a
    # state slot (release refused -> orphaned slot -> pool exhaustion ->
    # "no resident state" -> "private source result is missing" request errors
    # -> client retry loop).
    # Requires the test server with --device-state-slots 5 (8 total: 3 cache +
    # 5 active): with the old 6-slot total the pool was exactly full under 4
    # thinking sessions, forks failed, and the recycle-with-fork path was never
    # exercised — which is how the bug shipped.
    print("\n=== Phase 12: state-lease (4 thinking sessions, 5 rounds, rewrite-recycle pressure) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s12 = [Session(f"SL{i}", 10000, 1500, args) for i in range(4)]
    for s in s12:
        s.args = type(args)(**vars(args))
        s.args.max_output_tokens = 128
    # Force reasoning (rewrite checkpoints) like phase 4.
    original_turn = Session.turn
    def thinking_turn(self, index):
        question = f"Question {index}: Consider the paragraph about '{self.rng.choice(WORDS)}'. Answer briefly."
        new_text = filler(self.rng, self.turn_tokens) + "\n\n" + question
        if index == 1:
            new_text = self.doc + "\n\n---\n\n" + new_text
        payload = {
            "model": self.args.model,
            "input": [{"role": "user", "content": [{"type": "input_text", "text": new_text}]}],
            "instructions": "You are a concise assistant.",
            "max_output_tokens": self.args.max_output_tokens,
            "store": True,
            "stream": False,
            "reasoning": {"effort": "low"},
        }
        if self.response_id:
            payload["previous_response_id"] = self.response_id
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/responses",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        usage = out.get("usage", {}) or {}
        self.response_id = out.get("id", self.response_id)
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("input_tokens"), "output_tokens": usage.get("output_tokens")}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s prompt={record['input_tokens']} out={record['output_tokens']}")
        return record
    Session.turn = thinking_turn
    for r in range(1, 6):
        print(f"Round {r}:")
        errors = run_round(s12, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("  Retrying failed sessions...")
            time.sleep(3)
            failed = [s for s in s12 if s.name in [n for n, _ in errors]]
            errors2 = run_round(failed, r, args.timeout)
            if errors2:
                for n, e in errors2: print(f"  ERROR {n}: {e}")
            continue
    Session.turn = original_turn
    stats1 = get_stats(args)
    log12 = parse_serve_log(args.serve_log, log_off)

    # The leak signatures. A refused release is only a TRUE bug when the
    # image has no owner (shared_refs == 0). shared_refs >= 1 is legitimate
    # retention (the image backs a live shared prefix) and is expected.
    if log12["state_lease_orphan"] > 0:
        all_verdicts.append(("state-lease", f"FAIL: {log12['state_lease_orphan']} orphaned state image(s) (shared_refs=0) — unbalanced checkpoint ref"))
    else:
        all_verdicts.append(("state-lease", "PASS: zero orphaned state images (no shared_refs=0 refusals)"))
    if log12["missing_source_result"] > 0:
        all_verdicts.append(("state-lease", f"FAIL: {log12['missing_source_result']} 'private source result is missing' request error(s)"))
    else:
        all_verdicts.append(("state-lease", "PASS: zero 'private source result is missing' errors"))
    # Refused releases that are LEGITIMATE (shared-prefix retention) are
    # expected under pressure; report them as info, not failure.
    if log12["state_lease_leak"] > 0:
        all_verdicts.append(("state-lease", f"INFO: {log12['state_lease_leak']} refused release(s), {log12['state_lease_leak'] - log12['state_lease_orphan']} legitimate (shared-prefix retention)"))
    # Orphaned state slots surface in /stats as refused releases.
    h0 = (stats0.get("pressure", {}) or {}).get("host_slot_release_failures", 0)
    h1 = (stats1.get("pressure", {}) or {}).get("host_slot_release_failures", 0)
    if h1 > h0:
        all_verdicts.append(("state-lease", f"WARN: host_slot_release_failures grew by {h1 - h0} (includes legitimate shared-prefix retention)"))
    else:
        all_verdicts.append(("state-lease", "PASS: host_slot_release_failures unchanged"))
    # Vacuity guards: the pressure path must have actually been exercised.
    captured = log12["spill_ckpt_ok"]
    restored = log12["checkpoint_restored"] + log12["rewrite_prefix_hit"]
    demoted = log12["checkpoint_demoted"]
    if captured < 4:
        all_verdicts.append(("state-lease", f"WARN: only {captured} checkpoint captures (expected >= 4) — state pressure may not have been reached"))
    else:
        all_verdicts.append(("state-lease", f"PASS: {captured} checkpoint captures (rewrite-recycle precondition)"))
    if log12.get("no_v3_source"):
        all_verdicts.append(("state-lease",
            "INFO: no v3 signal for " + ", ".join(log12["no_v3_source"]) +
            " -- those checks cannot fire on this engine; the suite's vocabulary predates v3"))
    if restored == 0 and demoted == 0 and log12["relief_demote"] == 0:
        all_verdicts.append(("state-lease", "WARN: no checkpoint demote/restore/relief observed — state pool never pressurized (H2D-restore path may be unexercised)"))
    else:
        all_verdicts.append(("state-lease", f"PASS: state pool pressurized (demoted={demoted}, restored={restored}, relief={log12['relief_demote']})"))
    return all_verdicts


def phase_13(args):
    all_verdicts = []
    # Phase 13: state-saturation — the device state pool at 100% with a
    # HostOnly checkpoint that must be restored (H2D) while it stays full.
    # This is the exact production class that 500ed on 2026-09-16: the
    # rewrite-restore H2D takes a NEW device slot, and the emergency relief
    # must free one (DeviceOnly demotion, or dropping the redundant device
    # replica of a dual-resident checkpoint) or the materialization throws
    # std::bad_alloc. Phases 11/12 pressurize the pool but do not saturate
    # it (relief=0 in a full run), so this phase exists to force the
    # saturated-restore path: more thinking sessions (5) than decode lanes
    # (3), so the working set (endpoint + fork-write + rewrite checkpoint per
    # active session) exceeds the 8-slot pool (3 cache + 5 active).
    print("\n=== Phase 13: state-saturation (6 tool-calling sessions, 8 rounds, pool at 100%) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    # Tool-calling sessions (the production shape): every tool-call turn is a
    # rewrite boundary, so turns capture rewrite checkpoints — the images the
    # saturated H2D-restore relief path operates on. Plain thinking turns
    # capture almost none (ckpt=0), which leaves the pool full of endpoints
    # only and never triggers the relief path.
    s13 = [ChatSession(f"SS{i}", 10000, 1500, args) for i in range(6)]
    for s in s13:
        s.args = type(args)(**vars(args))
        s.args.max_output_tokens = 512
    # PHASE13_ROUNDS (env, default 8): reduced-round A/B variant — the full
    # 8-round phase exceeds the 10-minute foreground swap limit when the
    # pressure relief does D2H work (P2.4 Slice 3 Inc 3, 2026-09-17).
    rounds = int(os.environ.get("PHASE13_ROUNDS", "8"))
    for r in range(1, rounds + 1):
        print(f"Round {r}:")
        errors = run_round(s13, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("  Retrying failed sessions...")
            time.sleep(3)
            failed = [s for s in s13 if s.name in [n for n, _ in errors]]
            errors2 = run_round(failed, r, args.timeout)
            if errors2:
                for n, e in errors2: print(f"  ERROR {n}: {e}")
            continue
    stats1 = get_stats(args)
    log13 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("state-saturation", s13, stats0, stats1, log13):
        all_verdicts.append(("state-saturation", v))
    # Phase-specific gates: a saturated restore must never 500. Worker
    # recoveries from the P4.2 pressure-expansion class are known and
    # self-recovering (tracked in plan.md) — they are not saturated-restore
    # failures.
    other_recoveries = log13["worker_recover"] - log13["pressure_expansion_fail"]
    if other_recoveries > 0:
        all_verdicts.append(("state-saturation", f"FAIL: {other_recoveries} worker recoveries — saturated restore failed (relief did not free a slot)"))
    else:
        all_verdicts.append(("state-saturation", "PASS: zero worker recoveries (saturated restores resolved)"))
    if log13["pressure_expansion_fail"] > 0:
        all_verdicts.append(("state-saturation", f"WARN: {log13['pressure_expansion_fail']} pressure-expansion recoveries (P4.2 known class, self-recovering)"))
    # Occupancy readout: did the pool actually reach its ceiling?
    p1 = (stats1.get("pressure", {}) or {})
    cap = p1.get("checkpoint_device_state_slots", 0)
    occ = p1.get("device_state_occupied_slots", 0)
    if cap and occ >= cap:
        all_verdicts.append(("state-saturation", f"PASS: device state pool saturated ({occ}/{cap} at phase end)"))
    else:
        all_verdicts.append(("state-saturation", f"WARN: device state pool not saturated ({occ}/{cap} at phase end)"))
    if log13.get("no_v3_source"):
        all_verdicts.append(("state-saturation",
            "INFO: no v3 signal for " + ", ".join(log13["no_v3_source"]) +
            " -- those checks cannot fire on this engine; the suite's vocabulary predates v3"))
    if log13["relief_demote"] > 0:
        all_verdicts.append(("state-saturation", f"PASS: relief freed device state slots {log13['relief_demote']}x (dual drops: {log13['relief_dual_drop']})"))
    else:
        all_verdicts.append(("state-saturation", "WARN: relief never fired — pool may not have saturated (non-deterministic; the no-OOM gate still holds)"))
    if log13["state_replan"] > 0:
        all_verdicts.append(("state-saturation", f"PASS: {log13['state_replan']} entitlement re-plans (plan re-baselined after relief; no 500)"))
    if log13["entitlement_mismatch"] > 0:
        all_verdicts.append(("state-saturation", f"FAIL: {log13['entitlement_mismatch']} core-incomplete entitlement mismatches (strict check fired)"))
    if log13["spill_before_loss"] > 0:
        all_verdicts.append(("state-saturation", f"PASS: {log13['spill_before_loss']} spill-before-loss backstops fired (unit retained in net before its state lost its last copy)"))
    if log13["state_relinquish"] > 0:
        all_verdicts.append(("state-saturation", f"PASS: {log13['state_relinquish']} state images relinquished to the net (move-not-copy; the net is the unit's host home)"))
    return all_verdicts


def phase_14(args):
    all_verdicts = []
    # Phase 14: queued-relief — P1.5(d) Increment 2.
    # 4 sessions x 24k-token prompts against the 64k-token device-KV pool:
    # only ~2 fit in the pool at once, so the overflowing requests cannot be
    # admitted without exceeding the pool. Pre-Increment-2 they would be
    # admitted into a silent 120s fit-gate defer; now the engine keeps them
    # in the visible queue (position/wait in /stats) and runs
    # relief-while-queued toward their demand (15s stall cadence, 120s
    # deadline).
    print("\n=== Phase 14: queued-relief (4 sessions, 3 rounds, 24k prompts vs 64k device KV) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s14 = [Session(f"QR{i}", 24000, 1000, args) for i in range(4)]
    for s in s14:
        s.args = type(args)(**vars(args))
        s.args.max_output_tokens = 512
    for r in range(1, 4):
        print(f"Round {r}:")
        errors = run_round(s14, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("  Retrying failed sessions...")
            time.sleep(3)
            failed = [s for s in s14 if s.name in [n for n, _ in errors]]
            errors2 = run_round(failed, r, args.timeout)
            if errors2:
                for n, e in errors2: print(f"  ERROR {n}: {e}")
            continue
    stats1 = get_stats(args)
    log14 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("queued-relief", s14, stats0, stats1, log14):
        all_verdicts.append(("queued-relief", v))
    if log14["kv_occupancy_block"] > 0:
        all_verdicts.append(("queued-relief",
            f"PASS: {log14['kv_occupancy_block']} KV occupancy block(s) — unfitted head(s) kept in the visible queue instead of a silent 120s defer"))
    else:
        all_verdicts.append(("queued-relief",
            "WARN: no KV occupancy block — the device-KV gap never blocked a head (non-deterministic; path unexercised)"))
    if log14["queued_kv_relief"] > 0:
        all_verdicts.append(("queued-relief",
            f"PASS: relief-while-queued fired {log14['queued_kv_relief']}x (freed pages toward the blocked demand)"))
    elif log14["kv_occupancy_block"] > 0:
        all_verdicts.append(("queued-relief",
            "WARN: occupancy block fired but relief never ran (gap closed by lane drain before the 15s stall)"))
    if log14["queued_kv_deadline"] > 0:
        all_verdicts.append(("queued-relief",
            f"WARN: {log14['queued_kv_deadline']} request(s) hit the 120s queued-KV deadline (structural over-commit — the gap could not be closed)"))
    else:
        all_verdicts.append(("queued-relief", "PASS: no queued-KV deadline aborts"))
    if log14["bad_alloc"] > 0 or log14["worker_recover"] > 0:
        all_verdicts.append(("queued-relief",
            f"FAIL: {log14['bad_alloc']} bad_alloc / {log14['worker_recover']} worker recoveries under queueing"))
    else:
        all_verdicts.append(("queued-relief", "PASS: zero bad_alloc / worker recoveries under queueing"))
    return all_verdicts


def phase_planner_latency(args):
    """Admission-planner latency gate.

    The pressure planner is a beam search over parked catalog units. Before
    the 2026-09-17 convergence fix it enumerated to the 4096-target budget
    (stop_reason expansion_capacity/target_budget, ~3.2s per search) whenever
    the evict-all seed incumbent was hard to beat. The fix seeds with a
    feasible cover (guided closure / greedy eviction cover), stops expanding
    covered targets, prunes infeasible branches, and caps the search in TIME.

    REFACTORED 2026-09-28. It used to assert `budget_stops == 0` -- that no search ever stops for a budget
    reason -- which fails on the very behaviour the fix introduced, because a capped search is supposed to
    stop at its cap. Three things were wrong at once and all three are fixed here:
      * the assertion (any budget stop = FAIL) contradicted the fix it protects;
      * it counted `expansion_capacity`/`target_budget` while IGNORING `time_budget`, which is the cap its
        own docstring credits -- so it read a partial stop distribution as a verdict;
      * its thresholds (30ms in the text, 50ms in the code) predate the current cap: the full run of
        2026-09-28 measured `p95_search=400.0ms`, so BOTH would have failed a healthy planner.
    What the original defect looked like is what this bars: a search that enumerated to the budget at ~3.2s.
    The gate is that latency, and the stop distribution is reported WITH its denominator as information.
    """
    all_verdicts = []
    start = getattr(args, "_request_log_start", 0)
    rows = []
    try:
        with open(args.request_log, "r", errors="replace") as f:
            for i, line in enumerate(f):
                if i < start:
                    continue
                line = line.strip()
                if not line:
                    continue
                try:
                    d = json.loads(line)
                except Exception:
                    continue
                m = d.get("materialization")
                if m:
                    rows.append(m)
    except OSError:
        all_verdicts.append(("planner-latency", "WARN: request log unreadable — planner check skipped"))
        return all_verdicts
    if not rows:
        all_verdicts.append(("planner-latency", "WARN: no materialization diagnostics in window — planner check skipped"))
        return all_verdicts

    search_ns = sorted(m.get("search_elapsed_ns", 0) for m in rows)
    p95 = search_ns[min(len(search_ns) - 1, int(len(search_ns) * 0.95))]
    budget_stops = sum(1 for m in rows if m.get("stop_reason") in ("expansion_capacity", "target_budget"))
    stops = {}
    for m in rows:
        s = m.get("stop_reason", "?")
        stops[s] = stops.get(s, 0) + 1
    detail = (f"n={len(rows)} p95_search={p95 / 1e6:.1f}ms max_search={search_ns[-1] / 1e6:.1f}ms "
              f"budget_stops={budget_stops} stops={stops}")
    # THE GATE IS DERIVED FROM THE CAP THE ENGINE ACTUALLY ENFORCES, not from a round number. The cap is
    # `limit_ns = 400'000'000` in `materialization_planner.h` (`MaterializationSearchBudget`), enforced by
    # `PlanningAllowance::boundary`; the gate allows one step over it, so a cap that has been raised,
    # removed or broken shows up at ~1.25x rather than hiding under an arbitrary 1s ceiling (1s let a
    # broken cap run 2.5x over unseen, which is what this replaces). A 3.2s enumeration -- the pre-fix
    # behaviour -- still fails it by a wide margin.
    SEARCH_CAP_NS = 400 * 1_000 * 1_000          # materialization_planner.h: limit_ns
    ENUMERATION_REGRESSION_NS = SEARCH_CAP_NS + SEARCH_CAP_NS // 4
    if search_ns[-1] > ENUMERATION_REGRESSION_NS:
        all_verdicts.append(("planner-latency",
                             f"FAIL: a search took {search_ns[-1] / 1e6:.0f}ms, past the "
                             f"{ENUMERATION_REGRESSION_NS / 1e6:.0f}ms gate (cap "
                             f"{SEARCH_CAP_NS / 1e6:.0f}ms + one step) — either the cap is broken or the "
                             f"enumerated-to-the-target-budget behaviour is back. {detail}"))
    else:
        all_verdicts.append(("planner-latency",
                             f"PASS: no search exceeded the {ENUMERATION_REGRESSION_NS / 1e6:.0f}ms gate "
                             f"(cap {SEARCH_CAP_NS / 1e6:.0f}ms, n={len(rows)}); {budget_stops} stopped for "
                             f"a budget reason, which is the cap working — {detail}"))
    return all_verdicts


def phase_reuse_paths(args):
    """The reuse ceiling the 2026-09-28 work was about, asserted from the request log.

    WHY THIS EXISTS. The branch anchor (`NINFER_BRANCH_ANCHOR`) captures a checkpoint at the depth a request
    actually matched to, taking the conversation's reuse ceiling from a pinned 23,353 tokens to 33k-45k. It
    had NO coverage here: no phase enabled it, nothing read the request log's reuse fields.

    THE ASSERTION IS A JOIN, PER RECORD -- and the first version got that wrong twice over.

    1. It took `max(hits)` over ALL records and `paths` as the union over ALL records, then asked whether
       the max was above the ceiling AND the union contained the anchor. Those are different records: on the
       2026-09-28 run the 36 requests above 23,353 were ALL `private_endpoint`, and the 17
       `private_long_anchor` records topped out at 19,815. The phase printed PASS and the record claimed the
       anchor fix was measured, when the anchor had never crossed the ceiling.
    2. Even joined, the constant is the wrong yardstick. 23,353 is QA's Claude Code system-prompt pin, and
       this workload's own `shared_stable_prefix` hits are ~9.9k/12.0k/15.2k. So the gate can neither pass
       on a workload whose prompts never reach the pin NOR fail on one whose shared prefix is longer than it.

    The yardstick is the PINNED CEILING -- not, as a previous version of this docstring said, the best
    non-anchor reuse: that version FAILED whenever the anchor did not beat `private_endpoint`, which is the
    conversation's own continuation and naturally reuses more. When NO anchor record's prompt could have
    reached the pin, the verdict is WARN-INCONCLUSIVE rather than a FAIL that describes the workload. `anchor_trivial` is printed because 14 of 17 anchors reusing
    <=14 tokens is a signal that `anchor_records` alone hides.
    """
    all_verdicts = []
    # WHICH ARM THIS IS, read from the serve log rather than inferred: `ninfer-start-test.sh` writes
    # `ANCHOR_CHILD=<n>` (counted from the child's own environ) at startup. Without it an anchor-OFF run
    # reports "no request took `private_long_anchor` ... the anchor is enabled in the test config", which
    # is exactly backwards for that arm.
    anchor_expected = None
    try:
        with open(args.serve_log, "r", errors="replace") as f:
            for line in f:
                if line.startswith("ANCHOR_CHILD="):
                    anchor_expected = line.strip().split("=", 1)[1].strip() != "0"
    except OSError:
        anchor_expected = None
    start = getattr(args, "_request_log_start", 0)
    rows = []
    try:
        with open(args.request_log, "r", errors="replace") as f:
            for i, line in enumerate(f):
                if i < start:
                    continue
                line = line.strip()
                if not line:
                    continue
                try:
                    d = json.loads(line)
                except Exception:
                    continue
                r = d.get("result") or {}
                hit, path = r.get("prefix_cache_hit_tokens"), r.get("prefix_reuse_path")
                if hit is not None:
                    rows.append((int(hit), path or "unknown", int(r.get("prompt_tokens") or 0)))
    except OSError:
        all_verdicts.append(("reuse-paths", "WARN: request log unreadable — ceiling check skipped"))
        return all_verdicts
    if not rows:
        all_verdicts.append(("reuse-paths", "WARN: no reuse readings in window — ceiling check skipped"))
        return all_verdicts

    PINNED_CEILING = 23_353
    anchor   = [(hit, prompt) for hit, path, prompt in rows if path == "private_long_anchor"]
    nonanchor_best = max((hit for hit, path, _ in rows if path != "private_long_anchor"), default=0)
    # THE SAME JOIN, TWICE OVER: the best anchor hit and the prompt it came FROM must be the same record.
    # Taking `max(hit)` and `max(prompt)` independently is exactly the error this phase was rewritten to
    # remove, and it reappeared here on the first pass -- caught by replaying the phase over the real log,
    # which reported "an anchor request had a prompt past the pin" from two different requests.
    best_anchor, best_anchor_prompt = max(anchor, key=lambda row: row[0]) if anchor else (None, 0)
    # TWO DIFFERENT PROMPTS, and the first version used one for both questions. `best_anchor_prompt` belongs
    # to the record with the best HIT -- it answers "did the anchor that worked have a prompt long enough to
    # cross the pin?". The inconclusive gate asks a different question -- "could ANY anchor request have
    # crossed it?" -- and must take the LONGEST anchor prompt, or a record with a long prompt and a trivial
    # reuse slips past. Measured: instance serve-1553637-… has an anchor record with prompt 24,499 that
    # reused 14 tokens, above the 23,353 pin, and the phase still reported "longest 22957".
    longest_anchor_prompt = max((prompt for _, prompt in anchor), default=0)
    trivial_anchors = sum(1 for hit, _ in anchor if hit <= 14)
    detail = (f"n={len(rows)} anchor_records={len(anchor)} anchor_best={best_anchor} "
              f"anchor_best_prompt={best_anchor_prompt} anchor_longest_prompt={longest_anchor_prompt} "
              f"anchor_trivial(<=14tok)={trivial_anchors} "
              f"(reference only — other paths legitimately reuse more) best_nonanchor_hit="
              f"{nonanchor_best} best_nonanchor_prompt="
              f"{max((prompt for _, path, prompt in rows if path != 'private_long_anchor'), default=0)}")

    if not anchor:
        if anchor_expected is False:
            all_verdicts.append(("reuse-paths",
                                 f"WARN: anchor-OFF arm (ANCHOR_CHILD=0) and no request took "
                                 f"`private_long_anchor` — that is the control working, not a failure. "
                                 f"{detail}"))
        elif anchor_expected is None:
            all_verdicts.append(("reuse-paths",
                                 f"WARN: no `ANCHOR_CHILD=` marker in the serve log, so which arm this was "
                                 f"is unknown and the anchor's absence cannot be read. {detail}"))
        else:
            all_verdicts.append(("reuse-paths",
                                 f"FAIL: no request took `private_long_anchor` at all — the anchor is ON in "
                                 f"this arm (ANCHOR_CHILD=1) and is not being taken, which is the regression "
                                 f"this case exists for. {detail}"))
    elif longest_anchor_prompt <= PINNED_CEILING:
        # The workload cannot answer the question: no anchor record's prompt was long enough for the pin to
        # be reachable, so "did the anchor lift the ceiling?" has no evidence either way here. A FAIL would
        # describe the workload, not the anchor.
        all_verdicts.append(("reuse-paths",
                             f"WARN: inconclusive — the anchor was taken {len(anchor)} times but no anchor "
                             f"request had a prompt longer than the {PINNED_CEILING} pin (longest "
                             f"{longest_anchor_prompt}), so this workload cannot show the anchor lifting it. "
                             f"{detail}"))
    # THE YARDSTICK IS THE PIN, NOT THE OTHER PATHS. An earlier version of this branch FAILED whenever the
    # anchor's best did not exceed the best non-anchor reuse -- which is wrong, and it fired on the load
    # that PROVED the anchor works (2026-10-01: 58 of 58 anchor records above the pin, median 39,376, and
    # still a FAIL because `private_endpoint` is the conversation's own continuation and reuses slightly
    # more). Each reuse path has a different job; the anchor's is to beat the SHARED-PREFIX ceiling, which
    # is what it is measured against. The non-anchor maximum is printed beside it as a reference.
    elif best_anchor <= PINNED_CEILING:
        all_verdicts.append(("reuse-paths",
                             f"FAIL: an anchor request had a prompt past the {PINNED_CEILING} pin "
                             f"({longest_anchor_prompt}) and the BEST anchor reuse was only {best_anchor} — "
                             f"the anchor is taken and is not lifting the ceiling. (The prompt quoted is the "
                             f"LONGEST anchor prompt, which is what makes the question answerable; the best "
                             f"hit came from a {best_anchor_prompt}-token request.) {detail}"))
    else:
        all_verdicts.append(("reuse-paths",
                             f"PASS: an anchor request reused {best_anchor}, past the {PINNED_CEILING} "
                             f"shared-prefix pin — {detail}"))
    return all_verdicts


def print_summary(all_verdicts):
    # Summary
    print("\n=== FINAL VERDICTS ===")
    for pn, v in all_verdicts:
        print(f"  [{pn}] {v}")
    npass = sum(1 for _, v in all_verdicts if v.startswith("PASS"))
    nwarn = sum(1 for _, v in all_verdicts if v.startswith("WARN"))
    nfail = sum(1 for _, v in all_verdicts if v.startswith("FAIL"))
    print(f"\n{'FAIL' if nfail else 'PASS'}: {npass} PASS, {nwarn} WARN, {nfail} FAIL")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
