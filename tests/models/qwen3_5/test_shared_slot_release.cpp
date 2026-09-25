// The disposition of a shared-prefix slot when the transaction that reserved it goes away.
//
// The cases are enumerated rather than sampled, because the defect this replaces was a *gap* in an
// if/else chain: two combinations of (replaces_shared, replacement_removed, role) matched no branch,
// the slot stayed reserved, and the cleanup path -- which releases only `Catalogued` slots -- could not
// reach it. A test that checked the three handled cases would have passed against the broken code, so
// the control here is the enumeration: **no reserved role may ever resolve to `Leave`**, and the two
// formerly-uncovered combinations have their own explicit cases.

#include "models/qwen3_5/program/shared_slot_release.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

using ninfer::models::qwen3_5::detail::resolve_shared_slot_release;
using ninfer::models::qwen3_5::detail::SharedPrefixSlotRole;
using ninfer::models::qwen3_5::detail::SharedSlotReleaseAction;
using Action = SharedSlotReleaseAction;
using Role   = SharedPrefixSlotRole;

constexpr std::array<Role, 4> kRoles{Role::Free, Role::ReservedCapture, Role::ReservedReplacement,
                                     Role::Catalogued};

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void test_no_reserved_role_is_left_stranded() {
    // The defect, stated as the invariant it violated. This is the case the broken chain fails.
    for (const bool replaces : {false, true}) {
        for (const bool removed : {false, true}) {
            for (const Role role : {Role::ReservedCapture, Role::ReservedReplacement}) {
                const Action action = resolve_shared_slot_release(replaces, removed, role);
                expect(action != Action::Leave,
                       "a reserved role resolved to Leave -- its KV and state reference would be "
                       "stranded with no owner");
            }
        }
    }
}

void test_the_previously_handled_combinations_are_unchanged() {
    expect(resolve_shared_slot_release(true, true, Role::ReservedCapture) == Action::Free,
           "a removed replacement's capture reservation no longer frees");
    expect(resolve_shared_slot_release(true, false, Role::ReservedReplacement) == Action::Catalogue,
           "a live replacement no longer becomes catalogued");
    expect(resolve_shared_slot_release(false, true, Role::ReservedCapture) == Action::Free,
           "a plain capture reservation no longer frees");
    expect(resolve_shared_slot_release(false, false, Role::ReservedCapture) == Action::Free,
           "a plain capture reservation no longer frees (both flags false)");
}

void test_the_two_formerly_uncovered_combinations() {
    // These matched no branch before, so the slot stayed reserved for ever.
    expect(resolve_shared_slot_release(true, true, Role::ReservedReplacement) == Action::Free,
           "a reserved replacement whose replacement was removed stayed reserved");
    expect(resolve_shared_slot_release(false, true, Role::ReservedReplacement) == Action::Free,
           "a reserved replacement outside a replacement flow stayed reserved");
    expect(resolve_shared_slot_release(false, false, Role::ReservedReplacement) == Action::Catalogue,
           "a live reserved replacement outside a replacement flow was not catalogued");
}

void test_unreserved_roles_are_left_alone() {
    for (const bool replaces : {false, true}) {
        for (const bool removed : {false, true}) {
            expect(resolve_shared_slot_release(replaces, removed, Role::Free) == Action::Leave,
                   "a free slot was touched");
            expect(resolve_shared_slot_release(replaces, removed, Role::Catalogued) == Action::Leave,
                   "a catalogued slot was touched by the release decision");
        }
    }
}

} // namespace

int main() {
    test_no_reserved_role_is_left_stranded();
    test_the_previously_handled_combinations_are_unchanged();
    test_the_two_formerly_uncovered_combinations();
    test_unreserved_roles_are_left_alone();
    if (failures != 0) {
        std::cerr << failures << " shared-slot release checks failed\n";
        return 1;
    }
    std::cout << "shared-slot release checks passed\n";
    return 0;
}
