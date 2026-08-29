#include "ninfer/ops/terminal_check.h"
#include "ninfer/types.h"
#include "ops/op_tester.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace ninfer;
using namespace ninfer::test;

namespace {

struct RowCase {
    std::vector<std::int32_t> tokens;    // width tokens for this row
    std::int32_t count;                   // licensed count
    std::int32_t budget;                  // remaining budget
    std::vector<std::int32_t> stops;      // stop token ids (padded to kMaxStopTokens)
    std::int32_t expected_committed;
    std::int32_t expected_terminal;
};

int run_case(const char* label, std::int32_t width, const std::vector<RowCase>& rows) {
    const std::int32_t batch = static_cast<std::int32_t>(rows.size());
    const std::int32_t max_stops = static_cast<std::int32_t>(kMaxStopTokens);

    std::vector<std::int32_t> h_licensed_tokens(static_cast<std::size_t>(width * batch));
    std::vector<std::int32_t> h_licensed_counts(static_cast<std::size_t>(batch));
    std::vector<std::int32_t> h_budgets(static_cast<std::size_t>(batch));
    std::vector<std::int32_t> h_stop_table(static_cast<std::size_t>(max_stops * batch));
    std::vector<std::int32_t> h_stop_counts(static_cast<std::size_t>(batch));
    std::vector<std::int32_t> expected_committed(static_cast<std::size_t>(batch));
    std::vector<std::int32_t> expected_terminal(static_cast<std::size_t>(batch));

    for (std::int32_t b = 0; b < batch; ++b) {
        const auto& rc = rows[static_cast<std::size_t>(b)];
        for (std::int32_t j = 0; j < width; ++j) {
            h_licensed_tokens[static_cast<std::size_t>(b * width + j)] =
                j < static_cast<std::int32_t>(rc.tokens.size())
                    ? rc.tokens[static_cast<std::size_t>(j)]
                    : -1;
        }
        h_licensed_counts[static_cast<std::size_t>(b)] = rc.count;
        h_budgets[static_cast<std::size_t>(b)]         = rc.budget;
        for (std::int32_t s = 0; s < max_stops; ++s) {
            h_stop_table[static_cast<std::size_t>(b * max_stops + s)] =
                s < static_cast<std::int32_t>(rc.stops.size())
                    ? rc.stops[static_cast<std::size_t>(s)]
                    : -1;
        }
        h_stop_counts[static_cast<std::size_t>(b)]    = static_cast<std::int32_t>(rc.stops.size());
        expected_committed[static_cast<std::size_t>(b)] = rc.expected_committed;
        expected_terminal[static_cast<std::size_t>(b)]  = rc.expected_terminal;
    }

    DeviceBuffer d_tokens  = to_device(h_licensed_tokens);
    DeviceBuffer d_counts  = to_device(h_licensed_counts);
    DeviceBuffer d_budgets = to_device(h_budgets);
    DeviceBuffer d_stops   = to_device(h_stop_table);
    DeviceBuffer d_scounts = to_device(h_stop_counts);
    GuardedDeviceBuffer d_committed(static_cast<std::size_t>(batch) * sizeof(std::int32_t));
    GuardedDeviceBuffer d_terminal(static_cast<std::size_t>(batch) * sizeof(std::int32_t));
    d_committed.fill(0xcd);
    d_terminal.fill(0xcd);

    Tensor t_tokens(d_tokens.p, DType::I32, {width, batch});
    Tensor t_counts(d_counts.p, DType::I32, {batch});
    Tensor t_budgets(d_budgets.p, DType::I32, {batch});
    Tensor t_stops(d_stops.p, DType::I32, {max_stops, batch});
    Tensor t_scounts(d_scounts.p, DType::I32, {batch});
    Tensor t_committed(d_committed.data(), DType::I32, {batch});
    Tensor t_terminal(d_terminal.data(), DType::I32, {batch});

    ops::terminal_check(t_tokens, t_counts, t_budgets, t_stops, t_scounts,
                        t_committed, t_terminal, batch, width, nullptr);
    cuda_synchronize();

    int failures = 0;
    failures += verify_exact(
        (std::string(label) + " committed_counts").c_str(),
        from_device<std::int32_t>(d_committed.data(), static_cast<std::size_t>(batch)),
        expected_committed);
    failures += verify_exact(
        (std::string(label) + " terminal_flags").c_str(),
        from_device<std::int32_t>(d_terminal.data(), static_cast<std::size_t>(batch)),
        expected_terminal);
    failures += d_committed.verify_guards((std::string(label) + " committed guards").c_str());
    failures += d_terminal.verify_guards((std::string(label) + " terminal guards").c_str());
    return failures;
}

} // namespace

int main() {
    if (cuda_unavailable()) {
        std::cout << "terminal_check: SKIP (CUDA unavailable)\n";
        return 77;
    }

    int failures = 0;

    // Case 1: Stop token found at position 2 (MTP width=6)
    failures += run_case("stop_at_pos2", 6, {
        {{10, 20, 99, 30, 40, 50}, 6, 10, {99}, 3, 1},
    });

    // Case 2: No stop token, budget sufficient
    failures += run_case("no_stop_budget_ok", 6, {
        {{10, 20, 30, 40, 50, 60}, 3, 10, {99}, 3, 0},
    });

    // Case 3: No stop token, budget exhausted
    failures += run_case("no_stop_budget_exhausted", 6, {
        {{10, 20, 30, 40, 50, 60}, 3, 2, {99}, 2, 1},
    });

    // Case 4: Ordinary path (width=1), stop token
    failures += run_case("ordinary_stop", 1, {
        {{99}, 1, 5, {99}, 1, 1},
    });

    // Case 5: Ordinary path (width=1), no stop, budget ok
    failures += run_case("ordinary_continue", 1, {
        {{42}, 1, 5, {99}, 1, 0},
    });

    // Case 6: Multi-row batch - stop, budget exhausted, continue
    failures += run_case("multi_row", 6, {
        {{10, 20, 99, 30, 40, 50}, 6, 10, {99}, 3, 1},  // stop at pos 2
        {{10, 20, 30, 40, 50, 60}, 3, 2, {99}, 2, 1},    // budget exhausted
        {{10, 20, 30, 40, 50, 60}, 3, 10, {99}, 3, 0},   // continue
    });

    // Case 7: Stop found but budget limits commit before stop
    failures += run_case("stop_beyond_budget", 6, {
        {{10, 20, 30, 99, 40, 50}, 4, 2, {99}, 2, 1},  // stop at pos 3 but budget=2, so committed=2
    });

    // Case 8: Multiple stop tokens in table
    failures += run_case("multi_stop_tokens", 6, {
        {{10, 55, 30, 40, 50, 60}, 4, 10, {99, 55, 77}, 2, 1},  // 55 is a stop at pos 1
    });

    if (failures != 0) {
        std::cerr << "terminal_check failures=" << failures << '\n';
        return 1;
    }
    std::cout << "terminal_check: PASS\n";
    return 0;
}
