#pragma once

#include "models/qwen3_5/state/state_image.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ninfer::models::qwen3_5::detail {

class StateImageStore;

class StateImageHandle {
public:
    StateImageHandle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

    [[nodiscard]] friend bool operator==(StateImageHandle, StateImageHandle) noexcept = default;

private:
    StateImageHandle(const StateImageStore* owner, std::uint32_t index,
                     std::uint32_t generation) noexcept
        : owner_(owner), index_(index), generation_(generation) {}

    const StateImageStore* owner_ = nullptr;
    std::uint32_t index_          = 0;
    std::uint32_t generation_     = 0;

    friend class StateImageStore;
};

enum class StateImageRole : std::uint8_t {
    Free,
    ActiveMutable,
    CheckpointImmutable,
    ReservedDestination,
};

enum class StateReplicaResidency : std::uint8_t {
    None,
    DeviceOnly,
    HostOnly,
    Both,
};

enum class StateTransferDirection : std::uint8_t {
    DeviceToHost,
    HostToDevice,
    HostToFork,
};

struct StateImageSelectors {
    std::int32_t source      = -1;
    std::int32_t destination = -1;
};

class StateImageTransfer {
public:
    StateImageTransfer() noexcept = default;
    ~StateImageTransfer();

    StateImageTransfer(StateImageTransfer&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), id_(std::exchange(other.id_, 0)),
          direction_(other.direction_), source_(other.source_), destination_(other.destination_) {}

    StateImageTransfer& operator=(StateImageTransfer&&)      = delete;
    StateImageTransfer(const StateImageTransfer&)            = delete;
    StateImageTransfer& operator=(const StateImageTransfer&) = delete;

    [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

    [[nodiscard]] StateTransferDirection direction() const noexcept { return direction_; }

private:
    StateImageTransfer(StateImageStore& owner, std::uint64_t id, StateTransferDirection direction,
                       StateImageHandle source, StateImageHandle destination) noexcept
        : owner_(&owner), id_(id), direction_(direction), source_(source),
          destination_(destination) {}

    StateImageStore* owner_           = nullptr;
    std::uint64_t id_                 = 0;
    StateTransferDirection direction_ = StateTransferDirection::DeviceToHost;
    StateImageHandle source_;
    StateImageHandle destination_;

    friend class StateImageStore;
};

// Program-private logical StateImage ownership. Logical identity is independent from Device and
// Host replicas; published raw Tensor views are reconstructed only from an active Device binding.
class StateImageStore {
public:
    StateImageStore(qwen3_5::StateImageDevicePool& device, qwen3_5::HostStatePool* host,
                    std::uint32_t logical_capacity)
        : device_(&device), host_(host), objects_(logical_capacity),
          free_objects_(),
          free_device_slots_(static_cast<std::size_t>(device.slot_count())),
          free_device_count_(static_cast<std::uint32_t>(device.slot_count())) {
        // The check covers the INITIAL reservation; the table then FOLLOWS the host pool, because the pool is
        // elastic now and every host-resident image needs its own object entry -- without that the table, not
        // the pool, is the ceiling, and the elasticity buys nothing (`ensure_host_object_capacity`).
        if (logical_capacity == 0 || device.slot_count() <= 0 ||
            logical_capacity < static_cast<std::uint32_t>(device.slot_count()) ||
            (host != nullptr && logical_capacity < static_cast<std::uint32_t>(device.slot_count()) +
                                                       host->capacity())) {
            throw std::invalid_argument("StateImageStore capacity is inconsistent");
        }
        // A STACK, maintained with push_back/pop_back throughout. It used to be a pre-sized vector written by
        // index (`free_objects_[free_object_count_++] = ...`), which is only valid while the vector is never
        // resized -- and this table now grows. (The same pattern in HostStatePool produced a double
        // allocation when its container became dynamic; that is why this is converted rather than extended.)
        free_objects_.reserve(logical_capacity);
        for (std::uint32_t index = 0; index < logical_capacity; ++index) {
            free_objects_.push_back(logical_capacity - 1U - index);
        }
        for (std::uint32_t index = 0; index < free_device_slots_.size(); ++index) {
            free_device_slots_[index] =
                static_cast<std::int32_t>(free_device_slots_.size() - 1U - index);
        }
    }

    StateImageStore(const StateImageStore&)            = delete;
    StateImageStore& operator=(const StateImageStore&) = delete;
    StateImageStore(StateImageStore&&)                 = delete;
    StateImageStore& operator=(StateImageStore&&)      = delete;

    [[nodiscard]] std::uint32_t capacity() const noexcept {
        return static_cast<std::uint32_t>(objects_.size());
    }

    [[nodiscard]] std::uint32_t occupied() const noexcept {
        return capacity() - static_cast<std::uint32_t>(free_objects_.size());
    }

