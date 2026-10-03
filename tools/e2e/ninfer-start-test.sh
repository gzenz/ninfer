#!/bin/bash
# ninfer-start-test.sh — test server for this tree (~/ninfer, v3), light by default.
#
# Profiles:
#   CTX=32k (default): 32k ctx / 64k KV / c=3 / 20,480 MiB host KV. Fast, safe on
#       this host; the cache-eviction and demote/restore paths are exercised by sizing.
#   CTX=prod4:         prod SLOT SHAPE (c=4, device-state-slots=4, host-state-slots=16)
#       with a LIGHT host KV (20,480 MiB) — the concurrency shape that collapsed prod,
#       without prod's 30 GiB pinned footprint.
#
# THE HOST-KV NUMBERS ABOVE WERE 12 GiB UNTIL 2026-10-01. Since `--host-kv-mib` became the ceiling for
# KV *and* state together (2026-09-28), 12,288 MiB could not hold a 64-slot state pool plus a KV span,
# and the server refused to start (`Host KV arena could not pin its initial span`). Read the branches
# below rather than this header if the two ever disagree.
#   CTX=200k:          prod parity (262k ctx, c=2, 30 GiB host KV). HEAVY: takes ages
#       and OOM-crashes WSL (host pinned shmem). Requires ALLOW_HEAVY=1.
#
# Env knobs: HOST_KV_MIB, DEVICE_STATE_SLOTS, HOST_STATE_SLOTS, MAX_CONCURRENCY,
#   MAX_CONTINUATIONS, SPEC (mtp|dflash2|none), BIN, MODEL, PORT, CHAT_TEMPLATE,
#   PRESERVE_THINKING (0|1), NINFER_MAT_* instrumentation.
set -euo pipefail
CTX="${CTX:-32k}"
MAX_CONTINUATIONS="${MAX_CONTINUATIONS:-18}"
ALLOW_HEAVY="${ALLOW_HEAVY:-0}"

if [ "$CTX" = "200k" ] && [ "$ALLOW_HEAVY" != "1" ]; then
  echo "REFUSED: CTX=200k pins ~30 GiB host KV; it takes ages and OOM-crashes WSL."
  echo "          Set ALLOW_HEAVY=1 only if you accept that."
  exit 2
fi

if [ "$CTX" = "200k" ]; then
  HOST_KV_MIB="${HOST_KV_MIB:-30720}"
  DEVICE_STATE_SLOTS="${DEVICE_STATE_SLOTS:-7}"
  HOST_STATE_SLOTS="${HOST_STATE_SLOTS:-112}"
elif [ "$CTX" = "prod4" ]; then
  # prod's slot shape: one device state slot per concurrent request -> zero spare
  # (device state capacity is max_concurrency + device_state_slots).
  HOST_KV_MIB="${HOST_KV_MIB:-12288}"
  DEVICE_STATE_SLOTS="${DEVICE_STATE_SLOTS:-4}"
  HOST_STATE_SLOTS="${HOST_STATE_SLOTS:-16}"
else
  # ONE BUDGET: `--host-kv-mib` is the ceiling for KV *and* state (2026-09-28). 128 state slots is ~23.4 GiB
  # at ~0.187 GiB each, which was twice the old 12,288 MiB ceiling and made the test server fail to pin
  # its KV span -- `Host KV arena could not pin its initial span`, with the state pool already past the cap. The
  # profile now leaves the ceiling room to act as the pressure knob.
  HOST_KV_MIB="${HOST_KV_MIB:-20480}"
  DEVICE_STATE_SLOTS="${DEVICE_STATE_SLOTS:-5}"
  HOST_STATE_SLOTS="${HOST_STATE_SLOTS:-64}"
fi

sudo mv /lib/x86_64-linux-gnu/libnvidia-ptxjitcompiler.so.1 /lib/x86_64-linux-gnu/libnvidia-ptxjitcompiler.so.1.disabled 2>/dev/null || true
export PATH=/usr/local/cuda/bin:$HOME/.local/bin:$HOME/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda/lib64:/usr/lib/wsl/lib:${LD_LIBRARY_PATH:-}

# THE OLD DEFAULT NAMED A DIRECTORY THAT DOES NOT EXIST (`official-v3/`), so any caller that did not pass
# MODEL -- pre-commit.sh did not -- handed the server a nonexistent artifact. It is now the artifact QA and
# the recorded e2e acceptance runs both use, so a bare call reproduces the acceptance configuration instead
# of a second, weaker one. Pass MODEL= explicitly to test a different artifact.
MODEL="${MODEL:-$HOME/ninfer-models/swift15/qwen3_8_27b_nvfp4swift15.ninfer}"
LOG="${LOG:-$HOME/ninfer-serve.log}"

