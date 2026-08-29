#include "ninfer/ops/terminal_check.h"
#include "ninfer/types.h"
#include "ops/launcher/terminal_check.h"

#include <stdexcept>
#include <string>

namespace ninfer::ops {
namespace {

void require_contiguous_nonnull(const Tensor& t, const char* op, const char* name) {
    if (!t.is_contiguous()) {
        throw std::invalid_argument(std::string(op) + ": " + name + " must be contiguous");
    }
    if (t.data == nullptr) {
        throw std::invalid_argument(std::string(op) + ": " + name + " data must be non-null");
    }
}

void require_matrix(const Tensor& t, DType dtype, std::int32_t rows, std::int32_t cols,
                    const char* op, const char* name) {
    if (t.dtype != dtype || rows <= 0 || cols <= 0 || t.ne[0] != rows || t.ne[1] != cols ||
        t.ne[2] != 1 || t.ne[3] != 1) {
        throw std::invalid_argument(std::string(op) + ": invalid matrix shape for " + name);
    }
    require_contiguous_nonnull(t, op, name);
}

void require_vector(const Tensor& t, DType dtype, std::int32_t count, const char* op,
                    const char* name) {
    require_matrix(t, dtype, count, 1, op, name);
}

} // namespace

void terminal_check(
    const Tensor& licensed_tokens,
    const Tensor& licensed_counts,
    const Tensor& remaining_budgets,
    const Tensor& stop_token_table,
    const Tensor& stop_token_counts,
    Tensor& committed_counts,
    Tensor& terminal_flags,
    std::int32_t batch,
    std::int32_t width,
    cudaStream_t stream) {
    constexpr const char* op = "terminal_check";
    if (batch < 1) { throw std::invalid_argument("terminal_check: B must be positive"); }
    if (width < 1) { throw std::invalid_argument("terminal_check: width must be positive"); }
    require_matrix(licensed_tokens, DType::I32, width, batch, op, "licensed_tokens");
    require_vector(licensed_counts, DType::I32, batch, op, "licensed_counts");
    require_vector(remaining_budgets, DType::I32, batch, op, "remaining_budgets");
    require_matrix(stop_token_table, DType::I32, static_cast<std::int32_t>(kMaxStopTokens), batch,
                   op, "stop_token_table");
    require_vector(stop_token_counts, DType::I32, batch, op, "stop_token_counts");
    require_vector(committed_counts, DType::I32, batch, op, "committed_counts");
    require_vector(terminal_flags, DType::I32, batch, op, "terminal_flags");
    detail::terminal_check_launch(licensed_tokens, licensed_counts, remaining_budgets,
                                  stop_token_table, stop_token_counts, committed_counts,
                                  terminal_flags, batch, width, stream);
}

} // namespace ninfer::ops
