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
        written_text_    = text_row;
        written_backend_ = backend_row;
        ever_written_    = true;
    }

    // Releases *ownership*, and only if `lane` is the owner. The device scalars keep the values they
    // last received -- nothing writes them on unbind -- so the written pair survives it.
    //
    // Two defects came from getting this wrong, both caught by review on 2026-09-25. First the
    // written pair was cleared too, so once `verify()` moved before the re-bind a lane's readback
    // reported DEVICE-DIVERGED every time any other lane finished. Then the remaining unconditional
    // clear still made `observe()` count a *foreign rebind* whenever a lane finished while another
    // lane owned the scalars -- the label says "the scalars named somebody else", and they had not.
    // Ownership and "what the device last got" are different facts; this class keeps them apart, and
    // the release is attributed to the lane it belongs to.
    void clear(std::uint32_t lane) noexcept {
        if (lane_ == lane) { lane_ = kUnboundLane; }
    }

    [[nodiscard]] std::uint32_t lane() const noexcept { return lane_; }
    [[nodiscard]] std::int32_t text_row() const noexcept { return written_text_; }
    [[nodiscard]] std::int32_t backend_row() const noexcept { return written_backend_; }
    [[nodiscard]] bool has_written_values() const noexcept { return ever_written_; }

    // True when the shared scalars still name exactly this lane's pair of rows. An unbound record
    // holds nothing: without this guard `held_by(kUnboundLane, -1, -1)` is true on a cleared record,
    // so an observation of a nonsense triple would count as an own reading and the instrument would
    // report "measured clean" for a step that never bound anything.
    [[nodiscard]] bool held_by(std::uint32_t lane, std::int32_t text_row,
                               std::int32_t backend_row) const noexcept {
        if (lane_ == kUnboundLane) { return false; }
        return lane_ == lane && written_text_ == text_row && written_backend_ == backend_row;
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
    enum class Verdict { Agrees, Diverges, Unverifiable };

    // Compares a device reading against the values the host last WROTE -- not against current
    // ownership, which `clear()` may have released while the device still holds them. Divergence can
    // therefore only mean a second writer, which is the one thing this check exists to catch.
    // A record that has never been written yields Unverifiable rather than either answer: with no
    // value to compare against, "agree" and "disagree" would both be fabrications.
    Verdict verify(std::int32_t observed_text, std::int32_t observed_backend) noexcept {
        if (!ever_written_) {
            ++unverifiable_;
            return Verdict::Unverifiable;
        }
        ++verified_;
        if (observed_text == written_text_ && observed_backend == written_backend_) {
            return Verdict::Agrees;
        }
        ++diverged_;
        return Verdict::Diverges;
    }

    [[nodiscard]] std::uint64_t verified_readings() const noexcept { return verified_; }
    [[nodiscard]] std::uint64_t diverged_readings() const noexcept { return diverged_; }
    [[nodiscard]] std::uint64_t unverifiable_readings() const noexcept { return unverifiable_; }

private:
    std::uint32_t lane_          = kUnboundLane;
    std::int32_t written_text_    = -1;
    std::int32_t written_backend_ = -1;
    bool ever_written_           = false;
    std::uint64_t foreign_       = 0;
    std::uint64_t observed_      = 0;
    std::uint64_t verified_      = 0;
    std::uint64_t diverged_      = 0;
    std::uint64_t unverifiable_  = 0;
};

} // namespace ninfer::models::qwen3_5::detail