if [ "$CTX" = "200k" ]; then
  CTX_FLAGS="--max-concurrency ${MAX_CONCURRENCY:-2} --max-context 262144 --kv-capacity ${KV_CAPACITY:-auto} --max-private-continuations ${MAX_CONTINUATIONS:-16} --max-shared-prefixes 8"
elif [ "$CTX" = "prod4" ]; then
  CTX_FLAGS="--max-concurrency ${MAX_CONCURRENCY:-4} --max-context 262144 --kv-capacity ${KV_CAPACITY:-262144} --max-private-continuations ${MAX_CONTINUATIONS:-18} --max-shared-prefixes ${MAX_SHARED_PREFIXES:-6}"
else
  CTX_FLAGS="--max-concurrency ${MAX_CONCURRENCY:-3} --max-context 32768 --kv-capacity 65536 --max-shared-prefixes ${MAX_SHARED_PREFIXES:-6} --max-private-continuations $MAX_CONTINUATIONS"
fi

SPEC="${SPEC:-mtp}"
if [ "$SPEC" = "dflash2" ]; then
  SPEC_FLAGS="--spec dflash2 --draft-tokens 7 --lm-head-draft"
elif [ "$SPEC" = "none" ]; then
  # No --spec at all: the CLI rejects "--spec none"; omitting it leaves
  # SpeculativeOptions at backend=None. Lighter GPU sync + host footprint.
  SPEC_FLAGS=""
else
  SPEC_FLAGS="--spec mtp --draft-tokens 5 --lm-head-draft"
fi

# EXPORT BIN -- it is not optional, and the reason is the launch form below.
#
# The server is started as `bash -c '"$BIN" "$1" ...' _ "$MODEL"`, and that body is SINGLE-QUOTED, so `$BIN`
# is expanded by the CHILD shell, not this one. A plain assignment is NOT inherited, so any caller that does
# not put BIN in the environment leaves the child with an EMPTY COMMAND WORD: `line 1: : command not found`,
# NINFER_EXIT=127, and a log whose only content is that one line. That is exactly how pre-commit.sh's e2e
# block failed -- it calls this script bare.
#
# e2e-swap.sh was never affected, because it passes `BIN=...` as a command-PREFIX assignment, which bash
# exports. So the bug hid behind the one caller that happened to be correct, and the gate that was supposed
# to catch this class of failure was itself the thing that could not run. Exporting here fixes it for every
# caller at once, which is the point: the script owns its defaults, so a bare call must work.
export BIN="${BIN:-$HOME/ninfer/build/apps/ninfer-serve}"

# A MISNAMED OR MISSING ARTIFACT SHOULD SAY SO, not surface as a 127 from an empty command word or as a
# model-load error minutes later. These two checks turn both into one line naming the path.
[ -x "$BIN" ]   || { echo "FATAL: server binary is not executable: $BIN" >&2; exit 2; }
[ -r "$MODEL" ] || { echo "FATAL: model artifact is not readable: $MODEL" >&2; exit 2; }

# STOP ANY PREVIOUS TEST SERVER BY PORT AND BY RECORDED PID -- NEVER BY PATH. `pkill -f
# "build/apps/ninfer-serve"` matches the CALLER's command line, so any caller whose command mentions that
# path (a freshness check, a `git status` line, a variable holding it) killed its own shell mid-run: twice
# on 2026-09-25 and twice on 2026-10-03. Bracketing the pattern does NOT help -- pkill takes an ERE, so
# `ninfer-serv[e]` still matches the caller's literal string. Only dropping the path match does.
for _p in "${PORT:-8085}" "${PROD_PORT:-8080}"; do
  _pid="$(ss -ltnp 2>/dev/null | sed -n "s/.*:${_p} .*pid=\([0-9]*\).*/\1/p" | head -1)"
  # never kill QA by accident: :8080 IS the operator's server, so only ever kill a NON-systemd listener
  [ -n "$_pid" ] && [ "$_pid" != "$(systemctl show -p MainPID --value ninfer.service 2>/dev/null)" ] \
    && kill -9 "$_pid" 2>/dev/null || true
done
unset _p _pid
[ -s "$HOME/ninfer-test.pid" ] && kill -9 "$(cat "$HOME/ninfer-test.pid")" 2>/dev/null || true
sleep 2
# Shed page cache before load: the pinned shmem (host-KV arena + host-state pool)
# is non-reclaimable, so reclaimable page cache is the headroom we can return.
sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
cd ~/ninfer
echo "Starting TEST (ctx=$CTX spec=$SPEC, host-kv=${HOST_KV_MIB}MiB, conts=${MAX_CONTINUATIONS}, dev-state=${DEVICE_STATE_SLOTS}, host-state=${HOST_STATE_SLOTS})"
echo "Log: $LOG"

