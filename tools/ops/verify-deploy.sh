#!/usr/bin/env bash
# IS QA RUNNING THE TREE I THINK IT IS? Three checks, because two of them pass while the binary is wrong.
#
#   bash tools/ops/verify-deploy.sh                 # after restarting ninfer.service on a fresh build
#   bash tools/ops/verify-deploy.sh 'some new string'  # plus: the running exe must contain it
#
# WHY THREE. `sha256sum /proc/<pid>/exe == sha256sum build/apps/ninfer-serve` proves **running == last link**
# and says NOTHING about whether the last link contains the change: on 2026-10-01 QA served a build missing
# its change for ~10 minutes while both hashes agreed, because the target under iteration was a different
# one and `ninfer-serve` was never relinked. `find src include apps -newer build/apps/ninfer-serve` closes
# that -- and is itself blind to a source edited WHILE the build ran (the binary ends up NEWER than the
# source, so `-newer` reports clean). Hence the third check: grep the RUNNING exe for a string the change
# introduces, with an old string as the control. For a naming-only or string-less change that grep cannot
# exist, and the only guard is freezing the source during the build.
#
#   bash tools/ops/verify-deploy.sh 'RopeScaling' 'old_marker_that_should_be_gone'
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 2

BIN=build/apps/ninfer-serve
UNIT=ninfer.service
PID="$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null)"
[ -z "$PID" ] || [ "$PID" = "0" ] && { echo "FAIL: $UNIT has no MainPID (not running?)"; exit 1; }

RUN_HASH="$(sha256sum "/proc/$PID/exe" 2>/dev/null | cut -d' ' -f1)"
BLD_HASH="$(sha256sum "$BIN" 2>/dev/null | cut -d' ' -f1)"
[ -z "$BLD_HASH" ] && { echo "FAIL: $BIN does not exist -- build first"; exit 1; }
echo "running exe : ${RUN_HASH:0:16}"
echo "built binary: ${BLD_HASH:0:16}"
if [ "$RUN_HASH" != "$BLD_HASH" ]; then
  echo "FAIL 1: the RUNNING exe is not the built one -- QA is serving a different binary."
  exit 1
fi
echo "ok 1: running == last link   <-- this does NOT mean last link == HEAD"

NEWER="$(find src include apps -newer "$BIN" 2>/dev/null)"
if [ -n "$NEWER" ]; then
  echo "FAIL 2: source is newer than the binary -- it was not rebuilt from this tree:"
  echo "$NEWER" | head -10
  exit 1
fi
echo "ok 2: no source newer than the binary   <-- this does NOT catch a source edited DURING the build"

if [ "$#" -ge 1 ]; then
  n="$(grep -ac -- "$1" "/proc/$PID/exe" 2>/dev/null || echo 0)"
  echo "grep '$1' in the running exe: $n"
  [ "$n" -ge 1 ] || { echo "FAIL 3: the running exe does not contain '$1' -- it is not this change"; exit 1; }
  echo "ok 3: the change's string is IN the running binary"
  if [ "$#" -ge 2 ]; then
    o="$(grep -ac -- "$2" "/proc/$PID/exe" 2>/dev/null || echo 0)"
    echo "control '$2' (expected absent): $o"
    [ "$o" -eq 0 ] || echo "NOTE: the control string is still present -- it may be a legitimate old string"
  fi
else
  echo "note 3: no string given, so the freshness of the CHANGE is unverified. Pass one if the change"
  echo "        introduces a string; if it does not, freeze the source during the build instead."
fi

echo "health: $(curl -s -o /dev/null -w '%{http_code}' -m 5 http://127.0.0.1:8080/health)"
echo "sentinel: $(systemctl is-active ninfer-wedge-sentinel.service)"
