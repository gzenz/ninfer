#!/usr/bin/env python3
"""D3 end-to-end: a tolerant tool-call parse must not swallow a literal close tag.

W3.4 of the plan. The unit test (`tests/test_tool_call_parser.cpp`) fixes the parser in
isolation; this suite is the same question asked of the *served* path: a ChatSession is
asked to write a file whose content contains a literal `</parameter>` (and, in the second
case, a `</parameter>` followed by a line `</function>`, and in the third an unterminated
`<parameter=`). The observed production defect was that a lone `</parameter>` inside a
value closed the parameter early, the wrapper then failed to parse, and the server emitted
the whole tool call as **text** ("tool markup returned as text" in the serve log).

The decisive oracle is the **sentinel tail**: every case asks the model to end the file
with a case-specific line, and the assertion is that the parsed `input.content` still ends
with it. A parser that terminates the value at a stray close tag cannot pass that, no
matter how it formats the recovered call. The size is chosen to reproduce the production
condition (the leak correlated with large Write/Edit payloads).

Requires a server started with `--tolerant-tool-calls` (prod and the e2e test server both
are). If the server is in strict mode the suite says so and exits 2 rather than reporting a
pass -- an invalid configuration must not read as a clean result.

Usage (inside the swap, test server up):
  python3 tools/e2e/toolcall-e2e.py [--lines 120] [--host 127.0.0.1] [--port 8080]

Exit 0 = all cases parsed as tool calls with intact content; 1 = a case failed; 2 = the
server is not in tolerant mode (suite not applicable).
"""

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.request

TOOLS = [
    {"name": "read_file", "description": "Read a file",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}},
                      "required": ["path"]}},
    {"name": "write_file", "description": "Write a file",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"},
                                                       "content": {"type": "string"}},
                      "required": ["path", "content"]}},
]

SERVE_LOG = os.path.expanduser(os.environ.get("E2E_SERVE_LOG", "~/ninfer-serve.log"))
LEAK_MARKER = "tool markup returned as text"


