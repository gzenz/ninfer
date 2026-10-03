#include "core/device.h"
#include "models/qwen3_5/state/state_image.h"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace q36 = ninfer::models::qwen3_5;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

bool cuda_unavailable(cudaError_t error) {
    return error == cudaErrorNoDevice || error == cudaErrorInsufficientDriver;
}

struct PlannedPool {
    q36::StateImageDeviceLayout layout;
    std::size_t bytes = 0;
};

PlannedPool plan_pool(bool dflash, std::int32_t slots = 4, bool dflash2 = false) {
    q36::StateImageSpec spec{
        .linear =
            {
                .layers         = 2,
                .conv_channels  = 5,
                .conv_width     = 3,
                .value_heads    = 2,
                .value_head_dim = 4,
                .key_head_dim   = 3,
                .slot_count     = slots,
                .conv_dtype     = ninfer::DType::BF16,
            },
        .hidden = 7,
    };
    if (dflash) {
        spec.dflash_local = dflash2
                                ? q36::DFlashLocalStateSpec{.layers   = 5,
                                                            .capacity = 2048,
                                                            .kv_heads = 8,
                                                            .head_dim = 128}
                                : q36::DFlashLocalStateSpec{
                                      .layers = 2, .capacity = 17, .kv_heads = 2, .head_dim = 4};
    }
    ninfer::LayoutBuilder builder;
    q36::StateImageDeviceLayout layout = q36::plan_state_image_device_pool(builder, spec);
    return {.layout = std::move(layout), .bytes = builder.finish(256)};
}

void set_bytes(const ninfer::Tensor& tensor, unsigned char value) {
    CUDA_CHECK(cudaMemset(tensor.data, value, tensor.bytes()));
}

void expect_bytes(const ninfer::Tensor& tensor, unsigned char expected, std::string_view label) {
    std::vector<unsigned char> host(tensor.bytes());
    CUDA_CHECK(cudaMemcpy(host.data(), tensor.data, host.size(), cudaMemcpyDeviceToHost));
    for (const unsigned char value : host) {
        if (value != expected) {
            ++failures;
            std::cerr << "FAIL: " << label << " differs\n";
            return;
        }
    }
}

void fill_slot(q36::StateImageDevicePool& pool, std::int32_t slot, unsigned char base) {
    for (std::uint32_t layer = 0; layer < pool.linear().layer_count(); ++layer) {
        set_bytes(pool.linear().conv_slot(layer, slot), static_cast<unsigned char>(base + layer));
        set_bytes(pool.linear().recurrent_slot(layer, slot),
                  static_cast<unsigned char>(base + 0x10 + layer));
    }
    set_bytes(pool.continuation_hidden_slot(slot), static_cast<unsigned char>(base + 0x20));
    if (ninfer::CyclicKVCache* local = pool.dflash_local(); local != nullptr) {
        for (std::uint32_t layer = 0; layer < local->layer_count(); ++layer) {
            const auto view = local->layer_view(layer);
            set_bytes(view.k.slice(3, slot, 1), static_cast<unsigned char>(base + 0x30 + layer));
            set_bytes(view.v.slice(3, slot, 1), static_cast<unsigned char>(base + 0x40 + layer));
        }
    }
}

void expect_slot(q36::StateImageDevicePool& pool, std::int32_t slot, unsigned char base,
                 std::string_view label) {
    for (std::uint32_t layer = 0; layer < pool.linear().layer_count(); ++layer) {
        expect_bytes(pool.linear().conv_slot(layer, slot), static_cast<unsigned char>(base + layer),
                     label);
        expect_bytes(pool.linear().recurrent_slot(layer, slot),
                     static_cast<unsigned char>(base + 0x10 + layer), label);
    }
    expect_bytes(pool.continuation_hidden_slot(slot), static_cast<unsigned char>(base + 0x20),
                 label);
    if (ninfer::CyclicKVCache* local = pool.dflash_local(); local != nullptr) {
        for (std::uint32_t layer = 0; layer < local->layer_count(); ++layer) {
            const auto view = local->layer_view(layer);
            expect_bytes(view.k.slice(3, slot, 1), static_cast<unsigned char>(base + 0x30 + layer),
                         label);
            expect_bytes(view.v.slice(3, slot, 1), static_cast<unsigned char>(base + 0x40 + layer),
                         label);
        }
    }
}

