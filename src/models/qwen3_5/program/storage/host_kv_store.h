#pragma once

#include "core/host_kv_arena.h"
#include "models/qwen3_5/program/storage/kv_store.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ninfer::models::qwen3_5::detail {

class HostKVExtentStore;

struct HostKVPageReplicaRelease {
    LogicalKVPageStore* pages = nullptr;
    LogicalKVPageHandle page;
};

class HostKVExtentReservation {
public:
    HostKVExtentReservation() noexcept = default;
    ~HostKVExtentReservation();

    HostKVExtentReservation(HostKVExtentReservation&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), descriptor_(other.descriptor_),
          generation_(other.generation_), page_store_(other.page_store_) {}

    HostKVExtentReservation& operator=(HostKVExtentReservation&&)      = delete;
    HostKVExtentReservation(const HostKVExtentReservation&)            = delete;
    HostKVExtentReservation& operator=(const HostKVExtentReservation&) = delete;

    [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

private:
    HostKVExtentStore* owner_       = nullptr;
    std::uint32_t descriptor_       = 0;
    std::uint32_t generation_       = 0;
    LogicalKVPageStore* page_store_ = nullptr;

    friend class HostKVExtentStore;
};

// Owns typed Host extents and the ordered logical pages represented by each allocation. It has no
// checkpoint, retention, or scheduling policy.
class HostKVExtentStore {
public:
    HostKVExtentStore(HostKVArena& arena, std::uint32_t descriptor_capacity)
        : arena_(&arena), extents_(descriptor_capacity), free_(descriptor_capacity),
          memberships_(descriptor_capacity),
          release_marks_(descriptor_capacity), extent_marks_(descriptor_capacity) {
        if (descriptor_capacity == 0) {
            throw std::invalid_argument("Host KV extent descriptor capacity is zero");
        }
        // The two free lists are STACKS (push_back/pop_back). They used to be pre-sized and written by
        // index, which is only valid while the vector never resizes -- and these tables now grow with the
        // arena (`reserve_capacity`), so the indexed form would write past the end exactly as
        // `HostStatePool`'s did before a test caught it handing out a slot twice.
        free_.reserve(descriptor_capacity);
        free_memberships_.reserve(descriptor_capacity);
        for (std::uint32_t index = 0; index < descriptor_capacity; ++index) {
            free_.push_back(descriptor_capacity - 1U - index);
            free_memberships_.push_back(descriptor_capacity - 1U - index);
        }
        affected_extents_.reserve(descriptor_capacity);
        extent_scan_scratch_.reserve(descriptor_capacity);
        partition_runs_.reserve(descriptor_capacity);
        suballocation_scratch_.reserve(descriptor_capacity);
    }

    HostKVExtentStore(const HostKVExtentStore&)            = delete;
    HostKVExtentStore& operator=(const HostKVExtentStore&) = delete;
    HostKVExtentStore(HostKVExtentStore&&)                 = delete;
    HostKVExtentStore& operator=(HostKVExtentStore&&)      = delete;

    [[nodiscard]] std::uint32_t capacity() const noexcept {
        return static_cast<std::uint32_t>(extents_.size());
    }

    // Grow the tables to cover the arena's CURRENT capacity. Called wherever the arena may have grown: a
    // growth that extended the arena and not this store would either throw in `partition_extent` or hand out
    // pages the store cannot describe.
    [[nodiscard]] bool ensure_capacity_for(const HostKVArena& arena) noexcept {
        const std::size_t needed = arena.descriptor_hint_for(arena.capacity_bytes());
        if (needed <= extents_.size()) { return true; }
        const std::size_t missing = needed - extents_.size();
        if (missing > std::numeric_limits<std::uint32_t>::max()) { return false; }
        return reserve_capacity(static_cast<std::uint32_t>(missing));
    }

