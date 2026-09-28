#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# One-shot e2e swap: stop prod -> 32k e2e server -> run e2e -> restart prod.
# MUST run as ONE blocking foreground command (this session runs on prod;
# the 32k e2e server 400s on its prompts, so the session is frozen for the
# whole swap).
#
#   bash ~/ninfer-e2e/e2e-swap.sh                 # demote-capable (12 GiB host KV)
#   E2E_HOST_KV_MIB=512 bash ~/ninfer-e2e/e2e-swap.sh   # eviction-forcing
set -uo pipefail
LOG=/tmp/ninfer-e2e-swap.log
# Per-run suite log: a fixed name was overwritten by every swap, so the raw stdout of an earlier run
# (the only record that can distinguish two configurations whose result JSON was copied over each
# other) was gone by the time it was needed. The stable path stays as a symlink to the latest.
RUN_LOG="/tmp/ninfer-e2e-run-$(date +%s).log"
ln -sf "$RUN_LOG" /tmp/ninfer-e2e-run.log
ts(){ date '+%H:%M:%S'; }

# One swap at a time. Two interleaved swaps stop each other's servers and leave prod down with the
# sentinel stopped (2026-09-24 20:47/20:51: a second swap started while the first was still running
# its suite; the log then shows two "e2e server up" lines and no "prod restored").
exec 9>/tmp/ninfer-e2e-swap.lock
if ! flock -n 9; then
  echo "[$(ts)] another swap holds /tmp/ninfer-e2e-swap.lock — aborting (holder: $(fuser /tmp/ninfer-e2e-swap.lock 2>&1 | tr -s ' ') )" | tee -a $LOG
  exit 1
fi

# Restore prod on ANY exit, including a signal: the swap is a blocking foreground command under a
# 10-minute tool limit, so a SIGTERM/SIGINT (tool timeout, Ctrl-C) is a live scenario, and without
# this trap it leaves prod AND the sentinel down. `restore_prod` is idempotent, so the normal path's
# own call wins and the trap becomes a no-op.
# Split on purpose: a trap on INT/TERM runs the handler and then the script CONTINUES (reproduced
# with a mock: after `kill -TERM` the handler ran and the body kept going). That matters here because
# restore_prod is idempotent -- it would start prod, the script would resume, start the test server
# over it (pkill), run the suite, and the final restore would be a no-op, leaving prod down. So the
# signal handlers restore and then EXIT; only EXIT uses the plain form.
# Flags BEFORE the traps: with `set -u`, a signal arriving between installing the handlers and
# initialising these would make the handler itself fail on an unbound variable.
RESTORING=0
RESTORED=0
trap 'restore_prod || true' EXIT
# `trap '' INT TERM HUP` inside the handler is the point: a second signal (harness escalation after
# the tool limit, or an impatient second Ctrl-C) must not abandon a restore that is already polling
# for prod to bind (~43 s). Ignoring further signals makes the handler commit to finishing it.
# If a restore is ALREADY in flight (the normal path's step 4), the handler must not exit: an exit
# would kill the polling restore that is running inside the normal path, and the sentinel re-arm at
# the end of restore_prod would never run. The previous version returned 0 from the guard and then
# `exit 1` -- which is exactly that kill (review pass 15 reproduced it: `already in progress` then no
# SENTINEL STARTED, and no FATAL line either). So: a signal during a restore means "finish it".
# Two consequences of the trap below, stated so nobody is surprised: a signal arriving while the
# suite runs is handled only after the foreground child exits (up to E2E_TIMEOUT), and after the first
# signal INT/TERM/HUP are ignored for the rest of the script's life -- so a harness that escalates by
# SIGTERM cannot stop a swap that is already restoring; only SIGKILL can, and that abandons the
# restore. The trade is deliberate: finishing the restore matters more than being killable.
on_signal(){
  trap '' INT TERM HUP
  if [ "${RESTORING:-0}" = "1" ]; then return 0; fi
  restore_prod || true
  exit 1
}
trap 'on_signal' INT TERM HUP

# The pid listening on the swap's port, or empty. `ss -ltnp` needs no privileges here and names the
# process, which a `/health` probe cannot: prod satisfies /health too, so a swap whose `systemctl
# stop` silently failed would run its four 150k-token sessions against the operator's live server.
# One port variable for the whole script. It used to be three (E2E_PORT here, PORT in the start
# script, a hard-coded 8080 in the health probes), so setting E2E_PORT aborted every swap AFTER prod
# had been stopped -- a latent false abort. `PORT` is forwarded to the start script explicitly.
# The TEST server's port. It must NOT be 8080: that is prod *and* this session's own model port,
# so a test server there would serve normal session traffic (the classifier, my own requests) and
# taint the measurement with a workload the suite never asked for -- and those queries 400 on the
# test profile's smaller context. 8085 is the established test port; prod stays on 8080 via
# PROD_PORT below, which is what the restore checks probe.
PORT="${E2E_PORT:-${PORT:-8085}}"
# Prod's port is NOT the swap's port: prod binds 8080 from ~/.config/ninfer.conf unconditionally, while
# the test server binds $PORT. Probing the restore with $PORT would report FATAL: prod did not come up
# for an E2E_PORT of anything but 8080 while prod is up on 8080 -- and would leave the sentinel stopped
# (review pass 15).
PROD_PORT="${PROD_PORT:-8080}"
listener_pid(){ ss -ltnp 2>/dev/null | sed -n "s/.*:${1:-$PORT} .*pid=\([0-9]*\).*/\1/p" | head -1; }

