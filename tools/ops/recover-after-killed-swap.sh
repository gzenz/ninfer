#!/usr/bin/env bash
# A KILLED SWAP LEAVES THREE THINGS BROKEN. Put all three back, in the order that matters.
#
#   bash tools/ops/recover-after-killed-swap.sh
#
# WHY A SCRIPT RATHER THAN A NOTE. plan.md §5 records the trap and I still had to perform this recovery by
# hand repeatedly -- and the failure is not obvious from the outside, because the third symptom is silent:
#
#   1. **QA is stopped** (the swap stopped it and never reached its restore).
#   2. **The sentinel is stopped.** The swap stops it FIRST and re-arms it LAST, so a killed swap leaves it
#      down -- and neither `ninfer-ensure.sh` nor a restart of ninfer.service re-arms it. A running QA with
#      no sentinel looks completely healthy and has lost its wedge protection.
#   3. **The test server may still hold :8085**, pinning ~12 GiB of host KV while a restart pins QA's ~30.
#      Killing it first is what stops the restart from colliding with it for memory.
#
# `pkill -f` is deliberately NOT used -- on this host it matches the caller's own command line (see
# plan.md §5), and a recovery script that kills its own shell is worse than no script.
set -uo pipefail
PROD_PORT="${PROD_PORT:-8080}"
TEST_PORT="${TEST_PORT:-8085}"

listener_pid(){ ss -ltnp 2>/dev/null | sed -n "s/.*:${1} .*pid=\([0-9]*\).*/\1/p" | head -1; }

# 1. the orphaned test server, by PORT (never by path)
tp="$(listener_pid "$TEST_PORT")"
if [ -n "$tp" ]; then
  echo "[recover] orphan test server on :$TEST_PORT (pid $tp) -- stopping it first, so a restart does not"
  echo "[recover] collide with its ~12 GiB of pinned host KV"
  kill -TERM "$tp" 2>/dev/null || true
  for _ in $(seq 1 15); do [ -z "$(listener_pid "$TEST_PORT")" ] && break; sleep 1; done
  [ -n "$(listener_pid "$TEST_PORT")" ] && kill -9 "$tp" 2>/dev/null || true
else
  echo "[recover] no orphan on :$TEST_PORT"
fi

# 2. QA itself
if systemctl is-active --quiet ninfer.service; then
  echo "[recover] ninfer.service already active"
else
  echo "[recover] starting ninfer.service"
  sudo -n systemctl start ninfer.service || systemctl start ninfer.service || true
  for _ in $(seq 1 60); do
    [ "$(curl -s -o /dev/null -w '%{http_code}' -m 3 "http://127.0.0.1:$PROD_PORT/health" 2>/dev/null)" = "200" ] && break
    sleep 3
  done
fi

# 3. the sentinel, LAST -- and verified, because this is the one that is silent when it is missing
if systemctl is-active --quiet ninfer-wedge-sentinel.service; then
  echo "[recover] sentinel already active"
else
  echo "[recover] starting ninfer-wedge-sentinel.service (the swap's re-arm never ran)"
  sudo -n systemctl start ninfer-wedge-sentinel.service || systemctl start ninfer-wedge-sentinel.service || true
fi

# VERIFY, do not assume -- the whole point is that this failure is invisible without a check
sleep 2
h="$(curl -s -o /dev/null -w '%{http_code}' -m 5 "http://127.0.0.1:$PROD_PORT/health" 2>/dev/null)"
s="$(systemctl is-active ninfer-wedge-sentinel.service)"
echo "[recover] health=$h  sentinel=$s"
if [ "$h" = "200" ] && [ "$s" = "active" ]; then
  echo "[recover] recovered"
  exit 0
fi
echo "[recover] NOT recovered -- investigate (health=$h sentinel=$s)"
exit 1
