#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# ab-runner.sh — reliable A/B runner for the NInfer yarn fork (V3).
#
# WHY THIS EXISTS
#   Ad-hoc A/B runs kept misbehaving: the test server could start more than
#   once (a nohup'd server lingers after the start script's health timeout and
#   then coexists with restored prod), prod was not always restored on the
#   failure path, and there was no signal to resume the session. This runner
#   makes the procedure airtight.
#
# GUARANTEES
#   (a) the test server is started EXACTLY ONCE (single call to the start
#       script; the runner kills it on every exit path, so at most one exists).
#   (b) prod (8080) is restored on EVERY exit path — success, config error,
#       timeout, workload failure, SIGINT/SIGTERM — via a trap.
#   (c) a distinct sentinel line is appended to /tmp/ninfer-ab-sentinel.log on
#       completion so a Claude Monitor can watch for it and resume the session.
#
# MUST run as ONE blocking foreground command: this session runs on prod; the
# test server is full-context and would serve session traffic if it bound 8080,
# so it uses TEST_PORT and the session is frozen for the whole run.
#
#   BIN=/home/zenz/ninfer-serve-CANDIDATE TAG=ab-cand CTX=32k \
#     HOST_KV_MIB=6144 HOST_STATE_SLOTS=32 SPEC=none \
#     START=$HOME/ninfer/tools/e2e/ninfer-start-test-yarn.sh \
#     WORKLOAD="$HOME/ninfer/tools/e2e/cmp-e2e.py --profile trash" \
#     TRASH_SESSIONS=6 TRASH_SEED=15000 TRASH_TURN=1500 TRASH_ROUNDS=3 TRASH_OUT=48 \
#     bash $HOME/ninfer/tools/e2e/ab-runner.sh
#
# After it returns (or a Monitor fires on the sentinel), read:
#   /tmp/ninfer-ab-serve-$TAG.log   serve log (restore signature: cand>0/reused_tok>0)
#   /tmp/ninfer-ab-run-$TAG.log     workload output
#   /tmp/ninfer-cmp-$TAG.json       cmp-e2e results (if the workload ran)
set -uo pipefail

# ---------- required config ----------
BIN="${BIN:?BIN (binary to test) required}"
TAG="${TAG:?TAG required (unique per run)}"
START="${START:-$SCRIPT_DIR/ninfer-start-test.sh}"
WORKLOAD="${WORKLOAD:-$SCRIPT_DIR/cmp-e2e.py --profile trash}"
TEST_PORT="${TEST_PORT:-8085}"
SHMEM_CAP_GB="${SHMEM_CAP_GB:-40}"

RUNNER_LOG="/tmp/ninfer-ab-runner-$TAG.log"
SENTINEL_LOG="/tmp/ninfer-ab-sentinel.log"
rm -f "$RUNNER_LOG"; : >> "$SENTINEL_LOG"

ts(){ date '+%H:%M:%S'; }
log(){ echo "[$(ts)] $*" | tee -a "$RUNNER_LOG"; }

# ---------- (c) sentinel: one line the Claude Monitor watches for ----------
emit_sentinel(){
  # $1 = status: success | error:<reason> | interrupt
  echo "AB_DONE tag=$TAG status=$1 $(date '+%Y-%m-%dT%H:%M:%S%z')" >> "$SENTINEL_LOG"
}

# ---------- (b) restore prod; idempotent, safe to call repeatedly ----------
restore_prod(){
  pkill -f "ninfer-serve-CANDIDATE" 2>/dev/null
  pkill -f "ninfer-serve-BASELINE"  2>/dev/null
  pkill -f "build/apps/ninfer-serve" 2>/dev/null
  sleep 3
  if ! curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1; then
    log "RESTORE: prod down -> starting ninfer.service"
    sudo -n systemctl start ninfer.service 2>/dev/null || log "RESTORE: WARN systemctl start failed"
    local i
    for i in $(seq 1 120); do
      curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1 && break
      sleep 2
    done
    sudo -n systemctl start ninfer-wedge-sentinel.service 2>/dev/null || true
  fi
  if curl -sf -m3 http://localhost:8080/health >/dev/null 2>&1; then
    log "RESTORE: prod back up on 8080"
  else
    log "RESTORE: FATAL prod did NOT come up — manual restart: sudo -n systemctl start ninfer.service"
  fi
}

# ---------- finish: restore + emit sentinel exactly once ----------
_done=0
finish(){
  local status="$1"
  [ "$_done" = "1" ] && return
  _done=1
  restore_prod
  emit_sentinel "$status"
  log "RUNNER DONE status=$status"
}
trap 'finish "error:exit:$?"' EXIT
trap 'finish "interrupt"' INT TERM

log "AB-RUNNER start TAG=$TAG BIN=$BIN CTX=${CTX:-32k} host-kv=${HOST_KV_MIB:-} host-slots=${HOST_STATE_SLOTS:-} spec=${SPEC:-mtp}"

