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
newline, rendering the actual v22.5 template): 25/25 contexts identical,
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

Harness lives at `/tmp/jinja_parity` (25-context suite, render drivers for
both engines, patched header). Promote into the repo in Phase 2.

## Phase 2a — Perf gate (measured 2026-09-10, Mac, -O2, C++20) — PASS

~200.7k-token context (802,896 chars, 311 messages, 154 tool rounds with
code arguments/results, xhigh thinking), tool arguments in the production
shape (JSON mappings, not wire strings), 5 timed renders each:

| renderer | best | mean | output |
|---|---|---|---|
| hand-written port (v22 + adoptions) | 5.19 ms | 5.23 ms | 807,676 B |
| jinja engine, v22 template | 5.90 ms | 6.03 ms | 804,563 B |
| jinja engine, v22.5 template | 7.80 ms | 8.27 ms | 807,489 B |

One-time (startup, not per request): template compile ~6-15 ms; context JSON
parse ~8 ms (the integration builds the context in-memory, so only the
compile applies).

- Gate: no regression. The engine renders 13% slower than the hand-written
  port (5.90 vs 5.19 ms best); v22.5 complexity adds +1.9 ms. Both are <0.2%
  of a 2-4 s 200k-token prefill.
- Scale parity: the engine is byte-identical to the Python jinja2 oracle at
  200k tokens for BOTH v22 (804,563 B) and v22.5 (807,489 B), so the
  25-context suite's guarantee holds at scale.
- The port is 3,113 B larger than the template's own v22 render. Those
  adoption differences (D1/D2/D3) are port-only and disappear at cutover,
  because the template becomes the renderer.
- Engine defects this gate exposed (all fixed in the vendored header; see
  third_party/jinja/NOTES.md): jinja2 iterates mappings in insertion order
  while nlohmann::json sorts keys, which silently reordered every multi-key
  parameter block; `set` stored variables inside the context document, and
  with vector-backed ordered objects that insertion relocated the document
  being iterated (dropped/truncated tool arguments); `tojson` sorted
  reference-like JSON wrappers in place, writing through them and corrupting
  values.
- Final confirmation on strix with a real 200k request stays in Phase 2
  validation (absolute numbers differ on the 5090 box; the relative
  comparison is the gate).

## Phase 2 — Integration (done, 2026-09-10)

1. Vendored `third_party/jinja/` (header + ujson bridge, pinned, patches, LICENSE).
2. CMake: `ninfer_jinja` interface target (top level), added to `ninfer_engine`;
   CUDA-free test targets for the engine and the frontend renderer.
3. Frontend rewiring (done): `CompiledChatTemplate` now owns a compiled template and
   delegates every render to it; the ~800-line hand-written renderer is gone.
   `--chat-template-semantics` selects advertised capabilities and the
   registered-template diagnostic only, because the file is the renderer.
   - Context construction maps `ChatMessage`/`ChatRenderOptions` onto the template
     variables (messages, tools, enable_thinking, reasoning_effort, preserve_thinking,
     add_vision_id, add_generation_prompt). Tool arguments are parsed from the wire
     string into a mapping, which is what routes the template into its
     `<parameter=...>` branch (the D2 adoption).
   - Structured output is reconstructed from the engine's render trace, not by
     scanning the text: message frontiers come from the message loop's iteration
     spans, literal spans from printed-leaf provenance (minus media and vision-id
     markers), media placeholders from the content layout, cache markers from the
     corresponding frontiers, and rewrite checkpoints from the assistant-block and
     generation-suffix structure.
4. Adoptions moved into the template file (done): the thinking-disabled replay rule
   (an empty reasoning block must match the generation prologue for prefix reuse) and
   the no-dangling-intent rule now live in the fixture, not in C++.
5. Hand-written render path retired (done): `chat_template.cpp` is 120 lines of
   registration and delegation.
6. Docs updated: README "Chat template loading" and `docs/serving.md` describe
   execution of the file.

Verification (Mac, no CUDA):
- `tests/test_jinja_template.cpp`: 26 contexts byte-exact against the Python oracle.
- `tests/targets/qwen3_6/test_jinja_frontend_render.cpp`: all three registered
  templates x 22 contexts = 66 renders byte-exact against the Python oracle for the
  same file, plus structured-output invariants (boundaries monotone and in range,
  literal spans sorted and disjoint, media spans covering the pad token in render
  order, cache/rewrite frontiers in range).
- `tests/targets/qwen3_6/test_froggeric_render.cpp`: all 16 prompt-contract checks
  pass against the executed template.
