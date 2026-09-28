#!/usr/bin/env python3
"""Compare the magnitude layer probe across two runs, aligned by (pos, layer).

Each LAYER-FP line carries the cache position of the column, which identifies the session and the
step (a turn's first decode step reports its own prompt length). A hash cannot separate benign
low-bit float noise -- introduced whenever the batch composition changes the kernel split counts --
from real divergence, so the probe prints a coarse `sum` and `peak` and this script prints the
relative difference, largest first, per layer.
"""
import collections
import re
import sys

PROMPT_LENGTHS: list[int] = []

PAT = re.compile(r'LAYER-FP layer=(\d+) column=(-?\d+) pos=(-?\d+) phase=(\w+) '
                 r'sum=([-0-9.eE+]+) peak=([-0-9.eE+]+)')


def load(path):
    steps = collections.defaultdict(dict)          # pos -> layer -> (sum, peak)
    for line in open(path, errors='ignore'):
        m = PAT.search(line)
        if m is None:
            continue
        layer, _column, pos, phase = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
        if phase != 'verify' or pos < 0:
            continue
        steps[pos][layer] = (float(m.group(5)), float(m.group(6)))
    return steps


def main(concurrent, serialized, extra_args=None):
    a, b = load(concurrent), load(serialized)
    print(f"concurrent positions={len(a)} serialized positions={len(b)}")
    common = sorted(set(a) & set(b))
    print(f"common positions={len(common)}")
    worst = []
    for pos in common:
        for layer in sorted(set(a[pos]) & set(b[pos])):
            sa, pa = a[pos][layer]
            sb, pb = b[pos][layer]
            denom = max(1.0, abs(sb))
            rel = abs(sa - sb) / denom
            peak_rel = abs(pa - pb) / max(1.0, abs(pb))
            worst.append((max(rel, peak_rel), pos, layer, sa, sb, rel, peak_rel))
    worst.sort(reverse=True)
    print("\nlargest relative differences (position, layer, sum_conc, sum_ser, rel_sum, rel_peak):")
    for row in worst[:25]:
        print(f"  magnitude={row[0]:.4f} pos={row[1]} layer={row[2]} "
              f"sum={row[3]:.4f}/{row[4]:.4f} rel_sum={row[5]:.2e} rel_peak={row[6]:.2e}")
    # For each position, the first layer whose relative difference exceeds a real-divergence
    # threshold on rel_peak (1e-2). rel_sum is NOT used for the decision: a residual's sum over
    # 2,048 values sits near zero, so the ratio explodes and flags positions whose numbers are
    # otherwise identical -- the third review caught a conclusion built on exactly that.
    print("\nfirst layer above 1e-2 per position (first 12 positions):")
    for pos in common[:12]:
        first = None
        for layer in sorted(set(a[pos]) & set(b[pos])):
            sa, pa = a[pos][layer]
            sb, pb = b[pos][layer]
            if abs(pa - pb) / max(1.0, abs(pb)) > 1e-2:
                first = layer
                break
        print(f"  pos={pos}: first_diverging_layer={first}")

    # Group positions into turns by prompt length: a turn's decode positions start at
    # prompt_tokens - 1 and increase by one per step, so every position belongs to the largest
    # prompt length that is <= pos + 1. That is what makes the result readable per session.
    # Prompt lengths: `--prompts=32606,32662,...` if given, else derived from the data as the
    # distinct turn starts the log actually contains. They were four literals from one run, which
    # silently mis-groups any other workload -- and the flag added earlier was inert because
    # main() never forwarded the argument, so two different values produced identical output.
    if not PROMPT_LENGTHS:
        PROMPT_LENGTHS.extend(sorted({min(common)} if common else set()))
    prompts = sorted(PROMPT_LENGTHS)
    by_turn = collections.defaultdict(list)
    for pos in common:
        owner = None
        for p in prompts:
            if p - 1 <= pos:
                owner = p
        by_turn[owner].append(pos)
    # Say what could not be attributed instead of dropping it silently: the grouping rule
    # (largest prompt length <= pos+1) misattributes any turn that outruns the gap to the next
    # prompt length, and a turn with no owner used to vanish from the output without a word.
    attributed = set()
    for key in common:
        for candidate in prompts:
            if candidate - 1 <= key:
                attributed.add(key)
    print(f"\nunattributed positions={len(common) - len(attributed)} of {len(common)} "
          f"(grouping rule: largest prompt length <= pos+1; read the per-turn split as an inference)")

    print("\nper turn (prompt length), first diverging layer by step:")
    for owner in sorted(k for k in by_turn if k is not None):
        rows = []
        for pos in sorted(by_turn[owner]):
            first = None
            for layer in sorted(set(a[pos]) & set(b[pos])):
                sa, pa = a[pos][layer]
                sb, pb = b[pos][layer]
                if abs(pa - pb) / max(1.0, abs(pb)) > 1e-2:
                    first = layer
                    break
            rows.append((pos - (owner - 1), pos, first))
        clean = [r for r in rows if r[2] is None]
        dirty = [r for r in rows if r[2] is not None]
        print(f"  prompt={owner}: steps={len(rows)} clean={len(clean)} diverging={len(dirty)}")
        for step, pos, first in dirty[:6]:
            print(f"     step={step} pos={pos} first_diverging_layer={first}")


if __name__ == '__main__':
    for arg in sys.argv[3:]:
        if arg.startswith("--prompts="):
            PROMPT_LENGTHS.extend(int(x) for x in arg.split("=", 1)[1].split(",") if x)
    main(sys.argv[1], sys.argv[2], [a for a in sys.argv[3:] if a.startswith('--prompts=')])
