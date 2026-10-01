#!/bin/bash
# Swift 350k recall+speed — ONE blocking foreground window (clean procedure).
# Usage: bash swift-recall350.sh <tag> <spec> <draft> <vision 0|1>
#   e.g. bash swift-recall350.sh dflash2 dflash2 7 1
# Stops prod, boots the Swift artifact on :8085 (350k, YaRN x2, nvfp4 KV),
# runs the 340k recall probe + captures speed, restores prod.
# The script self-cleans at start (pkill + stop prod) so a prior orphan
# Swift server cannot hold the GPU.
set -u
TAG=$1; SPEC=$2; DRAFT=$3; VISION=$4
LOG=/tmp/ninfer-cmp-swap.log
MODEL=~/ninfer-models/swift15/qwen3_8_27b_nvfp4swift15.ninfer
BIN=~/ninfer-yarn/build/apps/ninfer-serve
SERVE_LOG=~/ninfer-serve.log
JSON=/tmp/ninfer-swift-recall-$TAG.json
ts(){ date '+%H:%M:%S'; }
restored=0
restore_prod(){ [ "$restored" = 1 ] && return; restored=1
  pkill -f "build/apps/ninfer-serve" 2>/dev/null || true; sleep 3
  sudo -n systemctl start ninfer.service 2>/dev/null || true
  for i in $(seq 1 90); do curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1 && break; sleep 2; done
  sudo -n systemctl start ninfer-wedge-sentinel.service 2>/dev/null || \
    echo "[$(ts)] WARN: sentinel not restarted — do it manually" | tee -a $LOG
  if curl -sf -m3 http://localhost:8080/health >/dev/null; then
    echo "[$(ts)] prod restored" | tee -a $LOG
  else
    echo "[$(ts)] FATAL: prod did not come up (sentinel STOPPED — restart manually)" | tee -a $LOG
  fi
}
trap restore_prod EXIT

echo "[$(ts)] swift-recall350 start tag=$TAG spec=$SPEC vision=$VISION" | tee -a $LOG
sudo -n systemctl stop ninfer-wedge-sentinel.service 2>/dev/null || true
sudo -n systemctl stop ninfer.service; sleep 3
pkill -f "build/apps/ninfer-serve" 2>/dev/null; sleep 2

VFLAG=""; [ "$VISION" = 1 ] && VFLAG="--vision --media-cache-mib 512 --media-live-mib 512"
: > $SERVE_LOG
nohup bash -c "$BIN '$MODEL' --host 0.0.0.0 --port 8085 \
  --default-max-tokens 131072 --pending-timeout-ms 900000 \
  --kv-dtype nvfp4 --spec $SPEC --draft-tokens $DRAFT --lm-head-draft \
  $VFLAG --host-kv-mib 8192 --device-state-slots 4 --host-state-slots 16 \
  --log-stats-interval-ms 5000 --max-concurrency 1 --max-context 350000 --kv-capacity 350000 \
  --prefill-chunk 128 --rope-scaling-factor 2 --rope-scaling-original-context 262144 \
  ; echo \"NINFER_EXIT=\$?\" >> '$SERVE_LOG' 2>&1" >/dev/null 2>&1 &

up=0
for i in $(seq 1 60); do sleep 3
  curl -sf -m2 http://localhost:8085/health >/dev/null 2>&1 && { up=1; break; }
  grep -q "NINFER_EXIT=" $SERVE_LOG 2>/dev/null && break
done
if [ "$up" != 1 ]; then
  echo "[$(ts)] BOOT FAIL $TAG: $(tail -3 $SERVE_LOG 2>/dev/null | tr '\n' ' | ')" | tee -a $LOG
  exit 0
fi
echo "[$(ts)] BOOT OK $TAG" | tee -a $LOG
curl -sf -m10 http://localhost:8085/stats > /tmp/ninfer-swift-recall-$TAG-stats.json
python3 -c "
import json
m=json.load(open('/tmp/ninfer-swift-recall-$TAG-stats.json')).get('memory',{})
print('  free VRAM: %.2f GiB | weights %.2f GiB | kv %s tok' % (
  m.get('available_after_startup_bytes',0)/2**30, m.get('weights_bytes',0)/2**30, m.get('kv_capacity')))
" | tee -a $LOG

echo "[$(ts)] recall probe $TAG (340k target)..." | tee -a $LOG
python3 ~/ninfer/tools/longctx_recall_probe.py --target-tokens 340000 --port 8085 \
  --timeout 900 --json $JSON 2>&1 | tail -10 | tee -a $LOG
echo "[$(ts)] speed lines $TAG:" | tee -a $LOG
grep -E "ttft=|decode=" $SERVE_LOG | tail -4 | tee -a $LOG
echo "[$(ts)] swift-recall350 done tag=$TAG" | tee -a $LOG
