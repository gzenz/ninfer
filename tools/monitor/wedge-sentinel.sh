#!/bin/bash
# ninfer wedge sentinel (v3.4, 2026-09-18).
#
# Restarts ninfer.service when the engine is wedged: work outstanding, no
# engine progress. Three classes per poll (15s):
#
#   Class A: request queued, engine idle        (waiting>=1, materializing=0,
#                                                r=p=d=0)
#   Class B: stuck in deferred materialization  (materializing>=1, r=p=d=0)
#   Class C (v3): work in flight/queued but no engine progress. The
#          2026-09-17 12:20 zombie: a fatal CUDA error (cudaStreamSynchronize
#          -> SIGABRT) froze the engine mid-prefill. A/B never armed — they
#          need r=p=d=0, but the zombie last reported prefilling=1 and then
#          went fully silent for 13 min. C arms when, with work in
#          flight/queued, there has been no engine progress for 150s.
#          Progress evidence (v3.1): a /stats "counters" advance (prefill/
#          decode tokens, rounds, capture completions) OR a fresh journal
#          throughput line with non-zero tok/s. v3.4 adds a third source:
#          fresh [relief-kv] / [admission] queued KV block lines — a queued
#          demand converging under the fit gate (15s relief bursts freeing
#          pages) shows zero tok/s and flat counters while the engine works,
#          and is bounded by the request's own 120s deadline. Any real
#          engine progress shows up in at least one of the three at the 15s
#          poll scale, so flat evidence with work outstanding is a wedge —
#          including a frozen prefilling=1 (the 12:20 case) and a fully
#          silent process.
#
# Signal sources per poll, in order:
#   1. HTTP /stats — scheduler gauges + counters sum.
#   2. journal "throughput interval" line within 60s — fallback for the
#      gauges. The line's NON-ZERO prefill/decode tok/s is ALSO progress
#      evidence for C (v3.1): the 5s reporter samples the same cumulative
#      counters on its own thread, so a non-zero rate proves the counters
#      advanced even when the /stats poll failed (the HTTP pool can be
#      saturated during a long engine step — 2026-09-17 21:10 misfire: a
#      360k-token prefill armed C at 153s while the journal showed a healthy
#      ~1000 tok/s prefill throughout). A wedged engine cannot fake this:
#      it either goes silent (no fresh line) or logs 0.0 tok/s.
#   3. no signal at all — A/B hold (a wedged engine goes silent, so absence
#      of data must NOT clear an armed timer; v1 bug #1). For C, silence
#      with work outstanding IS the no-progress signal (c_last_advance
#      simply stops advancing).
#
# Fresh-server reset: journal "listening on http" within 120s => a (re)start
# just happened — disarm all classes, clear the progress baseline, enter a
# 90s grace. Covers our own restarts AND systemd's on-failure auto-restarts
# (which v2's grace_until never saw — v2 could have restarted a still-
# loading server after a crash).
#
# The 150s threshold MUST exceed the engine's 120s fit-gate defer deadline:
# a deferring request self-aborts at 120s, drains the gauges (the all-zero
# line disarms C), and completes before C's threshold. Progress evidence is
# a /stats counter advance OR a fresh journal line with non-zero tok/s, so a
# healthy busy server — including a multi-minute prefill — never arms C.
#
# Arm/disarm:
#   A/B (v2): active line (r|p|d>=1) -> disarm; all-zero line -> disarm;
#            pending-only line -> arm; restart at arm+150s.
#   C: progress (counter advance or journal non-zero tok/s) -> stops the
#      staleness; all-zero line -> disarm; arm requires 150s of stale
#      progress with work outstanding, restart at arm+15s (total ~165s
#      from wedge start).
#
# Deployment: the running process never picks up on-disk edits — bash
# parses the whole `while` compound at startup. Any change to this file
# requires `systemctl restart ninfer-wedge-sentinel.service`.
#
# Safety rails:
# - 90s grace after a fresh server (model load window) — no restarts.
# - 3 restarts within 30 minutes (any class) stops auto-restart and alerts.
PORT=8080
STATS_PORT=8081   # dedicated single-thread /stats server (--stats-port); never
                  # saturates behind streaming handlers. Main port is the
                  # fallback (old binaries without --stats-port).
if [ "$(id -u)" != "0" ]; then JC="sudo -n journalctl"; else JC="journalctl"; fi

