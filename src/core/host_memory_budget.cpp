#include "core/host_memory_budget.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ninfer {
namespace {

// /proc/meminfo values are in kB. Returns false when the field is absent or unparseable, which leaves the
// reading invalid rather than treating a missing field as zero -- "Shmem: absent" and "Shmem: 0" are very
// different statements about a machine, and only one of them is safe to grow on.
bool field_kib(std::string_view text, const char* name, std::size_t& out_bytes) noexcept {
    const std::size_t name_length = std::strlen(name);
    std::size_t       position    = 0U;
    while (position < text.size()) {
        const std::size_t line_end = text.find('\n', position);
        const std::size_t end      = line_end == std::string_view::npos ? text.size() : line_end;
        const std::string_view line = text.substr(position, end - position);
        if (line.size() > name_length && line.compare(0U, name_length, name) == 0 &&
            line[name_length] == ':') {
            std::size_t cursor = name_length + 1U;
            while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) { ++cursor; }
            const std::size_t digits_begin = cursor;
            while (cursor < line.size() && line[cursor] >= '0' && line[cursor] <= '9') { ++cursor; }
            if (cursor == digits_begin) { return false; }
            // strtoull on a bounded copy: the digits are known to be digits and the length is bounded.
            char buffer[32] = {};
            const std::size_t length = (cursor - digits_begin) < (sizeof(buffer) - 1U)
                                           ? (cursor - digits_begin)
                                           : (sizeof(buffer) - 1U);
            std::memcpy(buffer, line.data() + digits_begin, length);
            out_bytes = static_cast<std::size_t>(std::strtoull(buffer, nullptr, 10)) * 1024U;
            return true;
        }
        if (line_end == std::string_view::npos) { break; }
        position = line_end + 1U;
    }
    return false;
}

}  // namespace

HostMemoryReading parse_meminfo(std::string_view text) noexcept {
    HostMemoryReading reading;
    const bool available = field_kib(text, "MemAvailable", reading.mem_available);
    const bool shmem     = field_kib(text, "Shmem", reading.shmem);
    const bool swap      = field_kib(text, "SwapFree", reading.swap_free);
    // All three are required. A partial reading cannot answer "is there room", and answering it from part
    // of the picture is how a guard passes while the machine dies.
    reading.valid = available && shmem && swap;
    return reading;
}

HostMemoryReading read_host_memory() noexcept {
    std::FILE* file = std::fopen("/proc/meminfo", "rb");
    if (file == nullptr) { return HostMemoryReading{}; }
    std::string buffer;
    char        chunk[4096];
    std::size_t read = 0U;
    while ((read = std::fread(chunk, 1U, sizeof(chunk), file)) > 0U) {
        buffer.append(chunk, read);
        if (buffer.size() > (1U << 20U)) { break; }  // /proc/meminfo is ~1.5 kB; this is a sanity bound
    }
    std::fclose(file);
    return parse_meminfo(buffer);
}

GrowthVeto HostMemoryBudget::veto_for(const HostMemoryReading& reading,
                                      std::size_t bytes) const noexcept {
    if (bytes == 0U) { return GrowthVeto::NothingWanted; }
    if (!reading.valid) { return GrowthVeto::InvalidReading; }  // fail closed: see the header
    // The reserve is the safety property: pinned memory may grow into what is available, never into the
    // RAM that the weights' host side, the media cache and the OS need to keep this machine alive.
    if (reading.mem_available < config_.reserve_bytes) { return GrowthVeto::Reserve; }
    if (bytes > reading.mem_available - config_.reserve_bytes) { return GrowthVeto::Reserve; }
    if (config_.max_bytes != 0U) {
        if (pinned_bytes_ >= config_.max_bytes) { return GrowthVeto::MaxBytes; }
        if (bytes > config_.max_bytes - pinned_bytes_) { return GrowthVeto::MaxBytes; }
    }
    if (config_.shmem_cap_bytes != 0U) {
        if (reading.shmem > config_.shmem_cap_bytes) { return GrowthVeto::ShmemCap; }
        if (bytes > config_.shmem_cap_bytes - reading.shmem) { return GrowthVeto::ShmemCap; }
    }
    return GrowthVeto::None;
}

bool HostMemoryBudget::decide(const HostMemoryReading& reading, std::size_t bytes) const noexcept {
    return veto_for(reading, bytes) == GrowthVeto::None;
}

bool HostMemoryBudget::allow(std::size_t bytes) noexcept {
    return allow_with(read_host_memory(), bytes);
}

bool HostMemoryBudget::allow_with(const HostMemoryReading& reading, std::size_t bytes) noexcept {
    reading_ = reading;
    const GrowthVeto veto = veto_for(reading_, bytes);
    const bool ok         = veto == GrowthVeto::None;
    if (ok) {
        ++approvals_;
    } else {
        ++refusals_;
        last_veto_               = veto;
        last_wanted_bytes_       = bytes;
        last_veto_mem_available_ = reading_.mem_available;
    }
    return ok;
}

std::size_t HostMemoryBudget::max_pinnable() const noexcept {
    if (!reading_.valid) { return 0U; }
    std::size_t limit = reading_.mem_available > config_.reserve_bytes
                            ? reading_.mem_available - config_.reserve_bytes
                            : 0U;
    if (config_.max_bytes != 0U) {
        const std::size_t remaining = config_.max_bytes > pinned_bytes_ ? config_.max_bytes - pinned_bytes_ : 0U;
        limit = std::min(limit, remaining);
    }
    if (config_.shmem_cap_bytes != 0U) {
        const std::size_t remaining =
            config_.shmem_cap_bytes > reading_.shmem ? config_.shmem_cap_bytes - reading_.shmem : 0U;
        limit = std::min(limit, remaining);
    }
    return limit;
}

}  // namespace ninfer
