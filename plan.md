# Host-KV context-cache — open optimization work

This plan tracks the open host-KV / context-cache optimization work for the
Qwen3.6/3.8 27B hybrid (SSM + attention) targets. Two earlier efforts that lived
in this file are now merged and are no longer tracked here (see "Shipped" below);
what remains is the atomic-unit invariant plus the still-open arena, topology and
cache-shedding work.

The governing invariant for all of it is the standing plan below.

## Standing plan — a cache unit is {KV + state}, atomically (invariant for every phase)

A context-cache unit is **the attention KV and the GDN recurrent state together**.
It is captured, retained, evicted and restored as ONE indivisible unit. Never store,
evict, retain, budget or restore either half on its own. This overrides any earlier
text in this plan that describes them as separately storable items.

Why (hybrid topology): the KV covers only the attention (softmax) layers — 7 of 28 in
the Qwen3.6-27B configuration this plan targets, with the other 21 being GDN/SSM —
while the ~150 MB state image carries the GDN recurrent state. The split is
model-specific (the Qwen3.8-27B production configuration is 16 attention of 64); the
invariant is not. Resuming at position N requires BOTH halves, and neither half is
independently useful:

- **KV without state cannot resume**, and is not "cheaper than losing both": the
  attention K/V at position p depends on the GDN layers' output at p, so attention
  KV is not independently recomputable. Any recompute is a full prefix pass, which
  rebuilds the GDN state as a by-product and regenerates the KV being kept.
- **State without KV can only resume while that KV is still device-resident**, so it
  is not a durable unit and must not be a stored/retained form.

Therefore:

- a partially-retained unit is a **bug, not a degraded cache**: it wastes host memory
  and it is a correctness hazard, because prefix matching can select it and then
  restore with a missing half, silently producing a wrong continuation;
- eviction removes a **whole unit** — there is no "state-only" or "KV-only" victim class;
- the host budget accounts a unit's **full size** (KV pages + state image) as one number;
- "which half is expensive" is never an eviction criterion, because the halves have no
  independent value.

- eviction is **cost-aware, not strict LRU**: re-prefill cost scales with the unit's
  context length, so reclaim the SMALLEST complete units first and hold on to recent
  large caches — re-prefilling a big context is far more expensive. Recency is only a
  tie-breaker between units of comparable size.

Consequences for the host-KV safety net: entries are atomic; never create a partial
entry; reclaim any pre-existing partial entry first; then reclaim by re-prefill cost
(smallest unit first, recent large units kept), with recency as the tie-breaker.

## Shipped (merged — see git history and maintainer docs)

The jinja and post-thinking efforts that previously occupied this file are done:

