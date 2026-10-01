// Tests for PinnedHostPool (src/core/pinned_host_pool.h) -- the elastic pinned host budget.
//
// These run WITHOUT a GPU, which is the point of the injected chunk source: production pins real memory
// (cudaMallocHost) while this file pins ordinary host memory. What is under test here is the pool's
// BOOKKEEPING -- growth, address stability, fragmentation, shrink safety -- not that the memory is pinned;
// `test_state_image.cpp` covers the real pinning.
//
// The cases that would otherwise go untested until production are the ones that decided the design:
// growth must not move an existing allocation (raw pointers are held across in-flight copies), and shrink
// must refuse while any allocation in a chunk is live.

#include "core/pinned_host_pool.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

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

// A chunk source backed by ordinary host memory. It counts pins and unpins so the tests can assert that
// the pool does not leak chunks, and it can be told to start failing, so the "could not pin" path is
// exercised rather than assumed.
struct FakeChunks {
    std::size_t pins           = 0;
    std::size_t unpins         = 0;
    std::size_t bytes_pinned   = 0;
    std::size_t fail_after     = static_cast<std::size_t>(-1);

    static void* pin(void* self, std::size_t bytes) {
        auto* chunks = static_cast<FakeChunks*>(self);
        if (chunks->pins >= chunks->fail_after) { return nullptr; }
        void* memory = std::malloc(bytes == 0U ? 1U : bytes);
        if (memory == nullptr) { return nullptr; }
        ++chunks->pins;
        chunks->bytes_pinned += bytes;
        return memory;
    }
    static void unpin(void* self, void* base) {
        auto* chunks = static_cast<FakeChunks*>(self);
        ++chunks->unpins;
        std::free(base);
    }
};

ninfer::PinnedHostPool make_pool(FakeChunks& chunks, std::size_t chunk_bytes,
                                std::size_t initial_bytes = 0U) {
    ninfer::PinnedHostPool::Config config;
    config.chunk_bytes   = chunk_bytes;
    config.alignment     = 256U;
    config.initial_bytes = initial_bytes;
    return ninfer::PinnedHostPool(
        config,
        [&chunks](std::size_t bytes) { return FakeChunks::pin(&chunks, bytes); },
        [&chunks](void* base) { FakeChunks::unpin(&chunks, base); });
}

void test_grows_on_demand() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
    check(pool.chunk_count() == 0U && pool.capacity_bytes() == 0U, "starts empty (no fixed ceiling)");

    auto first = pool.allocate(1U << 20U);
    check(first.has_value(), "first allocation pins a chunk");
    check(pool.chunk_count() == 1U && chunks.pins == 1U, "exactly one chunk pinned");
    check(pool.capacity_bytes() >= (1U << 20U), "capacity grew by at least the chunk");

    auto second = pool.allocate(1U << 20U);
    check(second.has_value(), "second allocation");
    check(pool.chunk_count() >= 2U, "a second chunk was pinned when the first filled");
    check(chunks.pins == pool.chunk_count(), "one pin per chunk");
    check(pool.growth_count() == pool.chunk_count(), "growth_count tracks pins");

    const std::size_t before = pool.occupied_bytes();
    check(before >= 2U << 20U, "occupied accounts both allocations");
    check(pool.free_bytes() + pool.occupied_bytes() == pool.capacity_bytes(), "free + occupied == capacity");
}

// The property the design rests on: growth adds chunks and MOVES NOTHING.
void test_growth_preserves_addresses() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
    auto held = pool.allocate(4096U);
    check(held.has_value(), "held allocation");
    std::byte* const address = pool.data(*held);
    check(address != nullptr, "held allocation has an address");
    std::memset(address, 0x5A, 4096U);

    for (int i = 0; i < 4; ++i) {
        auto more = pool.allocate(1U << 20U);
        check(more.has_value(), "growth allocation");
    }
    check(pool.chunk_count() >= 2U, "the pool grew");
    check(pool.data(*held) == address, "the held allocation's address did not move");

    bool intact = true;
    for (std::size_t i = 0; i < 4096U; ++i) {
        if (address[i] != std::byte{0x5A}) { intact = false; break; }
    }
    check(intact, "its contents survived the growth");
}