poll_stats() {
  # prints "r p d w m counters_sum" or nothing. Tries the dedicated stats
  # port first: a /stats poll on the main port can time out while the shared
  # worker pool is saturated by streaming handlers spanning a long prefill
  # (2026-09-17 21:10 misfire contributor); the dedicated port cannot.
  local body
  body=$(curl -s --max-time 5 "http://127.0.0.1:$STATS_PORT/stats" 2>/dev/null)
  [ -n "$body" ] || body=$(curl -s --max-time 5 "http://127.0.0.1:$PORT/stats" 2>/dev/null)
  [ -n "$body" ] && printf '%s' "$body" | python3 -c '
import json, sys
try:
    s = json.load(sys.stdin)
    sc = s.get("scheduler", {})
    c = s.get("counters", {})
    total = sum(c.get(k, 0) for k in
        ("computed_prefill_tokens", "committed_decode_tokens",
         "decode_rounds", "decode_row_rounds",
         "active_captures_completed", "active_captures_aborted"))
    print(sc.get("running", 0), sc.get("prefilling", 0),
          sc.get("decode_ready", 0), sc.get("waiting", 0),
          sc.get("materializing", 0), total)
except Exception:
    sys.exit(1)
' 2>/dev/null
}

poll_journal_state() {
  # prints "r p d w m" from the newest journal throughput line, or nothing
  $JC -u ninfer.service --since "60 sec ago" --no-pager 2>/dev/null \
    | grep "throughput interval" | tail -1 \
    | sed -nE 's/.*running=([0-9]+) prefilling=([0-9]+) decode_ready=([0-9]+) waiting=([0-9]+) materializing=([0-9]+).*/\1 \2 \3 \4 \5/p'
}

journal_progress() {
  # exit 0 if the newest journal throughput line within 60s shows non-zero
  # prefill or decode tok/s — the engine is demonstrably computing. The 5s
  # reporter samples the same cumulative counters the /stats poll reads, on
  # its own thread, so a non-zero rate proves counter advance even when the
  # /stats poll failed (HTTP pool saturated during a long engine step).
  # A wedged engine cannot satisfy this: it goes silent (no fresh line) or
  # logs 0.0 tok/s.
  local rates
  rates=$($JC -u ninfer.service --since "60 sec ago" --no-pager 2>/dev/null \
    | grep "throughput interval" \
    | sed -nE 's/.* prefill=([0-9]+\.[0-9]+)tok\/s decode=([0-9]+\.[0-9]+)tok\/s.*/\1 \2/p' \
    | tail -1)
  [ -n "$rates" ] && \
    awk -v r="$rates" 'BEGIN { split(r, a, " "); exit !(a[1] > 0 || a[2] > 0) }'
}

journal_relief_progress() {
  # exit 0 if a fresh (within 90s) [relief-kv] / [admission] queued KV block
  # line exists — the fit-gate/relief machinery is actively working a queued
  # demand. That is real engine progress invisible to BOTH other evidence
  # sources: while a large request waits in the queue, the tok/s lines log
  # 0.0 (no token work) and the /stats counters stay flat, but the 15s relief
  # bursts demote units, free pages, and the demand converges — bounded by
  # the request's own 120s fit-gate deadline, which is under this sentinel's
  # 150s threshold. A wedged engine cannot emit these lines (the scheduler
  # loop is frozen). 2026-09-18 20:53: request 36 (250k tokens) queued at
  # 20:50:50, relief freed 18-34 pages every 15s (free 3409->3473), the
  # request fit and was admitted at 111.9s — healthy throughout — yet C
  # armed at 150s (20:53:35, token-centric evidence only) and the restart
  # killed a healthy prefill ~4s after admission.
  $JC -u ninfer.service --since "90 sec ago" --no-pager 2>/dev/null \
    | grep -qE '\[relief-kv\]|\[admission\] queued KV block'
}

engine_progress() { journal_progress || journal_relief_progress; }

fresh_server() {
  $JC -u ninfer.service --since "120 sec ago" --no-pager 2>/dev/null \
    | grep -q "listening on http"
}

armed_since=0       # A/B
c_armed_since=0     # C
last_progress=-1
c_last_advance=0    # last counter advance (0 = none seen since reset)
grace_until=0
restart_count=0
window_start=0
stopped=0