# Retain the previous run's log instead of truncating it: `> "$LOG"` below destroyed the only
# record of every probe result the previous run produced (the plan's "cleared" rows rested on
# hand-transcribed prose because of it).
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.prev"

CT_FLAGS_EXTRA=""
[ -n "${CHAT_TEMPLATE:-}" ] && CT_FLAGS_EXTRA="--chat-template $CHAT_TEMPLATE"
# STATS_PORT: bind the stats endpoint for the TEST server. The caller cannot do this through
# CT_FLAGS_EXTRA -- this assignment REINITIALISES it, so a caller's value (and its `--stats-port`) is
# discarded silently, which cost two runs on 2026-09-27: both read empty `/stats` behind `2>/dev/null`, and
# "no stats server", "wrong port" and "bad parse" were indistinguishable, so an instrument that had in fact
# fired looked dead. `stats_port = 0` (the default) means the stats server is never bound at all, so the
# variable is required for any run that wants a second, independent read of a counter.
if [ -n "${STATS_PORT:-}" ]; then
  # 8081 is PROD's stats port. The 8080/8085 guards above do not cover it, and a test server answering
  # `:8081` would serve prod's `/stats` readers their own numbers -- the same class of mix-up the port rules
  # exist for, on the other channel.
  [ "$STATS_PORT" = "8081" ] && { echo "REFUSED: STATS_PORT 8081 is prod's stats port"; exit 2; }
  CT_FLAGS_EXTRA="$CT_FLAGS_EXTRA --stats-port $STATS_PORT"
fi
# --preserve-thinking retains the open assistant turn as a byte-stable
# ResponseReplay rewrite checkpoint instead of re-rendering it (TurnClosure).
# v2's render defaulted preserve_thinking=true, v3 to false — a known regression
# candidate for re-touch digest divergence (no restore candidate -> re-prefill).
[ "${PRESERVE_THINKING:-0}" = "1" ] && CT_FLAGS_EXTRA="$CT_FLAGS_EXTRA --preserve-thinking"
[ "${NO_CUDA_GRAPH:-0}" = "1" ] && CT_FLAGS_EXTRA="$CT_FLAGS_EXTRA --no-cuda-graph"

# Export these only when they carry a value. `std::getenv` returns non-NULL for an EMPTY string, so
# the previous form (`export NINFER_MAT_DEBUG="${NINFER_MAT_DEBUG:-}"`) switched every MAT_* probe ON
# in every e2e run: 1.7 MB of probe output for one prod4 run, a 678 MB serve log, and all of it on
# the paths the suite times. Unset is what "off" means here.
for _mat_name in NINFER_MAT_DEBUG NINFER_MAT_GRID NINFER_MAT_TAIL NINFER_MAT_FRONT NINFER_MAT_FINE; do
  _mat_value="${!_mat_name:-}"
  if [ -n "$_mat_value" ]; then export "$_mat_name=$_mat_value"; else unset "$_mat_name"; fi
done
unset _mat_name _mat_value

# The server runs in the background of this wrapper so that `$!` here is the SERVER's pid. Writing
# the wrapper's pid instead (the previous form, `echo $! > ~/ninfer-test.pid` outside) made the pid
# file name a shell that is not the port listener, so no caller could check which process owns :8080
# -- which is the check that stops a swap whose `systemctl stop` silently failed from running its
# whole suite against prod (/health alone cannot tell them apart). Verified: the recorded pid equals
# the pid `ss -ltnp` reports for the port.
# THE BRANCH ANCHOR IS ON for the test server (2026-09-28), because it is the SHIPPED configuration and it
# had no coverage at all: it is a server-level flag, so no phase could enable it, and the change that took the
# pinned-prefix share from 25% to 7% was never exercised by this suite. `phase_reuse_paths` asserts it is
# taken.
#
# ANCHOR-OFF ARM. The engine reads PRESENCE, not value (`engine_core.h:1487` tests
# `std::getenv("NINFER_BRANCH_ANCHOR") != nullptr`), so `NINFER_BRANCH_ANCHOR=0` still enables the anchor --
# which is why the documented arm was inert. Two further traps this replaces: `env` with an EMPTY prefix
# does not REMOVE an inherited variable (QA's own environ carries `NINFER_BRANCH_ANCHOR=1`), so the off arm
# needs `env -u`; and a presence test would treat `E2E_ANCHOR_OFF=0` as "off". The comparison is therefore
# against the VALUE, and the child's environment is written to the log so the arm is recorded rather than
# inferred.
# ARMS ARE BUILT AS (unsets...) (assignments...), AND THE ORDER IS LOAD-BEARING. GNU `env` stops option
# parsing at the first NAME=VALUE, so `env A=1 -u B cmd` treats `-u` as the COMMAND: the server never
# starts and the swap dies with "e2e server did not come up". That is exactly what the first version of
# this block did on the OFF arm -- the arm whose whole job is to be the control. Unserts are collected
# separately and emitted FIRST.
#
# `-u` rather than `X=`: `std::getenv` returns non-NULL for an EMPTY value, so an off arm that assigns
# `X=` ENABLES the feature it is the control for. QA's own environ carries `NINFER_BRANCH_ANCHOR=1`, which
# is why the anchor's off arm needs `-u` rather than merely not setting it.
ARM_UNSET=()
ARM_SET=("NINFER_BRANCH_ANCHOR=${NINFER_BRANCH_ANCHOR:-1}")
if [ "${E2E_ANCHOR_OFF:-0}" = "1" ]; then
  ARM_UNSET+=(-u NINFER_BRANCH_ANCHOR)
  ARM_SET=()
