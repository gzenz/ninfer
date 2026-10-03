# NInfer — ops rules

QA = the local server on this WSL2 host (`ninfer.service`, Qwen3.8-27B on the v3 engine). It serves the
user's local Claude Code sessions and the Bash classifier — which session runs through it is not fixed:
read `env ANTHROPIC_BASE_URL` rather than assuming. A deploy/restart interrupts whatever QA is serving,
including this session while its base URL points at `127.0.0.1:8080`.

**Rules only.** Why each rule exists: `docs/ops-rules-why.md`. Current investigation state and counter
semantics: `plan.md` → **Current state** (actionable: what is fixed and its evidence, what is not, the next
instruments with their validation, the open-claims list). Each `tools/ops` script's header records the
failure it prevents.

## Instruments — run these; do not re-derive them by hand

| when | run | it answers |
|---|---|---|
| after resolving a merge or rebase | `bash tools/ops/merge-loss-check.sh <base> <ours-pre> <theirs>` | **did the resolution drop local work?** Token-free line-multiset diff; run it TWICE and require agreement. |
| before believing a deploy | `bash tools/ops/verify-deploy.sh ['new string']` | running == built, built == tree, and (with a string) that the change is IN the running exe. It says which of the three it could not check. |
| after a killed swap, or any time QA+monitor look wrong | `bash tools/ops/recover-after-killed-swap.sh` | the orphaned test server, QA, and **the sentinel** — the one that is silent when missing. |
| when arming a monitor | `bash tools/ops/watcher-token-reachability.sh` | **can every alert token still fire?** A token that matches nothing reads exactly like a quiet system. |
| after touching a startup path or a config flag | `bash tools/e2e/startup-smoke.sh` (GPU window) | do YaRN and `--vision-cpu` still START. No other gate covers them; e2e, ctest and the soak all passed over a tree where they were broken. |
| before citing a test binary | `bash tools/ops/check-test-binaries.sh` | is that executable built from the source you think? |
| to pin a hook in a fresh clone | `git config core.hooksPath tools/hooks` | `.git/hooks` is IGNORED when `core.hooksPath` is set, so symlinking there does nothing. |
| before quoting a number from the request log | `bash tools/ops/reqs.sh instances` / `QA` | that file is a MIXED population; a figure is QA-only only if it was filtered. |
| **on every commit** | `tools/ops/pre-commit.sh`, run by git via `core.hooksPath=tools/hooks` | **The gate.** Runs the full host suite, the quarantined target and the e2e in a QA-down window, and blocks on a failure, a SKIP, a timeout, an oversized `CLAUDE.md`, or staged `.log` files. Bypass only with `--no-verify`, and say why in the message. |

**Two instruments have stated blind spots; do not over-trust them.** The token check catches only losses
where a string is deleted outright; the startup smoke proves a configuration *starts*, not that its flag is
*applied*.

**`pkill -f`/`pgrep -f`: do not use them to kill.** Find the pid and `kill <pid>`
(`ps -eo pid,args | grep <thing>`), or kill by PORT. Bracketing (`ninfe[r]`) defeats only the pattern
matching *itself*, never a literal copy elsewhere on the same command line — so it fails exactly when the
command also mentions the path, which is the form that keeps biting. The worst form is editing a script
whose path is on the same command line. `e2e-swap.sh` and `ninfer-start-test.sh` kill by port.

## Monitoring

- Keep a monitor armed during a soak/QA. It expires at 30 min — re-arm on expiry and after any stop.
- **Arm `tools/ops/ninfer-watch.sh`** (the monitor: it carries state, polls `/health`, reports transitions,
  and splits one pass over the journal into an alert stream and a full-fidelity log at
  `~/ninfer-watch/latest.log`). **Its alert set is TWO files, and this line named only the first until
  2026-10-03 — which is how an alert class goes invisible.** `tools/ops/ninfer-watch.awk` covers the JOURNAL
  (tested by `tools/ops/ninfer-watch-test.sh`); **`tools/ops/ninfer-watch-requests.jq` covers the REQUEST LOG**
  (`LOW PREFIX USE` / `SLOW REUSE`, thresholds `WATCH_REUSE_MIN_FRACTION=0.6`, `WATCH_REUSE_MAX_PREFILL_S=2.0`).
  The omission was found the only way it could be: a `LOW PREFIX USE` alert fired that the awk does not
  contain. **`watcher-token-reachability.sh` reads only the awk**, so the request-log class has no reachability
  check — a threshold that can no longer trip is silent there too. There is deliberately NO hand-written
  fallback pattern. If `ninfer-watch.sh` cannot run, fix the script.
