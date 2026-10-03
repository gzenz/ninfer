#!/bin/bash
# Swift artifact KV-ceiling probe: with dflash2 + vision + nvfp4 KV, what is
# the largest --max-context (== min KV pool, v3 constraint) that starts?
# Tries ctx candidates descending; first that boots wins. Reads /stats for
# the actual pool + VRAM breakdown.
#
# ONE window: stops prod, probes, restores prod. An EXIT trap guarantees the
# restore runs no matter how this script dies (the 2026-09-19 orphan incident
# left prod down because the script was killed mid-window).
set -u
LOG=/tmp/ninfer-cmp-swap.log
SUMMARY=/tmp/ninfer-swift-kvprobe-summary-$SPEC.txt
SERVE_LOG=~/ninfer-serve.log
STATS_JSON=/tmp/ninfer-swift-kvprobe-$SPEC.json
MODEL=~/ninfer-models/swift15/qwen3_8_27b_nvfp4swift15.ninfer
BIN=~/ninfer-yarn/build/apps/ninfer-serve
CTXS=(350000 300000 262144 220000)
SPEC="${SPEC:-dflash2}"
DRAFT="${DRAFT:-7}"
VISION="${VISION:-1}"
KV="${KV:-auto}"
ROPE_FACTOR="${ROPE_FACTOR:-1}"
ROPE_ORIG="${ROPE_ORIG:-262144}"
ts(){ date '+%H:%M:%S'; }
restored=0

restore_prod() {
  [ "$restored" = "1" ] && return
  restored=1
  pkill -f "build/apps/ninfer-serve" 2>/dev/null || true
  sleep 3
  sudo -n systemctl start ninfer.service 2>/dev/null || true
  for i in $(seq 1 90); do
    curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1 && break
    sleep 2
  done
  sudo -n systemctl start ninfer-wedge-sentinel.service 2>/dev/null || \
    echo "[$(ts)] WARN: sentinel not restarted — do it manually" | tee -a $LOG
  if curl -sf -m3 http://localhost:8080/health >/dev/null; then
    echo "[$(ts)] prod restored" | tee -a $LOG
  else
    echo "[$(ts)] FATAL: prod did not come up (sentinel STOPPED — restart manually)" | tee -a $LOG
  fi
}
trap restore_prod EXIT

echo "[$(ts)] swift-kvprobe start" | tee -a $LOG
sudo -n systemctl stop ninfer-wedge-sentinel.service 2>/dev/null || true
sudo -n systemctl stop ninfer.service
sleep 3
pkill -f "build/apps/ninfer-serve" 2>/dev/null; sleep 2

FIT=""
VFLAG=""; [ "$VISION" = "1" ] && VFLAG="--vision"
ROPEFLAGS=""; [ "$ROPE_FACTOR" != "1" ] && ROPEFLAGS="--rope-scaling-factor $ROPE_FACTOR --rope-scaling-original-context $ROPE_ORIG"
for ctx in "${CTXS[@]}"; do
  echo "[$(ts)] trying ctx=$ctx ($SPEC, vision=$VISION, rope=$ROPE_FACTOR, kv=$KV)" | tee -a $LOG
  : > $SERVE_LOG
  nohup bash -c "$BIN '$MODEL' --host 0.0.0.0 --port 8085 \
    --default-max-tokens 131072 --pending-timeout-ms 900000 \
    --kv-dtype nvfp4 --spec $SPEC --draft-tokens $DRAFT --lm-head-draft \
    $VFLAG $ROPEFLAGS --host-kv-mib 8192 --device-state-slots 4 --host-state-slots 16 \
    --log-stats-interval-ms 5000 \
    --max-concurrency 1 --max-context $ctx --kv-capacity $KV \
    ; echo \"NINFER_EXIT=\$?\" >> '$SERVE_LOG' 2>&1" >/dev/null 2>&1 &
  up=0
  for i in $(seq 1 60); do
    sleep 3
    if curl -sf -m2 http://localhost:8085/health >/dev/null 2>&1; then up=1; break; fi
    grep -q "NINFER_EXIT=" $SERVE_LOG 2>/dev/null && break
  done
  if [ "$up" = "1" ]; then
    FIT=$ctx
    curl -sf -m10 http://localhost:8085/stats > $STATS_JSON
    echo "[$(ts)] FIT ctx=$ctx — /stats saved to $STATS_JSON" | tee -a $LOG
    break
  else
    err=$(tail -5 $SERVE_LOG 2>/dev/null | tr '\n' ' | ')
    echo "[$(ts)] FAIL ctx=$ctx: $err" | tee -a $LOG
    pkill -f "build/apps/ninfer-serve" 2>/dev/null; sleep 3
  fi
done

echo "swift-kvprobe done: FIT=${FIT:-none}" | tee -a $LOG

# summary for the agent
{
  echo "FIT ctx: ${FIT:-none}"
  if [ -n "$FIT" ] && [ -s $STATS_JSON ]; then
    python3 -c "
import json
d=json.load(open('$STATS_JSON'))
m=d.get('memory',{})
print('kv_capacity tokens:', m.get('kv_capacity'))
print('kv_capacity_page_groups:', m.get('kv_capacity_page_groups'))
print('kv_payload_bytes: %.2f GiB' % (m.get('kv_payload_bytes',0)/2**30))
print('available_after_weights: %.2f GiB' % (m.get('available_after_weights_bytes',0)/2**30))
print('runtime_reservation: %.2f GiB' % (m.get('runtime_reservation_bytes',0)/2**30))
print('kv_capacity_headroom: %.2f GiB' % (m.get('kv_capacity_headroom_bytes',0)/2**30))
print('weights: %.2f GiB' % (m.get('weights_bytes',0)/2**30))
print('workspace: %.2f GiB' % (m.get('workspace_bytes',0)/2**30))
print('workspace_logical_peak: %.2f GiB' % (m.get('workspace_logical_peak_bytes',0)/2**30))
"
  fi
} > $SUMMARY 2>&1
cat $SUMMARY
