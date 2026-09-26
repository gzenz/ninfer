#!/usr/bin/env bash
# Validate the watcher's filter against a synthetic journal: the alert set must be EXACTLY the expected
# lines. This exists because the monitor it replaces carried four tokens that matched nothing in `src/`
# (`WEDGE`, `planner-no-plan`, `relief-shared`, `host-state-pool`) and read like a quiet system -- so the
# filter is tested rather than trusted, and the test asserts the negative direction too.
#
# The fixture is chosen to contain the cases that made the FIRST version of this filter wrong:
#   * a shutdown `(fail-all)` residual, all-zero -> LOG ONLY (restart noise);
#   * a recovery followed by an all-zero residual -> LOG ONLY;
#   * `WORKER OOM` followed by a NON-ZERO residual -> ALERT. The first version armed only on
#     `WORKER RECOVER|WORKER CRASH`, so this -- the wedge signature -- went to the log;
#   * a NON-ZERO `(fail-all)` residual with nothing arming it -> ALERT (an accumulated leak exposed at stop);
#   * `demotable=1` -> ALERT, `demotable=0` -> log only (#6's evidence is not an error).
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat >"$work/journal" <<'EOF'
2026-09-26T17:14:55+02:00 Strix bash[1]: [engine] post-recovery residual (fail-all): main_kv_pages=0 backend_kv_pages=0 device_state_slots=0 host_state_slots=0 host_kv_bytes=0
2026-09-26T17:15:00+02:00 Strix bash[1]: [engine] checkpoint StateImage priced: incomplete=0 (numerator=0, denominator=1)
2026-09-26T17:15:01+02:00 Strix bash[1]: [engine] private victim evicted: demotable=0 frontier=12 endpoint=0 rewrite=0 anchors=0 host_state_slots=4/8 host_kv=1024/8589934592 checked=3 demotable_total=1
2026-09-26T17:15:02+02:00 Strix bash[1]: [engine] WORKER RECOVER: subtraction underflow
2026-09-26T17:15:03+02:00 Strix bash[1]: [engine] post-recovery residual (recover): main_kv_pages=0 backend_kv_pages=0 device_state_slots=0 host_state_slots=0 host_kv_bytes=0
2026-09-26T17:15:04+02:00 Strix bash[1]: [engine] private victim evicted: demotable=1 frontier=190 endpoint=1 rewrite=1 anchors=0 host_state_slots=0/8 host_kv=0/8589934592 checked=4 demotable_total=2
2026-09-26T17:15:05+02:00 Strix bash[1]: [engine] WORKER RECOVER: some other diagnosis
2026-09-26T17:15:06+02:00 Strix bash[1]: [engine] post-recovery residual (recover): main_kv_pages=117 backend_kv_pages=0 device_state_slots=0 host_state_slots=0 host_kv_bytes=8450000
2026-09-26T17:15:07+02:00 Strix bash[1]: [engine] admission stalled head=3 lanes_free=4
2026-09-26T17:15:08+02:00 Strix bash[1]: [engine] fail-all cleanup: shared catalogued=0 released-refused=0 skipped=0
2026-09-26T17:15:09+02:00 Strix bash[1]: [engine] some entirely unrelated line that no token matches
2026-09-26T17:15:10+02:00 Strix bash[1]: [engine] WORKER OOM: std::bad_alloc - recovering
2026-09-26T17:15:11+02:00 Strix bash[1]: [engine] post-recovery residual (recover): main_kv_pages=958 backend_kv_pages=0 device_state_slots=0 host_state_slots=1 host_kv_bytes=0
2026-09-26T17:15:12+02:00 Strix bash[1]: [engine] post-recovery residual (fail-all): main_kv_pages=117 backend_kv_pages=0 device_state_slots=0 host_state_slots=0 host_kv_bytes=8450000
2026-09-26T17:15:13+02:00 Strix bash[1]: [engine] WORKER OOM: out of memory - recovering
2026-09-26T17:15:14+02:00 Strix bash[1]: [engine] post-recovery residual (recover): main_kv_pages=958 backend_kv_pages=0 device_state_slots=0 host_state_slots=1 host_kv_bytes=0
EOF

gawk -v logpath="$work/log" -f ninfer-watch.awk <"$work/journal" >"$work/alerts"

cat >"$work/expected" <<'EOF'
2026-09-26T17:15:02+02:00 Strix bash[1]: [engine] WORKER RECOVER: subtraction underflow
2026-09-26T17:15:04+02:00 Strix bash[1]: [engine] private victim evicted: demotable=1 frontier=190 endpoint=1 rewrite=1 anchors=0 host_state_slots=0/8 host_kv=0/8589934592 checked=4 demotable_total=2
2026-09-26T17:15:05+02:00 Strix bash[1]: [engine] WORKER RECOVER: some other diagnosis
2026-09-26T17:15:06+02:00 Strix bash[1]: [engine] post-recovery residual (recover): main_kv_pages=117 backend_kv_pages=0 device_state_slots=0 host_state_slots=0 host_kv_bytes=8450000
2026-09-26T17:15:07+02:00 Strix bash[1]: [engine] admission stalled head=3 lanes_free=4
2026-09-26T17:15:10+02:00 Strix bash[1]: [engine] WORKER OOM: std::bad_alloc - recovering
2026-09-26T17:15:11+02:00 Strix bash[1]: [engine] post-recovery residual (recover): main_kv_pages=958 backend_kv_pages=0 device_state_slots=0 host_state_slots=1 host_kv_bytes=0
2026-09-26T17:15:12+02:00 Strix bash[1]: [engine] post-recovery residual (fail-all): main_kv_pages=117 backend_kv_pages=0 device_state_slots=0 host_state_slots=0 host_kv_bytes=8450000
2026-09-26T17:15:13+02:00 Strix bash[1]: [engine] WORKER OOM: out of memory - recovering
2026-09-26T17:15:14+02:00 Strix bash[1]: [engine] post-recovery residual (recover): main_kv_pages=958 backend_kv_pages=0 device_state_slots=0 host_state_slots=1 host_kv_bytes=0
EOF

total=$(wc -l <"$work/journal")
rc=0
if diff -u "$work/expected" "$work/alerts" >"$work/diff"; then
  echo "[watch-test] alerts PASS: $(wc -l <"$work/alerts") of $total lines alerted"
else
  echo "[watch-test] alerts FAIL -- expected vs actual:" >&2
  cat "$work/diff" >&2
  rc=1
fi

# The log must carry the insight lines the alert stream deliberately drops, or "log-only" means "lost".
for t in 'checkpoint StateImage priced' 'demotable=0' 'fail-all cleanup' 'post-recovery residual (fail-all): main_kv_pages=0'; do
  if grep -q -- "$t" "$work/log"; then
    echo "[watch-test] log keeps: $t"
  else
    echo "[watch-test] log MISSING: $t" >&2
    rc=1
  fi
done

# And the denominator: the filter must not have swallowed the whole stream.
n=$(wc -l <"$work/log")
if [ "$n" -ge 10 ]; then
  echo "[watch-test] log lines=$n of $total (stream was not swallowed)"
else
  echo "[watch-test] log lines=$n of $total -- too few, the filter dropped lines it should keep" >&2
  rc=1
fi

exit "$rc"
