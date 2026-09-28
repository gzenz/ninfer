# Journal filter for tools/ops/ninfer-watch.sh: stdout = alerts, `logpath` file = everything diagnostic.
#
# Kept in its own file so it can be TESTED instead of trusted: `tools/ops/ninfer-watch-test.sh` feeds it
# synthetic lines and requires the exact expected alert set. A filter that matches nothing reads exactly
# like a quiet system -- that is how four dead tokens survived in the monitor pattern for weeks, and how
# the first version of THIS filter stayed silent on the wedge signature (below).
#
# awk variables: `logpath` (path). Usage: gawk -v logpath=<path> -f ninfer-watch.awk < <journal stream>

function keep(line) { print line >> logpath; fflush(logpath) }
function alert(line) { print line; fflush(); keep(line) }

# The recovery paths, including the OOM one. `WORKER OOM` was MISSING from the first version, which armed
# only on RECOVER/CRASH: the OOM catch prints `WORKER OOM: ... - recovering`, then runs the same recovery,
# then prints its residual -- so a non-zero residual after an OOM (the wedge signature) was classified as
# un-armed and dropped to the log.
/WORKER OOM|WORKER RECOVER|WORKER CRASH/ { alert($0); next }

# A residual line's AMOUNT is the evidence, and the line already carries its own discriminator -- `(recover)`
# or `(fail-all)`. So: non-zero ALERTS, whatever produced it; all-zero is a restart artifact.
# (`fail_all_locked` runs on the shutdown path too, which is how 19 stops read as 19 healthy recoveries.
# The first version tried to encode "was a recovery" in an `armed` flag; that both missed the OOM path and
# dropped a non-zero `(fail-all)` line, which is the one that would expose an accumulated leak at stop.)
/post-recovery residual/ {
    zero = ($0 ~ /main_kv_pages=0 backend_kv_pages=0 device_state_slots=0 host_state_slots=0 host_kv_bytes=0/)
    if (zero) keep($0)
    else      alert($0)
    next
}

# #6's evidence, and it is NOT an error: an eviction taken while the host tier still had room. It must be
# tested BEFORE the log-only rule below, which matches every eviction -- otherwise this line is swallowed
# and the item it serves goes blind while the filter still looks correct. (The test caught exactly that.)
/private victim evicted: demotable=1/ { alert($0); next }

# #6's OTHER HALF, and it is not an error: a candidate that would have reused MORE than the plan that won had
# NO adoptable route at all, because the catalog had no cell to offer. It is an alert because nothing else in
# the stream shows it -- the request is served anyway, with less reuse.
#
# WHAT IT DOES **NOT** SHOW, and an earlier version of this comment overclaimed here: this is NOT a general
# "the catalog is exhausted, raise it" signal. It fires only when a candidate has no route whatsoever -- no
# cell AND no owner its own solution evicts -- and the case that looks like the obvious one (a candidate that
# RETAINS its source while a full catalog forces consumption or eviction) is EXCLUDED BY CONSTRUCTION:
# consuming a private source publishes into that source's own cell, so it can never be a cell-only failure.
# The catalog's cost there is FUTURE reuse, which no counter here sees. Measured 2026-09-27: `at_risk > 0` in
# 12 printed readings, all with evictable owners available and all with a goal -- i.e. the reading supports a
# bound argument that this event needs `C <= ~2L + Claimed`, well below prod's 32 cells against 4 lanes.
# So read it as "a request lost reuse with no other route", not as a capacity verdict.
#
# DO NOT ARM ON A PROBE COUNT. An earlier version of this instrument alerted on the goal builder's
# cell-free probes (`catalog full: ... refusals=`), which the withdrawn counter reported in place of lost
# reuse -- the planner makes those probes thousands of times per request on its
# way to an ordinary eviction -- 108,544 in one 24-request run, and 97 in another run where four requests
# reused 99.9%. It fired on healthy traffic and could not have falsified a capacity raise. `probes` is a
# denominator, and a denominator is exactly the thing that must not alert.
/catalog cell blocked reuse: occupied=/ { alert($0); next }

# Insight, logpath-only: too frequent to notify on. Their ABSENCE is the signal -- a denominator that never
# appears means the instrument stopped running, not that the count is zero.
/checkpoint StateImage priced|fail-all cleanup|private victim evicted|catalog cell at-risk/ { keep($0); next }

# THE WEDGE ITSELF. It lives in the SENTINEL's journal, not ninfer.service's: the sentinel is what
# detects "engine idle with work pending" and restarts prod. The watcher tailed ONE unit, so on
# 2026-09-26 it missed the most important event of the evening -- a live wedge at 19:51:54 followed by
# the sentinel's restart -- and reported only "health 200 -> 000" three minutes later, with no cause.
# JOURNAL_CMD now follows both units, and this rule alerts on it.
/WEDGE|restarting ninfer/ { alert($0); next }

# A KILLED OR RESTARTED UNIT, which is what took prod down on 2026-09-26 and which NOTHING here matched:
# the `Killed [0-9]` token below is written for the KERNEL's "Killed process" message, and that message
# never appears in a unit-scoped journal (WSL2 publishes no kernel log to the guest at all). What does
# appear is systemd's own line, `Main process exited, code=killed, status=9/KILL`, plus
# `Failed with result 'signal'` and `Scheduled restart job`. Those are the reachable signatures, so they
# are the ones to alert on.
/Main process exited|Failed with result|Scheduled restart job|Stopped ninfer.service|Started ninfer.service/ { alert($0); next }

# Fatal and near-fatal: the process, the driver, the kernel, and the engine's own fatal path.
/CUDA error|cudaError|bad_alloc|terminate called|Assertion|Segmentation|core dumped|Killed [0-9]/ { alert($0); next }

# Conditions an open item is waiting to see: #9's leak shape, and the two admission outcomes that made the
# 2026-09-25 wedge invisible.
/host-arena|single_alloc|admission stalled|admission rejected|subtraction underflow/ { alert($0); next }

# #9's leak shape, #11(a)'s abort branch, #11(b)'s alerting line.
/non-strict release REFUSED|recycled-checkpoint|checkpoint StateImage INCOMPLETE/ { alert($0); next }
