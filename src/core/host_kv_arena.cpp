#include "core/host_kv_arena.h"

#include "core/dtype.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace ninfer {
namespace {

constexpr std::size_t kHostKVAlignment = 256;

std::size_t checked_add(std::size_t a, std::size_t b, const char* label) {
    if (b > std::numeric_limits<std::size_t>::max() - a) { throw std::overflow_error(label); }
    return a + b;
}

std::size_t checked_mul(std::size_t a, std::size_t b, const char* label) {
    if (b != 0 && a > std::numeric_limits<std::size_t>::max() / b) {
        throw std::overflow_error(label);
    }
    return a * b;
}

std::size_t align_up(std::size_t value, std::size_t alignment, const char* label) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        throw std::invalid_argument(std::string(label) + " alignment must be a power of two");
    }
    const std::size_t mask = alignment - 1;
    if (value > std::numeric_limits<std::size_t>::max() - mask) {
        throw std::overflow_error(std::string(label) + " alignment overflow");
    }
    return (value + mask) & ~mask;
}

void increment_generation(std::uint32_t& generation) noexcept {
    ++generation;
    if (generation == 0) { ++generation; }
}

} // namespace

HostKVPageLayout plan_host_kv_page_layout(const KVPageGeometry& geometry) {
    if (geometry.page_tokens == 0 || geometry.planes.empty()) {
        throw std::invalid_argument("Host KV page geometry is empty");
    }

    HostKVPageLayout out;
    out.geometry = geometry;
    out.planes.reserve(geometry.planes.size());
    std::size_t cursor = 0;
    for (const KVPlaneGeometry& plane : geometry.planes) {
        if (plane.leading_extent <= 0 || plane.head_extent <= 0) {
            throw std::invalid_argument("Host KV plane geometry must be positive");
        }
        // Device slab alignment is not part of the canonical packed Host representation.
        cursor = align_up(cursor, kHostKVAlignment, "Host KV plane");
        const std::size_t head_bytes =
            checked_mul(checked_mul(static_cast<std::size_t>(plane.leading_extent),
                                    geometry.page_tokens, "Host KV head payload overflow"),
                        dtype_size(plane.dtype), "Host KV head payload overflow");
        const std::size_t page_bytes =
            checked_mul(head_bytes, static_cast<std::size_t>(plane.head_extent),
                        "Host KV plane payload overflow");
        out.planes.push_back(HostKVPlaneLayout{
            .offset             = cursor,
            .page_payload_bytes = page_bytes,
            .head_payload_bytes = head_bytes,
        });
        cursor = checked_add(cursor, page_bytes, "Host KV page payload overflow");
    }
    out.page_stride = align_up(cursor, kHostKVAlignment, "Host KV page record");
    return out;
}

TransferWork plan_host_kv_transfer_work(const HostKVPageLayout& layout, std::uint32_t pages,
                                        std::uint32_t contiguous_runs) {
    if (pages == 0) { return {}; }
    if (contiguous_runs == 0 || contiguous_runs > pages || layout.planes.empty() ||
        layout.planes.size() != layout.geometry.planes.size()) {
        throw std::invalid_argument("Host KV transfer geometry is invalid");
    }

    std::size_t bytes_per_page     = 0;
    std::size_t operations_per_run = 0;
    for (std::size_t index = 0; index < layout.planes.size(); ++index) {
        bytes_per_page = checked_add(bytes_per_page, layout.planes[index].page_payload_bytes,
                                     "Host KV transfer payload overflow");
        const KVPlaneGeometry& plane = layout.geometry.planes[index];
        const std::size_t operations =
            layout.geometry.device_plane_order == PagedKVPlaneOrder::PageMajor
                ? 1U
                : static_cast<std::size_t>(plane.head_extent);
        operations_per_run = checked_add(operations_per_run, operations,
                                         "Host KV transfer operation count overflow");
    }

    const std::size_t payload =
        checked_mul(bytes_per_page, pages, "Host KV transfer payload overflow");
    const std::size_t operations = checked_mul(operations_per_run, contiguous_runs,
                                               "Host KV transfer operation count overflow");
    if (operations > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("Host KV transfer operation count exceeds uint32");
    }
    return TransferWork{.payload_bytes   = static_cast<std::uint64_t>(payload),
                        .copy_operations = static_cast<std::uint32_t>(operations)};
}

