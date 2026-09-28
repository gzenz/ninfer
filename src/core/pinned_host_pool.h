// One ELASTIC budget of pinned host memory, shared by consumers with different shapes: KV page runs are
// variable-size, state images are fixed ~187 MB blocks. It replaces two fixed, mutually-blind allocations
// (`HostKVArena`'s buffer and `HostStatePool`'s buffer), which is what made the host tier a configured
// partition rather than a resource: on 2026-09-26 production traffic the state pool sat full at 16/16
// while the KV arena held 9-10 GB of its 30 used, and the engine evicted eight restorable continuations
// instead of demoting them.
//
// WHY CHUNKS AND NOT ONE BUFFER. Views into this memory are raw pointers held across in-flight H2D/D2H
// copies (`state_image.cpp:244-265`), so an existing allocation's address must NEVER move. The pool
// therefore grows by pinning ADDITIONAL chunks and never reallocates; shrinking frees whole chunks, and
// only ones that hold nothing.
//
// THE CHUNK SOURCE IS INJECTED. Production passes a source that pins real memory (`PinnedHostBuffer`,
// `arena.h:97` -> `cudaMallocHost`); tests pass one backed by ordinary host memory. An allocator whose only
// test needs a GPU is an allocator whose growth path is rarely exercised, and growth is the whole point of
// this class.
//
// THE GROWTH POLICY IS ALSO INJECTED, because only the caller can see the host's free RAM and decide the
// reserve that keeps "no fixed ceiling" from becoming this host's documented OOM mode.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace ninfer {

class PinnedHostPool {
public:
    struct Handle {
        std::size_t   chunk      = 0;
        std::size_t   index      = 0;
        std::uint32_t generation = 0;

        [[nodiscard]] friend constexpr bool operator==(Handle, Handle) noexcept = default;
    };

    // Pin `bytes` and return its base, or nullptr on failure. The returned address must stay valid until
    // the matching releaser is called.
    using ChunkSource   = std::function<void*(std::size_t bytes)>;
    using ChunkReleaser = std::function<void(void* base)>;

    struct Config {
        std::size_t chunk_bytes   = 64U << 20U;  // one growth step
        std::size_t alignment     = 256U;        // state slots are laid out 256-byte aligned
        std::size_t initial_bytes = 0U;          // pinned at construction; 0 = start empty and grow on demand
        std::size_t max_bytes     = 0U;          // 0 = no fixed ceiling (the operator's requirement)
    };

    PinnedHostPool(Config config, ChunkSource source, ChunkReleaser releaser);
    ~PinnedHostPool();

    PinnedHostPool(const PinnedHostPool&)            = delete;
    PinnedHostPool& operator=(const PinnedHostPool&) = delete;
    PinnedHostPool(PinnedHostPool&&)                 = delete;
    PinnedHostPool& operator=(PinnedHostPool&&)      = delete;

    // May the pool pin `bytes` more? Supplied by the caller: it holds the free-RAM reading and the reserve.
    // With no policy set, growth is allowed up to `config.max_bytes` only.
    void set_growth_policy(std::function<bool(std::size_t bytes)> policy) { policy_ = std::move(policy); }

    // Allocate `bytes`. Grows if free space cannot satisfy it and the policy allows. Returns nothing when
    // neither is possible -- the caller decides whether that is benign or fatal, which is why this does not
    // throw (the fixed pool's callers currently throw `bad_alloc` here, `materialization.cpp:1718-1720`).
    [[nodiscard]] std::optional<Handle> allocate(std::size_t bytes) noexcept;

    [[nodiscard]] bool  release(Handle handle) noexcept;
    [[nodiscard]] std::byte* data(Handle handle) const noexcept;

    // Pin one more chunk of at least `bytes` (or `config.chunk_bytes`). Fails if the policy refuses or the
    // source cannot pin. A failed grow leaves the pool exactly as it was.
    [[nodiscard]] bool grow(std::size_t bytes = 0U) noexcept;

    // Carve a region the caller has ALREADY planned (chunk, offset, bytes). PURE: it never grows, and
    // returns nothing if that region is not currently free. This exists because the KV arena's
    // `apply_recipe` is `noexcept` and must not call the growing `allocate()` -- a recipe is planned against
    // one extent map and applied against it, and growing in between is exactly what `revision()` guards.
    [[nodiscard]] std::optional<Handle> carve_exact(std::size_t chunk, std::size_t offset,
                                                    std::size_t bytes) noexcept;

    // Would `bytes` fit in what is pinned RIGHT NOW? Pure, never grows. "Does it fit" and "will it be there
    // when I use it" became different questions the moment the pool could grow, and keeping a pure predicate
    // separate from a growing one is how the planner stays honest about which it is asking.
    [[nodiscard]] bool can_serve(std::size_t bytes) const noexcept;