while true; do
  now=$(date +%s)

  # fresh (re)start seen -> full reset + grace
  if fresh_server; then
    armed_since=0; c_armed_since=0; last_progress=-1; c_last_advance=0
    grace_until=$((now + 90))
  fi

  state=""; csum=""
  s=$(poll_stats)
  if [ -n "$s" ]; then
    read -r r p d w m csum <<< "$s"
    state="$r $p $d $w $m"
    if [ "$last_progress" = "-1" ] || [ "$csum" -gt "$last_progress" ]; then
      last_progress=$csum
      c_last_advance=$now
    fi
    # v3.3: a flat counter sum is NOT no-progress during a long single
    # prefill — computed_prefill_tokens commits at completion, so a
    # 250k-token cold prefill (150s+) holds the /stats counters flat while
    # the 5s reporter's throughput line shows non-zero tok/s throughout.
    # /stats succeeding only proves the HTTP pool is alive, not that the
    # engine is computing, so the journal evidence must be consulted on
    # EVERY poll, not just when /stats fails (v3.1 wired it into the
    # failure path only). 2026-09-18 20:53 misfire: a 250k cold prefill
    # (post-compaction re-prefill) armed C at 150s of flat counters
    # mid-prefill and the restart killed it ~10s before completion. A
    # wedged engine still cannot satisfy this: it goes silent (no fresh
    # line) or logs 0.0 tok/s.
    # v3.4: relief/queued-block lines are progress too (see
    # journal_relief_progress) — a queued demand converging under the fit
    # gate shows zero tok/s and flat counters while the engine works.
    engine_progress && c_last_advance=$now
  else
    state=$(poll_journal_state)
    # v3.1: the /stats poll failed — a fresh journal line with non-zero tok/s
    # still proves engine progress (2026-09-17 21:10 misfire: long prefill,
    # /stats unreachable, journal showed a healthy ~1000 tok/s throughout).
    engine_progress && c_last_advance=$now
  fi
  echo "$state" | grep -qE '^[0-9]+( [0-9]+){4}$' || state=""

  if [ -n "$state" ]; then
    read -r r p d w m <<< "$state"
    work=0
    { [ "$r" -ge 1 ] || [ "$p" -ge 1 ] || [ "$d" -ge 1 ] || [ "$w" -ge 1 ] || [ "$m" -ge 1 ]; } && work=1
    # A/B (v2 logic, unchanged)
    if [ "$r" -ge 1 ] || [ "$p" -ge 1 ] || [ "$d" -ge 1 ]; then
      armed_since=0
    elif [ "$w" -ge 1 ] || [ "$m" -ge 1 ]; then
      [ "$armed_since" = "0" ] && armed_since=$now
    else
      armed_since=0
      c_armed_since=0               # all-zero line drains work -> C disarms
    fi
    # C arm: work outstanding + >=150s without a counter advance
    if [ "$work" = "1" ] && [ "$c_last_advance" != "0" ] \
       && [ $((now - c_last_advance)) -ge 150 ] && [ "$c_armed_since" = "0" ]; then
      c_armed_since=$now
      echo "WEDGE-C ARMED: work in flight (r=$r p=$p d=$d w=$w m=$m), no engine progress for $((now - c_last_advance))s — restart in 15s"
    fi
  fi
  # no signal: A/B hold; C holds (c_last_advance simply goes stale)

  # restart decision (shared by all classes)
  ab_due=0; cd_due=0
  [ "$armed_since" != "0" ] && [ $((now - armed_since)) -ge 150 ] && ab_due=1
  [ "$c_armed_since" != "0" ] && [ $((now - c_armed_since)) -ge 15 ] && cd_due=1
  if { [ "$ab_due" = "1" ] || [ "$cd_due" = "1" ]; } \
     && [ "$now" -ge "$grace_until" ] && [ "$stopped" = "0" ]; then
    if [ "$window_start" = "0" ] || [ $((now - window_start)) -ge 1800 ]; then
      window_start=$now
      restart_count=0
    fi
    restart_count=$((restart_count + 1))
    if [ "$restart_count" -ge 3 ]; then
      stopped=1
      echo "WEDGE REPEAT: 3 restarts in 30 min — auto-restart stopped, needs investigation"
    else
      [ "$ab_due" = "1" ] && echo "WEDGE (A/B): engine idle with work pending $((now - armed_since))s — restarting ninfer (restart #$restart_count)"
      [ "$cd_due" = "1" ] && echo "WEDGE-C: no engine progress with work in flight — restarting ninfer (restart #$restart_count)"
      sudo -n systemctl restart ninfer.service 2>/dev/null || systemctl restart ninfer.service
      armed_since=0; c_armed_since=0
      c_last_advance=0; last_progress=-1
      grace_until=$((now + 90))
    fi
  fi
  sleep 15
done