TransferWork plan_device_kv_copy_work(const HostKVPageLayout& layout, std::uint32_t pages) {
    if (pages == 0) { return {}; }
    if (layout.planes.empty() || layout.planes.size() != layout.geometry.planes.size()) {
        throw std::invalid_argument("Device KV copy geometry is invalid");
    }

    std::size_t bytes_per_page = 0;
    for (const HostKVPlaneLayout& plane : layout.planes) {
        bytes_per_page = checked_add(bytes_per_page, plane.page_payload_bytes,
                                     "Device KV copy payload overflow");
    }
    const std::size_t payload =
        checked_mul(bytes_per_page, pages, "Device KV copy payload overflow");
    const std::size_t operations =
        checked_mul(layout.planes.size(), pages, "Device KV copy operation count overflow");
    if (operations > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("Device KV copy operation count exceeds uint32");
    }
    return TransferWork{.payload_bytes   = static_cast<std::uint64_t>(payload),
                        .copy_operations = static_cast<std::uint32_t>(operations)};
}

bool HostKVAllocationView::valid() const noexcept {
    return handle_.owner_ != nullptr && handle_.owner_->valid_handle(handle_);
}

const HostKVPageLayout& HostKVAllocationView::layout() const {
    if (!valid() || layout_ == nullptr) { throw std::logic_error("Host KV view is stale"); }
    return *layout_;
}

HostKVAllocationView HostKVAllocationView::subview(std::uint32_t begin, std::uint32_t count) const {
    if (!valid() || count == 0 || begin > page_count_ || count > page_count_ - begin) {
        throw std::out_of_range("Host KV subview is outside its allocation");
    }
    return HostKVAllocationView(
        handle_, data_ + static_cast<std::size_t>(begin) * layout_->page_stride, layout_, count);
}

bool HostKVAllocationConstView::valid() const noexcept {
    return handle_.owner_ != nullptr && handle_.owner_->valid_handle(handle_);
}

const HostKVPageLayout& HostKVAllocationConstView::layout() const {
    if (!valid() || layout_ == nullptr) { throw std::logic_error("Host KV view is stale"); }
    return *layout_;
}

HostKVAllocationConstView HostKVAllocationConstView::subview(std::uint32_t begin,
                                                             std::uint32_t count) const {
    if (!valid() || count == 0 || begin > page_count_ || count > page_count_ - begin) {
        throw std::out_of_range("Host KV subview is outside its allocation");
    }
    return HostKVAllocationConstView(
        handle_, data_ + static_cast<std::size_t>(begin) * layout_->page_stride, layout_, count);
}

HostKVAllocation::~HostKVAllocation() { (void)release(); }

HostKVAllocation::HostKVAllocation(HostKVAllocation&& other) noexcept
    : owner_(other.owner_), descriptor_(other.descriptor_), generation_(other.generation_) {
    other.disarm();
}

HostKVAllocation& HostKVAllocation::operator=(HostKVAllocation&& other) noexcept {
    if (this == &other) { return *this; }
    (void)release();
    owner_      = other.owner_;
    descriptor_ = other.descriptor_;
    generation_ = other.generation_;
    other.disarm();
    return *this;
}

HostKVAllocationHandle HostKVAllocation::handle() const noexcept {
    return valid() ? HostKVAllocationHandle(owner_, descriptor_, generation_)
                   : HostKVAllocationHandle();
}

std::uint32_t HostKVAllocation::page_count() const noexcept {
    if (!valid() || descriptor_ >= owner_->descriptors_.size()) { return 0; }
    const HostKVArena::Descriptor& descriptor = owner_->descriptors_[descriptor_];
    return descriptor.active && descriptor.generation == generation_ ? descriptor.pages : 0;
}

bool HostKVAllocation::release() noexcept {
    if (!valid()) { return false; }
    const bool released = owner_->release_descriptor(descriptor_, generation_);
    disarm();
    return released;
}

void HostKVAllocation::disarm() noexcept {
    owner_      = nullptr;
    descriptor_ = 0;
    generation_ = 0;
}

