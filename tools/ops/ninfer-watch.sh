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
  echo "unit=$UNIT started=$(date -Is) pid=$mainpid exe_sha256=${exe_sha:-unknown}"
  echo "health_url=$HEALTH_URL poll_s=$POLL_S"
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
JOURNAL_CMD="${JOURNAL_CMD:-journalctl -u $UNIT -f -q -n 0 --output=short-iso}"
AWK_FILTER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ninfer-watch.awk"

journal() {
  # shellcheck disable=SC2086
  $JOURNAL_CMD 2>/dev/null | gawk -v logpath="$LOG" -f "$AWK_FILTER"
}

health() {
  local last="" code
  while sleep "$POLL_S"; do
    code=$(curl -s -o /dev/null -w '%{http_code}' -m5 "$HEALTH_URL" 2>/dev/null || echo 000)
    [ -z "$code" ] && code=000
    if [ "$code" != "$last" ]; then
      local line="[watch] health ${last:-none} -> ${code} at $(date -Is)"
      echo "$line"
      echo "$line" >>"$LOG"
      last="$code"
    fi
  done
}

journal &
health &
trap 'kill $(jobs -p) 2>/dev/null' INT TERM HUP EXIT
wait