# ---------- 1. stop prod (sentinel FIRST), verify it is actually down ----------
sudo -n systemctl stop ninfer-wedge-sentinel.service 2>/dev/null || true
sudo -n systemctl stop ninfer.service
sleep 3
pkill -f "build/apps/ninfer-serve" 2>/dev/null
pkill -f "ninfer-serve-CANDIDATE\|ninfer-serve-BASELINE" 2>/dev/null
sleep 2
for i in $(seq 1 30); do
  curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1 || break
  sleep 1
done
if curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1; then
  log "FATAL: prod still up on 8080 — refusing to run two servers"; finish "error:prod-not-stopped"; exit 1
fi
log "prod down, 8080 free"

# ---------- 2. wait for host + GPU memory release ----------
gpu_used(){ nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null | head -1; }
mem_avail_gb(){ awk '/MemAvailable/{printf "%d",$2/1024/1024}' /proc/meminfo; }
rel=0
for i in $(seq 1 90); do
  g=$(gpu_used); a=$(mem_avail_gb)
  if [ -n "$g" ] && [ "$g" -lt 1500 ] 2>/dev/null && [ -n "$a" ] && [ "$a" -ge 38 ]; then rel=1; break; fi
  sleep 2
done
if [ "$rel" = "1" ]; then log "memory released (gpu=$(gpu_used)MiB mem_avail=$(mem_avail_gb)GiB)"
else log "FATAL: memory not released (gpu=$(gpu_used)MiB mem_avail=$(mem_avail_gb)GiB)"; finish "error:memory-not-released"; exit 1; fi

# ---------- 3. shmem guard: kill the test server before it OOMs the VM ----------
( peak=0
  trap 'echo "[$(ts)] SHMEM peak: ${peak}kB = $((peak/1024/1024))GiB" >> "$RUNNER_LOG"' TERM
  while :; do
    s=$(awk '/^Shmem:/{print $2}' /proc/meminfo 2>/dev/null)
    [ -n "$s" ] && [ "$s" -gt "$peak" ] 2>/dev/null && peak=$s
    if [ -n "$s" ] && [ "$s" -gt $((SHMEM_CAP_GB*1024*1024)) ] 2>/dev/null; then
      echo "[$(ts)] SHMEM GUARD: shmem ${s}kB > ${SHMEM_CAP_GB}GiB cap — killing test server" | tee -a "$RUNNER_LOG"
      pkill -f "ninfer-serve-CANDIDATE" 2>/dev/null; pkill -f "ninfer-serve-BASELINE" 2>/dev/null; pkill -f "build/apps/ninfer-serve" 2>/dev/null
      exit 0
    fi
    sleep 2
  done ) &
GUARD_PID=$!

# ---------- 4. start the test server EXACTLY ONCE ----------
CTX="${CTX:-32k}" PORT="$TEST_PORT" BIN="$BIN" bash "$START" >> "$RUNNER_LOG" 2>&1
# The start script nohups one server and may exit early on its own health
# timeout while the server keeps loading; we do our own health check below.

# ---------- 5. wait for the test server on TEST_PORT ----------
up=0
for i in $(seq 1 150); do
  curl -sf -m2 "http://localhost:$TEST_PORT/health" >/dev/null 2>&1 && { up=1; break; }
  sleep 2
done
kill "$GUARD_PID" 2>/dev/null || true

if [ "$up" = "1" ]; then
  log "test server up on $TEST_PORT — running workload"
  # Route per-workload output flags: the workload must hit the TEST server
  # ($TEST_PORT), NOT prod's default 8080 (which we just stopped).
  case "$WORKLOAD" in
    *c1-probe.py*) WORKLOAD="$WORKLOAD --json /tmp/ninfer-c1-$TAG.json --port $TEST_PORT" ;;
    *cmp-e2e.py*)  WORKLOAD="$WORKLOAD --tag $TAG --port $TEST_PORT" ;;
  esac
  : > ~/ninfer-requests.jsonl 2>/dev/null || true
  python3 -u $WORKLOAD > /tmp/ninfer-ab-run-$TAG.log 2>&1
  RC=$?
  cp ~/ninfer-serve.log /tmp/ninfer-ab-serve-$TAG.log 2>/dev/null || true
  # cmp-e2e.py exits 0 even on connection errors (n_turns=0); a run that drove
  # zero turns is a failure, not a success.
  TURNS=0
  if [ -f "/tmp/ninfer-cmp-$TAG.json" ]; then
    TURNS=$(python3 -c "import json;print(sum(r.get('n_turns',0) for r in json.load(open('/tmp/ninfer-cmp-$TAG.json')).get('results',[])))" 2>/dev/null || echo 0)
  fi
  if [ "$TURNS" -eq 0 ] 2>/dev/null; then
    tail -8 /tmp/ninfer-ab-run-$TAG.log >> "$RUNNER_LOG" 2>/dev/null
    finish "error:workload-zero-turns"
  elif [ "$RC" -eq 0 ]; then
    finish "success"
  else
    finish "error:workload-rc:$RC"
  fi
else
  tail -6 ~/ninfer-serve.log 2>/dev/null | grep -vE '^\[fine\]|^\[grid\]' >> "$RUNNER_LOG"
  log "FATAL: test server not ready on $TEST_PORT — see $RUNNER_LOG"; finish "error:server-not-ready"
fi
exit 0
