// The idle-block grace window: the predicate that decides when a request blocked with an empty active
// set is refused instead of waiting out its deadline.
//
// The first version of this logic lived inline in `engine_core.h` and could not fire, so these cases
// are written to fail against it. The one that matters is `sustained_block_fires_under_jitter`: the
// inline version reset its window whenever the elapsed time exceeded the period, which is also the
// condition it tested, so it re-armed forever and only a poll landing exactly on the boundary could
// have rejected. Sub-millisecond jitter is the control that distinguishes the two.

#include "runtime/engine/idle_block_grace.h"

#include <chrono>
#include <iostream>
#include <string_view>

namespace {

using Grace = ninfer::runtime::IdleBlockGrace;
using Clock = Grace::Clock;
using Period = Grace::Duration;

constexpr Period kPeriod = std::chrono::seconds(5);

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void test_a_single_observation_never_fires() {
    Grace grace;
    expect(!grace.observe(7, Clock::time_point{}, kPeriod),
           "the first observation of a head fired immediately");
}

void test_transient_block_does_not_fire() {
    Grace grace;
    const Clock::time_point t0{};
    expect(!grace.observe(7, t0, kPeriod), "first observation fired");
    expect(!grace.observe(7, t0 + std::chrono::milliseconds(4999), kPeriod),
           "a block shorter than the period fired");
    expect(!grace.observe(7, t0 + std::chrono::microseconds(4999999), kPeriod),
           "a block just under the period fired (4.999999 s against a 5 s period)");
}

void test_sustained_block_fires_under_jitter() {
    Grace grace;
    const Clock::time_point t0{};
    expect(!grace.observe(7, t0, kPeriod), "first observation fired");
    bool fired = false;
    Clock::time_point now = t0;
    // ~1 ms polls with sub-millisecond jitter, for far longer than the period. The inline version
    // re-armed on every poll once the elapsed time exceeded the period and so never reached its own
    // test; this must fire, and it must fire once the period is behind it.
    for (int i = 0; i < 20000; ++i) {
        now += std::chrono::microseconds(1000 + (i % 7));
        if (grace.observe(7, now, kPeriod)) {
            fired = true;
            expect(now - t0 >= kPeriod, "fired before the period had elapsed");
            break;
        }
    }
    expect(fired, "a block sustained well past the period never fired under jittered polls");
}

void test_a_new_head_starts_its_own_window() {
    Grace grace;
    const Clock::time_point t0{};
    expect(!grace.observe(1, t0, kPeriod), "first observation fired");
    // A different request becomes the head long after the first head's window: it must not inherit it.
    expect(!grace.observe(2, t0 + std::chrono::seconds(30), kPeriod),
           "a new head inherited the previous head's window");
    expect(grace.observe(2, t0 + std::chrono::seconds(35), kPeriod),
           "the new head's own window did not fire after its period");
}

void test_clear_resets_the_window() {
    Grace grace;
    const Clock::time_point t0{};
    expect(!grace.observe(7, t0, kPeriod), "first observation fired");
    grace.clear();
    expect(!grace.observe(7, t0 + std::chrono::seconds(30), kPeriod),
           "a cleared window still fired for the same head");
}

} // namespace

int main() {
    test_a_single_observation_never_fires();
    test_transient_block_does_not_fire();
    test_sustained_block_fires_under_jitter();
    test_a_new_head_starts_its_own_window();
    test_clear_resets_the_window();
    if (failures != 0) {
        std::cerr << failures << " idle-block grace checks failed\n";
        return 1;
    }
    std::cout << "idle-block grace checks passed\n";
    return 0;
}