    [[nodiscard]] std::uint32_t device_occupied() const noexcept {
        return static_cast<std::uint32_t>(free_device_slots_.size()) - free_device_count_;
    }

    [[nodiscard]] std::uint32_t device_capacity() const noexcept {
        return static_cast<std::uint32_t>(free_device_slots_.size());
    }

    [[nodiscard]] std::uint32_t host_occupied() const noexcept {
        return host_ == nullptr ? 0U : host_->occupied();
    }

    [[nodiscard]] std::optional<StateImageHandle> reserve_destination() noexcept {
        return allocate(StateImageRole::ReservedDestination, true);
    }

    [[nodiscard]] std::optional<StateImageHandle> reserve_logical_destination() noexcept {
        return allocate(StateImageRole::ReservedDestination, false);
    }

    [[nodiscard]] std::optional<StateImageHandle> reserve_reset(cudaStream_t stream = nullptr) {
        std::optional<StateImageHandle> handle = allocate(StateImageRole::ActiveMutable, true);
        if (!handle) { return std::nullopt; }
        try {
            Object& object = require(*handle);
            device_->zero_slot(*object.device_slot, stream);
            object.content_epoch = next_epoch();
        } catch (...) {
            (void)release(*handle);
            throw;
        }
        return handle;
    }

    void activate_reset(StateImageHandle handle, cudaStream_t stream = nullptr) {
        Object& object = require(handle);
        if (object.role != StateImageRole::ReservedDestination || !object.device_slot ||
            object.source_pins != 0 || object.destination_pinned || has_pending_replica(object)) {
            throw std::logic_error("StateImage reset reservation is not activatable");
        }
        device_->zero_slot(*object.device_slot, stream);
        object.content_epoch = next_epoch();
        object.role          = StateImageRole::ActiveMutable;
    }

    // Zero a reserved Device replica without touching its role or content epoch. A fork
    // destination is filled by the ops that consume the selector pair, and an op that covers
    // only part of the image leaves the rest holding the previous occupant's bytes; a root
    // admission zeroes its slot for that reason (activate_reset) while a fork does not. This
    // is the fork-side equivalent, used to make a fork destination's untouched regions
    // deterministic instead of inheriting another sequence's state.
    void zero_reserved_device_replica(StateImageHandle handle, cudaStream_t stream = nullptr) {
        Object& object = require(handle);
        if (object.role != StateImageRole::ReservedDestination || !object.device_slot ||
            object.source_pins != 0 || object.destination_pinned || has_pending_replica(object)) {
            throw std::logic_error("StateImage reserved destination is not zeroable");
        }
        device_->zero_slot(*object.device_slot, stream);
    }

    [[nodiscard]] bool valid(StateImageHandle handle) const noexcept {
        return handle.owner_ == this && handle.index_ < objects_.size() &&
               objects_[handle.index_].role != StateImageRole::Free &&
               objects_[handle.index_].generation == handle.generation_;
    }

    [[nodiscard]] std::uint32_t descriptor_index(StateImageHandle handle) const {
        (void)require(handle);
        return handle.index_;
    }

    [[nodiscard]] StateImageRole role(StateImageHandle handle) const {
        return require(handle).role;
    }

    [[nodiscard]] StateReplicaResidency residency(StateImageHandle handle) const {
        const Object& object = require(handle);
        if (object.device_slot && object.host_slot) { return StateReplicaResidency::Both; }
        if (object.device_slot) { return StateReplicaResidency::DeviceOnly; }
        if (object.host_slot) { return StateReplicaResidency::HostOnly; }
        return StateReplicaResidency::None;
    }

    [[nodiscard]] std::uint64_t content_epoch(StateImageHandle handle) const {
        return require(handle).content_epoch;
    }

    [[nodiscard]] std::uint32_t source_pins(StateImageHandle handle) const {
        return require(handle).source_pins;
    }

    // Does THIS handle hold a slot in each tier? A pool total cannot answer "was this victim already
    // demoted", which is what separates a genuinely premature eviction from one where the host tier merely
    // had room -- and `demotable` is a pool comparison taken BEFORE release, so it counts the victim's own
    // space as free.
    [[nodiscard]] bool holds_host_slot(StateImageHandle handle) const noexcept {
        return valid(handle) && objects_[handle.index_].host_slot.has_value();
    }
    [[nodiscard]] bool holds_device_slot(StateImageHandle handle) const noexcept {
        return valid(handle) && objects_[handle.index_].device_slot.has_value();
    }

    [[nodiscard]] std::uint32_t checkpoint_references(StateImageHandle handle) const {
        return require(handle).checkpoint_references;
    }