- The oracle environment is the HuggingFace one (`trim_blocks=True`,
  `lstrip_blocks=True`, `keep_trailing_newline=True`), which is what the registered
  templates are written for; the engine implements both flags.

Open items carried out of Phase 2:
- Artifact regeneration: the qwen3.8 artifact embeds the template, so the fixture's
  local additions (and its digest, now registered as `kFroggericV22TemplateDigest`)
  need a conversion run. `kFroggericV22DeployedTemplateDigest` keeps the currently
  deployed digest accepted until that rebuild happens, then goes away.
- GPU-box verification: the CUDA build plus the e2e suite, the tool-leak A/B battery,
  and a live-server run under the new path (the engine-level `test_frontend` links the
  full engine and cannot be built on the Mac).
- The engine executes tool-argument/response truncation and `tool_call_format` from
  the template, but the frontend does not expose those knobs yet; contexts that use
  them are skipped in the frontend suite.

## Phase 3 — Froggeric v22.5 (done, 2026-09-10)

The merge was a template content change, as planned; no C++ work was needed.

1. Fixture replaced: `tests/fixtures/frontend/froggeric_v225_chat_template.jinja`
   (upstream v22.5 plus our local no-dangling-intent tool rule, applied to both the
   XML and JSON instruction branches). The superseded v22 fixture is deleted.
2. Registered as the same identity: a digest update, plus
   `kFroggericV22DeployedTemplateDigest` so the artifact that is deployed today keeps
   loading until it is rebuilt from the v22.5 fixture.
3. All v22.1 -> v22.5 semantics come from the template: effort aliases and the
   default-medium resolution, merging of all leading system messages, the explicit
   reasoning-field variants (reasoning_content / thinking / reasoning) with lead-strip,
   the reworked tool-error tiering, single-newline multi-tool separation, non-thinking
   tool-prompt alignment, video_url, and JSON-payload truncation protection.
4. Our previous thinking-disabled replay adoption is now upstream behaviour: v22.5
   keeps the reasoning block on replay for every message whenever the reasoning is
   preserved or belongs to the current turn, so the block is no longer a local patch.

Verification:
- Raw engine parity at v22.5: 32 contexts byte-exact against the Python oracle,
  including six new contexts for the v22.5 features (multi-system merge, effort
  alias, reasoning-field variants, video_url, inline effort tags, non-thinking tool
  prompt).
- Frontend: 84 renders (3 registered templates x 28 contexts) byte-exact, plus the
  structured-output invariants.
- Froggeric prompt contract: 16/16 after updating two checks that pinned v22
  behaviour (replay now keeps the reasoning block; consecutive tool calls are
  separated by one newline).
- Scale: byte-identical to Python at ~200k tokens (807,647 bytes).
- Render time at ~200k tokens: 7.83 ms best (v22 was 5.90 ms, the retired
  hand-written renderer 5.18 ms) - about 0.2-0.4% of a 2-4 s prefill.

Standing caveat unchanged: upstream v22.5 extracts in-content thinking at the first
think-close occurrence; the deployed server-side tolerant parser remains the backstop
for already-emitted text-form calls.

Still open from Phase 2: the GPU-box build and e2e/A-B verification, the artifact
rebuild, and exposing the max_tool_*_chars / tool_call_format knobs.

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


## Architecture Optimization — Exploit Hybrid SSM+Attention Topology (2026-09-10)

### Discovery

Qwen3-27B QUASAR uses a **hybrid architecture** (`hybrid_topology.h`):
every 4th layer is full attention, the other 3 are GDN/SSM layers.

```
Layer:     0  1  2  3  4  5  6  7  8  ... 24 25 26 27
Type:      G  G  G  A  G  G  G  A  G  ...  G  G  G  A
```

For 28 total layers:
- **7 full-attention layers** (25%): need growing K/V cache (text KV)
- **21 GDN/SSM layers** (75%): need only fixed-size recurrent state (state image)

The GDN layers use a State Space Model (Mamba-style) architecture.
Each token's GDN state depends only on the previous token's state
(recurrent, O(1) per token). The state image (`StateImageHostLayout`)
captures this recurrent state as a **fixed-size** tensor (~150MB) that
does NOT scale with context length.

### What we currently store vs what we actually need

**Current spill (per checkpoint):**
- `text_kv` — attention K/V for 7 layers, scales with context (~6GB at 370k tokens)
- `backend_kv` — GDN replay records for 21 layers, scales with context (~6GB at 370k tokens)
- State image — GDN recurrent state, fixed ~150MB
- Total: ~12.5GB per checkpoint (text + backend), both scaling linearly