HostKVArena::HostKVArena(PinnedHostPool& pool, std::size_t initial_bytes, std::size_t span_bytes,
                         std::span<const HostKVPageLayout> supported_layouts)
    : pool_(&pool),
      span_growth_bytes_(span_bytes != 0U ? span_bytes : (initial_bytes != 0U ? initial_bytes : (1ULL << 20U))),
      layouts_(supported_layouts.begin(), supported_layouts.end()) {
    for (std::size_t index = 0; index < layouts_.size(); ++index) {
        const HostKVPageLayout planned = plan_host_kv_page_layout(layouts_[index].geometry);
        if (planned != layouts_[index]) {
            throw std::invalid_argument("Host KV arena received an inconsistent page layout");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (layouts_[previous] == layouts_[index]) {
                throw std::invalid_argument("Host KV arena contains a duplicate page layout");
            }
        }
    }
    if (initial_bytes == 0) { return; }
    if (layouts_.empty()) {
        throw std::invalid_argument("Non-empty Host KV arena requires supported page layouts");
    }
    // The FIRST span, pinned from the shared pool. Everything else arrives through `grow_span`, so the old
    // "one buffer sized at startup" is now just "the first of however many the engine ends up needing".
    if (!grow_span(initial_bytes)) {
        throw std::runtime_error("Host KV arena could not pin its initial span");
    }
}

bool HostKVArena::grow_bytes(std::size_t bytes, bool speculative) noexcept {
    return grow_span(bytes, speculative);
}

bool HostKVArena::grow_for(std::uint32_t pages, std::size_t page_stride) noexcept {
    if (pages == 0U || page_stride > std::numeric_limits<std::size_t>::max() / pages) { return false; }
    return grow_span(page_stride * static_cast<std::size_t>(pages));
}

std::size_t HostKVArena::smallest_stride() const noexcept {
    std::size_t stride = std::numeric_limits<std::size_t>::max();
    for (const HostKVPageLayout& layout : layouts_) { stride = std::min(stride, layout.page_stride); }
    return stride == std::numeric_limits<std::size_t>::max() ? 1U : stride;
}

std::size_t HostKVArena::descriptor_hint_for(std::size_t bytes) const noexcept {
    return bytes / smallest_stride();
}

std::size_t HostKVArena::span_bytes(std::uint32_t span) const noexcept {
    return span < spans_.size() ? spans_[span].bytes : 0U;
}

bool HostKVArena::grow_span(std::size_t bytes, bool speculative) noexcept {
    if (pool_ == nullptr || bytes == 0U) {
        ++growth_refusals_;
        return false;
    }
    const std::size_t wanted = std::max(bytes, span_growth_bytes_);
    auto allocation         = pool_->allocate(wanted, speculative);
    if (!allocation) {
        ++growth_refusals_;  // could not pin: a refusal the caller can see, not an exception
        return false;
    }
    const std::size_t actual = pool_->size_of(*allocation);  // what the pool really granted, not the request
    spans_.push_back(Span{*allocation, actual});
    capacity_bytes_ += actual;
    // Descriptors for the new span, so the arena can hand out the pages it just gained. Sized by the
    // SMALLEST stride, which is the most descriptors the span could ever need.
    const std::size_t additional = descriptor_hint_for(actual);
    if (descriptors_.size() + additional > std::numeric_limits<std::uint32_t>::max()) {
        // Cannot describe the span: give the memory straight back rather than holding a span we cannot carve.
        (void)pool_->release(*allocation);
        spans_.pop_back();
        capacity_bytes_ -= actual;
        ++growth_refusals_;
        return false;
    }
    const auto first_descriptor = static_cast<std::uint32_t>(descriptors_.size());
    descriptors_.resize(descriptors_.size() + additional);
    for (std::size_t index = additional; index > 0; --index) {
        free_descriptors_.push_back(first_descriptor + static_cast<std::uint32_t>(index - 1U));
    }
    insert_free_extent(FreeExtent{static_cast<std::uint32_t>(spans_.size() - 1U), 0U, actual});
    ++growth_count_;
    bump_revision();  // a plan made before this span exists is stale
    return true;
}

std::optional<std::uint32_t>
HostKVArena::find_layout(const HostKVPageLayout& layout) const noexcept {
    const auto it = std::find(layouts_.begin(), layouts_.end(), layout);
    if (it == layouts_.end()) { return std::nullopt; }
    return static_cast<std::uint32_t>(it - layouts_.begin());
}

const HostKVPageLayout* HostKVArena::layout_for(const KVPageGeometry& geometry) const noexcept {
    const auto layout =
        std::find_if(layouts_.begin(), layouts_.end(), [&](const HostKVPageLayout& candidate) {
            return candidate.geometry == geometry;
        });
    return layout == layouts_.end() ? nullptr : &*layout;
}

