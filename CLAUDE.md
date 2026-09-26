# NInfer — ops rules

Prod = the local server on this WSL2 host (`ninfer.service`, Qwen3.8-27B on the
v3 engine). It serves the user's local Claude Code sessions and the Bash
classifier — which session runs through it is not fixed: read
`env ANTHROPIC_BASE_URL` rather than assuming. A deploy/restart interrupts
whatever prod is serving, including this session while its base URL points at
`127.0.0.1:8080`.

Current state lives in `plan.md` at the repo root -- read its **Current state**
section (actionable: what is fixed and its evidence, what is not, the next
instruments with their validation, the open-claims list), which is kept current;
the dated narrative follows it in the same file, and the still-live items of the
old upstream-adoption plan are carried at its end. **Correction history belongs
there, not in this file** -- these are the rules.

Since 2026-09-25 `plan.md` is the **single record**: `results/HANDOFF.md` was
merged into it and deleted, and `~/.claude/plans/ticklish-sniffing-wadler.md` is
a superseded duplicate kept only for the session that produced it.

## Monitoring
- Keep a monitor armed during a soak/prod. It expires at 30 min — re-arm on expiry and after any stop.
- **Arm `tools/ops/ninfer-watch.sh`.** It is the monitor: it carries state (a regex cannot express the
  rules below), polls `/health` and reports transitions, and splits one pass over the journal into an
  alert stream and a full-fidelity log at `~/ninfer-watch/latest.log`. **Its alert set is
  `tools/ops/ninfer-watch.awk`, and that file is the only specification of it** — do not maintain a second
  copy here, because one already drifted: the pattern that used to be written in this file was missing
  `WORKER OOM` and `private victim evicted: demotable=1`, so following it armed a monitor blind to both.
