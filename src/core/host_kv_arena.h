#pragma once

#include "core/arena.h"
#include "core/pinned_host_pool.h"
#include "core/paged_kv_cache.h"
#include "core/transfer_work.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ninfer {

struct HostKVPlaneLayout {
    std::size_t offset             = 0;
    std::size_t page_payload_bytes = 0;
    std::size_t head_payload_bytes = 0;

    friend bool operator==(const HostKVPlaneLayout&, const HostKVPlaneLayout&) = default;
};

struct HostKVPageLayout {
    KVPageGeometry geometry;
    std::vector<HostKVPlaneLayout> planes;
    std::size_t page_stride = 0;

    friend bool operator==(const HostKVPageLayout&, const HostKVPageLayout&) = default;
};

[[nodiscard]] HostKVPageLayout plan_host_kv_page_layout(const KVPageGeometry& geometry);

[[nodiscard]] TransferWork plan_host_kv_transfer_work(const HostKVPageLayout& layout,
                                                      std::uint32_t pages,
                                                      std::uint32_t contiguous_runs);
[[nodiscard]] TransferWork plan_device_kv_copy_work(const HostKVPageLayout& layout,
                                                    std::uint32_t pages);

class HostKVArena;

class HostKVAllocationHandle {
public:
    HostKVAllocationHandle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

    [[nodiscard]] friend bool operator==(HostKVAllocationHandle,
                                         HostKVAllocationHandle) noexcept = default;

private:
    friend class HostKVArena;
    friend class HostKVAllocation;
    friend class HostKVAllocationView;
    friend class HostKVAllocationConstView;

    HostKVAllocationHandle(const HostKVArena* owner, std::uint32_t descriptor,
                           std::uint32_t generation) noexcept
        : owner_(owner), descriptor_(descriptor), generation_(generation) {}

    const HostKVArena* owner_ = nullptr;
    std::uint32_t descriptor_ = 0;
    std::uint32_t generation_ = 0;
};

class HostKVAllocationView {
public:
    HostKVAllocationView() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] std::byte* data() const noexcept { return data_; }

    [[nodiscard]] std::uint32_t page_count() const noexcept { return page_count_; }

    [[nodiscard]] const HostKVPageLayout& layout() const;
    [[nodiscard]] HostKVAllocationView subview(std::uint32_t begin, std::uint32_t count) const;

private:
    friend class HostKVArena;
    friend class HostKVAllocationConstView;

    HostKVAllocationView(HostKVAllocationHandle handle, std::byte* data,
                         const HostKVPageLayout* layout, std::uint32_t page_count) noexcept
        : handle_(handle), data_(data), layout_(layout), page_count_(page_count) {}

    HostKVAllocationHandle handle_;
    std::byte* data_                = nullptr;
    const HostKVPageLayout* layout_ = nullptr;
    std::uint32_t page_count_       = 0;
};

class HostKVAllocationConstView {
public:
    HostKVAllocationConstView() noexcept = default;

    HostKVAllocationConstView(HostKVAllocationView view) noexcept
        : handle_(view.handle_), data_(view.data_), layout_(view.layout_),
          page_count_(view.page_count_) {}

    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] const std::byte* data() const noexcept { return data_; }

    [[nodiscard]] std::uint32_t page_count() const noexcept { return page_count_; }

    [[nodiscard]] const HostKVPageLayout& layout() const;
    [[nodiscard]] HostKVAllocationConstView subview(std::uint32_t begin, std::uint32_t count) const;

private:
    friend class HostKVArena;

    HostKVAllocationConstView(HostKVAllocationHandle handle, const std::byte* data,
                              const HostKVPageLayout* layout, std::uint32_t page_count) noexcept
        : handle_(handle), data_(data), layout_(layout), page_count_(page_count) {}

    HostKVAllocationHandle handle_;
    const std::byte* data_          = nullptr;
    const HostKVPageLayout* layout_ = nullptr;
    std::uint32_t page_count_       = 0;
};

class HostKVAllocation {
public:
    HostKVAllocation() noexcept = default;
    ~HostKVAllocation();