    // How many MORE bytes must be pinned before `bytes` fits. 0 when it already fits. This is the signal
    // that replaces "return nullopt, then throw bad_alloc": a caller can pre-grow by this much and re-plan,
    // which is what lets a demote happen instead of an eviction.
    [[nodiscard]] std::size_t shortfall_for(std::size_t bytes) const noexcept;

    [[nodiscard]] std::size_t chunk_of(Handle handle) const noexcept { return handle.chunk; }
    [[nodiscard]] std::size_t offset_of(Handle handle) const noexcept;
    // The aligned size the pool actually granted for this handle. A caller that asked for X and wants to
    // carve X into pieces needs the real figure, not the request.
    [[nodiscard]] std::size_t size_of(Handle handle) const noexcept;
    [[nodiscard]] bool chunk_is_idle(std::size_t chunk) const noexcept;

    // Bumped by every grow and every shrink. A consumer that plans against this pool's extent map (the KV
    // arena's recipes) must compare it before applying: the pool revalidates inside `carve_exact`, but the
    // consumer's own revision check is the first line of defence and must not go stale.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    // Unpin one chunk that holds nothing. Never touches a chunk with a live allocation, whatever the
    // occupancy, because an address in it may be held by an in-flight copy.
    [[nodiscard]] bool shrink_idle() noexcept;

    [[nodiscard]] std::size_t capacity_bytes() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t occupied_bytes() const noexcept { return occupied_; }
    [[nodiscard]] std::size_t free_bytes() const noexcept { return capacity_ - occupied_; }
    // LIVE chunks. It counted `chunks_.size()` at first, which includes the tombstones shrink leaves behind
    // for handle stability -- so it reported chunks that no longer existed, and a test caught it.
    [[nodiscard]] std::size_t chunk_count() const noexcept {
        std::size_t live = 0;
        for (const Chunk& chunk : chunks_) {
            if (chunk.base != nullptr) { ++live; }
        }
        return live;
    }
    // The largest single run that could satisfy an allocation WITHOUT growing. Fragmentation is visible
    // here and nowhere else: `free_bytes()` can be large while nothing large can be placed.
    [[nodiscard]] std::size_t largest_free_run() const noexcept;

    // Instruments. `growth_refusals` counts "room was needed and the policy said no" -- without it, a
    // refusal is indistinguishable from a pool that never needed to grow.
    [[nodiscard]] std::uint64_t growth_count() const noexcept { return growth_count_; }
    [[nodiscard]] std::uint64_t growth_refusals() const noexcept { return growth_refusals_; }
    [[nodiscard]] std::uint64_t allocation_refusals() const noexcept { return allocation_refusals_; }

private:
    struct Allocation {
        std::size_t   offset     = 0;
        std::size_t   size       = 0;
        std::uint32_t generation = 1;
        bool          live       = false;
    };
    struct Extent {
        std::size_t offset = 0;
        std::size_t size   = 0;
    };
    struct Chunk {
        std::byte*               base = nullptr;  // ALIGNED start; nullptr marks a tombstone (unpinned)
        void*                    raw  = nullptr;  // what the source returned, which is what the releaser gets
        std::size_t              size = 0;        // usable bytes from `base`
        std::vector<Extent>      free;
        std::vector<Allocation>  allocations;
    };

    [[nodiscard]] std::size_t align_up(std::size_t bytes) const noexcept;
    // INVARIANT: first-fit scans chunks in ASCENDING index and extents by offset, so the lowest-index
    // (oldest) chunk is consumed first and the newest chunk stays empty longest. `shrink_idle` frees the
    // last fully-free chunk, so this ordering is what makes shrink useful rather than a coin flip -- it was
    // true by accident before it was written down here, and a test now pins it.
    [[nodiscard]] std::optional<Handle> try_allocate(std::size_t bytes) noexcept;
    [[nodiscard]] bool take_from(std::size_t chunk, std::size_t aligned_bytes) noexcept;
    void release_all() noexcept;

    Config                            config_;
    ChunkSource                       source_;
    ChunkReleaser                     releaser_;
    std::function<bool(std::size_t)>  policy_;
    std::vector<Chunk>                chunks_;
    std::size_t                       capacity_            = 0;
    std::size_t                       occupied_            = 0;
    std::uint64_t                     growth_count_        = 0;
    std::uint64_t                     growth_refusals_     = 0;
    std::uint64_t                     allocation_refusals_ = 0;
    std::uint64_t                     revision_            = 1;
};

}  // namespace ninfer
