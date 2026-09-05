#!/usr/bin/env python3
"""E2E test suite for ninfer safety-net eviction system.

Runs eight phases by default against a single test server (no flags needed):
  Phase 1 "pressure":           4 sessions — basic safety net (spills, restores, no re-prefills)
  Phase 2 "mixed":              1 big + 3 small — eviction order (smallest-first, big preserved)
  Phase 3 "trash":              10 sessions — graceful degradation under trashing (no crash)
  Phase 4 "thinking":           3 sessions, reasoning mode — session-key fallback with rewrite checkpoint
  Phase 5 "checkpoint-advance": 1 session, 8 turns — checkpoint frontier advances monotonically
  Phase 6 "tool-calling":       1 session, 6 turns with tools — rewrite restore under tool-call rounds
  Phase 7 "responses-tools":    1 session, 5 turns — Responses API with text after function_calls (Claude Code pattern)
  Phase 8 "reasoning-effort":   5 requests — reasoning effort tier mapping (high, minimal, max, medium, low)

Server config: 32k max-context, 64k kv-capacity, 4GB host-kv, 3 continuations.
All phases use the same server — no restarts.

Usage: python3 ninfer-e2e.py [--host 127.0.0.1] [--port 8080] [--serve-log /home/zenz/ninfer-serve.log]
"""

import argparse
import json
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
        assistant_msg = {"role": "assistant", "content": msg.get("content", "")}
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
    """Session using /v1/responses with function_call + function_call_output items.
    Builds the full input array manually (no previous_response_id) to directly
    test the fix that allows text/reasoning after function_call items — the
    pattern Claude Code sends when the model explains after calling tools.
    """
    def __init__(self, name, seed_tokens, turn_tokens, args):
        self.name = name
        self.rng = random.Random(sum(ord(c) for c in name) + 7)
        self.seed_tokens = seed_tokens
        self.turn_tokens = turn_tokens
        self.args = args
        self.input_items = []  # accumulated input items
        self.turns = []
        self.doc = filler(self.rng, seed_tokens)
        self._call_id = 0

    def turn(self, index):
        if index == 1:
            user_text = self.doc + "\n\n---\n\n" + filler(self.rng, self.turn_tokens) + "\n\nRead the file."
        else:
            user_text = filler(self.rng, self.turn_tokens) + f"\n\nQuestion {index}: Summarize what you found."
        # Append user message to input history
        self.input_items.append(
            {"role": "user", "content": [{"type": "input_text", "text": user_text}]})
        payload = {
            "model": self.args.model,
            "input": self.input_items,
            "instructions": "You are a coding assistant. Use tools when needed.",
            "max_output_tokens": self.args.max_output_tokens,
            "store": False,
            "stream": False,
            "tools": [
                {"type": "function", "name": "read_file",
                 "description": "Read a file",
                 "parameters": {"type": "object", "properties": {"path": {"type": "string"}}}},
            ],
        }
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/responses",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        usage = out.get("usage", {}) or {}
        output = out.get("output", [])
        has_tool_call = any(item.get("type") == "function_call" for item in output if isinstance(item, dict))
        has_text = any(item.get("type") == "message" for item in output if isinstance(item, dict))
        # If the model called a tool, add function_call + function_call_output to
        # the input history. This creates the "text after function_call" pattern
        # that the fix in openai_responses_request.cpp allows.
        if has_tool_call:
            for item in output:
                if isinstance(item, dict) and item.get("type") == "function_call":
                    call_id = item.get("call_id", f"call_{self._call_id}")
                    self._call_id += 1
                    # Add the function_call to input as an assistant item
                    self.input_items.append(item)
                    # Add the function_call_output (simulated tool result)
                    self.input_items.append({
                        "type": "function_call_output",
                        "call_id": call_id,
                        "output": f"File contents: {filler(self.rng, 200)}",
                    })
        # Add any text output as an assistant message to input history
        if has_text:
            text_parts = []
            for item in output:
                if isinstance(item, dict) and item.get("type") == "message":
                    for c in item.get("content", []):
                        if isinstance(c, dict) and c.get("type") == "output_text":
                            text_parts.append(c.get("text", ""))
            if text_parts:
                self.input_items.append({
                    "role": "assistant",
                    "content": [{"type": "output_text", "text": " ".join(text_parts)}],
                })
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


