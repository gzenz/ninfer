#pragma once

// What to do with a shared-prefix slot when the transaction that reserved it goes away.
//
// This is the decision that created the 2026-09-25 leak candidate (#9). `abort_active_capture` handled
// three combinations of (replaces_shared, replacement_removed, role) and left the rest untouched, while
// `fail_all_cleanup` releases only slots whose role is `Catalogued` -- so a slot left in *any* other
// role keeps its KV addresses and its state checkpoint reference with no owner, and no path can free
// it. That is the leak's signature: device pages plus a host state slot, unfreeable.
//
// The rule is therefore: a reserved role belonging to a transaction that is going away must never be
// left reserved. It either goes back to the pool (`Free`) or, where the reservation was a replacement
// that had already been installed, becomes visible to the catalog (`Catalogued`).
//
// It is a pure function in a leaf header so the combinations can be enumerated in a unit test -- the
// engine path itself cannot be driven to those intermediate states without a reproduction, and an
// unexercised mutation of this bookkeeping is what this workstream keeps paying for.

#include <cstdint>

namespace ninfer::models::qwen3_5::detail {

enum class SharedPrefixSlotRole : std::uint8_t {
    Free,
    ReservedCapture,
    ReservedReplacement,
    Catalogued,
};

enum class SharedSlotReleaseAction : std::uint8_t {
    // The slot is not reserved (or not ours): leave it exactly as it is.
    Leave,
    // Return it to the pool.
    Free,
    // Its replacement was installed, so it becomes catalogued rather than discarded.
    Catalogue,
};

// `replaces_shared` / `replacement_removed` describe the transaction that held the reservation;
// `role` is the slot's role as observed. Any reserved role must resolve to something other than
// `Leave`, or the occupancy it carries is stranded.
[[nodiscard]] constexpr SharedSlotReleaseAction resolve_shared_slot_release(
    bool replaces_shared, bool replacement_removed, SharedPrefixSlotRole role) noexcept {
    if (role != SharedPrefixSlotRole::ReservedCapture &&
        role != SharedPrefixSlotRole::ReservedReplacement) {
        return SharedSlotReleaseAction::Leave;
    }
    if (role == SharedPrefixSlotRole::ReservedReplacement) {
        // The reserved replacement's own fate: if the replacement it stood for was removed, the slot
        // goes back to the pool; otherwise the replacement is live and the slot belongs in the catalog.
        return replacement_removed ? SharedSlotReleaseAction::Free
                                   : SharedSlotReleaseAction::Catalogue;
    }
    // ReservedCapture: this was a capture reservation that never published.
    return SharedSlotReleaseAction::Free;
}

} // namespace ninfer::models::qwen3_5::detail