    void retain_checkpoint_reference(StateImageHandle handle) {
        Object& object = require(handle);
        if (object.role != StateImageRole::CheckpointImmutable ||
            object.checkpoint_references == std::numeric_limits<std::uint32_t>::max()) {
            throw std::logic_error("StateImage checkpoint reference is not retainable");
        }
        ++object.checkpoint_references;
    }

    void release_checkpoint_reference(StateImageHandle handle) {
        Object& object = require(handle);
        if (object.role != StateImageRole::CheckpointImmutable ||
            object.checkpoint_references == 0) {
            throw std::logic_error("StateImage checkpoint reference is not releasable");
        }
        --object.checkpoint_references;
    }

    [[nodiscard]] std::int32_t physical_slot(StateImageHandle handle) const {
        const Object& object = require(handle);
        if (!object.device_slot) {
            throw std::logic_error("StateImage has no published Device replica");
        }
        return *object.device_slot;
    }

    void move_checkpoint_to_active(StateImageHandle handle) {
        Object& object = require(handle);
        if (object.role != StateImageRole::CheckpointImmutable || !object.device_slot ||
            object.checkpoint_references != 0 || object.source_pins != 0 ||
            object.destination_pinned || has_pending_replica(object)) {
            throw std::logic_error("StateImage checkpoint is not movable");
        }
        if (object.host_slot) {
            if (host_ == nullptr || !host_->release(*object.host_slot)) {
                throw std::logic_error("StateImage Host replica could not be consumed by Move");
            }
            object.host_slot.reset();
        }
        object.role = StateImageRole::ActiveMutable;
    }

    [[nodiscard]] bool drop_device_replica(StateImageHandle handle) noexcept {
        if (!valid(handle)) { return false; }
        Object& object = objects_[handle.index_];
        if (object.role != StateImageRole::CheckpointImmutable || !object.device_slot ||
            !object.host_slot || object.source_pins != 0 || object.destination_pinned ||
            has_pending_replica(object)) {
            return false;
        }
        return_device_slot(*object.device_slot);
        object.device_slot.reset();
        return true;
    }

    [[nodiscard]] bool drop_host_replica(StateImageHandle handle) noexcept {
        if (!valid(handle)) { return false; }
        Object& object = objects_[handle.index_];
        if (host_ == nullptr || object.role != StateImageRole::CheckpointImmutable ||
            !object.device_slot || !object.host_slot || object.source_pins != 0 ||
            object.destination_pinned || has_pending_replica(object) ||
            !host_->release(*object.host_slot)) {
            return false;
        }
        object.host_slot.reset();
        return true;
    }

    // Transfers the Device replica to a new logical identity while the old immutable identity
    // keeps its Host replica. This is the zero-copy preserving branch for a Both source.
    void split_device_replica_identity(StateImageHandle source, StateImageHandle destination) {
        Object& source_object      = require(source);
        Object& destination_object = require(destination);
        if (source == destination || source_object.role != StateImageRole::CheckpointImmutable ||
            !source_object.device_slot || !source_object.host_slot ||
            source_object.source_pins != 0 || source_object.destination_pinned ||
            has_pending_replica(source_object) ||
            destination_object.role != StateImageRole::ReservedDestination ||
            destination_object.device_slot || destination_object.host_slot ||
            has_pending_replica(destination_object) || destination_object.source_pins != 0 ||
            destination_object.destination_pinned) {
            throw std::logic_error("StateImage replica identity split is invalid");
        }
        destination_object.device_slot   = source_object.device_slot;
        destination_object.content_epoch = source_object.content_epoch;
        destination_object.role          = StateImageRole::ActiveMutable;
        source_object.device_slot.reset();
    }

    void freeze(StateImageHandle handle) {
        Object& object = require(handle);
        if (object.role != StateImageRole::ActiveMutable || !object.device_slot ||
            object.source_pins != 0 || object.destination_pinned || has_pending_replica(object)) {
            throw std::logic_error("StateImage active image is not freezable");
        }
        object.content_epoch = next_epoch();
        object.role          = StateImageRole::CheckpointImmutable;
    }

    void thaw(StateImageHandle handle) {
        Object& object = require(handle);
        if (object.role != StateImageRole::CheckpointImmutable || !object.device_slot ||
            object.checkpoint_references != 0 || object.source_pins != 0 ||
            object.destination_pinned || has_pending_replica(object)) {
            throw std::logic_error("StateImage checkpoint is not thawable");
        }
        object.role = StateImageRole::ActiveMutable;
    }

    [[nodiscard]] bool can_recycle_checkpoint_destination(StateImageHandle handle) const noexcept {
        if (!valid(handle)) { return false; }
        const Object& object = objects_[handle.index_];
        return object.role == StateImageRole::CheckpointImmutable &&
               object.device_slot.has_value() && !object.host_slot &&
               object.checkpoint_references == 1 && object.source_pins == 0 &&
               !object.destination_pinned && !has_pending_replica(object);
    }

