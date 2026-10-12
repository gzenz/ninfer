#include "core/device.h"
#include "models/qwen3_5/program/storage/host_kv_store.h"
#include "models/qwen3_5/program/storage/kv_store.h"
#include "models/qwen3_5/program/storage/state_store.h"

#include "models/qwen3_5/state/state_image.h"

#include <cuda_runtime.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <cstdio>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace q36   = ninfer::models::qwen3_5;
namespace store = ninfer::models::qwen3_5::detail;

int failures = 0;

// THE CENSUS VERDICT IS A PRINT, SO THE TEST HAS TO READ STDERR. A line nobody reads is the defect this whole
// instrument exists to avoid: `[census] ... TOTAL address_reserved_pages=... unaccounted=...` has no return
// value, so without this a case asserting on it would pass by measuring nothing at all.
std::string capture_census(store::KVAddressSpaceStore& addresses, const char* label) {
    const int saved = dup(STDERR_FILENO);
    FILE* sink = std::tmpfile();
    if (saved < 0 || sink == nullptr) {
        if (sink != nullptr) { std::fclose(sink); }
        if (saved >= 0) { close(saved); }
        return {};
    }
    (void)std::fflush(stderr);
    dup2(fileno(sink), STDERR_FILENO);
    addresses.census(label);
    (void)std::fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    std::rewind(sink);
    std::string out;
    char buffer[512];
    while (std::fgets(buffer, sizeof buffer, sink) != nullptr) { out += buffer; }
    std::fclose(sink);
    return out;
}

