#!/bin/bash
# Launch the NInfer monitoring sidecar (idempotent via pidfile).
# The sidecar writes its own pidfile (monitor.pid); we only kill a stale
# instance first so a re-run restarts it cleanly.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIDFILE="$DIR/monitor.pid"
LOG="$DIR/monitor.log"

# Rotate our own log (keep 3) so it cannot grow without bound:
# monitor.log.2 <- monitor.log.1 <- monitor.log (oldest dropped).
if [[ -f "$LOG" ]]; then
  rm -f "$LOG.2"
  mv -f "$LOG.1" "$LOG.2" 2>/dev/null || true
  mv -f "$LOG" "$LOG.1"
fi

if [[ -f "$PIDFILE" ]]; then
  oldpid="$(cat "$PIDFILE" 2>/dev/null || true)"
  if [[ -n "$oldpid" ]] && kill -0 "$oldpid" 2>/dev/null; then
    kill "$oldpid" 2>/dev/null || true
    for _ in $(seq 1 25); do
      kill -0 "$oldpid" 2>/dev/null || break
      sleep 0.2
    done
  fi
  rm -f "$PIDFILE"
fi

# Configurable via environment variables:
#   MONITOR_PORT (8090), MONITOR_BIND (0.0.0.0), MONITOR_SERVER_URL,
#   MONITOR_JSONL, MONITOR_SERVE_LOG
PORT="${MONITOR_PORT:-8090}"
BIND="${MONITOR_BIND:-0.0.0.0}"
SERVER_URL="${MONITOR_SERVER_URL:-http://127.0.0.1:8080}"
JSONL="${MONITOR_JSONL:-$HOME/ninfer-requests.jsonl}"
SERVE_LOG="${MONITOR_SERVE_LOG:-$HOME/ninfer-serve.log}"

# setsid so the sidecar survives the launching shell/session exiting.
setsid nohup python3 "$DIR/monitor.py" \
  --port "$PORT" --bind "$BIND" \
  --server-url "$SERVER_URL" \
  --jsonl "$JSONL" \
  --serve-log "$SERVE_LOG" \
  --pidfile "$PIDFILE" \
  >> "$LOG" 2>&1 &

# Wait for the pidfile (monitor.py writes it at startup).
for _ in $(seq 1 50); do
  [[ -f "$PIDFILE" ]] && break
  sleep 0.2
done
if [[ -f "$PIDFILE" ]]; then
  echo "ninfer-monitor started (pid $(cat "$PIDFILE")) on :$PORT"
else
  echo "WARNING: sidecar did not write $PIDFILE; check $LOG" >&2
  exit 1
fi