std::optional<HostKVAllocation> HostKVArena::allocate_growing(const HostKVPageLayout& layout,
                                                              std::uint32_t pages) noexcept {
    if (auto allocation = allocate(layout, pages); allocation) { return allocation; }
    // No span could satisfy it: pin one more from the shared pool and retry ONCE. A second failure is final --
    // retrying would spin, and the caller needs a definite answer to choose between demoting and evicting.
    if (layout.page_stride > std::numeric_limits<std::size_t>::max() / (pages == 0 ? 1U : pages)) {
        return std::nullopt;
    }
    if (!grow_span(layout.page_stride * static_cast<std::size_t>(pages == 0 ? 1U : pages))) {
        return std::nullopt;
    }
    return allocate(layout, pages);
}

std::size_t HostKVArena::shortfall_for(std::uint32_t pages, std::size_t page_stride) const noexcept {
    if (pages == 0U || page_stride > std::numeric_limits<std::size_t>::max() / pages) { return 0U; }
    const std::size_t bytes = page_stride * static_cast<std::size_t>(pages);
    const auto        best  = find_free_extent(bytes);
    if (best) { return 0U; }
    // The largest run that exists, per span: what is missing is the difference. NO PRODUCTION CALLER:
    // nothing pre-grows from this today, so the "blocked plan becomes affordable" path it was written for
    // does not exist -- the planner pre-grow was tried and removed (see the header). Tests read it only.
    std::size_t largest = 0U;
    for (const FreeExtent& extent : free_extents_) { largest = std::max(largest, extent.bytes); }
    return bytes > largest ? bytes - largest : bytes;
}

std::optional<std::size_t> HostKVArena::find_free_extent(std::size_t bytes) const noexcept {
    for (std::size_t index = 0; index < free_extents_.size(); ++index) {
        if (free_extents_[index].bytes >= bytes) { return index; }
    }
    return std::nullopt;
}

bool HostKVArena::can_allocate(const HostKVPageLayout& layout, std::uint32_t pages) const noexcept {
    if (pages == 0 || free_descriptors_.empty() || !find_layout(layout) ||
        layout.page_stride > std::numeric_limits<std::size_t>::max() / pages) {
        return false;
    }
    return find_free_extent(layout.page_stride * static_cast<std::size_t>(pages)).has_value();
}

std::optional<HostKVAllocation> HostKVArena::allocate(const HostKVPageLayout& layout,
                                                      std::uint32_t pages) noexcept {
    const std::optional<std::uint32_t> layout_index = find_layout(layout);
    if (!layout_index || pages == 0 || free_descriptors_.empty() ||
        layout.page_stride > std::numeric_limits<std::size_t>::max() / pages) {
        return std::nullopt;
    }
    const std::size_t bytes = layout.page_stride * static_cast<std::size_t>(pages);
    const std::optional<std::size_t> free_index = find_free_extent(bytes);
    if (!free_index) { return std::nullopt; }

    const std::uint32_t descriptor_index = take_descriptor();
    if (descriptor_index == std::numeric_limits<std::uint32_t>::max()) { return std::nullopt; }
    FreeExtent& free             = free_extents_[*free_index];
    const std::uint32_t span     = free.span;  // read BEFORE the extent may be erased below
    const std::size_t offset     = free.offset;
    free.offset += bytes;
    free.bytes -= bytes;
    if (free.bytes == 0) {
        free_extents_.erase(free_extents_.begin() + static_cast<std::ptrdiff_t>(*free_index));
    }

    Descriptor& descriptor = descriptors_[descriptor_index];
    descriptor.span        = span;
    descriptor.offset      = offset;
    descriptor.bytes       = bytes;
    descriptor.layout      = *layout_index;
    descriptor.pages       = pages;
    descriptor.active      = true;
    occupied_bytes_ += bytes;
    bump_revision();
    return HostKVAllocation(*this, descriptor_index, descriptor.generation);
}

