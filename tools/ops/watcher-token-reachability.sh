#!/usr/bin/env bash
# CAN EVERY ALERT TOKEN IN THE WATCHER STILL FIRE? Run this when you arm a monitor.
#
#   bash tools/ops/watcher-token-reachability.sh
#
# WHY. CLAUDE.md states the rule -- "a token that cannot fire reads exactly like a quiet system", and
# "before adding or removing one, check it with `grep -rl '<token>' src/`" -- and records FOUR tokens
# (`WEDGE`, `planner-no-plan`, `relief-shared`, `host-state-pool`) that were carried for weeks matching
# nothing. A rule that is performed by hand is performed until the first busy night. On 2026-10-03 a merge
# silently deleted the `[engine] fail-all cleanup:` line from the binary, and nothing noticed: the token
# stayed in the alert set, `ninfer-watch-test.sh` kept passing because it feeds the line synthetically, and
# the only visible symptom was a monitor that was quiet about shutdowns.
#
# WHAT IT DOES. Reads the patterns out of `tools/ops/ninfer-watch.awk` -- the file CLAUDE.md names as the
# ONLY specification of the alert set -- and, for each alternative, asks whether it matches anything under
# `src/`. Tokens are classified:
#   * in src/            an engine-printed string. If the engine stops printing it, this check fails.
#   * JOURNAL-CLASS      known NOT to appear in src/ and still correct: libc/the kernel/the runtime print
#                        them (`Killed [0-9]`, `Segmentation`, `terminate called`), or a helper script does
#                        (`WEDGE` is the wedge sentinel's line). Listed below with the reason.
#   * UNREACHABLE        neither. **This is the finding** -- either the engine stopped printing it, or it
#                        was never a real token.
#
# Its own limit: "matches something in src/" is NOT "the running binary prints it". A token can survive in
# source and be dead in the build (a string removed from the FORMAT of a live call, say). This narrows the
# blindness; it does not remove it.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 2
AWK_FILE=tools/ops/ninfer-watch.awk
[ -r "$AWK_FILE" ] || { echo "FAIL: $AWK_FILE not readable"; exit 2; }

# Tokens that legitimately match nothing in src/, each with the reason it is still live.
CATEGORY_FILE="$(mktemp)"
trap 'rm -f "$CATEGORY_FILE"' EXIT
cat > "$CATEGORY_FILE" <<'EOF'
Killed [0-9]	kernel OOM killer -- the kernel prints it
Segmentation	libc on SIGSEGV
core dumped	the shell/libc
terminate called	libstdc++ on an uncaught exception
Assertion	assert()/libc
WEDGE	the wedge SENTINEL prints it, not the engine
restarting ninfer	the sentinel
Main process exited	systemd
Failed with result	systemd
Scheduled restart job	systemd
Stopped ninfer.service	systemd
Started ninfer.service	systemd
EOF

python3 - "$AWK_FILE" "$CATEGORY_FILE" <<'PY'
import re, subprocess, sys
awk_file, cat_file = sys.argv[1], sys.argv[2]

known = {}
for line in open(cat_file):
    parts = line.rstrip("\n").split("\t")
    if len(parts) == 2:
        known[parts[0]] = parts[1]

# Every regex used as an alert condition:  /.../ { alert(   or   /.../ { log(
pats = []
for line in open(awk_file, errors="replace"):
    # `keep` AS WELL AS `alert`/`log`. The first version matched only alert|log and therefore MISSED the
    # keep rules -- and `fail-all cleanup`, the token a 2026-10-03 merge silently deleted from the binary,
    # lives in a KEEP rule (`ninfer-watch.awk:60`). The check would not have caught the loss it was written
    # for. A keep rule matters exactly as much: it is the full-fidelity log's filter, so a token that stops
    # matching takes the line out of the log the same way.
    m = re.match(r'\s*/(.+)/\s*\{\s*(alert|log|keep)\b', line)
    if m:
        pats.append((m.group(1), m.group(2)))

alts = []
for pat, kind in pats:
    for a in pat.split("|"):
        a = a.strip()
        if a and (a, kind) not in alts:
            alts.append((a, kind))

def in_src(tok):
    # A plain substring search over the source -- these are printed strings, not regexes, so -F is right.
    # BUT A RENDERED VALUE IS NOT A SOURCE LITERAL: the alert set matches what the journal shows, and the
    # engine prints a FORMAT SPECIFIER there. `private victim evicted: demotable=1` matches the journal
    # while the source says `demotable=%d`. **The first version of this check reported exactly that token as
    # UNREACHABLE**, on a night it had fired eight times -- a false finding from an unvalidated instrument,
    # which is the failure this whole script exists to catch. So: fall back to the stem before the last `=`
    # and accept a hit there as "reaches src/ by rendering".
    for probe in (tok, tok.rsplit("=", 1)[0] if "=" in tok else tok):
        r = subprocess.run(["grep", "-rlF", "--", probe, "src", "include", "apps"],
                           capture_output=True, text=True)
        out = r.stdout.strip().splitlines()
        if out:
            return out, ("" if probe == tok else f"rendered: source has '{probe}=<value>'")
    return [], ""

unreachable, engine, journal = [], [], []
for a, kind in alts:
    if a in known:
        journal.append((a, kind, known[a]))
        continue
    hit, note = in_src(a)
    (engine if hit else unreachable).append((a, kind, (hit[0] + ("  " + note if note else "")) if hit else ""))

print(f"watcher alert alternatives: {len(alts)}")
print(f"  in src/        : {len(engine)}   (an engine string; the engine must keep printing it)")
print(f"  journal-class  : {len(journal)}  (printed by the kernel/libc/systemd/a helper, by design)")
print(f"  UNREACHABLE    : {len(unreachable)}")
for a, kind, _ in unreachable:
    print(f"     !! '{a}'  ({kind})  -- matches nothing in src/ and is not a known journal-class token")
print()
for a, kind, where in engine:
    print(f"     ok '{a}'  -> {where}")
PY
