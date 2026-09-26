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
  `CUDA error|cudaError|bad_alloc|terminate called|Assertion|Segmentation|core dumped|Killed [0-9]|WORKER RECOVER|WORKER CRASH|host-arena|single_alloc|admission stalled|admission rejected|subtraction underflow|post-recovery residual|non-strict release REFUSED|recycled-checkpoint`
  on `journalctl -u ninfer.service -f -q --output=short-iso`.

  The last four were added 2026-09-25. `admission stalled` / `admission rejected` are the two
  admission-path outcomes `5fe12cf3` introduced (a head blocked with an empty active set: reported
  during its 5 s grace window, then rejected with `Overloaded`); without them the condition is
  invisible to this monitor, which is the whole reason the wedge looked like a silent outage.
  `subtraction underflow` names the diagnosis instead of relying on the `WORKER RECOVER` prefix, and
  `post-recovery residual` is the leak test: the line prints occupancy right after a recovery, and
  **its expected value is all-zero** -- a non-zero residual there is unowned occupancy, i.e. the cause
  of the wedge (tasks #9), visible at the moment it happens rather than hours later in `/stats`.

  **The zero case proves nothing (corrected 2026-09-26).** `fail_all_locked` also runs on the
  **shutdown** path (`engine_core.h:2152-2178`), so every clean prod stop logs this line for an empty
  engine. On 2026-09-26 I read 19 of those zero lines as "19 recoveries, residual zero" and wrote it
  into plan.md; every one was a shutdown, with `server stopped` before it and `WORKER RECOVER` = 0 in
  the whole journal. A residual line is evidence **only when a `WORKER RECOVER` or `WORKER CRASH`
  precedes it**; alone it is the noise of a restart -- and with GPU windows in use there is one per
  window.

  **Corrected 2026-09-26, and the correction was made by grep rather than by taste.** Four tokens this
  pattern used to carry match *nothing* in `src/` -- `WEDGE`, `planner-no-plan`, `relief-shared`,
  `host-state-pool` -- so they could never fire, and a pattern that cannot fire reads exactly like a
  quiet system. Three live instruments were missing and are now here: `WORKER CRASH` (the fatal path,
  distinct from `WORKER RECOVER`), `non-strict release REFUSED` (a release that did not free its address
  or state image: the leak's shape), and `recycled-checkpoint` (the abort branch over a reused
  checkpoint, #11a).

  When adding or removing a token: check it with `grep -rl '<token>' src/`. Most tokens are strings the
  engine prints, so they should match a source file; **two are journal-level signatures and match none** --
  `Killed [0-9]` (the kernel's OOM killer) and `CUDA error|cudaError` (the driver). An engine instrument
  that prints to the journal belongs here; a token nothing can ever emit does not.
- One monitor at a time (duplicates double-notify). After stopping one,
  `pkill -f 'journalctl -u ninfer'` any orphaned process.

## Working
- All development, commits, builds, and tests happen on this host (Strix,
  WSL2) — the Mac workflow is retired (2026-09-19). Never plan "do X on the
  Mac, pull here" steps.
- Soak data (net/census/evictions) accrues only while driving the server with
  requests. Never offer "wait / let it accumulate" as an option — proceed, and
  keep working.

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
- Eviction lines carry `tier=` (dead/live/idle/active); the reaper logs
  `[host-state-pool] reap=stale`.

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