std::optional<HostKVAllocationRecipe> HostKVArena::plan_after_releases(
    std::span<const HostKVAllocationHandle> proposed_releases,
    std::span<const HostKVAllocationRequest> target_allocations) const {
    if (proposed_releases.empty() && target_allocations.empty()) { return std::nullopt; }
    if (target_allocations.size() > free_descriptors_.size() + proposed_releases.size()) {
        return std::nullopt;
    }

    HostKVAllocationRecipe recipe;
    recipe.owner_          = this;
    recipe.arena_revision_ = revision_;
    recipe.releases_.reserve(proposed_releases.size());
    recipe.targets_.reserve(target_allocations.size());

    std::vector<FreeExtent> simulated = free_extents_;
    // SPAN-AWARE through the SAME helper the real free list uses, so the two cannot drift. It has to be:
    // extents in different spans are never adjacent, so an offset-only ordering would merge two pool
    // extents into one simulated run and then hand out memory that is not contiguous. (This compiled while
    // being wrong, because `span` default-initialises -- the compiler cannot see this class of mistake.)
    const auto insert_extent = [&](FreeExtent extent) {
        insert_extent_ordered(simulated, extent);
    };

    for (std::size_t index = 0; index < proposed_releases.size(); ++index) {
        const HostKVAllocationHandle handle = proposed_releases[index];
        if (!valid_handle(handle) ||
            std::find(proposed_releases.begin(),
                      proposed_releases.begin() + static_cast<std::ptrdiff_t>(index),
                      handle) != proposed_releases.begin() + static_cast<std::ptrdiff_t>(index)) {
            return std::nullopt;
        }
        const Descriptor& descriptor = descriptors_[handle.descriptor_];
        // Span FIRST. `FreeExtent` gained a span field, and the two-argument form that stood here
        // aggregate-initialised span=offset, offset=bytes, bytes=0 -- so every released extent described zero
        // bytes and NO recipe could be planned. The compiler cannot see this: a short initializer list is
        // legal, not an error.
        insert_extent({descriptor.span, descriptor.offset, descriptor.bytes});
        recipe.releases_.push_back(handle);
    }

    for (const HostKVAllocationRequest& request : target_allocations) {
        if (request.layout == nullptr || request.pages == 0) { return std::nullopt; }
        const std::optional<std::uint32_t> layout_index = find_layout(*request.layout);
        if (!layout_index ||
            request.layout->page_stride > std::numeric_limits<std::size_t>::max() / request.pages) {
            return std::nullopt;
        }
        const std::size_t bytes =
            request.layout->page_stride * static_cast<std::size_t>(request.pages);
        const auto extent =
            std::find_if(simulated.begin(), simulated.end(),
                         [&](const FreeExtent& free) { return free.bytes >= bytes; });
        if (extent == simulated.end()) { return std::nullopt; }
        const std::uint32_t span   = extent->span;
        const std::size_t   offset = extent->offset;
        extent->offset += bytes;
        extent->bytes -= bytes;
        if (extent->bytes == 0) { simulated.erase(extent); }
        recipe.targets_.push_back(HostKVAllocationRecipe::Target{
            .layout = *layout_index,
            .pages  = request.pages,
            .span   = span,
            .offset = offset,
            .bytes  = bytes,
        });
    }
    return recipe;
}