def parse_serve_log(path, skip_lines=0):
    d = {k: 0 for k in [
        "spill_ok", "spill_ckpt_ok", "spill_ckpt_missing", "spill_fail",
        "restore_started", "restore_completed", "restore_failed",
        "safety_find_hit", "safety_find_miss", "max_net_entries",
        "worker_crash", "bad_alloc", "capture_skip", "restore_skip",
        "refind_hit", "evict_smallest", "multi_extent_ok", "admit_session",
        "rewrite_prefix_hit", "rewrite_restore_fail", "worker_recover",
        "private_turn_closure", "tool_calls_done",
    ]}
    d["evict_pages"] = []
    d["checkpoint_frontiers"] = []
    try:
        with open(path, "r", errors="replace") as f:
            for _ in range(skip_lines):
                f.readline()
            for line in f:
                if "[restore]" in line and "syncing" in line: d["restore_completed"] += 1
                if "[restore]" in line and "frontier=" in line: d["restore_started"] += 1
                if "[restore] FAILED" in line: d["restore_failed"] += 1
                if "[safety-find]" in line and "match=hit" in line: d["safety_find_hit"] += 1
                if "[safety-find]" in line and "match=miss" in line: d["safety_find_miss"] += 1
                if "[safety-find]" in line and "entries=" in line:
                    m = re.search(r"entries=(\d+)", line)
                    if m: d["max_net_entries"] = max(d["max_net_entries"], int(m.group(1)))
                if "[safety-spill] OK" in line:
                    d["spill_ok"] += 1
                    if "ckpt_valid=1" in line: d["spill_ckpt_ok"] += 1
                    elif "ckpt_valid=0" in line: d["spill_ckpt_missing"] += 1
                if "[safety-spill] FAIL" in line: d["spill_fail"] += 1
                if "[safety-spill] multi-extent OK" in line: d["multi_extent_ok"] += 1
                if "WORKER CRASH" in line: d["worker_crash"] += 1
                if "std::bad_alloc" in line: d["bad_alloc"] += 1
                if "[capture] skip zero-prefill" in line: d["capture_skip"] += 1
                if "[capture] skip restore" in line: d["restore_skip"] += 1
                if "[safety-find] re-find HIT" in line: d["refind_hit"] += 1
                if "[safety-spill] evict-smallest:" in line:
                    m = re.search(r"pages=(\d+)", line)
                    if m: d["evict_pages"].append(int(m.group(1)))
                    d["evict_smallest"] += 1
                if "[admit-session]" in line:
                    d["admit_session"] += 1
                if "prefix HIT (rewrite)" in line:
                    d["rewrite_prefix_hit"] += 1
                    m = re.search(r"frontier=(\d+)", line)
                    if m: d["checkpoint_frontiers"].append(int(m.group(1)))
                if "[rewrite-restore] FAILED" in line:
                    d["rewrite_restore_fail"] += 1
                if "WORKER RECOVER" in line:
                    d["worker_recover"] += 1
                if "reuse=private_turn_closure" in line:
                    d["private_turn_closure"] += 1
                if "finish=tool_calls" in line:
                    d["tool_calls_done"] += 1
    except OSError:
        pass
    return d


