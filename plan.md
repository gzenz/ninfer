# Frontend Template Execution — jinja engine integration + froggeric v22.5 (2026-09-10)

> Replaces the "Upstream Sync 2026-09-09" plan (dual-recipe NVFP4 KV),
> which is **not executed** as of today: verified absent from master and
> both local checkouts (no `kv-recipe` in src, no `local/up-sync-2026-09`
> branch, master still at d6ae7d2f = PR #3 checkpoint-host-demotion merge).
> That sync remains a separate pending effort and is deferred by this plan.
>
> What did land from the leak-incident line:
> - the tool-call leak fix shipped (3c0b4dc5, `--tolerant-tool-calls` last-close
>   recovery + empty-tools contract) and is deployed on the production server
> - the pi-plan client fix (never wipe the active tool set) landed
> - the A/B leak battery now has a permanent no-tools condition (D_notools)
>   verified RECOVERED end-to-end

## Context: the template file is a digest label, not executable behavior

Verified state (2026-09-10):

- `frontend/chat_template.jinja` in the artifact is consumed only as (1) a
  SHA-256 digest that selects the C++ renderer semantics, or is bypassed
  entirely by `--chat-template-semantics`; (2) a byte-consistency check
  against `tokenizer_config.json.chat_template`; (3) a raw resource in the
  artifact.
- No jinja interpreter exists anywhere in the repo (code, tests, history).
  All rendering is the hand-written C++ port in
  `src/targets/qwen3_6/impl/frontend/chat_template.cpp` (~40 KB, the
  "port froggeric v22 chat template semantics" lineage plus local adoptions:
  multi-variant think-close handling, no-dangling-intent rule, tool-example
  fixes).
- The README "Chat template loading" section promises that `--chat-template`
  loads any jinja template and changes behavior. It does not: pointing the
  flag at a different file changes the rendered output by zero bytes. The
  documented feature is a fake.

Decision (user, 2026-09-10): make the promise real — add a real jinja engine
so `--chat-template` genuinely loads and executes the specified file. The
froggeric v22.5 merge (Phase 3) then becomes a template content change
instead of a C++ porting project.

## Phase 1 — Engine selection (done, 2026-09-10)

Selected: `wangzhaode/jinja.cpp` — Apache-2.0, single 80 KB header + 25 KB
JSON bridge (`ujson.hpp`), nlohmann backend (already vendored in
`third_party/nlohmann`). Purpose-built for LLM chat templates; validated by
its authors against Python transformers on Qwen 2.5/3, DeepSeek, Llama.

Rejected: `jinja2cpp/Jinja2Cpp` (599 stars, broader conformance) — requires
Boost >= 1.65 + fmt + four nonstd-lite libraries; contradicts the repo's
three-small-libs `third_party` discipline. `hughperkins/Jinja2CppLight` —
dead since 2020.

Proof (byte-exact parity, oracle = Python jinja2 3.1.6, keep_trailing_
newline, rendering the actual v22.5 template): 24/24 contexts identical,
including tool definitions (xml + json formats), tool-history replay,
reasoning replay, all effort aliases, error-tiering (text + JSON payload),
truncation (text path + v22.5 JSON-payload-skip), inline effort tags,
image/video/vision-id media, auto-disable, multi-call turns, the literal
think-close-tag incident case, and both raise_exception error paths
(error-message parity).

Five contained patches to the vendored header (each verified in the parity
harness):
1. `make_unique` polyfill collides with `std::make_unique` under modern
   standards -> guard with `__cpp_lib_make_unique`.
2. `join` filter missing (20 uses in the template) -> added.
3. `tojson` used a custom tool-canonical key order -> aligned to jinja2
   semantics (sort_keys, `", "`/`": "` separators, UTF-8 preserved).
4. String slicing missing (`content[:n]`, `[:1]`) — the truncation and
   error-tiering branches were silent no-ops -> added string-slice branch.
5. Tuple literals (`x in ('a', 'low')`) parsed as function calls — the
   effort-alias branches misrouted -> parenthesized comma lists parse as
   arrays.
Plus one registration: `raise_exception` via the engine's `add_function`
API as a throwing function (render() propagates exceptions — verified).

Harness lives at `/tmp/jinja_parity` (24-context suite, render drivers for
both engines, patched header). Promote into the repo in Phase 2.

## Phase 2a — Perf gate (measured 2026-09-10, Mac, -O2, C++20)

~200.7k-token context (802,896 chars, 311 messages: system + 40 tool rounds
with generated code arguments/results + xhigh thinking), 5 timed renders:

| renderer | best | mean | output |
|---|---|---|---|
| hand-written C++ port, v22 | 5.78 ms | 6.44 ms | 861,908 B |
| jinja engine, v22 template | 5.37 ms | 5.56 ms | 830,711 B |
| jinja engine, v22.5 template | 7.46 ms | 7.61 ms | 833,637 B |

One-time (startup, not per request): template compile 6.2–8.6 ms, context
JSON parse 3.4 ms (real integration builds the context in-memory — no parse).

- Engine vs port at equal scale: engine is faster (no regression; the port's
  per-message span tracking costs more than the expression walk).
- v22.5 template complexity: +1.9 ms/render vs v22 (template delta, not engine).
- Against prefill (2–4 s at 200k tokens) render is <0.1% of wall time.
- Confirmed: the port's output is 31 KB larger than the engine rendering the
  v22 file — the local adoptions (stricter IMPORTANT block, no-dangling-intent,
  think-first example) are NOT in the v22 fixture and are re-emitted per tool
  round (~775 B/round). Phase 2.4 (move adoptions into the template) is
  mandatory for cutover; otherwise the rendered prompt changes and prefix
  reuse breaks.
- Final confirmation on strix with a real 200k request is part of Phase 2
  validation (absolute numbers will differ; relative comparison is the gate).

## Phase 2 — Integration

1. Vendor `third_party/jinja/` (header + ujson bridge, pinned version, the
   five patches, LICENSE).
2. CMake target, linked into `ninfer_engine`.
3. Frontend rewiring:
   - `CompiledChatTemplate` = compiled jinja template + registered globals
     (raise_exception) + identity digest.
   - Context construction maps PromptInput to the template variables
     (enable_thinking, reasoning_effort, tool_call_format, preserve_
     thinking, add_vision_id, max_tool_arg_chars, max_tool_response_chars,
     auto_disable_thinking_with_tools, messages, tools). The current C++
     port's option mapping is the reference for required keys.
   - **Structured output reconstruction (the open piece, do first):**
     `RenderedChat` needs literal spans, media placeholder spans with
     item_index, and message/cache byte boundaries. A plain render is a
     string. Approach: media tokens are emitted by the template in item
     order, so item_index = occurrence order via an ordered placeholder
     scan; cache/message boundaries via sentinel markers injected at
     boundary positions and stripped post-render. Parity-validate against
     the current renderer's structured output (all three registered
     semantics) before cutover.