HostKVArena::Placement HostKVArena::simulate_after_suballocation_releases(
    std::span<const HostKVSuballocationRelease> proposed_releases,
    std::span<const HostKVAllocationRequest> target_allocations) const {
    Placement out;
    out.simulated = free_extents_;
    if (proposed_releases.empty() && target_allocations.empty()) {
        out.valid = true;
        return out;
    }

    // SPAN-AWARE, through the SAME helper: this lambda was offset-only and coalesced across span
    // boundaries, so it declared 8 contiguous pages available across two 4-page spans and a plan
    // was called affordable that could not be allocated.
    const auto insert_extent = [&](FreeExtent extent) {
        insert_extent_ordered(out.simulated, extent);
    };

    for (std::size_t index = 0; index < proposed_releases.size(); ++index) {
        const HostKVSuballocationRelease& release = proposed_releases[index];
        if (!valid_handle(release.allocation) || release.page_count == 0) { return out; }
        const Descriptor& descriptor = descriptors_[release.allocation.descriptor_];
        if (release.begin_page > descriptor.pages ||
            release.page_count > descriptor.pages - release.begin_page) {
            return out;
        }
        const std::uint32_t end = release.begin_page + release.page_count;
        for (std::size_t prior = 0; prior < index; ++prior) {
            const HostKVSuballocationRelease& other = proposed_releases[prior];
            if (other.allocation != release.allocation) { continue; }
            const std::uint32_t other_end = other.begin_page + other.page_count;
            if (release.begin_page < other_end && other.begin_page < end) { return out; }
        }
        const std::size_t stride = layouts_[descriptor.layout].page_stride;
        insert_extent(FreeExtent{
            // Designated initialisers hide the other trap: a missing `.span` defaults to 0, which is correct
            // only for a single-span arena and silently wrong for a grown one.
            .span   = descriptor.span,
            .offset = checked_add(descriptor.offset,
                                  checked_mul(static_cast<std::size_t>(release.begin_page), stride,
                                              "Host KV suballocation release offset overflow"),
                                  "Host KV suballocation release offset overflow"),
            .bytes  = checked_mul(static_cast<std::size_t>(release.page_count), stride,
                                  "Host KV suballocation release size overflow"),
        });
    }

    std::size_t available_descriptors = free_descriptors_.size();
    std::size_t required_descriptors  = target_allocations.size();
    for (std::size_t index = 0; index < proposed_releases.size(); ++index) {
        const HostKVAllocationHandle allocation = proposed_releases[index].allocation;
        bool first                              = true;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (proposed_releases[prior].allocation == allocation) {
                first = false;
                break;
            }
        }
        if (!first) { continue; }

        std::vector<std::pair<std::uint32_t, std::uint32_t>> intervals;
        for (const HostKVSuballocationRelease& release : proposed_releases) {
            if (release.allocation == allocation) {
                intervals.emplace_back(release.begin_page, release.begin_page + release.page_count);
            }
        }
        std::sort(intervals.begin(), intervals.end());
        const Descriptor& descriptor = descriptors_[allocation.descriptor_];
        std::uint32_t cursor         = 0;
        std::size_t retained_runs    = 0;
        for (const auto [begin, end] : intervals) {
            if (begin > cursor) { ++retained_runs; }
            cursor = end;
        }
        if (cursor < descriptor.pages) { ++retained_runs; }
        if (retained_runs == 0) {
            ++available_descriptors;
        } else if (retained_runs > 1) {
            required_descriptors += retained_runs - 1U;
        }
    }
    out.descriptors_short = required_descriptors > available_descriptors;

    for (const HostKVAllocationRequest& request : target_allocations) {
        if (request.layout == nullptr || request.pages == 0) { return out; }
        const std::optional<std::uint32_t> layout_index = find_layout(*request.layout);
        if (!layout_index ||
            request.layout->page_stride > std::numeric_limits<std::size_t>::max() / request.pages) {
            return out;
        }
        const std::size_t bytes =
            request.layout->page_stride * static_cast<std::size_t>(request.pages);
        const auto extent =
            std::find_if(out.simulated.begin(), out.simulated.end(),
                         [&](const FreeExtent& free) { return free.bytes >= bytes; });
        if (extent == out.simulated.end()) {
            // CONTINUE, do not bail. The boolean form stopped at the first request that did not fit, which
            // is all it needed; the tri-state needs the SHORTFALL, and that is a property of every
            // unsatisfied request. Each costs exactly one span, sized `max(bytes, span step)` -- the size
            // `grow_span` would pin -- because `HostKVExtentStore::prepare` grows one span per contiguous
            // allocation it makes and then re-checks inside `grow_span`.
            ++out.unsatisfied;
            out.unsatisfied_bytes += std::max(bytes, span_growth_bytes_);
            continue;
        }
        ++out.satisfied;
        extent->offset += bytes;
        extent->bytes -= bytes;
        if (extent->bytes == 0) { out.simulated.erase(extent); }
    }
    out.valid = true;
    return out;
}

bool HostKVArena::can_allocate_after_suballocation_releases(
    std::span<const HostKVSuballocationRelease> proposed_releases,
    std::span<const HostKVAllocationRequest> target_allocations) const {
    // A thin reading of the simulation above: one definition of the walk, two spellings of the question.
    const Placement placement =
        simulate_after_suballocation_releases(proposed_releases, target_allocations);
    return placement.valid && placement.unsatisfied == 0 && !placement.descriptors_short;
}

HostKVFitResult HostKVArena::fit_after_suballocation_releases(
    std::span<const HostKVSuballocationRelease> proposed_releases,
    std::span<const HostKVAllocationRequest> target_allocations,
    std::size_t growth_headroom_bytes) const {
    const Placement placement =
        simulate_after_suballocation_releases(proposed_releases, target_allocations);
    HostKVFitResult result;
    if (!placement.valid || placement.descriptors_short) {
        // Malformed input, or the arena's own descriptor table is exhausted. BOTH map to `Blocked`: the
        // first is not a fit question at all, and for the second -- a span does add descriptors, so it is
        // growth-recoverable in principle -- this query does not model the table growth and MUST NOT
        // promise room it cannot count. Conservative, and identical to today's behaviour.
        return result;
    }
    result.shortfall_bytes   = placement.unsatisfied_bytes;
    result.unsatisfied_spans = placement.unsatisfied;
    if (placement.unsatisfied == 0) {
        result.fit = HostKVFit::Pinned;
    } else if (placement.unsatisfied_bytes <= growth_headroom_bytes) {
        result.fit = HostKVFit::Growable;
    }
    return result;
}

