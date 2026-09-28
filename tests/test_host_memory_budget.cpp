// Tests for HostMemoryBudget (src/core/host_memory_budget.h) -- the growth policy for the elastic pinned
// pool. Host-only: it parses text, so the malformed cases are testable without touching this machine's
// memory state, which is the point of splitting `parse_meminfo` from `read_host_memory`.
//
// The case that justifies the type: an UNREADABLE reading must refuse. Refusing to grow costs an eviction;
// growing blind risks taking the VM down with the operator's session in it, and this host's notes already
// record pinned shmem as its failure mode.

#include "core/host_memory_budget.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* name) {
    if (condition) {
        std::printf("ok   %s\n", name);
    } else {
        std::printf("FAIL %s\n", name);
        ++failures;
    }
}

constexpr std::size_t kKib = 1024U;
constexpr std::size_t kGib = 1024U * 1024U * 1024U;

std::string well_formed(std::size_t available_kib, std::size_t shmem_kib, std::size_t swap_free_kib) {
    return "MemTotal:       53000000 kB\n"
           "MemFree:         1000000 kB\n"
           "MemAvailable:   " + std::to_string(available_kib) + " kB\n"
           "Buffers:          100000 kB\n"
           "Cached:           500000 kB\n"
           "SwapTotal:      16000000 kB\n"
           "SwapFree:       " + std::to_string(swap_free_kib) + " kB\n"
           "Shmem:          " + std::to_string(shmem_kib) + " kB\n";
}

void test_parse_well_formed() {
    const auto reading = ninfer::parse_meminfo(well_formed(38U * 1024U * 1024U, 34U * 1024U * 1024U, 16U * 1024U * 1024U));
    check(reading.valid, "a well-formed reading is valid");
    check(reading.mem_available == 38U * kGib, "MemAvailable parsed in bytes (kB x 1024)");
    check(reading.shmem == 34U * kGib, "Shmem parsed");
    check(reading.swap_free == 16U * kGib, "SwapFree parsed");
    // A field whose name is a PREFIX of another must not be mistaken for it.
    const auto prefix = ninfer::parse_meminfo("MemTotal: 100 kB\nMemAvailable: 200 kB\nShmem: 300 kB\nSwapFree: 400 kB\n");
    check(prefix.valid && prefix.mem_available == 200U * kKib, "MemAvailable is not confused with MemTotal");
}

void test_parse_malformed_is_invalid() {
    check(!ninfer::parse_meminfo("").valid, "empty input is invalid");
    check(!ninfer::parse_meminfo("garbage\n").valid, "garbage is invalid");
    check(!ninfer::parse_meminfo("MemAvailable: 100 kB\nShmem: 200 kB\n").valid,
          "a MISSING field is invalid (a partial picture must not answer 'is there room')");
    check(!ninfer::parse_meminfo("MemAvailable: kB\nShmem: 200 kB\nSwapFree: 300 kB\n").valid,
          "a field with no number is invalid");
    check(!ninfer::parse_meminfo(
              "MemAvailable: notanumber kB\nShmem: 200 kB\nSwapFree: 300 kB\n").valid,
          "a non-numeric value is invalid");
}

void test_unreadable_refuses() {
    ninfer::HostMemoryBudget budget(ninfer::HostMemoryBudgetConfig{.reserve_bytes = 4U * kGib});
    ninfer::HostMemoryReading invalid;  // valid == false
    check(!budget.decide(invalid, 1U * kGib), "an INVALID reading refuses -- fail closed");
    check(!budget.decide(invalid, 1U), "even a one-byte request is refused when the reading is invalid");
}

void test_reserve_boundary() {
    ninfer::HostMemoryBudget budget(ninfer::HostMemoryBudgetConfig{.reserve_bytes = 4U * kGib});
    const auto reading = ninfer::parse_meminfo(well_formed(10U * 1024U * 1024U, 0U, 16U * 1024U * 1024U));
    check(reading.valid, "reading for the boundary case");
    check(budget.decide(reading, 6U * kGib), "exactly (available - reserve) is allowed");
    check(!budget.decide(reading, 6U * kGib + 1U), "one byte more is refused");
    check(!budget.decide(reading, 7U * kGib), "and beyond that is refused");
    check(!budget.decide(reading, 0U), "a zero-byte growth request is refused (nothing to decide)");
}

