#!/usr/bin/env bash
# Positive control for the claim "#11(a)'s recycling branch is unreachable" (plan.md §2 item 3).
#
# The probe in `inspect_capture` aborts (not prints) when `assessment.recycles_private_state` is true
# under NINFER_CAPTURE_PROBE=1, so every scenario that can reach the branch fails loudly and every
# scenario that cannot completes. An abort-by-design is deliberate: a print can be read as "did not
# happen" when the probe never ran, which is the mistake that produced today's retracted conclusions.
#
# Run it as the GPU window's target (one window, all scenarios):
#   bash tools/e2e/ninfer-gpu-window.sh tools/e2e/recycling-reachability.sh 2400
#
# Logs are kept per scenario under results/recycling-reachability/ and are the evidence a claim about
# this branch should cite -- the earlier runs of this investigation were not saved anywhere, so the
# trace quoted from them could not be re-derived.
set -uo pipefail

BIN="${BIN:-./build-diag/tests/ninfer_qwen3_5_prefix_real_test}"
# Timestamped by default: an earlier version wrote every run to the same directory, so the re-run
# destroyed the first run's logs and the "both runs agree" claim lost its artifact.
OUT="${OUT:-results/recycling-reachability/$(date +%Y%m%d-%H%M%S)}"
export NINFER_TEST_ARTIFACT="${NINFER_TEST_ARTIFACT:-/home/zenz/ninfer-models/swift15/qwen3_8_27b_nvfp4swift15.ninfer}"
export NINFER_CAPTURE_PROBE=1

mkdir -p "$OUT"

if [ ! -x "$BIN" ]; then echo "[reach] no such binary: $BIN" >&2; exit 2; fi

# Every in-process scenario of this target that builds a capture, `vision` included -- it generates its
# own image (`gradient_ppm()`), needs no fixture, runs two turns with reuse enabled and asserts a nonzero
# reuse. `all` is excluded: it fails early on a pre-existing golden mismatch ("registered tokenizer/chat
# template changed the thinking prompt golden"), so a pass or a fail there would say nothing here.
SCENARIOS=(
  recycling-capture
  rewrite-checkpoint
  rewrite-checkpoint-shared
  shared-rewrite-materialization
  vision
  shared-replacement
  private-long-anchor
  pressure-resume
  private-checkpoint-pressure
  source-pressure-protection
  concurrent
  anthropic-prefix-regression
)

reachable=0
failed=0
total_calls=0
for scenario in "${SCENARIOS[@]}"; do
  log="$OUT/${scenario}.log"
  NINFER_PREFIX_REAL_SCENARIO="$scenario" timeout 600 "$BIN" >"$log" 2>&1
  rc=$?
  # No `|| echo 0` here: `grep -c` prints the count AND exits 1 when it is zero, so the fallback
  # appended a second line and made every scenario compare unequal to "0". That misclassified all
  # eleven as REACHABLE on the first run of this script -- the same "instrument measuring something
  # else" failure the probe exists to avoid, in the control for it.
  hits=$(grep -c 'ASSERT recycles_private_state is REACHABLE' "$log" 2>/dev/null)
  # The denominator, per scenario. Without it a run where the probe never executed -- stale binary,
  # crash at startup, test that never reached a capture -- is indistinguishable from a clean negative,
  # which is exactly the failure mode this whole investigation kept hitting.
  # The probe's line now carries its call site (`assess site=%s lane=%u`), so this counter must match the
  # CURRENT format: it was left greping `assess lane=` after that change, which made every scenario report
  # "NO ASSESSMENTS" and the run's denominator zero -- the script declaring its own evidence worthless
  # because its counter was stale. Match the prefix that cannot move: `[capture] assess`.
  calls=$(grep -c '\[capture\] assess' "$log" 2>/dev/null)
  total_calls=$((total_calls + calls))
  if [ "$calls" -eq 0 ] && [ "$hits" -eq 0 ]; then
    echo "[reach] $scenario: NO ASSESSMENTS (rc=$rc) -- the probe did not run; this scenario is"
    echo "[reach] $scenario:   evidence of nothing. read $log"
    failed=$((failed + 1))
    continue
  fi
  if [ "$hits" != 0 ]; then
    echo "[reach] $scenario: REACHABLE (rc=$rc, $hits abort(s), $calls assessments) -- read $log"
    reachable=$((reachable + 1))
  elif [ "$rc" = 124 ]; then
    echo "[reach] $scenario: TIMEOUT after $calls assessments -- inconclusive, read $log"
    failed=$((failed + 1))
  elif [ "$rc" != 0 ]; then
    echo "[reach] $scenario: rc=$rc (not the assert), $calls assessments -- failed for its own"
    echo "[reach] $scenario:   reason, so its silence is weak evidence; read $log"
    failed=$((failed + 1))
  else
    echo "[reach] $scenario: ok, $calls assessments, recycles_private_state never true"
  fi
done

# The manifest ties the logs to the exact binary and tree that produced them. Without it a log is only
# as good as the reader's memory of which build made it -- and the run of 2026-09-26 10:13 was rebuilt
# over 2 minutes later, so its logs could not be tied to the committed source.
{
  echo "binary: $(sha256sum "$BIN")"
  echo "git-diff: $(git -C "$(dirname "$0")/../.." diff | sha256sum)"
  echo "git-head: $(git -C "$(dirname "$0")/../.." rev-parse HEAD)"
  echo "artifact: $NINFER_TEST_ARTIFACT"
  echo "assessments-total: $total_calls"
} >"$OUT/manifest"

# Attribution phase for the two scenarios that fail their own golden assertions. They are re-run with the
# probe UNSET: if they fail the same way, the probes are not the cause; if they pass, the probes are. This
# does not establish that they pre-date the diff (that needs a HEAD build), but it separates the one
# hypothesis that would impugn the engine change from the ones that would not.
for scenario in rewrite-checkpoint-shared shared-replacement; do
  log="$OUT/${scenario}.no-probe.log"
  env -u NINFER_CAPTURE_PROBE NINFER_PREFIX_REAL_SCENARIO="$scenario" timeout 600 "$BIN" >"$log" 2>&1
  rc=$?
  if [ "$rc" = 0 ]; then
    echo "[reach] $scenario: PASSES without the probe (informational only -- the goldens were CORRECTED\n          2026-09-26, so these two pass either way and this phase no longer discriminates anything)"
  else
    echo "[reach] $scenario: fails the same way without the probe (rc=$rc) -- not probe timing;"
    echo "[reach]   whether it pre-dates the diff is still unestablished; read $log"
  fi
done
echo "[reach] attribution logs: $OUT/*.no-probe.log (re-run with NINFER_CAPTURE_PROBE unset)"

echo "[reach] done: ${#SCENARIOS[@]} scenarios, reachable=$reachable, failed-or-inconclusive=$failed,"
echo "[reach]       assessments=$total_calls (the denominator -- if this is 0 the run proved nothing)"
echo "[reach] logs: $OUT/  manifest: $OUT/manifest"

# Contract, so the window's rc is self-validating: 0 only when every scenario ran, the probe fired in
# each, and nothing reached the branch. 1 = the branch is reachable (the plan's claim is refuted);
# 3 = the run was incomplete, so it is not evidence either way.
if [ "$reachable" != 0 ]; then exit 1; fi
if [ "$failed" != 0 ] || [ "$total_calls" = 0 ]; then exit 3; fi
exit 0