std::size_t HostKVArena::largest_free_run_bytes() const noexcept {
    std::size_t best = 0;
    for (const FreeExtent& extent : free_extents_) { best = std::max(best, extent.bytes); }
    return best;
}

bool HostKVArena::apply_recipe(HostKVAllocationRecipe&& recipe,
                               std::span<HostKVAllocation* const> proposed_releases,
                               std::span<HostKVAllocation> target_allocations) noexcept {
    if (recipe.owner_ != this || recipe.arena_revision_ != revision_ ||
        recipe.releases_.size() != proposed_releases.size() ||
        recipe.targets_.size() != target_allocations.size()) {
        return false;
    }
    for (std::size_t index = 0; index < proposed_releases.size(); ++index) {
        const HostKVAllocation* allocation = proposed_releases[index];
        if (allocation == nullptr || allocation->handle() != recipe.releases_[index] ||
            !valid_handle(recipe.releases_[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < target_allocations.size(); ++index) {
        const HostKVAllocationRecipe::Target& target = recipe.targets_[index];
        if (target_allocations[index].valid() || target.layout >= layouts_.size() ||
            target.pages == 0 ||
            layouts_[target.layout].page_stride >
                std::numeric_limits<std::size_t>::max() / target.pages ||
            layouts_[target.layout].page_stride * static_cast<std::size_t>(target.pages) !=
                target.bytes) {
            return false;
        }
    }

    // All generations and outputs are validated before the first mutation. The recipe was minted
    // from this exact revision, so every operation below is an invariant-preserving adoption.
    for (HostKVAllocation* allocation : proposed_releases) {
        if (!allocation->release()) { std::terminate(); }
    }
    for (std::size_t index = 0; index < recipe.targets_.size(); ++index) {
        const HostKVAllocationRecipe::Target& target = recipe.targets_[index];
        std::optional<HostKVAllocation> allocation =
            allocate(layouts_[target.layout], target.pages);
        if (!allocation) { std::terminate(); }
        const Descriptor& descriptor = descriptors_[allocation->descriptor_];
        // The SPAN is part of the identity: same offset and bytes in a DIFFERENT pool extent is a different
        // piece of memory, and accepting it would apply a plan to memory it was not planned against.
        if (descriptor.span != target.span || descriptor.offset != target.offset ||
            descriptor.bytes != target.bytes) {
            std::terminate();
        }
        target_allocations[index] = std::move(*allocation);
    }
    recipe.owner_          = nullptr;
    recipe.arena_revision_ = 0;
    recipe.releases_.clear();
    recipe.targets_.clear();
    return true;
}

std::pair<HostKVAllocation, HostKVAllocation> HostKVArena::split(HostKVAllocation&& allocation,
                                                                 std::uint32_t page_offset) {
    if (!valid_handle(allocation.handle())) {
        throw std::invalid_argument("Cannot split a stale Host KV allocation");
    }
    Descriptor& original = descriptors_[allocation.descriptor_];
    if (page_offset == 0 || page_offset >= original.pages) {
        throw std::out_of_range("Host KV split must leave two non-empty allocations");
    }
    const std::uint32_t right_index = take_descriptor();
    if (right_index == std::numeric_limits<std::uint32_t>::max()) {
        throw std::logic_error("Host KV descriptor capacity invariant was violated");
    }

    const std::size_t stride        = layouts_[original.layout].page_stride;
    const std::uint32_t right_pages = original.pages - page_offset;
    Descriptor& right               = descriptors_[right_index];
    right.offset = original.offset + static_cast<std::size_t>(page_offset) * stride;
    right.bytes  = static_cast<std::size_t>(right_pages) * stride;
    right.layout = original.layout;
    right.pages  = right_pages;
    right.active = true;
    // The SPAN, which this missed: without it the right half keeps whatever span its recycled descriptor
    // last had, so its data pointer lands inside another live allocation and releasing it inserts a free
    // extent into the wrong span -- a double allocation. Reproduced by the 2026-09-26 review.
    right.span   = original.span;

    increment_generation(original.generation);
    original.pages                       = page_offset;
    original.bytes                       = static_cast<std::size_t>(page_offset) * stride;
    const std::uint32_t left_generation  = original.generation;
    const std::uint32_t right_generation = right.generation;
    const std::uint32_t left_index       = allocation.descriptor_;
    allocation.disarm();
    bump_revision();
    return {HostKVAllocation(*this, left_index, left_generation),
            HostKVAllocation(*this, right_index, right_generation)};
}

HostKVAllocationView HostKVArena::writable_view(HostKVAllocation& allocation) {
    if (!valid_handle(allocation.handle())) {
        throw std::invalid_argument("Cannot view a stale Host KV allocation");
    }
    const Descriptor& descriptor = descriptors_[allocation.descriptor_];
    return HostKVAllocationView(allocation.handle(), allocation_data(descriptor),
                                &layouts_[descriptor.layout], descriptor.pages);
}

HostKVAllocationConstView HostKVArena::view(const HostKVAllocation& allocation) const {
    if (!valid_handle(allocation.handle())) {
        throw std::invalid_argument("Cannot view a stale Host KV allocation");
    }
    const Descriptor& descriptor = descriptors_[allocation.descriptor_];
    return HostKVAllocationConstView(allocation.handle(), allocation_data(descriptor),
                                     &layouts_[descriptor.layout], descriptor.pages);
}

bool HostKVArena::valid_handle(HostKVAllocationHandle handle) const noexcept {
    if (handle.owner_ != this || handle.descriptor_ >= descriptors_.size()) { return false; }
    const Descriptor& descriptor = descriptors_[handle.descriptor_];
    return descriptor.active && descriptor.generation == handle.generation_;
}

std::uint32_t HostKVArena::take_descriptor() noexcept {
    if (free_descriptors_.empty()) { return std::numeric_limits<std::uint32_t>::max(); }
    const std::uint32_t out = free_descriptors_.back();
    free_descriptors_.pop_back();
    return out;
}

bool HostKVArena::release_descriptor(std::uint32_t descriptor_index,
                                     std::uint32_t generation) noexcept {
    if (descriptor_index >= descriptors_.size()) { return false; }
    Descriptor& descriptor = descriptors_[descriptor_index];
    if (!descriptor.active || descriptor.generation != generation) { return false; }

    // Span FIRST, and this one mattered most: the two-argument form aggregate-initialised
    // span=offset, offset=bytes, bytes=0, so EVERY release inserted a zero-byte extent at a bogus span. The
    // simulation in `plan_after_releases` then disagreed with reality, recipes were "not planned", and once
    // that was fixed the adoption hit this and terminated. A short initializer list is legal C++, not an
    // error -- the compiler could not see either site.
    const FreeExtent released{descriptor.span, descriptor.offset, descriptor.bytes};
    occupied_bytes_ -= descriptor.bytes;
    descriptor.active = false;
    descriptor.offset = 0;
    descriptor.bytes  = 0;
    descriptor.pages  = 0;
    increment_generation(descriptor.generation);
    free_descriptors_.push_back(descriptor_index);
    insert_free_extent(released);
    bump_revision();
    return true;
}

void HostKVArena::insert_extent_ordered(std::vector<FreeExtent>& extents, FreeExtent extent) noexcept {
    // Ordering key is (SPAN, offset): extents in different spans are never adjacent, however their offsets
    // compare, so coalescing across a span boundary would merge two pool extents into one run and hand out
    // memory that is not contiguous.
    const auto before = [](const FreeExtent& candidate, const FreeExtent& value) {
        return candidate.span != value.span ? candidate.span < value.span : candidate.offset < value.offset;
    };
    auto inserted = extents.insert(std::lower_bound(extents.begin(), extents.end(), extent, before), extent);
    if (inserted != extents.begin()) {
        auto previous = inserted - 1;
        if (previous->span == inserted->span && previous->offset + previous->bytes == inserted->offset) {
            previous->bytes += inserted->bytes;
            inserted = extents.erase(inserted);
            inserted = previous;
        }
    }
    const auto next = inserted + 1;
    if (next != extents.end() && next->span == inserted->span &&
        inserted->offset + inserted->bytes == next->offset) {
        inserted->bytes += next->bytes;
        extents.erase(next);
    }
}

void HostKVArena::insert_free_extent(FreeExtent extent) noexcept {
    insert_extent_ordered(free_extents_, extent);
}

std::byte* HostKVArena::allocation_data(const Descriptor& descriptor) const noexcept {
    if (pool_ == nullptr || descriptor.span >= spans_.size()) { return nullptr; }
    // Offset is SPAN-RELATIVE: a run never straddles two spans, so the address is always
    // (this span's pool extent) + offset. The pool never moves an extent, which is what makes this stable.
    return pool_->data(spans_[descriptor.span].allocation) + descriptor.offset;
}

void HostKVArena::bump_revision() noexcept {
    ++revision_;
    if (revision_ == 0) { ++revision_; }
}

} // namespace ninfer
