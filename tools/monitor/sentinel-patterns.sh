# Journal patterns for the wedge sentinel. ONE DEFINITION, sourced by `wedge-sentinel.sh` AND by
# `tools/ops/sentinel-pattern-test.sh`, so the test cannot drift from the detector it validates. A duplicated
# pattern here would be the same defect one level up.
#
# WHY THIS FILE EXISTS. The sentinel's two journal fallbacks were written against a journal format the engine
# does not emit, and both matched NOTHING on any input, ever:
#
#   old grep:  "throughput interval"                    real: "throughput |"
#   old parse: "prefill=14.9tok/s decode=17.8tok/s"     real: "prefill 14.9 tok/s (2,617 tok) | ..."
#   old gauges: "running=2 prefilling=2 waiting=6 ..."  real: "running 2 (prefill 2) | waiting 6"
#
# Measured 2026-10-04: 91 real `throughput ` lines that day, 0 matches for the grep string, empty extraction
# from both parsers. The consequences compounded into a restart of a HEALTHY engine:
#   * during a long engine step /stats blocks on the execution mutex and times out (both ports);
#   * with the gauge fallback dead, `state` was empty, so no arm was possible -- and no progress was credited;
#   * with the progress fallback dead, `c_last_advance` went stale while 25 requests COMPLETED;
#   * the moment /stats answered again, the arm fired against that stale timestamp.
# At 14:47:45 it armed, captured a healthy engine (one thread R, the rest on futex), and restarted ninfer at
# 14:48:13, dropping 20 live continuations.
#
# THE LESSON THIS FILE CODES AROUND: a pattern that matches nothing reads exactly like a quiet system. These
# are therefore TESTED against real captured lines (positive AND negative controls) rather than trusted.

# A journal line that carries scheduler gauges or token rates. The periodic reporter AND every completed
# request: a completion is progress too, and 25 of them went unseen inside the window above.
SENTINEL_STATE_LINES_RE='throughput \||req#[0-9]+ done \|'

# A `prefill` or `decode` rate SEGMENT. THE PRESENCE OF THE SEGMENT IS THE TEST, not a non-zero value, and
# the reason is in the engine: `src/serve/operational_log.cpp:350,354` emits each segment ONLY when its token
# count is non-zero, so a segment implies tokens were committed and the engine computed. Requiring `> 0` on
# top of that was additionally WRONG -- `decode 0.0 tok/s (1 tok)` is a real shape (one token across a long
# reporter interval rounds to 0.0) and it IS progress, which the non-zero filter rejected. The producible
# NEGATIVE is a line with no rate segment at all (`throughput | 5.0s | running 0 | waiting 3`), because an
# all-zero report is suppressed rather than printed with zeros.
SENTINEL_PROGRESS_RATE_RE='(prefill|decode) [0-9.]+k? tok/s'

# Gauge fields, from the `throughput |` line only (a `done |` line carries no gauges).
SENTINEL_RUNNING_RE='(^|[^a-z-])running ([0-9]+)'
SENTINEL_WAITING_RE='waiting ([0-9]+)'
# THE PARENTHETICAL IS SHARED WHEN BOTH PHASES ARE ACTIVE. `running 2 (prefill 1, decode-ready 1)` is 49 of
# the 101 `running N (...)` lines on 2026-10-04, and a pattern requiring `)` right after the digits misses
# every one of them -- which is what this pair did until a review measured it. `[,)]` covers both shapes.
# `[(,]` and NOT `\(`: in the shared form the SECOND phase does not follow an opening paren at all --
# `(prefill 1, decode-ready 1)` puts a comma before `decode-ready`, so a pattern anchored on `(` matched the
# first phase and silently missed the second. Caught by the SHARED-parenthetical case above, which exists
# because a review found the first version's test had no such line and so could not fail on this.
SENTINEL_PREFILLING_RE='[(,] *prefill ([0-9]+)[,)]'
SENTINEL_DECODE_READY_RE='[(,] *decode-ready ([0-9]+)[,)]'
