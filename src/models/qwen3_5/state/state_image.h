#pragma once

#include "core/arena.h"
#include "core/pinned_host_pool.h"
#include "core/cyclic_kv_cache.h"
#include "core/layout.h"
#include "core/linear_attention_state.h"
#include "core/tensor.h"
#include "core/transfer_work.h"

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ninfer::models::qwen3_5 {

struct DFlashLocalStateSpec {
    std::uint32_t layers   = 0;
    std::uint32_t capacity = 0;
    std::int32_t kv_heads  = 0;
    std::int32_t head_dim  = 0;
};

struct StateImageSpec {
    LinearAttentionStatePoolSpec linear;
    std::int32_t hidden = 0;
    std::optional<DFlashLocalStateSpec> dflash_local;
};

struct StateImageHostLayout {
    StateImageSpec spec;
    LayoutRegion linear_conv;
    std::size_t linear_conv_layer_bytes = 0;
    LayoutRegion linear_recurrent;
    std::size_t linear_recurrent_layer_bytes = 0;
    LayoutRegion continuation_hidden;
    std::optional<LayoutRegion> dflash_local_k;
    std::optional<LayoutRegion> dflash_local_v;
    std::size_t dflash_local_layer_bytes = 0;
    std::size_t image_bytes              = 0;
};

struct StateImageDeviceLayout {
    LinearAttentionStatePoolLayout linear;
    TensorRegion continuation_hidden;
    std::optional<CyclicKVCacheLayout> dflash_local;
    StateImageHostLayout host;
};

[[nodiscard]] TransferWork state_image_transfer_work(const StateImageHostLayout& layout);
[[nodiscard]] TransferWork dflash_local_transfer_work(const StateImageHostLayout& layout);

[[nodiscard]] StateImageDeviceLayout plan_state_image_device_pool(LayoutBuilder& builder,
                                                                  const StateImageSpec& spec);

struct HostStateImageView {
    std::byte* data                    = nullptr;
    const StateImageHostLayout* layout = nullptr;
};

struct HostStateImageConstView {
    const std::byte* data              = nullptr;
    const StateImageHostLayout* layout = nullptr;
};

struct HostStateSlotHandle {
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;
};

/** Fixed-capacity pinned storage for complete physical StateImage payloads; owns no cache policy.
 */
class HostStatePool {
public:
    // The slots come from the SHARED pinned budget rather than owning a buffer: host KV and host state are
    // one pile of memory split on demand, which is what lets a full state pool borrow room the KV arena is
    // not using instead of evicting restorable state with host RAM idle (2026-09-26).
    HostStatePool(StateImageHostLayout layout, PinnedHostPool& pool);

    HostStatePool(const HostStatePool&)            = delete;
    HostStatePool& operator=(const HostStatePool&) = delete;
    HostStatePool(HostStatePool&&)                 = delete;
    HostStatePool& operator=(HostStatePool&&)      = delete;

    [[nodiscard]] std::optional<HostStateSlotHandle> allocate() noexcept;

    // Allocate a slot, growing the pool by one slot when none is free. This is the ONLY growth path, and it
    // is deliberately separate from `allocate()`: a caller on the demote path wants to grow, while a caller
    // applying an already-planned decision must not change the pool underneath it.
    [[nodiscard]] std::optional<HostStateSlotHandle> allocate_growing() noexcept;

    // Create up to `count` slots and leave them FREE -- `count` is the number of NEW slots ADDED, not a
    // total (the unit test asserts that: `reserve_slots(3)` gives `floor + 3`). This is how a configured
    // count is honoured without a fixed capacity in the type. The configured count is a FLOOR, not the
    // ceiling it once was: `ensure_host_state_headroom` pre-grows the pool by one slot when it is full,
    // before the planner prices anything.
    // Returns how many were created (fewer than asked if the pool refused to grow).
    [[nodiscard]] std::uint32_t reserve_slots(std::uint32_t count, bool speculative = false) noexcept;

    // Give back the trailing slots that hold nothing, so the shared pool can unpin the memory under them.
    // TRAILING ONLY: a slot index is a handle, so removing one from the middle would renumber live ones.
    // Returns how many were given back.
    // `keep` is a FLOOR and it is not optional. `capacity()` is `slots_.size()`, which `admission_capacity()`
    // feeds to `physical_peak_fits`, so trimming below the configured reservation makes every demote option
    // fail feasibility -- the engine would have no host room to demote INTO. Trimming is only ever safe above
    // the configured count: the state pool IS now pre-grown at planning time by one slot when full
    // (`ensure_host_state_headroom`, §3 item 6), but that growth happens BEFORE a plan is priced, so a trim
    // taken here is not restored by it -- the floor is what keeps `admission_capacity()` from dropping below
    // what the planner needs to demote into.
    [[nodiscard]] std::uint32_t trim_idle_slots(std::uint32_t keep) noexcept;

    // Slots added by growth. A pool that grew silently is a pool whose capacity change cannot be told from
    // a machine that simply never needed it.
    [[nodiscard]] std::uint64_t growth_count() const noexcept { return growth_count_; }
    // Growth attempts the pool or the budget refused, so a refusal is not silence either.
    [[nodiscard]] std::uint64_t growth_refusals() const noexcept { return growth_refusals_; }
    [[nodiscard]] bool release(HostStateSlotHandle handle) noexcept;

    [[nodiscard]] HostStateImageView writable_view(HostStateSlotHandle handle);
    [[nodiscard]] HostStateImageConstView view(HostStateSlotHandle handle) const;

    [[nodiscard]] std::uint32_t capacity() const noexcept;

    [[nodiscard]] std::uint32_t occupied() const noexcept { return occupied_; }

    [[nodiscard]] const StateImageHostLayout& layout() const noexcept { return layout_; }

private:
    struct Slot {
        PinnedHostPool::Handle allocation{};  // the slot's pinned extent in the shared pool
        std::uint32_t generation = 1;
        bool occupied            = false;
    };

    [[nodiscard]] bool valid(HostStateSlotHandle handle) const noexcept;
    [[nodiscard]] std::byte* slot_data(std::uint32_t index) const noexcept;
    // Pin one more slot from the shared pool and append it to the free list.
    [[nodiscard]] bool grow_slot(bool speculative = false) noexcept;

    StateImageHostLayout layout_;
    PinnedHostPool*     pool_ = nullptr;  // non-owning; the program owns the shared budget
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;  // a stack; `size()` is the free count
    std::uint32_t occupied_   = 0;
    std::uint64_t growth_count_    = 0;
    std::uint64_t growth_refusals_ = 0;
};

struct StateImageDeviceSlotView {
    LinearAttentionStateSlotView linear;
    Tensor continuation_hidden;
    std::optional<CyclicKVCacheSlotView> dflash_local;
};

/**
 * Caller-backed fixed storage for Qwen3.6 continuation state.
 *
 * Every absolute slot contains common GDN/hidden state and, for a DFlash Program, its local cyclic
 * K/V state. The pool owns neither slot roles nor logical checkpoint identity.
 */
class StateImageDevicePool {
public:
    StateImageDevicePool(DeviceSpan backing, const StateImageDeviceLayout& layout);

    StateImageDevicePool(const StateImageDevicePool&)            = delete;
    StateImageDevicePool& operator=(const StateImageDevicePool&) = delete;
    StateImageDevicePool(StateImageDevicePool&&)                 = delete;
    StateImageDevicePool& operator=(StateImageDevicePool&&)      = delete;

    [[nodiscard]] std::int32_t slot_count() const noexcept { return linear_.slot_count(); }

    [[nodiscard]] StateImageDeviceSlotView slot_view(std::int32_t slot) const;
    [[nodiscard]] Tensor continuation_hidden_slot(std::int32_t slot) const;

    [[nodiscard]] LinearAttentionStatePool& linear() noexcept { return linear_; }

    [[nodiscard]] const LinearAttentionStatePool& linear() const noexcept { return linear_; }

    [[nodiscard]] Tensor& continuation_hidden_store() noexcept { return continuation_hidden_; }

    [[nodiscard]] const Tensor& continuation_hidden_store() const noexcept {
        return continuation_hidden_;
    }

    [[nodiscard]] CyclicKVCache* dflash_local() noexcept;
    [[nodiscard]] const CyclicKVCache* dflash_local() const noexcept;

    [[nodiscard]] const StateImageHostLayout& host_layout() const noexcept { return host_layout_; }

    void zero_slot(std::int32_t slot, cudaStream_t stream = nullptr);
    void zero_all(cudaStream_t stream = nullptr);
    void copy_slot(std::int32_t source, std::int32_t destination, cudaStream_t stream = nullptr);
    void copy_dflash_local(std::int32_t source, std::int32_t destination,
                           cudaStream_t stream = nullptr);
    void copy_to_host(std::int32_t source, HostStateImageView destination,
                      cudaStream_t stream = nullptr) const;
    void copy_from_host(HostStateImageConstView source, std::int32_t destination,
                        cudaStream_t stream = nullptr);

private:
    void validate_host_layout(const StateImageHostLayout* layout, const std::byte* data) const;

    LinearAttentionStatePool linear_;
    Tensor continuation_hidden_;
    std::optional<CyclicKVCache> dflash_local_;
    StateImageHostLayout host_layout_;
};

// Pre-grow decision for the host state pool, as a free function so it can be TESTED host-only: the planner
// prices `host.state_slots` against the capacity that exists NOW, so a full pool must be grown by exactly
// ONE slot before planning or a demote can never be offered (§3 item 6).
//
// `reserve_slots(count)` ADDS `count` slots -- it is not "reserve up to this total". The first version of
// the caller passed `capacity + 1` and DOUBLED the pool (16 -> 33 -> 67), pinning ~3 GiB synchronously the
// first time prod's pool filled; a review found it by running the call against the real object. This
// function exists so the increment is asserted by a TEST: the unit test checks the capacity DIFFERENCE,
// which is the thing the return value does not tell you.
inline constexpr std::uint32_t HOST_STATE_PREGROW_SLOTS = 1U;

enum class HostStatePreGrow {
    Disabled,  // capacity 0: the startup reservation was refused. Fail closed rather than guess.
    NotFull,   // a free slot exists; nothing to do, and nothing is pinned.
    Grew,
    Refused,  // the pool or the host budget said no. Counted by the caller, so it is not silence.
};

[[nodiscard]] HostStatePreGrow pre_grow_host_state_pool(HostStatePool& pool) noexcept;

} // namespace ninfer::models::qwen3_5
