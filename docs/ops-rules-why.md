# Why the ops rules exist

Why each rule in `CLAUDE.md` exists; read before relaxing or deleting a rule.
Each heading names the rule in `CLAUDE.md`; the text records the incident that
taught it. Where a rule is enforced by a script, the script's own header carries
the same story and this file does not repeat it.

This file is STABLE and rule-indexed. `plan.md` → **Current state** is VOLATILE
and date-indexed (what is fixed, its evidence, the next instruments). Rationale
here is what stops the next session from trimming a rule away; investigation
state belongs in `plan.md`.

---

## QA is the local server, and which session runs through it is not fixed

The local `ninfer.service` is QA — it serves the operator's Claude Code sessions
and the Bash classifier (operator correction, 2026-09-28; the repo docs' older
"prod" is stale usage). Because a restart interrupts whatever is currently routed
through it, a session must read `env ANTHROPIC_BASE_URL` rather than assume its
own traffic goes elsewhere: when it points at `127.0.0.1:8080`, a deploy/restart
takes this session down for its duration.

## Dev time vs QA time is declared by `/tmp/NINFERDEV`, not inferred

A mode must be declared where every tool call can see it. An `export` typed into
the operator's shell does not reach the agent's Bash invocations (each gets a
fresh shell), so an env var silently reads empty; a file does not. Inferring the
mode from the hour, the base URL, or how busy the journal looks has been wrong.
`~/.claude/settings.json`'s `env` block *does* propagate, so it would also work,
but the file is the lighter switch. The two modes differ only in what stopping QA
costs, never in the safety rules.

## Instruments: run the script, do not re-derive the check

Each `tools/ops` and `tools/e2e` check began as a rule that lived only in prose,
and the failure happened anyway. A rule performed from memory is performed until
the first busy night. The `merge-loss-check` script exists because a 2026-10-03
merge resolved 37 conflicts by token-matching and took every non-matching file
wholesale from upstream — an overwrite, not a merge. `verify-deploy` exists
because both the running-hash check and the `find -newer` check pass while the
binary is wrong (2026-10-01: QA served a build missing its change for ~10 minutes;
a target under iteration was relinked but `ninfer-serve` was not).
`recover-after-killed-swap` exists because a killed swap leaves three things
broken and the third — the sentinel — is silent. `check-test-binaries` exists
because a stale `build/tests/<name>` printed PASS (2026-09-27) after a target was
added to CMake without re-running CMake. `watcher-token-reachability` exists
because four tokens (`WEDGE`, `planner-no-plan`, `relief-shared`,
`host-state-pool`) matched nothing for weeks and read like a quiet system, and on
2026-10-03 a merge silently deleted the `fail-all cleanup` line from the binary
with no other symptom. `startup-smoke` exists because e2e, ctest and the soak all
passed over a tree where YaRN and `--vision-cpu` did not start. `reqs.sh` exists
because the request log is a MIXED population (e2e, benchmark and QA traffic share
`~/ninfer-requests.jsonl`), and an e2e cold-cache request looks exactly like a
reuse defect.

**Stated blindness (do not over-trust):** the token check catches only losses
where a string is deleted outright — where an identifier survives as a
declaration while its use is dropped, it is blind. The startup smoke proves a
configuration *starts*, not that its flag is *applied*.

## Never kill with `pkill -f` / `pgrep -f`

`-f` matches the calling shell's own command line, including text that is only a
regex. `pkill -f 'journalctl -u ninfer'` killed its own shell (exit 144, no
output — which reads like a failed kill). Bracketing (`ninfe[r]`) defeats only
the pattern matching *itself*, never a literal copy of the path elsewhere on the
same command line, so it fails exactly when the command also mentions the path.
The worst form: editing a script whose literal path is on the same line as a
bracketed `pgrep` for it (observed 2026-10-03, seven times, exit 144). The
reliable form is `ps -eo pid,args | grep <thing>` to find the pid, then
`kill <pid>`; or kill by port, as `e2e-swap.sh` and `ninfer-start-test.sh` do.

## Arm the watcher; the alert set is the awk file, tested

The monitor must carry state and follow two units; a `grep -E` pattern cannot,
which is why two hand-written fallbacks drifted — one was missing `WORKER OOM`
and `private victim evicted: demotable=1`, the other omitted `WEDGE` and followed
only `ninfer.service`. `ninfer-watch.awk` is therefore the single specification of
the alert set, and `ninfer-watch-test.sh` feeds it synthetic lines and requires
the exact expected alert set (the test caught the eviction line being swallowed by
the log-only rule that follows it). There is deliberately NO fallback: if the
script cannot run, fix the script.