4. Move our local adoptions into the template file (they live in the C++
   port, not the v22 fixture): multi-variant think-close handling,
   no-dangling-intent rule, tool-instruction example fixes. Small template
   edits to our fixture.
5. Retire the hand-written C++ render path (~40 KB) — one renderer, not two.
   The digest/semantics flag surface shrinks accordingly (decide at cutover:
   keep `--chat-template-semantics` only if distinct behavior remains).
6. Docs: README "Chat template loading" now describes real behavior;
   update `docs/serving.md` and the artifact docs.

Verification:
- Unit: the 24-context parity suite promoted to `tests/` (C++ render vs
  Python-oracle reference outputs, byte-exact).
- Structured-output parity vs the current renderer on a fixed corpus
  (media, boundaries, tool history) — gate for cutover.
- E2E on the GPU box: existing e2e suite + the tool-leak A/B battery
  (D_notools must stay RECOVERED) + the live server under the new path.
- Performance: render-time measurement on the prefill hot path (large
  prompts, 500k+ context) vs the current port. The port is O(n) string
  assembly; the engine adds parse overhead. No acceptable regression —
  this is a gating measurement, not a follow-up.

Risks:
- Engine maturity (20-star library): mitigated by the byte-exact parity
  proof, vendoring at a pin, and all five patches being small and local.
- Structured boundary reconstruction: main engineering risk — build and
  validate it before rewiring anything else.
- Hot-path throughput: measured, not assumed.

## Phase 3 — Froggeric v22.5 (after Phase 2)

With a real engine, the v22.5 "merge" stops being a C++ port and becomes a
template content change:

1. New fixture `froggeric_v22.5_chat_template.jinja` = upstream v22.5 + our
   local adoptions from Phase 2.4 (same file, no split ownership).
2. Register as the template identity (v22 -> v22.5 is a semantic upgrade of
   the same identity: digest update, fixture replacement in the artifact).