**What we actually need for turn_closure (forward generation from checkpoint):**
- `text_kv` — attention K/V for 7 layers (needed — attention layers must attend to past tokens)
- State image — GDN recurrent state (needed — SSM layers need their running state)
- `backend_kv` — **NOT NEEDED** — GDN replay records are only for mid-sequence branching,
  not for forward generation. The recurrent state already summarizes all history.

The backend KV (`GdnReplayRecordLayer`: conv, key, value, gate per token per layer)
stores per-token transition records that allow "replaying" GDN computation from an
arbitrary position. This is useful for branching from a mid-sequence checkpoint but
is pure overhead for the common case: appending tokens to the end (turn_closure).

### Three optimization levels

**Level 1 — Drop backend_kv from host spill (keep text_kv + state image):**

- Arena storage per checkpoint: ~6GB (text only) + ~150MB (state) = ~6.2GB
- vs current ~12.5GB → **2x more checkpoints fit in the same arena**
- Turn_closure: restore text_kv from host + restore state image → same speed as today
- Mid-sequence branching: not supported from these entries (would need re-prefill of GDN layers)
- Arena at 30GB: fits ~4-5 checkpoints instead of ~2-3
- Implementation: spill function skips backend_kv for entries beyond a threshold;
  add `text_only` flag to `HostKVSafetyNetEntry`

**Level 2 — Drop both text_kv and backend_kv (state image only):**

- This is what state-only fallback already does!
- ~150MB per checkpoint → effectively unlimited checkpoints in arena
- Turn_closure: restore state image + re-prefill 7 attention layers only
- Re-prefill cost: 25% of full model (7/28 layers) = ~4x faster than full re-prefill
- For 370k tokens: ~25s instead of ~100s (estimated, 7 attention layers only)
- Already partially implemented (state-only fallback), but currently treated as
  a failure mode rather than a deliberate storage strategy

**Level 3 — Don't store text_kv on device either (radical):**

- Device KV only holds 7 attention layers' worth of K/V (not 28 layers' text+backend)
- Same 12.8GB device KV budget → **4x larger context** (555k → ~2.2M tokens)
- Every turn_closure: restore GDN state (instant) + re-prefill 7 attention layers (25% compute)
- Re-prefill of 7 layers for 555k tokens: ~25s (vs 0.3s with full KV cache)
- Trade-off: 4x context capacity at the cost of ~25s per turn_closure
- This is the fundamental advantage of hybrid SSM+attention: the SSM layers
  have O(1) state, so only the attention layers' KV cache limits context length

### Impact on arena fragmentation (Finding A)

Level 1 directly addresses the fragmentation problem:
- Entries are ~6GB instead of ~12.5GB (half the size)
- Smaller, more uniform allocations reduce fragmentation
- 2x more entries fit in the same 30GB arena
- Fewer evictions needed → less churn → less fragmentation

### Correctness

GDN recurrent state (in the state image) is sufficient for correct forward
generation. The model produces identical outputs whether using:
- Full backend_kv (replay records) + text_kv, or
- State image (recurrent state) + text_kv

The replay records are a performance optimization for mid-sequence restore,
not a correctness requirement. The recurrent state is the authoritative
summary of all prior token processing.

### Implementation sketch

```
// New flag on HostKVSafetyNetEntry
bool text_only = false;  // true = no backend_kv stored, only text_kv + state

// In spill function: for entries beyond tier-1 threshold
if (entry_age > tier1_threshold) {
    // Skip backend_kv copy, only spill text_kv + state image
    entry.text_only = true;
    // No backend_allocations, no backend_page_count
}

// In restore function: check entry.text_only
if (entry.text_only) {
    // Restore text_kv from host (H2D)
    // Restore state image (already in state-only entry)
    // No backend_kv to restore — GDN uses recurrent state from state image
    // Schedule attention-layer-only prefill for the delta tokens
}
```

### Verification

- Correctness: outputs must be byte-identical between full-KV and text-only-KV
  paths (GDN recurrent state produces same results as replay records for forward gen)
- Performance: text-only turn_closure should be same speed as full-KV turn_closure
  (same H2D for text_kv, same state image restore)
- Arena: 2x more entries fit, fragmentation reduced
- Mid-sequence branching: document as unsupported from text-only entries
  (would need full re-prefill of GDN layers, which is correct but slow)


### Refined Analysis — Reasoning Block Shedding (from user feedback)

