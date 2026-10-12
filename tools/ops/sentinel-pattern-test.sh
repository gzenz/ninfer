#!/usr/bin/env bash
# Reachability test for the wedge sentinel's journal patterns.
#
# WHY THIS EXISTS: the sentinel's two journal fallbacks matched NOTHING for their whole lives
# (`tools/monitor/sentinel-patterns.sh` records the wrong-vs-real format), so the safety net silently lost its
# only progress evidence during a /stats outage and restarted a healthy engine on 2026-10-04. A pattern that
# matches nothing reads exactly like a quiet system.
#
# THE FIXTURES ARE THE GUARD; the live journal is a bonus. A review of the first version found the live check
# was `check x x "..."` -- an assertion that compared a value to itself and could never fail -- and that it
# ALSO failed on a healthy QA whose newest line happened to be a cancelled request. Both are fixed: the live
# check is now a COUNT that fails on zero, and it skips visibly when the window holds no state lines, so the
# hermetic fixture controls -- not the ambient journal -- are what a dead pattern trips.
#
# WHICH LINES ARE REAL AND WHICH ARE CONSTRUCTED, since the first version's header claimed all were captured
# and three of them could not occur at all. Real = copied from the journal with only the timestamp shortened.
# Constructed = written to be PRODUCIBLE from the engine's own emitters, and flagged as such at each case.
#
# The patterns are SOURCED from the detector, never copied, so this test cannot drift from it.
# Host-only, no GPU, no service.

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MONITOR_DIR="$(cd "$HERE/../monitor" && pwd)"

# shellcheck source=tools/monitor/sentinel-patterns.sh
. "$MONITOR_DIR/sentinel-patterns.sh"

pass=0
fail=0
skip=0
check() { # check <expected> <actual> <what>
  if [ "$1" = "$2" ]; then
    pass=$((pass + 1))
    printf '  ok   %-54s %s\n' "$3" "$2"
  else
    fail=$((fail + 1))
    printf '  FAIL %-54s expected %s, got %s\n' "$3" "$1" "$2"
  fi
}
check_ge() { # check_ge <min> <actual> <what> -- CAN FAIL, unlike an x-vs-x comparison
  if [ "$2" -ge "$1" ]; then
    pass=$((pass + 1))
    printf '  ok   %-54s %s (>= %s)\n' "$3" "$2" "$1"
  else
    fail=$((fail + 1))
    printf '  FAIL %-54s %s (needed >= %s)\n' "$3" "$2" "$1"
  fi
}

# The production code path, per line: select the line shapes, then require a rate SEGMENT.
progress() {
  printf '%s\n' "$1" | grep -E "$SENTINEL_STATE_LINES_RE" | grep -qE "$SENTINEL_PROGRESS_RATE_RE" \
    && echo yes || echo no
}
# poll_journal_state's extraction: gauges come from a `throughput |` line only, and there are five fields.
gauges() {
  local line="$1" r w pf dr
  printf '%s\n' "$line" | grep -q 'throughput |' || { echo ""; return 0; }
  r=$(printf '%s' "$line" | sed -nE "s/.*$SENTINEL_RUNNING_RE.*/\2/p")
  w=$(printf '%s' "$line" | sed -nE "s/.*$SENTINEL_WAITING_RE.*/\1/p")
  pf=$(printf '%s' "$line" | sed -nE "s/.*$SENTINEL_PREFILLING_RE.*/\1/p")
  dr=$(printf '%s' "$line" | sed -nE "s/.*$SENTINEL_DECODE_READY_RE.*/\1/p")
  [ -n "$r" ] || { echo ""; return 0; }
  printf '%s %s %s %s %s\n' "$r" "${pf:-0}" "${dr:-0}" "${w:-0}" 0
}

