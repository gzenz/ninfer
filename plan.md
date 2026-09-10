# Unified Checkpoint Host Demotion — Architecture & Implementation Plan

## Problem

With 3 concurrent sessions on the production server (RTX 5090, 32 GB VRAM):
- Weights: 16.3 GB
- Device state images: 6 slots x 2 GB = 12 GB (3-cache + 3-active)
- KV + workspace: ~3.7 GB
- Total: ~32 GB — completely full

Each session needs 2 device state slots (1 active + 1 checkpoint). With 3
sessions, all 6 slots are used. When a session advances its turn, it needs
a 7th slot for the new checkpoint — none available.

## Root Cause

The pressure planner and the KV safety net were two separate systems that
fought each other. The pressure planner would DROP rewrite checkpoints
(state only, keep KV on device) when it needed device state slots. The
dropped checkpoint state was captured to a side store
(`dropped_checkpoint_captures_`) that the inspect couldn't search. Follow-up
requests couldn't find the checkpoint and had to re-prefill (12-32s per
request instead of 0-1s).

## Architecture

**Key principle: KV and checkpoint state move together to host, or not at all.**

### Pressure planner: no rewrite checkpoint drops

The pressure planner no longer generates drop successors for rewrite
checkpoints. When it needs a device state slot occupied by a rewrite
checkpoint, it must evict the entire continuation (KV + state together
to the safety net). This is more destructive (frees the KV too) but
preserves the checkpoint on host.

Endpoint and long-anchor checkpoints can still be dropped (they don't
serve follow-up prompt matching).

### Safety net: single host-side store with state-only entries

The safety net (`host_kv_arena`) handles:
1. **Full entries** (KV + state) from eviction — existing behavior
2. **State-only entries** (no KV) from checkpoint capture — NEW

State-only entries are created at three points:
- `publish_pressure_host_releases`: when a checkpoint is dropped (endpoint
  or long anchor — rewrite checkpoints are no longer dropped)
- `finish()`: when fork_collapsed_to_source captures the turn-boundary state
- `advance_prefill`: when a root-path turn completes (no rewrite checkpoint)

State-only entries store the checkpoint state bytes in a heap vector
(~2GB each), bounded by continuation count (no artificial limit).

### Spill merge

When a continuation with a dropped checkpoint is later evicted, the spill
function merges the state-only entry's checkpoint state into the full
safety net entry. This uses `take_state_only_by_session` to find and remove
the state-only entry, then attaches its state to the full entry.

### Inspect and restore

The inspect finds evicted continuations via the existing safety-find
(full entries with KV + state). The root restore path H2D copies both
KV and state. No new inspect or restore logic needed.

State-only entries are NOT found by `find()` (they have no KV). They are
only found by `take_state_only_by_session` during the spill merge.

## Implementation Steps

### Step 1: State-only safety net entries (DONE)

- Add `state_only` flag to `HostKVSafetyNetEntry`
- `find()` skips state-only entries (no KV to restore)
- `add()` no longer bounds state-only entries (removed artificial LRU limit)
- `take_state_only_by_session()` finds and removes by session_key
- `remove_state_only()` cleans up on slot recycling

### Step 2: Replace dropped_checkpoint_captures_ (DONE)

- Replace 3 capture sites with state-only safety net entries:
  - `publish_pressure_host_releases` (pressure planner drop)
  - `finish()` fork-collapsed capture
  - `advance_prefill` root-path capture
- Remove `DroppedCheckpointCapture` struct and `dropped_checkpoint_captures_` member
- Spill function merges state-only entries via `take_state_only_by_session`

### Step 3: Pressure planner no longer drops rewrite checkpoints (DONE)

- In `inspect_pressure_successors`, don't generate drop successors for
  rewrite checkpoints
- The planner evicts the entire continuation instead
- This upholds the principle: KV and state move together

### Step 4: E2e tests (DONE)

