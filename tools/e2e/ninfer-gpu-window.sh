#!/usr/bin/env bash
# GPU window: stop prod, run one test binary with the whole GPU, restore prod.
#
# Real-engine tests need this because prod holds ~30 GB of the 32 GB of VRAM. The e2e swap cannot serve
# it (it runs *suites* against a server, and its teardown kills only its own binary path), and
# `ninfer-ab-test` compares two serve binaries -- neither runs a ctest target. So every in-process test
# of the engine (recovery, capture abort, constructive underflow, orphaned occupancy) was blocked on
# this script existing.
#
#   bash tools/e2e/ninfer-gpu-window.sh ninfer_qwen3_5_prefix_real_test [timeout-seconds]
#
# Safety rules, learned from three outages caused by my own command lines earlier tonight:
#   * the sentinel is stopped FIRST and re-armed LAST, so it cannot see prod stopped and start
#     "recovering" it mid-window;
#   * restore runs on EXIT *and* on INT/TERM/HUP, and is idempotent, so a signal or a killed caller
#     cannot leave prod down;
#   * the restore verifies by polling /health and prints what it found, rather than assuming;
#   * the test runs under `timeout`, so a hung test cannot hold the GPU for ever.
#
# It does NOT stop prod if the environment says this session is served by it (ANTHROPIC_BASE_URL
# pointing at 127.0.0.1:8080): that would freeze the operator's own session for the window.
set -uo pipefail

TARGET="${1:?usage: ninfer-gpu-window.sh <test-binary-name> [timeout-seconds]}"
TIMEOUT_S="${2:-900}"
BIN="./build/tests/${TARGET}"

case "${ANTHROPIC_BASE_URL:-}" in
  *127.0.0.1:8080*|*localhost:8080*)
    echo "[window] REFUSING: this session is served by prod (ANTHROPIC_BASE_URL=$ANTHROPIC_BASE_URL);" >&2
    echo "[window] stopping prod would freeze the operator's own session." >&2
    exit 2
    ;;
esac

if [ ! -x "$BIN" ]; then echo "[window] no such test binary: $BIN" >&2; exit 2; fi

restored=0
restore() {
  [ "$restored" = 1 ] && return 0
  restored=1
  echo "[window] restoring prod"
  sudo -n systemctl start ninfer.service 2>/dev/null || true
  for _ in $(seq 1 60); do
    curl -sf -m3 -o /dev/null http://127.0.0.1:8080/health && break
    sleep 3
  done
  sudo -n systemctl start ninfer-wedge-sentinel.service 2>/dev/null || true
  local health
  health=$(curl -s -o /dev/null -w '%{http_code}' -m5 http://127.0.0.1:8080/health 2>/dev/null || echo 000)
  echo "[window] prod restored: health=$health ninfer=$(systemctl is-active ninfer.service) sentinel=$(systemctl is-active ninfer-wedge-sentinel.service)"
}
trap 'restore; exit 130' INT TERM HUP
trap 'restore' EXIT

echo "[window] stopping sentinel, then prod"
sudo -n systemctl stop ninfer-wedge-sentinel.service 2>/dev/null || true
sudo -n systemctl stop ninfer.service 2>/dev/null || true
sleep 3

echo "[window] running $BIN (timeout ${TIMEOUT_S}s)"
timeout "$TIMEOUT_S" "$BIN"
rc=$?
echo "[window] test rc=$rc"
exit "$rc"
