#!/bin/bash
# ninfer-start-test.sh - Small-ctx test server for fast e2e cache-eviction iteration.
# 32k context, 64k KV capacity (2 sessions fit, 3rd forces demotion/eviction).
#
# HOST_KV_MIB controls the pressure mode:
#   4096 (default): demote-capable. Planner demotes LRU continuations to host
#        and restores them (H2D). Exercises the catalog demote/restore path.
#    512: eviction-forcing. Host KV too small to demote even one session, so
#        all pressure becomes eviction -> safety-net spills -> next turns must
#        restore via the safety net (checkpoint fallback). Exercises the
#        checkpoint-restore port directly.
set -euo pipefail
HOST_KV_MIB="${HOST_KV_MIB:-4096}"
export HOST_KV_MIB
# Private continuation slots. 3 sessions fill 18 slots comfortably; smaller
# values (e.g. 3) force catalog-slot-pressure EVICTIONS with arena room to
# spare — the production eviction trigger — so spills succeed and the
# safety-net checkpoint restore path is exercised.
MAX_CONTINUATIONS="${MAX_CONTINUATIONS:-18}"
export MAX_CONTINUATIONS

sudo mv /lib/x86_64-linux-gnu/libnvidia-ptxjitcompiler.so.1 /lib/x86_64-linux-gnu/libnvidia-ptxjitcompiler.so.1.disabled 2>/dev/null || true

export PATH=/usr/local/cuda/bin:$HOME/.local/bin:$HOME/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda/lib64:/usr/lib/wsl/lib:${LD_LIBRARY_PATH:-}

MODEL=~/ninfer-models/qwen3_8_27b_nvfp4-froggeric.ninfer
LOG=~/ninfer-serve.log

pkill -f "build/apps/ninfer-serve" -9 2>/dev/null || true
sleep 2
cd ~/ninfer
echo "Starting NInfer TEST (c=3, 32k ctx, 64k KV capacity, host-kv=${HOST_KV_MIB}MiB, conts=${MAX_CONTINUATIONS})"
echo "Log: $LOG"

nohup bash -c './build/apps/ninfer-serve "$1" \
  --host 0.0.0.0 --port 8080 \
  --max-concurrency 3 --max-context 32768 --kv-capacity 65536 \
  --default-max-tokens 131072 --pending-timeout-ms 900000 \
  --kv-dtype nvfp4 --spec mtp --draft-tokens 5 --lm-head-draft \
  --tolerant-tool-calls --host-kv-mib $HOST_KV_MIB --max-shared-prefixes 6 --max-private-continuations $MAX_CONTINUATIONS \
  --temperature 1.0 --top-p 0.95 --top-k 20 \
  --rope-scaling-factor 2.12 --rope-scaling-original-context 262144 \
  --request-log-jsonl ~/ninfer-requests.jsonl --request-log-max-mib 64 --request-log-keep 4 \
  ; echo "NINFER_EXIT=$?" >> '"$LOG"' 2>&1' _ "$MODEL" \
  > "$LOG" 2>&1 &
echo $! > ~/ninfer-test.pid

for i in $(seq 1 60); do
  sleep 3
  if curl -sf http://localhost:8080/health > /dev/null 2>&1; then
    echo "Test server ready on 8080 (pid $(cat ~/ninfer-test.pid))"
    exit 0
  fi
done
echo "WARNING: test server did not become ready in 3 min. Check $LOG"
exit 1