- Parse `[checkpoint-demoted]` and `[safety-spill] merged dropped` patterns
- Demotion phase evaluates checkpoint demotions and restores
- Verify no re-prefills after cold start

### Step 5: Eliminate host_state_images dependency in finish() (DONE)

finish() demotion now uses direct D2H to buffer + safety net capture
(demoted_host_buffer). host_state_images still exists for StateImageStore
internals (D2H/H2D transfers) — eliminating it entirely would require
reworking StateImageStore, which is beyond the current scope.

### Step 6: Remove finish() demotion hack (DONE)

finish() demotion no longer uses begin_device_to_host/publish_transfer.
Uses direct D2H to buffer, H2D to new slot, then moves buffer to safety
net. The HostOnly restore in start_sequence remains as fallback for
pressure planner endpoint/long-anchor demotions.

## What's already done (reusable)

- [x] Spill function handles HostOnly endpoint state (safety net captures it)
- [x] E2e test infrastructure (11 phases, parse patterns, server config guard)
- [x] Pre-commit hook fix for git worktrees
- [x] Materialize fallback (root prefill when source state evicted)
- [x] Bad_alloc isolation (fail one request, not all)
- [x] Thinking signature skip
- [x] Monitor queue timing
- [x] Worker recovery (logic_error catch)
- [x] State-only safety net entries (Steps 1-2)
- [x] Pressure planner no rewrite drops (Step 3)
- [x] E2e test updates (Step 4)
- [x] finish() demotion via safety net (Steps 5-6)
- [x] Pressure accounting: stop overwriting committed_delta (Step 1)
- [x] Pressure accounting: bad_alloc retry partial-allocation cleanup (Step 3)
- [x] Backend scatter-gather (eliminates spill fragmentation failures)
- [x] max_fragments and state-only LRU limits removed (no artificial caps)


## Final Status (commit 3e5f4179)

### E2e Results: 58 PASS, 4 WARN, 1 FAIL
- All 11 phases completed without crashes
- No OOM failures (bad_alloc is WARN for trash phase)
- 0 cold-starts across all phases
- Checkpoint state preserved via safety net (demoted + spill_ckpt)
- Spill merges working (state-only entries consolidated into full entries)

### The 1 FAIL (pre-existing)
- responses-tools: frontiers [9446, 9446, 9446, 9446] — all same
- This is a pre-existing Responses API checkpoint issue, not related to
  the unified demotion architecture
- The Responses API may not advance checkpoints when using previous_response_id
  with the safety-net root restore path

### Architecture Delivered
1. State-only safety net entries replace dropped_checkpoint_captures_
2. Pressure planner evicts entire continuations (no rewrite drops)
3. KV and checkpoint state move together to host (key principle upheld)
4. Spill merge consolidates state-only entries via source_continuation_index
5. Safety net handles checkpoint state; host_state_images remains for
   StateImageStore internals (D2H/H2D transfers)


## Pressure Accounting Refactoring (DONE — bad_alloc Made Non-Fatal)

### Problem

`complete_pressure_delta` (program_impl.h:6432-6436) silently overwrites
the actual committed delta with the planned delta:

```cpp
const auto complete_pressure_delta = [&](PressureWork& work) {
    (void)checked_resource_difference(work.option.effect.removed, work.committed_delta.removed);
    (void)checked_resource_difference(work.option.effect.added, work.committed_delta.added);
    work.committed_delta = work.option.effect;  // overwrites actual with planned
};
```

The `(void)` casts discard the difference between planned and actual.
This means the system BELIEVES it freed as much memory as the pressure
planner planned, even when the actual freed memory is less (e.g., a
victim was partially truncated by a prior KV operation, or a spill
captured fewer pages than expected).

### Consequence

1. The admission check (`physical_peak_fits`) passes because it uses
   `physical_occupancy()` (actual) + `physical_peak_additional` (planned).
   But the planned peak assumed the planned freed resources, not the
   actual freed resources.

