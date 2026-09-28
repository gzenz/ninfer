#!/usr/bin/env bash
# Evidence run for the #9/#11(b) claims, saving stderr under results/ so the readings plan.md quotes
# exist as artifacts rather than only in a session transcript (a review pass flagged exactly that).
#
#   bash tools/e2e/ninfer-gpu-window.sh tools/e2e/n9-evidence.sh 1200
#
# Covers: the `[kv-restore]` recording-capacity probe, the #11(b) counter printing its DENOMINATOR at
# zero (the property that makes it an instrument at all), the residual readings, and the census.
set -uo pipefail

cd "$(dirname "$0")/../.."
OUT="${OUT:-results/n9-evidence/$(date +%Y%m%d-%H%M%S)}"
BIN="${BIN:-./build-diag/tests/ninfer_qwen3_5_prefix_real_test}"
export NINFER_TEST_ARTIFACT="${NINFER_TEST_ARTIFACT:-/home/zenz/ninfer-models/swift15/qwen3_8_27b_nvfp4swift15.ninfer}"
mkdir -p "$OUT"

run() { # run <label> <scenario> [env assignments...]
  local label="$1" scenario="$2"; shift 2
  local log="$OUT/${label}.log"
  echo "[n9] $label -> $log"
  env "$@" NINFER_PREFIX_REAL_SCENARIO="$scenario" timeout 300 "$BIN" >"$log" 2>&1
  echo "[n9] $label rc=$? kv-restore=$(grep -c 'kv-restore' "$log") priced=$(grep -c 'StateImage priced' "$log") residual_nonzero=$(grep -cE 'residual.*(main_kv_pages|host_kv_bytes)=[1-9]' "$log")"
}

# 1. the recording-capacity probe, no injection
run probe-pressure-resume pressure-resume NINFER_CAPTURE_PROBE=1
# 2. the same scenario WITHOUT the probe: no [kv-restore] lines, but the #11(b) counters still print --
#    they are ungated and rate-limited, which is deliberate, so the denominator is visible without any
#    diagnostic variable set. (An earlier comment here claimed the opposite; it was written for a version
#    where the print was gated behind the probe.)
run plain-pressure-resume pressure-resume
# 3. the reproduced leak, for the record: residual non-zero and the census naming the survivor
run repro-pressure-resume pressure-resume NINFER_INJECT_THROW=mat-reserve-replica
# 4. a second scenario, to show the counter's denominator appears wherever pricing runs
run probe-private-checkpoint-pressure private-checkpoint-pressure NINFER_CAPTURE_PROBE=1

{
  echo "binary: $(sha256sum "$BIN")"
  echo "git-diff: $(git -C . diff | sha256sum)"
  echo "git-head: $(git rev-parse HEAD)"
} >"$OUT/manifest"
# The DIFF ITSELF, not only its hash: a hash cannot be turned back into source, so a run whose
# placement of an injected fault mattered could not be re-read from its own evidence. A review pass
# caught exactly that gap ("'these are the mis-placed-fault runs' rests on memory").
git -C . diff >"$OUT/tree.diff"
echo "[n9] logs: $OUT/"
