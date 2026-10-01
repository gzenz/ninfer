#!/bin/bash
# Window 9: P2.4 full default scenario set (GPU-only) — first full clean run
# of all 4 exercises: source-pressure-protection, private-checkpoint-pressure,
# pressure-resume (two-message continuation, checkpoint-frontier match, oracle
# re-pinned to the page-count evidence), and concurrent-settlement (never ran:
# earlier windows early-returned at pressure-resume).
#
# Runs as a background task; prod + sentinel restored in the EXIT trap.
set -uo pipefail

TS(){ date '+%H:%M:%S'; }
LOG=/tmp/ninfer-window9.log
: > "$LOG"
W(){ echo "[$(TS)] $*" | tee -a "$LOG"; }

P24_TEST=/home/zenz/ninfer/build/tests/ninfer_qwen3_6_27b_prefix_real_test
P24_WEIGHTS=/home/zenz/ninfer-models/qwen3_8_27b_nvfp4-froggeric.ninfer
P24_LOG=/tmp/ninfer-p24-full9.log

gpu_used(){ nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null | head -1; }
wait_gpu_free(){
  for i in $(seq 1 45); do
    local u; u=$(gpu_used); [ -n "$u" ] && [ "$u" -lt 1000 ] 2>/dev/null && return 0
    sleep 2
  done
  return 1
}

RESTORED=0
restore_prod(){
  [ "$RESTORED" = 1 ] && return
  RESTORED=1
  W "RESTORE: stopping any server, starting prod + sentinel"
  pkill -f "build/apps/ninfer-serve" 2>/dev/null || true
  sleep 3
  sudo -n systemctl start ninfer.service 2>/dev/null || W "WARN: systemctl start ninfer failed"
  for i in $(seq 1 90); do
    curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1 && break
    sleep 2
  done
  if curl -sf -m3 http://localhost:8080/health >/dev/null 2>&1; then
    W "RESTORE: prod healthy"
  else
    W "RESTORE: FATAL prod not healthy after restore"
  fi
  sudo -n systemctl start ninfer-wedge-sentinel.service 2>/dev/null || W "WARN: sentinel not restarted"
  W "RESTORE: done"
}
trap restore_prod EXIT
trap 'exit 1' INT TERM

W "=== WINDOW 9 START (GPU used: $(gpu_used) MiB) ==="

# --- Phase 1: stop sentinel + prod ---
W "P1: stopping sentinel + prod"
sudo -n systemctl stop ninfer-wedge-sentinel.service 2>/dev/null || true
sudo -n systemctl stop ninfer.service 2>/dev/null || true
sleep 3
pkill -f "build/apps/ninfer-serve" 2>/dev/null || true
sleep 2
if wait_gpu_free; then W "P1: GPU free ($(gpu_used) MiB)"; else W "P1: WARN GPU not fully free ($(gpu_used) MiB)"; fi

# --- Phase 2: P2.4 full default scenario set ---
W "P2: P2.4 full default scenario set (4 exercises)"
if [ -x "$P24_TEST" ]; then
  timeout 900 env NINFER_QWEN3_8_27B_NVFP4_WEIGHTS="$P24_WEIGHTS" \
      "$P24_TEST" > "$P24_LOG" 2>&1
  P24_RC=$?
  W "P2: P2.4 rc=$P24_RC (log: $P24_LOG)"
  grep -E "did not|relief|safety-spill|safety-find|relief-kv|admission|ok$|PASS|FAIL" "$P24_LOG" | tail -50 | tee -a "$LOG"
  [ "$P24_RC" = "0" ] && echo "P2: ALL SCENARIOS PASS" | tee -a "$LOG"
else
  W "P2: P2.4 test binary MISSING at $P24_TEST"
fi

# --- Phase 3: restore (EXIT trap) ---
W "=== WINDOW 9 END (prod restore in EXIT trap) ==="
exit 0