> **Attribution:** The reasoning-block shedding concept was proposed by
> Mark (tokenring.ai), who identified that `--preserve-reasoning` causes
> reasoning blocks to accumulate and consume the entire context budget,
> and suggested tagging KV entries by semantic chunk to selectively shed
> old reasoning while keeping responses.

The real use case is not just "drop backend_kv" but **semantic KV cache
shedding**: selectively discarding KV for old reasoning blocks that
accumulate with `--preserve-reasoning` (thinking=on).

#### The Problem

With thinking enabled, each turn generates 10k-50k tokens of reasoning
that is preserved in the conversation. After 10 turns, the context is
~350k tokens — **mostly old reasoning that the model already used** to
produce its response. The reasoning from turn 3 is irrelevant to turn 10,
but its KV cache sits there consuming device and host space.

#### The Insight

The hybrid SSM+attention topology makes shedding **almost free**:

- **GDN/SSM layers (75%):** The recurrent state (state image) already
  "digested" the reasoning tokens. Shedding them from the KV cache has
  **zero effect** on GDN forward generation. The state image captures
  the recurrent state *after* processing the reasoning, so the
  "memory" of that reasoning is baked into the fixed-size state.

- **Attention layers (25%):** Shed reasoning tokens are masked in
  attention (treated as padding). The model loses the ability to attend
  to old reasoning details but retains all responses, tool calls, and
  tool results. This is semantically meaningful: "forget the work,
  remember the conclusion."

#### Combined Impact

| Strategy | Tokens spilled | Arena per checkpoint | Reduction |
|---|---|---|---|
| Current (text+backend) | 370k | ~12.5GB | 1x |
| Drop backend_kv (Level 1) | 370k | ~6.2GB | 2x |
| Shed old reasoning | ~50k (responses only) | ~1.7GB | 7x |
| **Both combined** | **~50k** | **~0.85GB** | **14x** |

With 14x reduction, the 30GB arena could hold ~35 checkpoints instead
of ~2.5. Arena exhaustion, fragmentation, and bad_alloc would all
disappear for typical workloads.

#### Implementation