void expect_zero_slot(q36::StateImageDevicePool& pool, std::int32_t slot, std::string_view label) {
    for (std::uint32_t layer = 0; layer < pool.linear().layer_count(); ++layer) {
        expect_bytes(pool.linear().conv_slot(layer, slot), 0, label);
        expect_bytes(pool.linear().recurrent_slot(layer, slot), 0, label);
    }
    expect_bytes(pool.continuation_hidden_slot(slot), 0, label);
    if (ninfer::CyclicKVCache* local = pool.dflash_local(); local != nullptr) {
        for (std::uint32_t layer = 0; layer < local->layer_count(); ++layer) {
            const auto view = local->layer_view(layer);
            expect_bytes(view.k.slice(3, slot, 1), 0, label);
            expect_bytes(view.v.slice(3, slot, 1), 0, label);
        }
    }
}

void test_host_roundtrip(bool dflash, ninfer::DeviceContext& device, bool dflash2 = false) {
    PlannedPool planned = plan_pool(dflash, 2, dflash2);
    if (dflash2) {
        expect(q36::dflash_local_transfer_work(planned.layout.host).payload_bytes ==
                   40ULL * 1024 * 1024,
               "DFlash2 local snapshot must transfer exactly 40 MiB");
    }
    ninfer::DeviceArena arena(planned.bytes);
    q36::StateImageDevicePool pool({arena.base(), arena.capacity()}, planned.layout);
    fill_slot(pool, 0, dflash ? 0x19 : 0x25);
    pool.zero_slot(1, device.stream);

    // A GPU-free chunk source: what is under test is the pool's bookkeeping, and a pool whose growth can
    // only be exercised on a machine with a GPU is a growth path that is rarely exercised. `chunk_bytes` is
    // set to ONE IMAGE so the second slot has to pin a second chunk -- that is what makes the
    // address-stability assertion below a real test of growth rather than of packing.
    ninfer::PinnedHostPool host_pool(
        ninfer::PinnedHostPool::Config{/*chunk_bytes=*/planned.layout.host.image_bytes, /*alignment=*/256U},
        [](std::size_t bytes) { return std::malloc(bytes); },
        [](void* base) { std::free(base); });
    q36::HostStatePool host(planned.layout.host, host_pool);
    expect(host.reserve_slots(1) == 1U, "HostStatePool reserves its configured initial slot");
    const auto handle = host.allocate();
    expect(handle.has_value(), "HostStatePool allocates its reserved slot");
    expect(!host.allocate().has_value(),
           "the PURE allocate reports capacity exhaustion (it never grows)");
    expect(host.occupied() == 1, "HostStatePool occupied count after allocation");

    pool.copy_to_host(0, host.writable_view(*handle), device.stream);
    device.synchronize();
    pool.copy_from_host(host.view(*handle), 1, device.stream);
    device.synchronize();
    expect_slot(pool, 1, dflash ? 0x19 : 0x25,
                dflash ? "DFlash Host roundtrip" : "common Host roundtrip");

    const q36::HostStateSlotHandle stale = *handle;
    expect(host.release(stale), "HostStatePool releases a live handle");
    expect(host.occupied() == 0, "HostStatePool occupied count after release");
    bool stale_view_rejected = false;
    try {
        (void)host.view(stale);
    } catch (const std::invalid_argument&) { stale_view_rejected = true; }
    expect(stale_view_rejected, "HostStatePool rejects a stale view");
    const auto reused = host.allocate();
    expect(reused && reused->index == stale.index && reused->generation != stale.generation,
           "HostStatePool reuse advances generation");
    expect(!host.release(stale), "HostStatePool rejects stale release");
    expect(host.release(*reused), "HostStatePool releases the reused slot");

    // GROWTH, and the property the whole elastic design rests on: adding a slot must NOT move an existing
    // one, because views into these bytes are held across in-flight H2D/D2H copies.
    const auto held = host.allocate();
    expect(held.has_value(), "a slot to hold across a growth");
    const std::byte* const address_before = host.writable_view(*held).data;
    // Relative, not absolute: the INITIAL reservation is itself a growth, so a fixed expected count would
    // encode how the test happens to be set up rather than what this line is testing.
    const std::uint64_t growth_before = host.growth_count();
    expect(host.capacity() == 1U, "capacity is one slot before the growth");
    const auto grown = host.allocate_growing();
    expect(grown.has_value(), "allocate_growing adds a slot when none is free");
    expect(host.capacity() == 2U, "capacity followed the growth");
    expect(host.growth_count() == growth_before + 1U,
           "the growth is counted (a silent capacity change is not)");
    expect(host.writable_view(*held).data == address_before,
           "the existing slot's address did NOT move -- the pool grew by pinning another chunk");
    expect(host.writable_view(*grown).data != nullptr, "the new slot has an address");
    expect(host.writable_view(*grown).data != address_before, "and it is a different one");
    expect(host.release(*held) && host.release(*grown), "both slots release");

    // THE TRIM FLOOR, which the review of 2026-09-26 found missing: the first version trimmed every idle
    // trailing slot, and `capacity()` feeds `admission_capacity()`, which the planner treats as the host
    // state limit -- so trimming to zero would leave no room to demote INTO, which is the 16/16 incident
    // made worse.
    const std::uint32_t floor = host.capacity();
    expect(host.reserve_slots(3) == 3U, "three more slots for the trim fixture");
    expect(host.capacity() == floor + 3U, "capacity grew");
    expect(host.trim_idle_slots(floor) == 3U, "trim gives back exactly what is above the floor");
    expect(host.capacity() == floor, "capacity is back AT the floor, never below it");
    expect(host.trim_idle_slots(floor) == 0U, "and a second trim has nothing to give back");
}

} // namespace