void test_fragmentation_is_visible() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 4U << 20U);
    // Four quarters of one chunk, then free the middle two: 2 MiB free, in two 1 MiB runs.
    auto a = pool.allocate(1U << 20U);
    auto b = pool.allocate(1U << 20U);
    auto c = pool.allocate(1U << 20U);
    auto d = pool.allocate(1U << 20U);
    check(a && b && c && d, "four quarter-chunk allocations");
    const std::size_t chunks_before = pool.chunk_count();
    // Free the FIRST and THIRD quarters: adjacent runs would be coalesced by release(), which is correct
    // and is not fragmentation -- an earlier version of this test freed two neighbours and then asserted
    // the pool was fragmented, which it was not.
    (void)pool.release(*a);
    (void)pool.release(*c);
    check(pool.largest_free_run() < (2U << 20U), "largest run reports the fragmentation");
    // A 2 MiB request therefore cannot be satisfied in place and must grow -- and it must NOT fail.
    auto big = pool.allocate(2U << 20U);
    check(big.has_value(), "a 2 MiB request still succeeds (by growing)");
    check(pool.chunk_count() > chunks_before, "it grew rather than reporting failure");
    // THE COUNTER, asserted here rather than in a parallel test because this is already the exact path: the
    // FIRST placement attempt failed while free BYTES were sufficient, which is the only fragmentation
    // signature that is reachable. Delete the increment and this line fails.
    check(pool.allocation_fragmented_misses() == 1U,
          "the first-attempt miss with sufficient free bytes is counted");
    check(pool.allocation_post_grow_failures() == 0U,
          "and the post-grow path did NOT fire (it is believed unreachable)");
    check(pool.allocation_ram_refusals() == 0U, "nor was this a RAM refusal -- it grew successfully");
}

void test_shrink_refuses_while_live() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
    auto held = pool.allocate(1024U);
    check(held.has_value(), "an allocation to hold the chunk");
    check(!pool.shrink_idle(), "shrink refuses while that chunk holds a live allocation");
    check(pool.chunk_count() == 1U, "the chunk is still there");

    check(pool.release(*held), "release succeeds");
    const std::size_t capacity_before = pool.capacity_bytes();
    check(pool.shrink_idle(), "shrink then succeeds on a chunk holding nothing");
    check(pool.capacity_bytes() < capacity_before, "capacity came DOWN (the accounting follows the unpin)");
    check(pool.chunk_count() == 0U, "no live chunks remain");
    check(!pool.shrink_idle(), "a second shrink has nothing to do");
}

// A released chunk's index must not be reused for a DIFFERENT chunk while handles into it exist: the pool
// keeps the slot as a tombstone and reuses it for the next pin, so `Handle::chunk` stays meaningful.
void test_handles_survive_a_shrink() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
    auto first = pool.allocate(1024U);
    check(first.has_value(), "first chunk allocation");
    // Fill the first chunk so a second one is pinned and stays live.
    std::vector<ninfer::PinnedHostPool::Handle> held;
    for (int i = 0; i < 300; ++i) {
        auto handle = pool.allocate(4096U);
        if (!handle) { break; }
        held.push_back(*handle);
    }
    check(pool.chunk_count() >= 2U, "more than one chunk");
    const std::size_t live_chunks = pool.chunk_count();
    // Empty the FIRST chunk completely before shrinking. An earlier version released only one allocation and
    // expected a chunk to be freed -- but 256 others still occupied it, and `shrink_idle` correctly refuses
    // any chunk holding a live allocation. That refusal is the safety property; the test was wrong.
    check(pool.release(*first), "release the first chunk's allocation");
    std::size_t released_in_first = 0;
    for (const auto& handle : held) {
        if (handle.chunk == first->chunk && pool.release(handle)) { ++released_in_first; }
    }
    check(released_in_first > 0U, "released the first chunk's other occupants");
    std::size_t shrunk = 0;
    while (pool.shrink_idle()) { ++shrunk; }
    check(shrunk >= 1U, "at least one chunk was unpinned once it held nothing");
    check(pool.chunk_count() < live_chunks, "chunk_count reflects the unpin (it counts live chunks)");
    bool all_valid = true;
    for (const auto& handle : held) {
        if (handle.chunk == first->chunk) { continue; }  // released above, deliberately
        if (pool.data(handle) == nullptr) { all_valid = false; break; }
    }
    check(all_valid, "every surviving handle still resolves after the shrink");
    check(pool.occupied_bytes() > 0U, "and the live allocations are still accounted");
}

