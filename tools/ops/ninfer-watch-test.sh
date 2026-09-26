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
2026-09-26T18:39:12+02:00 Strix systemd[1]: ninfer.service: Main process exited, code=killed, status=9/KILL
2026-09-26T18:39:16+02:00 Strix systemd[1]: ninfer.service: Scheduled restart job, restart counter is at 1.
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
2026-09-26T18:39:12+02:00 Strix systemd[1]: ninfer.service: Main process exited, code=killed, status=9/KILL
2026-09-26T18:39:16+02:00 Strix systemd[1]: ninfer.service: Scheduled restart job, restart counter is at 1.
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
# --- the request-log leg (tools/ops/ninfer-watch-requests.jq) -----------------
# The cases that make the filter WRONG are asserted, including the negatives. Two of them are the
# false alarms that a standalone "prefill > 2s" rule produced on live traffic (92.4% and 89.3% hit,
# 2.6-3.1 s): at fraction 0.9 the 92.4% one must be silent, and the 89.3% one alerts only because it is
# just under the fraction -- which is the rule doing its job, not the slow rule misfiring.
cat >"$work/requests.jsonl" <<JSONL
{"event":"request_done","request":{"request_id":"sharedonly"},"result":{"prompt_tokens":89215,"prefix_cache_hit_tokens":23706,"prefix_reuse_path":"shared_stable_prefix","computed_prefill_tokens":65509},"timings_seconds":{"prefill":19.2}}
{"event":"request_done","request":{"request_id":"goodslow"},"result":{"prompt_tokens":87848,"prefix_cache_hit_tokens":81208,"prefix_reuse_path":"shared_stable_prefix","computed_prefill_tokens":6640},"timings_seconds":{"prefill":2.61}}
{"event":"request_done","request":{"request_id":"justunder"},"result":{"prompt_tokens":82872,"prefix_cache_hit_tokens":74000,"prefix_reuse_path":"shared_stable_prefix","computed_prefill_tokens":8872},"timings_seconds":{"prefill":3.09}}
{"event":"request_done","request":{"request_id":"rootpath"},"result":{"prompt_tokens":65059,"prefix_cache_hit_tokens":0,"prefix_reuse_path":"root","computed_prefill_tokens":65059},"timings_seconds":{"prefill":14.5}}
JSONL
jq -rc --argjson fraction 0.9 --argjson max_prefill_s 2.0 -f ninfer-watch-requests.jq <"$work/requests.jsonl" >"$work/reqout"
printf '%s\n' \
 '[watch] LOW PREFIX USE on reuse (SLOW): sharedonly path=shared_stable_prefix hit=23706/89215 computed=65509 prefill_s=19.2' \
 '[watch] reuse-ok: goodslow path=shared_stable_prefix hit=81208/87848 computed=6640 prefill_s=2.61' \
 '[watch] LOW PREFIX USE on reuse (SLOW): justunder path=shared_stable_prefix hit=74000/82872 computed=8872 prefill_s=3.09' \
 >"$work/reqexpected"
if diff -u "$work/reqexpected" "$work/reqout" >"$work/reqdiff"; then
  echo "[watch-test] requests PASS: $(grep -c . "$work/reqout") of 4 classified"
else
  echo "[watch-test] requests FAIL:" >&2
  cat "$work/reqdiff" >&2
  rc=1
fi
if grep -q 'rootpath' "$work/reqout"; then
  echo "[watch-test] requests FAIL: a root-path request produced output (nothing was reused)" >&2
  rc=1
else
  echo "[watch-test] requests: root path correctly silent"
fi
if grep -q 'goodslow' "$work/reqout" && grep -q 'reuse-ok: goodslow' "$work/reqout"; then
  echo "[watch-test] requests: the 92.4% + 2.6s false alarm is silenced"
else
  echo "[watch-test] requests FAIL: the 92.4%/2.6s case must not alert" >&2
  rc=1
fi

exit "$rc"
