#!/usr/bin/env bash
# Prod watcher: a full-fidelity log AND a high-signal alert stream, from one pass over the journal.
#
#   bash tools/ops/ninfer-watch.sh            # stdout = alerts, file = everything diagnostic
#
# Meant to be run as a Monitor's command, so each stdout line becomes a notification. The log file under
# `~/ninfer-watch/` keeps the insight lines that are too frequent to notify on (`checkpoint StateImage
# priced`, every eviction, the recovery/shutdown residuals) -- that split is the point: on 2026-09-26 a
# pattern-only view could not tell a real recovery from a restart, and a log-only view is not read.
#
# WHY THIS IS NOT A `grep -E` PATTERN:
#   * `post-recovery residual` is printed by `fail_all_locked`, which ALSO runs on the shutdown path. On
#     2026-09-26 I read 19 of those zero lines as "19 recoveries, residual zero"; every one was a stop.
#     The amount is the evidence, and the line carries its own discriminator (`(recover)` / `(fail-all)`):
#     a NON-ZERO residual alerts whatever produced it, an all-zero one stays log-only. An earlier version
#     instead armed on `WORKER RECOVER|WORKER CRASH` and alerted only when armed -- which dropped a
#     non-zero residual after `WORKER OOM` (the wedge signature) and a non-zero `(fail-all)` line.
#   * `private victim evicted: demotable=1` is not an error at all: it is #6's evidence (an eviction taken
#     while the host tier still had room), so it is an ALERT here even though a crash-only pattern would
#     drop it.
#   * liveness is not in the journal at all. The wedge of 2026-09-25 had the unit `active`, an empty
#     scheduler and `/health` 503, with nothing in the journal to grep -- so this also polls `/health`
#     and reports TRANSITIONS. (Corrected: this used to claim that is "why a stop/start pair does not page
#     anyone", which was false -- a pair produces TWO alerts, down and up. That is deliberate: the return
#     to service is information, and a transition report is what keeps a sustained outage to one alert
#     instead of one per poll.)
set -uo pipefail

UNIT="${UNIT:-ninfer.service}"
# The sentinel is a SECOND unit and it is where the wedge is announced (`WEDGE (A/B): engine idle with
# work pending Ns -- restarting ninfer`). Watching only ninfer.service means the one event that explains a
# dead prod is invisible; on 2026-09-26 the watcher reported a bare health failure and no cause.
SENTINEL_UNIT="${SENTINEL_UNIT:-ninfer-wedge-sentinel.service}"
HEALTH_URL="${HEALTH_URL:-http://127.0.0.1:8080/health}"
POLL_S="${POLL_S:-30}"
LOGDIR="${LOGDIR:-$HOME/ninfer-watch}"
LOG="${LOG:-$LOGDIR/watch-$(date +%Y%m%d-%H%M%S).log}"
mkdir -p "$LOGDIR"
ln -sfn "$(basename "$LOG")" "$LOGDIR/latest.log"

# The build the log belongs to, by hash of the RUNNING exe -- never by the build directory's path, which
# is replaced by the next rebuild (plan.md, Acceptance).
mainpid=$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null || echo 0)
exe_sha=$(sha256sum "/proc/${mainpid}/exe" 2>/dev/null | awk '{print $1}')
{
  # Say plainly what state the unit is in. `pid=0 exe_sha256=unknown` reads as "the exe could not be
  # identified", when what it means is "the unit is not running" -- and a monitor that starts while prod is
  # deliberately stopped is a normal state, not an anomaly (the operator stops prod to work on it).
  if [ "${mainpid:-0}" = "0" ]; then
    echo "unit=$UNIT INACTIVE (watcher started $(date -Is)) -- MainPID is 0, so there is no running exe to"
    echo "  hash; the watch is on the journal and the request log, and /health will report when it returns"
  else
    echo "unit=$UNIT running pid=$mainpid exe_sha256=${exe_sha:-unknown} (watcher started $(date -Is))"
  fi
  echo "health_url=$HEALTH_URL poll_s=$POLL_S request_log=$REQUEST_LOG"
} | tee "$LOG"

