#!/usr/bin/env bash
# Guard against citing a TEST BINARY that does not correspond to the source you think it does.
#
# WHY THIS EXISTS (2026-09-27): a target was added to tests/cmake/CoreTests.cmake without re-running CMake, so
# `cmake --build --target <name>` printed `No rule to make target`; the caller had filtered the output for
# "error:" and saw nothing, then ran `build/tests/<name>` — which was a STALE binary from an earlier state
# that printed PASS. A green result came from an artifact that no current source produces. The same directory
# held 41 executables with no CMake target at all (V2-era names: ninfer_qwen3_6_*, ninfer_host_kv_*), any of
# which could be run by path and believed.
#
# TWO CHECKS, because there are two ways to be misled:
#   orphans  -- an executable in build/tests that no target builds. CERTAINLY misleading; safe to delete
#               (build artifacts are regenerable).
#   --verify -- for one test: does a target exist, and does a FRESH build of it succeed? A pass is evidence
#               only when it comes from a build that ran now, so this REMOVES the artifact first: a stale
#               binary cannot then be mistaken for a fresh one.
#
# This is the test-side analogue of the repo's rule for prod ("verify by the running exe's hash, never the
# build directory's") and of `find -newer` for the serve binary.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
BUILD="${BUILD:-build}"

if [ "${1:-}" = "--verify" ]; then
  name="${2:?usage: $0 --verify <test-target>}"
  # GREP A FILE, NOT A PIPE. `cmake ... | grep -q` is a trap under `set -o pipefail`: grep -q exits at the
  # first match, cmake then dies of SIGPIPE, and the pipeline reports failure -- so the check refused a target
  # that exists (observed 2026-09-27, on the first run of this very script). The test must be able to PASS or
  # it is not a guard.
  cmake --build "$BUILD" --target help 2>/dev/null >/tmp/ctb-help.txt
  if ! grep -qx "\.\.\. ${name}" /tmp/ctb-help.txt; then
    echo "REFUSED: '$name' is not a CMake target -- a binary of that name, if present, is stale" >&2
    exit 2
  fi
  rm -f "$BUILD/tests/$name"
  if ! cmake --build "$BUILD" -j --target "$name" >/tmp/verify-test-build.log 2>&1; then
    echo "REFUSED: the build of '$name' FAILED -- no result may be cited from it. Tail:" >&2
    tail -5 /tmp/verify-test-build.log >&2
    exit 3
  fi
  [ -x "$BUILD/tests/$name" ] || { echo "REFUSED: build succeeded but no binary was produced" >&2; exit 4; }
  echo "fresh build of $name at $(date +%H:%M:%S), running it:"
  exec "$BUILD/tests/$name"
fi

cmake --build "$BUILD" --target help 2>/dev/null | sed -n 's/^\.\.\. \([a-zA-Z0-9_]*\).*/\1/p' | sort -u >/tmp/ctb-targets.txt
orphans=0
for f in "$BUILD"/tests/*; do
  [ -f "$f" ] && [ -x "$f" ] || continue
  case "$f" in *.a|*.o|*.cmake) continue ;; esac
  n=$(basename "$f")
  grep -qx "$n" /tmp/ctb-targets.txt || { echo "ORPHAN (no target builds it): $f"; orphans=$((orphans+1)); }
done
if [ "$orphans" -eq 0 ]; then
  echo "no orphan test binaries: every executable in $BUILD/tests has a target"
  exit 0
fi
echo "$orphans orphan test binaries above -- delete them, or a run by path may report a stale result" >&2
exit 1