    // Grow the tables so the arena can hold more host memory than the capacity it started with. Every table
    // is index-addressed, so appending is safe; the marks resize to 0, which reads as "not marked" because
    // `release_stamp_` starts at 1 and is never 0. The two free lists are stacks and are APPENDED TO.
    //
    // Without this, growth shows up as `partition_extent`'s hard throw ("Host KV partition descriptor
    // capacity is exhausted") -- a failure inside the address-space teardown path, at the worst moment.
    [[nodiscard]] bool reserve_capacity(std::uint32_t additional_extents) noexcept {
        if (additional_extents == 0U) { return true; }
        const std::size_t target = extents_.size() + additional_extents;
        if (target > std::numeric_limits<std::uint32_t>::max()) { return false; }
        try {
            const auto begin = static_cast<std::uint32_t>(extents_.size());
            extents_.resize(target);
            memberships_.resize(target);
            release_marks_.resize(target, 0U);
            extent_marks_.resize(target, 0U);
            free_.reserve(target);
            free_memberships_.reserve(target);
            affected_extents_.reserve(target);
            extent_scan_scratch_.reserve(target);
            partition_runs_.reserve(target);
            suballocation_scratch_.reserve(target);
            for (auto index = begin; index < static_cast<std::uint32_t>(target); ++index) {
                free_.push_back(index);
                free_memberships_.push_back(index);
            }
        } catch (...) {
            return false;  // a failed reserve leaves the tables usable, just not larger
        }
        return true;
    }

    [[nodiscard]] std::uint32_t occupied() const noexcept {
        return capacity() - static_cast<std::uint32_t>(free_.size());
    }

    [[nodiscard]] const HostKVPageLayout& page_layout(const LogicalKVPageStore& pages) const {
        const HostKVPageLayout* layout = arena_->layout_for(pages.physical_pool().geometry());
        if (layout == nullptr) {
            throw std::invalid_argument("Host KV page store has no arena layout");
        }
        return *layout;
    }

    [[nodiscard]] std::optional<HostKVExtentReservation>
    prepare(LogicalKVPageStore& pages, std::span<const LogicalKVPageHandle> membership) {
        if (membership.empty() || free_.empty() || membership.size() > free_memberships_.size()) {
            return std::nullopt;
        }
        for (const LogicalKVPageHandle page : membership) {
            if (!pages.can_pin_source(page) || pages.host_resident(page)) { return std::nullopt; }
        }

        const HostKVPageLayout& layout = page_layout(pages);
        // GROWING, and only here: this is the KV demote path, the one place that should be able to take room
        // the state pool is not using rather than failing and letting the caller evict. The arena and this
        // store's tables grow TOGETHER -- that pairing is the whole reason this is one method and not two
        // calls at the call site.
        std::optional<HostKVAllocation> allocation =
            arena_->allocate(layout, static_cast<std::uint32_t>(membership.size()));
        if (!allocation) {
            if (!arena_->grow_for(static_cast<std::uint32_t>(membership.size()), layout.page_stride) ||
                !ensure_capacity_for(*arena_)) {
                return std::nullopt;
            }
            allocation = arena_->allocate(layout, static_cast<std::uint32_t>(membership.size()));
            if (!allocation) { return std::nullopt; }
        }

        const std::uint32_t descriptor = free_.back();
        free_.pop_back();
        Extent& extent                 = extents_[descriptor];
        if (extent.state != ExtentState::Free) { std::terminate(); }
        extent.state      = ExtentState::Reserved;
        extent.page_store = &pages;
        extent.allocation = std::move(allocation);
        for (const LogicalKVPageHandle page : membership) {
            const std::uint32_t node = take_membership();
            if (node == kInvalidIndex) { std::terminate(); }
            Membership& entry = memberships_[node];
            entry.page        = page;
            entry.epoch       = pages.content_epoch(page);
            entry.coverage    = pages.committed_columns(page);
            entry.extent      = descriptor;
            entry.offset      = extent.page_count;
            entry.next        = kInvalidIndex;
            if (extent.tail == kInvalidIndex) {
                extent.head = node;
            } else {
                memberships_[extent.tail].next = node;
            }
            extent.tail = node;
            ++extent.page_count;
        }
        for (const LogicalKVPageHandle page : membership) { pages.pin_source(page); }

        HostKVExtentReservation reservation;
        reservation.owner_      = this;
        reservation.descriptor_ = descriptor;
        reservation.generation_ = extent.generation;
        reservation.page_store_ = &pages;
        return reservation;
    }