// THE POSITIVE CONTROL for §3 item 6's pre-grow, and it exists because its first version had a bug no run
// could see: `reserve_slots(count)` ADDS `count` slots, so the caller's `capacity + 1` DOUBLED the pool
// (16 -> 33 -> 67, ~3 GiB pinned synchronously) instead of growing it by one. Nothing exercised it, so
// nothing failed. This asserts the DIFFERENCE in capacity, not the function's return value, and it is
// host-only: malloc-backed chunks, no device, so it runs anywhere.
void test_host_state_pregrow() {
    const q36::StateImageHostLayout host_layout = plan_pool(true).layout.host;
    const auto make_pool = [&](std::uint64_t max_bytes) {
        auto pinned = std::make_unique<ninfer::PinnedHostPool>(
            ninfer::PinnedHostPool::Config{/*chunk_bytes=*/host_layout.image_bytes,
                                           /*alignment=*/256U,
                                           /*initial_bytes=*/0U,
                                           /*max_bytes=*/max_bytes},
            [](std::size_t bytes) { return std::malloc(bytes); },
            [](void* base) { std::free(base); });
        return std::make_pair(std::move(pinned), std::make_unique<q36::HostStatePool>(host_layout, *pinned));
    };

    {
        // FULL by exactly one slot -> grew by EXACTLY ONE. Under the shipped `capacity + 1` call this read
        // `capacity * 2 + 1` and the assertion below is what fails.
        auto [pinned, pool] = make_pool(0U);
        expect(pool->reserve_slots(2) == 2U, "pre-grow fixture reserves two slots");
        const std::uint32_t before = pool->capacity();
        const auto a = pool->allocate();
        const auto b = pool->allocate();
        expect(a.has_value() && b.has_value() && pool->occupied() == before,
               "pre-grow fixture fills the pool");
        expect(q36::pre_grow_host_state_pool(*pool) == q36::HostStatePreGrow::Grew,
               "a FULL pool is grown");
        expect(pool->capacity() == before + q36::HOST_STATE_PREGROW_SLOTS,
               "and by EXACTLY HOST_STATE_PREGROW_SLOTS -- reserve_slots ADDS, it is not a total");
        expect(pool->capacity() == before + 1U, "which is one slot, the number that matters here");
    }
    {
        // NOT FULL -> nothing pinned, nothing changed.
        auto [pinned, pool] = make_pool(0U);
        expect(pool->reserve_slots(2) == 2U, "not-full fixture reserves two slots");
        const std::uint32_t before = pool->capacity();
        expect(q36::pre_grow_host_state_pool(*pool) == q36::HostStatePreGrow::NotFull,
               "a pool with a free slot is not grown");
        expect(pool->capacity() == before, "and its capacity does not move");
    }
    {
        // CAPACITY 0 -> disabled, not "full": the startup reservation was refused, and guessing is worse.
        auto [pinned, pool] = make_pool(0U);
        expect(pool->capacity() == 0U, "disabled fixture has no slots");
        expect(q36::pre_grow_host_state_pool(*pool) == q36::HostStatePreGrow::Disabled,
               "a zero-capacity pool is DISABLED, not full");
        expect(pool->capacity() == 0U, "and nothing is pinned for it");
    }
    {
        // REFUSED -> the budget says no, and the caller counts it rather than staying silent.
        auto [pinned, pool] = make_pool(host_layout.image_bytes);  // room for the first chunk only
        // CAPTURED BEFORE THE FIXTURE'S OWN REFUSAL, so the demand path can be pinned below too.
        const std::uint64_t real_at_entry = pinned->allocation_refusals();
        expect(pool->reserve_slots(2) == 1U, "refusal fixture gets one slot, not two");
        // THE DEMAND PATH IS REAL. `reserve_slots` is called by real demand as well (the initial
        // reservation, and the capture path through `reserve_logical_destination_growing`), so a change
        // that made it pass `speculative=true` would turn real refusals into speculative ones and
        // `host_pinned_allocation_refusals` would read 0 under real failure -- the same "instrument
        // measuring nothing" shape this whole separation exists to remove. Measured: two such mutants pass
        // every check this fixture had before this assertion.
        expect(pinned->allocation_refusals() == real_at_entry + 1U,
               "a DEMAND refusal through reserve_slots moves the REAL counter");
        const auto held = pool->allocate();
        expect(held.has_value() && pool->occupied() == pool->capacity(),
               "refusal fixture fills its single slot");
        // THE COUNTERS, AND AS DELTAS. The return value alone cannot tell a SPECULATIVE refusal from a real
        // one, and this is the path a review found routing pre-grow refusals into the real allocation
        // counters (`host_state_pregrow_refusals` 2195 == `host_pinned_allocation_refusals` 2195, which
        // produced a false conclusion before the two were compared). Without these, flipping
        // `/*speculative=*/true` back to `false` at `state_image.cpp` fails NOTHING.
        //
        // DELTAS, not absolutes: this fixture's own `reserve_slots(2)` above already refuses its second slot
        // NON-speculatively, so the real counters are non-zero before the pre-grow is even called. An
        // absolute assertion here fails on a correct build -- measured, and it is why this is written the
        // long way.
        const std::uint64_t real0  = pinned->allocation_refusals();
        const std::uint64_t ram0   = pinned->allocation_ram_refusals();
        const std::uint64_t spec0  = pinned->allocation_speculative_refusals();
        expect(q36::pre_grow_host_state_pool(*pool) == q36::HostStatePreGrow::Refused,
               "a budget refusal is REPORTED, not silently absent");
        expect(pinned->allocation_speculative_refusals() == spec0 + 1U,
               "a refused STATE pre-grow is counted as SPECULATIVE");
        expect(pinned->allocation_refusals() == real0,
               "and does NOT move the real allocation-refusal counter");
        expect(pinned->allocation_ram_refusals() == ram0,
               "nor the RAM one");
        // THE REVERSE DIRECTION, explicitly: a demand allocation on the same refusing pool must still be
        // REAL. Without this the trade is pinned only one way.
        expect(!pool->allocate_growing().has_value(), "a demand allocation on the full pool is refused");
        expect(pinned->allocation_refusals() == real0 + 1U,
               "and a DEMAND refusal through allocate_growing IS counted as real");
        expect(pinned->allocation_speculative_refusals() == spec0 + 1U,
               "while the speculative count is unmoved by it");
    }
}

