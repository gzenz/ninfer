#pragma once

// Host-side memory of which lane the shared KV row scalars currently name (W1-A).
//
// `io.text_kv_table_row` and `io.backend_kv_table_row` are single device i32 scalars. The prefill
// forward pass reads them for the row it writes *and* attends through -- `TextContext` stores the
// lane-specific KV view it is handed and never reads it -- and the only writer in the tree is
// `ProgramImpl::bind_sequence_kv`. Until 2026-09-25 nothing re-bound them during a prefill, so as
// soon as the next lane was admitted while a lane was still prefilling, that lane's remaining chunks
// ran against the newly admitted lane's row: one session's document landed in another session's KV
// pages, and the victim's reply carried the newly admitted session's content (D2). Nothing detected
// it, because every audit checked the KV *bindings* -- page identity, block tables, table contents,
// the decode ingress, the ledger, the state slots -- and never the scalar the kernel actually reads.
//
// This class is the half of that check that costs no device readback: the host remembers what it
// last wrote, so a step can ask whether the row it is about to use is still its own. What the device
// scalar actually holds is a separate question, answered only by the gated readback
// (`NINFER_KV_BINDING_CHECK`). The record alone is a denominator, which is the part that keeps a
// silent instrument from passing for a clean one.
//
// A *foreign* reading is not a defect on its own, and must not be reported as one: under
// concurrency another lane legitimately binds between this lane's steps, which is exactly why the
// fix re-binds at every step rather than once at admission. The defect is running kernels against a
// foreign row, and that is what the readback check is for.

#include <cstdint>

namespace ninfer::models::qwen3_5::detail {

class KvRowBinding {
public:
    static constexpr std::uint32_t kUnboundLane = 0xFFFFFFFFU;

    void bind(std::uint32_t lane, std::int32_t text_row, std::int32_t backend_row) noexcept {
        lane_        = lane;
        text_row_    = text_row;
        backend_row_ = backend_row;
    }

    void clear() noexcept { bind(kUnboundLane, -1, -1); }

    [[nodiscard]] std::uint32_t lane() const noexcept { return lane_; }
    [[nodiscard]] std::int32_t text_row() const noexcept { return text_row_; }
    [[nodiscard]] std::int32_t backend_row() const noexcept { return backend_row_; }

    // True when the shared scalars still name exactly this lane's pair of rows. An unbound record
    // holds nothing: without this guard `held_by(kUnboundLane, -1, -1)` is true on a cleared record,
    // so an observation of a nonsense triple would count as an own reading and the instrument would
    // report "measured clean" for a step that never bound anything.
    [[nodiscard]] bool held_by(std::uint32_t lane, std::int32_t text_row,
                               std::int32_t backend_row) const noexcept {
        if (lane_ == kUnboundLane) { return false; }
        return lane_ == lane && text_row_ == text_row && backend_row_ == backend_row;
    }

    // Record an observation of a lane's own expected pair, counting how often the shared scalars
    // named somebody else at that moment. Returns true when the reading was foreign. Both counters
    // are printed by the caller, so "measured zero" cannot be confused with "measured nothing".
    bool observe(std::uint32_t lane, std::int32_t text_row, std::int32_t backend_row) noexcept {
        ++observed_;
        if (held_by(lane, text_row, backend_row)) { return false; }
        ++foreign_;
        return true;
    }

    [[nodiscard]] std::uint64_t foreign_observations() const noexcept { return foreign_; }
    [[nodiscard]] std::uint64_t total_observations() const noexcept { return observed_; }

    // Compare a *device* reading of the two scalars with what the host recorded writing. This is the
    // only form that can catch a second writer: the record is the host's memory of its own write, so
    // it agrees with itself by construction, and the D2 fix rests on `bind_sequence_kv` being the
    // sole writer of these scalars. Callers read the device back under a gate; both axes are
    // compared, for the same reason the observation compares both.
    bool verify(std::int32_t observed_text, std::int32_t observed_backend) noexcept {
        ++verified_;
        // An unbound record cannot agree with anything: after `clear()` the two -1 defaults would
        // match a device reading of -1/-1 and the check would report agreement for a lane that never
        // bound. Its only caller binds immediately before, so this is a guard against the class being
        // reused somewhere that does not.
        if (lane_ != kUnboundLane && observed_text == text_row_ && observed_backend == backend_row_) {
            return true;
        }
        ++diverged_;
        return false;
    }

    [[nodiscard]] std::uint64_t verified_readings() const noexcept { return verified_; }
    [[nodiscard]] std::uint64_t diverged_readings() const noexcept { return diverged_; }

private:
    std::uint32_t lane_       = kUnboundLane;
    std::int32_t text_row_    = -1;
    std::int32_t backend_row_ = -1;
    std::uint64_t foreign_    = 0;
    std::uint64_t observed_   = 0;
    std::uint64_t verified_   = 0;
    std::uint64_t diverged_   = 0;
};

} // namespace ninfer::models::qwen3_5::detail