    HostKVAllocation(const HostKVAllocation&)            = delete;
    HostKVAllocation& operator=(const HostKVAllocation&) = delete;
    HostKVAllocation(HostKVAllocation&& other) noexcept;
    HostKVAllocation& operator=(HostKVAllocation&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

    [[nodiscard]] HostKVAllocationHandle handle() const noexcept;
    [[nodiscard]] std::uint32_t page_count() const noexcept;
    bool release() noexcept;

private:
    friend class HostKVArena;

    HostKVAllocation(HostKVArena& owner, std::uint32_t descriptor,
                     std::uint32_t generation) noexcept
        : owner_(&owner), descriptor_(descriptor), generation_(generation) {}

    void disarm() noexcept;

    HostKVArena* owner_       = nullptr;
    std::uint32_t descriptor_ = 0;
    std::uint32_t generation_ = 0;
};

struct HostKVAllocationRequest {
    const HostKVPageLayout* layout = nullptr;
    std::uint32_t pages            = 0;
};

struct HostKVSuballocationRelease {
    HostKVAllocationHandle allocation;
    std::uint32_t begin_page = 0;
    std::uint32_t page_count = 0;
};

class HostKVAllocationRecipe {
public:
    HostKVAllocationRecipe() noexcept                                    = default;
    HostKVAllocationRecipe(HostKVAllocationRecipe&&) noexcept            = default;
    HostKVAllocationRecipe& operator=(HostKVAllocationRecipe&&) noexcept = default;

    HostKVAllocationRecipe(const HostKVAllocationRecipe&)            = delete;
    HostKVAllocationRecipe& operator=(const HostKVAllocationRecipe&) = delete;

    [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

    [[nodiscard]] std::size_t release_count() const noexcept { return releases_.size(); }

    [[nodiscard]] std::size_t allocation_count() const noexcept { return targets_.size(); }

private:
    struct Target {
        std::uint32_t layout = 0;
        std::uint32_t pages  = 0;
        std::uint32_t span   = 0;   // which pool extent the plan placed this run in
        std::size_t offset   = 0;   // span-relative
        std::size_t bytes    = 0;
    };

    const HostKVArena* owner_     = nullptr;
    std::uint64_t arena_revision_ = 0;
    std::vector<HostKVAllocationHandle> releases_;
    std::vector<Target> targets_;

    friend class HostKVArena;
};

class HostKVArena {
public:
    // The arena now draws its memory from the SHARED pinned pool instead of owning one buffer, so it can
    // take room the state pool is not using and give it back. `initial_bytes` is a reservation, not a
    // ceiling; `span_bytes` is the size of one growth step.
    HostKVArena(PinnedHostPool& pool, std::size_t initial_bytes, std::size_t span_bytes,
                std::span<const HostKVPageLayout> supported_layouts);

    HostKVArena(const HostKVArena&)            = delete;
    HostKVArena& operator=(const HostKVArena&) = delete;
    HostKVArena(HostKVArena&&)                 = delete;
    HostKVArena& operator=(HostKVArena&&)      = delete;

    [[nodiscard]] std::size_t capacity_bytes() const noexcept { return capacity_bytes_; }

    [[nodiscard]] std::size_t occupied_bytes() const noexcept { return occupied_bytes_; }

    [[nodiscard]] std::size_t free_bytes() const noexcept {
        return capacity_bytes_ - occupied_bytes_;
    }

    [[nodiscard]] const HostKVPageLayout* layout_for(const KVPageGeometry& geometry) const noexcept;

    [[nodiscard]] bool can_allocate(const HostKVPageLayout& layout,
                                    std::uint32_t pages) const noexcept;
    [[nodiscard]] std::optional<HostKVAllocation> allocate(const HostKVPageLayout& layout,
                                                           std::uint32_t pages) noexcept;

    // THE SAME ALLOCATION, ALLOWED TO GROW A SPAN. `allocate` stays pure so a caller applying an
    // already-planned recipe never changes the arena underneath itself.
    [[nodiscard]] std::optional<HostKVAllocation> allocate_growing(const HostKVPageLayout& layout,
                                                                   std::uint32_t pages) noexcept;

    // How many more bytes would have to be pinned for this request to fit, 0 when it already does.
    // NO PRODUCTION CALLER TODAY: a planner pre-grow is exactly what this change tried and removed, so the
    // only callers are tests. Kept because it is the measurement a future pre-grow would need -- and named
    // here as unused rather than described as though a planner were consulting it.
    [[nodiscard]] std::size_t shortfall_for(std::uint32_t pages, std::size_t page_stride) const noexcept;

    // Descriptors needed to describe `bytes` of host KV, using the SMALLEST supported page stride -- the
    // most descriptors that region could ever require. Used by `grow_span`, so a span can never arrive
    // unable to be described.
    [[nodiscard]] std::size_t descriptor_hint_for(std::size_t bytes) const noexcept;

    // Pin enough for `pages` of this layout, as one span. Page-shaped rather than byte-shaped to match the
    // rest of this interface, and public because a caller that must not silently fail (the extent store's
    // demote path, `host_kv_store.h`) needs to ask for the room explicitly.
    [[nodiscard]] bool grow_for(std::uint32_t pages, std::size_t page_stride) noexcept;
    [[nodiscard]] bool grow_bytes(std::size_t bytes) noexcept;