    [[nodiscard]] HostKVAllocationView writable_view(HostKVExtentReservation& reservation) {
        validate(reservation);
        return arena_->writable_view(*extents_[reservation.descriptor_].allocation);
    }

    [[nodiscard]] std::vector<DeviceKVPageHandle>
    device_sources(const HostKVExtentReservation& reservation) const {
        validate(reservation);
        std::vector<DeviceKVPageHandle> out;
        out.resize(extents_[reservation.descriptor_].page_count);
        device_sources(reservation, out);
        return out;
    }

    void device_sources(const HostKVExtentReservation& reservation,
                        std::span<DeviceKVPageHandle> out) const {
        validate(reservation);
        const Extent& extent = extents_[reservation.descriptor_];
        if (out.size() != extent.page_count) {
            throw std::invalid_argument("Host KV device-source output has the wrong size");
        }
        std::uint32_t node = extent.head;
        for (std::size_t index = 0; index < out.size(); ++index) {
            if (node == kInvalidIndex) { std::terminate(); }
            out[index] = extent.page_store->physical(memberships_[node].page);
            node       = memberships_[node].next;
        }
        if (node != kInvalidIndex) { std::terminate(); }
    }

    [[nodiscard]] std::uint32_t page_count(const HostKVExtentReservation& reservation) const {
        validate(reservation);
        return extents_[reservation.descriptor_].page_count;
    }

    [[nodiscard]] HostKVExtentCapability publish(HostKVExtentReservation&& reservation) noexcept {
        if (!valid(reservation)) { std::terminate(); }
        Extent& extent     = extents_[reservation.descriptor_];
        std::uint32_t node = extent.head;
        for (std::uint32_t index = 0; index < extent.page_count; ++index) {
            if (node == kInvalidIndex) { std::terminate(); }
            const Membership& entry = memberships_[node];
            if (!extent.page_store->can_attach_host_replica(entry.page, entry.epoch,
                                                            entry.coverage)) {
                std::terminate();
            }
            node = entry.next;
        }
        if (node != kInvalidIndex) { std::terminate(); }
        extent.state = ExtentState::Published;
        const HostKVExtentCapability capability(this, reservation.descriptor_, extent.generation);
        node = extent.head;
        for (std::uint32_t index = 0; index < extent.page_count; ++index) {
            Membership& entry = memberships_[node];
            try {
                extent.page_store->attach_host_replica(
                    entry.page, HostKVPageReplica{.extent            = capability,
                                                  .page_offset       = index,
                                                  .membership_node   = node,
                                                  .content_epoch     = entry.epoch,
                                                  .committed_columns = entry.coverage});
            } catch (...) { std::terminate(); }
            extent.page_store->unpin_source(entry.page);
            node = entry.next;
        }
        consume(reservation);
        return capability;
    }

    void abort(HostKVExtentReservation& reservation) noexcept {
        if (!valid(reservation)) {
            consume(reservation);
            return;
        }
        Extent& extent     = extents_[reservation.descriptor_];
        std::uint32_t node = extent.head;
        for (std::uint32_t index = 0; index < extent.page_count; ++index) {
            if (node == kInvalidIndex) { std::terminate(); }
            const LogicalKVPageHandle page = memberships_[node].page;
            if (!extent.page_store->valid(page) || extent.page_store->source_pins(page) == 0) {
                std::terminate();
            }
            try {
                extent.page_store->unpin_source(page);
            } catch (...) { std::terminate(); }
            node = memberships_[node].next;
        }
        release_descriptor(reservation.descriptor_, extent);
        consume(reservation);
    }