## A `post-recovery residual` is evidence iff non-zero, whatever its prefix

`fail_all_locked` runs on the **shutdown** path as well as the recovery path
(`src/runtime/engine/engine_core.h`, `fail_all_locked`), so every clean stop logs
a residual for an empty engine — which is how 19 zero lines were once read as
"19 recoveries, residual zero" when every one was a stop. Arming on a preceding
`WORKER RECOVER`/`WORKER CRASH` instead is also wrong: it drops a non-zero
residual after `WORKER OOM`, which is the wedge signature (`WORKER OOM: …
- recovering`, then the same recovery, then the residual).

## The watcher follows TWO units; `WEDGE` is a sentinel string

The wedge is announced by the SENTINEL, not by the engine, so watching only
`ninfer.service` hides the one event that explains a dead QA. Observed
2026-09-26: a live wedge at ~19:51:54, the sentinel's restart at 19:54:37, and the
monitor reporting only a bare `health 200 -> 000` with no cause. `WEDGE` is a
sentinel output rather than an engine string, so the `grep src/` reachability rule
does not cover it — that rule is what removed the token, and tonight is what that
cost.

## A wedge is captured before it is restarted

`tools/monitor/wedge-sentinel.sh` writes `wedge-<ts>.waits` (per-thread
`state=`/`wchan=`, needing no symbols) and `.bt` (best-effort backtrace)
immediately before its `systemctl restart`, into `~/ninfer-watch/`. That directory
was not explicit until 2026-10-01: the script used `$HOME/ninfer-watch` and the
sentinel is a SYSTEM unit where `HOME` is unset, so it expanded to the root-level
`/ninfer-watch` while this repo's docs said `~/ninfer-watch/` — the natural
conclusion ("the capture was not written") is wrong. It was fixed at the cause by
a drop-in setting `WEDGE_CAPTURE_DIR`; the mechanism was `HOME`, not `PWD`. A
wedged engine cannot run its own shutdown path (the shutdown needs the lock the
wedge holds), so this capture is the only moment the evidence exists, and on
2026-09-26 the restart destroyed it.

**The capture is valid only if the engine is still stalled when it is taken.** The
sentinel detects a stall by looking backwards and captures NOW, so an engine that
resumed in between is captured in its healthy idle state. Observed 2026-10-01:
the engine finished a request at 20:26:45.398 and the capture ran at 20:26:46 —
0.6 s later — so the 46 threads it recorded (all sleeping on futex/condvar, none
running) described an idle server that had just completed work. Reading that as
the wedge's signature would have produced a confident, false conclusion ("all
sleeping, therefore a missed wakeup"). Check the journal at the capture timestamp
before characterising the state. The backtrace resolves only the innermost frame
on a release build; the wait distribution is what characterised the 2026-09-26
wedge (21 of 25 threads on `futex_do_wait`, five on one condvar, one spinning in
`sched_yield`). For a resolvable backtrace, rebuild `build-diag` (configured
`RelWithDebInfo`) from HEAD and run QA from it — a stale
`build-diag/apps/ninfer-serve` (dated Sep 26, with hundreds of newer sources)
would name the wrong binary's stack.

## `health 200 -> 000` is a timeout, not an outage

Observed 2026-09-27 under an agentic workload: the watcher reported `200 -> 000`
while the unit was ACTIVE on the same pid and start time and the journal showed it
working throughout. Under that load `/stats` took 60+ s — its handler takes the
engine's execution mutex — and `/health` can miss a probe's timeout behind a
prefill though it answers in 0.0003 s when free. So `000` means "did not answer in
time"; the check is the unit's `MainPID` and `ExecMainStartTimestamp` unchanged
plus the journal still printing progress. The wedge is the OPPOSITE shape:
`/health` 503 with `/stats` still answering. The sentinel is not fooled — it
restarts on idle-with-work-pending or stalled progress, has a 90 s grace, and its
own notes say a healthy busy server "never arms". (The watcher reads `MainPID`
only at startup, so the pid check is a manual reading.)

## One monitor at a time; an expired monitor is not one that stopped

