#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# A/B comparison swap: stop prod -> start the build under test -> run the
# comparison workload -> restart prod.
# MUST run as ONE blocking foreground command (this session runs on prod;
# the e2e server 400s on its prompts, so the session is frozen for the swap).
#
#   bash tools/e2e/e2e-swap-cmp.sh
#     -> OURS + official v2 artifact, window A (focused+p13+p14, 32k ctx)
#   START=tools/e2e/ninfer-start-test-upstream.sh bash tools/e2e/e2e-swap-cmp.sh
#     -> UPSTREAM + official v3 artifact, window A
#   CTX=200k WORKLOAD=tools/e2e/c1-probe.py bash tools/e2e/e2e-swap-cmp.sh
#     -> C1 probe (2x200k anthropic, prod-parity sizing)
#
# Env:
#   START      start script (default: ours + froggeric)
#   PROFILE    cmp-e2e profile: focused|p13|all (default all)
#   CTX        32k (default) | 200k
#   WORKLOAD   python workload (default: cmp-e2e.py --profile $PROFILE)
#
# Outputs (per run, keyed by TAG = <start-script-basename>-ctx<CTX>):
#   /tmp/ninfer-cmp-<TAG>.json        cmp-e2e results
#   /tmp/ninfer-c1-<TAG>.json         c1-probe results
#   /tmp/ninfer-cmp-serve-<TAG>.log   serve log snapshot (decode rates etc.)
#   /tmp/ninfer-cmp-run.log           workload stdout (overwritten per run)
set -uo pipefail
LOG=/tmp/ninfer-cmp-swap.log
RUN_LOG=/tmp/ninfer-cmp-run.log
START="${START:-$SCRIPT_DIR/ninfer-start-test.sh}"
PROFILE="${PROFILE:-all}"
CTX="${CTX:-32k}"
# Test server port. MUST NOT be 8080: 8080 is prod + this session's LLM
# endpoint (LLM calls and the Bash classifier both route to 8080). If the test
# server bound 8080, session traffic would land in the test arena and taint
# the run. Prod stays on 8080; the test server + workload use TEST_PORT.
TEST_PORT="${TEST_PORT:-8085}"
# A/B: tag override so candidate vs baseline runs don't clobber each other's JSON.
TAG="${TAG:-$(basename "$START" .sh | sed 's/^ninfer-start-test-//')-ctx$CTX}"
WORKLOAD="${WORKLOAD:-$SCRIPT_DIR/cmp-e2e.py --profile $PROFILE}"
# route per-workload output flags
case "$WORKLOAD" in
  *c1-probe.py*) WORKLOAD="$WORKLOAD --json /tmp/ninfer-c1-$TAG.json --port $TEST_PORT" ;;
  *cmp-e2e.py*)  WORKLOAD="$WORKLOAD --tag $TAG --port $TEST_PORT" ;;
esac
ts(){ date '+%H:%M:%S'; }
# prod tears down asynchronously: its ~22GB VRAM model + 30GB pinned host KV are
# not released until the process is gone AND the kernel reclaims the pinned pages.
# With prod up, MemAvailable is only ~19GiB; starting the test server (a ~23GB
# model-load host peak) before that releases OOMs WSL2. Wait until BOTH the GPU is
# free AND there is enough host headroom for the next model load. Mirrors
# batched-window.sh's wait_gpu_free, extended with a host-memory gate.
gpu_used(){ nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null | head -1; }
mem_avail_gb(){ awk '/MemAvailable/{printf "%d", $2/1024/1024}' /proc/meminfo; }
wait_release(){
  for i in $(seq 1 90); do
    local g a; g=$(gpu_used); a=$(mem_avail_gb)
    if { [ -n "$g" ] && [ "$g" -lt 1500 ] 2>/dev/null; } && [ -n "$a" ] && [ "$a" -ge 38 ]; then
      echo "[$(ts)] memory released (gpu=${g}MiB mem_avail=${a}GiB)"; return 0
    fi
    sleep 2
  done
  echo "[$(ts)] WARN: memory not fully released (gpu=$(gpu_used)MiB mem_avail=$(mem_avail_gb)GiB)"
  return 1
}
# Restore prod on ANY exit path, incl. SIGINT/SIGTERM (Ctrl+C, session drop).
# Idempotent: if prod is already healthy, just make sure the sentinel is up.
# (A hard SIGKILL from the Bash-tool timeout, or a WSL crash, cannot be trapped —
# keep the tool timeout generous so the swap finishes before it can fire.)
restore_prod(){
  if curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1; then
    sudo -n systemctl start ninfer-wedge-sentinel.service 2>/dev/null || true
    return
  fi
  echo "[$(ts)] RESTORE trap: prod down — stopping test server, starting prod" | tee -a $LOG
  pkill -f "build/apps/ninfer-serve" 2>/dev/null || true
  pkill -f "ninfer-serve-CANDIDATE\|ninfer-serve-BASELINE" 2>/dev/null || true
  sleep 3
  sudo -n systemctl start ninfer.service 2>/dev/null || echo "[$(ts)] WARN: systemctl start ninfer failed"
  for i in $(seq 1 90); do
    curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1 && break
    sleep 2
  done
  sudo -n systemctl start ninfer-wedge-sentinel.service 2>/dev/null || echo "[$(ts)] WARN: sentinel not restarted"
}
trap restore_prod EXIT INT TERM
echo "[$(ts)] cmp-swap start (TAG=$TAG START=$START PROFILE=$PROFILE CTX=$CTX)" | tee -a $LOG

