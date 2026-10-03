#pragma once

// How long a request may stay blocked with an empty active set before it is refused as unsatisfiable.
//
// The wedge (#14/#9, 2026-09-25) is the reason this exists: with occupancy owned by nothing, a request
// above what remains free planned to nothing and waited out its 900 s deadline, blocking every request
// queued behind it, until a restart. Nothing can free capacity while no lane is active, so the wait
// cannot end well -- but a *transient* idle moment must not fail a request the next boundary would
// admit, hence a persistence window rather than an immediate refusal.
//
// It is a separate type because the first version of this logic was written inline inside
// `engine_core.h` and could not fire: the window was reset by the same condition that tested it
// (`elapsed > period` reset the start before `elapsed >= period` was checked), so rejection happened
// only if a poll landed exactly on the period boundary. Its only evidence was "0 rejections on healthy
// traffic" -- a negative that could not have failed. Here the rule is one function with a unit test,
// and the test is checked against mutants of it.

#include <chrono>
#include <cstdint>
#include <optional>

namespace ninfer::runtime {

class IdleBlockGrace {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    // Records one observation of `request_id` being blocked and returns true when it has now been
    // blocked continuously for at least `period`. The window is reset only when a *different* request
    // becomes the head: a monotone clock per head is what makes the predicate able to fire, and
    // re-arming on every observation is what made the first version inert.
    [[nodiscard]] bool observe(std::uint64_t request_id, Clock::time_point now, Duration period) noexcept {
        if (!current_.has_value() || *current_ != request_id) {
            current_ = request_id;
            since_   = now;
            return false;
        }
        return now - since_ >= period;
    }

    void clear() noexcept {
        current_.reset();
        since_ = Clock::time_point{};
    }

    [[nodiscard]] std::optional<std::uint64_t> request() const noexcept { return current_; }

private:
    std::optional<std::uint64_t> current_;
    Clock::time_point since_{};
};

} // namespace ninfer::runtime