Duplicates double-notify. The 30-minute expiry REPORTS itself but does not reap
the process, so "re-arm on expiry" accumulates watchers: 2026-10-01, three live
watchers (pids 537863/543379/544481) from two expiries whose processes survived.
The count that matters is the TAILER count, one per watcher
(`pgrep -cf 'journalctl -u ninfe[r]'`); `ninfer-watch.sh` matches are not the
count, because every watcher's own children carry that string (the same three
watchers once read as 21 processes). `TaskStop` reaps; expiry does not. No script
enforces this — it is a manual check.

## Soak data accrues only while driving the server

Net, census and eviction counters move only under request load. "Wait / let it
accumulate" is therefore a fake option — it produces no data and wastes the
cheapest resource the session has. Proceed and keep working.

## Reviews run in parallel with e2e, one pass per milestone

Ten consecutive review passes once ran while the last five had no e2e test; the
passes found real defects in the *documentation* while the actual bugs sat
untouched. A review is worth its cost only when the tree changed in a way e2e
cannot judge (claims, staging, instrumentation safety).

## How a review is run (the brief, mode, calibration)

The global `brutal-honesty-review` agent is spawned for its own context — it must
read the artifacts, not a summary. Never self-review, and never a general-purpose
agent. The four required brief items and the report shape exist so the reviewer
attacks the change rather than describing it, is told the prohibition (`:8080`
serves the user's live session), is asked one falsifiable question, and validates
any instrument it reasons from — because most false conclusions in this repo came
from an instrument that measured nothing (a silent skip, a probe that never
executed, a hash that cannot distinguish one ULP from divergence, a wrong dtype, a
comparison keyed by an allocation detail) while looking like a clean result. Relay
the verdict; do not paraphrase a finding into something weaker.

## Definitive results come from e2e and the full implementation

Not from review prose, not from reading artifacts, not from your own analysis of
logs. A claim is a result only when a run produced it; state the run.

## Every finding gets triaged, and fixed where a reader meets it

An actionable code defect is fixed; a claim/comment/documentation defect is
corrected *in place* — a note in a triage section does not unstate a wrong claim.
Grep the whole tree for other copies first: a stale claim usually has duplicates,
and the one a reviewer quotes is rarely the only one. Load-bearing findings (which
can change behaviour or the validity of a measurement) gate another pass;
documentation findings do not — chaining prose passes while bugs sit untouched is
the failure the "one pass per milestone" rule records.

## Validate the instrument before believing its output

Run the same configuration twice and require the two runs to agree; if they do
not, the probe measures something other than what you think. Print skips and
denominators, and check counts over the whole log, never the first screenful.

## A negative is a result too

Record what was closed and by which control, so the same surface is not
re-audited.

## The e2e test server must never bind `:8080`

`:8080` is QA and serves real traffic whatever this session is doing (the
classifier, any live local session). A test server there answers requests the
suite never issued, tainting every cache and timing number with an unknown
workload and 400ing them on the test profile's smaller context. The swap defaults
its test server to `E2E_PORT=8085`, and two checks enforce isolation: nothing may
listen on either port before the test server starts, and after it starts the
listener's pid must equal the pid recorded by `ninfer-start-test.sh`.

## Load driven at QA is valid for concurrency, invalid for counters

That load shares the server with the operator's real traffic and the request log
cannot separate the two (same protocol, same growing-context shape, no client
id). Read it for "does a foreign marker appear under N lanes?"; reuse, queue and
counter numbers belong on the isolated port.

## Verify the deploy by the running exe's hash — and prove the tree is fresh

`sha256sum /proc/<pid>/exe == sha256sum build/apps/ninfer-serve` proves
**running == last link** and says NOTHING about whether the last link contains the
change. 2026-10-01: QA served a build missing its guard and its instrument for
~10 minutes while both hashes agreed, because the target under iteration was a
different one and `ninfer-serve` was never relinked. `find -newer` closes that,
and is itself blind to a source edited WHILE the build ran (the binary ends up
newer than the source, so `-newer` reports clean — measured 2026-10-01: the header
was edited 3 minutes into a 4-minute build and the stale binary passed both
checks). Hence the third check, grep the running exe for a string the change
introduces. For a naming-only or otherwise string-less change no grep control can
exist, so freezing the source from build launch to link is the only guard.

## `TimeoutStopSec=300` is deliberate

Raised from 30 on 2026-09-26. A stop-timeout SIGKILL destroys the engine's OWN
shutdown path (`fail-all cleanup`, `post-recovery residual`), the only lines that
report whether a shutdown left occupancy unowned — and 30 s is not a budget a
server can meet while unpinning 30 GiB of host KV and tearing down CUDA on WSL2
(measured 39 s from `Stopping` to SIGKILL, so the shutdown never ran and every
stop silently lost that evidence). A stop that still times out at 300 s is a HANG
to diagnose, not a budget to shrink back.

## A wedged engine is not fixed by `~/ninfer-ensure.sh`

That script is idempotent and reports "already running with desired config"
whenever the unit is *active* — and a wedge is internal to the engine, not a unit
failure (2026-09-25: unit active, `/health` and `/v1/messages` both 503,
scheduler empty, `/stats` still answering). Recovery is
`sudo systemctl restart ninfer.service`.

## `/stats` is on `:8081`, never `:8080`; absence is not zero

`:8080/stats` serves nothing at all (`http=000 bytes=0`); querying the wrong port
once made a working counter look dead. Rate-limited journal lines and counters
that print on a separate schedule make silence ambiguous — the eviction print is
rate-limited to the first 8 then every 512th, and `checkpoint StateImage priced`
prints on its own cadence, so its absence means "not printed", not "not
recurring"; read the counter in `/stats`. **A probe count is a denominator**: the
planner probes a cell-free option from five sites (`goal_probes_by_site_` in
`src/runtime/engine/context_cache/materialization_planner.h`) on its way to an
ordinary eviction — thousands per planning run, 108,544 in one 24-request run —
so a probe count must never alert or be quoted as the event. The first version of
that instrument reported the probe number as the loss count and put it in the
alert set, which made it fire on healthy traffic and unable to falsify a capacity
raise.

## Evidence logs are gitignored; the manifest is what is committed

`*.log` is ignored on purpose (`.gitignore:45`). Never `git add -f` one — the
operator once had to undo 57 force-added logs. What is committed is the MANIFEST
beside them: binary sha256, artifact, `git-head`, and (for
`tools/e2e/prefix-real-evidence.sh` runs) each log's own sha256 and rc. A claim
cites `results/<run>/manifest` in the repo and the log locally; a log that was
never written down is not evidence.

## The 8-agent soak is a manual operator step, on a proven binary

The e2e suite runs the server on its own isolated port with its own profile; it
does not exercise QA's real configuration under more parallelism than QA is sized
for, so the last gate is the operator's agentic load (the "What the load showed"
run: 8 parallel agents, deliberately over-subscribing, 190 requests / 17
conversations). It is manual — the agent never runs it and must not invent a
command. Deploy the frozen tree and prove it is what is running FIRST, or the
soak loads QA while QA runs an older binary and says nothing about the change
(measured 2026-10-01: QA's `/proc/<pid>/exe` was a DELETED inode hashing
`855d8143…` while `build/apps/ninfer-serve` was `9a133aea…`). The absence
checklist is a manual reading — no script prints it — and a green e2e is not a
substitute, because the e2e is the acceptance test for the CHANGE while the soak
is the regression gate for the SERVER (tonight's three instrument defects were all
found in the e2e, not the engine).

## PRs target the operator's fork, as one squashed commit

Stated by the operator 2026-10-01. The two PRs that went upstream (#64
`pr/host-kv-cache`, #65 `fix/tool-arg-schema-type`) were both CLOSED unmerged, and
every PR that has landed here went to the fork: #12
(`engine/cache-reuse-and-host-budget` → `gzenz:master`, merged 2026-09-28) is the
pattern. The fork's master is behind, so any PR from this tree carries the whole
delta and is sent as ONE commit (PR #12 collapsed 149 commits); the collapse is a
property of the BRANCH and its record is `plan.md` §1f, not the PR description.

## A PR description says what the change delivers, not how it was assembled

Corrected by the operator 2026-10-01, on a description that opened with the
squash, the upstream/local commit composition, and the review history — all true,
all irrelevant to a reviewer asking "what does this do?". Include what changes,
why, and the evidence; exclude the squash, the commit provenance and the review
narrative. The test is not "is it true" but "does a reviewer of this diff need it
to judge the diff". Provenance that changes what a reviewer is looking at is
handled by the branch and the mapping in `plan.md`.

## A commit message cannot be corrected after a push

Cite ids that exist on the remote, never a pre-rewrite id; put the mapping in
`plan.md` if history was rewritten.