- **Jinja chat-template engine + froggeric v22.5** (PR #4, `cb535944`): the
  vendored `wangzhaode/jinja.cpp` engine now genuinely executes `--chat-template`;
  the ~800-line hand-written renderer is gone; every parity gate is byte-exact
  against the Python jinja2 oracle (engine + frontend suites, 16/16 froggeric
  contract); deployed on strix with the A/B tool-leak battery green.
- **Post-thinking sampler + atomic {KV + state} cache unit** (PR #6, `ae8eb23e`):
  the `post_thinking` sampling phase (CLI + HTTP) and the atomic-unit invariant
  below, enforced in `host_kv_safety_net.h` (half units refused, cost-aware
  smallest-unit-first eviction, one shared host budget). Measured: quality-neutral
  on BFCL v4 tool calling (1240 paired samples, p = 0.79) — a reproducibility /
  reliability knob, not a quality one; full study in
  `docs/maintainer/post-thinking-temperature.md`.

Still open from the jinja line (carried, not on the serving path):
- the qwen3.8 artifact still embeds the pre-v22.5 template; rebuilding it needs the
  BF16 source checkpoint (not on the Mac). `kFroggericV22DeployedTemplateDigest`
  keeps the currently deployed digest accepted until the rebuild; live serving
  overrides the embedded template via `--chat-template`, so it is not on the path.
- the frontend does not yet expose `tool_call_format`, `auto_disable_thinking_with_
  tools`, `max_tool_arg_chars`, `max_tool_response_chars`; contexts using them are
  covered by the raw engine suite only.

## Post-Merge Findings — Arena Fragmentation & Eviction Strategy (2026-09-10)

> **Status (2026-09-12):** the state-only FALLBACK failure mode is gone — PR #6 made
> the cache unit atomic (no state-only entries), and eviction is now cost-aware,
> smallest-unit-first, looping until the incoming unit fits the shared host budget
> (`host_kv_safety_net.h::retain_state_capture`). That resolves Finding B and removes
> the "state-only fallback loses KV" tail of the incident.
>
> **Status (2026-09-13):** Finding A is fixed by **arena compaction**: the
> scatter-gather path is removed, and a single-extent allocation that fails despite
> sufficient total free now triggers `HostKVArena::compact()` (live extents relocate
> to the front, free space becomes one contiguous extent) before the allocation
> retries. The `/stats` fragmentation metrics below are implemented. **Still open:**
> the real-scenario evaluation on the GPU box.

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

**Root cause (corrected, verified against the deployed tree):** This is NOT a
missing-coalescing bug. `HostKVArena::insert_free_extent` has coalesced
adjacent free extents on every free since the safety net landed (2026-08-24),
and the journal evidence above was collected from a build that includes it.
The real mechanism is external fragmentation: large live spill entries
(6-12.5GB each) interleaved with small live entries partition the free space
into non-adjacent extents, and coalescing can only merge adjacent free
regions — it cannot merge across a live block. When no single extent is large
enough, the single-allocation path fails despite sufficient total free. The
scatter-gather workaround then allocates across multiple extents; when those
allocations are freed they add more live regions, so under churn the loop is
self-reinforcing (Finding A's feedback chain holds, driven by live-entry
interleaving rather than a missing coalesce).

**Fix (implemented, 2026-09-13): compaction.** `HostKVArena::compact()`
relocates every live allocation into a contiguous block at the front of the
arena so all free space merges into one trailing extent. The spill path
triggers it reactively: a single-extent allocation that fails despite
sufficient total free syncs the transfer stream (no in-flight copies may
reference arena memory), compacts, and retries — after which the retry
always succeeds, because the free space is one extent of the total free.
The scatter-gather path (`allocate_multi`) is removed: with compaction it is
unreachable (total free < need is a capacity failure, not fragmentation).
Options 2 (uniform entries via dropping backend KV) and 3 (buddy/slab) are
not pursued: option 2's premise was wrong (see the corrected Architecture
Optimization section — the "backend KV" is the MTP module's KV, not droppable
replay records), and option 3 is a larger rewrite for a problem compaction
already solves.

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

### Fragmentation measurement — extend the existing `/stats` endpoint

No new endpoint: `/stats` (`src/serve/stats_json.cpp`) already exposes
`host_kv_capacity_bytes` / `host_kv_occupied_bytes`. Add fragmentation
fields so the fix is measured instead of log-grepped.

Instantaneous (into `MemorySummary`):
- `host_kv_free_bytes`
- `host_kv_largest_free_extent_bytes`
- `host_kv_free_extent_count`
- `host_kv_fragmentation_ratio` = largest extent / total free (1.0 = one
  contiguous extent; lower = more shredded). The arena already tracks
  `free_extents_`; this is a small read-only accessor.

Cumulative counters (plumbed into `RuntimeStats`, exposed under the
`host_kv` group of `/stats`):
- `host_kv_single_alloc_failures` — single-extent allocations that failed
  despite sufficient total free (the fragmentation signature; in the spill
  path each is immediately repaired by a compaction)
- `host_kv_compactions` — arena compactions that merged the free space
- `host_kv_evictions` — whole units evicted from the safety net

(The section's original `host_kv_state_only_fallbacks` counter is obsolete:
PR #6 removed the state-only fallback path. The original
`host_kv_scatter_gather_*` counters are obsolete too: the scatter-gather
path was removed when compaction landed.)

Land the metrics first, before the allocator fix: they are additive and
give us the "before" baseline immediately.

### Verification after fix (quantified against the metrics above)

- every `host_kv_single_alloc_failures` increment in the spill path is
  immediately followed by a compaction and a successful allocation — no
  fragmentation-caused spill failures (incident baseline: 144 failures,
  278 fallbacks)
- no multi-extent allocations at all — the scatter-gather path is removed
- evict-smallest frees enough space for the new allocation in one pass
  (`host_kv_evictions` does not climb to drain the safety net)
- `host_kv_fragmentation_ratio` stays at or above 0.9 during the evaluation
  run (it is 1.0 right after a compaction; it only drops when interleaved
  frees split the free space again)

### Evaluation — demonstrate the defrag fix in a real scenario

Run the same pressure workload against pre-fix and post-fix builds and
compare the counters above.

Workload (reproduces the production shape that triggered the incident):
- one long multi-turn thinking session, 371k tokens / ~1000 messages
  (drives repeated 6-12.5GB checkpoint spills), plus several small
  interleaved sessions (~5k tokens each) so the arena holds mixed-size
  live entries — the exact allocation pattern that produced the 144
  single-alloc failures and 278 FALLBACKs.
- 30GB arena, same seed, thinking with `--preserve-reasoning` on.

Method:
1. Pre-fix build (metrics only): run the workload, poll `/stats` through
   the run; record worst-case `host_kv_fragmentation_ratio` and the four
   cumulative counters.
2. Post-fix build: identical run.
3. Compare. Pass = all four verification criteria hold in the post-fix run
   and fail (as in the incident) in the pre-fix run.

Configurations: pre-fix (incident build) vs post-fix. The pre-fix baseline
is the incident journal data (144 single-alloc failures, 278 fallbacks); a
fresh pre-fix run is optional.

Fast regression (implemented): `tests/test_kv_cache.cpp::exercise_compaction`
replays the interleaved allocate/free pattern (eight 8-page blocks, two
interleaved frees), asserts the 12-page allocation fails despite sufficient
total free, compacts, asserts the free space is one extent with the data
intact, and asserts the allocation then succeeds. Runs with the other
`ninfer_kv_cache_test` checks (needs the CUDA driver for the pinned buffer,
like the rest of that target).

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
- `text_kv` — attention K/V for the attention layers, scales with context
- `backend_kv` — speculative-backend KV (the MTP module's causal-attention KV when
  `--spec mtp`, or the DFlash context KV), scales with context
- State image — GDN recurrent state, fixed ~150MB

> **Correction (2026-09-13, verified in code):** the original text of this section
> described `backend_kv` as "GDN replay records" that are "only for mid-sequence
> branching" and therefore droppable. That premise is **wrong**. In this codebase the
> "backend KV" is the speculative backend's KV: `backend_kv_cache()` returns
> `decoder->mtp_cache()` for `--spec mtp` (the production configuration), and the MTP
> ops run **causal attention over the entire MTP KV prefix**
> (`mtp_forward_batch` / `mtp_forward_ar_step`, `CausalAttentionExecutionEnvelope{visible,
> visible}`). The GDN replay records the original text meant to drop are not in the
> host spill at all — they live in the fixed `replay_records` arena plus the state
> image, and are only consumed by the Fold of the current speculative round's candidate
> window. Dropping the MTP KV from the host spill would therefore permanently degrade
> MTP drafting after any safety-net restore (the restored prefix's MTP KV would be zero
> and never recovered short of a full re-prefill). Level 1 below is rejected on this
> basis. The size estimates in this section (backend_kv ≈ text_kv ≈ 6GB) are also
> suspect: the MTP module is a small extra layer, so its KV is much smaller than the
> main attention KV.

### Three optimization levels

**Level 1 — Drop backend_kv from host spill (keep text_kv + state image): REJECTED (2026-09-13).**

> **Rejected — wrong premise.** This level assumed `backend_kv` holds droppable GDN
> replay records. It does not: it is the MTP module's causal-attention KV (see the
> correction above). Dropping it from the host spill permanently degrades MTP drafting
> after any safety-net restore — the restored prefix's MTP KV would be zero and never
> recovered short of a full re-prefill, which is exactly what the safety net exists to
> avoid. Production runs `--spec mtp --draft-tokens 5`, so the cost is real. The
> "2x more checkpoints" goal is also not achieved: the MTP KV is a small extra layer,
> far smaller than the main attention KV that dominates the unit's size. The
> fragmentation goal this level was meant to serve is instead met by arena compaction
> (Post-Merge Findings, Finding A).

