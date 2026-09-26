# Request-log filter for tools/ops/ninfer-watch.sh: stdout = alerts, log file = every reuse record.
#
# The journal says what the ENGINE did (evictions, recoveries); this says what a CLIENT got. Two conditions
# are worth an alert, both reported by the operator on 2026-09-26 after seeing requests reuse only the
# ~23.7k shared prefix and re-prefill >40k:
#
#   1. LOW PREFIX USE on a reuse: the request took a reuse path, so it should not be re-prefilling much of
#      its prompt -- yet the cache hit is a small fraction of it.
#   2. SLOW REUSE: it took a reuse path and still spent more than `max_prefill_s` in prefill. Reuse is
#      supposed to buy that time back, so a slow reuse means the path did not deliver.
#
# BOTH THRESHOLDS ARE A FIRST CUT, not measured constants, and they are arguments so they can be tuned
# from the log rather than recompiled. `fraction` is deliberately high (0.9): the interesting cases are
# "reused something, re-prefilled a lot", and the operator's two examples were 76% and 36% hit.
#
# Every request_done with a non-root path is LOGGED, alert or not, so the thresholds can be set from data
# -- and so that "no alert" can be told from "no reuse happened at all" (the denominator rule).
select(.event == "request_done") |
(.result // {}) as $r |
($r.prompt_tokens // 0 | tonumber) as $prompt |
($r.prefix_cache_hit_tokens // 0 | tonumber) as $hit |
($r.prefix_reuse_path // "root") as $path |
($r.computed_prefill_tokens // 0 | tonumber) as $computed |
((.timings_seconds.prefill // 0) | tonumber) as $prefill |
(.request.request_id // "?") as $id |
"\($id) path=\($path) hit=\($hit)/\($prompt) computed=\($computed) prefill_s=\($prefill)"
  as $rec |
if $path == "root" then empty
elif ($prompt > 0 and ($hit / $prompt) < $fraction) then
  "[watch] LOW PREFIX USE on reuse: \($rec)"
elif $prefill > $max_prefill_s then
  "[watch] SLOW REUSE (prefill \($prefill)s > \($max_prefill_s)s): \($rec)"
else
  "[watch] reuse-ok: \($rec)"
end