# --- REAL lines (copied from the journal; only the leading timestamp is shortened) -------------------
L_PREFILL_K='2026-10-04 14:49:44.687  INFO  throughput | 10.1s | prefill 3.59k tok/s (36,149 tok) | running 2 (prefill 2) | waiting 6'
L_BOTH='2026-10-04 14:47:48.533  INFO  throughput | 2m 55.9s | prefill 14.9 tok/s (2,617 tok) | decode 17.8 tok/s (3,125 tok) | running 2 (decode-ready 2) | waiting 8'
L_MIXED='2026-10-04 14:38:53.548  INFO  throughput | 25.1s | prefill 2.37k tok/s (59,425 tok) | decode 231.3 tok/s (5,797 tok) | running 2 (prefill 1, decode-ready 1) | waiting 10'
L_DONE='2026-10-04 14:47:43.228  INFO  req#136 done | anthropic | tool calls 1 | prompt 61,638 | output 2,090 | queue 55.4s | prefill 1.94k tok/s | decode 149.2 tok/s | dflash2 accepted 1,418/4,711'
L_CANCELLED='2026-10-04 14:51:18.816  INFO  req#24 done | anthropic | cancelled | prompt 61,868 | output 0'
L_START='2026-10-04 14:47:45.117  INFO  req#152 started | anthropic stream | 28 messages'
# --- CONSTRUCTED lines, each producible from the engine's own emitters, and each flagged ------------
# NO rate segment because BOTH token counters are 0, which suppresses both segments
# (operational_log.cpp:350,354) and `report_has_activity` suppresses the all-zero report entirely.
# This REPLACES the first version's `prefill 0.00 tok/s` negative, which was IMPOSSIBLE to emit.
L_NO_RATE='2026-10-04 14:50:00.000  INFO  throughput | 5.0s | running 0 | waiting 3'
# PRODUCIBLE and a POSITIVE: one commitd decode token across a 25 s interval rates as 0.0, and the engine
# emits the segment because the COUNT is non-zero. The old non-zero filter rejected this.
L_RATE_ZERO='2026-10-04 14:50:00.000  INFO  throughput | 25.1s | decode 0.0 tok/s (1 tok) | running 1 (decode-ready 1) | waiting 0'

echo "POSITIVE CONTROLS — a working engine must read as progress"
check yes "$(progress "$L_PREFILL_K")" "throughput, prefilling only, k suffix (real)"
check yes "$(progress "$L_BOTH")" "throughput, prefill+decode (real)"
check yes "$(progress "$L_MIXED")" "throughput, SHARED parenthetical (real, 49/101 lines)"
check yes "$(progress "$L_DONE")" "req#N done line — a completion is progress (real)"
check yes "$(progress "$L_RATE_ZERO")" "decode 0.0 tok/s (1 tok) — zero RATE, non-zero COUNT (constructed)"

echo "NEGATIVE CONTROLS — a wedged or idle engine must NOT read as progress"
check no "$(progress "$L_NO_RATE")" "throughput with NO rate segment (constructed, producible)"
check no "$(progress "$L_CANCELLED")" "req#N done cancelled (real)"
check no "$(progress "$L_START")" "a started line (real)"
check no "$(progress "2026-10-04 15:00:00  INFO  [relief-kv] freed 18 pages")" "a relief line (no rate)"

echo "GAUGE EXTRACTION — poll_journal_state, five fields"
check "2 2 0 6 0" "$(gauges "$L_PREFILL_K")" "prefilling-only parenthetical"
check "2 0 2 8 0" "$(gauges "$L_BOTH")" "decode-ready-only parenthetical"
check "2 1 1 10 0" "$(gauges "$L_MIXED")" "SHARED parenthetical — both phases at once"
check "" "$(gauges "$L_DONE")" "a done line carries no gauges"
check "0 0 0 3 0" "$(gauges "$L_NO_RATE")" "idle line: running 0, waiting 3"

echo "THE DEAD PATTERNS, PINNED — a revert to either must fail here"
dead_hits=$(printf '%s\n%s\n%s\n' "$L_PREFILL_K" "$L_BOTH" "$L_DONE" | grep -c "throughput interval")
check 0 "$dead_hits" "old grep string 'throughput interval' matches nothing"
dead_parse=$(printf '%s\n' "$L_BOTH" | sed -nE 's/.* prefill=([0-9]+\.[0-9]+)tok\/s decode=([0-9]+\.[0-9]+)tok\/s.*/\1 \2/p' | wc -l)
check 0 "$dead_parse" "old sed 'prefill=Xtok/s' extracts nothing"

echo "LIVE JOURNAL — a COUNT that can fail, and a visible SKIP when there is nothing to count"
JC_BIN="${JC:-journalctl}"
state_lines=$($JC_BIN -u ninfer.service --since "6 hours ago" --no-pager 2>/dev/null | grep -cE "$SENTINEL_STATE_LINES_RE")
if [ "$state_lines" -gt 0 ]; then
  progress_lines=$($JC_BIN -u ninfer.service --since "6 hours ago" --no-pager 2>/dev/null \
    | grep -E "$SENTINEL_STATE_LINES_RE" | grep -cE "$SENTINEL_PROGRESS_RATE_RE")
  check_ge 1 "$progress_lines" "real lines reading as progress, of $state_lines matched"
else
  skip=$((skip + 1))
  printf '  SKIP %-54s no state lines in 6h — a dead pattern and a stopped engine look the same here\n' \
    "live journal"
fi

echo
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ] || exit 1
