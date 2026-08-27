#!/bin/bash
# Stop the NInfer monitoring sidecar via its pidfile.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIDFILE="$DIR/monitor.pid"

if [[ ! -f "$PIDFILE" ]]; then
  echo "no pidfile at $PIDFILE; sidecar not running?"
  exit 0
fi
pid="$(cat "$PIDFILE" 2>/dev/null || true)"
if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
  kill "$pid" 2>/dev/null || true
  echo "stopped ninfer-monitor (pid $pid)"
else
  echo "pid $pid not running"
fi
rm -f "$PIDFILE"