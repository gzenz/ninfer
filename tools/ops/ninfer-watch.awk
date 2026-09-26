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

# Insight, logpath-only: too frequent to notify on. Their ABSENCE is the signal -- a denominator that never
# appears means the instrument stopped running, not that the count is zero.
/checkpoint StateImage priced|fail-all cleanup|private victim evicted/ { keep($0); next }

# Fatal and near-fatal: the process, the driver, the kernel, and the engine's own fatal path.
/CUDA error|cudaError|bad_alloc|terminate called|Assertion|Segmentation|core dumped|Killed [0-9]/ { alert($0); next }

# Conditions an open item is waiting to see: #9's leak shape, and the two admission outcomes that made the
# 2026-09-25 wedge invisible.
/host-arena|single_alloc|admission stalled|admission rejected|subtraction underflow/ { alert($0); next }

# #9's leak shape, #11(a)'s abort branch, #11(b)'s alerting line.
/non-strict release REFUSED|recycled-checkpoint|checkpoint StateImage INCOMPLETE/ { alert($0); next }