void test_max_bytes_ceiling() {
    ninfer::HostMemoryBudget budget(
        ninfer::HostMemoryBudgetConfig{.reserve_bytes = 0U, .max_bytes = 8U * kGib});
    const auto reading = ninfer::parse_meminfo(well_formed(40U * 1024U * 1024U, 0U, 16U * 1024U * 1024U));
    check(budget.decide(reading, 8U * kGib), "up to the ceiling is allowed");
    budget.set_pinned_bytes(8U * kGib);
    check(!budget.decide(reading, 1U), "at the ceiling, nothing more is allowed");
    budget.set_pinned_bytes(4U * kGib);
    check(budget.decide(reading, 4U * kGib), "with 4 GiB pinned, 4 more reaches the ceiling exactly");
    check(!budget.decide(reading, 4U * kGib + 1U), "and one byte past it is refused");
}

void test_shmem_cap() {
    ninfer::HostMemoryBudget budget(
        ninfer::HostMemoryBudgetConfig{.reserve_bytes = 0U, .shmem_cap_bytes = 36U * kGib});
    const auto reading = ninfer::parse_meminfo(well_formed(40U * 1024U * 1024U, 34U * 1024U * 1024U, 16U * 1024U * 1024U));
    check(budget.decide(reading, 2U * kGib), "2 GiB more shmem reaches the 36 GiB cap exactly");
    check(!budget.decide(reading, 2U * kGib + 1U), "one byte past the shmem cap is refused");
    const auto over = ninfer::parse_meminfo(well_formed(40U * 1024U * 1024U, 40U * 1024U * 1024U, 16U * 1024U * 1024U));
    check(!budget.decide(over, 1U), "a machine already over the cap refuses everything");
}

void test_counters_and_max_pinnable() {
    ninfer::HostMemoryBudget budget(
        ninfer::HostMemoryBudgetConfig{.reserve_bytes = 4U * kGib, .max_bytes = 12U * kGib});
    const auto reading = ninfer::parse_meminfo(well_formed(20U * 1024U * 1024U, 2U * 1024U * 1024U, 16U * 1024U * 1024U));
    // available - reserve = 16 GiB, the ceiling leaves 12 GiB: the ceiling binds.
    check(budget.decide(reading, 12U * kGib), "allowed up to the binding limit");
    check(!budget.decide(reading, 12U * kGib + 1U), "refused past it");
    // `allow` counts what it decided, so a refusal is not silence.
    const std::uint64_t refusals_before = budget.refusals();
    (void)budget.allow(1U << 20U);  // reads THIS machine; on a healthy host this is an approval
    check(budget.refusals() + budget.approvals() > 0U, "allow() records a decision either way");
    (void)refusals_before;
}

// A smoke check on the real machine, asserted rather than tolerated: a budget that can never read
// /proc/meminfo would refuse every growth and look exactly like a pool that never needed to grow.
void test_reads_this_machine() {
    const auto reading = ninfer::read_host_memory();
    check(reading.valid, "this machine's /proc/meminfo parses (a budget that cannot read refuses everything)");
    check(reading.mem_available > 0U, "and it reports some available memory");
}

}  // namespace

int main() {
    test_parse_well_formed();
    test_parse_malformed_is_invalid();
    test_unreadable_refuses();
    test_reserve_boundary();
    test_max_bytes_ceiling();
    test_shmem_cap();
    test_counters_and_max_pinnable();
    test_reads_this_machine();

    if (failures != 0) { std::printf("%d FAILURE(S)\n", failures); return 1; }
    std::printf("all host-memory-budget checks passed\n");
    return 0;
}