- (Original proposal, retained for the record:) spill function skips backend_kv for
  entries beyond a threshold; add `text_only` flag to `HostKVSafetyNetEntry`.

**Level 2 — Drop both text_kv and backend_kv (state image only):**

> **SUPERSEDED by the standing plan (atomic {KV + state} unit).** This level proposes
> storing the state image WITHOUT its attention KV. Per the invariant that is not a
> valid cache unit: the attention K/V for a position depends on the hidden state produced
> by the preceding GDN layers, so it is not recomputable in isolation — the premise here
> that only the attention layers need re-prefilling must be re-derived before this level
> can be considered. Do not implement this level as written.

- This is what state-only fallback already does!
- ~150MB per checkpoint → effectively unlimited checkpoints in arena
- Turn_closure: restore state image + re-prefill 7 attention layers only
- Re-prefill cost: 25% of full model (7/28 layers) = ~4x faster than full re-prefill
- For 370k tokens: ~25s instead of ~100s (estimated, 7 attention layers only)
- Already partially implemented (state-only fallback), but currently treated as
  a failure mode rather than a deliberate storage strategy

**Level 3 — Don't store text_kv on device either (radical):**

> **SUPERSEDED by the standing plan (atomic {KV + state} unit).** This level proposes
> storing the state image WITHOUT its attention KV. Per the invariant that is not a
> valid cache unit: the attention K/V for a position depends on the hidden state produced
> by the preceding GDN layers, so it is not recomputable in isolation — the premise here
> that only the attention layers need re-prefilling must be re-derived before this level
> can be considered. Do not implement this level as written.

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
| Shed old reasoning | ~50k (responses only) | ~1.7GB | 7x |

