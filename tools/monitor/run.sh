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

# setsid so the sidecar survives the launching shell/session exiting.
setsid nohup python3 "$DIR/monitor.py" \
  --port 8090 --bind 0.0.0.0 \
  --server-url http://127.0.0.1:8080 \
  --jsonl "$HOME/ninfer-requests.jsonl" \
  --serve-log "$HOME/ninfer-serve.log" \
  --pidfile "$PIDFILE" \
  >> "$LOG" 2>&1 &

# Wait for the pidfile (monitor.py writes it at startup).
for _ in $(seq 1 50); do
  [[ -f "$PIDFILE" ]] && break
  sleep 0.2
done
if [[ -f "$PIDFILE" ]]; then
  echo "ninfer-monitor started (pid $(cat "$PIDFILE")) on :8090"
else
  echo "WARNING: sidecar did not write $PIDFILE; check $LOG" >&2
  exit 1
fi