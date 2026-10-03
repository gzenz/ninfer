#!/usr/bin/env bash
# DID A MERGE DROP LOCAL WORK? A token-free answer.
#
#   bash tools/ops/merge-loss-check.sh <merge-base> <ours-pre-merge> <theirs> [result]
#
# Default result is HEAD. Example, for the upstream integration of 2026-10-03:
#
#   bash tools/ops/merge-loss-check.sh 594930e7 c75341bf origin/master
#
# WHY THIS EXISTS, and why it is not a token list. On 2026-10-03 a merge of `origin/master` into a long-lived
# fork resolved 37 conflicts by asking, per file, "does HEAD's copy contain one of these tokens?" and taking
# every file that did not match WHOLESALE from upstream. That is not a merge -- it is an overwrite of every
# local hunk the list did not happen to name -- and it silently dropped six local features: YaRN's
# `rope_scaling_factor` publication, `--vision-cpu` workspace planning, the `bind_sequence_kv` re-bind that is
# the D2 cross-session KV fix, the `[engine] fail-all cleanup:` line the monitor's alert set keeps, and two
# `NINFER_*_PROBE` probes. Every gate was green over that tree: e2e, ctest, the soak, and a GPU test.
#
# **A LIST OF TOKENS CAN ONLY FIND LOSSES WHOSE NAMES ARE ALREADY ON THE LIST.** That is the same failure
# CLAUDE.md records for the monitor's alert set -- "a token that cannot fire reads exactly like a quiet
# system" -- and it was reproduced two levels at once in one night.
#
# The method here is mechanical and name-blind: for every line `ours` ADDED over the merge base, does that
# line still occur, with at least the same multiplicity, at the result? A local line can only disappear if a
# resolution overwrote it, and no knowledge of what the line means is needed to notice.
#
# HOW TO READ THE OUTPUT. `src/ops/*`, `bench/ops/*` and `tests/ops/*` are expected to appear -- upstream
# rewrites its kernels and we do not contribute there. **Any other file is a finding.** On the 2026-10-03
# re-merge the only non-ops entry was `README.md` (documentation).
#
# RUN IT TWICE AND REQUIRE AGREEMENT (the repo's rule for any instrument), and keep the positive control:
# a line you know you re-applied by hand must NOT be reported. If it is, the arguments are wrong -- most
# likely `ours` is not the pre-merge commit, in which case this answers a different question silently.
set -uo pipefail
MB="${1:?usage: merge-loss-check.sh <merge-base> <ours-pre-merge> <theirs> [result]}"
OURS="${2:?}"
THEIRS="${3:?}"
R="${4:-HEAD}"

exec python3 - "$MB" "$OURS" "$THEIRS" "$R" <<'PY'
import subprocess, sys, collections
MB, OURS, THEIRS, R = sys.argv[1:5]

def sh(*a):
    return subprocess.run(a, capture_output=True, text=True, errors='replace').stdout

files = sh('git', 'diff', '--name-only', MB, OURS).split()
tot = 0
rep = []
for f in files:
    d = sh('git', 'diff', '-U0', MB, OURS, '--', f)
    added = [l[1:] for l in d.splitlines() if l.startswith('+') and not l.startswith('+++')]
    added = [a for a in added if len(a.strip()) > 3]   # ignore blank/brace-only churn
    if not added:
        continue
    res = sh('git', 'show', f'{R}:{f}')
    rc = collections.Counter(res.splitlines())
    ac = collections.Counter(added)
    missing = [(a, ac[a] - rc[a]) for a in ac if ac[a] > rc[a]]
    if missing:
        n = sum(m for _, m in missing)
        tot += n
        rep.append((n, f, missing))

rep.sort(reverse=True)
print(f'merge-loss-check: {OURS} -> {R} (base {MB}, theirs {THEIRS})')
print(f'  files with missing ours-added lines: {len(rep)}  lines: {tot}')

non_ops = [r for r in rep if not r[1].startswith(('src/ops/', 'bench/ops/', 'tests/ops/'))]
for n, f, m in rep:
    mark = '  ' if f.startswith(('src/ops/', 'bench/ops/', 'tests/ops/')) else '**'
    print(f'{mark} {n:5d} {f}')
    for a, k in m[:6]:
        print('       ', k, repr(a[:140]))

print()
print(f'  NON-OPS FILES: {len(non_ops)}  <-- each one is a finding unless named and justified')
for _, f, _ in non_ops:
    print(f'    - {f}')
sys.exit(1 if non_ops else 0)
PY