    [[nodiscard]] bool valid(HostKVExtentCapability capability) const noexcept {
        return capability.owner_ == this && capability.index_ < extents_.size() &&
               extents_[capability.index_].state == ExtentState::Published &&
               extents_[capability.index_].generation == capability.generation_;
    }

    [[nodiscard]] HostKVAllocationConstView view(HostKVExtentCapability capability) const {
        const Extent& extent = require(capability);
        return arena_->view(*extent.allocation);
    }

    [[nodiscard]] bool release(HostKVExtentCapability capability) noexcept {
        if (!valid(capability)) { return false; }
        Extent& extent     = extents_[capability.index_];
        std::uint32_t node = extent.head;
        for (std::uint32_t index = 0; index < extent.page_count; ++index) {
            if (node == kInvalidIndex) { return false; }
            const Membership& membership   = memberships_[node];
            const LogicalKVPageHandle page = membership.page;
            const HostKVPageReplica replica =
                extent.page_store->valid(page) && extent.page_store->host_resident(page)
                    ? extent.page_store->host_replica(page)
                    : HostKVPageReplica{};
            if (!extent.page_store->valid(page) || !extent.page_store->host_resident(page) ||
                replica.extent != capability || replica.page_offset != index ||
                replica.membership_node != node || membership.extent != capability.index_ ||
                membership.offset != index ||
                (!extent.page_store->device_resident(page) &&
                 extent.page_store->address_references(page) != 0)) {
                return false;
            }
            node = memberships_[node].next;
        }
        if (node != kInvalidIndex) { return false; }
        node = extent.head;
        for (std::uint32_t index = 0; index < extent.page_count; ++index) {
            const LogicalKVPageHandle page = memberships_[node].page;
            if (!extent.page_store->detach_host_replica(page, capability)) { std::terminate(); }
            node = memberships_[node].next;
        }
        release_descriptor(capability.index_, extent);
        return true;
    }

