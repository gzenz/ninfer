#!/usr/bin/env bash
# THE REQUEST LOG IS A MIXED POPULATION. Say which server a number came from, or do not quote it.
#
#   bash tools/ops/reqs.sh instances                # instances present, newest first, with counts
#   bash tools/ops/reqs.sh current                  # the live QA instance id (from /stats)
#   bash tools/ops/reqs.sh QA                       # QA-only records (filters by the live instance)
#   bash tools/ops/reqs.sh <instance-id-or-prefix>  # records from that instance
#
# WHY. `plan.md` §5: the e2e TEST SERVER writes to QA's path (`~/ninfer-requests.jsonl`), so an aggregate
# over that file mixes e2e, benchmark and QA traffic. `server_instance_id` is the only field that separates
# them, and the e2e's cold-cache requests (a fresh server re-prefilling 16k tokens with `hit=14`) look
# exactly like a reuse defect -- which is how a monitor once alerted on the test server's traffic as though
# it were QA's. **A figure from this file is QA-only only if it was filtered.**
set -uo pipefail
LOG="${REQ_LOG:-$HOME/ninfer-requests.jsonl}"
[ -r "$LOG" ] || { echo "FAIL: $LOG not readable"; exit 2; }

case "${1:-}" in
  instances|"")
    python3 - "$LOG" <<'PY'
import json, sys, collections
c = collections.Counter()
for line in open(sys.argv[1], errors="replace"):
    line = line.strip()
    if not line:
        continue
    try:
        r = json.loads(line)
    except Exception:
        continue
    i = r.get("server_instance_id")
    if i:
        c[i] += 1
print(f"{len(c)} instances in {sys.argv[1]} (newest first by record count):")
for i, n in c.most_common(15):
    print(f"  {n:7d}  {i}")
print()
print("Pass one of these to this script to get that instance's records only.")
PY
    ;;
  current)
    curl -s http://127.0.0.1:8081/stats 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin).get("server_instance_id",""))' 2>/dev/null \
      || echo "(could not read /stats on :8081)"
    ;;
  QA)
    ID="$(curl -s http://127.0.0.1:8081/stats 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin).get("server_instance_id",""))' 2>/dev/null)"
    [ -z "$ID" ] && { echo "FAIL: cannot read the live instance id from /stats on :8081"; exit 1; }
    echo "# QA-only: server_instance_id == $ID" >&2
    grep -F "\"server_instance_id\":\"$ID\"" "$LOG" 2>/dev/null || grep -F "$ID" "$LOG"
    ;;
  *)
    echo "# records from instances matching '$1'" >&2
    grep -F "$1" "$LOG"
    ;;
esac
