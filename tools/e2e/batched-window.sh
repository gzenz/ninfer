#!/bin/bash
# Batched prod-stopped window: P2.4 GPU-exclusive e2e + P1.9 recall probe.
#
# Runs as a BACKGROUND task (the window exceeds the 10-min foreground tool
# cap). PROD + SENTINEL are restored in the EXIT trap on ANY exit path, so the
# session is never left with prod down. The session is frozen for the whole
# window (no classifier-dependent calls until prod is back up).
#
# Phases:
#   1. stop sentinel + prod (frees the GPU)
#   2. P2.4 GPU-exclusive test (loads 18GB froggeric model; no server)
#   3. start full-context probe server (prod QUASAR model + prod ARGS, 555k ctx)
#   4. recall probe (cold prefill + warm restore) -> settles H-A vs H-C
#   5. stop probe server; EXIT trap restores prod + sentinel
set -uo pipefail

TS(){ date '+%H:%M:%S'; }
LOG=/tmp/ninfer-batched-window.log
: > "$LOG"
W(){ echo "[$(TS)] $*" | tee -a "$LOG"; }

CONF=/home/zenz/.config/ninfer.conf
SERVER_BIN=/home/zenz/ninfer/build/apps/ninfer-serve
P24_TEST=/home/zenz/ninfer/build/tests/ninfer_qwen3_6_27b_prefix_real_test
P24_WEIGHTS=/home/zenz/ninfer-models/qwen3_8_27b_nvfp4-froggeric.ninfer
PROBE=/home/zenz/ninfer/tools/longctx_recall_probe.py
PROBE_JSON=/tmp/ninfer-p19-probe.json
PROBE_LOG=/tmp/ninfer-p19-probe.log
P24_LOG=/tmp/ninfer-p24-test.log
SRV_LOG=/tmp/ninfer-probe-server.log

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

W "=== BATCHED WINDOW START (GPU used: $(gpu_used) MiB) ==="

# --- Phase 1: stop sentinel + prod ---
W "P1: stopping sentinel + prod"
sudo -n systemctl stop ninfer-wedge-sentinel.service 2>/dev/null || true
sudo -n systemctl stop ninfer.service 2>/dev/null || true
sleep 3
pkill -f "build/apps/ninfer-serve" 2>/dev/null || true
sleep 2
if wait_gpu_free; then W "P1: GPU free ($(gpu_used) MiB)"; else W "P1: WARN GPU not fully free ($(gpu_used) MiB)"; fi

# --- Phase 2: P2.4 GPU-exclusive test ---
W "P2: P2.4 GPU-exclusive test (froggeric 18GB)"
if [ -x "$P24_TEST" ]; then
  timeout 900 env NINFER_QWEN3_8_27B_NVFP4_WEIGHTS="$P24_WEIGHTS" \
      "$P24_TEST" > "$P24_LOG" 2>&1
  P24_RC=$?
  W "P2: P2.4 rc=$P24_RC (log: $P24_LOG)"
  tail -n 20 "$P24_LOG" | tee -a "$LOG"
else
  W "P2: P2.4 test binary MISSING at $P24_TEST"
fi
wait_gpu_free || W "P2: WARN GPU not free after test"

# --- Phase 3: start full-context probe server (prod model + prod ARGS) ---
W "P3: starting full-context probe server (prod QUASAR + prod ARGS)"
. "$CONF"
export PATH=/usr/local/cuda/bin:/home/zenz/.local/bin:/home/zenz/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda/lib64:/usr/lib/wsl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
cd /home/zenz/ninfer
nohup "$SERVER_BIN" "$MODEL" $ARGS > "$SRV_LOG" 2>&1 &
SRV_PID=$!
W "P3: probe server pid=$SRV_PID"
SRV_UP=0
for i in $(seq 1 90); do
  sleep 3
  curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1 && { SRV_UP=1; break; }
done
if [ "$SRV_UP" = 1 ]; then
  W "P3: probe server healthy"
else
  W "P3: FATAL probe server not healthy — tail of $SRV_LOG:"
  tail -n 25 "$SRV_LOG" | tee -a "$LOG"
fi

# --- Phase 4: recall probe (cold + warm) ---
if [ "$SRV_UP" = 1 ]; then
  W "P4: recall probe (cold prefill + warm restore)"
  timeout 1500 python3 "$PROBE" --json "$PROBE_JSON" > "$PROBE_LOG" 2>&1
  PROBE_RC=$?
  W "P4: probe rc=$PROBE_RC (log: $PROBE_LOG, json: $PROBE_JSON)"
  grep -E "===|PASS|FAIL|zone|recalled|prompt_tokens|VERDICT" "$PROBE_LOG" | tee -a "$LOG" | tail -40
else
  W "P4: SKIP (probe server not up)"
fi

# --- Phase 5: stop probe server (EXIT trap restores prod) ---
W "P5: stopping probe server"
kill "$SRV_PID" 2>/dev/null || true
pkill -f "build/apps/ninfer-serve" 2>/dev/null || true
sleep 3

W "=== BATCHED WINDOW END (prod restore in EXIT trap) ==="
exit 0