void test_policy_refusal_is_counted() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
    pool.set_growth_policy([](std::size_t) { return false; });  // e.g. free RAM below the reserve
    auto refused = pool.allocate(1U << 20U);
    check(!refused.has_value(), "a policy refusal reaches the caller as a refusal");
    check(pool.growth_refusals() == 1U, "the refusal is counted (not indistinguishable from 'never needed')");
    check(pool.allocation_refusals() == 1U, "the allocation refusal is counted too");
    // THE SPLIT: a policy refusal is NOT a failed pin. These two shared one counter, and swapping the
    // increments now fails here.
    check(pool.growth_policy_refusals() == 1U, "counted as a POLICY refusal");
    check(pool.growth_pin_failures() == 0U, "and not as a pin failure");
    // AND THE ALLOCATION HALF, which nothing asserted: without these, a mutation making ram->post_grow (or
    // deleting the ram increment) survived, because the total is the same either way. This is the
    // no-room-and-could-not-grow case, so it is the RAM half and NOT the fragmentation one.
    check(pool.allocation_ram_refusals() == 1U, "the allocation refusal is the RAM half");
    check(pool.allocation_post_grow_failures() == 0U, "not the post-grow half");
    check(pool.allocation_fragmented_misses() == 0U,
          "and not a fragmentation miss -- the pool was empty, so nothing was fragmented");
    check(chunks.pins == 0U, "nothing was pinned against the policy");
}

void test_pin_failure_is_not_a_crash() {
    FakeChunks chunks;
    chunks.fail_after = 0U;  // the source cannot pin at all
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
    auto refused = pool.allocate(1U << 20U);
    check(!refused.has_value(), "a failed pin is a refusal, not a throw");
    check(pool.growth_refusals() == 1U, "the failed pin is counted");
    check(pool.growth_pin_failures() == 1U, "counted as a PIN failure");
    check(pool.growth_policy_refusals() == 0U, "and not as a policy refusal");
}

void test_release_semantics() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
    auto handle = pool.allocate(1000U);
    check(handle.has_value(), "allocate");
    check(pool.release(*handle), "release once succeeds");
    check(!pool.release(*handle), "a second release of the same handle fails");
    check(pool.data(*handle) == nullptr, "a released handle resolves to nothing");
    check(pool.occupied_bytes() == 0U, "occupancy returned to zero");
    // Alignment: a one-byte request occupies a whole aligned unit, and the address is aligned.
    auto small = pool.allocate(1U);
    check(small.has_value(), "a one-byte allocation");
    const auto address = reinterpret_cast<std::uintptr_t>(pool.data(*small));
    check(address % 256U == 0U, "its address is 256-byte aligned");
    check(pool.occupied_bytes() == 256U, "and it occupies one aligned unit, not one byte");
}

void test_destructor_unpins_everything() {
    FakeChunks chunks;
    {
        ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
        (void)pool.allocate(1U << 20U);
        (void)pool.allocate(1U << 20U);
        check(chunks.unpins == 0U, "nothing unpinned while the pool lives");
    }
    check(chunks.unpins == chunks.pins, "every pinned chunk was unpinned on destruction");
}


void test_can_serve_and_shortfall() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
    check(!pool.can_serve(1U << 20U), "nothing pinned: can_serve says no");
    check(pool.shortfall_for(1U << 20U) == (1U << 20U), "and the shortfall names the full amount");
    auto handle = pool.allocate(1U << 20U);
    check(handle.has_value(), "allocate fills the chunk");
    check(!pool.can_serve(1024U), "a full chunk cannot serve even a small request");
    check(pool.shortfall_for(1024U) == 1024U, "shortfall is the aligned request when nothing is free");
    check(pool.release(*handle), "release");
    check(pool.can_serve(1U << 20U), "the released chunk can serve again");
    check(pool.shortfall_for(1U << 20U) == 0U, "and the shortfall is zero");
}

// carve_exact is what a `noexcept` recipe applier needs: it must never grow, and must refuse a region that
// is not free rather than quietly placing the allocation somewhere else.
void test_carve_exact_is_pure_and_strict() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
    auto handle = pool.allocate(1U << 20U);
    check(handle.has_value(), "a chunk to carve from");
    check(pool.release(*handle), "released so the whole chunk is free");
    const std::uint64_t revision_before = pool.revision();
    const std::size_t pins_before = chunks.pins;

    auto carved = pool.carve_exact(handle->chunk, 0U, 1024U);
    check(carved.has_value(), "carving a free aligned region succeeds");
    check(pool.data(*carved) != nullptr, "the carved region has an address");
    check(pool.occupied_bytes() == 1024U, "and is accounted");
    check(chunks.pins == pins_before, "carving pinned nothing (it is pure)");

    check(!pool.carve_exact(handle->chunk, 0U, 1024U).has_value(), "the same region cannot be carved twice");
    check(!pool.carve_exact(handle->chunk, 5U, 256U).has_value(), "an unaligned offset is refused");
    check(!pool.carve_exact(handle->chunk, 0U, 300U).has_value(), "an unaligned size is refused");
    check(!pool.carve_exact(99U, 0U, 256U).has_value(), "a chunk that does not exist is refused");
    check(pool.revision() == revision_before, "carving did not change the revision (nothing was pinned)");
}

