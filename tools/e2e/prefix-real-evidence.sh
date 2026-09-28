#!/usr/bin/env bash
# Evidence run for the 2026-09-26 review repair: ONE GPU window, three artifacts, each tied to the binary
# that produced it.
#
#   bash tools/e2e/ninfer-gpu-window.sh tools/e2e/prefix-real-evidence.sh 1800
#
# Runs, in order:
#   1. the reachability battery (`recycling-reachability.sh`, `NINFER_CAPTURE_PROBE=1`, 12 scenarios) --
#      it writes its own manifest under results/recycling-reachability/;
#   2. `all`, TWICE, probe off -- "all passes twice" is a claim about two runs and needs two saved logs,
#      which is what the review found missing (the previous pair came from /tmp and was piped to `head`);
#   3. `vision` with `NINFER_MAT_DEBUG=1`, probe off -- re-derives the #6 victim line
#      (`demotable=... frontier=... endpoint=... rewrite=...`) from the COMMITTED code. The quoted copy of
#      that line was produced before `materialization.cpp` was last edited, so it did not describe the
#      code it was cited for.
#
# Why this is a script and not a command line: every number these runs support was, until now, quoted from
# terminal output that was never saved, and one of them (`frontier=190`) appears in no file under
# `results/` at all. A run that leaves no artifact is not evidence (`CLAUDE.md`, Reviewing), so the
# artifact is the output of this script, and its manifest names the binary sha256 that produced it.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BIN="${BIN:-./build-diag/tests/ninfer_qwen3_5_prefix_real_test}"
ART="${NINFER_TEST_ARTIFACT:-/home/zenz/ninfer-models/swift15/qwen3_8_27b_nvfp4swift15.ninfer}"
export NINFER_TEST_ARTIFACT="$ART"
OUT="${OUT:-results/prefix-real-evidence/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"

if [ ! -x "$BIN" ]; then echo "[evidence] no such binary: $BIN" >&2; exit 2; fi
if [ ! -f "$ART" ]; then echo "[evidence] no such artifact: $ART" >&2; exit 2; fi

# The SOURCE is recorded as a diff, not as a count of dirty paths. `git status --porcelain | wc -l` cannot
# tell two runs apart -- `172138` and `172831` share `git_head=d4cd4d67` and differ in source (only 172831
# prints the delta line), so their manifests could not be compared. `recycling-reachability.sh` hashes a
# `git diff` and `n9-evidence`/`golden-attribution` keep a full `tree.diff`; this does both.
# `git diff HEAD`, not `git diff`: plain `git diff` compares the worktree with the INDEX, so a staged edit
# is invisible and an untracked new file appears in neither (the watcher's own files were untracked until
# they were committed). Untracked files are hashed separately, because no diff can show them.
git diff HEAD >"$OUT/tree.diff" 2>/dev/null || true
git status --porcelain >"$OUT/git-status.txt" 2>/dev/null || true
git ls-files -o --exclude-standard -z 2>/dev/null | xargs -0 -r sha256sum >"$OUT/untracked-sha256.txt" 2>/dev/null || true
{
  echo "binary=$BIN"
  echo "binary_sha256=$(sha256sum "$BIN" | awk '{print $1}')"
  echo "binary_mtime=$(date -r "$BIN" -Is)"
  echo "artifact=$ART"
  echo "artifact_sha256=$(sha256sum "$ART" | awk '{print $1}')"
  echo "git_head=$(git rev-parse HEAD)"
  echo "git_head_tree=$(git rev-parse HEAD^{tree})"
  echo "tree_diff_sha256=$(sha256sum "$OUT/tree.diff" | awk '{print $1}')"
  echo "git_status_sha256=$(sha256sum "$OUT/git-status.txt" | awk '{print $1}')"
  echo "untracked_sha256=$(sha256sum "$OUT/untracked-sha256.txt" | awk '{print $1}')"
  echo "git_dirty_paths=$(wc -l <"$OUT/git-status.txt")"
  echo "started=$(date -Is)"
} | tee "$OUT/manifest"

rc_total=0
run() { # <name> <env-assignment>...
  local name="$1"; shift
  echo "[evidence] $name"
  env "$@" "$BIN" >"$OUT/$name.log" 2>&1
  local rc=$?
  printf '%s rc=%s log_sha256=%s\n' "$name" "$rc" "$(sha256sum "$OUT/$name.log" | awk '{print $1}')" \
    >>"$OUT/manifest"
  [ "$rc" = 0 ] || rc_total=$rc
  # The rc is the assertion result; a scenario that never ran also exits 0 in some harnesses, so print the
  # scenario's own terminal line as the denominator that says it executed at all.
  grep -h '^\[scenario\]\|^ok\b\|^\[all\]' "$OUT/$name.log" | tail -3 || true
}

# 1. Reachability battery (probe ON). Its own script, its own manifest.
echo "[evidence] reachability battery"
NINFER_CAPTURE_PROBE=1 bash tools/e2e/recycling-reachability.sh 2>&1 | tee "$OUT/reachability.log"
reach_rc=${PIPESTATUS[0]}
echo "reachability rc=$reach_rc" >>"$OUT/manifest"
[ "$reach_rc" = 0 ] || rc_total=$reach_rc

# 2. `all` twice, probe OFF.
run all-1 NINFER_PREFIX_REAL_SCENARIO=all
run all-2 NINFER_PREFIX_REAL_SCENARIO=all

# 3. `vision` with the victim print on, probe OFF.
run vision-victim NINFER_PREFIX_REAL_SCENARIO=vision NINFER_MAT_DEBUG=1

echo "finished=$(date -Is)" >>"$OUT/manifest"
echo "[evidence] logs: $OUT"
exit "$rc_total"
