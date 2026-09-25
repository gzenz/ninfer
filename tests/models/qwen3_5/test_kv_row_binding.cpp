// W1-A: the host-side record of which lane the shared KV row scalars name.
//
// The point of these cases is discrimination, not agreement with the code: each row axis is varied
// one at a time, so a check that compared only the lane (or only the text row) would fail here. The
// last case states that control explicitly.

#include "models/qwen3_5/program/kv_row_binding.h"

#include <iostream>
#include <string_view>

namespace {

namespace q36   = ninfer::models::qwen3_5;
using Binding    = q36::detail::KvRowBinding;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void test_unbound_is_not_held() {
    Binding binding;
    expect(binding.lane() == Binding::kUnboundLane, "a fresh binding claims a lane");
    expect(!binding.held_by(0, 0, 0), "an unbound binding is held by lane 0");
    expect(!binding.held_by(Binding::kUnboundLane, -1, -1),
           "an unbound binding compares equal to itself, so a step would pass on it");
}

void test_each_axis_discriminates() {
    Binding binding;
    binding.bind(2, 2, 0);
    expect(binding.held_by(2, 2, 0), "a binding does not hold its own triple");
    expect(!binding.held_by(3, 2, 0), "a foreign lane is held (D2: the admitted lane's row)");
    expect(!binding.held_by(2, 3, 0), "a foreign text row is held");
    expect(!binding.held_by(2, 2, 7), "a foreign backend row is held");
    // The gate is the full triple, not the lane: with rows 1:1 by lane a lane-only predicate looks
    // equivalent, and this is the case where it is not.
    const bool lane_only_would_accept = (binding.lane() == 2);
    expect(lane_only_would_accept && !binding.held_by(2, 3, 0),
           "the row axes do not add discrimination over the lane (control is vacuous)");
}

void test_observations_count_both_denominators() {
    Binding binding;
    binding.bind(1, 1, 0);
    expect(!binding.observe(1, 1, 0), "own binding observed as foreign");
    expect(binding.total_observations() == 1 && binding.foreign_observations() == 0,
           "an own observation moved the foreign counter");

    // Lane 3 is admitted mid-prefill and binds: the pre-fix condition, observed once.
    binding.bind(3, 3, 0);
    expect(binding.observe(1, 1, 0), "the foreign condition was not detected");
    expect(binding.total_observations() == 2 && binding.foreign_observations() == 1,
           "the observation counters disagree with the two observations made");

    // The fix re-binds at the next step, and the foreign counter stops growing while the total does.
    binding.bind(1, 1, 0);
    expect(!binding.observe(1, 1, 0), "the re-bound lane is still foreign");
    expect(binding.total_observations() == 3 && binding.foreign_observations() == 1,
           "re-binding did not stop the foreign count");
}

void test_clear_reports_foreign() {
    Binding binding;
    binding.bind(0, 0, 0);
    binding.clear();
    expect(binding.lane() == Binding::kUnboundLane, "clear did not unbind");
    expect(binding.observe(0, 0, 0), "an observation against a cleared binding was not foreign");
}

void test_verify_compares_the_device_reading() {
    using Verdict = Binding::Verdict;
    Binding binding;
    binding.bind(0, 0, 0);
    expect(binding.verify(0, 0) == Verdict::Agrees, "a device reading equal to the written pair diverged");
    expect(binding.verified_readings() == 1 && binding.diverged_readings() == 0,
           "an agreeing reading moved the divergence counter");
    // A second writer of the scalar, which is what this check exists to catch: the host record is
    // untouched, the device says something else, and only one axis differs.
    expect(binding.verify(0, 5) == Verdict::Diverges, "a divergent backend row was accepted");
    expect(binding.verify(9, 0) == Verdict::Diverges, "a divergent text row was accepted");
    expect(binding.verified_readings() == 3 && binding.diverged_readings() == 2,
           "the verification counters disagree with the three readings made");
}

void test_releasing_ownership_does_not_fabricate_divergence() {
    using Verdict = Binding::Verdict;
    // The defect a 2026-09-25 review caught, kept as a case: `unbind_sequence_kv` releases ownership
    // while the device still holds what it last received. Comparing a reading against *ownership*
    // made every readback after any other lane finished report a divergence that was not there.
    Binding binding;
    binding.bind(1, 1, 1);
    binding.clear();
    expect(binding.lane() == Binding::kUnboundLane, "clear did not release ownership");
    expect(binding.has_written_values(), "clear discarded what the device last received");
    expect(binding.verify(1, 1) == Verdict::Agrees,
           "a released record reported divergence although the device holds what was written");
    expect(binding.verify(1, 2) == Verdict::Diverges, "a real divergence after clear was accepted");
}

void test_unwritten_record_is_unverifiable_not_agreeing() {
    using Verdict = Binding::Verdict;
    Binding binding;
    expect(binding.verify(-1, -1) == Verdict::Unverifiable,
           "a record that was never written claimed agreement with its own defaults");
    expect(binding.unverifiable_readings() == 1, "the unverifiable counter did not move");
    expect(binding.verified_readings() == 0 && binding.diverged_readings() == 0,
           "an unverifiable reading was counted as verified or as divergent");
}

} // namespace

int main() {
    test_unbound_is_not_held();
    test_each_axis_discriminates();
    test_observations_count_both_denominators();
    test_clear_reports_foreign();
    test_verify_compares_the_device_reading();
    test_releasing_ownership_does_not_fabricate_divergence();
    test_unwritten_record_is_unverifiable_not_agreeing();
    if (failures != 0) {
        std::cerr << failures << " KV row binding checks failed\n";
        return 1;
    }
    std::cout << "KV row binding checks passed\n";
    return 0;
}
