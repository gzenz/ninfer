# CLAUDE.local.md

Local, gitignored notes for working on NInfer on strix. AGENTS.md governs the repo;
this file adds the machine-specific environment. Do not commit.

## Branches (repo /home/zenz/ninfer)

- `local/combined` - PRODUCTION branch (main worktree /home/zenz/ninfer) =
  local/local-fixes + merge(pr/host-kv-cache) + merge(fix/tool-arg-schema-type)
  + merge(feat/serve-stats).
- `local/local-fixes` - local-only commits (froggeric v22 port, tolerant tool calls,
  effort-tier mapping, turn-closure checkpoint, Ostfralla routing, no-dangling-intent).
- `pr/host-kv-cache` (worktree /home/zenz/ninfer-rebase) - PR #64, host-KV cache.
- `fix/tool-arg-schema-type` (worktree /home/zenz/ninfer-tool-arg-fix) - PR #65,
  tool-arg schema type coercion.
- `feat/serve-stats` - local feature branch (read-only /stats endpoint + request-log
  size rotation, `f640f7b8`); merged directly into local/combined, not via
  local/local-fixes and not a PR.
- Local fixes never land directly on a PR branch or on combined; they merge into combined.

## Tool-call marker escaping (when editing tool_call_parser)

- The six tool-call XML markers (the tool_call wrapper open/close tags, function-open,
  parameter-open, parameter-close, function-close) are the exact byte sequences the
  Claude Code harness's own function-call parser looks for. Emitting any of them
  literally in tool arguments or output text makes the harness parse your text as a
  tool call and fail with "No such tool available". This is the same bug the parser
  fixes; it bites the agent that edits the parser.
- Rule: when a script must materialize, match, or grep these markers, build every `<`
  and `>` from `chr(60)` / `chr(62)` so the script source contains no literal marker.
  Run the script with `python3 script.py`. Do NOT use the Edit tool with literal markers
  in old_string/new_string; do NOT grep for them with a literal pattern on the command
  line. Worked example (this source is marker-free):

  ```python
  LT, GT = chr(60), chr(62)
  tool_open  = LT + "tool_call" + GT
  tool_close = LT + "/tool_call" + GT
  fn_open    = LT + "function="
  fn_close   = LT + "/function" + GT
  param_open = LT + "parameter="
  param_close= LT + "/parameter" + GT
  # grep the conflict markers the same way:
  head_mark = chr(60) * 4   # <<<<
  eq_mark   = chr(61) * 7   # ====
  their_mark= chr(62) * 7   # >>>>
  ```

## Merging local/local-fixes into local/combined

Repeat this whenever local-fixes has new commits that belong in production.

1. The combined worktree usually holds untracked junk (node_modules/, package.json,
   CLAUDE.md, CLAUDE.local.md). Stash any TRACKED modifications first
   (`git stash push -m pre-merge -- <files>`), merge, then `git stash pop`. Do NOT
   `git add -A` to stage the merge resolution - it sweeps in the untracked junk.
   Stage only the resolved files explicitly.
2. Merge with a merge commit (keeps history clear):
   `git merge --no-ff local/local-fixes` (edit the message in the editor, or pass
   `-F <msgfile>` for a prepared message).
3. Expect conflicts in the hotspots: src/serve/tool_call_parser.{h,cpp},
   src/serve/serve_options.{h,cpp}, src/serve/generation_service.cpp,
   tests/CMakeLists.txt, tests/test_tool_call_parser.cpp.
4. tool_call_parser.cpp conflict pattern: combined carries PR #65's tool-arg
   schema-type machinery (build_tool_param_type_map, param_allows_deserialization,
   a 5-arg parse_parameter taking tool_name + ToolParamTypeMap); local-fixes carries
   the depth-matching helper (find_matching_close, find_next_marker, ToolCallMarker).
   These are independent fixes over the same base - resolve to the UNION, not one side:
   keep HEAD's schema helpers, insert local-fixes' depth helper, keep HEAD's 5-arg
   signature (the shared body below the conflict already calls BOTH
   find_matching_close AND param_allows_deserialization). Drop local-fixes' 3-arg
   parse_parameter signature.
