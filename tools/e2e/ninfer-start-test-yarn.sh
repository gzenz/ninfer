#!/bin/bash
# e2e/compare server: YARN FORK (~/ninfer-yarn @ Doelfke/ninfer-yarn, merged
# with upstream 9e163eee + YaRN) + official v3 artifact.
#
# CTX=32k  (default): window A config (32k ctx / 64k KV, c=3, 12 GiB host KV)
# CTX=200k:           C1 probe config (262k native ctx, c=2, 30 GiB host KV)
# CTX=420k:           long-context recall probe (420k ctx via YaRN, c=1,
#                     30 GiB host KV, rope-scaling-factor 2)
#
# Fork flag notes (vs upstream start script):
#   - adds --rope-scaling-factor / --rope-scaling-original-context (YaRN)
#   - same v3 flag surface otherwise; --no-thinking for parity with ours
#   - needs the v3 artifact (rejects v2)
set -euo pipefail
CTX="${CTX:-32k}"
MAX_CONTINUATIONS="${MAX_CONTINUATIONS:-18}"
if [ "$CTX" = "200k" ]; then
  HOST_KV_MIB="${HOST_KV_MIB:-30720}"
  DEVICE_STATE_SLOTS="${DEVICE_STATE_SLOTS:-7}"
  HOST_STATE_SLOTS="${HOST_STATE_SLOTS:-112}"
elif [ "$CTX" = "420k" ]; then
  # Small host arena: the recall probe is single-session (c=1, store=False)
  # and needs no host retention. The prod-parity 46 GiB pin (30 GiB host KV +
  # 112 host state) exhausted the 53 GiB host -> std::bad_alloc on the 380k
  # prompt's host buffers. 8 GiB host KV + 16 slots leaves host headroom.
  HOST_KV_MIB="${HOST_KV_MIB:-8192}"
  DEVICE_STATE_SLOTS="${DEVICE_STATE_SLOTS:-7}"
  HOST_STATE_SLOTS="${HOST_STATE_SLOTS:-16}"
else
  HOST_KV_MIB="${HOST_KV_MIB:-12288}"
  DEVICE_STATE_SLOTS="${DEVICE_STATE_SLOTS:-5}"
  HOST_STATE_SLOTS="${HOST_STATE_SLOTS:-128}"
fi

sudo mv /lib/x86_64-linux-gnu/libnvidia-ptxjitcompiler.so.1 /lib/x86_64-linux-gnu/libnvidia-ptxjitcompiler.so.1.disabled 2>/dev/null || true
export PATH=/usr/local/cuda/bin:$HOME/.local/bin:$HOME/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda/lib64:/usr/lib/wsl/lib:${LD_LIBRARY_PATH:-}

# A/B memory-safety: swift (22GB, what prod runs) is lighter than official-v3
# (23GB) and proven to load on this host. Override with MODEL=... as needed.
MODEL="${MODEL:-$HOME/ninfer-models/official-v3/qwen3_8_27b_nvfp4.ninfer}"
LOG=~/ninfer-serve.log

if [ "$CTX" = "200k" ]; then
  CTX_FLAGS="--max-concurrency ${MAX_CONCURRENCY:-2} --max-context 262144 --kv-capacity ${KV_CAPACITY:-auto} --max-private-continuations ${MAX_CONTINUATIONS:-16} --max-shared-prefixes 8"
elif [ "$CTX" = "420k" ]; then
  # Prod-shaped: large max-context (YaRN position ceiling) but the device KV
  # pool sized to the WORKING SET (262k native), not the full context. This is
  # how prod runs 555k max-context: the pool holds the working set, host spill
  # handles overflow. The fork lacks the host-spill net, so this tests the
  # device-only ceiling for a 2-session ~180k working set.
  HOST_KV_MIB="${HOST_KV_MIB:-8192}"
  DEVICE_STATE_SLOTS="${DEVICE_STATE_SLOTS:-7}"
  HOST_STATE_SLOTS="${HOST_STATE_SLOTS:-16}"
  CTX_FLAGS="--max-concurrency 1 --max-context 350000 --kv-capacity 350000 --prefill-chunk 128 --max-private-continuations 16 --max-shared-prefixes 8 --rope-scaling-factor 2 --rope-scaling-original-context 262144"
else
  # 32k: honor MAX_CONCURRENCY (default 3). Run at 1 for the memory-light A/B:
  # serialized GPU work is lighter on host RAM + driver, and the demote/evict
  # decision is device-KV/slot-driven, so c=1 does not change what we measure.
  CTX_FLAGS="--max-concurrency ${MAX_CONCURRENCY:-3} --max-context 32768 --kv-capacity 65536 --max-shared-prefixes 6 --max-private-continuations $MAX_CONTINUATIONS"
