# Request-log filter for tools/ops/ninfer-watch.sh: stdout = alerts, log file = every reuse record.
#
# The journal says what the ENGINE did (evictions, recoveries); this says what a CLIENT got. One rule, so
# that an alert means "reuse did not deliver" and nothing else:
#
#   LOW PREFIX USE -- the request took a reuse path, so it should not be re-prefilling much of its prompt,
#   yet its cache hit is a small fraction of it. The prefill time is reported as an ATTRIBUTE of that
#   alert, marked SLOW when it exceeds `max_prefill_s`.
#
# WHY SLOWNESS IS NOT ITS OWN ALARM (changed 2026-09-26 after measuring, not after reasoning): a standalone
# "prefill > 2s on a reuse" rule was tried first and it flagged healthy traffic --
#
#   hit=81208/87848 (92.4%) prefill_s=2.61   <- alerted, and this is a normal re-prefill
#   hit=74000/82872 (89.3%) prefill_s=3.09   <- alerted, also normal
#
# A few thousand tokens of re-prefill at 2-3 s is throughput, not a symptom. What the rule was reaching for
# is "reuse FAILED and cost us time", which is the low-use case: gating slow on low-use then leaves a
# second branch unreachable (the first would already have fired), and a branch that cannot fire is the
# defect this repo keeps recording. So there is one alert, and slowness is carried inside it.
#
# THRESHOLDS: `fraction` 0.9 is a first cut -- the operator's reported cases were 36% and 76% hit -- and it
# is deliberately high, because "reused something, re-prefilled a lot" is the interesting shape. Both
# thresholds are arguments so they can be set from the log rather than recompiled. Every request_done with a
# non-root path is LOGGED either way, so "no alert" can be told from "no reuse happened at all".
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
  "[watch] LOW PREFIX USE on reuse\(if $prefill > $max_prefill_s then " (SLOW)" else "" end): \($rec)"
else
  "[watch] reuse-ok: \($rec)"
end
