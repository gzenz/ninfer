find_package(Python3 REQUIRED COMPONENTS Interpreter)

include(cmake/NinferTests.cmake)
include(cmake/CoreTests.cmake)
include(artifact/tests.cmake)
include(cmake/RuntimeTests.cmake)
include(cmake/ProductTests.cmake)
include(cmake/BenchSupportTests.cmake)
include(models/qwen3_5/tests.cmake)
include(ops/tests.cmake)

# THE WATCHER'S ALERT SET, WHICH NOTHING RAN. `tools/ops/ninfer-watch-test.sh` has existed and passed since
# the filter was written, but it was registered nowhere -- so the monitor's semantics were covered by a script
# that only ran when a human invoked it, and a review pass found three of this tree's own instruments unalerted
# while this test was green. It is a script test, so it uses `add_test` directly (the `ninfer_add_test` helper
# builds an executable). No GPU, no artifact, no server: it filters a synthetic journal and asserts the alert
# set is exactly as expected, in both directions.
add_test(NAME ninfer_watch_filter_test
  COMMAND bash ${PROJECT_SOURCE_DIR}/tools/ops/ninfer-watch-test.sh)
