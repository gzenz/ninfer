// The #13 underflow: the predicate a diagnostic now asks BEFORE the subtraction, and the throw it must agree
// with.
//
// WHAT THIS PINS, and why it is per-axis. `resource_subtraction_underflows` was exported so a diagnostic can
// ask "would this subtraction throw?" ahead of the throw -- an occurrence AT the subtraction site previously
// produced no output at all, which is why the 2026-10-02 #13 occurrence is still unattributed and could only
// be read as evidence by arguing. The predicate and `checked_resource_difference` share one body, so they
// cannot disagree about WHICH axes are checked or in what order; these cases exist so a second copy of the six
// comparisons cannot creep in beside it. Each axis is varied ON ITS OWN, so a predicate that checked only
// `main_kv_pages` -- the axis the incident named -- fails here rather than passing on it.

#include "models/qwen3_5/program/context_work.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace q36   = ninfer::models::qwen3_5;
using Resources = q36::detail::PhysicalResources;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

Resources make(std::uint32_t lanes, std::uint32_t main_kv, std::uint32_t backend_kv,
               std::uint32_t dev_slots, std::uint32_t host_slots, std::uint64_t host_kv) {
    return Resources{
        .device =
            {
                .active_lanes     = lanes,
                .state_slots      = dev_slots,
                .main_kv_pages    = main_kv,
                .backend_kv_pages = backend_kv,
            },
        .host = {.state_slots = host_slots, .kv_bytes = host_kv},
    };
}

bool throws_on(const Resources& value, const Resources& removed) {
    try {
        (void)q36::detail::checked_resource_difference(value, removed);
        return false;
    } catch (const std::logic_error&) {
        return true;
    }
}

// The predicate and the throw are two entry points onto one rule. If they ever answer differently, a
// diagnostic would print for a subtraction that succeeds (or stay silent for one that throws) -- which is the
// defect this whole change is about, one level up.
void expect_agreement(const Resources& value, const Resources& removed, std::string_view what) {
    const bool predicted = q36::detail::resource_subtraction_underflows(value, removed);
    const bool threw     = throws_on(value, removed);
    expect(predicted == threw, what);
}

void test_a_subtraction_that_fits_is_allowed() {
    const Resources value   = make(0, 1057, 4, 2, 3, 1000);
    const Resources removed = make(0, 59, 1, 1, 1, 100);
    expect_agreement(value, removed, "a subtraction that fits is predicted and behaves differently");
    const Resources delta = q36::detail::checked_resource_difference(value, removed);
    expect(delta.device.main_kv_pages == 998, "the clean difference is wrong on main_kv_pages");
    expect(delta.device.backend_kv_pages == 3, "the clean difference is wrong on backend_kv_pages");
    expect(delta.host.kv_bytes == 900, "the clean difference is wrong on host.kv_bytes");
}

// One axis over by exactly one, every other axis removed by nothing -- so only that axis can be the reason.
void test_each_axis_discriminates() {
    const Resources value = make(2, 100, 10, 5, 4, 1000);

    const struct {
        const char* name;
        Resources   removed;
    } cases[] = {
        {"device.active_lanes", make(3, 0, 0, 0, 0, 0)},
        {"device.state_slots", make(0, 0, 0, 6, 0, 0)},
        {"device.main_kv_pages", make(0, 101, 0, 0, 0, 0)},
        {"device.backend_kv_pages", make(0, 0, 11, 0, 0, 0)},
        {"host.state_slots", make(0, 0, 0, 0, 5, 0)},
        {"host.kv_bytes", make(0, 0, 0, 0, 0, 1001)},
    };
    for (const auto& c : cases) {
        expect(q36::detail::resource_subtraction_underflows(value, c.removed),
               std::string("the predicate misses an underflow on ") + c.name);
        expect_agreement(value, c.removed,
                         std::string("the predicate and the throw disagree on ") + c.name);
    }
}

// The boundary: equal is allowed. A predicate written with `>=` (or a throw written with `>=`) would refuse
// every exact-fit release, which is the common case rather than a rare one.
void test_exact_fit_is_not_an_underflow() {
    const Resources value   = make(1, 100, 10, 5, 4, 1000);
    const Resources removed = value;
    expect(!q36::detail::resource_subtraction_underflows(value, removed),
           "an exact-fit subtraction is refused");
    expect_agreement(value, removed, "an exact-fit subtraction throws");
    const Resources delta = q36::detail::checked_resource_difference(value, removed);
    expect(delta.device.main_kv_pages == 0 && delta.host.kv_bytes == 0,
           "an exact-fit subtraction does not yield zero");
}

// The saturating form is what the diagnostic prints the delta with, so that it can report a SKEWED subtraction
// instead of throwing on the way to reporting it. It must not wrap: a wrapping subtraction would print a
// huge positive delta and read as over-removal by billions.
void test_saturating_difference_does_not_wrap() {
    const Resources value   = make(0, 1057, 0, 0, 0, 0);
    const Resources removed = make(0, 1059, 0, 0, 0, 0);
    const Resources delta   = q36::detail::positive_resource_difference(value, removed);
    expect(delta.device.main_kv_pages == 0,
           "the saturating difference wrapped on the axis that underflowed");
    expect(q36::detail::resource_subtraction_underflows(value, removed),
           "the saturating case is not an underflow, so this control measures nothing");
}

std::string message_on(const Resources& value, const Resources& removed) {
    try {
        (void)q36::detail::checked_resource_difference(value, removed);
    } catch (const std::logic_error& e) {
        return e.what();
    }
    return {};
}

// `resource_underflow_detail` is a SECOND copy of the six comparisons -- it must be, since its job is to say
// WHICH axis fired rather than merely that one did. So it can drift from the predicate, and its symptom is a
// message that names the wrong axis (or, worse, reads "none" while an underflow is real). Each axis is
// therefore asserted BY NAME, which is the only way that copy is covered at all.
void test_the_message_names_the_axis_that_fired() {
    const Resources value = make(2, 100, 10, 5, 4, 1000);

    const struct {
        const char* name;
        Resources   removed;
    } cases[] = {
        {"device.active_lanes", make(3, 0, 0, 0, 0, 0)},
        {"device.state_slots", make(0, 0, 0, 6, 0, 0)},
        {"device.main_kv_pages", make(0, 101, 0, 0, 0, 0)},
        {"device.backend_kv_pages", make(0, 0, 11, 0, 0, 0)},
        {"host.state_slots", make(0, 0, 0, 0, 5, 0)},
        {"host.kv_bytes", make(0, 0, 0, 0, 0, 1001)},
    };
    for (const auto& c : cases) {
        const std::string message = message_on(value, c.removed);
        expect(!message.empty(), std::string("no underflow was raised for ") + c.name);
        expect(message.find(c.name) != std::string::npos,
               std::string("the underflow message does not name ") + c.name);
        expect(message.find("UNNAMED") == std::string::npos,
               std::string("the predicate and the detail function have drifted on ") + c.name);
    }
}

} // namespace

int main() {
    test_a_subtraction_that_fits_is_allowed();
    test_each_axis_discriminates();
    test_exact_fit_is_not_an_underflow();
    test_saturating_difference_does_not_wrap();
    test_the_message_names_the_axis_that_fired();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
