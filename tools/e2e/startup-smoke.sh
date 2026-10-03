#!/usr/bin/env bash
# THE GATE THAT WAS MISSING: DO THE CONFIGURATIONS NOTHING ELSE COVERS STILL START?
#
#   bash tools/e2e/startup-smoke.sh
#
# WHY. A 2026-10-03 merge silently dropped YaRN's rope-scaling publication and the `--vision-cpu` workspace
# planning, and **every gate stayed green**: the e2e (42 PASS), ctest (134/136), an 8-agent soak and a GPU
# test all passed over that tree. None of them starts the engine with either flag, so neither loss was
# visible to any of them. This is that missing gate.
#
# IT STARTS A TEST SERVER ON THE TEST PORT AND NEVER TOUCHES QA. The port is asserted before and after (the
# repo's rule: nothing may listen on :8080, and anything on :8085 is the test server by construction), and
# the server is killed BY PORT, never by path -- `pkill -f` on a binary path matches the caller's own
# command line on this host and has killed this session's shell four times (plan.md §5).
#
# WHAT IT DOES NOT CHECK, stated because the gap is the point: it proves each configuration STARTS and
# answers /health. It does NOT prove the flag is APPLIED -- a YaRN factor that is parsed and then ignored
# starts just as happily. Catching that needs an assertion on a value the engine reports back (a log line
# carrying the applied factor, or a /stats field), and no such line exists today. Start-up coverage only.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 2

PORT="${TEST_PORT:-8085}"
PROD_PORT="${PROD_PORT:-8080}"
BIN=build/apps/ninfer-serve
LOG=/tmp/startup-smoke.log
ART="${NINFER_TEST_ARTIFACT:-/home/zenz/ninfer-models/swift15/qwen3_8_27b_nvfp4swift15.ninfer}"

[ -x "$BIN" ] || { echo "FAIL: $BIN not built"; exit 2; }
[ -r "$ART" ] || { echo "FAIL: artifact not readable: $ART"; exit 2; }
[ -n "$(ss -ltn 2>/dev/null | grep -E ":${PROD_PORT} ")" ] && echo "note: QA is up on :$PROD_PORT; this script will not touch it"

listener(){ ss -ltnp 2>/dev/null | sed -n "s/.*:${1} .*pid=\([0-9]*\).*/\1/p" | head -1; }
stop_test(){ p="$(listener "$PORT")"; [ -n "$p" ] && { kill -TERM "$p" 2>/dev/null; sleep 2; kill -9 "$p" 2>/dev/null || true; }; }

fails=0
run_case(){
  local name="$1"; shift
  if [ -n "$(listener "$PORT")" ]; then
    echo "FAIL [$name]: something already listens on :$PORT before the case started"
    fails=$((fails+1)); return
  fi
  : > "$LOG"
  # THE MODEL IS POSITIONAL, not `--artifact` (that flag belongs to the ctest targets, not to the server).
  # The first version of this script passed `--artifact` and every case -- including the baseline control --
  # died on a usage banner. The control is what made that visible instead of looking like three engine bugs.
  "$BIN" "$ART" --host 127.0.0.1 --port "$PORT" \
         --max-concurrency 3 --max-context 32768 --kv-capacity 65536 --device-state-slots 5 --host-state-slots 16 \
         --host-kv-mib 6144 "$@" >> "$LOG" 2>&1 &
  local srv=$!
  local up=""
  for _ in $(seq 1 60); do
    [ "$(curl -s -o /dev/null -w '%{http_code}' -m 3 "http://127.0.0.1:$PORT/health" 2>/dev/null)" = "200" ] && { up=1; break; }
    kill -0 "$srv" 2>/dev/null || break
    sleep 3
  done
  local pid_listen; pid_listen="$(listener "$PORT")"
  if [ -n "$up" ] && [ "$pid_listen" = "$srv" ]; then
    echo "ok  [$name]: started, /health 200, listener pid == the server started ($srv)"
  else
    echo "FAIL [$name]: did not come up (health='$up' listener='$pid_listen' server='$srv')"
    grep -E "FATAL|throw|what\(\)|error" "$LOG" | tail -4
    fails=$((fails+1))
  fi
  stop_test
  local left; left="$(listener "$PORT")"
  [ -n "$left" ] && { echo "FAIL [$name]: :$PORT still held by $left after the case"; fails=$((fails+1)); }
}

run_case "baseline"          # the control: if this fails, nothing else here means anything
run_case "yarn-rope-scaling" --rope-scaling-factor 4.0
run_case "vision-cpu"        --vision-cpu

echo
if [ "$fails" -eq 0 ]; then
  echo "startup-smoke: 3/3 configurations started (baseline, --rope-scaling-factor, --vision-cpu)"
  exit 0
fi
echo "startup-smoke: $fails case(s) FAILED"
exit 1