fi

# Speculative backend: mtp (default) | dflash2 | none.
#   dflash2 frees MTP's draft weights + paged backend KV pool (cyclic draft) ->
#   more device headroom for long-context prefill, and faster.
#   none disables the draft model entirely: removes the concurrent draft+target
#   D2H/H2D sync. Used for the memory-light A/B (lighter host footprint).
SPEC="${SPEC:-mtp}"
if [ "$SPEC" = "dflash2" ]; then
  SPEC_FLAGS="--spec dflash2 --draft-tokens 7 --lm-head-draft"
elif [ "$SPEC" = "none" ]; then
  # Omit --spec entirely: the CLI rejects "--spec none" (parse_speculative_backend
  # throws), but SpeculativeOptions defaults to backend=None / draft_tokens=0, so
  # no flag = no draft model. Lighter GPU sync + smaller host footprint.
  SPEC_FLAGS=""
else
  SPEC_FLAGS="--spec mtp --draft-tokens 5 --lm-head-draft"
fi

# A/B: run a specific binary instead of the in-tree build (e.g. candidate vs baseline).
BIN="${BIN:-$HOME/ninfer-yarn/build/apps/ninfer-serve}"

pkill -f "build/apps/ninfer-serve" -9 2>/dev/null || true
pkill -f "ninfer-serve-(CANDIDATE|BASELINE)" 2>/dev/null || true
sleep 2
# Shed model-file page cache before the load: the test server's pinned shmem
# (host-KV arena + host-state pool) is non-reclaimable, so reclaimable page
# cache is the host-RAM headroom we can give back before it. This is what the
# 54 GiB host needs to not OOM (the A/B crash root cause).
sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
cd ~/ninfer-yarn
echo "Starting YARN-FORK TEST (ctx=$CTX spec=$SPEC, host-kv=${HOST_KV_MIB}MiB, conts=${MAX_CONTINUATIONS}, dev-state=${DEVICE_STATE_SLOTS}, host-state=${HOST_STATE_SLOTS})"
echo "Log: $LOG"

# Optional full-replacement chat template (froggeric "ninja" jinja). The
# official-v3 artifact's DEFAULT template renders multi-turn conversations
# non-prefix-stably (the seed's token/position stream shifts across turns),
# so the prefix shortlist digest diverges on re-touch and the restore
# candidate is never generated -> re-prefill. CHAT_TEMPLATE overrides it.
CHAT_TEMPLATE="${CHAT_TEMPLATE:-}"
if [ -n "$CHAT_TEMPLATE" ]; then
  CT_FLAGS_EXTRA="--chat-template $CHAT_TEMPLATE"
else
  CT_FLAGS_EXTRA=""
fi

# --preserve-thinking forces the re-touch to retain the open assistant turn as a
# ResponseReplay rewrite checkpoint (byte-stable) instead of TurnClosure (re-render).
# This is the v3 default regression under test: v2's render defaulted preserve_thinking
# to true, v3 to false.
PRESERVE_THINKING="${PRESERVE_THINKING:-0}"
if [ "$PRESERVE_THINKING" = "1" ]; then
  CT_FLAGS_EXTRA="$CT_FLAGS_EXTRA --preserve-thinking"
fi

# Pass through instrumentation env vars explicitly so they reach the server process
# (rely on this rather than ambient env inheritance).
# Only non-empty names: `std::getenv` returns non-NULL for an EMPTY string, so exporting
# `NINFER_MAT_DEBUG=""` switched every MAT_* probe ON in every run (fixed 2026-09-25).
for _mat_name in NINFER_MAT_DEBUG NINFER_MAT_GRID NINFER_MAT_TAIL NINFER_MAT_FRONT NINFER_MAT_FINE; do
  _mat_value="${!_mat_name:-}"
  if [ -n "$_mat_value" ]; then export "$_mat_name=$_mat_value"; else unset "$_mat_name"; fi
done
unset _mat_name _mat_value

nohup bash -c '"$BIN" "$1" \
  --host 0.0.0.0 --port "${PORT:-8080}" \
  --default-max-tokens 131072 --pending-timeout-ms 900000 \
  --kv-dtype nvfp4 '"$SPEC_FLAGS"' \
  --no-thinking --host-kv-mib '"$HOST_KV_MIB"' \
  --device-state-slots '"$DEVICE_STATE_SLOTS"' --host-state-slots '"$HOST_STATE_SLOTS"' \
  --temperature 1.0 --top-p 0.95 --top-k 20 \
  --request-log-jsonl ~/ninfer-requests.jsonl --log-stats-interval-ms 5000 \
  '"$CTX_FLAGS"' '"$CT_FLAGS_EXTRA"' \
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