# The filter lives in ninfer-watch.awk so it can be run against a fixture (`ninfer-watch-test.sh`); a
# filter that matches nothing is indistinguishable from a healthy quiet system, which is the failure this
# whole arrangement exists to avoid. Overriding JOURNAL_CMD points it at a file for that test.
# `-n 0`: `journalctl -f` prints its last 10 lines before following, so every 30-minute re-arm re-processed
# the previous run's tail and could re-alert a line already reported (observed: a watcher started at 17:38
# opened its log with three lines stamped 17:37).
# THE TRADE-OFF, stated because it is a real one: `-n 0` means anything written while no watcher is running
# is never seen. A tail replay would re-show the last 10 lines, which is not the same thing as covering the
# gap -- it re-shows what was already reported and still misses everything older. Each arm writes its own
# timestamped log, so a gap shows up as an interval no log covers rather than as silence inside one.
JOURNAL_CMD="${JOURNAL_CMD:-journalctl -u $UNIT -u $SENTINEL_UNIT -f -q -n 0 --output=short-iso}"
# The request log is a SECOND instrument, not a duplicate of the journal: the journal says what the engine
# did (evictions, recoveries), this says what a client got (reuse, prefill time). Both thresholds are a
# first cut and are settable, so they can be tuned from the log rather than recompiled.
REQUEST_LOG="${REQUEST_LOG:-$HOME/ninfer-requests.jsonl}"
# 0.6, SET FROM THE LOG (2026-09-26), not chosen: over 4,439 non-root reuse requests with
# prompt >= 20k the hit fraction is bimodal -- 0.1-0.5 (1,016 requests, the shared-prefix-only cluster)
# and 0.9-1.0 (3,292, healthy) -- with only 69 requests in the 0.4-0.8 valley. The threshold belongs in
# that valley; the first cut of 0.9 sat on the healthy cluster's floor and so called an 89%-reuse request
# "low use".
WATCH_REUSE_MIN_FRACTION="${WATCH_REUSE_MIN_FRACTION:-0.6}"
WATCH_REUSE_MAX_PREFILL_S="${WATCH_REUSE_MAX_PREFILL_S:-2.0}"
AWK_FILTER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ninfer-watch.awk"
JQ_FILTER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ninfer-watch-requests.jq"

journal() {
  # shellcheck disable=SC2086
  $JOURNAL_CMD 2>/dev/null | gawk -v logpath="$LOG" -f "$AWK_FILTER"
}

requests() {
  # Alerts to stdout (they become notifications); every reuse record to the log, so "no alert" can be told
  # from "no reuse happened" and the thresholds can be set from data.
  tail -F -n 0 "$REQUEST_LOG" 2>/dev/null |
    jq -rc --argjson fraction "$WATCH_REUSE_MIN_FRACTION" \
           --argjson max_prefill_s "$WATCH_REUSE_MAX_PREFILL_S" \
           -f "$JQ_FILTER" 2>/dev/null |
    while IFS= read -r line; do
      case "$line" in
        "[watch] LOW PREFIX USE"*|"[watch] SLOW REUSE"*) echo "$line"; printf '%s\n' "$line" >>"$LOG" ;;
        *) printf '%s\n' "$line" >>"$LOG" ;;
      esac
    done
}

health() {
  # Two fixes here, both found by running it while the operator had stopped prod on purpose:
  #  * a failure gave the code "000000", because curl's -w printed 000 AND the `|| echo 000` fallback
  #    appended a second one -- so the reported code was not a code. Normalise instead of doubling.
  #  * the FIRST reading being non-200 paged, which is wrong: arming a monitor during a planned stop (which
  #    is exactly when this is done) is not an incident. A transition only means something once a reading
  #    exists -- so a first non-200 is logged, and only later changes alert. Coming back UP does alert:
  #    recovery is information, and the 2026-09-25 wedge is why liveness is watched at all.
  local last="" code
  while sleep "$POLL_S"; do
    code=$(curl -s -o /dev/null -w '%{http_code}' -m5 "$HEALTH_URL" 2>/dev/null)
    [ -n "$code" ] || code=000
    if [ "$code" != "$last" ]; then
      local line="[watch] health ${last:-no-reading} -> ${code} at $(date -Is)"
      if [ -n "$last" ]; then echo "$line"; fi
      echo "$line" >>"$LOG"
      last="$code"
    fi
  done
}

journal &
health &
[ -r "$REQUEST_LOG" ] && requests &
trap 'kill $(jobs -p) 2>/dev/null' INT TERM HUP EXIT
wait