1. **Tagging:** The chat template already emits `  2. **KV metadata:** Tag page ranges with (turn_index, is_reasoning) metadata.
   Pages are grouped by logical chunk, not individually.

3. **Shed operation:** When context exceeds threshold (e.g., 80% of
   max_context), find old reasoning blocks (turn_index < current - N)
   and mark their KV pages as shed:
   - Device: free pages for reuse (targeted eviction of reasoning only)
   - Host: don't spill shed pages (smaller spill = less arena pressure)
   - GDN: no change (recurrent state unaffected)

4. **Attention masking:** Shed pages are skipped in attention computation
   (same as padding tokens). No architectural change needed — the model
   already handles masked positions.

5. **Shed threshold:** Configurable via CLI (e.g.,
   `--shed-reasoning-after N` — shed reasoning blocks older than N turns).
   Default: shed when context exceeds 80% of max_context.

#### Correctness

- GDN layers: **perfect** — recurrent state is identical
- Attention layers: **approximate** — same quality degradation as context
  window truncation (the model is trained to handle missing context)
- The response text (kept) captures the outcome of the reasoning, so
  the model can still reference conclusions from old turns
- This is strictly better than the current behavior (full eviction of
  entire continuations, which loses EVERYTHING including responses)


## Post-Thinking Sampler — Dynamic Sampling Parameter Switching

> **Attribution:** Based on [vLLM PR #52876](https://github.com/vllm-project/vllm/pull/52876),
> which proposes separate post-thinking sampling parameters for reasoning models.

### Problem

With `--thinking` enabled, a single set of sampling parameters is used for the
entire generation: both the reasoning block AND the final answer.

For Qwen3-27B, the optimal parameters differ substantially between phases:

| Phase | Optimal temp | Why |
|---|---|---|
| Reasoning (`...`) | 0.9–1.0 | High temp avoids reasoning loops and repetition |
| Final answer (after ``) | 0.2 | Low temp gives precise, reliable output and tool calls |

Using temp=0.9 throughout: reasoning is healthy but final answers are error-prone.
Using temp=0.2 throughout: final answers are clean but reasoning can loop/stall.

### ninfer's Existing Infrastructure

ninfer already has the building blocks:
- `SamplingMode::Thinking` vs `SamplingMode::NonThinking` presets (in `package.cpp`)
- `OutputChannel::Content` vs `OutputChannel::Reasoning` — the engine tracks
  which channel it's currently generating to
- `ModelSamplingDefaults` with separate `thinking` and `non_thinking` presets
- Thinking state tracking (`model_thinking_tokens`, thinking budget, `</think>` detection)

What's missing: the sampler is resolved **once** at request submission
(`resolve_sampling`) and used for the entire generation. There's no
dynamic switch when the model transitions from reasoning to content.

### Proposed Implementation

1. Add `SamplingPreset post_thinking` to `ModelSamplingDefaults`:
   ```
   constexpr ModelSamplingDefaults kQwen3_8Defaults{
       .thinking       = {.temperature = 1.0F, .top_k = 20, .top_p = 0.95F, ...},
       .post_thinking  = {.temperature = 0.2F, .top_k = 20, .top_p = 0.95F, ...},
       .non_thinking   = {.temperature = 0.7F, .top_k = 20, .top_p = 0.80F, ...},
   };
   ```

2. In the decode loop, when `` is detected (the engine already
   detects this for `OutputChannel` transitions), switch the active
   sampler from `thinking` to `post_thinking`.

3. CLI flags:
   - `--post-thinking-temperature 0.2`
   - `--post-thinking-top-p 0.95`
   - `--post-thinking-top-k 20`
   - Or a combined `--post-thinking-sampler temp=0.2,top_p=0.95,top_k=20`

4. API: `post_thinking` field in request options (same as vLLM PR).

5. Backward compatible: if no `post_thinking` preset is configured,
   behavior is unchanged (thinking sampler used throughout).

### Why This Matters for Our Setup

Our production server runs with `--temperature 0.9 --top-p 0.95 --top-k 20`
for the entire generation. The vLLM PR author tested specifically with
Qwen 3.8 27B and found:
- temp=0.9 during reasoning: correct, avoids loops
- temp=0.2 after reasoning: significantly fewer errors in answers and tool calls

This is a small, clean change that fits naturally into ninfer's existing
thinking/non_thinking architecture. The engine already knows when it
transitions from `OutputChannel::Reasoning` to `OutputChannel::Content`.

## Deployment verification (strix, 2026-09-11)

Both changes are deployed together as `4bab2c35` (`b432a107` jinja cutover +
`3c0b4dc5` tolerant parser cherry-picked onto it). The prod tree
`/home/zenz/ninfer` is reset to that commit and rebuilt there.

- Build: `ninfer-serve` plus `ninfer_tool_call_parser_test`,
  `ninfer_jinja_template_test`, `ninfer_qwen3_6_froggeric_render_test` and
  `ninfer_qwen3_6_jinja_frontend_render_test` all build and run clean on the box:
  parser `ok`, engine parity `ok (33 contexts)`, froggeric contract all checks,
  frontend `ok (84 renders, 15 skipped)`.
- Live service: the previous instance had been wedged for about six hours
  (`materializing=1` with every other counter at zero, `/health` and inference
  both returning an empty reply). That predates this work and is independent of it
  (the process was the old binary, 13 h uptime). A restart cleared it and put the
  new build live: `/health` 200 after 18 s, model alias `qwen3.8-27b` serving.
- `~/.config/ninfer.conf` now points `--chat-template` at
  `froggeric_v225_chat_template.jinja` (backup kept beside it), because the v22
  fixture is deleted by this change.
- Tool-leak A/B battery, run against the new build: 20/20 requests HTTP 200, all
  four conditions returned structured tool calls, zero marker text in content, and
  `D_notools` (the pi-plan incident shape) is RECOVERED 5/5. No transient
  materialization 500s in this run.

Regression this deploy surfaced: the first attempt reset the prod tree to the jinja
branch, which does not contain the deployed tolerant parser, so `D_notools` leaked
5/5. The parser fix is now cherry-picked onto the jinja branch; anyone deploying
from a feature branch must check `git merge-base --is-ancestor <deployed> <target>`
first instead of assuming the tree already carries it.

Still open:
- The qwen3.8 artifact embeds the old template; rebuilding it needs the BF16 source
  checkpoint, which is not on this Mac. Until then `kFroggericV22DeployedTemplateDigest`
  stays in the registry. The live deployment overrides the embedded template with
  `--chat-template`, so it is not on the serving path today.
- `ninfer_qwen3_6_frontend_test` cannot run on strix: it wants a checkpoint under
  `/home/neroued/models/...` that does not exist on that box. Environmental, and it
  fails before rendering.
- The template knobs the frontend does not expose (`tool_call_format`,
  `auto_disable_thinking_with_tools`, `max_tool_arg_chars`, `max_tool_response_chars`)
  remain covered by the raw engine suite only.