void test_revision_marks_size_changes() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
    const std::uint64_t start = pool.revision();
    auto handle = pool.allocate(1U << 20U);
    check(handle.has_value(), "allocate (pins a chunk)");
    check(pool.revision() != start, "growth bumps the revision, so a pre-growth plan is detectable");
    const std::uint64_t after_growth = pool.revision();
    check(pool.release(*handle), "release");
    check(pool.revision() == after_growth, "a release is not a size change");
    check(pool.shrink_idle(), "shrink");
    check(pool.revision() != after_growth, "shrink bumps it too (a plan made before it is stale)");
}

// The ordering `shrink_idle` depends on: first-fit consumes the OLDEST chunk first, so the NEWEST chunk is
// the one that goes idle and can be given back.
// BEST-FIT WITHIN A CHUNK, pinned so the policy cannot silently revert. The fixture is built so the two
// policies give DIFFERENT answers: after the frees below the chunk holds a 512 KiB run at offset 0 and a
// 256 KiB run at 768 KiB, and a 128 KiB request fits both. First-fit takes the one it meets first (512 KiB,
// and splits its FRONT, cutting it to 384 KiB); best-fit takes the tightest (256 KiB) and leaves the 512 KiB
// run whole. `largest_free_run` distinguishes them, 512 KiB against 384 KiB.
void test_best_fit_protects_the_large_run() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);  // 1 MiB chunk
    auto a = pool.allocate(512U << 10U);
    auto b = pool.allocate(256U << 10U);
    check(a && b, "two allocations to carve the chunk");
    check(pool.release(*a), "release the first, leaving a 512 KiB run at the front");
    const std::size_t before = pool.largest_free_run();
    check(before == (512U << 10U), "the front run is 512 KiB before the tight allocation");
    auto c = pool.allocate(128U << 10U);
    check(c.has_value(), "a 128 KiB request is served");
    check(c->chunk == a->chunk, "and it stays in the same chunk (chunks are consumed in order)");
    check(pool.largest_free_run() == (512U << 10U),
          "BEST-FIT: the 512 KiB run is untouched -- first-fit would have cut it to 384 KiB");
}

void test_first_fit_consumes_the_oldest_chunk_first() {
    FakeChunks chunks;
    ninfer::PinnedHostPool pool = make_pool(chunks, 1U << 20U);
    auto small = pool.allocate(4096U);          // lands in chunk 0
    check(small.has_value(), "small allocation in the first chunk");
    auto big = pool.allocate(2U << 20U);        // cannot fit in chunk 0 -> pins another chunk
    check(big.has_value(), "big allocation pins a further chunk");
    check(pool.chunk_count() == 2U, "two chunks");
    check(big->chunk > small->chunk, "the big one landed in the NEWER chunk");
    check(!pool.chunk_is_idle(small->chunk), "the oldest chunk is not idle");
    check(pool.release(*big), "release the newer chunk's occupant");
    check(pool.chunk_is_idle(big->chunk), "the newer chunk is now idle");
    check(pool.shrink_idle(), "so it is the one shrink gives back");
    check(pool.chunk_count() == 1U, "one chunk remains");
    check(pool.data(*small) != nullptr, "and the older chunk's allocation is untouched");
}

}  // namespace

int main() {
    test_grows_on_demand();
    test_growth_preserves_addresses();
    test_fragmentation_is_visible();
    test_shrink_refuses_while_live();
    test_handles_survive_a_shrink();
    test_policy_refusal_is_counted();
    test_pin_failure_is_not_a_crash();
    test_release_semantics();
    test_destructor_unpins_everything();
    test_can_serve_and_shortfall();
    test_carve_exact_is_pure_and_strict();
    test_revision_marks_size_changes();
    test_best_fit_protects_the_large_run();
    test_first_fit_consumes_the_oldest_chunk_first();

    if (failures != 0) { std::printf("%d FAILURE(S)\n", failures); return 1; }
    std::printf("all pinned-host-pool checks passed\n");
    return 0;
}