3. All v22.1->v22.5 semantics come from the template itself — effort
   aliases, default-medium, leading system-prompt merge, explicit reasoning
   field variants (reasoning_content/thinking/reasoning) + lead-strip,
   error tiering, multi-tool token parity, non-thinking tool-prompt
   alignment, video_url, JSON-payload truncation protection. No C++ work.
4. Verification: parity suite re-run at v22.5, tool-leak A/B, e2e suite on
   the GPU box.

Standing caveat: upstream v22.5 still extracts in-content thinking at the
FIRST think-close occurrence. Our local hardening (close-tag handling that
does not split on a quoted literal) stays in the template as our deviation,
and the deployed server-side tolerant parser remains the backstop for
already-emitted text-form calls.

## Effort

- Phase 2: ~2-3 days (structured-output reconstruction is the bulk).
- Phase 3: ~0.5-1 day (template edit + validation) once Phase 2 lands.


## Post-Merge Findings — Arena Fragmentation & Eviction Strategy (2026-09-10)

Production testing under extreme pressure (single 371k-token session, 1016
messages, arena exhausted to 14MB free) revealed two issues in the host KV
safety net arena that were not visible under moderate load.

### Finding A — Arena fragmentation from scatter-gather allocations

The scatter-gather multi-extent allocation (added to work around single-
allocation failures) is **causing the fragmentation it was meant to solve**:

1. Single contiguous allocation fails (free space split into small chunks)
2. Scatter-gather splits into 2-3 smaller allocations
3. These create separate used regions in the arena
4. When freed, they leave separate small free holes
5. Next allocation can't fit in any single hole → more scatter-gather
6. Feedback loop: more fragmentation → more scatter-gather → more fragmentation

**Evidence (from systemd journal, 16725 lines):**
- 144 single-allocation failures with avg 7.90GB free (needing only 5.6GB)
- Worst case: 14.22GB free, 6.72GB needed — couldn't allocate contiguously
- 137 double-allocations, 6 triple-allocations (scatter-gather splitting)
- Arena free hit 14MB despite 30GB capacity and only ~15GB used
- 278 FALLBACK (state-only) events as a direct consequence
- 1 bad_alloc (caught by WORKER OOM handler, server survived)

**Root cause:** The arena allocator does not coalesce adjacent free regions.
It appears to use a simple free-list or bump allocator that fragments under
multi-size allocation patterns. The scatter-gather workaround creates the
multi-size pattern that triggers fragmentation.

**Fix needed:** Arena allocator must coalesce adjacent free blocks (like a
proper malloc) or implement compaction when free space is fragmented below
a threshold. Alternatively, use a slab/buddy allocator that naturally
avoids fragmentation for power-of-two allocation sizes.

### Finding B — evict-smallest evicts only ONE entry

When the arena is full and a new spill needs space, `evict-smallest`
evicts only ONE entry. If that entry is small (e.g., 2 pages = ~2MB),
freeing it does not provide enough space for the new allocation (e.g.,
5-7GB). The spill then falls through to scatter-gather (which may
succeed by splitting) or to state-only FALLBACK (which loses KV).

**Evidence:**
- 276 evict-smallest operations
- Evicted entry sizes range from 2 pages (~2MB) to 12106 pages (~12.8GB)
- Most common `remaining` count after eviction: 3 (91 times)
- With 3 entries of ~12.5GB each = 37.5GB > 30GB arena → still full
- Min free after eviction: 14MB — evicting one small entry barely helped

**Fix needed:** `evict-smallest` should loop: keep evicting entries
(smallest first) until `free >= need`, not just evict one and stop.
This is a simple loop change in the spill function. The current
single-eviction design assumes entries are roughly uniform in size,
which is false for mixed session sizes (a 371k session's checkpoint
is 50x larger than a 5k session's checkpoint).

### Impact

Both issues compound: fragmentation prevents single allocations (Finding A),
which triggers scatter-gather, which worsens fragmentation, which eventually
makes even scatter-gather fail, which triggers state-only FALLBACK, which
loses KV and causes re-prefills. The bad_alloc at the end of this chain
was caught by the OOM handler (server survived), but the re-prefills
(278 FALLBACK events) are the real cost.

### Verification after fix

- Arena should reach 0 single-allocation failures when free >= need
- Scatter-gather should only fire for genuine large allocations (>50% arena)
- State-only FALLBACK should only fire when arena is genuinely full
  (total used >= 90% of capacity), not when fragmented
- evict-smallest should free enough space for the new allocation in one pass