    [[nodiscard]] bool can_release_page_replica(LogicalKVPageStore& pages,
                                                LogicalKVPageHandle page) const noexcept {
        if (!pages.valid(page) || !pages.host_resident(page) || pages.source_pins(page) != 0 ||
            (!pages.device_resident(page) && pages.address_references(page) != 0)) {
            return false;
        }
        const HostKVPageReplica replica = pages.host_replica(page);
        if (!valid(replica.extent)) { return false; }
        const Extent& extent     = extents_[replica.extent.index_];
        const std::uint32_t node = replica.membership_node;
        if (extent.page_store != &pages || replica.page_offset >= extent.page_count ||
            node >= memberships_.size() || memberships_[node].page != page ||
            memberships_[node].extent != replica.extent.index_ ||
            memberships_[node].offset != replica.page_offset) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool
    can_release_page_replicas(std::span<const HostKVPageReplicaRelease> releases) const noexcept {
        begin_release_marks();
        for (const HostKVPageReplicaRelease& release : releases) {
            if (release.pages == nullptr || !mark_release(*release.pages, release.page, false)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool
    can_release_page_replicas(LogicalKVPageStore& pages,
                              std::span<const LogicalKVPageHandle> releases) const noexcept {
        begin_release_marks();
        for (const LogicalKVPageHandle release : releases) {
            if (!mark_release(pages, release, false)) { return false; }
        }
        return true;
    }

    [[nodiscard]] bool
    can_allocate_after_page_releases(std::span<const HostKVPageReplicaRelease> releases,
                                     std::span<const HostKVAllocationRequest> allocations) const {
        return can_allocate_after_page_releases(releases, {}, allocations);
    }

    // Simulates both immediately droppable Host duplicates and Host replicas that become
    // unreferenced when a transaction removes their last address-space membership.
    [[nodiscard]] bool can_allocate_after_page_releases(
        std::span<const HostKVPageReplicaRelease> releases,
        std::span<const HostKVPageReplicaRelease> last_reference_releases,
        std::span<const HostKVAllocationRequest> allocations) const {
        begin_release_marks();
        suballocation_scratch_.clear();
        const auto append = [&](const HostKVPageReplicaRelease& release) {
            if (release.pages == nullptr) { return false; }
            const HostKVPageReplica replica = release.pages->host_replica(release.page);
            const Extent& extent            = require(replica.extent);
            if (!extent.allocation) { return false; }
            suballocation_scratch_.push_back(HostKVSuballocationRelease{
                .allocation = extent.allocation->handle(),
                .begin_page = replica.page_offset,
                .page_count = 1,
            });
            return true;
        };
        for (const HostKVPageReplicaRelease& release : releases) {
            if (release.pages == nullptr || !mark_release(*release.pages, release.page, false) ||
                !append(release)) {
                return false;
            }
        }
        for (const HostKVPageReplicaRelease& release : last_reference_releases) {
            if (release.pages == nullptr || !mark_release(*release.pages, release.page, true) ||
                !append(release)) {
                return false;
            }
        }
        return arena_->can_allocate_after_suballocation_releases(suballocation_scratch_,
                                                                 allocations);
    }

    [[nodiscard]] bool release_page_replicas(std::span<const HostKVPageReplicaRelease> releases) {
        if (!can_release_page_replicas(releases)) { return false; }
        release_marked_extents();
        return true;
    }

    [[nodiscard]] bool release_page_replicas(LogicalKVPageStore& pages,
                                             std::span<const LogicalKVPageHandle> releases) {
        if (!can_release_page_replicas(pages, releases)) { return false; }
        release_marked_extents();
        return true;
    }

    // Address-space teardown can leave part of an extent with no logical owner. Partition each
    // affected extent once, release all zero-reference runs, and republish retained runs with
    // generation-checked capabilities. Descriptor capacity is provisioned per Host page.
    [[nodiscard]] std::size_t release_unreferenced() noexcept {
        std::size_t released_bytes = 0;
        try {
            extent_scan_scratch_.clear();
            for (std::uint32_t index = 0; index < extents_.size(); ++index) {
                const Extent& extent = extents_[index];
                if (extent.state == ExtentState::Published && extent.allocation &&
                    extent.page_store != nullptr) {
                    extent_scan_scratch_.push_back(index);
                }
            }
            for (const std::uint32_t index : extent_scan_scratch_) {
                const std::size_t bytes =
                    partition_extent(index, [](const LogicalKVPageStore& pages,
                                               LogicalKVPageHandle page, std::uint32_t) {
                        return pages.address_references(page) == 0 && pages.source_pins(page) == 0;
                    });
                if (bytes > std::numeric_limits<std::size_t>::max() - released_bytes) {
                    std::terminate();
                }
                released_bytes += bytes;
            }
        } catch (...) { std::terminate(); }
        return released_bytes;
    }

private:
    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    enum class ExtentState : std::uint8_t {
        Free,
        Reserved,
        Published,
    };

    struct Extent {
        ExtentState state              = ExtentState::Free;
        std::uint32_t generation       = 1;
        LogicalKVPageStore* page_store = nullptr;
        std::optional<HostKVAllocation> allocation;
        std::uint32_t head       = kInvalidIndex;
        std::uint32_t tail       = kInvalidIndex;
        std::uint32_t page_count = 0;
    };

    struct Membership {
        LogicalKVPageHandle page;
        std::uint64_t epoch    = 0;
        std::uint32_t coverage = 0;
        std::uint32_t extent   = kInvalidIndex;
        std::uint32_t offset   = 0;
        std::uint32_t next     = kInvalidIndex;
    };

    struct PartitionRun {
        std::uint32_t begin = 0;
        std::uint32_t head  = kInvalidIndex;
        std::uint32_t tail  = kInvalidIndex;
        std::uint32_t count = 0;
        bool release        = false;
    };

    [[nodiscard]] bool valid(const HostKVExtentReservation& reservation) const noexcept {
        if (reservation.owner_ != this || reservation.descriptor_ >= extents_.size() ||
            reservation.page_store_ == nullptr) {
            return false;
        }
        const Extent& extent = extents_[reservation.descriptor_];
        return extent.state == ExtentState::Reserved &&
               extent.generation == reservation.generation_ &&
               extent.page_store == reservation.page_store_ && extent.allocation &&
               extent.page_count != 0 && extent.head != kInvalidIndex &&
               extent.tail != kInvalidIndex;
    }

    void validate(const HostKVExtentReservation& reservation) const {
        if (!valid(reservation)) { throw std::logic_error("Host KV extent reservation is stale"); }
    }

    [[nodiscard]] Extent& require(HostKVExtentCapability capability) {
        if (!valid(capability)) { throw std::invalid_argument("Host KV extent is stale"); }
        return extents_[capability.index_];
    }

    [[nodiscard]] const Extent& require(HostKVExtentCapability capability) const {
        if (!valid(capability)) { throw std::invalid_argument("Host KV extent is stale"); }
        return extents_[capability.index_];
    }

    static void increment_generation(std::uint32_t& generation) noexcept {
        ++generation;
        if (generation == 0) { ++generation; }
    }

    [[nodiscard]] std::uint32_t take_membership() noexcept {
        if (free_memberships_.empty()) { return kInvalidIndex; }
        const std::uint32_t node = free_memberships_.back();
        free_memberships_.pop_back();
        return node;
    }

    void begin_release_marks() const noexcept {
        affected_extents_.clear();
        ++release_stamp_;
        if (release_stamp_ == 0) {
            std::fill(release_marks_.begin(), release_marks_.end(), 0);
            std::fill(extent_marks_.begin(), extent_marks_.end(), 0);
            release_stamp_ = 1;
        }
    }

    [[nodiscard]] bool mark_release(LogicalKVPageStore& pages, LogicalKVPageHandle page,
                                    bool last_reference) const noexcept {
        if (last_reference) {
            if (!pages.valid(page) || !pages.host_resident(page) || pages.source_pins(page) != 0 ||
                pages.address_references(page) != 1) {
                return false;
            }
        } else if (!can_release_page_replica(pages, page)) {
            return false;
        }
        const HostKVPageReplica replica = pages.host_replica(page);
        if (!valid(replica.extent) || replica.membership_node >= memberships_.size()) {
            return false;
        }
        const Membership& membership = memberships_[replica.membership_node];
        if (membership.page != page || membership.extent != replica.extent.index_ ||
            membership.offset != replica.page_offset ||
            replica.page_offset >= extents_[replica.extent.index_].page_count ||
            extents_[replica.extent.index_].page_store != &pages) {
            return false;
        }
        if (release_marks_[replica.membership_node] == release_stamp_) { return false; }
        release_marks_[replica.membership_node] = release_stamp_;
        if (extent_marks_[replica.extent.index_] != release_stamp_) {
            extent_marks_[replica.extent.index_] = release_stamp_;
            affected_extents_.push_back(replica.extent.index_);
        }
        return true;
    }

    void release_marked_extents() {
        for (const std::uint32_t index : affected_extents_) {
            (void)partition_extent(
                index, [&](const LogicalKVPageStore&, LogicalKVPageHandle, std::uint32_t node) {
                    return release_marks_[node] == release_stamp_;
                });
        }
    }

    template <typename Predicate>
    [[nodiscard]] std::size_t partition_extent(std::uint32_t index, Predicate&& should_release) {
        if (index >= extents_.size()) {
            throw std::logic_error("Host KV partition extent index is out of range");
        }
        Extent& original = extents_[index];
        if (original.state != ExtentState::Published || original.page_store == nullptr ||
            !original.allocation || original.page_count == 0 ||
            original.allocation->page_count() != original.page_count) {
            throw std::logic_error("Host KV extent is not partitionable");
        }
        LogicalKVPageStore* const pages = original.page_store;
        const HostKVExtentCapability old(this, index, original.generation);

        partition_runs_.clear();
        std::uint32_t node           = original.head;
        bool any_release             = false;
        std::uint32_t retained_runs  = 0;
        std::uint32_t released_pages = 0;
        for (std::uint32_t offset = 0; offset < original.page_count; ++offset) {
            if (node == kInvalidIndex || node >= memberships_.size()) {
                throw std::logic_error("Host KV extent membership is truncated");
            }
            const Membership& entry = memberships_[node];
            if (!pages->valid(entry.page) || !pages->host_resident(entry.page)) {
                throw std::logic_error("Host KV extent membership is stale");
            }
            const HostKVPageReplica replica = pages->host_replica(entry.page);
            if (entry.extent != index || entry.offset != offset || replica.extent != old ||
                replica.page_offset != offset || replica.membership_node != node ||
                replica.content_epoch != entry.epoch ||
                replica.committed_columns != entry.coverage) {
                throw std::logic_error("Host KV extent membership disagrees with its replica");
            }
            const bool release = should_release(*pages, entry.page, node);
            if (release && (pages->source_pins(entry.page) != 0 ||
                            (!pages->device_resident(entry.page) &&
                             pages->address_references(entry.page) != 0))) {
                throw std::logic_error("Host KV partition contains an unreleasable page");
            }
            if (partition_runs_.empty() || partition_runs_.back().release != release) {
                partition_runs_.push_back(PartitionRun{
                    .begin = offset, .head = node, .tail = node, .count = 1, .release = release});
                if (!release) { ++retained_runs; }
            } else {
                PartitionRun& run = partition_runs_.back();
                run.tail          = node;
                ++run.count;
            }
            if (release) {
                any_release = true;
                ++released_pages;
            }
            node = entry.next;
        }
        if (node != kInvalidIndex) {
            throw std::logic_error("Host KV extent membership exceeds its page count");
        }
        if (!any_release) { return 0; }
        const std::uint32_t additional_extents = retained_runs == 0 ? 0U : retained_runs - 1U;
        if (additional_extents > free_.size()) {
            throw std::logic_error("Host KV partition descriptor capacity is exhausted");
        }
        const std::size_t stride = page_layout(*pages).page_stride;
        if (released_pages > std::numeric_limits<std::size_t>::max() / stride) {
            throw std::overflow_error("Host KV released byte count overflow");
        }
        const std::size_t released_bytes = stride * static_cast<std::size_t>(released_pages);

        HostKVAllocation remaining = std::move(*original.allocation);
        original.allocation.reset();
        increment_generation(original.generation);
        bool original_assigned = false;
        for (std::size_t run_index = 0; run_index < partition_runs_.size(); ++run_index) {
            const PartitionRun& run = partition_runs_[run_index];
            HostKVAllocation allocation;
            if (run_index + 1U == partition_runs_.size()) {
                allocation = std::move(remaining);
            } else {
                auto split = arena_->split(std::move(remaining), run.count);
                allocation = std::move(split.first);
                remaining  = std::move(split.second);
            }
            memberships_[run.tail].next = kInvalidIndex;

            if (run.release) {
                std::uint32_t release_node = run.head;
                for (std::uint32_t offset = 0; offset < run.count; ++offset) {
                    if (release_node == kInvalidIndex) { std::terminate(); }
                    Membership& entry        = memberships_[release_node];
                    const std::uint32_t next = entry.next;
                    if (!pages->detach_host_replica(entry.page, old)) { std::terminate(); }
                    entry                                       = {};
                    free_memberships_.push_back(release_node);
                    release_node                                = next;
                }
                if (release_node != kInvalidIndex || !allocation.release()) { std::terminate(); }
                continue;
            }

            // The pop happens ONLY when the free list was read. (An earlier conversion of this line had the
            // condition negated: it popped when the list had NOT been read -- discarding a free descriptor --
            // and leaked one when it had.)
            std::uint32_t target_index = index;
            if (original_assigned) {
                target_index = free_.back();
                free_.pop_back();
            }
            Extent& target                   = extents_[target_index];
            if (target_index != index && target.state != ExtentState::Free) { std::terminate(); }
            original_assigned = true;
            target.state      = ExtentState::Published;
            target.page_store = pages;
            target.allocation.emplace(std::move(allocation));
            target.head       = run.head;
            target.tail       = run.tail;
            target.page_count = run.count;
            const HostKVExtentCapability replacement(this, target_index, target.generation);
            std::uint32_t retained_node = run.head;
            for (std::uint32_t offset = 0; offset < run.count; ++offset) {
                if (retained_node == kInvalidIndex) { std::terminate(); }
                Membership& entry        = memberships_[retained_node];
                const std::uint32_t next = entry.next;
                pages->rebind_host_replica(entry.page,
                                           HostKVPageReplica{.extent          = old,
                                                             .page_offset     = run.begin + offset,
                                                             .membership_node = retained_node,
                                                             .content_epoch   = entry.epoch,
                                                             .committed_columns = entry.coverage},
                                           HostKVPageReplica{.extent            = replacement,
                                                             .page_offset       = offset,
                                                             .membership_node   = retained_node,
                                                             .content_epoch     = entry.epoch,
                                                             .committed_columns = entry.coverage});
                entry.extent  = target_index;
                entry.offset  = offset;
                retained_node = next;
            }
            if (retained_node != kInvalidIndex) { std::terminate(); }
        }

        if (!original_assigned) {
            original.state      = ExtentState::Free;
            original.page_store = nullptr;
            original.allocation.reset();
            original.head        = kInvalidIndex;
            original.tail        = kInvalidIndex;
            original.page_count  = 0;
            free_.push_back(index);
        }
        return released_bytes;
    }

    void release_descriptor(std::uint32_t index, Extent& extent) noexcept {
        std::uint32_t node = extent.head;
        for (std::uint32_t offset = 0; offset < extent.page_count; ++offset) {
            if (node == kInvalidIndex) { std::terminate(); }
            const std::uint32_t next                    = memberships_[node].next;
            memberships_[node]                          = {};
            free_memberships_.push_back(node);
            node                                        = next;
        }
        if (node != kInvalidIndex) { std::terminate(); }
        extent.state      = ExtentState::Free;
        extent.page_store = nullptr;
        extent.allocation.reset();
        extent.head       = kInvalidIndex;
        extent.tail       = kInvalidIndex;
        extent.page_count = 0;
        increment_generation(extent.generation);
        free_.push_back(index);
    }

    static void consume(HostKVExtentReservation& reservation) noexcept {
        reservation.owner_      = nullptr;
        reservation.page_store_ = nullptr;
    }

    HostKVArena* arena_ = nullptr;
    std::vector<Extent> extents_;
    std::vector<std::uint32_t> free_;
    std::vector<Membership> memberships_;
    std::vector<std::uint32_t> free_memberships_;
    mutable std::vector<std::uint32_t> release_marks_;
    mutable std::vector<std::uint32_t> extent_marks_;
    mutable std::vector<std::uint32_t> affected_extents_;
    mutable std::vector<HostKVSuballocationRelease> suballocation_scratch_;
    mutable std::uint32_t release_stamp_ = 0;
    std::vector<std::uint32_t> extent_scan_scratch_;
    std::vector<PartitionRun> partition_runs_;
};

inline HostKVExtentReservation::~HostKVExtentReservation() {
    if (owner_ != nullptr) { owner_->abort(*this); }
}

} // namespace ninfer::models::qwen3_5::detail