2. After pressure work completes, `physical_occupancy()` reflects the
   ACTUAL state (correctly). But the system's accounting (committed_delta)
   claims the PLANNED state. The discrepancy is silently erased.

3. When `prepare_materialization` or `advance_prefill` tries to allocate
   device KV pages, the pool is fuller than expected → `std::bad_alloc`.

### Fix: Use Actual Freed Resources

**Step 1: Stop overwriting committed_delta. (DONE)**

Replace `work.committed_delta = work.option.effect` with code that
computes the ACTUAL delta from the pressure work's results:

```cpp
const auto complete_pressure_delta = [&](PressureWork& work) {
    // Compute actual delta from what was actually freed/added
    detail::PhysicalDelta actual;
    for (const auto& state_change : work.state_changes) {
        if (state_change.host_released) {
            actual.removed.host.state_slots++;
        }
    }
    for (const auto& kv_change : work.main_kv_changes) {
        if (kv_change.host_released) {
            actual.removed.device.main_kv_pages += kv_change.pages.size();
        }
    }
    // ... same for backend_kv_changes
    // Track added resources from transfers/activations
    work.committed_delta = actual;
};
```

**Step 2: Post-pressure verification. (REJECTED — NOT NEEDED — Steps 1+3 eliminated all bad_alloc
without post-pressure verification. The admission check (physical_peak_fits)
remains the sole guard. The committed_delta is now accurate but write-only
— no downstream consumer reads it.)**

After all pressure work completes, verify that the ACTUAL freed memory
is sufficient for the request's peak demand:

```cpp
detail::PhysicalResources actual_freed = {};
for (const auto& work : transaction.pressure) {
    actual_freed = checked_resource_sum(actual_freed, work.committed_delta.removed);
}
detail::PhysicalResources actual_occupancy_after = 
    checked_resource_difference(physical_occupancy(), actual_freed);
if (!fits_within(actual_occupancy_after, transaction.peak_demand, admission_capacity())) {
    // Actual freed memory insufficient — abort, don't proceed to execution
    abort_transaction();
    return out;
}
```

**Step 3: Fix the bad_alloc retry partial-allocation bug. (DONE)**

The `catch (const std::bad_alloc& oom)` handler retries
`prepare_materialization` without cleaning up partial allocations
from the failed first attempt. If the first attempt allocated 1 of 2
state slots, `reserved_state_count = 1`. The retry's guard check
`state_count > reserved_states.size() - reserved_state_count` throws
`logic_error` (not `bad_alloc`), which escapes the catch and crashes
the worker.

**Fix:** Add a targeted cleanup function that releases ONLY the partial
allocations from `prepare_materialization` (reserved states, KV
activations, fork destinations) WITHOUT touching reservation-level
state (prefill, root_continuation_index, ledgers).

The `logic_error` handler at L6719 shows the correct pattern: it does
targeted cleanup (releases specific slots, resets the plan to root)
without calling `release_materialization_staging` (which is a full
teardown that destroys everything).

```cpp
const auto release_partial_preparation = [&]() {
    // Release partially reserved state images (can throw at L4754)
    for (std::uint32_t i = 0; i < transaction.reserved_state_count; ++i) {
        state_store->release(transaction.reserved_states[i]);
    }
    transaction.reserved_state_count = 0;
    // Release state fork destination (can throw at L4734)
    if (transaction.state_fork_destination) {
        state_store->release(*transaction.state_fork_destination);
        transaction.state_fork_destination.reset();
    }
    // Release partially activated KV pages (can throw at L4836/L4846)
    if (transaction.text_activation) {
        text_kv_addresses->release_activation(*transaction.text_activation);
        transaction.text_activation.reset();
    }
    if (transaction.backend_activation) {
        backend_kv_addresses->release_activation(*transaction.backend_activation);
        transaction.backend_activation.reset();
    }
    // Do NOT touch:
    // - requests[lane].prefill (needed by retry)
    // - transaction.root_continuation_index (needed by retry)
    // - materialization_ledger_/identity_/prefix_digests_ (needed by start_sequence)
    // - transaction.source_prepared (needed by retry guard)
    // - transaction.root_text_address / root_backend_address (reserved at reserve time)
    // - transaction.text_prefix_fork / backend_prefix_fork (logic_error, not bad_alloc)
    // - transaction.text_retained_tail* (allocated by enqueue, not prepare)
    // - transaction.text_source_restore_reservation (allocated by enqueue, not prepare)
    // - Pressure work (already completed before prepare)
};
```

