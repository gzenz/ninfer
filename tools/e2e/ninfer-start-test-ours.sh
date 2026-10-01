#!/bin/bash
# e2e/compare server: OURS (~/ninfer, docs/prune-plan binary) + froggeric artifact.
# NOTE: the official v2 artifact (rev 52907138a5d2) does NOT load on our v2
# tree — "tensor descriptor does not match target contract: text/token_embedding"
# (official uses FP8 row-scale embedding + Q4/Q5 text groups; our 27b package
# contract pins different layouts). Froggeric (NVFP4, converted from
# unsloth/Qwen3.8-27B-NVFP4) is the known-good artifact for our binary —
# same backbone model as the official artifact, different quant source
# (weights confound on decode speed; C1 cache behavior unaffected).
#
# CTX=32k (default): window A config (32k ctx / 64k KV, c=3, 12 GiB host KV)
# CTX=200k:          C1 probe config (prod parity: 262k ctx, c=2, 30 GiB host KV)
set -euo pipefail
CTX="${CTX:-32k}"
MAX_CONTINUATIONS="${MAX_CONTINUATIONS:-18}"
if [ "$CTX" = "200k" ]; then
  # prod parity: 30 GiB host KV, 7 device state slots, 112 host state slots
  HOST_KV_MIB="${HOST_KV_MIB:-30720}"
  DEVICE_STATE_SLOTS="${DEVICE_STATE_SLOTS:-7}"
  HOST_STATE_SLOTS="${HOST_STATE_SLOTS:-112}"
else
  HOST_KV_MIB="${HOST_KV_MIB:-12288}"
  DEVICE_STATE_SLOTS="${DEVICE_STATE_SLOTS:-5}"
  HOST_STATE_SLOTS="${HOST_STATE_SLOTS:-128}"
fi

sudo mv /lib/x86_64-linux-gnu/libnvidia-ptxjitcompiler.so.1 /lib/x86_64-linux-gnu/libnvidia-ptxjitcompiler.so.1.disabled 2>/dev/null || true
export PATH=/usr/local/cuda/bin:$HOME/.local/bin:$HOME/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda/lib64:/usr/lib/wsl/lib:${LD_LIBRARY_PATH:-}

MODEL=~/ninfer-models/qwen3_8_27b_nvfp4-froggeric.ninfer
LOG=~/ninfer-serve.log

if [ "$CTX" = "200k" ]; then
  # KV_CAPACITY: pin the device KV pool for A/B parity (default auto). Upstream
  # auto-sized to 5487 pages (~262k tokens) with the official artifact; ours
  # auto-sized to ~7800 pages (~373k) with the lighter froggeric weights —
  # pin to 262144 for a same-capacity comparison.
  CTX_FLAGS="--max-concurrency ${MAX_CONCURRENCY:-2} --max-context 262144 --kv-capacity ${KV_CAPACITY:-auto} --max-private-continuations ${MAX_CONTINUATIONS:-16} --max-shared-prefixes 8"
else
  CTX_FLAGS="--max-concurrency 3 --max-context 32768 --kv-capacity 65536 --max-shared-prefixes 6 --max-private-continuations $MAX_CONTINUATIONS"
fi
# YaRN rope scaling: part of the proven e2e/prod config for this artifact
# (no-op below the original context, but keep parity with the known-good harness)
CTX_FLAGS="$CTX_FLAGS --rope-scaling-factor 2.12 --rope-scaling-original-context 262144"

pkill -f "build/apps/ninfer-serve" -9 2>/dev/null || true
sleep 2
cd ~/ninfer
echo "Starting OURS TEST (ctx=$CTX, host-kv=${HOST_KV_MIB}MiB, conts=${MAX_CONTINUATIONS}, dev-state=${DEVICE_STATE_SLOTS}, host-state=${HOST_STATE_SLOTS})"
echo "Log: $LOG"

nohup bash -c './build/apps/ninfer-serve "$1" \
  --host 0.0.0.0 --port "${PORT:-8080}" \
  --default-max-tokens 131072 --pending-timeout-ms 900000 \
  --kv-dtype nvfp4 --spec mtp --draft-tokens 5 --lm-head-draft \
  --tolerant-tool-calls --host-kv-mib '"$HOST_KV_MIB"' \
  --device-state-slots '"$DEVICE_STATE_SLOTS"' --host-state-slots '"$HOST_STATE_SLOTS"' \
  --temperature 1.0 --top-p 0.95 --top-k 20 \
  --request-log-jsonl ~/ninfer-requests.jsonl --request-log-max-mib 64 --request-log-keep 4 \
  '"$CTX_FLAGS"' \
  ; echo "NINFER_EXIT=$?" >> '"$LOG"' 2>&1' _ "$MODEL" \
  > "$LOG" 2>&1 &
echo $! > ~/ninfer-test.pid

for i in $(seq 1 60); do
  sleep 3
  if curl -sf "http://localhost:${PORT:-8080}/health" > /dev/null 2>&1; then
    echo "Test server ready on ${PORT:-8080} (pid $(cat ~/ninfer-test.pid))"
    exit 0
  fi
done
echo "WARNING: test server did not become ready in 3 min. Check $LOG"
exit 1
