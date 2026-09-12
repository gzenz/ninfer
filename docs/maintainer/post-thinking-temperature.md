# Post-thinking sampling temperature: measured behaviour, and the case for deferring ReSET

Scope: the post-thinking sampler (the sampling parameters that take effect once the model closes its
reasoning block), measured on Qwen3.8-27B QUASAR NVFP4 on one RTX 5090 at Git `b802c515`. This note
records seven experiments across three accuracy benchmarks plus a temperature-response sweep and a
reproducibility measurement, together with a measurement defect discovered on the way, the literature
position, and the resulting recommendation.

"ReSET" throughout names a published decoding-time temperature-control method for NVFP4 reasoning
models - Lee et al., [arXiv:2606.13233](https://arxiv.org/abs/2606.13233) - and the final section
assesses whether pursuing it is worthwhile in this setup.

## Summary

- The knob changes **which** tokens are emitted (90.3% of plans produce four different emissions
  across a temperature ladder) but changes **no** aggregate: answer length, tool-call validity,
  prose degeneration and accuracy are all flat.
- Accuracy: no measurable effect on any of three benchmarks - BFCL v4 tool calling (n=1240 paired,
  McNemar p=0.79), AIME (60 problems per arm, inside a +/-4 pp band), IFBench (300 samples per arm,
  0.2 and 1.0 identical).
- One measurable benefit: the **reproducibility** of long free-form answers under numerical-route
  variation (100% -> 62.5% distinct answers at identical route variation; mean pairwise similarity
  0.389 -> 0.629). Short structured output is insensitive at every temperature.
- Conclusion: the shipped `post_thinking` temperature of 0.2 is a **reliability knob, not a quality
  knob**, and it only acts on long output. It is not justified as an accuracy improvement by any
  measurement we have.
- A faithful ReSET-style experiment should be **deferred** for this use case and setup: the damage
  pool it targets is not observable on our stack, and our artifact is already quantization-aware
  trained. Details in the final section.

## Measurement methodology, and a defect we found

The sampler is a pure positional hash of `(seed, position, purpose, sub)` with no mutable RNG state,
which invites a paired design: same seed and same thinking parameters should give a bit-identical
reasoning block, leaving the emission phase as the only difference between arms. Two controls show
that this is **not** safe in this serving configuration.

| control: one identical request repeated, seed 42 | distinct reasoning blocks | distinct emission |
|---|---:|---:|
| sequential, one request in flight | 1 | 1 |
| concurrent, two requests in flight | 3 | 1 |

Identical requests reproduce only when the execution route is identical. Prefix caching does the same
thing: in 28 of 150 ladder groups only the first (cold-prefill) request differed while the three
cache-warm ones agreed, and two of six prompts returned different reasoning for identical requests
across rounds even at concurrency 1.

Consequences for this document:
- the BFCL A/B is a valid comparison of two configurations but **not** a token-paired measurement;
- every paired claim below is conditioned on identical reasoning, and where a pair is not comparable
  it is excluded rather than silently averaged in.

This is the known **batch-invariance** problem (kernels whose result depends on batch composition),
not an RNG problem: batch-invariant kernels give perfect reproducibility at roughly a 60% throughput
cost.

## Experiment 1 - BFCL v4 tool calling A/B (n=1240 paired samples)

Two arms on the QUASAR artifact at temperature 1.0, identical thinking parameters and seed, differing
only in `post_thinking`: `emit@1.0` mirrors the thinking sampler (one preset for the whole
generation, the pre-feature behaviour) and `emit@0.2` lets the registered preset govern the emission
phase. All five subsets at full population, fresh server per arm.

| metric | `emit@1.0` | `emit@0.2` |
|---|---:|---:|
| overall accuracy | 86.0% (1067/1240) | 85.8% (1064/1240) |
| discordant samples | - | 55 (26 fixed, 29 broken) |
| exact McNemar p | - | 0.79 |
| invalid emissions | 173/1240 | 176/1240 |

The discordant-pair interval bounds any true difference to roughly +/-1.2 pp. The error-type
histograms also match category for category (`irrelevance_error:decoder_success` 42/42,
`ast_decoder:decoder_failed` 23/24, `value_error:string` 12/12, parallel-checker mismatches 7/8 and
5/5, `type_error:nested` 4/4). Residual failures are semantic - missing or wrong function, bad
argument - not temperature-sensitive formatting, so a colder emission has nothing to repair.

## Experiment 2 - output-level comparison on the same data

Comparing the emitted call sample-by-sample, then canonicalising it (function name plus sorted
arguments, whitespace-insensitive):

| quantity | count | share |
|---|---:|---:|
| emitted text differs | 474 | 38.2% |
| call structure differs | 104 | 8.4% |
| call differs but scores identically | 66 | 63.5% of call changes |
| call differs and score changes | 38 | 36.5% of call changes |

The benchmark discards most of what the knob produces, which is why an accuracy-only view reports a
null. A further 17 score changes (all in `multi_turn_base`) were not explained by any call-structure
difference; that subset's scoring depends on execution results across turns, so its decomposition is
partial.

## Experiment 3 - emission-temperature response sweep

30 prompts (20 tool-call, 10 free-form) x 5 seeds x emission temperature {0.0, 0.2, 0.7, 1.0}, with
the thinking phase frozen at 1.0 and every request issued sequentially so the route is stable.

Conditioned on identical thinking:

| pair | samples differing | mean character similarity |
|---|---:|---:|
| greedy vs 0.2 | 90.2% | 0.793 |
| greedy vs 1.0 | 97.3% | 0.713 |
| 0.2 vs 1.0 | 97.2% | 0.705 |

**90.3% of plans produce four distinct emissions across the ladder** - the knob is not inert. Every
aggregate, however, is flat:

| property | result across the ladder |
|---|---|
| emission length | median 62 tokens at every temperature; 0% truncation |
| tool-call validity | 83-84 of 100 at every temperature |
| prose degeneration | max repeated 4-gram 1.42 (greedy) to 1.30 (1.0) - no degeneration anywhere |
| dispersion across seeds | mean distinct fraction 0.980 (0.0) to 0.993 (1.0) |

## Experiment 4 - reproducibility of the final answer under route variation

5 prompts x 3 seeds x 8 repetitions per temperature, with two requests kept in flight so the route
varies, and the condition order rotated each round.

| prompt | `emit@0.2`: route / answer variation | `emit@1.0`: route / answer variation | similarity 0.2 -> 1.0 |
|---|---|---|---|
| long free-form prose | 0.208 / 0.625 | 0.208 / 1.000 | 0.629 -> 0.389 |
| short JSON object | 0.208 / 0.208 | 0.208 / 0.250 | 0.987 -> 0.979 |
| short tool call | 0.208 / 0.125 | 0.208 / 0.125 | 1.000 -> 1.000 |

At **identical** route variation (0.208 distinct reasoning per 8 in both arms), the cold emission made
long-form answers 37.5 percentage points more reproducible, and materially more similar. Short
structured output was insensitive at both temperatures. Aggregated over prompts: 0.342 distinct
answers per 8 repetitions at 0.2 against 0.425 at 1.0.

This is the only positive effect we were able to measure for the feature, and it is invisible to any
accuracy benchmark.

## Experiment 5 - AIME emission ladder (60 problems per arm)

| arm | accuracy | no boxed answer | multiple boxes | malformed box | median answer-phase tokens |
|---|---:|---:|---:|---:|---:|
| `emit@0.0` | 90.00% | 8.3% | 3.3% | 0% | 6,473 |
| `emit@0.2` | 90.00% | 8.3% | 1.7% | 0% | 5,887 |
| `emit@1.0` | 86.67% | 6.7% | 1.7% | 0% | 6,473 |

The apparent +3.33 pp cold advantage is two problems against a +/-4.2 pp band, and the formatting
metrics move the *other* way (1.0 produced the fewest missing boxes and the most clean integers).
**No malformed symbolic answers anywhere.** The dominant failure is emitting no answer box at all,
which is a termination/formatting failure that no emission temperature fixed, and none of those cases
were truncations.

## Experiment 6 - IFBench emission ladder (300 samples per arm)

Long constrained output, machine-verifiable constraints
([IFBench](https://arxiv.org/abs/2507.02833)). Primary metric is `prompt_level_strict`.

| arm | prompt strict | inst strict | prompt loose | inst loose |
|---|---:|---:|---:|---:|
| `emit@0.0` | 0.6333 | 0.6683 | 0.6733 | 0.7067 |
| `emit@0.2` | 0.6367 | 0.6667 | 0.6800 | 0.7083 |
| `emit@1.0` | 0.6367 | 0.6733 | 0.6667 | 0.7033 |

`emit@0.2` and `emit@1.0` are identical on the primary metric; greedy is one sample lower; all four
metrics sit within 1.3 pp. A third benchmark, a third null - including for constraint compliance,
which was the most plausible place for a cold emission to help.

Open observation: our absolute level is 63-64% where the model card records 77.00% for IFBench. That
card figure was measured on the upstream artifact this fork cannot load, at different sampling, so it
is not like-for-like - but IFBench is part of our suite and the size of the gap deserves its own
investigation.

## Experiment 7 - controlled artifact comparison

QUASAR (quantization-aware trained) against Ostfralla, with the **same** chat template (CLI froggeric
v22.5 for both), the **same** sampling (1.0/0.95/20), the **same** emission (post_thinking mirroring
the thinking sampler) and a fresh server per arm. Only the artifact differs.

| artifact | AIME 2025 | AIME 2026 | total |
|---|---:|---:|---:|
| QUASAR (QAT) | 28/30 | 27/30 | **55/60 = 91.7%** |
| Ostfralla | 28/30 | 24/30 | 52/60 = 86.7% |

QUASAR is 5 pp ahead, which is under one standard error at n=60: **no significant artifact difference
under controlled conditions.**

AIME results for these artifacts depend on the serving configuration rather than on the artifact
alone. The same Ostfralla artifact scored 59/60 when served with its own embedded chat template at a
0.7 thinking temperature, and 52/60 here with the CLI froggeric v22.5 template at 1.0. Artifact AIME
numbers are therefore only comparable when the template and sampling settings are identical, which is
why this comparison pins both.

## Literature position

- Lee, Janghwan Lee, Yoo, Kim, Ryu, Ryu and Choi, *ReSET: Accurate Latency-Critical NVFP4 Reasoning
  via Step-Aware Temperature Scaling* ([arXiv:2606.13233](https://arxiv.org/abs/2606.13233),
  [code](https://github.com/aiha-lab/ReSET)): step-aware, entropy-gated decoding temperature for NVFP4
  reasoning models. Cold for low-entropy symbolic tokens, temperature *above* the default for
  high-uncertainty steps (because NVFP4 over-concentrates them), up to roughly +2 points over an NVFP4
  baseline, with `T_low` calibrated per (model, task). Our knob is the uniform special case and cools
  the uncertain content too - which is precisely what ReSET says not to do. **Our nulls therefore do
  not falsify ReSET**; they falsify the narrower claim that uniformly cooling the emission phase
  improves accuracy or format compliance.
- Wu, Mirhoseini and Tambe, *On the Role of Temperature Sampling in Test-Time Scaling* (Stanford,
  [arXiv:2510.02611](https://arxiv.org/abs/2510.02611)): different temperatures solve different problem
  subsets, and multi-temperature scaling added +7.3 Pass@All over single-temperature scaling. This
  supports the concern that a fixed cold emission narrows the explored temperature dimension for
  finalisation - though the reasoning phase, which carries most of the diversity, is untouched in our
  design.
- Salah and Muneer, *Temperature-Dependent Performance of Prompting Strategies in Extended Reasoning
  Large Language Models* ([arXiv:2604.08563](https://arxiv.org/abs/2604.08563)): the benefit of extended
  reasoning grows from 6x at T=0.0 to 14.3x at T=1.0, consistent with keeping the thinking phase hot.
- Thinking Machines Lab, *[Defeating Nondeterminism in LLM
  Inference](https://thinkingmachines.ai/blog/defeating-nondeterminism-in-llm-inference/)*: the root
  cause of our irreproducibility, and the reason the paired design had to be abandoned.

## Recommendation

**Feature.** Keep the mechanism and the override, treat the preset value as a reliability setting, and
stop describing it as a quality improvement. Concretely, the evidence supports one of:

1. keep `post_thinking` at 0.2 and document it as a reproducibility knob for long output, on the
   grounds that it costs nothing measurable on three benchmarks and helps stability under route
   variation; or
2. default `post_thinking` to mirroring the thinking sampler, so the shipped behaviour is the one we
   can defend, and expose 0.2 as an explicit opt-in for reproducibility-sensitive deployments.

Option 2 is the more honest default given that the only measured effect is variance reduction, and
that the test-time-scaling literature shows narrow temperature coverage costs accuracy on hard
problems. Either way the reasoning phase should stay at 1.0, which is both what ReSET prescribes for
high-uncertainty content and what the extended-reasoning temperature study supports.

**ReSET-style entropy-gated temperature: defer.** The justification for building it is missing on our
stack:

- the damage pool it targets is not observable - the malformed-symbolic-answer rate is 0% on every
  artifact and every emission setting we measured, and the one symbolic failure we do see (no answer
  box at all) is unaffected by emission temperature;
- our artifact is quantization-aware trained, which is the mitigation ReSET itself names for that
  damage, and the controlled comparison shows no artifact-quality gap that could stand in for it;
- a faithful test needs the gated policy implemented (a feature, not a flag), per-task `T_low`
  calibration, and a benchmark with power - AIME at n=60 gives +/-4 pp, far too coarse for a ~2-point
  effect;
- and the uniform version of the idea measurably does nothing on three benchmarks.

Revisit only if (a) we move to a post-training-quantized artifact or a lower-precision path and can
observe the symbolic damage reappearing, or (b) a user-visible failure is traced specifically to
answer-phase sampling.

**Separate workstream: batch invariance.** That identical `(prompt, seed, params)` does not reproduce
across route changes is independent of this feature, and has wider consequences: evaluation runs are
not exactly repeatable, paired evaluation designs are unsound unless the route is held constant, and
any assumption that a fixed seed makes a cached response reproducible is false. Options are
batch-invariant kernels at a significant throughput cost, or accepting and documenting the variance.
This should be tracked on its own.
