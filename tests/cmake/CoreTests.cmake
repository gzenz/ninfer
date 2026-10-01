# This target deliberately receives no src/, CUDA, artifact, kernel, or target
# include root. It proves that the public product headers stand alone.
add_executable(ninfer_public_api_test "${CMAKE_CURRENT_LIST_DIR}/../test_public_api.cpp")
target_include_directories(ninfer_public_api_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME ninfer_public_api_test COMMAND ninfer_public_api_test)

ninfer_add_test(ninfer_device_test       SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_device.cpp"
  LIBRARIES ninfer_core)

set_tests_properties(ninfer_device_test PROPERTIES
  ENVIRONMENT_MODIFICATION "NINFER_CUDA_SYNC=unset:")
set(sync_modes spin blocking yield auto)
set(sync_flags 1 4 2 0)
foreach(mode flags IN ZIP_LISTS sync_modes sync_flags)
  add_test(NAME ninfer_device_sync_${mode}_test COMMAND ninfer_device_test ${flags})
  set_tests_properties(ninfer_device_sync_${mode}_test PROPERTIES
    ENVIRONMENT "NINFER_CUDA_SYNC=${mode}" SKIP_RETURN_CODE 77)
endforeach()
foreach(mode IN ITEMS invalid empty)
  add_test(NAME ninfer_device_sync_${mode}_test COMMAND ninfer_device_test --invalid-sync)
endforeach()
set_tests_properties(ninfer_device_sync_invalid_test PROPERTIES
  ENVIRONMENT "NINFER_CUDA_SYNC=invalid")
set_tests_properties(ninfer_device_sync_empty_test PROPERTIES
  ENVIRONMENT "NINFER_CUDA_SYNC=")

ninfer_add_test(ninfer_decode_graph_test SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_decode_graph.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_tensor_test       SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_tensor.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_arena_test        SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_arena.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_materialization_budget_test SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_materialization_budget.cpp"
  LIBRARIES ninfer_core)

# The pressure target arena's bound -- the rule that failed on 2026-09-28 (a full arena threw and
# took the request down as an HTTP 500 + a worker recovery). Header-only; no GPU, no Program.
ninfer_add_test(ninfer_pressure_target_arena_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_pressure_target_arena.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_materialization_preservation_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_materialization_preservation.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_publication_cell_loss_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_publication_cell_loss.cpp"
  LIBRARIES ninfer_core)

# -D_GLIBCXX_ASSERTIONS, and it is LOAD-BEARING FOR ONE CASE IN THAT TEST. The bounds case (a candidate list
# longer than the tally) asserts that `publication_cell_loss` does not read past the end of the span. With a
# release-built libstdc++ that case CANNOT FAIL: removing the `std::min` bound makes it return rc=0 with every
# check green, and only aborts (span:288, `__idx < size()`) once assertions are on -- verified both ways,
# 2026-09-27. A control that cannot go red in the build it runs in is not a control, so the flag is set here
# rather than left to a sanitizer job that does not exist.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ninfer_publication_cell_loss_test PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-D_GLIBCXX_ASSERTIONS>)
endif()

ninfer_add_test(ninfer_serve_session_key_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_serve_session_key.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_kv_cache_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_kv_cache.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_state_store_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_state_store.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_gdn_replay_records_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_gdn_replay_records.cpp"
  LIBRARIES ninfer_core)

# No GPU needed: the pool's chunk source is injected, so its growth and shrink paths are exercised on any
# host (see tests/test_pinned_host_pool.cpp).
ninfer_add_test(ninfer_pinned_host_pool_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_pinned_host_pool.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_host_memory_budget_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_host_memory_budget.cpp"
  LIBRARIES ninfer_core)

set_tests_properties(
  ninfer_device_test
  ninfer_decode_graph_test
  ninfer_arena_test
  ninfer_kv_cache_test
  ninfer_state_store_test
  PROPERTIES SKIP_RETURN_CODE 77)

ninfer_add_test(ninfer_host_timing_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_host_timing.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_jinja_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../text/test_jinja.cpp"
  LIBRARIES ninfer_jinja ninfer::json)

add_test(NAME ninfer_chat_templates_test
  COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/text/test_chat_templates.py
          $<TARGET_FILE:ninfer_jinja_test>)
