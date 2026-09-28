#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ninfer {

// Diagnostic controls split in two:
//
//  * a control that only OBSERVES -- reads device memory, prints, D2H-syncs for a digest -- is a
//    plain `std::getenv(...)` check.
//  * a control that MUTATES device memory, a scheduler's membership, or a bound that changes which
//    work is done goes through this function. It returns false unless the build enables it
//    (`-DNINFER_HARMFUL_CONTROLS=ON`, which defines NINFER_ENABLE_HARMFUL_CONTROLS), so a stray
//    variable exported from an old shell cannot make a shipped binary behave like an instrumented
//    one. Rebuilding with the option is a deliberate act, which is the point.
//
// This is a *documented* split, not an enforced one, and two exceptions are deliberate:
//
//  * `NINFER_INGRESS_PROBE` writes device memory (a D2D copy into the ingress shadow) but is a
//    read-only diagnostic in intention: it copies a buffer the step already wrote, into a buffer
//    nothing else reads, and the copy is allocated before graph capture precisely so it does not
//    change what the graph contains. Gating it would make the one instrument that cleared the
//    device-ingress question unreproducible from the tree.
//  * `NINFER_SEARCH_MS` is an operator-facing override of the materialization search's allowance --
//    the supported way to A/B that bound, clamped, and documented as such next to the constant it
//    overrides. It is not a diagnostic, and it is deliberately not behind this guard: the operator
//    needs it in a normal build. (A negative value clamps rather than erroring; see the planner.)
//
// Every other `getenv("NINFER_...")` in the tree prints or reads only. Re-derive that list with
// `grep -rn 'getenv("NINFER_' src/` and classify each hit against the two bullets above.
[[nodiscard]] inline bool diagnostic_control_enabled(const char* name) noexcept {
#ifdef NINFER_ENABLE_HARMFUL_CONTROLS
    return std::getenv(name) != nullptr;
#else
    // Say so -- once PER NAME, not once per process. A fixed table of the five known controls was
    // the first attempt; a sixth name (or any name outside it) shared one fallback bit and was then
    // swallowed in silence, which is the same failure one level down. This claims a slot per distinct
    // name, comparing pointer identity first (callers pass string literals, so identity is enough) and
    // falling back to strcmp. Bounded and allocation-free: the per-step path is a few strcmp against a
    // 16-slot array, and the print happens only on the once-per-name hit.
    // 16 slots, and the tree has 20 distinct `NINFER_*` names -- but only the five state-mutating
    // controls reach this function today (the other fifteen are plain read-only `getenv` checks), so
    // the bound is not reachable; a name arriving after the table is full would go unwarned, which is
    // why the bound is stated here rather than assumed away.
    constexpr unsigned kSlots = 16;
    static const char* claimed[kSlots] = {};
    static std::atomic<unsigned> claimed_count{0};
    if (std::getenv(name) != nullptr) {
        const unsigned count = claimed_count.load(std::memory_order_acquire);
        bool known = false;
        for (unsigned i = 0; i < count && i < kSlots; ++i) {
            const char* slot = claimed[i];
            if (slot == name || (slot != nullptr && std::strcmp(slot, name) == 0)) { known = true; break; }
        }
        if (!known && count < kSlots &&
            claimed_count.compare_exchange_strong(const_cast<unsigned&>(count), count + 1)) {
            claimed[count] = name;
            std::fprintf(stderr,
                         "[diagnostics] %s is set but this build does not enable state-mutating "
                         "controls (configure with -DNINFER_HARMFUL_CONTROLS=ON); ignoring it\n",
                         name);
        }
    }
    return false;
#endif
}

// Inject a throw at a named site, to provoke the failure paths that traffic has not reached. Three load
// attempts and five constructed scenarios all came back negative, so "create the conditions and watch"
// is exhausted for these defects; what is left is creating the *failure*.
//
//   NINFER_INJECT_THROW=mat-consume        -- throw the first time that site is reached
//   NINFER_INJECT_THROW=mat-consume:3      -- ... the third time (for a site reached repeatedly)
//
// Gated like the other state-mutating controls: it changes control flow, so it must not be reachable in
// a shipped build even if the variable is exported from an old shell. The caller throws what it likes;
// this only decides *whether*. The per-site counter is keyed by pointer identity (callers pass literals)
// and bounded, like the warning table above.
[[nodiscard]] inline bool harmful_inject_throw(const char* site) noexcept {
    if (!diagnostic_control_enabled("NINFER_INJECT_THROW")) { return false; }
    const char* spec = std::getenv("NINFER_INJECT_THROW");
    if (spec == nullptr || site == nullptr) { return false; }
    const char* colon          = std::strchr(spec, ':');
    const std::size_t name_len = colon != nullptr ? static_cast<std::size_t>(colon - spec)
                                                 : std::strlen(spec);
    if (std::strlen(site) != name_len || std::strncmp(spec, site, name_len) != 0) { return false; }
    const unsigned nth = colon != nullptr ? static_cast<unsigned>(std::atoi(colon + 1)) : 1U;
    if (nth == 0U) { return false; }

    constexpr unsigned kSlots = 8;
    static const char* names[kSlots]  = {};
    static unsigned counts[kSlots]    = {};
    static unsigned used              = 0;
    for (unsigned i = 0; i < used; ++i) {
        if (names[i] == site || std::strcmp(names[i], site) == 0) {
            return ++counts[i] == nth;
        }
    }
    if (used == kSlots) { return false; }
    names[used]  = site;
    counts[used] = 1;
    ++used;
    return counts[used - 1] == nth;
}

} // namespace ninfer