    [[nodiscard]] std::uint64_t recycle_checkpoint_destination(StateImageHandle handle) {
        if (!can_recycle_checkpoint_destination(handle)) {
            throw std::logic_error("StateImage checkpoint is not recyclable as a destination");
        }
        Object& object               = require(handle);
        const std::uint64_t epoch    = object.content_epoch;
        object.checkpoint_references = 0;
        object.role                  = StateImageRole::ReservedDestination;
        return epoch;
    }

    // Still here, and still tested (`test_context_store.cpp` covers the rotation contract). The abort arm
    // no longer calls it -- it DROPS the recycled checkpoint instead of restoring the epoch, because the
    // branch is unreachable and the epoch restore is the hazard -- but deleting the primitive deleted a
    // tested behaviour and broke that test's build, which is not what a disposal should do. The primitive
    // and the policy that stops using it are separate changes.
    void restore_recycled_checkpoint(StateImageHandle handle, std::uint64_t content_epoch) {
        Object& object = require(handle);
        if (content_epoch == 0 || object.role != StateImageRole::ReservedDestination ||
            !object.device_slot || object.host_slot || object.checkpoint_references != 0 ||
            object.source_pins != 0 || object.destination_pinned || has_pending_replica(object)) {
            throw std::logic_error("StateImage recycled checkpoint is not restorable");
        }
        object.content_epoch         = content_epoch;
        object.checkpoint_references = 1;
        object.role                  = StateImageRole::CheckpointImmutable;
    }

    // #11(b): invariant #6's completeness half. The KV side walks every required page of a checkpoint and
    // throws when one is missing (`context.cpp:609-679`, `checkpoint_recovery.cpp:127`); nothing
    // equivalent existed for the StateImage half -- an image could be valid, referenced and role-correct
    // while having no replica to restore from, and every check would pass until something tried to read
    // it. This expresses it. A replica in flight is NOT incompleteness: what matters is whether any
    // replica is settled, which the two slot tests below decide.
    [[nodiscard]] bool complete(StateImageHandle handle) const noexcept {
        if (!valid(handle)) { return false; }
        const Object& object = objects_[handle.index_];
        // A restorable checkpoint, precisely: immutable (an ActiveMutable image with a slot is not a
        // checkpoint), with a replica that is present and settled.
        if (object.role != StateImageRole::CheckpointImmutable) { return false; }
        if (!object.device_slot && !object.host_slot) { return false; }
        // A replica in flight is NOT incompleteness: what matters is whether any replica is settled, and
        // the two slot tests above decide that. The first version tested `has_pending_replica` instead,
        // which includes an in-flight Device-to-Host demotion (`reserve_device_to_host` sets
        // `pending_host_slot`, `transfer_id` and `destination_pinned`) -- a checkpoint mid-demotion still
        // has a readable device slot, so rejecting it would have failed pricing for the whole owner on
        // healthy traffic.
        return object.content_epoch != 0;
    }

    [[nodiscard]] StateImageSelectors begin_fork(StateImageHandle source,
                                                 StateImageHandle destination) {
        Object& source_object      = require(source);
        Object& destination_object = require(destination);
        if (source == destination || source_object.role != StateImageRole::CheckpointImmutable ||
            !source_object.device_slot ||
            source_object.source_pins == std::numeric_limits<std::uint32_t>::max() ||
            destination_object.role != StateImageRole::ReservedDestination ||
            !destination_object.device_slot || destination_object.source_pins != 0 ||
            destination_object.destination_pinned || has_pending_replica(destination_object)) {
            throw std::logic_error("StateImage fork binding is invalid");
        }
        ++source_object.source_pins;
        destination_object.destination_pinned = true;
        destination_object.role               = StateImageRole::ActiveMutable;
        destination_object.content_epoch      = source_object.content_epoch;
        return {.source      = *source_object.device_slot,
                .destination = *destination_object.device_slot};
    }

    void commit_fork(StateImageHandle source, StateImageHandle destination) {
        Object& source_object      = require(source);
        Object& destination_object = require(destination);
        if (source_object.role != StateImageRole::CheckpointImmutable ||
            source_object.source_pins == 0 ||
            destination_object.role != StateImageRole::ActiveMutable ||
            !destination_object.device_slot || !destination_object.destination_pinned) {
            throw std::logic_error("StateImage fork commit is invalid");
        }
        --source_object.source_pins;
        destination_object.destination_pinned = false;
    }

    void abort_fork(StateImageHandle source, StateImageHandle destination) {
        if (!can_abort_fork(source, destination)) {
            throw std::logic_error("StateImage fork abort is invalid");
        }
        Object& source_object      = require(source);
        Object& destination_object = require(destination);
        --source_object.source_pins;
        destination_object.destination_pinned = false;
        destination_object.role               = StateImageRole::ReservedDestination;
        destination_object.content_epoch      = 0;
    }