- Fallback only if the script cannot be used at all — `grep -E` on
  `journalctl -u ninfer.service -f -q -n 0 --output=short-iso` (`-n 0`: `-f` otherwise replays its last 10
  lines, so every re-arm re-reports the previous run's tail):
  `CUDA error|cudaError|bad_alloc|terminate called|Assertion|Segmentation|core dumped|Killed [0-9]|WORKER OOM|WORKER RECOVER|WORKER CRASH|host-arena|single_alloc|admission stalled|admission rejected|subtraction underflow|post-recovery residual|non-strict release REFUSED|recycled-checkpoint|checkpoint StateImage INCOMPLETE|private victim evicted: demotable=1`
- What the tokens mean, and the two rules a flat pattern cannot carry:
  * **`post-recovery residual` — a residual line is evidence when it is NON-ZERO, whatever its prefix.**
    The amount is the signal and the line carries its own discriminator, `(recover)` or `(fail-all)`; an
    all-zero one is restart noise. `fail_all_locked` runs on the **shutdown** path too
    (`engine_core.h:2170-2196`), so every clean stop logs one for an empty engine — which is how 19 zero
    lines were once read as "19 recoveries, residual zero" when every one was a stop. Arming on a preceding
    `WORKER RECOVER`/`WORKER CRASH` instead is *also* wrong: it drops a non-zero residual after
    `WORKER OOM`, which is the wedge signature (`WORKER OOM: … - recovering`, then the same recovery, then
    the residual).
  * **`private victim evicted: demotable=1` is an alert although it is not an error** — it is #6's evidence
    (an eviction taken while the host tier had room), and a crash-only pattern drops it.
  * `admission stalled` / `admission rejected` are the two admission-path outcomes `5fe12cf3` introduced (a
    head blocked with an empty active set: reported during its 5 s grace window, then rejected with
    `Overloaded`). Without them the condition is invisible, which is why the wedge looked like a silent
    outage. `subtraction underflow` names the diagnosis instead of relying on the `WORKER RECOVER` prefix;
    `WORKER CRASH` is the fatal path, distinct from `WORKER RECOVER`; `non-strict release REFUSED` is a
    release that did not free its address or state image (the leak's shape); `recycled-checkpoint` is the
    abort branch over a reused checkpoint (#11a).
  * `checkpoint StateImage INCOMPLETE` is #11(b)'s alerting line: pricing found a state image that is not a
    restorable checkpoint (invalid, not immutable, no settled replica, or zero epoch). Its companion
    `checkpoint StateImage priced: incomplete=0 (… denominator=…)` prints on a separate schedule so that
    "priced, none incomplete" can be told from "never priced" — **if the denominator line is absent from a
    long journal, pricing is not running; do not read its silence as a healthy zero.** The same logic
    applies to every counter here: absence is not zero.
- **A token that cannot fire reads exactly like a quiet system.** Before adding or removing one, check it
  with `grep -rl '<token>' src/`. Most are strings the engine prints and must match a source file; a few are
  journal-level signatures that match none and still fire — `Killed [0-9]` (the kernel's OOM killer),
  `terminate called`, `Assertion`, `Segmentation`, `core dumped` (the C++ runtime, libc, the kernel).
  `CUDA error|cudaError` DOES appear in `src/` and is not in that class. Four tokens that matched nothing
  (`WEDGE`, `planner-no-plan`, `relief-shared`, `host-state-pool`) were carried here for weeks.
- **The watcher follows TWO units: `ninfer.service` and `ninfer-wedge-sentinel.service`.** The wedge is
  announced by the SENTINEL (`WEDGE (A/B): engine idle with work pending Ns -- restarting ninfer`), so
  watching one unit means the event that explains a dead prod is invisible -- observed 2026-09-26: a live
  wedge at ~19:51:54, the sentinel's restart at 19:54:37, and the monitor reporting only a bare
  `health 200 -> 000` with no cause. **`WEDGE` is therefore a live token**, and the note below that it
  "matches nothing in `src/`" is true and beside the point: it is a sentinel output, not an engine string,
  so the `grep src/` validation rule does not cover a token emitted by a helper script. That rule is what
  removed it, and tonight is what that cost.
- **A wedge is now CAPTURED before it is restarted.** `tools/monitor/wedge-sentinel.sh` writes
  `~/ninfer-watch/wedge-<ts>.waits` (per-thread `state=`/`wchan=`, which needs no symbols) and `.bt`
  (best-effort backtrace) immediately before its `systemctl restart`. A wedged engine cannot run its own
  shutdown path -- the shutdown needs the lock the wedge holds -- so this is the only moment the evidence
  exists, and on 2026-09-26 the restart destroyed it. Read those two files FIRST when a wedge is reported.
  The backtrace resolves only the innermost frame on a release build; the wait distribution is what
  characterised the 2026-09-26 wedge (21 of 25 threads on `futex_do_wait`, five on one condvar, with ONE
  thread spinning in `sched_yield`).
- One monitor at a time (duplicates double-notify). After stopping one, kill any orphaned `journalctl` —
  by **`pkill -f 'journalctl -u ninfe[r]'`**, bracketed, because an unbracketed `-f` pattern matches the
  calling shell's own command line (see the traps below).

## Working
- All development, commits, builds, and tests happen on this host (Strix,
  WSL2) — the Mac workflow is retired (2026-09-19). Never plan "do X on the
  Mac, pull here" steps.
- Soak data (net/census/evictions) accrues only while driving the server with
  requests. Never offer "wait / let it accumulate" as an option — proceed, and
  keep working.

### Dev time vs prod time — `NINFERDEV`

**The operator declares which mode the host is in by creating the file `/tmp/NINFERDEV`.** Check it with
`test -e /tmp/NINFERDEV` — present means dev time, absent means prod time. Do not infer the mode from the
hour, from `ANTHROPIC_BASE_URL`, or from how busy the journal looks.

**Why a file and not an environment variable:** an `export` typed into the operator's session shell does
not reach the agent's tool calls — each Bash invocation gets a fresh shell initialised from the profile, so
`env NINFERDEV` comes back empty while the operator's own shell has it set. A file is visible to every
invocation. (`~/.claude/settings.json`'s `env` block *does* propagate — that is where `ANTHROPIC_BASE_URL`
comes from — so an env var set there would also work; the file is the lighter switch.) The two modes differ
in what stopping prod *costs* and in what the session should do with its time — they do NOT differ in the
safety rules, which hold always (see below).

- **`/tmp/NINFERDEV` exists — dev time.** Prod is not serving the operator: nothing local depends on it, so a GPU
  window or an e2e swap costs essentially nothing beyond the Bash classifier's round-trip for the
  duration of the stop. **Run the experiment. Do not defer a run to "save prod time", and never offer
  batching as a reason to postpone one** — batch only when a run's own validity requires it (a shared
  build tree, a single freeze covering several suites). This is the mode where the open experiments are
  the work; a session here that only reads and edits is wasting the cheapest resource it has.
- **`/tmp/NINFERDEV` absent — prod time.** Prod is serving the operator's sessions. Stopping it interrupts them
  and breaks soak continuity, so the default action is **read, don't stop**: journal, `/stats`, the
  request log, the monitor. Collect insights — and note that soak data (net/census/evictions) accrues
  only while the server is driven with requests, so drive it rather than idling. If a stop is genuinely
  needed, say so before doing it and treat it as an event, not routine.

**Unchanged in both modes** (a mode switch is not a licence) — these exist because each one has already
cost an outage:
- the e2e test server must never bind `:8080`; it defaults to `E2E_PORT=8085`;
- the swap is ONE blocking foreground command, never split, never detached;
- `pkill -f` / `pgrep -f` patterns match the **calling shell's own command line**, including text that is
  only a regex. `pkill -f 'journalctl -u ninfer'` killed its own shell (exit 144, no output — which reads
  like a failed kill, not a self-kill); the same trap killed this session three times via the string
  `build/apps/ninfer-serve` in an e2e-swap command line. Bracket one character (`ninfe[r]`) or put the
  pattern in a script — **and note that bracketing only helps if the unbracketed text appears nowhere else
  on that command line**: a call that both ran `journalctl -u ninfer` and bracketed-pgrep'd for it still
  matched its own shell;
- after any window or swap, prod is restored and *verified* — `/health` 200, wedge sentinel active;
- the sentinel is stopped FIRST and re-armed LAST around a window;
- one journal monitor at a time.

The Bash classifier routes through NInfer in both modes, so a stop still blocks command classification
while it lasts. In dev time that is the entire cost; in prod time it is the smallest part of it.

## Reviewing
- **Reviews run in parallel with e2e, one pass per milestone — never a chain of
  passes with no e2e between them.** Ten consecutive review passes once ran
  while the last five had no e2e test; the passes were finding real defects in
  the *documentation* while the actual bugs sat untouched. A review is worth its
  cost only when the tree changed in a way e2e cannot judge (claims, staging,
  instrumentation safety) — otherwise run the thing.
- **How a review is run.** Spawn the global `brutal-honesty-review` agent
  (`~/.claude/agents/brutal-honesty-review.md`, backed by the skill of the same
  name, read-only tools) — its own context is the point, so it must read the
  artifacts rather than a summary of them. Never self-review, and never a
  general-purpose agent. The brief must contain:
  1. **What changed since the last pass**, by path, with the specific claims to
     attack — not "review this".
  2. **The prohibition**: never run `tools/e2e/e2e-swap.sh`, never stop or start
     `ninfer.service` or the wedge sentinel — `:8080` serves the user's live
     session. It may read anything, including the running process's logs.
  3. **The verdict to return**, phrased as one question (e.g. "is this fit to
     split and push?"), plus any judgement call you want decided rather than
     invented (whether a documentation omission counts, whether a control is
     valid).
  4. **Instruction to validate any instrument it reasons from** — run the same
     configuration twice, require agreement — because most false conclusions in
     this repo came from an instrument measuring nothing.
  Report shape we rely on: mode and calibration; findings each labelled
  CONFIRMED (it ran something) or PLAUSIBLE, each with `file:line` or a command
  and its output; what is genuinely good, verified rather than courteous; and
  what it did not check. Relay its verdict to the user; do not paraphrase a
  finding into something weaker.
- **Definitive results come from e2e tests and the full implementation**, not
  from review prose, not from reading artifacts, and not from your own analysis
  of logs. A claim is a result only when a run produced it; state the run.
- **Every finding gets triaged**: an actionable code defect is fixed; a
  claim/comment or documentation defect is corrected *in place*, where a reader
  meets it — a note in a triage section does not unstate a wrong claim. Grep the
  whole tree for other copies first: a stale claim usually has duplicates, and
  the one a reviewer quotes is rarely the only one.
- **Validate the instrument before believing its output.** Run the same
  configuration twice and require the two runs to agree; if they do not, the
  probe measures something other than what you think. Most false conclusions in
  this repo came from an instrument that measured nothing (a silent skip, a
  probe that never executed, a hash that cannot distinguish one ULP from
  divergence, a wrong dtype, a comparison keyed by an allocation detail) while
  looking like a clean result. Print skips and denominators, and check counts
  over the whole log, never the first screenful.
- **A negative is a result too**, and often the useful one: record what was
  closed and by which control, so the same surface is not re-audited.

## Deploys
- E2E swap = ONE blocking foreground command, ~8 min session freeze:
  `E2E_TIMEOUT=420 bash ~/ninfer-e2e/e2e-swap.sh`. Never split it, never detach.
- **Where this session's own model traffic goes is not fixed — check it, don't assume:**
  `env ANTHROPIC_BASE_URL`. It is OpenRouter at the time of writing, and the operator moves it back to
  local NInfer when they judge the server stable enough, so the tooling must be correct in
  both states rather than tuned to one.
- **The e2e server must NOT bind :8080, in either state.** :8080 is prod, and prod serves
  real traffic whatever this session is doing: the classifier, and any live local session
  (this one included whenever `ANTHROPIC_BASE_URL` points at 127.0.0.1:8080). A test
  server there answers requests the suite never issued — tainting every cache and timing
  number with an unknown workload, and 400ing them on the test profile's smaller context.
  If this session *is* local, a swap also takes the session down for its duration.
  The swap defaults its test server to `E2E_PORT=8085` (`PROD_PORT=8080` stays prod's) and
  forwards `--port` to the suite; `~/.config/ninfer.conf` pins prod to 8080, so anything on
  8085 is the test server by construction. Two checks enforce it: nothing may listen on
  either port before the test server starts, and after it starts the listener's pid must
  equal the pid recorded by `ninfer-start-test.sh`. Never pass `--port 8080` to a suite by
  hand — a run that talks to prod is not a run.
- **The reverse mix-up costs the same.** Load driven at prod (:8080, e.g.
  `tools/load/prod-load.py`, deliberately not in `tools/e2e/` and asserting nothing) shares
  the server with that real traffic, and the request log cannot separate the two — same
  protocol, same growing-context shape, no client id. Readings taken that way are valid for
  concurrency questions (does a foreign marker appear under N lanes?) and invalid for
  reuse, queue and counter numbers, which belong on the isolated port.
- The Bash classifier routes through NInfer; it may block swap/restart during
  prefills or thrash. Don't hammer retries — the user runs it with `! <cmd>`.
- Build: `cmake --build build -j --target ninfer-serve`. After restarting prod on a build, verify by the
  **running exe's hash**, not the build directory's: `sha256sum /proc/$(systemctl show -p MainPID --value
  ninfer.service)/exe` must equal `sha256sum build/apps/ninfer-serve`. A build-directory hash changes on
  every relink and proves nothing on its own.
- **Stopping prod is not a "kill it if busy" operation.** The unit runs with `TimeoutStopSec=300` (raised
  from 30 on 2026-09-26, `/etc/systemd/system/ninfer.service`; nothing else writes it, and
  `~/ninfer-ensure.sh` does not, so an edit there is authoritative). A stop-timeout SIGKILL destroys the
  engine's OWN shutdown path — `fail-all cleanup` and `post-recovery residual`, the only lines that report
  whether a shutdown left occupancy unowned — and 30 s is not a budget a server can meet while unpinning
  30 GiB of host KV and tearing down CUDA on WSL2: measured 39 s from `Stopping` to SIGKILL, so the shutdown
  never ran and every stop silently lost that evidence. **A stop that still times out at 300 s is a HANG to
  diagnose, not a budget to shrink back.**
- **A wedged engine is NOT fixed by `~/ninfer-ensure.sh`.** That script is idempotent and
  reports "already running with desired config" whenever the unit is *active* — and a wedge
  is internal to the engine, not a unit failure (observed 2026-09-25: unit active,
  `/health` and `/v1/messages` both 503, scheduler completely empty, `/stats` still
  answering). Recovery is `sudo systemctl restart ninfer.service`; then confirm `/health`
  is 200 and the wedge sentinel is active again.

## Observability
- Journal: `journalctl -u ninfer.service --since "..." --no-pager`.
- Stats: **`curl -s http://127.0.0.1:8081/stats`** → host-KV census (per-tier
  {entries,bytes}: dead/live/idle/active), host-KV unit bytes, `pressure.*`,
  `scheduler.*`. **The stats endpoint is `:8081`; `:8080/stats` serves nothing at all**
  (`http=000 bytes=0`) — querying the wrong port there once made a working counter look dead.
- `/stats` is where a counter that the journal has stopped printing can still be read. Two for #6:
  `pressure_private_evictions_demotable` and its denominator `pressure_private_eviction_checks` — needed
  because the eviction print below is rate-limited to the first 8 and then every 512th, so its silence
  after the 8th means "not printed", not "not recurring".
- **The eviction line is**
  `[engine] private victim evicted: demotable=%d frontier=%u endpoint=%d rewrite=%d anchors=%zu
  host_state_slots=%u/%llu host_kv=%zu/%llu checked=%llu demotable_total=%llu`
  (`materialization.cpp:2157`). `demotable` is a **capacity** test at the decision — `host_state_slots` and
  `host_kv_bytes` both under capacity — and NOT a statement that the victim could have been demoted (that
  needs a complete, immutable, settled StateImage) nor that the planner chose eviction for value reasons.
  The per-tier `{dead,live,idle,active}` census is in `/stats`, not in the journal.
- **Evidence logs are gitignored on purpose (`*.log`, `.gitignore:45`) and are NOT tracked.** Never
  `git add -f` one. What is committed is the MANIFEST beside them: binary sha256, artifact, `git-head`, and
  -- for runs made by `tools/e2e/prefix-real-evidence.sh` -- each log's own sha256 and rc. A claim cites
  `results/<run>/manifest` in the repo and the log locally; a log that was never written down is not
  evidence.

## Commits
- User-directed. Conventional Commit subjects (`fix(scope):`, `feat(scope):`,
  `chore(scope):`, `docs(scope):`) + descriptive body, no attribution line.
- Run brutal-honesty-review agent on commit.
  1. Triage findings: actionable code defect vs documentation/PR-body item. Fix real defects; document inherent limits
  (e.g. a path untestable without mock infra this suite lacks) in the PR body.
  2. Re-run the  brutal-honesty-review  agent on the new state. Convergence = a fresh pass finds ZERO actionable
  defects. Fixing the findings is NOT convergence; a clean review pass is. Never self-attest "looks clean" from a
  manual skim — re-run the agent.
  3. Repeat until converged, then push.
- **A commit message cannot be corrected after a push, so a hash or a run path it cites must resolve for
  every reader.** Cite ids that exist on the remote, never a pre-rewrite id; put the mapping in `plan.md`
  if history was rewritten.