(The original table's "Drop backend_kv (Level 1)" and "Both combined / 14x"
rows depended on the rejected Level 1 — see the correction above — and are
removed. Shedding alone is the remaining lever.)

With 7x reduction, the 30GB arena could hold ~18 checkpoints instead of
~2.5. Arena exhaustion and bad_alloc would disappear for typical workloads;
fragmentation is handled separately by arena compaction.

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

> **Reconciled with the standing plan:** the strip-invariance result below concerns the
> GDN recurrent state, which is position-independent — that part stands. It does NOT
> license storing the state WITHOUT its attention KV: "arena stores state image only" is
> not a valid cache unit. A strip-thinking design must retain the attention KV and the
> GDN state together; what may legitimately change is which *positions* the retained KV
> covers, not whether the KV is kept.

## Strip-Thinking-Cache — Don't Cache Reasoning KV (from vLLM/SGLang)

> **Attribution:** Based on [vLLM PR #39806](https://github.com/vllm-project/vllm/pull/39806)
> and [SGLang PR #23315](https://github.com/sgl-project/sglang/pull/23315).
> Both identified the same problem and implemented the same solution independently.

### The RoPE Position Problem

When a client strips reasoning blocks from subsequent turns (the standard
convention per DeepSeek/OpenAI API docs), the prefix cache breaks for a
fundamental reason: **RoPE position shift**.

Answer tokens after thinking have RoPE positional encodings computed at:
```
positions [input_len + thinking_len, input_len + thinking_len + answer_len]
```

In the next turn (without thinking), those same answer tokens appear at:
```
positions [input_len, input_len + answer_len]
```

The positions don't match. RoPE encodings are baked into the attention KV
cache. The answer KV is **permanently invalid** after stripping thinking —
not just a prefix-match failure, but a numerical correctness issue.

This applies to YaRN-scaled positions too: YaRN is a scaling factor on
RoPE positions. The shift is proportional. The problem exists with or
without YaRN; YaRN doesn't make it better or worse.

### What vLLM and SGLang Do

Both engines added an opt-in flag (vLLM: `cache_reasoning_tokens=False`,
SGLang: `--strip-thinking-cache`). On request completion with reasoning
tokens detected:

1. **Prompt prefix blocks** → normal cache path (retain hash for prefix matching)
2. **Thinking + answer blocks** → immediately evicted (hash removed, blocks freed)

Answer tokens are also stripped because their RoPE positions are mismatched.
Both engines measured significant improvements:
- SGLang: +6% cache hit rate, -1s TTFT (QwQ-32B, 2×B300)
- vLLM: eliminates 1.3–1.6 GB dead branches per turn

### ninfer's Advantage: Hybrid Architecture

The RoPE position problem only affects the **7 attention layers** (25% of
the model). The **21 GDN/SSM layers** (75%) don't use RoPE at all —
confirmed in `gdn_mix()` which has no position/RoPE parameters. GDN uses
`conv1d` + `gated_delta_net` (SSM recurrence), both position-independent.

This means:
- **Attention KV (text_kv, 7 layers):** NOT reusable after stripping thinking
  (RoPE position shift). Must re-prefill.
- **GDN recurrent state (state image, 21 layers):** Fully reusable regardless
  of thinking token stripping or YaRN. The recurrent state captures all
  history in a fixed-size tensor.

### Proposed Implementation

Add `--strip-thinking-cache` CLI flag (opt-in, default off).

When enabled, on request completion with reasoning tokens detected:

1. **Spill to host KV:** Only spill text_kv for the prompt prefix (before any
   thinking block). Don't spill thinking + answer text_kv (RoPE-invalid).
   Don't spill the thinking + answer range of backend_kv either — it is the
   MTP module's KV (see the correction in the Architecture Optimization
   section), and MTP causally attends over that range, so it has the same
   position-binding problem; the cost is degraded MTP drafting until the
   range is re-prefilled, which is acceptable for stripped/cancelled turns.

2. **State image:** Always preserve (GDN recurrent state is position-independent
   and fully reusable).

3. **Device KV:** Free thinking + answer KV pages immediately for reuse
   (same as vLLM/SGLang immediate eviction).

4. **Next turn restore:** Restore GDN state from state image (instant) +
   re-prefill 7 attention layers for the prompt prefix + answer + new message.
   Re-prefill cost: 25% of full model (7/28 layers).

### Impact

| Scenario | Without strip | With strip |
|---|---|---|
| Normal turn (no cancellation) | turn_closure (0.3s) | Re-prefill 7 layers (~25% compute) |
| Cancellation turn (reasoning stripped) | Root prefill ALL 28 layers (124s) | Re-prefill 7 layers only (~31s) |
| Arena storage per checkpoint | ~12.5GB (text+backend) | ~0.15GB (state image only) |
| Arena capacity (30GB) | ~2.4 entries | ~200 entries |

The trade-off: normal turns become slightly slower (re-prefill 7 layers
instead of instant turn_closure), but cancellation turns become 4x faster
(31s instead of 124s), and arena pressure drops by 80x.

For interactive coding sessions where cancellation is common, this is a
net win. For batch/streaming sessions without cancellation, the default
(off) preserves the current fast turn_closure behavior.

### Combined with Reasoning Block Shedding

The `--strip-thinking-cache` flag and the reasoning block shedding plan
are complementary:

- **strip-thinking-cache:** Don't cache thinking+answer KV at all. Handle
  the client-stripping case (cancellation). Arena stores state image only.
- **Reasoning block shedding:** Keep thinking in the prompt (preserve_thinking=on),
  tag and shed old reasoning KV in the engine. Handle the accumulation case.
  Arena stores text_kv for responses only.

Both reduce arena pressure. Both exploit the GDN/SSM advantage (75% of
layers unaffected by RoPE position shift). They can be used together or
independently depending on the workload.
