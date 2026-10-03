#include "core/pinned_host_pool.h"

#include <algorithm>
#include <utility>

namespace ninfer {

PinnedHostPool::PinnedHostPool(Config config, ChunkSource source, ChunkReleaser releaser)
    : config_(config), source_(std::move(source)), releaser_(std::move(releaser)) {
    if (config_.chunk_bytes == 0U) { config_.chunk_bytes = 64U << 20U; }
    if (config_.alignment == 0U) { config_.alignment = 1U; }
    // Best effort: a caller that asks for an initial reservation gets it if the policy and the source
    // allow, and otherwise gets a pool that will try again on the first allocation. Failing here would
    // make construction the place a transient host-memory shortage becomes fatal, which is the opposite of
    // what an elastic pool is for. `growth_refusals()` records it.
    if (config_.initial_bytes != 0U) { (void)grow(config_.initial_bytes); }
}

PinnedHostPool::~PinnedHostPool() { release_all(); }

std::size_t PinnedHostPool::align_up(std::size_t bytes) const noexcept {
    const std::size_t a = config_.alignment;
    return (bytes + a - 1U) / a * a;
}

std::size_t PinnedHostPool::largest_free_run() const noexcept {
    std::size_t best = 0;
    for (const Chunk& chunk : chunks_) {
        if (chunk.base == nullptr) { continue; }  // tombstone: unpinned, its index kept for handle stability
        for (const Extent& extent : chunk.free) { best = std::max(best, extent.size); }
    }
    return best;
}

bool PinnedHostPool::grow(std::size_t bytes) noexcept {
    const std::size_t wanted = align_up(std::max(bytes, config_.chunk_bytes));
    const bool allowed =
        policy_ ? policy_(wanted)
                : (config_.max_bytes == 0U || capacity_ + wanted <= config_.max_bytes);
    // THE TWO REFUSALS ARE NOT THE SAME EVENT, and counting them together made the difference unreadable:
    // "the growth policy said no" is a POLICY fact (the reserve, a ceiling, or a failed /proc/meminfo read)
    // while "pinning actually failed" is a MACHINE fact -- `cudaMallocHost` returning null. Diagnosing one
    // as the other sends the fix to the wrong place, so they are counted apart and the policy half also
    // records WHY (see `HostMemoryBudget::last_veto`).
    if (!allowed) {
        ++growth_refusals_;
        ++growth_policy_refusals_;
        return false;
    }
    const std::size_t slack = config_.alignment - 1U;
    void* const raw = source_ ? source_(wanted + slack) : nullptr;
    if (raw == nullptr) {
        ++growth_refusals_;
        ++growth_pin_failures_;
        return false;
    }
    // Align the base HERE rather than trusting the source: `cudaMallocHost` happens to be page-aligned, but
    // nothing in this interface promises that, and a test with an ordinary host-memory source caught the
    // pool handing out unaligned addresses (a 1-byte allocation's address was not 256-byte aligned).
    const auto raw_address = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned_address =
        (raw_address + slack) / config_.alignment * config_.alignment;
    std::byte* const base = reinterpret_cast<std::byte*>(aligned_address);
    // Reuse a tombstone if one exists, so `Handle::chunk` indices stay stable and the vector does not need
    // to reallocate (it may anyway, but indices are what matters).
    Chunk* target = nullptr;
    for (Chunk& chunk : chunks_) {
        if (chunk.base == nullptr) { target = &chunk; break; }
    }
    if (target == nullptr) {
        chunks_.push_back(Chunk{});
        target = &chunks_.back();
    }
    target->base        = base;
    target->raw         = raw;
    target->size        = wanted;
    target->free        = {Extent{0U, wanted}};
    target->allocations = {};
    capacity_ += wanted;
    ++growth_count_;
    ++revision_;  // a plan made before this growth is stale
    return true;
}

bool PinnedHostPool::shrink_idle() noexcept {
    // The LAST fully-free chunk, if any. A chunk with any live allocation is never touched: an address in
    // it may be held by an in-flight copy, and unpinning would be a use-after-free.
    for (std::size_t index = chunks_.size(); index-- > 0U;) {
        Chunk& chunk = chunks_[index];
        if (chunk.base == nullptr) { continue; }
        const bool any_live =
            std::any_of(chunk.allocations.begin(), chunk.allocations.end(),
                        [](const Allocation& a) { return a.live; });
        if (any_live) { continue; }
        // Read the size BEFORE clearing it: an earlier version zeroed `chunk.size` first and then
        // subtracted it, so capacity_ never came down and shrink silently did nothing to the accounting.
        const std::size_t freed_bytes = chunk.size;
        if (releaser_) { releaser_(chunk.raw); }
        chunk.base = nullptr;
        chunk.raw = nullptr;
        chunk.size = 0U;
        chunk.free.clear();
        chunk.allocations.clear();
        capacity_ -= std::min(capacity_, freed_bytes);
        ++revision_;  // a plan made before this shrink is stale
        return true;
    }
    return false;
}

std::optional<PinnedHostPool::Handle> PinnedHostPool::try_allocate(std::size_t aligned_bytes) noexcept {
    for (std::size_t c = 0; c < chunks_.size(); ++c) {
        Chunk& chunk = chunks_[c];
        if (chunk.base == nullptr) { continue; }
        // BEST-FIT WITHIN THE CHUNK -- the SMALLEST extent that fits, not the first one met.
        // First-fit splits from the FRONT of whichever extent it reaches first, so a small request eats into
        // the largest run: that is how the pool ends up with 2175 MiB free and a 290 MiB largest run, which
        // cost 25 placement refusals on live traffic (measured 2026-10-01) while a state image needs ~187
        // MiB. Taking the tightest fitting extent leaves the large runs intact, at the same O(n) cost.
        //
        // DELIBERATELY PER-CHUNK, NOT GLOBAL. Chunks must still be consumed in ASCENDING order, because that
        // is what keeps the newest chunk empty longest and makes `shrink_idle` useful rather than a coin
        // flip (see the invariant in the header). A global best-fit would scatter allocations into older
        // chunks with room and strand the newest one PARTLY filled, so `shrink_idle` -- which frees only a
        // FULLY free chunk -- would stop finding one. That trades a real capability for a local packing win,
        // and the existing chunk-order test would not have caught it.
        std::size_t best = chunk.free.size();
        for (std::size_t e = 0; e < chunk.free.size(); ++e) {
            const Extent& candidate = chunk.free[e];
            if (candidate.size < aligned_bytes) { continue; }
            if (best == chunk.free.size() || candidate.size < chunk.free[best].size) { best = e; }
        }
        if (best == chunk.free.size()) { continue; }  // nothing in THIS chunk fits; try the next
        {
            const std::size_t e = best;
            Extent& extent = chunk.free[e];
            const std::size_t offset = extent.offset;
            if (extent.size == aligned_bytes) {
                chunk.free.erase(chunk.free.begin() + static_cast<std::ptrdiff_t>(e));
            } else {
                extent.offset += aligned_bytes;
                extent.size -= aligned_bytes;
            }
            // Reuse a dead allocation record, so the pool does not grow its bookkeeping without bound.
            std::size_t slot = chunk.allocations.size();
            for (std::size_t i = 0; i < chunk.allocations.size(); ++i) {
                if (!chunk.allocations[i].live) { slot = i; break; }
            }
            if (slot == chunk.allocations.size()) {
                chunk.allocations.push_back(Allocation{});
            }
            Allocation& allocation = chunk.allocations[slot];
            allocation.offset      = offset;
            allocation.size        = aligned_bytes;
            allocation.live        = true;
            if (++allocation.generation == 0U) { allocation.generation = 1U; }  // 0 never identifies a live
            occupied_ += aligned_bytes;
            return Handle{c, slot, allocation.generation};
        }
    }
    return std::nullopt;
}

std::optional<PinnedHostPool::Handle> PinnedHostPool::allocate(std::size_t bytes,
                                                               bool speculative) noexcept {
    if (bytes == 0U) { return std::nullopt; }
    const std::size_t aligned_bytes = align_up(bytes);
    if (auto handle = try_allocate(aligned_bytes); handle) { return handle; }
    // Free space could not satisfy it: pin another chunk and retry once. A second failure is final --
    // retrying would spin, and the caller needs a definite answer to decide between demoting and evicting.
    // (1) THE FIRST ATTEMPT FAILED. If the pool still holds at least `aligned_bytes` FREE, then the failure
    // was PLACEMENT and not size -- that is the fragmentation signature, and it is the counter that can
    // prove it. The first version of this instrument counted the POST-GROW failure instead and called that
    // the proof; a review showed that path is UNREACHABLE (`grow()` pins a fresh extent of at least
    // `aligned_bytes`, so the retry always fits), which meant the counter asserting "fragmentation is real"
    // could never fire while the real fragmentation path was counted nowhere. Measured 2026-10-01:
    // `free_bytes` 1798 MiB against `largest_free_run` 90 MiB, with a state image needing ~187 MiB -- the
    // pool held far more than enough BYTES and no single place to put one.
    // A SPECULATIVE failure is counted separately and does NOT enter the fragmentation signal: the signal
    // exists to prove that a REAL allocation failed while free bytes sufficed, and a pre-grow is not one.
    if (!speculative && free_bytes() >= aligned_bytes) { ++allocation_fragmented_misses_; }
    if (!grow(aligned_bytes)) {
        // (2) NO ROOM AND GROWTH REFUSED -- a RAM/policy fact: the pool could neither place it nor grow.
        if (speculative) {
            ++allocation_speculative_refusals_;
            return std::nullopt;
        }
        ++allocation_refusals_;
        ++allocation_ram_refusals_;
        return std::nullopt;
    }
    if (auto handle = try_allocate(aligned_bytes); handle) { return handle; }
    // (3) GREW AND STILL COULD NOT PLACE IT. Believed UNREACHABLE -- see (1); kept as a contract check
    // rather than as evidence, because if it ever fires the growth guarantee is broken and that is worth
    // knowing. Do NOT read it as fragmentation: it would mean the pool could not use a chunk it had just
    // pinned, which is a different and much worse fault.
    ++allocation_refusals_;
    ++allocation_post_grow_failures_;
    return std::nullopt;
}

bool PinnedHostPool::release(Handle handle) noexcept {
    if (handle.chunk >= chunks_.size()) { return false; }
    Chunk& chunk = chunks_[handle.chunk];
    if (chunk.base == nullptr || handle.index >= chunk.allocations.size()) { return false; }
    Allocation& allocation = chunk.allocations[handle.index];
    if (!allocation.live || allocation.generation != handle.generation) { return false; }
    allocation.live = false;
    occupied_ -= (occupied_ >= allocation.size) ? allocation.size : 0U;
    // Coalesce with the neighbours, so a released slot does not fragment a chunk into unusable pieces.
    chunk.free.push_back(Extent{allocation.offset, allocation.size});
    std::sort(chunk.free.begin(), chunk.free.end(),
              [](const Extent& a, const Extent& b) { return a.offset < b.offset; });
    std::vector<Extent> merged;
    merged.reserve(chunk.free.size());
    for (const Extent& extent : chunk.free) {
        if (!merged.empty() && merged.back().offset + merged.back().size == extent.offset) {
            merged.back().size += extent.size;
        } else {
            merged.push_back(extent);
        }
    }
    chunk.free = std::move(merged);
    return true;
}

std::byte* PinnedHostPool::data(Handle handle) const noexcept {
    if (handle.chunk >= chunks_.size()) { return nullptr; }
    const Chunk& chunk = chunks_[handle.chunk];
    if (chunk.base == nullptr || handle.index >= chunk.allocations.size()) { return nullptr; }
    const Allocation& allocation = chunk.allocations[handle.index];
    if (!allocation.live || allocation.generation != handle.generation) { return nullptr; }
    return chunk.base + allocation.offset;
}

void PinnedHostPool::release_all() noexcept {
    for (Chunk& chunk : chunks_) {
        if (chunk.base != nullptr && releaser_) { releaser_(chunk.raw); }
        chunk.base = nullptr;
        chunk.raw = nullptr;
        chunk.size = 0U;
        chunk.free.clear();
        chunk.allocations.clear();
    }
    capacity_ = 0U;
    occupied_ = 0U;
}


bool PinnedHostPool::can_serve(std::size_t bytes) const noexcept {
    if (bytes == 0U) { return false; }
    return largest_free_run() >= align_up(bytes);
}

std::size_t PinnedHostPool::shortfall_for(std::size_t bytes) const noexcept {
    if (bytes == 0U) { return 0U; }
    const std::size_t aligned = align_up(bytes);
    const std::size_t best    = largest_free_run();
    return best >= aligned ? 0U : aligned - best;
}

std::optional<PinnedHostPool::Handle> PinnedHostPool::carve_exact(std::size_t chunk, std::size_t offset,
                                                                 std::size_t bytes) noexcept {
    if (chunk >= chunks_.size() || bytes == 0U) { return std::nullopt; }
    Chunk& target = chunks_[chunk];
    if (target.base == nullptr) { return std::nullopt; }
    // A planned region must itself be aligned: the pool's own allocations are, so an unaligned request means
    // the caller's arithmetic is off and silently accepting it would hand out an address the consumers'
    // pointer assumptions do not hold for.
    if (offset % config_.alignment != 0U || bytes % config_.alignment != 0U) { return std::nullopt; }
    for (std::size_t e = 0; e < target.free.size(); ++e) {
        Extent& extent = target.free[e];
        if (extent.offset > offset || extent.offset + extent.size < offset + bytes) { continue; }
        const std::size_t tail_offset = offset + bytes;
        const std::size_t tail_size   = extent.offset + extent.size - tail_offset;
        if (extent.offset == offset && tail_size == 0U) {
            target.free.erase(target.free.begin() + static_cast<std::ptrdiff_t>(e));
        } else if (extent.offset == offset) {
            extent.offset = tail_offset;
            extent.size   = tail_size;
        } else if (tail_size == 0U) {
            extent.size = offset - extent.offset;
        } else {
            extent.size = offset - extent.offset;
            target.free.insert(target.free.begin() + static_cast<std::ptrdiff_t>(e) + 1,
                               Extent{tail_offset, tail_size});
        }
        std::size_t slot = target.allocations.size();
        for (std::size_t i = 0; i < target.allocations.size(); ++i) {
            if (!target.allocations[i].live) { slot = i; break; }
        }
        if (slot == target.allocations.size()) { target.allocations.push_back(Allocation{}); }
        Allocation& allocation = target.allocations[slot];
        allocation.offset      = offset;
        allocation.size        = bytes;
        allocation.live        = true;
        if (++allocation.generation == 0U) { allocation.generation = 1U; }
        occupied_ += bytes;
        return Handle{chunk, slot, allocation.generation};
    }
    return std::nullopt;  // not free: the caller's plan is stale (see revision())
}

std::size_t PinnedHostPool::size_of(Handle handle) const noexcept {
    if (handle.chunk >= chunks_.size()) { return 0U; }
    const Chunk& chunk = chunks_[handle.chunk];
    if (chunk.base == nullptr || handle.index >= chunk.allocations.size()) { return 0U; }
    const Allocation& allocation = chunk.allocations[handle.index];
    if (!allocation.live || allocation.generation != handle.generation) { return 0U; }
    return allocation.size;
}

std::size_t PinnedHostPool::offset_of(Handle handle) const noexcept {
    if (handle.chunk >= chunks_.size()) { return 0U; }
    const Chunk& chunk = chunks_[handle.chunk];
    if (chunk.base == nullptr || handle.index >= chunk.allocations.size()) { return 0U; }
    const Allocation& allocation = chunk.allocations[handle.index];
    if (!allocation.live || allocation.generation != handle.generation) { return 0U; }
    return allocation.offset;
}

bool PinnedHostPool::chunk_is_idle(std::size_t chunk) const noexcept {
    if (chunk >= chunks_.size()) { return false; }
    const Chunk& target = chunks_[chunk];
    if (target.base == nullptr) { return false; }
    for (const Allocation& allocation : target.allocations) {
        if (allocation.live) { return false; }
    }
    return true;
}

}  // namespace ninfer