echo "[$(ts)] swap start (HOST_KV_MIB=${E2E_HOST_KV_MIB:-12288})" | tee -a $LOG

# Restore prod + sentinel. Used by the normal path and by every abort path: a swap that fails before
# its suite runs used to `exit 1` with prod AND the sentinel down, leaving the outage for a human to
# notice (that happened twice on 2026-09-24).
restore_prod(){
  [ "${RESTORED:-0}" = "1" ] && return 0   # idempotent: also reachable from the EXIT trap
  if [ "${RESTORING:-0}" = "1" ]; then
    # Re-entry while the poll is running (the reviewer reproduced this with two SIGTERMs): the flag
    # must mark COMPLETION, not entry, or the second handler returns immediately and its `exit 1`
    # abandons the restore mid-poll. Signals are ignored during the restore (see the trap above), so
    # this is the belt to that suspenders.
    echo "[$(ts)] restore already in progress — waiting for it" | tee -a $LOG
    return 0
  fi
  RESTORING=1
  pkill -f "build/apps/ninfer-serve" 2>/dev/null; sleep 3
  sudo -n systemctl start ninfer.service
  MAIN=""; LISTENER=""
  for i in $(seq 1 90); do
    MAIN="$(sudo -n systemctl show -p MainPID --value ninfer.service 2>/dev/null || true)"
    LISTENER="$(listener_pid "$PROD_PORT")"
    # /health alone cannot tell prod from the swap's test server (the start path got a listener check
    # for exactly this reason); require the unit's MainPID to be the process owning the port.
    if [ -n "$MAIN" ] && [ "$MAIN" != "0" ] && [ "$LISTENER" = "$MAIN" ] &&        curl -sf -m2 http://localhost:${PROD_PORT}/health >/dev/null 2>&1; then
      break
    fi
    sleep 2
  done
  if [ -n "$MAIN" ] && [ "$MAIN" = "$LISTENER" ] && curl -sf -m3 http://localhost:${PROD_PORT}/health >/dev/null; then
    # prod is healthy again — re-arm the wedge sentinel (it was stopped at the top of the swap so it
    # could not misfire against the e2e server).
    sudo -n systemctl start ninfer-wedge-sentinel.service 2>/dev/null || \
      echo "[$(ts)] WARN: could not restart ninfer-wedge-sentinel — do it manually" | tee -a $LOG
    echo "[$(ts)] prod restored (pid $MAIN)" | tee -a $LOG
    RESTORED=1
  else
    echo "[$(ts)] FATAL: prod did not come up (sentinel is STOPPED — restart it manually: sudo systemctl start ninfer-wedge-sentinel.service)" | tee -a $LOG
    # RESTORED marks "do not try again", not "succeeded": a failed restore that left it 0 made the
    # EXIT trap re-enter the whole 90-iteration poll, so a failure cost ~180 s twice with the sentinel
    # down throughout (review pass 16 measured it). The FATAL line is the signal, and it is printed.
    RESTORED=1
    RESTORING=0
    return 1
  fi
  RESTORING=0
}

# 1. stop prod
#    Stop the wedge sentinel FIRST: it polls /stats on 8080 and cannot tell
#    the e2e server from prod (the e2e process's counters start at 0, below
#    prod's final totals, so its Class-C "no progress" logic false-arms
#    during the swap). 2026-09-17: a mid-swap sentinel restart collided with
#    this script's pkill/start and left prod down until a manual restart.
sudo -n systemctl stop ninfer-wedge-sentinel.service 2>/dev/null || true
sudo -n systemctl stop ninfer.service
sleep 3
pkill -f "build/apps/ninfer-serve" 2>/dev/null; sleep 2

# Verify prod really is gone before starting the test server. This script is `set -uo pipefail` (no
# -e), so a failed `systemctl stop` would otherwise be invisible: the test server cannot bind :8080,
# the /health probe below succeeds against PROD, "e2e server up" is printed, and the whole suite then
# runs against the operator's live server -- tainting the measurement and hammering prod.
for i in $(seq 1 15); do
  [ -z "$(listener_pid)" ] && [ -z "$(listener_pid "$PROD_PORT")" ] && break
  sleep 1
done
# Both ports, not just the test server's: with E2E_PORT set, prod (which binds PROD_PORT) could still
# be alive and this guard would not see it -- the state the guard exists to prevent.
if [ -n "$(listener_pid)" ] || [ -n "$(listener_pid "$PROD_PORT")" ]; then
  echo "[$(ts)] FATAL: something is still listening on :${PORT} (pid $(listener_pid)) or :${PROD_PORT} (pid $(listener_pid "$PROD_PORT")) after stopping prod — refusing to run the suite against it" | tee -a $LOG
  exit 1   # the EXIT trap restores prod
