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
old upstream-adoption plan are carried at its end.

Since 2026-09-25 that is the **single record**. Two files it used to be split
across are gone as sources of truth: `results/HANDOFF.md` (the actionable half)
was merged into `plan.md` and deleted, and
`~/.claude/plans/ticklish-sniffing-wadler.md` is a superseded duplicate kept only
for the session that produced it -- read and edit `plan.md`, not those.

## Monitoring
- Keep a journal monitor armed during a soak/prod. It expires at 30 min —
  re-arm on expiry and after any stop.
- Pattern (full, not crash-only):
  `CUDA error|cudaError|bad_alloc|terminate called|Assertion|Segmentation|core dumped|Killed [0-9]|WORKER RECOVER|WORKER CRASH|host-arena|single_alloc|admission stalled|admission rejected|subtraction underflow|post-recovery residual|non-strict release REFUSED|recycled-checkpoint|checkpoint StateImage INCOMPLETE`
  on `journalctl -u ninfer.service -f -q --output=short-iso`.

  (That sentence used to say "the last four were added 2026-09-25"; the pattern has grown since, so the
  count no longer identifies them and the tokens are named individually below instead.) `admission stalled` / `admission rejected` are the two
  admission-path outcomes `5fe12cf3` introduced (a head blocked with an empty active set: reported
  during its 5 s grace window, then rejected with `Overloaded`); without them the condition is
  invisible to this monitor, which is the whole reason the wedge looked like a silent outage.
  `subtraction underflow` names the diagnosis instead of relying on the `WORKER RECOVER` prefix, and
  `post-recovery residual` is the leak test: the line prints occupancy right after a recovery, and
  **its expected value is all-zero** -- a non-zero residual there is unowned occupancy, i.e. the cause
  of the wedge (tasks #9), visible at the moment it happens rather than hours later in `/stats`.

  **The zero case proves nothing (corrected 2026-09-26).** `fail_all_locked` also runs on the
  **shutdown** path (`engine_core.h:2170-2196`), so every clean prod stop logs this line for an empty
  engine. On 2026-09-26 I read 19 of those zero lines as "19 recoveries, residual zero" and wrote it
  into plan.md; every one was a shutdown, with `server stopped` before it and `WORKER RECOVER` = 0 in
  the whole journal. **A residual line is evidence when it is NON-ZERO**, whatever produced it; an all-zero
  one is the noise of a restart, and with GPU windows in use there is one per
  window. (The rule was first written as "only when a `WORKER RECOVER`/`WORKER CRASH` precedes it", which
  both missed the `WORKER OOM` path -- the OOM catch prints `WORKER OOM: … - recovering`, then the same
  recovery, then the residual -- and would have discarded a non-zero `(fail-all)` line. The line carries
  its own discriminator, `(recover)` or `(fail-all)`; the amount is what a reader needs.)
  window.

  `checkpoint StateImage INCOMPLETE` is #11(b)'s alerting line: checkpoint pricing found a state image
  that is not a restorable checkpoint (invalid, not immutable, no settled replica, or zero epoch). It is rate-limited on its own count, so the FIRST occurrence prints whatever
  the traffic volume; the companion line `checkpoint StateImage priced: incomplete=0 (... denominator=...)`
  prints on a separate schedule and exists so that "priced, none incomplete" can be told from "never
  priced". If the denominator line is absent from a long journal, pricing is not running -- do not read
  its silence as a healthy zero.

  **Corrected 2026-09-26, and the correction was made by grep rather than by taste.** Four tokens this
  pattern used to carry match *nothing* in `src/` -- `WEDGE`, `planner-no-plan`, `relief-shared`,
  `host-state-pool` -- so they could never fire, and a pattern that cannot fire reads exactly like a
  quiet system. Three live instruments were missing and are now here: `WORKER CRASH` (the fatal path,
  distinct from `WORKER RECOVER`), `non-strict release REFUSED` (a release that did not free its address
  or state image: the leak's shape), and `recycled-checkpoint` (the abort branch over a reused
  checkpoint, #11a).

  When adding or removing a token: check it with `grep -rl '<token>' src/`. Most tokens are strings the
  engine prints, so they should match a source file; some are journal-level signatures and match none --
  `Killed [0-9]` (the kernel's OOM killer), `terminate called`, `Assertion`, `Segmentation` and
  `core dumped` (the C++ runtime, libc and the kernel; all five are absent from `src/`, and all five can
  fire -- `terminate called` appears in the #9 reproduction logs). `CUDA error|cudaError` is the driver's
  and does appear in `src/` (30 files for the alternation; `cudaError` alone is 29), so it is NOT in that class: the corrected claim here used to list
  it among the ones that match nothing, which a `grep -rl cudaError src/` refutes. An engine instrument
  that prints to the journal belongs here; a token nothing can ever emit does not.
- **The pattern is now a program: `tools/ops/ninfer-watch.sh`** (filter `tools/ops/ninfer-watch.awk`,
  contract tested by `tools/ops/ninfer-watch-test.sh`, whose fixture is the specification):
  * a `post-recovery residual` line **alerts when it is non-zero, whatever its prefix**, and an all-zero
    one is log-only -- `fail_all_locked` also runs on the shutdown path, which is how 19 null readings got
    mistaken for 19 healthy recoveries. (An earlier version armed on `WORKER RECOVER`/`WORKER CRASH` and
    alerted only when armed: that dropped a non-zero residual after `WORKER OOM` -- the wedge signature --
    and a non-zero `(fail-all)` line. `WORKER OOM` is now in the alert set explicitly.)
  * `private victim evicted: demotable=1` is an **alert** even though it is not an error: it is #6's
    evidence (an eviction taken while the host tier had room), and a crash-only pattern drops it.
  It also **polls `/health` and reports transitions**, because the 2026-09-25 wedge had the unit `active`,
  an empty scheduler, and nothing in the journal to grep. The token list above is its alert set; the
  insight lines whose *absence* is the signal -- `checkpoint StateImage priced`, `fail-all cleanup`, and
  every `private victim evicted` -- go to the log file only (`~/ninfer-watch/latest.log`), so the alert
  stream stays signal and the log still proves the instrument ran.
- One monitor at a time (duplicates double-notify). After stopping one,
  `pkill -f 'journalctl -u ninfer'` any orphaned process.

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

**Why a file and not an environment variable** (this was tried first, on 2026-09-26): an `export` typed
into the operator's session shell does not reach the agent's tool calls — each Bash invocation gets a
fresh shell initialised from the profile, so `env NINFERDEV` came back empty while the operator's own
shell had it set. A file is visible to every invocation. (`~/.claude/settings.json`'s `env` block *does*
propagate — that is where `ANTHROPIC_BASE_URL` comes from — so an env var set there would also work; the
file is simply the lighter switch, and `/tmp` makes it obviously a session-scoped declaration.) The two modes differ in what stopping prod *costs*, and in what the session should be
doing with its time — they do NOT differ in the safety rules, which hold always (see below).

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
- never put the string `build/apps/ninfer-serve` in a command line that runs `tools/e2e/e2e-swap.sh`
  (its `pkill -f` matches the caller and has killed this session three times);
- **and that rule is about `-f`, not about that string.** Any `pkill -f`/`pgrep -f` pattern written into a
  Bash tool call matches the *calling shell's own command line*, including text that is only a regex:
  `pkill -f 'journalctl -u ninfer'` killed its own shell on 2026-09-26 and returned **exit 144** with no
  output, which reads like a failed kill rather than a self-kill. Bracket one character (`ninfe[r]`) so the
  pattern cannot match its own text, or put the pattern in a script and call the script;
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
  meets it — a note in a triage section does not unstate a wrong claim.
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
  `env ANTHROPIC_BASE_URL`. Right now it is OpenRouter, and the operator moves it back to
  local NInfer when they judge the server stable enough, so the tooling must be correct in
  both states rather than tuned to one.
- **The e2e server must NOT bind :8080, in either state.** :8080 is prod, and prod serves
  real traffic whatever this session is doing: the classifier, and any live local session
  (this one included whenever `ANTHROPIC_BASE_URL` points at 127.0.0.1:8080). A test
  server there answers requests the suite never issued — tainting every cache and timing
  number with an unknown workload, and 400ing them on the test profile's smaller context.
  If this session *is* local, a swap also takes the session down for its duration, which is
  the freeze already documented below.
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
- Build: `cmake --build build -j --target ninfer-serve`.
- **A wedged engine is NOT fixed by `~/ninfer-ensure.sh`.** That script is idempotent and
  reports "already running with desired config" whenever the unit is *active* — and a wedge
  is internal to the engine, not a unit failure (observed 2026-09-25: unit active,
  `/health` and `/v1/messages` both 503, scheduler completely empty, `/stats` still
  answering). Recovery is `sudo systemctl restart ninfer.service`; then confirm `/health`
  is 200 and the wedge sentinel is active again.

## Observability
- Journal: `journalctl -u ninfer.service --since "..." --no-pager`.
- Stats: `curl -s http://127.0.0.1:8080/stats` → host-KV census (per-tier
  {entries,bytes}: dead/live/idle/active), host-KV unit bytes, `pressure.*`,
  `scheduler.*`.
- **The eviction line is**
  `[engine] private victim evicted: demotable=%d frontier=%u endpoint=%d rewrite=%d anchors=%zu
  host_state_slots=%u/%llu host_kv=%zu/%llu checked=%llu demotable_total=%llu`
  (`materialization.cpp:2157`), rate-limited to the first 8 and then every 512th. **Corrected 2026-09-26:**
  the two sentences that stood here -- "eviction lines carry `tier=` (dead/live/idle/active)" and "the
  reaper logs `[host-state-pool] reap=stale`" -- name instruments that **cannot emit anything**:
  `grep -rn 'host-state-pool' src/` and `grep -rn 'reap=' src/` are both empty, and `tier=` occurs in
  `src/` only inside `frontier=`. They are pre-v3 strings, and they are the same defect this file warns
  about for monitor tokens. The per-tier `{dead,live,idle,active}` census is real, but it is in `/stats`
  (the line above), not in the journal. `tools/ops/ninfer-watch.sh` reads the journal for the rest.
- **Evidence logs are gitignored on purpose (`*.log`, `.gitignore:45`) and are NOT tracked.** What is
  committed is the MANIFEST beside them: it names the binary's sha256, the artifact, `git-head`, and -- for
  runs made by `tools/e2e/prefix-real-evidence.sh` -- each log's own sha256 and rc. So a claim cites
  `results/<run>/manifest` in the repo and the log locally. Two consequences worth stating: a log that was
  never written down is not evidence (the defect the 2026-09-26 review found), and force-adding a log past
  the ignore rule is a mistake -- made that same day, and undone.

## Commits
- User-directed. Conventional Commit subjects (`fix(scope):`, `feat(scope):`,
  `chore(scope):`, `docs(scope):`) + descriptive body, no attribution line.
- Run brutal-honesty-review agent on commit.
  1. Triage findings: actionable code defect vs documentation/PR-body item. Fix real defects; document inherent limits
  (e.g. a path untestable without mock infra this suite lacks) in the PR body.
  2. Re-run the  brutal-honesty-review  agent on the new state. Convergence = a fresh pass finds ZERO actionable
  defects. Fixing the findings is NOT convergence; a clean review pass is. Never self-attest "looks clean" from a
  manual skim — re-run the agent.
  3. Repeat until converged, then push.