    [[nodiscard]] bool can_abort_fork(StateImageHandle source,
                                      StateImageHandle destination) const noexcept {
        if (!valid(source) || !valid(destination)) { return false; }
        const Object& source_object      = objects_[source.index_];
        const Object& destination_object = objects_[destination.index_];
        return source != destination && source_object.role == StateImageRole::CheckpointImmutable &&
               source_object.source_pins != 0 &&
               destination_object.role == StateImageRole::ActiveMutable &&
               destination_object.device_slot.has_value() && destination_object.destination_pinned;
    }

    [[nodiscard]] bool
    can_release_source_after_fork_abort(StateImageHandle source, StateImageHandle destination,
                                        std::uint32_t released_checkpoint_references) const {
        if (!can_abort_fork(source, destination)) { return false; }
        const Object& object = require(source);
        if (released_checkpoint_references > object.checkpoint_references) { return false; }
        if (object.checkpoint_references != released_checkpoint_references) { return true; }
        // abort_fork releases exactly the pin represented by this binding. Any other source pin
        // still protects the object and therefore prevents terminal owner settlement.
        if (object.source_pins != 1 || object.destination_pinned || has_pending_replica(object)) {
            return false;
        }
        if (object.host_slot) {
            if (host_ == nullptr) { return false; }
            (void)host_->view(*object.host_slot);
        }
        return true;
    }

    [[nodiscard]] bool
    can_release_destination_after_fork_abort(StateImageHandle source, StateImageHandle destination,
                                             std::uint32_t released_checkpoint_references) const {
        if (!can_abort_fork(source, destination)) { return false; }
        const Object& object = require(destination);
        if (object.checkpoint_references != released_checkpoint_references ||
            object.source_pins != 0 || has_pending_replica(object)) {
            return false;
        }
        if (object.host_slot) {
            if (host_ == nullptr) { return false; }
            (void)host_->view(*object.host_slot);
        }
        return true;
    }

    [[nodiscard]] StateImageSelectors selectors(StateImageHandle source,
                                                StateImageHandle destination) const {
        const Object& source_object      = require(source);
        const Object& destination_object = require(destination);
        const bool inplace               = source == destination;
        if (!source_object.device_slot || !destination_object.device_slot ||
            (inplace && (source_object.role != StateImageRole::ActiveMutable ||
                         source_object.source_pins != 0 || source_object.destination_pinned)) ||
            (!inplace && (source_object.role != StateImageRole::CheckpointImmutable ||
                          source_object.source_pins == 0 ||
                          destination_object.role != StateImageRole::ActiveMutable ||
                          !destination_object.destination_pinned))) {
            throw std::logic_error("StateImage execution binding is invalid");
        }
        return {.source      = *source_object.device_slot,
                .destination = *destination_object.device_slot};
    }