# 1. stop prod (same order as e2e-swap.sh: sentinel FIRST)
sudo -n systemctl stop ninfer-wedge-sentinel.service 2>/dev/null || true
sudo -n systemctl stop ninfer.service
sleep 3
pkill -f "build/apps/ninfer-serve" 2>/dev/null
pkill -f "ninfer-serve-CANDIDATE\|ninfer-serve-BASELINE" 2>/dev/null; sleep 2

# Gate: do NOT start the test server until prod's VRAM + pinned host KV are released,
# else the next model-load peak overlaps the residual and OOMs WSL2. If it never
# frees, abort and restore prod rather than crash the host.
if ! wait_release; then
  sudo -n systemctl start ninfer.service
  sudo -n systemctl start ninfer-wedge-sentinel.service 2>/dev/null || \
    echo "[$(ts)] WARN: sentinel not restarted — do it manually"
  echo "[$(ts)] FATAL: memory not released before test-server load — aborting to avoid WSL OOM, prod restored"
  exit 1
fi

# 2. start the build under test
CTX="$CTX" PORT="$TEST_PORT" bash "$START" >> $LOG 2>&1
for i in $(seq 1 90); do
  curl -sf -m2 http://localhost:$TEST_PORT/health >/dev/null 2>&1 && break
  sleep 2
done
if ! curl -sf -m3 http://localhost:$TEST_PORT/health >/dev/null; then
  # FATAL: test server did not come up — restore prod before exiting so a
  # failed window never leaves prod down.
  pkill -f "build/apps/ninfer-serve" 2>/dev/null; sleep 3
  sudo -n systemctl start ninfer.service
  for i in $(seq 1 90); do
    curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1 && break
    sleep 2
  done
  sudo -n systemctl start ninfer-wedge-sentinel.service 2>/dev/null || true
  echo "[$(ts)] FATAL: test server did not come up — prod restored (check $LOG)"; exit 1
fi
echo "[$(ts)] test server up" | tee -a $LOG

# Safety net: WSL2 cgroup MemoryMax is NOT enforced on this host (verified: a
# 4GiB process survived a 1G scope). Guard host OOM explicitly instead: host
# shmem is the non-reclaimable pinned KV/state memory; if it crosses
# SHMEM_CAP_GB, kill the test server so a mis-tune OOMs the test process (and
# fails the run cleanly, prod restored) instead of OOMing the WSL2 VM and
# rebooting the host (the 09-21 A/B crash root cause).
SHMEM_CAP_GB="${SHMEM_CAP_GB:-40}"
( peak=0
  trap 'echo "[$(ts)] SHMEM peak during run: ${peak}kB = $((peak/1024/1024))GiB (cap ${SHMEM_CAP_GB}GiB)" | tee -a $LOG' TERM
  while :; do
    s=$(awk '/^Shmem:/{print $2}' /proc/meminfo 2>/dev/null)
    [ -n "$s" ] && [ "$s" -gt "$peak" ] 2>/dev/null && peak=$s
    if [ -n "$s" ] && [ "$s" -gt $((SHMEM_CAP_GB * 1024 * 1024)) ] 2>/dev/null; then
      echo "[$(ts)] SHMEM GUARD: shmem ${s}kB > ${SHMEM_CAP_GB}GiB cap (peak ${peak}kB) — killing test server" | tee -a $LOG
      pkill -f "build/apps/ninfer-serve" 2>/dev/null
      pkill -f "ninfer-serve-CANDIDATE\|ninfer-serve-BASELINE" 2>/dev/null
      exit 0
    fi
    sleep 2
  done ) &
GUARD_PID=$!

# 3. run the workload
#    Truncate the request log so the workload's records are the only ones
#    (the server appends; ours rotates, upstream does not).
: > ~/ninfer-requests.jsonl 2>/dev/null || true
python3 -u $WORKLOAD > $RUN_LOG 2>&1
RC=$?
kill "$GUARD_PID" 2>/dev/null || true
# snapshot the serve log BEFORE the next window overwrites it
cp ~/ninfer-serve.log /tmp/ninfer-cmp-serve-$TAG.log 2>/dev/null || true
# Persist results outside /tmp: a WSL reboot wipes /tmp (lost the 09-19 A/B
# JSONs this way).
mkdir -p $HOME/ninfer/results
cp /tmp/ninfer-cmp-$TAG.json $HOME/ninfer/results/ 2>/dev/null || true
cp /tmp/ninfer-c1-$TAG.json $HOME/ninfer/results/ 2>/dev/null || true
echo "[$(ts)] workload rc=$RC" | tee -a $LOG
tail -40 $RUN_LOG | tee -a $LOG

# 4. stop test server, restart prod
pkill -f "build/apps/ninfer-serve" 2>/dev/null
pkill -f "ninfer-serve-CANDIDATE\|ninfer-serve-BASELINE" 2>/dev/null; sleep 3
sudo -n systemctl start ninfer.service
for i in $(seq 1 90); do
  curl -sf -m2 http://localhost:8080/health >/dev/null 2>&1 && break
  sleep 2
done
if curl -sf -m3 http://localhost:8080/health >/dev/null; then
  sudo -n systemctl start ninfer-wedge-sentinel.service 2>/dev/null || \
    echo "[$(ts)] WARN: could not restart ninfer-wedge-sentinel — do it manually"
  echo "[$(ts)] prod restored" | tee -a $LOG
else
  echo "[$(ts)] FATAL: prod did not come up (sentinel is STOPPED — restart it manually: sudo systemctl start ninfer-wedge-sentinel.service)"; exit 1
fi
echo "[$(ts)] cmp-swap done (rc=$RC)" | tee -a $LOG
exit $RC