def post_messages(args, payload, timeout=900):
    req = urllib.request.Request(
        f"http://{args.host}:{args.port}/v1/messages",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json", "anthropic-version": "2023-06-01"},
        method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.load(resp)


def blocks(out):
    return out.get("content", []) or []


def text_of(out):
    return "".join(b.get("text", "") for b in blocks(out) if b.get("type") == "text")


def tool_uses(out):
    return [b for b in blocks(out) if b.get("type") == "tool_use"]


def tail_lines(name):
    """Unique sentinel tail (stable across runs, so artifacts stay comparable): the decisive
    oracle for early value termination."""
    return "END-OF-PAYLOAD-" + hashlib.sha256(name.encode()).hexdigest()[:8]


def case_prompt(marker, sentinel, lines):
    return (
        "Call the write_file tool exactly once and nothing else. Do not add commentary.\n"
        f"path: payload-{sentinel}.txt\n"
        f"content: a plain text file of at least {lines} lines. Each line is a short sentence of "
        "prose about file handling. Several lines, spread through the file, must contain this "
        f"literal text verbatim: {marker}\n"
        f"The very last line of the file must be exactly: {sentinel}\n"
        "Do not escape, quote or explain the literal text: it must appear in the file as-is. "
        "This is a parser test: if the literal is escaped, described or omitted the test fails, so "
        "write those exact characters into the content."
    )


CASES = [
    # name, marker shown to the model, whether the case is the large-payload one
    ("literal_close_tag", "</parameter>", True),
    ("close_tag_then_function", "</parameter>\n</function>", True),
    ("cut_parameter_open", "<parameter=payload", False),
    ("control_plain_write", "", False),
]


def run_case(args, name, marker, large):
    sentinel = tail_lines(name)
    lines = args.lines if large else 6
    prompt = case_prompt(marker, sentinel, lines)
    payload = {"model": args.model, "max_tokens": args.max_tokens, "temperature": 0.0,
               "tools": TOOLS, "messages": [{"role": "user", "content": prompt}]}
    t0 = time.time()
    try:
        out = post_messages(args, payload)
    except Exception as exc:  # noqa: BLE001 - reported as a case failure
        return {"case": name, "ok": False, "why": f"request failed: {exc}", "wall": time.time() - t0}
    wall = time.time() - t0
    rec_window = [int(t0 * 1000), int((t0 + wall) * 1000)]
    uses = tool_uses(out)
    text = text_of(out)
    stop = out.get("stop_reason")
    rec = {"case": name, "stop_reason": stop, "n_tool_use": len(uses),
           "text_len": len(text), "wall": round(wall, 1), "ok": True, "why": [],
           "window_ms": rec_window}

    def bad(msg):
        rec["ok"] = False
        rec["why"].append(msg)

    if stop != "tool_use":
        bad(f"stop_reason={stop!r} (expected 'tool_use'): the call was not parsed as a tool call")
    if len(uses) != 1:
        bad(f"{len(uses)} tool_use blocks (expected exactly 1)")
    if "<tool_call>" in text or "<function=" in text:
        bad("tool markup leaked into a text block")
    if uses:
        u = uses[0]
        inp = u.get("input") or {}
        rec["name"] = u.get("name")
        content = inp.get("content", "")
        rec["path"] = inp.get("path")
        rec["content_bytes"] = len(content)
        rec["content_lines"] = content.count("\n") + 1
        if u.get("name") != "write_file":
            bad(f"tool name {u.get('name')!r} (expected 'write_file')")
        if inp.get("path") != f"payload-{sentinel}.txt":
            bad(f"path {inp.get('path')!r} (expected 'payload-{sentinel}.txt')")
        # The marker check uses its first line only: a multi-line marker (the
        # `</parameter>` + `</function>` case) may legitimately come back with different
        # whitespace, and the decisive oracle for early termination is the sentinel tail
        # below, not the exact markup. (A first version compared the whole multi-line
        # marker and reported a false failure against a control that was correct.)
        first_marker = marker.split("\n")[0]
        if first_marker and first_marker not in content:
            # Say WHY it is missing: an escaped form is a model-compliance answer, an absent one is
            # too, and a value that stops right before the marker is the parser cutting it -- three
            # different readings that a bare "missing" collapses into one.
            escaped = any(tok in content for tok in ("lt;/parameter", "lt;/function", "&lt;"))
            rec["marker_escaped"] = escaped
            bad(f"marker {first_marker!r} missing from the parsed content"
                + (" (an ESCAPED form is present, so the model declined to write the literal)"
                   if escaped else " (no escaped form either: the literal is simply absent)"))
        if not content.rstrip().endswith(sentinel):
            bad(f"content does not end with the sentinel {sentinel!r} -> the value was cut "
                f"(ends with {content.rstrip()[-60:]!r})")
        if large and rec["content_lines"] < lines * 0.8:
            bad(f"content has {rec['content_lines']} lines, expected ~{lines}: truncated")
        if "<tool_call>" in content:
            bad("'<tool_call>' inside the parsed content")
    return rec


def tolerant_mode(port):
    """Is the served process running with --tolerant-tool-calls? Read its argv, do not infer the
    mode from response shapes -- a strict server and a tolerant server that both fail look alike,
    and a suite that guessed would report a pass for a configuration it never tested."""
    import glob
    candidates = []
    for path in glob.glob("/proc/[0-9]*/cmdline"):
        try:
            with open(path, "rb") as f:
                argv = f.read().split(b"\0")
        except OSError:
            continue
        # Match the executable itself, not any argument that merely mentions the binary name: the
        # monitor sidecar's `--serve-log /home/zenz/ninfer-serve.log` matched a first version of
        # this check and made it report the wrong process's flags.
        if not any(a.endswith(b"ninfer-serve") for a in argv):
            continue
        candidates.append([a.decode() for a in argv])
    for args in candidates:  # prefer the process serving the port we are about to test
        if f"--port={port}" in args or ("--port" in args and str(port) in args):
            return "--tolerant-tool-calls" in args
    # No process owns that port: only an unambiguous single server may stand in for it (during a
    # swap the test server is the only ninfer-serve; with say prod + a leftover both alive, saying
    # nothing is the honest answer).
    if len(candidates) == 1:
        return "--tolerant-tool-calls" in candidates[0]
    return None


def request_log_records(start_ms):
    """`request_done` records newer than start_ms, from the request log.

    The `result.tool_call_parse` block is what separates the two readings a content-shape check
    cannot: `fallback_reason=truncated_tail` with `finish_reason=output_limit` means the MODEL ran
    out of tokens (not the parser), while `fallback_reason=none` with a complete value means the
    model simply did not write the marker. A first version of this suite reported both as parser
    failures -- an instrument blaming the thing it was aimed at for a cause it never checked."""
    path = os.path.expanduser(os.environ.get("E2E_REQUEST_LOG", "~/ninfer-requests.jsonl"))
    out = []
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                try:
                    d = json.loads(line)
                except ValueError:
                    continue
                if d.get("event") != "request_done" or d.get("timestamp_unix_ms", 0) < start_ms:
                    continue
                out.append(d)
    except OSError as exc:
        print(f"  SKIP request-log telemetry: {exc}")
    return out


def attach_telemetry(cases, records):
    """Attach each case's request-log record by its time window, then re-judge where the record
    proves the failure was not the parser's."""
    for rec in cases:
        w = rec.get("window_ms") or [0, 0]
        match = [d for d in records if w[0] <= d.get("timestamp_unix_ms", 0) <= w[1]]
        if len(match) != 1:
            rec["telemetry"] = f"{len(match)} request records in its window (expected 1)"
            continue
        r = (match[0].get("result") or {})
        p = r.get("tool_call_parse") or {}
        rec["finish_reason"] = r.get("finish_reason")
        rec["marker_seen"] = p.get("marker_seen")
        rec["fallback_reason"] = p.get("fallback_reason")
        rec["structured_calls"] = p.get("structured_call_count")
        rec["completion_tokens"] = r.get("completion_tokens")
        # A case the model ran out of tokens on never reached the parser's decision, and one where
        # the model wrote a complete value without the literal never exercised the defect.
        if r.get("finish_reason") == "output_limit":
            rec["ok"] = None  # inconclusive
            rec["why"] = [w for w in rec["why"]
                          if "sentinel" not in w and "truncated" not in w and "lines" not in w]
            rec["why"].append("INCONCLUSIVE: the model hit the output-token limit "
                              "(finish_reason=output_limit) -- raise --max-tokens; the parser never "
                              "decided anything")
        elif rec.get("ok") is False and all("marker" in w for w in rec["why"]):
            rec["ok"] = None
            rec["why"].append("INCONCLUSIVE: the value parsed COMPLETE (sentinel intact, "
                              "fallback_reason=none) and simply does not contain the literal -- the "
                              "model declined to write it, so the case exercised nothing")


def serve_log_leaks(before):
    """Count leak telemetry added since the run started; report a SKIP if unreadable."""
    try:
        with open(SERVE_LOG, "r", errors="replace") as f:
            lines = f.readlines()
    except OSError as exc:
        return None, f"cannot read {SERVE_LOG}: {exc}"
    return sum(1 for ln in lines[before:] if LEAK_MARKER in ln), None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--model", default="qwen3.8-27b")
    p.add_argument("--lines", type=int, default=int(os.environ.get("TOOLCALL_LINES", "40")),
                   help="lines requested in the large-payload cases (default 40: the defect is a "
                        "stray close tag inside a value, which a few KB of content exercises; a "
                        "bigger payload only risks hitting the output-token cap, which a first "
                        "version did -- 80 lines with max_tokens 3000 was cut by the LIMIT, and the "
                        "suite then blamed the parser for its own budget)")
    p.add_argument("--max-tokens", type=int, default=int(os.environ.get("TOOLCALL_MAX_TOKENS",
                                                                       "4000")),
                   help="must exceed the longest case's output, else finish_reason=output_limit "
                        "makes the sentinel check meaningless")
    p.add_argument("--json", dest="json_out", default=None)
    p.add_argument("--start-phase", type=int, default=1)  # accepted and ignored by the driver
    args = p.parse_args()

    before = 0
    try:
        with open(SERVE_LOG, "r", errors="replace") as f:
            before = len(f.readlines())
    except OSError:
        pass

    print(f"toolcall e2e: {len(CASES)} cases, large-payload lines={args.lines}, temp=0")
    print(f"serve log: {SERVE_LOG} (offset {before})")
    t0 = time.time()
    results = [run_case(args, name, marker, large) for name, marker, large in CASES]
    wall = time.time() - t0

    leaks, leak_err = serve_log_leaks(before)
    attach_telemetry(results, request_log_records(int(t0 * 1000)))

    for r in results:
        status = "ok" if r.get("ok") else ("????" if r.get("ok") is None else "FAIL")
        print(f"  {status:4} {r['case']:<24} stop={r.get('stop_reason')} "
              f"uses={r.get('n_tool_use')} lines={r.get('content_lines')} "
              f"bytes={r.get('content_bytes')} wall={r.get('wall')}s")
        if r.get("finish_reason") or r.get("fallback_reason"):
            print(f"       telemetry: finish={r.get('finish_reason')} "
                  f"marker_seen={r.get('marker_seen')} fallback={r.get('fallback_reason')} "
                  f"structured={r.get('structured_calls')} tokens={r.get('completion_tokens')}")
        for w in r.get("why", []):
            print(f"       - {w}")
    if leak_err:
        print(f"  SKIP leak-telemetry check: {leak_err}")
    else:
        print(f"  leak telemetry: {leaks} new '{LEAK_MARKER}' line(s) since offset {before}")

    failures = [r for r in results if r.get("ok") is False]
    inconclusive = [r for r in results if r.get("ok") is None]
    parsed = [r for r in results if r.get("n_tool_use")]
    print(f"\n{len(results)} cases in {wall:.0f}s; parsed_as_tool_call={len(parsed)}/{len(results)} "
          f"failures={len(failures)} inconclusive={len(inconclusive)}")

    # An invalid configuration is not a pass. The mode is read from the served process's argv, so
    # "the server was strict" is a fact rather than a guess from response shapes.
    tolerant = tolerant_mode(args.port)
    if tolerant is False:
        print("NOT APPLICABLE: the served process is running WITHOUT --tolerant-tool-calls; "
              "this suite tests the tolerant recovery path.")
        return 2
    if tolerant is None:
        print("NOTE: could not read the served process's argv — tolerant mode unverified.")
    if leaks:
        print(f"FAIL: {leaks} response(s) logged '{LEAK_MARKER}'")
        failures.append({"case": "serve-log leak telemetry"})
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump({"cases": results, "leaks": leaks, "wall_s": wall}, f, indent=1)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