void expect(bool condition, std::string_view message) {
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

bool cuda_unavailable(cudaError_t error) {
    return error == cudaErrorNoDevice || error == cudaErrorInsufficientDriver;
}

std::vector<std::int32_t> read_block_table(const ninfer::KVExecutionTablePool& tables,
                                           std::int32_t row, std::size_t count) {
    const ninfer::Tensor source = tables.matrix().slice(1, row, 1).view(
        {static_cast<std::int32_t>(tables.logical_page_capacity())});
    std::vector<std::int32_t> values(count);
    CUDA_CHECK(cudaMemcpy(values.data(), source.data, values.size() * sizeof(std::int32_t),
                          cudaMemcpyDeviceToHost));
    return values;
}

void test_state_store(ninfer::DeviceContext& device) {
    q36::StateImageSpec spec{
        .linear =
            {
                .layers         = 1,
                .conv_channels  = 8,
                .conv_width     = 3,
                .value_heads    = 2,
                .value_head_dim = 4,
                .key_head_dim   = 4,
                .slot_count     = 4,
                .conv_dtype     = ninfer::DType::BF16,
            },
        .hidden = 8,
        .dflash_local =
            q36::DFlashLocalStateSpec{.layers = 1, .capacity = 8, .kv_heads = 2, .head_dim = 4},
    };
    ninfer::LayoutBuilder builder;
    const q36::StateImageDeviceLayout layout = q36::plan_state_image_device_pool(builder, spec);
    ninfer::DeviceArena arena(builder.finish(256));
    q36::StateImageDevicePool physical({arena.base(), arena.capacity()}, layout);
    // The host slots come from the SHARED pinned budget now; the two slots this test needs are an initial
    // reservation. A CPU-only chunk source keeps this test's pool bookkeeping independent of whether the
    // machine has a GPU -- the test itself still needs one for the device pool.
    ninfer::PinnedHostPool host_pool(
        ninfer::PinnedHostPool::Config{/*chunk_bytes=*/layout.host.image_bytes * 2U, /*alignment=*/256U},
        [](std::size_t bytes) { return std::malloc(bytes); },
        [](void* base) { std::free(base); });
    q36::HostStatePool host(layout.host, host_pool);
    expect(host.reserve_slots(2) == 2U, "host slots reserved from the shared pool");
    store::StateImageStore images(
        physical, &host, static_cast<std::uint32_t>(physical.slot_count()) + host.capacity());

    const auto source = images.reserve_reset(device.stream);
    expect(source.has_value(), "state source allocation");
    const std::int32_t original_slot = images.physical_slot(*source);
    images.freeze(*source);
    images.move_checkpoint_to_active(*source);
    expect(images.physical_slot(*source) == original_slot &&
               images.role(*source) == store::StateImageRole::ActiveMutable,
           "private Move preserves the logical image and physical slot");
    images.freeze(*source);
    const auto destination = images.reserve_destination();
    expect(destination.has_value(), "state destination reservation");
    const store::StateImageSelectors selectors = images.begin_fork(*source, *destination);
    expect(selectors.source >= 0 && selectors.destination >= 0 &&
               selectors.source != selectors.destination,
           "fork resolves distinct physical selectors");
    expect(images.can_release_source_after_fork_abort(*source, *destination, 0) &&
               images.can_release_destination_after_fork_abort(*source, *destination, 0),
           "fork release preflight accounts for its source and destination pins");
    const auto concurrent_destination = images.reserve_destination();
    expect(concurrent_destination.has_value(), "concurrent fork destination reservation");
    (void)images.begin_fork(*source, *concurrent_destination);
    expect(!images.can_release_source_after_fork_abort(*source, *destination, 0),
           "fork release preflight preserves independent source pins");
    images.abort_fork(*source, *concurrent_destination);
    expect(images.release(*concurrent_destination), "concurrent fork destination release");
    expect(images.can_release_source_after_fork_abort(*source, *destination, 0),
           "fork release preflight accepts the final source pin");
    expect(!images.release(*source) && !images.release(*destination),
           "fork pins both logical images");
    images.commit_fork(*source, *destination);
    expect(images.role(*source) == store::StateImageRole::CheckpointImmutable &&
               images.role(*destination) == store::StateImageRole::ActiveMutable,
           "fork commit preserves source and activates destination");
    images.retain_checkpoint_reference(*source);
    const std::uint64_t rewrite_epoch = images.content_epoch(*source);
    images.freeze(*destination);
    const std::uint64_t recycled_epoch = images.recycle_checkpoint_destination(*source);
    const auto rotated_selectors       = images.begin_fork(*destination, *source);
    expect(rotated_selectors.source != rotated_selectors.destination && images.occupied() == 2,
           "rewrite rotation remains within two StateImages");
    images.abort_fork(*destination, *source);
    images.restore_recycled_checkpoint(*source, recycled_epoch);
    images.thaw(*destination);
    expect(images.content_epoch(*source) == rewrite_epoch &&
               images.checkpoint_references(*source) == 1,
           "aborted rewrite rotation restores the prior checkpoint identity");

    // #11(b): `complete()` is the StateImage half of invariant #6 -- the KV side walks every required
    // page and throws, and this expresses "there is a settled replica to restore from". Its first version
    // returned false for any image with a pending replica, which includes an IN-FLIGHT Host-to-Device
    // restore (`begin_host_to_device` sets `pending_device_slot`, `transfer_id` and `destination_pinned`
    // while the host replica is settled) -- a false positive that would have failed pricing for a whole
    // owner on healthy traffic, and the reason the condition now tests the settled slots instead. This
    // case pins that: an image mid-restore IS complete, because its host replica is there.
    const auto demoted = images.reserve_reset(device.stream);
    expect(demoted.has_value(), "completeness case could reserve an image");
    if (demoted.has_value()) {
        images.freeze(*demoted);
        expect(images.role(*demoted) == store::StateImageRole::CheckpointImmutable,
               "completeness case froze its image into a checkpoint");
        auto to_host = images.reserve_device_to_host(*demoted);
        expect(to_host.has_value(), "completeness case could start a Device-to-Host demotion");
        if (to_host.has_value()) {
            images.enqueue_device_to_host(*to_host, device.stream);
            images.publish_transfer(std::move(*to_host), false);
            expect(images.complete(*demoted),
                   "a checkpoint with a settled Host replica is complete");
            auto to_device = images.begin_host_to_device(*demoted, device.stream);
            expect(to_device.has_value(),
                   "completeness case could start a Host-to-Device restore");
            if (to_device.has_value()) {
                expect(images.complete(*demoted),
                       "a checkpoint mid-RESTORE is complete: its Host replica is settled, so an "
                       "in-flight Device replica must not be read as incompleteness");
                images.abort_transfer(std::move(*to_device));
                expect(images.complete(*demoted),
                       "aborting the restore leaves the checkpoint complete on its Host replica");
            }
        }
        // Release what this case created, or the store's ownership-closure check at the end of this
        // function fails -- which is exactly what the first version of this case did: two teardown
        // failures ("rotated state image ownership closes after release" among them) that had nothing to
        // do with `complete()` and everything to do with a leaked image.
        expect(images.release(*demoted), "completeness case released the image it created");
    }
    expect(!images.complete(store::StateImageHandle{}),
           "an invalid handle is not a complete checkpoint");

    images.freeze(*destination);
    (void)images.recycle_checkpoint_destination(*source);
    (void)images.begin_fork(*destination, *source);
    images.commit_fork(*destination, *source);
    images.retain_checkpoint_reference(*destination);
    images.release_checkpoint_reference(*destination);
    expect(images.release(*destination) && images.release(*source) && images.occupied() == 0,
           "rotated state image ownership closes after release");
    expect(!images.valid(*source), "released state generation becomes stale");

    const auto reused = images.reserve_reset(device.stream);
    expect(reused.has_value() && *reused != *source, "state descriptor reuse advances generation");
    expect(images.release(*reused), "reused state image releases");

    const auto host_source = images.reserve_reset(device.stream);
    expect(host_source.has_value(), "Host state source allocation");
    device.synchronize();
    images.freeze(*host_source);
    auto d2h = images.begin_device_to_host(*host_source, device.transfer_stream);
    expect(d2h.has_value() &&
               images.residency(*host_source) == store::StateReplicaResidency::DeviceOnly,
           "incomplete State D2H does not publish a Host replica");
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    images.publish_transfer(std::move(*d2h), true);
    expect(images.residency(*host_source) == store::StateReplicaResidency::Both,
           "State D2H backup publishes stable Both residency");

    const auto moved_device = images.reserve_logical_destination();
    expect(moved_device.has_value(), "State replica split destination allocation");
    images.split_device_replica_identity(*host_source, *moved_device);
    expect(images.residency(*host_source) == store::StateReplicaResidency::HostOnly &&
               images.residency(*moved_device) == store::StateReplicaResidency::DeviceOnly,
           "State replica identity split keeps old Host content and moves Device ownership");

    const auto fork_one = images.reserve_logical_destination();
    const auto fork_two = images.reserve_logical_destination();
    expect(fork_one && fork_two, "concurrent Host State forks reserve logical destinations");
    auto h2d_one = images.begin_host_fork(*host_source, *fork_one, device.transfer_stream);
    auto h2d_two = images.begin_host_fork(*host_source, *fork_two, device.transfer_stream);
    expect(h2d_one && h2d_two && images.source_pins(*host_source) == 2,
           "immutable Host State source supports multiple fork pins");
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    images.publish_transfer(std::move(*h2d_one), true);
    images.publish_transfer(std::move(*h2d_two), true);
    expect(images.source_pins(*host_source) == 0 &&
               images.residency(*fork_one) == store::StateReplicaResidency::DeviceOnly &&
               images.residency(*fork_two) == store::StateReplicaResidency::DeviceOnly,
           "Host State forks publish independent Device destinations");
    expect(images.release(*host_source) && images.release(*moved_device) &&
               images.release(*fork_one) && images.release(*fork_two) && host.occupied() == 0,
           "State Host/Device replica ownership closes without leaked slots");
}

// THE SESSION-START PRE-GROW for the host-KV axis, which had no equivalent while the STATE axis has had
// one since §3 item 6 (measured firing, `17 -> 18` slots). The state pre-grow could not convert the one
// eviction that workload produced, because that one was bound by host KV BYTES -- `host_kv` at 99.3% --
// so this is the missing half of the same mechanism.
//
// It asserts the CAPACITY DIFFERENCE, which is the lesson the state axis's equivalent records: `pre_grow`
// returning `Grew` while pinning nothing would satisfy a flag assertion and leave the demote unaffordable.
//
// The TRIGGER is the part worth pinning: `NotFull` only while the arena can place a whole STEP as one
// contiguous run, and `Grew` the moment it cannot -- INCLUDING when free bytes remain. That second case is
// what a "no free bytes at all" trigger would decline, and it is the measured case (99.3% leaves ~224 MiB,
// which is not zero).
void test_host_kv_pregrow(ninfer::DeviceContext& device) {
    // NO DEVICE POOL HERE, deliberately: the pre-grow is decided from the HOST arena's extent map and the
    // pool's growth policy alone, so a host layout is all this needs. (An earlier draft of this test carried
    // the neighbouring fixture's device scaffolding and would not compile -- it was never used.)
    (void)device;
    const std::array geom{ninfer::KVPageGeometry{
        .device_plane_order = ninfer::PagedKVPlaneOrder::PageMajor,
        .planes             = {{.dtype = ninfer::DType::I8, .leading_extent = 8, .head_extent = 2},
                               {.dtype = ninfer::DType::FP16, .leading_extent = 1, .head_extent = 2}},
    }};
    const ninfer::HostKVPageLayout host_layout = ninfer::plan_host_kv_page_layout(geom.front());
    const std::array host_layouts{host_layout};
    const std::size_t step = host_layout.page_stride * 8U;

    ninfer::PinnedHostPool pool(
        ninfer::PinnedHostPool::Config{step, 256U},
        [](std::size_t bytes) { return std::malloc(bytes); },
        [](void* base) { std::free(base); });
    ninfer::HostKVArena arena(pool, step, step, host_layouts);
    store::HostKVExtentStore extents(arena, 64);

    expect(store::HostKVExtentStore::pre_grow(arena, extents) ==
               store::HostKVExtentStore::HostKVPreGrow::NotFull,
           "pre-grow: a fresh arena reports NotFull");
    expect(arena.capacity_bytes() == step, "pre-grow: NotFull pinned nothing");

    auto taken = arena.allocate(host_layout, 1);
    expect(taken.has_value(), "pre-grow: fixture allocation");
    expect(arena.free_bytes() > 0U, "pre-grow: the fixture still holds free BYTES");
    expect(arena.largest_free_run_bytes() < step,
           "pre-grow: and no longer a whole-step run -- the trigger condition");
    const std::size_t capacity_before = arena.capacity_bytes();
    const std::size_t free_before     = arena.free_bytes();

    expect(store::HostKVExtentStore::pre_grow(arena, extents) ==
               store::HostKVExtentStore::HostKVPreGrow::Grew,
           "pre-grow: a fragmented arena grows rather than declining");
    expect(arena.capacity_bytes() == capacity_before + step,
           "pre-grow: and it added EXACTLY one step -- the difference, not a flag");
    expect(arena.free_bytes() == free_before + step, "pre-grow: the new step is free, not occupied");

    // REFUSED, on a FRESH arena. Reusing the one above does not work and the first version of this test
    // tried to: after a successful pre-grow the arena holds a whole new step, so it is `NotFull` again and
    // the refusal is never reached -- a fixture that cannot fail for the reason it claims.
    ninfer::PinnedHostPool refusing_pool(
        ninfer::PinnedHostPool::Config{step, 256U},
        [](std::size_t bytes) { return std::malloc(bytes); },
        [](void* base) { std::free(base); });
    ninfer::HostKVArena refusing_arena(refusing_pool, step, step, host_layouts);
    store::HostKVExtentStore refusing_extents(refusing_arena, 64);
    auto held = refusing_arena.allocate(host_layout, 1);
    expect(held.has_value(), "pre-grow: refusal-arm fixture allocation");
    expect(refusing_arena.largest_free_run_bytes() < step, "pre-grow: the refusal arm is fragmented");
    const std::size_t before_refusal = refusing_arena.capacity_bytes();
    refusing_pool.set_growth_policy([](std::size_t) { return false; });
    expect(store::HostKVExtentStore::pre_grow(refusing_arena, refusing_extents) ==
               store::HostKVExtentStore::HostKVPreGrow::Refused,
           "pre-grow: a refusing policy reports Refused, not silence");
    expect(refusing_arena.capacity_bytes() == before_refusal,
           "pre-grow: a refused pre-grow left the arena exactly as it was");
}

void test_kv_store(ninfer::DeviceContext& device) {
    ninfer::LayoutBuilder builder;
    ninfer::DeviceKVPagePoolSpec page_spec{
        .page_group_count = 8,
        .geometry =
            {
                .page_tokens        = static_cast<std::uint32_t>(ninfer::kPagedKVPageSize),
                .device_plane_order = ninfer::PagedKVPlaneOrder::PageMajor,
                .planes = {{.dtype = ninfer::DType::BF16, .leading_extent = 8, .head_extent = 2}},
            },
    };
    const ninfer::DeviceKVPagePoolLayout page_layout =
        ninfer::plan_device_kv_page_pool(builder, page_spec);
    const ninfer::KVExecutionTableLayout table_layout =
        ninfer::plan_kv_execution_tables(builder, {.logical_page_capacity = 4, .table_rows = 2});
    ninfer::DeviceArena arena(builder.finish(256));
    const ninfer::DeviceSpan backing{arena.base(), arena.capacity()};
    ninfer::DeviceKVPagePool physical_pages(backing, page_layout);
    ninfer::KVExecutionTablePool physical_tables(backing, table_layout, physical_pages);
    const ninfer::HostKVPageLayout host_layout =
        ninfer::plan_host_kv_page_layout(physical_pages.geometry());
    const std::array host_layouts{host_layout};
    ninfer::PinnedHostPool kv_pool(  // CPU-only source: this suite checks allocation, not pinning
        ninfer::PinnedHostPool::Config{host_layout.page_stride * 8U, 256U},
        [](std::size_t bytes) { return std::malloc(bytes); },
        [](void* base) { std::free(base); });
    ninfer::HostKVArena host_arena(kv_pool, host_layout.page_stride * 8, host_layout.page_stride * 8,
                                   host_layouts);
    store::LogicalKVPageStore pages(physical_pages, physical_pages.capacity_pages() + 8U);
    store::HostKVExtentStore extents(host_arena, 8);
    store::KVAddressSpaceStore addresses(pages, physical_tables, 4, 4);

    const auto address = addresses.create_active(3, 0);
    expect(address.has_value(), "active KV address allocation");
    addresses.ensure_mapped_to_tokens(*address, 65, device.stream);
    device.synchronize();
    expect(addresses.mapped_pages(*address) == 2 && addresses.committed_frontier(*address) == 0,
           "physical KV append does not publish canonical coverage before commit");
    expect(read_block_table(physical_tables, 0, 2) == std::vector<std::int32_t>({0, 1}),
           "batched KV materialization publishes the exact execution mapping");
    addresses.commit_frontier(*address, 65);
    expect(addresses.mapped_pages(*address) == 2 && addresses.entitlement(*address) == 3 &&
               addresses.committed_frontier(*address) == 65 && addresses.bound_row(*address) == 0,
           "KV address tracks mapped pages, entitlement, frontier, and execution row");
    expect(pages.active_address_references(addresses.logical_page(*address, 0)) == 1 &&
               pages.active_address_references(addresses.logical_page(*address, 1)) == 1,
           "active KV membership is counted on each logical page");
    addresses.ensure_mapped_to_tokens(*address, 129, device.stream);
    device.synchronize();
    expect(read_block_table(physical_tables, 0, 3) == std::vector<std::int32_t>({0, 1, 2}),
           "incremental KV materialization preserves the existing mapping prefix");
    addresses.destructive_truncate(*address, 65);
    expect(addresses.mapped_pages(*address) == 2 && addresses.entitlement(*address) == 3 &&
               addresses.committed_frontier(*address) == 65,
           "same-frontier trim releases uncommitted materialized suffix pages");
    addresses.set_checkpoint_requirement(*address, 65);
    expect(pages.occupied() == 2 && addresses.mapped_pages(*address) == 2,
           "endpoint and rewrite requirements share one ordered page mapping");
    const std::uint64_t old_epoch = addresses.content_epoch(*address, 0);

    addresses.deactivate(*address);
    expect(addresses.bound_row(*address) == -1 && addresses.entitlement(*address) == 2,
           "catalogued KV address owns no execution row or growth reservation");

    const std::array logical_pages{addresses.logical_page(*address, 0),
                                   addresses.logical_page(*address, 1)};
    expect(pages.active_address_references(logical_pages[0]) == 0 &&
               pages.active_address_references(logical_pages[1]) == 0 &&
               !addresses.has_active_reference(logical_pages[0]),
           "KV deactivation clears logical-page active references");
    addresses.set_checkpoint_requirement(*address, 32);
    expect(pages.protected_columns(logical_pages[0]) == 32 &&
               pages.protected_columns(logical_pages[1]) == 0,
           "lowered checkpoint frontier releases stale suffix protection");
    addresses.set_checkpoint_requirement(*address, 65);
    auto host_backup = extents.prepare(pages, logical_pages);
    expect(host_backup.has_value(), "Host KV extent reservation");
    const std::vector<ninfer::DeviceKVPageHandle> sources = extents.device_sources(*host_backup);
    physical_pages.copy_to_host(sources, extents.writable_view(*host_backup),
                                device.transfer_stream);
    expect(!pages.host_resident(logical_pages[0]) && !pages.host_resident(logical_pages[1]),
           "incomplete KV D2H does not publish Host replicas");
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    auto host_extent = extents.publish(std::move(*host_backup));
    expect(pages.host_resident(logical_pages[0]) && pages.host_resident(logical_pages[1]),
           "KV extent publication attaches every logical Host replica");
    const std::array first_host_release{logical_pages[0]};
    expect(extents.release_page_replicas(pages, first_host_release),
           "first Host page release transaction");
    const auto retained_host_extent = pages.host_replica(logical_pages[1]).extent;
    expect(!extents.valid(host_extent) && !pages.host_resident(logical_pages[0]) &&
               extents.valid(retained_host_extent) &&
               pages.host_replica(logical_pages[1]).page_offset == 0,
           "partial Host release partitions an extent and republishes the retained run");
    const std::array second_host_release{logical_pages[1]};
    expect(extents.release_page_replicas(pages, second_host_release),
           "second Host page release transaction");
    expect(!pages.host_resident(logical_pages[1]) && host_arena.occupied_bytes() == 0,
           "partitioned Host replicas release while Device replicas survive");

    auto second_host_backup = extents.prepare(pages, logical_pages);
    expect(second_host_backup.has_value(), "second Host KV extent reservation");
    const std::vector<ninfer::DeviceKVPageHandle> second_sources =
        extents.device_sources(*second_host_backup);
    physical_pages.copy_to_host(second_sources, extents.writable_view(*second_host_backup),
                                device.transfer_stream);
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    const auto second_host_extent = extents.publish(std::move(*second_host_backup));
    expect(pages.drop_device_replica(logical_pages[0]) &&
               pages.drop_device_replica(logical_pages[1]) && !extents.release(second_host_extent),
           "Host-only KV pages remain valid and cannot lose their last replica");
    const std::array last_reference_release{store::HostKVPageReplicaRelease{
        .pages = &pages,
        .page  = logical_pages[1],
    }};
    const std::array seven_page_request{
        ninfer::HostKVAllocationRequest{.layout = &host_layout, .pages = 7}};
    const std::array eight_page_request{
        ninfer::HostKVAllocationRequest{.layout = &host_layout, .pages = 8}};
    expect(
        extents.can_allocate_after_page_releases({}, last_reference_release, seven_page_request) &&
            !extents.can_allocate_after_page_releases({}, last_reference_release,
                                                      eight_page_request),
        "last address-reference release contributes exact Host allocation credit");

    // ***THE STORE-SIDE REFUSAL SPLIT (2026-10-05), AND THE CONTROL IS THE POINT.*** These returns happen
    // BEFORE the arena is asked, so an ARENA-side split counts nothing for them -- measured on QA: the veto
    // fired 18 times and EVERY arena-side reason stayed 0, which is how the instrument was found to be in the
    // wrong layer.
    //
    // **THIS CONTROL WAS WRONG TWICE BEFORE IT WAS RIGHT, and both errors were mine, not the engine's:**
    //   1. I put this page in the PLAIN-release slot. It is HOST-ONLY and still address-referenced, so
    //      `can_release_page_replica` refuses it CORRECTLY -- releasing it would destroy the last copy. The
    //      right slot is the LAST-REFERENCE list, which is what the planner uses for such a page.
    //   2. I then assumed the refusal was `already_marked` (the stamp), then that it was an engine bug. The
    //      probe said `store_mark_cannot_release_replica` -- a THIRD cause, and the correct one.
    // So the control below asserts the TRUE two-directional behaviour, and it is what pins the split.
    {
        using Reason = ninfer::HostKVBlockReason;
        const std::uint64_t cannot_before =
            extents.block_reason_count(Reason::StoreMarkCannotReleaseReplica);
        const std::array plain_release{store::HostKVPageReplicaRelease{
            .pages = &pages,
            .page  = logical_pages[1],
        }};

        // WRONG SLOT: a host-only, still-referenced page as a PLAIN release is refused -- correctly.
        const auto wrong_slot =
            extents.fit_after_page_releases(plain_release, {}, seven_page_request, 1U << 30U);
        expect(wrong_slot.fit == ninfer::HostKVFit::Blocked,
               "a host-only referenced page as a PLAIN release is Blocked");
        expect(extents.block_reason_count(Reason::StoreMarkCannotReleaseReplica) == cannot_before + 1U,
               "and store_mark_cannot_release_replica names it -- the predicate, not a catch-all");

        // RIGHT SLOT: the same page in the LAST-REFERENCE list is accepted, so the split is not simply
        // counting every call.
        const std::uint64_t cannot_after = extents.block_reason_count(Reason::StoreMarkCannotReleaseReplica);
        (void)extents.fit_after_page_releases({}, plain_release, seven_page_request, 1U << 30U);
        expect(extents.block_reason_count(Reason::StoreMarkCannotReleaseReplica) == cannot_after,
               "the SAME page in the LAST-REFERENCE slot adds no refusal -- the split distinguishes them");
    }

    // ***THE CLASSIFIER TABLE (2026-10-05), and this is the unit test for the FIX ITSELF.*** The planner
    // pushed EVERY released host page into the last-reference list; the store refuses any shared page there
    // (correctly), and that refusal made every preserving plan read `Blocked` -- 13 of 13 veto refusals
    // measured as `store_mark_not_sole_reference`. The classification now lives in ONE function so this
    // table and the planner share the specification. THE FOURTH ROW IS THE BUG CASE: a host-only page with
    // more than one reference must NOT be offered at all.
    {
        using ninfer::HostReleaseKind;
        const auto kind = [](bool host, bool device, std::uint32_t refs) {
            return ninfer::classify_host_release(host, device, refs);
        };
        expect(kind(true, true, 1U) == HostReleaseKind::Plain,
               "classifier: a DEVICE-RESIDENT page is a PLAIN release even at one reference (device wins)");
        expect(kind(true, true, 3U) == HostReleaseKind::Plain,
               "classifier: a device-resident shared page is still a PLAIN release");
        expect(kind(true, false, 1U) == HostReleaseKind::LastReference,
               "classifier: a host-only page at ONE reference is a LAST-REFERENCE release");
        expect(kind(true, false, 3U) == HostReleaseKind::NotReleased,
               "classifier: a HOST-ONLY SHARED page is NOT offered -- THE BUG CASE, refused by the store");
        expect(kind(true, false, 0U) == HostReleaseKind::NotReleased,
               "classifier: a host-only page with no references is not a last reference either");
        expect(kind(false, true, 1U) == HostReleaseKind::NotReleased,
               "classifier: a page with no host replica is never a host release");
        expect(kind(false, false, 0U) == HostReleaseKind::NotReleased,
               "classifier: and neither is one with nothing resident");
    }
    auto restore_reservation = physical_pages.reserve(2);
    expect(restore_reservation.has_value(), "KV Host restore Device reservation");
    const std::array restore_destinations{
        pages.reserve_device_replica(logical_pages[0], *restore_reservation),
        pages.reserve_device_replica(logical_pages[1], *restore_reservation)};
    physical_pages.copy_from_host(extents.view(second_host_extent), restore_destinations,
                                  device.transfer_stream);
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    pages.publish_device_replica(logical_pages[0]);
    pages.publish_device_replica(logical_pages[1]);
    expect(extents.release(second_host_extent) && host_arena.occupied_bytes() == 0,
           "KV Host restore republishes Device replicas before releasing the extent");
    auto activation = addresses.prepare_activation(*address, 3, 1);
    expect(addresses.bound_row(*address) == -1 && addresses.entitlement(*address) == 2 &&
               physical_pages.reserved_pages() == 1,
           "prepared KV activation preserves the catalogued mapping until publication");
    addresses.commit_activation(std::move(activation), device.stream);
    expect(pages.active_address_references(logical_pages[0]) == 1 &&
               pages.active_address_references(logical_pages[1]) == 1,
           "KV reactivation republishes logical-page active references");
    addresses.destructive_truncate(*address, 32);
    expect(addresses.mapped_pages(*address) == 1 && addresses.entitlement(*address) == 3 &&
               addresses.committed_frontier(*address) == 32 &&
               addresses.content_epoch(*address, 0) != old_epoch,
           "destructive rewrite truncates coverage and advances content epoch");
    addresses.deactivate(*address);
    const std::array final_logical_page{addresses.logical_page(*address, 0)};
    auto final_host_backup = extents.prepare(pages, final_logical_page);
    expect(final_host_backup.has_value(), "final Host KV backup reservation");
    const auto final_sources = extents.device_sources(*final_host_backup);
    physical_pages.copy_to_host(final_sources, extents.writable_view(*final_host_backup),
                                device.transfer_stream);
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    (void)extents.publish(std::move(*final_host_backup));
    // THE NON-ZERO CASE -- THE ONE THAT MAKES EVERY OTHER CENSUS ASSERTION MEAN SOMETHING. All the others
    // expect zero, and a counter hard-coded to zero satisfies every one of them. The state it needs is "no
    // address reference, but a surviving source pin" -- which is #9's second candidate exactly: a pin that
    // outlived its owner, holding host KV nothing else refers to.
    //
    // PIN **AFTER** THE RELEASE, and the first version of this case did it the other way round: `addresses.
    // release` REFUSES while a source pin is held (the pin is precisely what stops the release), so pinning
    // first made the RELEASE fail and cascaded eleven downstream cases with it. The handle is captured before
    // the release only because `logical_page` needs a valid address; the page itself outlives the address.
    const auto pinned_page = addresses.logical_page(*address, 0);
    expect(addresses.release(*address), "catalogued KV address releases");
    pages.pin_source(pinned_page);
    const auto with_pin = extents.release_unreferenced();
    expect(with_pin.released_bytes == 0 && with_pin.retained_pinned == 1 &&
               with_pin.retained_referenced == 0,
           "a source pin retains its Host extent, and the census counts it as PINNED not as referenced");

    // ...and dropping the pin must release the SAME extent, so the pair distinguishes the two states instead
    // of asserting one of them. BOTH HALVES OF THE CENSUS, where the assertion used to be only the first:
    // `released_bytes` says what went, the retained counts say what STAYED, and the latter were not observable
    // at all until this change -- a page kept by a live address, or by a pin that outlived its owner, stranded
    // host KV silently (#9).
    pages.unpin_source(pinned_page);
    const auto teardown = extents.release_unreferenced();
    expect(teardown.released_bytes == host_layout.page_stride &&
               teardown.retained_referenced == 0 && teardown.retained_pinned == 0 &&
               host_arena.occupied_bytes() == 0,
           "address teardown reclaims its zero-reference Host extent and retains nothing");
    expect(!addresses.valid(*address) && pages.occupied() == 0 &&
               physical_pages.allocated_pages() == 0 && physical_pages.reserved_pages() == 0,
           "KV release invalidates generations and closes physical ownership");

    // THE POSITIVE BRANCH -- the reading that MATTERS, and the one no production path can produce.
    //
    // At the census's real call site `fail_all_cleanup` has already destroyed the transaction and with it every
    // non-address holder, so a positive reading there IS the leak (#9), not an expectation. That state cannot
    // be staged in production, so it is staged HERE, where the pool is EMPTY: hold a reservation in a LOCAL,
    // which no address owns, and require the census to call it what it is. Without this, the branch is only
    // asserted to be unreachable -- a claim that reads as verified while nothing checks it.
    {
        auto held = physical_pages.reserve(2);
        expect(held.has_value(), "could not reserve the device pages the positive census case needs");
        const std::string with_orphan = capture_census(addresses, "text");
        const std::string::size_type at = with_orphan.find("TOTAL");
        const std::string said =
            at == std::string::npos ? std::string("<no TOTAL line>") : with_orphan.substr(at);
        expect(with_orphan.find("(POSITIVE:") != std::string::npos,
               std::string("the census does not call reserved pages with no owner a FINDING -- the one reading "
                           "it exists for; it said: ") + said);
        expect(with_orphan.find("(exact:") == std::string::npos,
               std::string("a reservation no address owns was reported as accounted; it said: ") + said);
        // `held` dies at the end of this block, so the next read must return to exact -- the pair distinguishes
        // the two states instead of asserting one of them.
    }
    {
        const std::string after = capture_census(addresses, "text");
        expect(after.find("(exact:") != std::string::npos,
               "the census does not return to 'exact' once the unowned reservation is released");
    }

    // Verify crosses a page boundary, but the terminal commit consumes only its first column.
    const auto terminal = addresses.create_active(3, 0);
    expect(terminal.has_value(), "terminal boundary KV address allocation");
    addresses.ensure_mapped_to_tokens(*terminal, 63, device.stream);
    addresses.commit_frontier(*terminal, 63);
    addresses.ensure_mapped_to_tokens(*terminal, 71, device.stream);
    device.synchronize();
    const auto terminal_mapping   = read_block_table(physical_tables, 0, 2);
    const auto unchanged_terminal = [&] {
        return addresses.mapped_pages(*terminal) == 2 &&
               addresses.committed_frontier(*terminal) == 63 &&
               addresses.entitlement(*terminal) == 3 && physical_pages.allocated_pages() == 2 &&
               physical_pages.reserved_pages() == 1 && physical_pages.available_pages() == 5 &&
               read_block_table(physical_tables, 0, 2) == terminal_mapping;
    };
    addresses.ensure_mapped_to_tokens(*terminal, 71, device.stream);
    addresses.ensure_mapped_to_tokens(*terminal, 64, device.stream);
    device.synchronize();
    expect(unchanged_terminal(), "covered KV requests preserve speculative mappings and ownership");
    bool exceeded = false;
    try {
        addresses.ensure_mapped_to_tokens(*terminal, 193, device.stream);
    } catch (const std::invalid_argument&) { exceeded = true; }
    expect(exceeded && unchanged_terminal(),
           "KV coverage beyond entitlement fails without mutation");
    addresses.commit_frontier(*terminal, 64);
    addresses.destructive_truncate(*terminal, 64);
    expect(addresses.mapped_pages(*terminal) == 1 &&
               addresses.committed_frontier(*terminal) == 64 &&
               addresses.entitlement(*terminal) == 3 && physical_pages.allocated_pages() == 1 &&
               physical_pages.reserved_pages() == 2 && physical_pages.available_pages() == 5,
           "terminal trim returns the uncommitted page to the same active reservation");
    addresses.deactivate(*terminal);
    expect(addresses.release(*terminal) && physical_pages.allocated_pages() == 0 &&
               physical_pages.reserved_pages() == 0 && physical_pages.available_pages() == 8,
           "terminal settlement releases both mappings and unused growth");

    const auto snapshot_source      = addresses.create_active(3, 0);
    const auto snapshot_destination = addresses.create_inactive();
    expect(snapshot_source && snapshot_destination, "active KV snapshot endpoints allocate");
    addresses.ensure_mapped_to_tokens(*snapshot_source, 65, device.stream);
    addresses.commit_frontier(*snapshot_source, 65);
    const auto snapshot_full = addresses.logical_page(*snapshot_source, 0);
    const auto snapshot_tail = addresses.logical_page(*snapshot_source, 1);
    auto snapshot = addresses.prepare_active_snapshot(*snapshot_source, *snapshot_destination, 65);
    physical_pages.copy_page(addresses.active_snapshot_tail_source(snapshot),
                             addresses.active_snapshot_tail_destination(snapshot),
                             device.transfer_stream);
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    addresses.commit_active_snapshot(std::move(snapshot), device.stream);
    const auto snapshot_destination_tail = addresses.logical_page(*snapshot_destination, 1);
    expect(addresses.active(*snapshot_destination) && !addresses.active(*snapshot_source) &&
               pages.active_address_references(snapshot_full) == 1 &&
               pages.active_address_references(snapshot_tail) == 0 &&
               pages.active_address_references(snapshot_destination_tail) == 1,
           "active KV snapshot transfers active ownership without double-counting shared pages");
    addresses.deactivate(*snapshot_destination);
    expect(pages.active_address_references(snapshot_full) == 0 &&
               pages.active_address_references(snapshot_destination_tail) == 0 &&
               addresses.release(*snapshot_destination) && addresses.release(*snapshot_source) &&
               pages.occupied() == 0,
           "active KV snapshot references close with both address spaces");

    const auto alternating = addresses.create_active(4, 0);
    expect(alternating.has_value(), "alternating Host release address allocation");
    addresses.ensure_mapped_to_tokens(*alternating, 193, device.stream);
    addresses.commit_frontier(*alternating, 193);
    addresses.deactivate(*alternating);
    const std::array alternating_pages{
        addresses.logical_page(*alternating, 0), addresses.logical_page(*alternating, 1),
        addresses.logical_page(*alternating, 2), addresses.logical_page(*alternating, 3)};
    auto alternating_backup = extents.prepare(pages, alternating_pages);
    expect(alternating_backup.has_value(), "alternating Host extent reservation");
    const auto alternating_sources = extents.device_sources(*alternating_backup);
    physical_pages.copy_to_host(alternating_sources, extents.writable_view(*alternating_backup),
                                device.transfer_stream);
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    const auto alternating_extent = extents.publish(std::move(*alternating_backup));
    const std::array alternating_release{alternating_pages[0], alternating_pages[2]};
    expect(extents.release_page_replicas(pages, alternating_release),
           "alternating Host page release transaction");
    expect(!extents.valid(alternating_extent) && !pages.host_resident(alternating_pages[0]) &&
               pages.host_resident(alternating_pages[1]) &&
               !pages.host_resident(alternating_pages[2]) &&
               pages.host_resident(alternating_pages[3]) &&
               pages.host_replica(alternating_pages[1]).extent !=
                   pages.host_replica(alternating_pages[3]).extent &&
               host_arena.occupied_bytes() == 2U * host_layout.page_stride,
           "one batch partitions alternating Host release and retained runs exactly once");
    const std::array alternating_retained{alternating_pages[1], alternating_pages[3]};
    expect(extents.release_page_replicas(pages, alternating_retained),
           "retained Host run release transaction");
    expect(host_arena.occupied_bytes() == 0 && addresses.release(*alternating) &&
               pages.occupied() == 0,
           "alternating Host extent partitions close without leaked descriptors");

    const auto shared = addresses.create_active(3, 0);
    expect(shared.has_value(), "shared-prefix source address allocation");
    addresses.ensure_mapped_to_tokens(*shared, 65, device.stream);
    addresses.commit_frontier(*shared, 65);
    addresses.set_checkpoint_requirement(*shared, 65);
    addresses.deactivate(*shared);
    const auto shared_full = addresses.logical_page(*shared, 0);
    const auto shared_tail = addresses.logical_page(*shared, 1);

    const auto branch_one = addresses.create_inactive();
    expect(branch_one.has_value(), "first shared-prefix branch address allocation");
    auto first_fork = addresses.prepare_prefix_fork(*shared, *branch_one, 65, 3, 1);
    expect(first_fork.needs_tail_copy() && pages.address_references(shared_full) == 1,
           "prefix fork remains unpublished while its tail copy is pending");
    physical_pages.copy_page(addresses.prefix_fork_tail_source(first_fork),
                             addresses.prefix_fork_tail_destination(first_fork),
                             device.transfer_stream);
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    addresses.commit_prefix_fork(std::move(first_fork), device.stream);
    const auto branch_one_tail = addresses.logical_page(*branch_one, 1);
    expect(addresses.logical_page(*branch_one, 0) == shared_full &&
               branch_one_tail != shared_tail && pages.address_references(shared_full) == 2 &&
               pages.address_references(shared_tail) == 1,
           "shared branch references full pages and publishes a private partial tail");
    addresses.ensure_mapped_to_tokens(*branch_one, 66, device.stream);
    addresses.commit_frontier(*branch_one, 66);
    expect(pages.committed_columns(shared_tail) == 1 &&
               pages.committed_columns(branch_one_tail) == 2,
           "private branch writes do not extend the shared partial tail");
    addresses.deactivate(*branch_one);
    addresses.destructive_truncate_inactive(*branch_one, 65);
    expect(addresses.committed_frontier(*branch_one) == 65 &&
               pages.committed_columns(branch_one_tail) == 1 &&
               pages.address_references(shared_full) == 2,
           "private suffix truncation retains shared full-prefix pages");

    const auto branch_two = addresses.create_inactive();
    expect(branch_two.has_value(), "second shared-prefix branch address allocation");
    auto second_fork = addresses.prepare_prefix_fork(*shared, *branch_two, 65, 3, 1);
    physical_pages.copy_page(addresses.prefix_fork_tail_source(second_fork),
                             addresses.prefix_fork_tail_destination(second_fork),
                             device.transfer_stream);
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    addresses.commit_prefix_fork(std::move(second_fork), device.stream);
    const auto branch_two_tail = addresses.logical_page(*branch_two, 1);
    expect(branch_two_tail != shared_tail && branch_two_tail != branch_one_tail &&
               pages.address_references(shared_full) == 3,
           "independent shared branches own distinct partial tails and one shared full page");
    addresses.deactivate(*branch_two);
    expect(addresses.release(*branch_one) && pages.address_references(shared_full) == 2 &&
               addresses.release(*branch_two) && pages.address_references(shared_full) == 1 &&
               addresses.release(*shared) && pages.occupied() == 0 &&
               physical_pages.allocated_pages() == 0,
           "shared full-page occupancy survives until its final address reference releases");

    const auto mixed_source = addresses.create_active(4, 0);
    expect(mixed_source.has_value(), "mixed snapshot retained-prefix source allocation");
    addresses.ensure_mapped_to_tokens(*mixed_source, 65, device.stream);
    addresses.commit_frontier(*mixed_source, 65);
    addresses.set_checkpoint_requirement(*mixed_source, 65);
    addresses.deactivate(*mixed_source);
    const auto mixed_shared_full = addresses.logical_page(*mixed_source, 0);

    const auto mixed_active = addresses.create_inactive();
    expect(mixed_active.has_value(), "mixed snapshot active branch allocation");
    auto mixed_fork = addresses.prepare_prefix_fork(*mixed_source, *mixed_active, 65, 4, 0);
    physical_pages.copy_page(addresses.prefix_fork_tail_source(mixed_fork),
                             addresses.prefix_fork_tail_destination(mixed_fork),
                             device.transfer_stream);
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    addresses.commit_prefix_fork(std::move(mixed_fork), device.stream);
    addresses.ensure_mapped_to_tokens(*mixed_active, 130, device.stream);
    addresses.commit_frontier(*mixed_active, 130);
    const auto mixed_mutable_full = addresses.logical_page(*mixed_active, 1);
    const auto mixed_tail         = addresses.logical_page(*mixed_active, 2);
    const store::KVActiveSnapshotShape first_shape =
        addresses.active_snapshot_shape(*mixed_active, 130);
    expect(first_shape.full_pages == 2 && first_shape.immutable_full_pages == 1 &&
               first_shape.unique_full_pages == 1 && first_shape.tail_columns == 2 &&
               first_shape.copied_pages() == 1,
           "mixed snapshot analysis distinguishes aliased prefix, mutable suffix, and tail");

    const auto aborted_destination = addresses.create_inactive();
    expect(aborted_destination.has_value(), "mixed snapshot abort destination allocation");
    {
        auto aborted = addresses.prepare_active_snapshot(*mixed_active, *aborted_destination, 130);
        expect(pages.source_pins(mixed_shared_full) == 1 &&
                   pages.source_pins(mixed_mutable_full) == 1 && pages.source_pins(mixed_tail) == 1,
               "mixed snapshot preparation pins every source ownership class");
        addresses.abort_active_snapshot(aborted);
    }
    expect(addresses.active(*mixed_active) && pages.source_pins(mixed_shared_full) == 0 &&
               pages.source_pins(mixed_mutable_full) == 0 && pages.source_pins(mixed_tail) == 0 &&
               pages.writer_references(mixed_shared_full) == 0 &&
               pages.writer_references(mixed_mutable_full) == 1 &&
               pages.writer_references(mixed_tail) == 1 && addresses.release(*aborted_destination),
           "aborted mixed snapshot restores pins without changing page ownership");

    const auto first_snapshot_active = addresses.create_inactive();
    expect(first_snapshot_active.has_value(), "mixed snapshot destination allocation");
    auto first_mixed_snapshot =
        addresses.prepare_active_snapshot(*mixed_active, *first_snapshot_active, 130);
    physical_pages.copy_page(addresses.active_snapshot_tail_source(first_mixed_snapshot),
                             addresses.active_snapshot_tail_destination(first_mixed_snapshot),
                             device.transfer_stream);
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    addresses.commit_active_snapshot(std::move(first_mixed_snapshot), device.stream);
    const auto first_snapshot_tail = addresses.logical_page(*first_snapshot_active, 2);
    expect(
        !addresses.active(*mixed_active) && addresses.active(*first_snapshot_active) &&
            addresses.logical_page(*first_snapshot_active, 0) == mixed_shared_full &&
            addresses.logical_page(*first_snapshot_active, 1) == mixed_mutable_full &&
            first_snapshot_tail != mixed_tail && pages.address_references(mixed_shared_full) == 3 &&
            pages.address_references(mixed_mutable_full) == 2 &&
            pages.writer_references(mixed_shared_full) == 0 &&
            pages.writer_references(mixed_mutable_full) == 0 &&
            pages.writer_references(mixed_tail) == 0 &&
            pages.writer_references(first_snapshot_tail) == 1,
        "mixed snapshot reuses immutable full pages, freezes mutable full pages, and copies tail");

    addresses.ensure_mapped_to_tokens(*first_snapshot_active, 192, device.stream);
    addresses.commit_frontier(*first_snapshot_active, 192);
    const store::KVActiveSnapshotShape aligned_shape =
        addresses.active_snapshot_shape(*first_snapshot_active, 192);
    expect(aligned_shape.full_pages == 3 && aligned_shape.immutable_full_pages == 2 &&
               aligned_shape.unique_full_pages == 1 && aligned_shape.tail_columns == 0 &&
               aligned_shape.copied_pages() == 0,
           "repeated aligned snapshot analysis preserves the immutable-to-mutable boundary");
    const auto aligned_active = addresses.create_inactive();
    expect(aligned_active.has_value(), "aligned mixed snapshot destination allocation");
    auto aligned_snapshot =
        addresses.prepare_active_snapshot(*first_snapshot_active, *aligned_active, 192);
    expect(!aligned_snapshot.needs_tail_copy(), "aligned mixed snapshot requires no KV copy");
    addresses.commit_active_snapshot(std::move(aligned_snapshot), device.stream);
    addresses.ensure_mapped_to_tokens(*aligned_active, 193, device.stream);
    addresses.commit_frontier(*aligned_active, 193);
    expect(addresses.active(*aligned_active) && addresses.mapped_pages(*aligned_active) == 4 &&
               pages.writer_references(addresses.logical_page(*aligned_active, 3)) == 1,
           "aligned snapshot continuation materializes a new mutable suffix page");

    addresses.deactivate(*aligned_active);
    expect(addresses.release(*aligned_active) && addresses.release(*first_snapshot_active) &&
               addresses.release(*mixed_active) && addresses.release(*mixed_source) &&
               pages.occupied() == 0 && physical_pages.allocated_pages() == 0,
           "repeated mixed snapshot ownership closes without leaked logical or physical pages");

    const auto filler = addresses.create_active(4, 0);
    expect(filler.has_value(), "full-capacity staged-fork filler allocation");
    addresses.ensure_mapped_to_tokens(*filler, 193, device.stream);
    addresses.commit_frontier(*filler, 193);
    addresses.set_checkpoint_requirement(*filler, 193);
    addresses.deactivate(*filler);

    const auto retained = addresses.create_active(4, 0);
    expect(retained.has_value(), "full-capacity retained source allocation");
    addresses.ensure_mapped_to_tokens(*retained, 65, device.stream);
    addresses.commit_frontier(*retained, 65);
    addresses.set_checkpoint_requirement(*retained, 65);
    addresses.deactivate(*retained);
    const auto staged_destination = addresses.create_inactive();
    expect(staged_destination.has_value(), "full-capacity staged-fork destination allocation");
    expect(physical_pages.allocated_pages() == 6 && physical_pages.available_pages() == 2,
           "full-capacity staged-fork fixture leaves only the transient tail headroom");

    bool ordinary_fork_rejected = false;
    try {
        auto ordinary = addresses.prepare_prefix_fork(*retained, *staged_destination, 65, 4, 1);
        (void)ordinary;
    } catch (const std::bad_alloc&) { ordinary_fork_rejected = true; }
    expect(ordinary_fork_rejected,
           "ordinary retained prefix fork cannot reserve the one-page-over capacity shape");

    auto staged = addresses.prepare_prefix_fork(*retained, *staged_destination, 65, 4, 1, true);
    const auto retained_tail = addresses.prefix_fork_tail_logical_source(staged);
    physical_pages.copy_page(addresses.prefix_fork_tail_source(staged),
                             addresses.prefix_fork_tail_destination(staged),
                             device.transfer_stream);
    const std::array retained_tail_membership{retained_tail};
    auto retained_tail_backup = extents.prepare(pages, retained_tail_membership);
    expect(retained_tail_backup.has_value(), "retained tail Host backup reservation");
    const auto retained_tail_sources = extents.device_sources(*retained_tail_backup);
    physical_pages.copy_to_host(retained_tail_sources, extents.writable_view(*retained_tail_backup),
                                device.transfer_stream);
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream));
    (void)extents.publish(std::move(*retained_tail_backup));
    addresses.settle_prefix_fork_tail_source(staged);
    expect(pages.drop_device_replica(retained_tail),
           "retained tail Device replica releases after both copies complete");
    addresses.complete_prefix_fork_after_tail_release(staged);
    addresses.commit_prefix_fork(std::move(staged), device.stream);
    expect(!pages.device_resident(retained_tail) && pages.host_resident(retained_tail) &&
               addresses.entitlement(*staged_destination) == 4 &&
               physical_pages.allocated_pages() == 6 && physical_pages.reserved_pages() == 2,
           "staged retained fork transfers the old tail capacity to the active entitlement");

    // THE RESERVATION CENSUS, PINNED TO WHAT IT ACTUALLY PRINTS. Observed at this point, not assumed:
    // `[census] text TOTAL address_reserved_pages=2 pool_reserved_pages=2 unaccounted=0 (exact: ...)`.
    //
    // WHY HERE, AND WHY IT MATTERS THAT THE SUM IS NON-ZERO: the pool holds 2 reserved pages at this line, so
    // `unaccounted` has a non-zero counterparty and the mutation control -- over-count the sum -- can actually
    // flip it NEGATIVE. At a point where 0 == 0, doubling the sum would still read `exact` and the control
    // would prove nothing while appearing to pass.
    {
        const std::string census = capture_census(addresses, "text");
        // THE VERDICT GOES INTO THE MESSAGE, because a failure that says only "did not match" cannot tell a
        // reader whether the line changed its answer or stopped printing at all -- and the mutation control
        // depends on seeing which.
        const std::string::size_type total = census.find("TOTAL");
        const std::string verdict =
            total == std::string::npos ? std::string("<no TOTAL line: the census did not print>")
                                       : census.substr(total);
        expect(census.find("TOTAL address_reserved_pages=2 pool_reserved_pages=2 unaccounted=0") !=
                   std::string::npos,
               std::string("the reservation census does not account for exactly the 2 reserved pages the "
                           "pool holds; it said: ") + verdict);
        expect(census.find("(exact:") != std::string::npos,
               std::string("the census does not report 'exact' where every reserved page belongs to a live "
                           "address; it said: ") + verdict);

        // (The POSITIVE case is staged further down, once the pool is EMPTY: reserving here fails, because the
        // pool is full at this point -- allocated 6 + reserved 2 -- and a case that cannot obtain its own
        // reservation measures nothing while reporting a failure about the census.)
    }

    addresses.deactivate(*staged_destination);
    const bool released = addresses.release(*staged_destination) && addresses.release(*retained) &&
                          addresses.release(*filler);
    // The retained side is asserted here too: this case is named "closes ownership without leaks", and until
    // the census existed the only way to read that was `released_bytes` on its own -- which cannot distinguish
    // "released everything" from "released some and kept the rest pinned".
    const auto fork_teardown = extents.release_unreferenced();
    expect(released && fork_teardown.released_bytes == host_layout.page_stride &&
               fork_teardown.retained_referenced == 0 && fork_teardown.retained_pinned == 0 &&
               pages.occupied() == 0 &&
               physical_pages.allocated_pages() == 0 && physical_pages.reserved_pages() == 0,
           "staged retained fork closes Device and Host ownership without leaks");
}

} // namespace

int main() {
    int count                   = 0;
    const cudaError_t count_err = cudaGetDeviceCount(&count);
    if (cuda_unavailable(count_err) || count == 0) {
        std::cout << "SKIP: no usable CUDA device\n";
        return 77;
    }
    CUDA_CHECK(count_err);

    try {
        ninfer::DeviceContext device(0);
        test_state_store(device);
        test_kv_store(device);
        test_host_kv_pregrow(device);
        device.synchronize();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