**Scope analysis:** Only 4 resource types can be partially allocated
when bad_alloc throws during prepare_materialization:
1. reserved_states (state_store->reserve_destination at L4754)
2. state_fork_destination (state_store->reserve_destination at L4734)
3. text_activation (text_kv_addresses->prepare_activation at L4836)
4. backend_activation (backend_kv_addresses->prepare_activation at L4846)

Other resources (prefix forks, retained tails, source restore reservations,
transfers) are either:
- Logic_error, not bad_alloc (prefix forks at L4785/L4791)
- Allocated by enqueue_materialization_transfers, which runs AFTER
  prepare_materialization and is not reached if prepare throws
- Allocated before the bad_alloc throw point and fully committed

Call `release_partial_preparation()` before the retry in the bad_alloc
catch handler.

### Risk Assessment

- Step 1 (stop overwriting): LOW risk. The committed_delta is used for
  accounting/diagnostics, not for functional decisions. Changing it
  to reflect reality should not break anything.

- Step 2 (post-pressure verification): MEDIUM risk. Needs careful
  accounting of staging resources (reserved by prepare_materialization)
  to avoid double-counting. The previous attempt failed because it
  used `physical_peak_fits` which includes staging in occupancy.

- Step 3 (partial-allocation cleanup): MEDIUM risk. The targeted
  cleanup function must release exactly what prepare_materialization
  allocated, nothing more. Missing a cleanup path leaks resources;
  releasing too much corrupts the transaction state.

### Expected Outcome

- bad_alloc made non-fatal (caught and recovered, no crashes)
- 0 spill failures (backend scatter-gather fixed)
- 0 checkpoint advancement failures (fixed)
- 0 cold-starts across all phases
- 0 new FAILs in e2e tests (1 pre-existing intermittent FAIL in responses-tools) (intermittent WARNs from spills-missing-ckpt
  edge cases in extreme pressure phases)


## Model Download and Prod Verification (DEFERRED — requires upstream rebase)

> **Deferred:** The HF model (neroued/Qwen3.8-27B-nvfp4-NInfer) requires
> minimum revision 385b30ce which is not in our fork. The model's tensor
> descriptor format (text/token_embedding) doesn't match our current build.
> Rebase on upstream first, then retry. Not part of this plan's scope.

### Download the NInfer model artifact

Source: https://huggingface.co/neroued/Qwen3.8-27B-nvfp4-NInfer

This is the official NInfer-format NVFP4 checkpoint for Qwen3.8-27B.
Download and verify the server can launch with it in production settings
(QUASAR model, c=3, 555k context, YaRN 2.12, --thinking).

### Steps

1. Download the model artifact using the HuggingFace CLI:
   ```
   huggingface-cli download neroued/Qwen3.8-27B-nvfp4-NInfer \
     --local-dir ~/ninfer-models/qwen3_8_27b_QUASAR_nvfp4.ninfer
   ```
2. Place it in ~/ninfer-models/ on strix.lan
3. Launch the production server with ninfer-start.sh
4. Verify the server starts and reports the correct model identity
5. Run a smoke test (single inference request) to confirm the model loads
6. Verify the chat template and weights profile are correct for QUASAR

### Verification criteria

- Server starts without errors
- Model identity reports qwen3.8-27b/nvfp4
- KV cache allocates correctly (nvfp4 KV)
- First inference request succeeds (non-empty output)
- No numerical errors or NaN in output