int main() {
    // THE ONE CASE THAT NEEDS NO GPU RUNS BEFORE THE GATE, on purpose: it is a pure bookkeeping test over
    // malloc-backed chunks, so it can and should run on a machine with no device. Called after the gate (as
    // the first version did) it silently skipped there -- a host-only test that never runs where it is most
    // useful, and a comment claiming it "runs anywhere" that was false.
    test_host_state_pregrow();

    int count                   = 0;
    const cudaError_t count_err = cudaGetDeviceCount(&count);
    if (cuda_unavailable(count_err) || count == 0) {
        std::cout << "SKIP: no usable CUDA device (the pregrow case above already ran)\n";
        return 77;
    }
    CUDA_CHECK(count_err);

    ninfer::DeviceContext device(0);
    PlannedPool planned = plan_pool(true);
    ninfer::DeviceArena arena(planned.bytes);
    q36::StateImageDevicePool pool({arena.base(), arena.capacity()}, planned.layout);

    expect(pool.slot_count() == 4, "StateImage slot count");
    expect(pool.linear().layer_count() == 2, "StateImage Linear Attention layer count");
    expect(pool.continuation_hidden_slot(0).dtype == ninfer::DType::BF16 &&
               pool.continuation_hidden_slot(0).ne[0] == 7,
           "StateImage continuation hidden geometry");
    expect(pool.dflash_local() != nullptr && pool.dflash_local()->layer_count() == 2 &&
               pool.dflash_local()->lane_capacity() == 4,
           "StateImage DFlash local geometry");
    const q36::StateImageDeviceSlotView complete = pool.slot_view(0);
    expect(complete.linear.layers == 2 && complete.continuation_hidden.ne[0] == 7 &&
               complete.dflash_local && complete.dflash_local->layers == 2,
           "StateImage slot view exposes every fixed component");

    fill_slot(pool, 0, 0x11);
    fill_slot(pool, 1, 0x52);
    pool.copy_slot(0, 2, device.stream);
    device.synchronize();
    expect_slot(pool, 0, 0x11, "StateImage D2D source isolation");
    expect_slot(pool, 2, 0x11, "StateImage D2D complete destination");

    pool.zero_slot(1, device.stream);
    device.synchronize();
    expect_zero_slot(pool, 1, "StateImage zero complete destination");
    expect_slot(pool, 0, 0x11, "StateImage zero source isolation");

    test_host_roundtrip(false, device);
    test_host_roundtrip(true, device);
    test_host_roundtrip(true, device, true);

    return failures == 0 ? 0 : 1;
}