    [[nodiscard]] std::optional<StateImageTransfer>
    reserve_device_to_host(StateImageHandle source) {
        Object& object = require(source);
        if (host_ == nullptr || object.role != StateImageRole::CheckpointImmutable ||
            !object.device_slot || object.host_slot || has_pending_replica(object) ||
            object.source_pins == std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        // GROWING, and only here: this is the demote path, the one place that should be able to take room
        // the KV arena is not using rather than failing and letting the caller evict restorable state
        // (2026-09-26). Every pure path keeps the pure `allocate()`.
        std::optional<qwen3_5::HostStateSlotHandle> target = host_->allocate_growing();
        if (!target) { return std::nullopt; }
        const std::uint64_t transfer = next_transfer();
        object.pending_host_slot     = *target;
        object.transfer_id           = transfer;
        object.destination_pinned    = true;
        ++object.source_pins;
        return StateImageTransfer(*this, transfer, StateTransferDirection::DeviceToHost, source,
                                  source);
    }

    void enqueue_device_to_host(const StateImageTransfer& transfer, cudaStream_t stream = nullptr) {
        validate_transfer(transfer);
        Object& object = require(transfer.source_);
        if (transfer.direction_ != StateTransferDirection::DeviceToHost || host_ == nullptr ||
            !object.device_slot || !object.pending_host_slot) {
            throw std::logic_error("StateImage Device-to-Host transfer is not enqueueable");
        }
        device_->copy_to_host(*object.device_slot, host_->writable_view(*object.pending_host_slot),
                              stream);
    }

    [[nodiscard]] std::optional<StateImageTransfer>
    begin_device_to_host(StateImageHandle source, cudaStream_t stream = nullptr) {
        std::optional<StateImageTransfer> transfer = reserve_device_to_host(source);
        if (!transfer) { return std::nullopt; }
        try {
            enqueue_device_to_host(*transfer, stream);
        } catch (...) {
            abort_transfer(std::move(*transfer));
            throw;
        }
        return transfer;
    }

    [[nodiscard]] std::optional<StateImageTransfer>
    begin_host_to_device(StateImageHandle source, cudaStream_t stream = nullptr) {
        Object& object = require(source);
        if (host_ == nullptr || object.role != StateImageRole::CheckpointImmutable ||
            !object.host_slot || object.device_slot || has_pending_replica(object) ||
            object.source_pins == std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        const std::optional<std::int32_t> target = take_device_slot();
        if (!target) { return std::nullopt; }
        const std::uint64_t transfer = next_transfer();
        object.pending_device_slot   = *target;
        object.transfer_id           = transfer;
        object.destination_pinned    = true;
        ++object.source_pins;
        try {
            device_->copy_from_host(host_->view(*object.host_slot), *target, stream);
        } catch (...) {
            --object.source_pins;
            object.destination_pinned = false;
            object.transfer_id        = 0;
            object.pending_device_slot.reset();
            return_device_slot(*target);
            throw;
        }
        return StateImageTransfer(*this, transfer, StateTransferDirection::HostToDevice, source,
                                  source);
    }

    [[nodiscard]] std::optional<StateImageTransfer> begin_host_fork(StateImageHandle source,
                                                                    StateImageHandle destination,
                                                                    cudaStream_t stream = nullptr) {
        Object& source_object      = require(source);
        Object& destination_object = require(destination);
        if (host_ == nullptr || source == destination ||
            source_object.role != StateImageRole::CheckpointImmutable || !source_object.host_slot ||
            source_object.source_pins == std::numeric_limits<std::uint32_t>::max() ||
            destination_object.role != StateImageRole::ReservedDestination ||
            destination_object.device_slot || destination_object.host_slot ||
            has_pending_replica(destination_object) || destination_object.source_pins != 0 ||
            destination_object.destination_pinned) {
            return std::nullopt;
        }
        const std::optional<std::int32_t> target = take_device_slot();
        if (!target) { return std::nullopt; }
        const std::uint64_t transfer           = next_transfer();
        destination_object.pending_device_slot = *target;
        destination_object.transfer_id         = transfer;
        destination_object.destination_pinned  = true;
        destination_object.role                = StateImageRole::ActiveMutable;
        destination_object.content_epoch       = source_object.content_epoch;
        ++source_object.source_pins;
        try {
            device_->copy_from_host(host_->view(*source_object.host_slot), *target, stream);
        } catch (...) {
            --source_object.source_pins;
            destination_object.pending_device_slot.reset();
            destination_object.transfer_id        = 0;
            destination_object.destination_pinned = false;
            destination_object.role               = StateImageRole::ReservedDestination;
            destination_object.content_epoch      = 0;
            return_device_slot(*target);
            throw;
        }
        return StateImageTransfer(*this, transfer, StateTransferDirection::HostToFork, source,
                                  destination);
    }

    void publish_transfer(StateImageTransfer&& transfer, bool keep_source_replica) {
        validate_transfer(transfer);
        Object& source      = require(transfer.source_);
        Object& destination = require(transfer.destination_);
        switch (transfer.direction_) {
        case StateTransferDirection::DeviceToHost:
            source.host_slot = source.pending_host_slot;
            source.pending_host_slot.reset();
            if (!keep_source_replica) {
                return_device_slot(*source.device_slot);
                source.device_slot.reset();
            }
            --source.source_pins;
            source.destination_pinned = false;
            source.transfer_id        = 0;
            break;
        case StateTransferDirection::HostToDevice:
            source.device_slot = source.pending_device_slot;
            source.pending_device_slot.reset();
            if (!keep_source_replica) {
                if (host_ == nullptr || !host_->release(*source.host_slot)) {
                    throw std::logic_error("StateImage Host replica release failed at publication");
                }
                source.host_slot.reset();
            }
            --source.source_pins;
            source.destination_pinned = false;
            source.transfer_id        = 0;
            break;
        case StateTransferDirection::HostToFork:
            destination.device_slot = destination.pending_device_slot;
            destination.pending_device_slot.reset();
            --source.source_pins;
            destination.destination_pinned = false;
            destination.transfer_id        = 0;
            break;
        }
        consume(transfer);
    }

    void abort_transfer(StateImageTransfer&& transfer) noexcept {
        if (!valid_transfer(transfer)) {
            consume(transfer);
            return;
        }
        Object& source      = objects_[transfer.source_.index_];
        Object& destination = objects_[transfer.destination_.index_];
        if (source.source_pins != 0) { --source.source_pins; }
        if (transfer.direction_ == StateTransferDirection::DeviceToHost) {
            if (host_ != nullptr && source.pending_host_slot) {
                (void)host_->release(*source.pending_host_slot);
            }
            source.pending_host_slot.reset();
            source.destination_pinned = false;
            source.transfer_id        = 0;
        } else {
            if (destination.pending_device_slot) {
                return_device_slot(*destination.pending_device_slot);
            }
            destination.pending_device_slot.reset();
            destination.destination_pinned = false;
            destination.transfer_id        = 0;
            if (transfer.direction_ == StateTransferDirection::HostToFork) {
                destination.role          = StateImageRole::ReservedDestination;
                destination.content_epoch = 0;
            }
        }
        consume(transfer);
    }

    [[nodiscard]] bool release(StateImageHandle handle) noexcept {
        if (!can_release(handle)) { return false; }
        Object& object = objects_[handle.index_];
        if (object.host_slot) {
            if (host_ == nullptr || !host_->release(*object.host_slot)) { return false; }
            object.host_slot.reset();
        }
        if (object.device_slot) {
            return_device_slot(*object.device_slot);
            object.device_slot.reset();
        }
        object.role          = StateImageRole::Free;
        object.content_epoch = 0;
        if (++object.generation == 0) { ++object.generation; }
        free_objects_.push_back(handle.index_);
        return true;
    }

    // WHY a release would be refused, named, so a caller can say which condition held instead of
    // asserting a leak shape it has not classified. Three of these mean the state image is still OWNED
    // (a checkpoint references it, or a fork pins it): the release was premature, and the slot is not
    // unowned. `PendingReplica` and the post-gate `host-release-failed` case are the shapes that could
    // actually strand a slot.
    enum class ReleaseBlocker : std::uint8_t {
        None,
        InvalidHandle,
        CheckpointReferences,
        SourcePins,
        DestinationPinned,
        PendingReplica,
    };

    [[nodiscard]] ReleaseBlocker release_blocker(StateImageHandle handle) const noexcept {
        if (!valid(handle)) { return ReleaseBlocker::InvalidHandle; }
        const Object& object = objects_[handle.index_];
        if (object.checkpoint_references != 0) { return ReleaseBlocker::CheckpointReferences; }
        if (object.source_pins != 0) { return ReleaseBlocker::SourcePins; }
        if (object.destination_pinned) { return ReleaseBlocker::DestinationPinned; }
        if (has_pending_replica(object)) { return ReleaseBlocker::PendingReplica; }
        return ReleaseBlocker::None;
    }

    [[nodiscard]] const char* release_blocker_name(ReleaseBlocker blocker) const noexcept {
        switch (blocker) {
            case ReleaseBlocker::None:                return "none -- the gate passed, so the HOST release failed and the slot was NOT freed";
            case ReleaseBlocker::InvalidHandle:       return "invalid-handle";
            case ReleaseBlocker::CheckpointReferences: return "checkpoint-references (still owned)";
            case ReleaseBlocker::SourcePins:          return "source-pins (still owned)";
            case ReleaseBlocker::DestinationPinned:   return "destination-pinned (still owned)";
            case ReleaseBlocker::PendingReplica:      return "pending-replica (may be unowned)";
        }
        return "unknown";
    }

    // Implemented IN TERMS OF the blocker so the two cannot drift apart -- a second copy of this
    // predicate is how a classifier ends up disagreeing with the thing it classifies.
    [[nodiscard]] bool can_release(StateImageHandle handle) const noexcept {
        return release_blocker(handle) == ReleaseBlocker::None;
    }

    [[nodiscard]] bool
    can_release_after_checkpoint_references(StateImageHandle handle,
                                            std::uint32_t released_references) const {
        const Object& object = require(handle);
        if (released_references > object.checkpoint_references) { return false; }
        if (object.checkpoint_references != released_references) { return true; }
        if (object.source_pins != 0 || object.destination_pinned || has_pending_replica(object)) {
            return false;
        }
        if (object.host_slot) {
            if (host_ == nullptr) { return false; }
            (void)host_->view(*object.host_slot);
        }
        return true;
    }

private:
    struct Object {
        std::uint32_t generation    = 1;
        std::uint64_t content_epoch = 0;
        std::optional<std::int32_t> device_slot;
        std::optional<qwen3_5::HostStateSlotHandle> host_slot;
        std::optional<std::int32_t> pending_device_slot;
        std::optional<qwen3_5::HostStateSlotHandle> pending_host_slot;
        std::uint64_t transfer_id           = 0;
        std::uint32_t checkpoint_references = 0;
        std::uint32_t source_pins           = 0;
        bool destination_pinned             = false;
        StateImageRole role                 = StateImageRole::Free;
    };

    [[nodiscard]] static bool has_pending_replica(const Object& object) noexcept {
        return object.pending_device_slot.has_value() || object.pending_host_slot.has_value() ||
               object.transfer_id != 0;
    }

    // Grow the handle table so it covers the host pool's CURRENT capacity. Called before an object is handed
    // out, never while a reference to an Object is held (the vector may reallocate).
    void ensure_host_object_capacity() noexcept {
        if (host_ == nullptr) { return; }
        const std::size_t required = static_cast<std::size_t>(device_->slot_count()) + host_->capacity();
        while (objects_.size() < required) {
            objects_.push_back(Object{});
            free_objects_.push_back(static_cast<std::uint32_t>(objects_.size() - 1U));
        }
    }

    [[nodiscard]] std::optional<StateImageHandle> allocate(StateImageRole role,
                                                           bool with_device) noexcept {
        ensure_host_object_capacity();
        if (free_objects_.empty() || role == StateImageRole::Free ||
            (with_device && free_device_count_ == 0)) {
            return std::nullopt;
        }
        const std::uint32_t index = free_objects_.back();
        free_objects_.pop_back();
        Object& object            = objects_[index];
        object                    = Object{.generation = object.generation, .role = role};
        if (with_device) { object.device_slot = free_device_slots_[--free_device_count_]; }
        return StateImageHandle(this, index, object.generation);
    }

    [[nodiscard]] std::optional<std::int32_t> take_device_slot() noexcept {
        if (free_device_count_ == 0) { return std::nullopt; }
        return free_device_slots_[--free_device_count_];
    }

    void return_device_slot(std::int32_t slot) noexcept {
        free_device_slots_[free_device_count_++] = slot;
    }

    [[nodiscard]] std::uint64_t next_epoch() noexcept {
        if (++next_content_epoch_ == 0) { ++next_content_epoch_; }
        return next_content_epoch_;
    }

    [[nodiscard]] std::uint64_t next_transfer() noexcept {
        if (++next_transfer_id_ == 0) { ++next_transfer_id_; }
        return next_transfer_id_;
    }

    [[nodiscard]] Object& require(StateImageHandle handle) {
        if (!valid(handle)) { throw std::invalid_argument("StateImage handle is stale"); }
        return objects_[handle.index_];
    }

    [[nodiscard]] const Object& require(StateImageHandle handle) const {
        if (!valid(handle)) { throw std::invalid_argument("StateImage handle is stale"); }
        return objects_[handle.index_];
    }

    [[nodiscard]] bool valid_transfer(const StateImageTransfer& transfer) const noexcept {
        if (transfer.owner_ != this || transfer.id_ == 0 || !valid(transfer.source_) ||
            !valid(transfer.destination_)) {
            return false;
        }
        const Object& destination = objects_[transfer.destination_.index_];
        return destination.transfer_id == transfer.id_;
    }

    void validate_transfer(const StateImageTransfer& transfer) const {
        if (!valid_transfer(transfer)) {
            throw std::logic_error("StateImage transfer capability is stale");
        }
    }

    static void consume(StateImageTransfer& transfer) noexcept {
        transfer.owner_ = nullptr;
        transfer.id_    = 0;
    }

    qwen3_5::StateImageDevicePool* device_ = nullptr;
    qwen3_5::HostStatePool* host_          = nullptr;
    std::vector<Object> objects_;
    std::vector<std::uint32_t> free_objects_;
    std::vector<std::int32_t> free_device_slots_;
    std::uint32_t free_device_count_  = 0;
    std::uint64_t next_content_epoch_ = 0;
    std::uint64_t next_transfer_id_   = 0;
};

inline StateImageTransfer::~StateImageTransfer() {
    if (owner_ != nullptr) { owner_->abort_transfer(std::move(*this)); }
}

enum class StateReadOwnership : std::uint8_t {
    // The primary binding owns the direct StateImage lifetime.
    Primary,
    // The same active lineage retains the source through an optional checkpoint reference.
    LineageCheckpoint,
    // A retained private or shared owner outside this active lineage retains the source.
    ExternalOwner,
};

struct ActiveStateBinding {
    StateImageHandle read;
    StateImageHandle write;
    bool fork_pending                 = false;
    StateReadOwnership read_ownership = StateReadOwnership::Primary;

    // A non-primary read source is pinned only for the pending Fork. Its allocation belongs to a
    // surviving same-lineage checkpoint or external owner, never to the primary binding.
    [[nodiscard]] bool borrows_read() const noexcept {
        return read_ownership != StateReadOwnership::Primary;
    }

    // An external owner can be a retained private endpoint whose lifetime has no StateImage
    // checkpoint reference, so refcount zero alone does not authorize this sequence to release it.
    [[nodiscard]] bool read_has_external_owner() const noexcept {
        return read_ownership == StateReadOwnership::ExternalOwner;
    }
};

} // namespace ninfer::models::qwen3_5::detail