fi

# 2. start e2e server (same binary, 32k ctx / 64k KV)
#    V3 test start (yarn script, env-var driven): the old V2 start script
#    hardcodes a v2 artifact (--spec mtp + YaRN) that the v3 engine rejects.
#    Prod parity: swift artifact + froggeric v225 template + dflash2.
BIN=$HOME/ninfer/build/apps/ninfer-serve \
MODEL=$HOME/ninfer-models/swift15/qwen3_8_27b_nvfp4swift15.ninfer \
CHAT_TEMPLATE=$HOME/froggeric_v225_chat_template.jinja \
SPEC="${SPEC:-dflash2}" \
HOST_KV_MIB="${E2E_HOST_KV_MIB:-12288}" \
PORT="$PORT" \
bash "$SCRIPT_DIR/ninfer-start-test.sh" 9>&- >> $LOG 2>&1
for i in $(seq 1 90); do
  curl -sf -m2 http://localhost:${PORT}/health >/dev/null 2>&1 && break
  sleep 2
done
if ! curl -sf -m3 http://localhost:${PORT}/health >/dev/null; then
  echo "[$(ts)] FATAL: e2e server did not come up — restoring prod" | tee -a $LOG
  restore_prod || true
  exit 1
fi
# The listener must be the process ninfer-start-test.sh recorded, not merely something answering
# /health: this is the check that makes "the e2e server is up" mean the test server.
LISTENER=""; RECORDED=""
for i in $(seq 1 30); do
  LISTENER="$(listener_pid)"
  RECORDED="$(cat "$HOME/ninfer-test.pid" 2>/dev/null || true)"
  [ -n "$LISTENER" ] && [ "$LISTENER" = "$RECORDED" ] && break
  sleep 1
done
if [ -z "$LISTENER" ] || [ "$LISTENER" != "$RECORDED" ]; then
  echo "[$(ts)] FATAL: :${PORT} is served by pid '${LISTENER:-none}' but the test server recorded '${RECORDED:-none}' — aborting before the suite can run against the wrong server" | tee -a $LOG
  exit 1   # the EXIT trap restores prod
fi
echo "[$(ts)] e2e server up (pid $LISTENER)" | tee -a $LOG

# 3. run e2e
#    E2E_SUITE: which suite to run (default: the local focused copy; the full
#    11-phase suite is ~/ninfer/tools/e2e/ninfer-e2e.py)
#    E2E_TIMEOUT: per-run cap in seconds (default 420; 0 = uncapped — for
#    long phase groups, run the swap in the background: a >10-minute swap
#    cannot be a foreground call under the caller's 10-minute tool limit)
#    E2E_START_PHASE: first phase to run (repo suite only; for split runs)
E2E_SUITE="${E2E_SUITE:-$SCRIPT_DIR/ninfer-e2e.py}"
E2E_TIMEOUT="${E2E_TIMEOUT:-420}"
E2E_START_PHASE="${E2E_START_PHASE:-1}"
EXTRA=()
case "$E2E_SUITE" in *tools/e2e/*) EXTRA=(--start-phase "$E2E_START_PHASE") ;; esac
# E2E_SUITE_ARGS: extra arguments for the suite, word-split. Without this the driver could not
# select a single cmp-e2e profile, so `--profile prod4` (the W4b gate) silently ran the whole
# battery instead -- a gate that never executed.
EXTRA+=(--port "$PORT")
if [ -n "${E2E_SUITE_ARGS:-}" ]; then
  # shellcheck disable=SC2206
  EXTRA+=(${E2E_SUITE_ARGS})
fi
cd "$(dirname "$E2E_SUITE")"
if [ "$E2E_TIMEOUT" = "0" ]; then
  python3 -u "$E2E_SUITE" "${EXTRA[@]}" 9>&- > $RUN_LOG 2>&1
else
  timeout "$E2E_TIMEOUT" python3 -u "$E2E_SUITE" "${EXTRA[@]}" 9>&- > $RUN_LOG 2>&1
fi
E2E_RC=$?
[ $E2E_RC -eq 124 ] && echo "[$(ts)] e2e hit the ${E2E_TIMEOUT}s cap — partial results" | tee -a $LOG
echo "[$(ts)] e2e rc=$E2E_RC" | tee -a $LOG
# Evidence of what actually ran: the suite's profile headers, so a run cannot be mistaken for
# another profile (this is how the prod4 gate was found to have never executed).
grep -E "^=== |^SUMMARY|^GATES|FAIL:" $RUN_LOG | tail -20 | tee -a $LOG
grep -E "PASS|FAIL|WARN|SUMMARY" $RUN_LOG | tail -30 | tee -a $LOG

# 4. stop e2e, restart prod (new binary)
restore_prod || exit 1
echo "[$(ts)] swap done (e2e rc=$E2E_RC)" | tee -a $LOG
exit $E2E_RC