fi
# SIBLING-RETAIN ARM (2026-10-02), read by PRESENCE at `resource_manager.h:603`.
if [ "${E2E_SIBLING_RETAIN:-0}" = "1" ]; then
  ARM_SET+=(NINFER_SIBLING_RETAIN=1)
else
  ARM_UNSET+=(-u NINFER_SIBLING_RETAIN)
fi
ANCHOR_ENV=(env "${ARM_UNSET[@]}" "${ARM_SET[@]}")
# TRUNCATE HERE, THEN APPEND EVERYWHERE. The child used to be started with a plain redirect to $LOG --
# O_TRUNC and NOT O_APPEND -- so it wrote at its own offset and any `>>` line added while it streamed was
# OVERWRITTEN by later server output. That is why ANCHOR_CHILD= and ARM_CHILD= never appeared in a serve
# log while NINFER_EXIT= did (nothing writes after that one), and the first diagnosis blamed the command
# substitution, which was never the cause. Explicit truncate first, then O_APPEND for both writers.
: > "$LOG"
rm -f "$HOME/ninfer-test.pid"
nohup "${ANCHOR_ENV[@]}" bash -c '"$BIN" "$1" \
  --host 0.0.0.0 --port "${PORT:-8080}" \
  --default-max-tokens 131072 --pending-timeout-ms 900000 \
  --kv-dtype nvfp4 '"$SPEC_FLAGS"' \
  --tolerant-tool-calls --host-kv-mib '"$HOST_KV_MIB"' \
  --device-state-slots '"$DEVICE_STATE_SLOTS"' --host-state-slots '"$HOST_STATE_SLOTS"' \
  --temperature 1.0 --top-p 0.95 --top-k 20 \
  --request-log-jsonl ~/ninfer-requests.jsonl --log-stats-interval-ms 5000 \
  '"$CTX_FLAGS"' '"$CT_FLAGS_EXTRA"' &
  SRV=$!
  echo "$SRV" > '"$HOME"'/ninfer-test.pid
  wait "$SRV"
  echo "NINFER_EXIT=$?" >> '"$LOG"' 2>&1' _ "$MODEL" \
  >> "$LOG" 2>&1 &

# THE ARM MARKERS, written HERE rather than inside the child. `phase_reuse_paths` reads `ANCHOR_CHILD=` to
# learn which arm it is looking at, and `ARM_CHILD=` records both flags for the reader; the version that
# wrote them from inside the `bash -c` NEVER LANDED -- both lines were absent from every serve log while
# `NINFER_EXIT=` (same `>>` mechanism, no command substitution) was present, which is what made
# `anchor_expected` always None and the anchor gate unable to fail. The child writes its pid before it waits,
# so poll for that and read the process's own environ: no nested quoting, and the file is the one the phase
# opens (`args.serve_log`).
for i in $(seq 1 60); do
  sleep 3
  if [ -s "$HOME/ninfer-test.pid" ]; then
    _srv="$(cat "$HOME/ninfer-test.pid" 2>/dev/null)"
    if [ -n "$_srv" ] && [ -r "/proc/$_srv/environ" ]; then
      {
        printf 'ANCHOR_CHILD=%s\n' "$(tr '\0' '\n' < "/proc/$_srv/environ" | grep -c NINFER_BRANCH_ANCHOR)"
        printf 'ARM_CHILD=%s\n' "$(tr '\0' '\n' < "/proc/$_srv/environ" | grep -E 'NINFER_BRANCH_ANCHOR|NINFER_SIBLING_RETAIN' | tr '\n' ' ')"
      } >> "$LOG"
      unset _srv
      break
    fi
    unset _srv
  fi
done
for i in $(seq 1 60); do
  sleep 3
  if curl -sf "http://localhost:${PORT:-8080}/health" > /dev/null 2>&1; then
    echo "Test server ready on ${PORT:-8080} (pid $(cat ~/ninfer-test.pid))"
    exit 0
  fi
done
echo "WARNING: test server did not become ready in 3 min. Check $LOG"
exit 1