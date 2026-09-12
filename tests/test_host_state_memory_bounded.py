"""E2E: host memory must stay bounded under sustained distinct conversations.

Regression test for unbounded retention of state-only host-KV safety-net entries.

Observed failure (RTX 5090, 55 GB WSL VM, 2026-09-11): each distinct conversation
that demotes a checkpoint state image leaves a state-only entry holding a host
state image (~147 MiB). Entries accumulate at roughly two per conversation with no
plateau, while the accounted host KV stays well below its configured capacity,
because state images are ordinary heap allocations and not host-KV arena bytes.
RSS therefore grows linearly until the OOM killer fires.

The assertion is a bound on retained memory, not a wait for OOM, so the test fails
while the defect exists and passes once retention is correctly bounded.

Skipped unless the serve binary and a real artifact are available:

    NINFER_HOSTMEM_TEST=1 \
    NINFER_HOSTMEM_ARTIFACT=/path/to/model.ninfer \
    NINFER_HOSTMEM_SERVE=/path/to/build/apps/ninfer-serve \
    pytest tests/test_host_state_memory_bounded.py -s
"""
from __future__ import annotations

import json
import os
import signal
import subprocess
import time
import urllib.request

import pytest

ARTIFACT = os.environ.get("NINFER_HOSTMEM_ARTIFACT", "")
SERVE = os.environ.get("NINFER_HOSTMEM_SERVE", "")
ENABLED = os.environ.get("NINFER_HOSTMEM_TEST") == "1"
PORT = int(os.environ.get("NINFER_HOSTMEM_PORT", "18099"))

# Bounds. Retention must be a function of the configured capacity, not of how many
# conversations the server has seen.
MAX_RSS_GROWTH_GIB = 6.0
CONVERSATIONS = 30

pytestmark = pytest.mark.skipif(
    not (ENABLED and ARTIFACT and SERVE and os.path.exists(ARTIFACT) and os.path.exists(SERVE)),
    reason="set NINFER_HOSTMEM_TEST=1 and NINFER_HOSTMEM_ARTIFACT/NINFER_HOSTMEM_SERVE to run",
)


def _post(payload):
    req = urllib.request.Request(
        f"http://127.0.0.1:{PORT}/v1/chat/completions",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.loads(r.read().decode())


def _rss_gib(pid):
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) / 1048576.0
    return 0.0


def get_stats():
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/stats", timeout=10) as r:
        return json.loads(r.read().decode())


def _count(log_path, needle):
    """Occurrences of a diagnostic in the server log."""
    return open(log_path, errors="replace").read().count(needle)


def _host_occupancy(stats):
    """Occupancy and capacity of the shared host budget (GiB)."""
    memory = stats.get("memory", {}) or {}
    return (
        memory.get("host_kv_occupied_bytes", 0) / 1073741824.0,
        memory.get("host_kv_capacity_bytes", 0) / 1073741824.0,
    )


def _prompt(i, approx_tokens=900):
    return (f"Case {i}. Read the following notes and reply with a one-sentence summary.\n\n"
            + ("Consider the value table for case %d. " % i) * max(1, int(approx_tokens / 6)))


def test_host_memory_stays_bounded_across_conversations(tmp_path):
    log_path = tmp_path / "server.log"
    args = [
        SERVE, ARTIFACT,
        "--host", "127.0.0.1", "--port", str(PORT), "--model-id", "qwen3.8-27b",
        "--max-concurrency", "2",
        "--max-context", "32768",
        "--kv-capacity", "auto",
        "--default-max-tokens", "192",
        "--kv-dtype", "int8",
        "--spec", "mtp", "--draft-tokens", "5", "--lm-head-draft",
        "--host-kv-mib", "4096",
        "--host-state-slots", "8",
        "--max-private-continuations", "18",
        "--max-shared-prefixes", "6",
        "--seed", "42",
        "--temperature", "0.7", "--top-p", "0.8", "--top-k", "20",
    ]
    with open(log_path, "w") as log:
        server = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.time() + 300
            while time.time() < deadline:
                try:
                    urllib.request.urlopen(f"http://127.0.0.1:{PORT}/stats", timeout=5).read()
                    break
                except Exception:
                    if server.poll() is not None:
                        pytest.fail(f"server exited during startup: {log_path.read_text()[-2000:]}")
                    time.sleep(2)
            else:
                pytest.fail("server did not become healthy")

            baseline = _rss_gib(server.pid)
            for i in range(CONVERSATIONS):
                user = _prompt(i)
                reply = _post({
                    "model": "qwen3.8-27b",
                    "messages": [{"role": "user", "content": user}],
                    "max_tokens": 192, "seed": 42, "enable_thinking": True,
                })["choices"][0]["message"].get("content") or ""
                _post({
                    "model": "qwen3.8-27b",
                    "messages": [
                        {"role": "user", "content": user},
                        {"role": "assistant", "content": reply or "ok"},
                        {"role": "user", "content": f"Now restate case {i} in one line."},
                    ],
                    "max_tokens": 192, "seed": 42, "enable_thinking": True,
                })
                growth = _rss_gib(server.pid) - baseline
                assert growth < MAX_RSS_GROWTH_GIB, (
                    f"host RSS grew {growth:.2f} GiB after {i + 1} conversations "
                    f"(baseline {baseline:.2f} GiB); retained state images must be "
                    "bounded by the configured host budget"
                )
                occupied, capacity = _host_occupancy(get_stats())
                assert occupied <= capacity, (
                    f"reported host occupancy {occupied:.2f} GiB exceeds the shared host "
                    f"budget {capacity:.2f} GiB after {i + 1} conversations; host KV pages "
                    "and retained state images must share one budget"
                )
                # Atomicity: a cache unit is {attention KV + GDN state}. The spill path must
                # retain a complete unit or nothing, so a half unit must never reach the
                # retention boundary. A partial unit would also be a correctness hazard,
                # since prefix matching could select it and restore a wrong continuation.
                partial = _count(log_path, "[safety-net] REJECT-PARTIAL")
                assert partial == 0, (
                    f"{partial} partial units were offered for retention; KV and state are one "
                    "atomic unit and must be retained together or not at all"
                )
            # Retention must still work: a change that only refuses half units would
            # silently disable unit retention altogether, which is a regression.
            retained = _count(log_path, "[safety-spill] OK")
            assert retained > 0, (
                "no complete {KV + state} unit was retained during the run; the spill path "
                "must still store whole units, not merely refuse half ones"
            )
        finally:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=30)
            except Exception:
                server.kill()