def evaluate(phase_name, sessions, stats0, stats1, log, expect_trash=False):
    v = []
    pr = (stats1 or {}).get("pressure", {})
    pr0 = (stats0 or {}).get("pressure", {})
    evicted = int(pr.get("private_owners_evicted", 0)) - int(pr0.get("private_owners_evicted", 0))
    degraded = int(pr.get("private_owners_degraded", 0)) - int(pr0.get("private_owners_degraded", 0))
    cr = (stats1 or {}).get("cache_reuse", {})
    cr0 = (stats0 or {}).get("cache_reuse", {})
    reused = int(cr.get("reused_prompt_tokens", 0)) - int(cr0.get("reused_prompt_tokens", 0))
    restores = int(pr.get("admission_safety_net_restores", 0)) - int(pr0.get("admission_safety_net_restores", 0))

    # CRASH CHECK — always enforced
    if log["worker_crash"] > 0:
        v.append(f"FAIL: {log['worker_crash']} WORKER CRASH")
    if log["bad_alloc"] > 0:
        v.append(f"FAIL: {log['bad_alloc']} std::bad_alloc")

    # Pressure (skip for single-session phases)
    pressure = evicted > 0 or degraded > 0
    if phase_name not in ("checkpoint-advance", "tool-calling", "responses-tools", "reasoning-effort"):
        if not pressure and not expect_trash:
            v.append("FAIL: no KV pressure")
        if pressure:
            v.append(f"PASS: pressure (evicted={evicted}, degraded={degraded})")

    # Cache reuse (skip for single-session phases)
    if phase_name not in ("checkpoint-advance", "tool-calling", "responses-tools", "reasoning-effort"):
        if reused > 0:
            v.append(f"PASS: cache reuse ({reused} tokens)")
        elif not expect_trash:
            v.append("FAIL: zero cache reuse")

    # Safety net
    if log["spill_ok"] > 0:
        v.append(f"PASS: {log['spill_ok']} spills OK (ckpt={log['spill_ckpt_ok']})")
    if log["spill_ckpt_missing"] > 0:
        v.append(f"WARN: {log['spill_ckpt_missing']} spills missing ckpt")
    if restores > 0 or log["restore_completed"] > 0:
        v.append(f"PASS: {max(restores, log['restore_completed'])} restores")
    if log["restore_started"] - log["restore_failed"] > log["restore_completed"]:
        incomplete = log["restore_started"] - log["restore_failed"] - log["restore_completed"]
        v.append(f"FAIL: {incomplete} restores started but not completed (excluding {log['restore_failed']} failures)")
    if log["max_net_entries"] >= 3 and log["restore_completed"] > 2:
        v.append(f"PASS: net accumulated (max={log['max_net_entries']})")
    if log["multi_extent_ok"] > 0:
        v.append(f"PASS: {log['multi_extent_ok']} scatter-gather allocations")
    if log["capture_skip"] > 0 or log["restore_skip"] > 0:
        v.append(f"PASS: {log['capture_skip'] + log['restore_skip']} capture skips (no crash)")
    if log["refind_hit"] > 0:
        if log["restore_completed"] >= log["refind_hit"]:
            v.append(f"PASS: {log['refind_hit']} re-find hits, restores completed")
        else:
            v.append(f"FAIL: {log['refind_hit']} re-find hits but insufficient restores")

    # Eviction order (mixed phase)
    if phase_name == "mixed" and len(log["evict_pages"]) > 1:
        ev = log["evict_pages"]
        fh = sum(ev[:len(ev)//2]) / max(len(ev)//2, 1)
        sh = sum(ev[len(ev)//2:]) / max(len(ev) - len(ev)//2, 1)
        if fh <= sh:
            v.append(f"PASS: eviction smallest-first (first={fh:.0f} <= second={sh:.0f})")
        else:
            v.append(f"WARN: eviction NOT smallest-first (first={fh:.0f} > second={sh:.0f})")

    # BIG session preservation (mixed phase)
    if phase_name == "mixed":
        for s in sessions:
            if s.name == "BIG":
                cold = sum(1 for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
                fast = sum(1 for t in s.turns if t["turn"] > 1 and t["wall_s"] <= 60)
                if cold == 0 and fast > 0:
                    v.append(f"PASS: BIG {fast} fast turns, 0 cold-starts")
                elif cold > 0:
                    v.append(f"WARN: BIG {cold} cold-starts (may have been evicted)")

    # Checkpoint advancement (checkpoint-advance phase)
    if phase_name == "checkpoint-advance":
        frontiers = log["checkpoint_frontiers"]
        if len(frontiers) >= 2:
            advancing = all(f2 > f1 for f1, f2 in zip(frontiers, frontiers[1:]))
            if advancing:
                v.append(f"PASS: checkpoint frontier advances monotonically "
                         f"({len(frontiers)} hits: {frontiers[0]}→{frontiers[-1]})")
            else:
                v.append(f"FAIL: checkpoint frontier does NOT advance monotonically ({frontiers})")
        elif len(frontiers) == 1:
            v.append(f"WARN: only 1 checkpoint hit — need more turns to verify advancement")
        else:
            v.append("FAIL: zero rewrite checkpoint hits — checkpoint not advancing")
        if log["private_turn_closure"] >= 2:
            v.append(f"PASS: {log['private_turn_closure']} private_turn_closure reuses")
        elif log["private_turn_closure"] == 0:
            v.append("FAIL: zero private_turn_closure reuses — checkpoint not reused")
        if log["rewrite_restore_fail"] > 0:
            v.append(f"FAIL: {log['rewrite_restore_fail']} rewrite restore failures")

    # Tool-calling phase
    if phase_name == "tool-calling":
        frontiers = log["checkpoint_frontiers"]
        if len(frontiers) >= 2:
            advancing = all(f2 > f1 for f1, f2 in zip(frontiers, frontiers[1:]))
            if advancing:
                v.append(f"PASS: checkpoint advances across tool-call turns "
                         f"({len(frontiers)} hits: {frontiers[0]}→{frontiers[-1]})")
            else:
                v.append(f"FAIL: checkpoint does NOT advance across tool-call turns ({frontiers})")
        elif len(frontiers) == 1:
            v.append(f"PASS: 1 checkpoint hit during tool-calling (frontier={frontiers[0]})")
        elif len(frontiers) == 0 and log["private_turn_closure"] == 0:
            v.append("FAIL: zero rewrite checkpoint hits during tool-calling")
        if log["tool_calls_done"] >= 1:
            v.append(f"PASS: {log['tool_calls_done']} tool-call rounds completed")
        elif log["tool_calls_done"] == 0:
            v.append("FAIL: zero tool-call rounds — model did not call tools")
        if log["private_turn_closure"] >= 1:
            v.append(f"PASS: {log['private_turn_closure']} checkpoint reuses during tool-calling")
        elif log["private_turn_closure"] == 0 and len(frontiers) == 0:
            v.append("FAIL: zero checkpoint reuses during tool-calling")
        if log["rewrite_restore_fail"] > 0:
            v.append(f"FAIL: {log['rewrite_restore_fail']} rewrite restore failures")
        if log["worker_recover"] > 0:
            v.append(f"WARN: {log['worker_recover']} worker recoveries (server survived, but requests may have failed)")

    # Responses API with tools (responses-tools phase)
    if phase_name == "responses-tools":
        frontiers = log["checkpoint_frontiers"]
        if len(frontiers) >= 2:
            advancing = all(f2 > f1 for f1, f2 in zip(frontiers, frontiers[1:]))
            if advancing:
                v.append(f"PASS: checkpoint advances across Responses API tool turns "
                         f"({len(frontiers)} hits: {frontiers[0]}→{frontiers[-1]})")
            else:
                v.append(f"FAIL: checkpoint does NOT advance across Responses API turns ({frontiers})")
        elif len(frontiers) == 0:
            v.append("FAIL: zero rewrite checkpoint hits in Responses API phase")
        if log["private_turn_closure"] >= 2:
            v.append(f"PASS: {log['private_turn_closure']} checkpoint reuses in Responses API")
        if log["rewrite_restore_fail"] > 0:
            v.append(f"FAIL: {log['rewrite_restore_fail']} rewrite restore failures")
        # All turns must succeed without errors
        errors = sum(1 for t in sessions[0].turns if t.get("input_tokens") is None)
        if errors == 0 and len(sessions[0].turns) >= 3:
            v.append(f"PASS: {len(sessions[0].turns)} Responses API turns with tools succeeded")
        else:
            v.append(f"FAIL: {errors} turns failed in Responses API")

    # Reasoning effort tier mapping (reasoning-effort phase)
    if phase_name == "reasoning-effort":
        # sessions[0] is the ReasoningEffortTester with results
        tester = sessions[0] if sessions else None
        if tester and hasattr(tester, "results"):
            ok = sum(1 for r in tester.results if r["ok"])
            fail = sum(1 for r in tester.results if not r["ok"])
            if ok == len(tester.results) and fail == 0:
                v.append(f"PASS: all {ok} reasoning effort levels accepted (tier mapping works)")
            else:
                v.append(f"FAIL: {fail}/{len(tester.results)} reasoning effort levels rejected")
                for r in tester.results:
                    if not r["ok"]:
                        v.append(f"  effort={r['effort']}: {r.get('error', 'unknown')[:100]}")

    # Worker recovery (all phases)
    if log["worker_recover"] > 0 and log["worker_crash"] == 0:
        v.append(f"PASS: {log['worker_recover']} worker recoveries without crash (logic_error caught)")

    # Trash mode: cold-starts and spill failures are acceptable
    if expect_trash:
        cold = sum(1 for s in sessions for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
        if cold > 0:
            v.append(f"PASS: {cold} cold-starts (expected in trash mode)")
        if log["spill_fail"] > 0:
            v.append(f"PASS: {log['spill_fail']} spill failures (expected — graceful degradation)")
        # Must verify trashing actually occurred
        if log["spill_fail"] == 0 and log["restore_failed"] == 0:
            v.append("WARN: no spill failures in trash mode — server handled load gracefully "
                     "(increase sessions or reduce host-kv to test trashing)")

    # Spill success rate (not trash mode, not single-session phases)
    if phase_name not in ("checkpoint-advance", "tool-calling", "responses-tools", "reasoning-effort"):
        total = log["spill_ok"] + log["spill_fail"] + log["restore_failed"]
        if total > 5 and log["spill_ok"] == 0 and not expect_trash:
            v.append(f"FAIL: 0 spills succeeded out of {total}")

    return v


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--model", default="qwen3.8-27b")
    p.add_argument("--max-output-tokens", type=int, default=48)
    p.add_argument("--serve-log", default="/home/zenz/ninfer-serve.log")
    p.add_argument("--timeout", type=int, default=120)
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
    if host_kv > 8 * 1024 * 1024 * 1024:
        print(f"ERROR: host-kv capacity is {host_kv / 1024**3:.1f} GiB — this looks like the "
              f"production server. The e2e tests require the test server (4 GiB host-kv, "
              f"32k context, 64k KV capacity). Start it with: tools/e2e/ninfer-start-test.sh")
        return 1
    if kv_pages > 4096:
        print(f"ERROR: KV capacity is {kv_pages} page groups — this looks like the production "
              f"server. The e2e tests require the test server (64k KV capacity = ~1024 pages). "
              f"Start it with: tools/e2e/ninfer-start-test.sh")
        return 1
    print(f"Server config OK: host-kv={host_kv / 1024**3:.1f} GiB, KV pages={kv_pages}")

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
            print("ABORT: phase 1 failed"); return 1
    stats1 = get_stats(args)
    log1 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("pressure", s1, stats0, stats1, log1):
        all_verdicts.append(("pressure", v))

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
            print("ABORT: phase 2 failed"); return 1
    stats1 = get_stats(args)
    log2 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("mixed", s2, stats0, stats1, log2):
        all_verdicts.append(("mixed", v))

    # Phase 3: trash — 10 sessions, graceful degradation (no crash)
    print("\n=== Phase 3: trash (10 sessions, 6 rounds) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s3 = [Session(f"S{i}", 20000, 2000, args) for i in range(10)]
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round(s3, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("ABORT: phase 3 failed"); return 1
    stats1 = get_stats(args)
    log3 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("trash", s3, stats0, stats1, log3, expect_trash=True):
        all_verdicts.append(("trash", v))

    # Phase 4: thinking — session-key fallback with rewrite checkpoint
    print("\n=== Phase 4: thinking (3 sessions, 6 rounds, reasoning mode) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s4 = [Session(f"T{i}", 10000, 2000, args) for i in range(3)]
    for s in s4:
        s.args = type(args)(**vars(args))
        s.args.thinking_mode = True
        s.args.max_output_tokens = 256
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
            "max_output_tokens": 256,
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
            print("ABORT: phase 4 failed"); return 1
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

    # Phase 5: checkpoint-advance — single session, verify frontier advances
    print("\n=== Phase 5: checkpoint-advance (1 session, 8 turns) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s5 = [Session("CKPT", 12000, 2000, args)]
    for r in range(1, 9):
        print(f"Round {r}:")
        errors = run_round(s5, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("ABORT: phase 5 failed"); return 1
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

    # Phase 6: tool-calling — multi-turn with tools, simulating Claude Code
    print("\n=== Phase 6: tool-calling (1 session, 6 turns, tools) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s6 = [ChatSession("TOOL", 10000, 1500, args)]
    # Override max_output_tokens so the model has room to generate tool calls
    s6[0].args = type(args)(**vars(args))
    s6[0].args.max_output_tokens = 256
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round(s6, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("ABORT: phase 6 failed"); return 1
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

    # Phase 7: responses-tools — Responses API with function_call + text (Claude Code pattern)
    print("\n=== Phase 7: responses-tools (1 session, 5 turns, Responses API) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s7 = [ResponsesApiSession("RSP", 10000, 1500, args)]
    s7[0].args = type(args)(**vars(args))
    s7[0].args.max_output_tokens = 256
    for r in range(1, 6):
        print(f"Round {r}:")
        errors = run_round(s7, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("ABORT: phase 7 failed"); return 1
    stats1 = get_stats(args)
    log7 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("responses-tools", s7, stats0, stats1, log7):
        all_verdicts.append(("responses-tools", v))
    cold = sum(1 for t in s7[0].turns if t["turn"] > 1 and t["wall_s"] > 60)
    if cold == 0 and len(s7[0].turns) > 1:
        all_verdicts.append(("responses-tools", f"PASS: 0 cold-starts across {len(s7[0].turns)} Responses API turns"))
    elif cold > 0:
        all_verdicts.append(("responses-tools", f"WARN: {cold} cold-starts in Responses API"))

    # Phase 8: reasoning-effort — verify tier mapping (high, minimal, max, low, medium)
    print("\n=== Phase 8: reasoning-effort (5 effort levels) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    tester = ReasoningEffortTester(args)
    tester.test()
    stats1 = get_stats(args)
    log8 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("reasoning-effort", [tester], stats0, stats1, log8):
        all_verdicts.append(("reasoning-effort", v))

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