- **A `post-recovery residual` is evidence iff NON-ZERO, whatever its prefix** (`(recover)` or `(fail-all)`);
  an all-zero one is restart noise. `fail_all_locked` runs on the **shutdown** path too
  (`src/runtime/engine/engine_core.h`, `fail_all_locked`), so a clean stop logs one for an empty engine.
- **The watcher follows TWO units: `ninfer.service` and `ninfer-wedge-sentinel.service`.** The wedge is
  announced by the SENTINEL (`WEDGE … restarting ninfer`); watching one unit hides the event that explains a
  dead QA. `WEDGE` is a sentinel output, not an engine string, so it is exempt from the `grep src/` reachability
  check.
- **A `health 200 -> 000` transition is NOT an outage until you check two things:** the unit's `MainPID` and
  `ExecMainStartTimestamp` unchanged, and the journal still printing progress. `000` means the probe did not
  answer in time (the `/stats` handler takes the engine's execution mutex). A wedge is the **opposite shape**:
  `/health` **503** with `/stats` still answering. (The watcher reads `MainPID` only at startup.)
- **A wedge is CAPTURED before it is restarted.** Read `~/ninfer-watch/wedge-<ts>.{waits,bt}` FIRST when a
  wedge is reported. **Before characterising the capture, check the journal at its timestamp** — the sentinel
  detects a stall by looking BACKWARDS and captures NOW, so an engine that resumed in between is captured in
  its healthy idle state, and the dump describes the recovery, not the wedge. If the path is ever wrong,
  `systemctl show -p Environment ninfer-wedge-sentinel.service` is the one-line answer. For a resolvable
  backtrace, **rebuild `build-diag` from HEAD first** and run QA from that binary (release frames below the
  innermost are `??`); a stale `build-diag/apps/ninfer-serve` deploys a week-old binary.
- One monitor at a time (duplicates double-notify). **A monitor that expires is not one that stopped:**
  `TaskStop` the expired task, then arm. **Count TAILERS, not matches: `ps -eo args | grep -cE
  '[j]ournalctl .*-f '` — a `pgrep -cf` on the name counts COMMAND-LINE TEXT, so every watcher child AND
  anything else whose line merely mentions journalctl inflates it** (observed 2026-10-03: it read 2 with
  exactly one tailer, because a background wrapper's own eval'd line contained the string). It must read
  exactly 1. (No script enforces this; it is a manual check.) Kill an orphaned `journalctl` by pid, never
  `pkill -f`.

## Working

- All development, commits, builds, and tests happen on this host (Strix, WSL2).
- **Soak data (net/census/evictions) accrues only while driving the server with requests.** Never offer
  "wait / let it accumulate" — proceed, and keep working.
- The Bash classifier routes through NInfer in both modes, so a stop still blocks command classification
  while it lasts; it may block swap/restart during prefills. Don't hammer retries — the user runs it with
  `! <cmd>`.

### Dev time vs QA time — `NINFERDEV`

**The operator declares the mode by creating the file `/tmp/NINFERDEV`.** Check it with
`test -e /tmp/NINFERDEV` — present means dev time, absent means QA time. Do not infer the mode from the hour,
from `ANTHROPIC_BASE_URL`, or from how busy the journal looks. (It is a file and not an env var because an
`export` in the operator's shell does not reach tool calls.) The modes differ in what stopping QA *costs*,
not in the safety rules, which hold always.

- **Present — dev time.** QA is not serving the operator; a GPU window or an e2e swap costs essentially
  nothing. **Run the experiment. Never defer a run to "save QA time", and never offer batching as a reason to
  postpone one** — batch only when a run's own validity requires it.
- **Absent — QA time.** QA serves the operator's sessions; the default is **read, don't stop**: journal,
  `/stats`, the request log, the monitor. Drive it rather than idling. If a stop is genuinely needed, say so
  before doing it and treat it as an event.

**Unchanged in both modes** (a mode switch is not a licence):
- the e2e test server must never bind `:8080`; it defaults to `E2E_PORT=8085`;
- the swap is **ONE command, never split**, run **uncapped and in the background**
  (`E2E_TIMEOUT=0 bash tools/e2e/e2e-swap.sh`);
- after any window or swap, QA is restored and *verified* — `/health` 200, wedge sentinel active;
- the sentinel is stopped FIRST and re-armed LAST around a window;
- one journal monitor at a time.

## Reviewing

- **Reviews run in parallel with e2e, one pass per milestone — never a chain of passes with no e2e between
  them.** A review is worth its cost only when the tree changed in a way e2e cannot judge (claims, staging,
  instrumentation safety); otherwise run the thing.
- **How a review is run.** Spawn the global `brutal-honesty-review` agent
  (`~/.claude/agents/brutal-honesty-review.md`, read-only tools) — its own context is the point, so it must
  read the artifacts, not a summary. Never self-review, and never a general-purpose agent. The brief must
  contain: (1) **what changed since the last pass**, by path, with the specific claims to attack; (2) **the
  prohibition** — never run `tools/e2e/e2e-swap.sh`, never stop/start `ninfer.service` or the wedge sentinel
  (`:8080` serves the user's live session); (3) **the verdict to return**, phrased as one question, plus any
  judgement call you want decided rather than invented; (4) **instruction to validate any instrument it
  reasons from** — run the same configuration twice, require agreement. Report shape: findings labelled
  CONFIRMED (it ran something) or PLAUSIBLE, each with `file:line` or a command and its output, plus what it
  did not check. Relay its verdict to the user; do not paraphrase a finding into something weaker.
- **Definitive results come from e2e tests and the full implementation**, not from review prose, not from
  reading artifacts, and not from your own analysis of logs. A claim is a result only when a run produced it;
  state the run.
- **Every finding gets triaged.** An actionable code defect is fixed; a claim/comment or documentation defect
  is corrected *in place*, where a reader meets it. Grep the whole tree for other copies first — a stale claim
  usually has duplicates, and the one a reviewer quotes is rarely the only one.
- **Validate the instrument before believing its output.** Run the same configuration twice and require the
  two runs to agree; if they do not, the probe measures something other than what you think. Print skips and
  denominators, and check counts over the whole log, never the first screenful.
- **A negative is a result too**, and often the useful one: record what was closed and by which control.

## Deploys

- E2E swap = **ONE command, never split**, run **uncapped and in the background**:
  `E2E_TIMEOUT=0 bash ~/ninfer/tools/e2e/e2e-swap.sh`; the completion notification brings the session back.
- **The e2e server must NOT bind `:8080`, in either state.** `:8080` is QA and serves real traffic whatever
  this session is doing; a test server there answers requests the suite never issued. The swap defaults its
  test server to `E2E_PORT=8085` (`PROD_PORT=8080` stays QA's). Two checks enforce it: nothing may listen on
  either port before the test server starts, and after it starts the listener's pid must equal the pid
  recorded by `ninfer-start-test.sh`. Never pass `--port 8080` to a suite by hand.
- **The reverse mix-up costs the same.** Load driven at QA (`:8080`, e.g. `tools/load/prod-load.py`) shares
  the server with real traffic, and the request log cannot separate the two. Such readings are valid for
  concurrency questions and invalid for reuse, queue and counter numbers, which belong on the isolated port.
- Build: `cmake --build build -j --target ninfer-serve`. **After any QA restart on a build:**
  `bash tools/ops/verify-deploy.sh '<string the change adds>'`.
- **Freeze the source from build launch to link.** A source edited while a build runs yields a binary NEWER
  than the source that lacks the change, and both the hash check and `find -newer` pass. If you edited during
  the build, `touch` the file, rebuild, and require the binary hash to CHANGE. (For a string-less change
  nothing catches this — the freeze is the only guard.)
- **`TimeoutStopSec=300` is deliberate** (`/etc/systemd/system/ninfer.service`). A stop-timeout SIGKILL
  destroys the engine's own shutdown path (`fail-all cleanup`, `post-recovery residual`), the only lines that
  report whether a shutdown left occupancy unowned. A stop that still times out at 300 s is a HANG to diagnose,
  never a budget to shrink.
- **A wedged engine is NOT fixed by `~/ninfer-ensure.sh`** (idempotent; reports "already running" whenever the
  unit is *active*, and a wedge is internal — unit active, `/health` and `/v1/messages` 503, `/stats` still
  answering). Recovery is `sudo systemctl restart ninfer.service`; then confirm `/health` is 200 and the
  wedge sentinel is active.

## Observability

- Journal: `journalctl -u ninfer.service --since "..." --no-pager`.
- **`/stats` is on `:8081`, never `:8080`** (`curl -s http://127.0.0.1:8081/stats`; `:8080/stats` serves
  nothing at all). The host-KV census, `pressure.*`, `scheduler.*` and the #6 counters live there; the
  eviction-line format string lives in `src/models/qwen3_5/program/transactions/materialization.cpp` — read it
  there, it has drifted once.
- **Absence is not zero:** a rate-limited line's silence is not non-recurrence — read the counter in `/stats`.
  **A probe count is a denominator; never alert on it or quote it as the event** (`probes` exists only so
  `losses == 0` can be told from a planner that stopped probing).
- **Evidence logs are gitignored on purpose (`*.log`) and are NOT tracked.** Never `git add -f` one. What is
  committed is the MANIFEST beside them: binary sha256, artifact, `git-head`, and each log's own sha256 and
  rc. A claim cites `results/<run>/manifest` in the repo and the log locally.

## Commits

- User-directed. Conventional Commit subjects (`fix(scope):`, `feat(scope):`, `chore(scope):`,
  `docs(scope):`) + descriptive body, no attribution line.
- **Before committing an ENGINE change: the 8-agent soak** (a change to `src/`, `include/` or the serve path;
  a docs/`tools`-only change does not need it). The e2e does not exercise QA's real configuration under more
  parallelism than QA is sized for, so the last gate is a manual load **the OPERATOR runs** (the agent never
  runs it and must not invent a command). Ask the operator to start it and to say when it is stopped.
  1. **Deploy the frozen tree first, and prove it is running:** rebuild, restart QA, then
     `verify-deploy.sh` (see Deploys).
  2. **Arm `tools/ops/ninfer-watch.sh` for ~5 minutes**, one monitor at a time, re-arm on expiry.
  3. **What must be absent, not merely unnoticed:** a `WORKER OOM`/`WORKER CRASH`/`WORKER RECOVER`, a NON-ZERO
     `post-recovery residual` (any prefix), a wedge (the sentinel's `WEDGE` line and its restart), an HTTP 500
     burst, and an unexplained `health` transition. Say which lines you looked for and what the counts were
     (a manual check — no script prints this checklist).
  4. **A green e2e is not a substitute.** The e2e is the acceptance test for the CHANGE; the soak is the
     regression gate for the SERVER.
- **Run the `brutal-honesty-review` agent on commit.**
  1. Triage every finding: **LOAD-BEARING** (can change behaviour or the validity of a measurement: memory
     unsafety, wrong state, a control or instrument that cannot fail on a path production uses, a test whose
     failure is what would catch that) vs **NOT** (every claim/comment/doc/PR-body/commit-message wording
     issue, *including a false one*). Fix both in place; only a load-bearing finding gates another pass.
  2. **Convergence = a fresh pass with ZERO LOAD-BEARING findings.** A pass whose findings are all
     documentation IS converged: fix them in place, note them in one line, and push. Never self-attest "looks
     clean" from a manual skim — the pass decides, and a wrong finding costs a whole cycle.
  3. Repeat until converged, then push.
- **A commit message cannot be corrected after a push**, so a hash or run path it cites must resolve for every
  reader. Cite ids that exist on the remote; put any rewritten-history mapping in `plan.md`.
- **A PULL REQUEST ALWAYS TARGETS THE OPERATOR'S OWN FORK (`gzenz/ninfer`), NEVER UPSTREAM (`Neroued/ninfer`).**
  Push the branch to the `fork` remote (`git push -u fork <branch>`), not `origin`. Open with
  `gh pr create --repo gzenz/ninfer --base master --head <branch>`. Send the whole delta as ONE commit and
  record that squash in `plan.md` (a property of the branch, not of the description).
- **A PR DESCRIPTION SAYS WHAT THE CHANGE DELIVERS, NOT HOW THE BRANCH WAS ASSEMBLED.** Include what changes,
  why, and the evidence (measurements with their conditions, the tests, the soak result). Exclude the squash
  itself, how many commits came from upstream, and how many review passes ran. Provenance that changes what a
  reviewer is looking at is handled by the branch and the mapping in `plan.md`, not by prose.