    [[nodiscard]] std::uint64_t growth_count() const noexcept { return growth_count_; }
    [[nodiscard]] std::uint64_t growth_refusals() const noexcept { return growth_refusals_; }

    [[nodiscard]] std::optional<HostKVAllocationRecipe>
    plan_after_releases(std::span<const HostKVAllocationHandle> proposed_releases,
                        std::span<const HostKVAllocationRequest> target_allocations) const;

    [[nodiscard]] bool can_allocate_after_suballocation_releases(
        std::span<const HostKVSuballocationRelease> proposed_releases,
        std::span<const HostKVAllocationRequest> target_allocations) const;

    // The caller supplies already-sized empty outputs so successful adoption cannot allocate.
    // A false return leaves the arena and every input allocation unchanged.
    [[nodiscard]] bool apply_recipe(HostKVAllocationRecipe&& recipe,
                                    std::span<HostKVAllocation* const> proposed_releases,
                                    std::span<HostKVAllocation> target_allocations) noexcept;

    [[nodiscard]] std::pair<HostKVAllocation, HostKVAllocation> split(HostKVAllocation&& allocation,
                                                                      std::uint32_t page_offset);

    [[nodiscard]] HostKVAllocationView writable_view(HostKVAllocation& allocation);
    [[nodiscard]] HostKVAllocationConstView view(const HostKVAllocation& allocation) const;

private:
    friend class HostKVAllocation;
    friend class HostKVAllocationView;
    friend class HostKVAllocationConstView;

    // ONE POOL EXTENT PER SPAN. A page run must be physically contiguous, so it lives inside a single span
    // and never straddles two -- which is why growth adds spans rather than extending one address space.
    struct Span {
        PinnedHostPool::Handle allocation{};
        std::size_t            bytes = 0;
    };

    struct Descriptor {
        std::uint32_t span       = 0;
        std::size_t offset       = 0;   // span-relative
        std::size_t bytes        = 0;
        std::uint32_t layout     = 0;
        std::uint32_t pages      = 0;
        std::uint32_t generation = 1;
        bool active              = false;
    };

    struct FreeExtent {
        std::uint32_t span  = 0;
        std::size_t offset = 0;   // span-relative
        std::size_t bytes  = 0;
    };

    [[nodiscard]] std::optional<std::uint32_t>
    find_layout(const HostKVPageLayout& layout) const noexcept;
    [[nodiscard]] std::optional<std::size_t> find_free_extent(std::size_t bytes) const noexcept;
    // Pin one more span of at least `bytes` from the shared pool. A failed attempt leaves the arena as it was.
    [[nodiscard]] bool grow_span(std::size_t bytes) noexcept;
    [[nodiscard]] std::size_t span_bytes(std::uint32_t span) const noexcept;
    [[nodiscard]] std::size_t smallest_stride() const noexcept;
    [[nodiscard]] bool valid_handle(HostKVAllocationHandle handle) const noexcept;
    [[nodiscard]] std::uint32_t take_descriptor() noexcept;
    bool release_descriptor(std::uint32_t descriptor, std::uint32_t generation) noexcept;
    void insert_free_extent(FreeExtent extent) noexcept;
    // ONE definition of "insert an extent into a (span, offset)-ordered list and coalesce, but only within
    // the same span". THREE sites need it -- the real free list, the recipe simulation and the suballocation
    // simulation -- and keeping three copies is exactly how two of them ended up span-blind, which the
    // 2026-09-26 review reproduced as a false "8 contiguous pages fit" across two spans.
    static void insert_extent_ordered(std::vector<FreeExtent>& extents, FreeExtent extent) noexcept;
    [[nodiscard]] std::byte* allocation_data(const Descriptor& descriptor) const noexcept;
    void bump_revision() noexcept;

    PinnedHostPool* pool_ = nullptr;  // non-owning: the program owns the shared budget
    std::vector<Span> spans_;
    std::size_t span_growth_bytes_ = 0;
    std::size_t capacity_bytes_ = 0;
    std::size_t occupied_bytes_ = 0;
    std::vector<HostKVPageLayout> layouts_;
    std::vector<Descriptor> descriptors_;
    std::vector<std::uint32_t> free_descriptors_;
    std::vector<FreeExtent> free_extents_;
    std::uint64_t revision_    = 1;
    std::uint64_t growth_count_    = 0;
    std::uint64_t growth_refusals_ = 0;
};

} // namespace ninfer