5. test_tool_call_parser.cpp conflict: combined has the schema-driven tests
   (3-arg calls: parse_qwen_tool_call_output(text, 64, map)); local-fixes adds
   test_nested_markers_in_value. Keep both. If the nested test was written with the
   2-arg form, fix it to the empty-map 3-arg form: pass a default-constructed
   `ninfer::serve::ToolParamTypeMap` (schema-less parsing - the value is a string).
   When the test body contains literal markers, regenerate it with the chr() method
   above rather than editing by hand.
6. Verify: confirm no conflict markers remain (chr-built Python scan), then
   `cmake --build build -j`. A clean build proves the two fixes coexist. Run
   ./tests/ninfer_tool_call_parser_test and ninfer_tolerant_tool_calls_test.
7. Commit the merge (stage only the resolved files), then `git stash pop`.

## Build & test

- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc`
  (nvcc is not on the PATH).
- Build: `cmake --build build -j` (no numeric job limit).
- Test: `ctest --test-dir build -R "<pattern>" --output-on-failure`, or run a binary
  directly: `./tests/<test_name>`.
- Known pre-existing failures, NOT regressions (do not chase):
  ninfer_openai_schema_test, ninfer_responses_schema_test,
  ninfer_anthropic_schema_test (effort-tier mapping drift, f58d55be);
  ninfer_qwen3_6_frontend_test (aborts on missing
  /home/neroued/models/llm/qwen/Qwen3.6-27B/base-hf-bf16/tokenizer.json - path not
  present on this box).

## Production server

- Binary: build/apps/ninfer-serve (built in the combined worktree /home/zenz/ninfer).
  Model: ~/ninfer-models/qwen3_8_27b_nvfp4-froggeric.ninfer.
- Start script: ~/ninfer-start.sh --thinking (kills old process, nohups the new binary,
  waits for /health, restarts the monitor sidecar on :8090). Logs to ~/ninfer-serve.log.
  Current flags include --tolerant-tool-calls (activates the depth-matching parser path)
  and --host-kv-cache-mib 29645.
- host-KV cache: use the budget model, `--host-kv-cache-mib 29645` (~29 GB pinned).
  The old fixed-slab `--host-kv-cache N` is superseded. Do not raise the budget to the
  point of four slabs (~38.8 GB pinned) - that OOM-crashes the WSL VM in a loop.
- Old binary backup: /home/zenz/ninfer-serve.bak-20260821.
- A Claude Code session on WSL is served by this server: restarting kills the session.
  Do stop-test-restart atomically; launch the restart detached (setsid, own log file)
  so the start script survives the launching shell exiting.
- Verify after restart: `/health` returns ok, `/stats` returns schema
  `ninfer_serve_stats`, and a fresh ninfer-serve PID is running the newly built binary.

## froggeric template & renderer

- No jinja interpreter: the C++ renderer (src/targets/qwen3_6/impl/frontend/
  chat_template.cpp) re-implements the template semantics; the jinja text is only
  a sha256 digest key (CompiledChatTemplate::resolve picks a ChatTemplateSemantics).
- Local additions on top of froggeric v22 (keep these when porting updates):
  - effort-tier mapping in src/serve/translate.cpp: 7-value RequestedReasoningEffort
    to 3-value engine enum (minimal to Low, high/max to XHigh).
  - no-dangling-intent bullet in kFroggericToolInstructions (never end a turn with
    a statement of intent).
- froggeric v22.3 port pending (digest 6e1439c9...).
- Artifacts are repacked with ~/repack.py (the template text must byte-match).
- updates from https://huggingface.co/froggeric/Qwen-Fixed-Chat-Templates

## Conventions

- Conventional Commit subjects; commit only when asked.
- No em dashes or smart quotes in output; plain hyphens and straight quotes.
- Prefer self-calibrating discovery over hardcoded values.
