#!/usr/bin/env python3
"""NInfer monitoring sidecar.

Tails the server's JSONL request log and stderr log, polls the /stats endpoint
(when present), nvidia-smi, and /proc, and serves:
  - a self-contained HTML dashboard at /
  - a JSON snapshot at /api/samples
  - a Prometheus-text feed at /metrics
  - a liveness probe at /healthz

Stdlib only. One sampler thread ticks every --interval seconds and appends a
sample to a bounded ring buffer; the HTTP server reads that state. The sidecar
is resilient to the inference server being down or restarting: /stats failures
degrade to a "stale" banner, and log rotation (rename or copytruncate) is
detected and handled by the tailers.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import threading
import time
import urllib.request
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# /stats liveness. The handler takes the engine's execution mutex, so under
# load a poll can be slow or fail transiently while the server is perfectly
# healthy. Use a generous poll timeout and only declare the server down when
# the last *successful* poll is this old - a few failed polls must not flap
# the "unreachable" banner.
POLL_TIMEOUT_S = 15
STALE_AFTER_MS = 60_000

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------


class Config:
    def __init__(self, args: argparse.Namespace) -> None:
        self.port = args.port
        self.bind = args.bind
        self.server_url = args.server_url.rstrip("/")
        self.jsonl = args.jsonl
        self.serve_log = args.serve_log
        self.interval = args.interval
        self.samples = args.samples
        self.events_cap = 500
        self.kv_log_cap = 200
        self.window_seconds = 3600
        # Serve-log (stderr) rotation: copytruncate safety net. The JSONL log is
        # rotated server-side; this only bounds the launcher-redirected stderr.
        self.serve_log_max_bytes = args.serve_log_max_mb * 1024 * 1024
        self.serve_log_keep = args.serve_log_keep
        self.pidfile = args.pidfile


# ---------------------------------------------------------------------------
# Log tailing (rotation-aware)
# ---------------------------------------------------------------------------


class Tail:
    """Incrementally tail a file, surviving rotation (rename) and truncation.

    On first open it seeks to the end (no backfill). Each read_lines() call
    returns the complete lines appended since the last call. If the file's
    inode changes (renamed away) or its size shrinks below our position
    (truncated), we reopen at the new end.
    """

    def __init__(self, path: str) -> None:
        self.path = path
        self.f = None
        self.ino = None
        self.pos = 0
        self.buf = ""
        self._open()

    def _open(self) -> None:
        try:
            self.f = open(self.path, "rb")
            st = os.fstat(self.f.fileno())
            self.ino = st.st_ino
            self.f.seek(0, 2)
            self.pos = self.f.tell()
        except (FileNotFoundError, OSError):
            self.f = None
            self.ino = None
            self.pos = 0
        self.buf = ""

    def read_lines(self) -> list[str]:
        lines: list[str] = []
        if self.f is None:
            self._open()
            if self.f is None:
                return lines
        try:
            st = os.stat(self.path)
        except (FileNotFoundError, OSError):
            self.f = None
            return lines
        if st.st_ino != self.ino or st.st_size < self.pos:
            # Rotated (new inode) or truncated: reopen at the new end.
            self._open()
            return lines
        self.f.seek(self.pos)
        data = self.f.read()
        if not data:
            return lines
        self.pos += len(data)
        text = self.buf + data.decode("utf-8", "replace")
        *complete, self.buf = text.split("\n")
        for ln in complete:
            if ln.strip():
                lines.append(ln)
        return lines


# ---------------------------------------------------------------------------
# Monitor: state + sampler
# ---------------------------------------------------------------------------


class Monitor:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.samples: deque[dict] = deque(maxlen=cfg.samples)
        self.events: deque[dict] = deque(maxlen=cfg.events_cap)
        self.kv_log: deque[dict] = deque(maxlen=cfg.kv_log_cap)
        self.server_info: dict = {
            "up": False,
            "instance_id": None,
            "model": None,
            "last_stats_unix_ms": None,
            "last_good_stats": None,
        }
        self.last_counters: dict | None = None
        self.last_rates: dict | None = None
        self.last_cpu_idle = None
        self.last_cpu_total = None
        self.jsonl_tail = Tail(cfg.jsonl)
        self.serve_tail = Tail(cfg.serve_log)
        # Per-tick host-KV event counts (park/evict/restore/miss), parsed from
        # the serve log; captured into each sample and charted.
        self._kv_tick = {"park": 0, "evict": 0, "restore": 0, "miss": 0}
        self.kv_totals = {"park": 0, "evict": 0, "restore": 0, "miss": 0}
        # Per-request host-KV reuse status (request_id -> "miss" | "hit (N tok)").
        # The "host KV miss" line carries its own request number (matching the
        # serve id), so misses are attributed directly by number. Restores carry
        # no number, so they are attributed to the oldest submitted request that
        # has no status yet (prefill is FIFO; a request that missed already has a
        # status and is skipped). Bounded to the most recent ids.
        self.host_kv_status: dict = {}
        self._submitted: list[int] = []
        self._lock = threading.Lock()

    def bootstrap_server_info(self) -> None:
        # The JSONL tail seeks to the end at startup (no backfill), so a
        # server_start record written before this sidecar started is missed.
        # Do a one-time scan of the current file for the most recent
        # server_start so the header shows the current instance + model.
        # Bounded: the request log rotates, so this stays cheap.
        try:
            with open(self.cfg.jsonl, "rb") as f:
                last = None
                for raw in f:
                    if b"server_start" in raw:
                        last = raw
                if last is None:
                    return
                rec = json.loads(last.decode("utf-8", "replace"))
                self.server_info["instance_id"] = rec.get("server_instance_id")
                self.server_info["model"] = rec.get("server", {}).get("public_model_id")
        except (OSError, json.JSONDecodeError):
            pass

    # -- JSONL dispatch -----------------------------------------------------

    def handle_jsonl_line(self, line: str) -> None:
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            return
        event = rec.get("event")
        ts = rec.get("timestamp_unix_ms")
        if event == "server_start":
            self.server_info["instance_id"] = rec.get("server_instance_id")
            self.server_info["model"] = rec.get("server", {}).get("public_model_id")
        elif event == "request_done":
            self._record_done(rec, ts)
        elif event == "request_rejected":
            self._record_event("rejected", rec, ts)
        elif event == "request_error":
            self._record_event("error", rec, ts)
        # "throughput" events are a cross-check only; /stats is authoritative.

    def _record_done(self, rec: dict, ts) -> None:
        req = rec.get("request", {})
        res = rec.get("result", {})
        spec = rec.get("speculative", {})
        timings = rec.get("timings_seconds", {})
        self.events.append(
            {
                "t": ts,
                "kind": "done",
                "id": req.get("request_id"),
                "protocol": req.get("protocol"),
                "prompt_tokens": res.get("prompt_tokens"),
                "completion_tokens": res.get("completion_tokens"),
                "ttft": timings.get("ttft"),
                "finish_reason": res.get("finish_reason"),
                "prefix_hit": res.get("prefix_cache_hit_tokens", 0),
                "prefix_path": res.get("prefix_reuse_path"),
                "mtp_accepted": spec.get("accepted_tokens", 0),
                "mtp_drafted": spec.get("drafted_tokens", 0),
                "mtp_rounds": spec.get("rounds", 0),
            }
        )

    def _record_event(self, kind: str, rec: dict, ts) -> None:
        err = rec.get("error", {})
        self.events.append(
            {
                "t": ts,
                "kind": kind,
                "id": rec.get("request", {}).get("request_id"),
                "protocol": rec.get("request", {}).get("protocol"),
                "message": err.get("message") or err.get("code") or kind,
            }
        )

    # -- serve-log dispatch -------------------------------------------------

    def handle_serve_line(self, line: str) -> None:
        # Track submitted request ids (FIFO) so host-KV restores can be
        # attributed. The "host KV miss" line carries its own request number
        # (matching the serve id), so misses are attributed directly by number
        # rather than to the "current" request.
        m = re.search(r"\[req (\d+)\][^→]*→ submitted", line)
        if m:
            self._submitted.append(int(m.group(1)))
            if len(self._submitted) > 100:
                self._submitted.pop(0)
        # Keep the host-KV lifecycle lines and per-request MTP summaries.
        if "ninfer-host-kv" in line or "speculative=mtp" in line:
            self.kv_log.append({"t": int(time.time() * 1000), "line": line.strip()})
        if "ninfer-host-kv" in line:
            if "host KV LRU evicting" in line:
                self._kv_tick["evict"] += 1
            elif "host KV miss" in line:
                self._kv_tick["miss"] += 1
                rm = re.search(r"req (\d+) host KV miss", line)
                if rm:
                    self._set_host_kv(int(rm.group(1)), "miss")
            elif "parked lane" in line:
                self._kv_tick["park"] += 1
            elif "restored lane" in line:
                self._kv_tick["restore"] += 1
                req = self._oldest_unresolved()
                if req is not None:
                    self._set_host_kv(req, "hit")

    def _oldest_unresolved(self) -> int | None:
        # Prefill is FIFO, so a restore resolves the oldest submitted request
        # that has no host-KV status yet. A request that missed already has a
        # status and is skipped.
        for req in self._submitted:
            if req not in self.host_kv_status:
                self._submitted.remove(req)
                return req
        return None

    def _set_host_kv(self, req_id: int, status: str) -> None:
        self.host_kv_status[req_id] = status
        # Request ids are monotonic; prune the oldest once past the cap.
        if len(self.host_kv_status) > 1000:
            for k in sorted(self.host_kv_status)[: len(self.host_kv_status) - 500]:
                del self.host_kv_status[k]

    # -- /stats poll --------------------------------------------------------

    def poll_stats(self) -> None:
        try:
            with urllib.request.urlopen(self.cfg.server_url + "/stats",
                                        timeout=POLL_TIMEOUT_S) as r:
                stats = json.loads(r.read().decode("utf-8", "replace"))
        except Exception:
            # A laggy server (busy event loop, /stats waiting on the
            # execution mutex) can fail a poll without being down. Keep the
            # last state; the grace check below decides liveness.
            stats = None
        if stats is not None:
            self.server_info["last_stats_unix_ms"] = stats.get("timestamp_unix_ms")
            self.server_info["last_good_stats"] = stats
            # Self-heal the model label from the live server: the JSONL
            # server_start record is written before a freshly-started sidecar
            # begins tailing, so /stats is the authoritative source for the
            # current model identity.
            model_id = stats.get("load", {}).get("model_id")
            if model_id:
                self.server_info["model"] = model_id
            counters = stats.get("counters", {})
            cur_decode = counters.get("committed_decode_tokens", 0)
            cur_prefill = counters.get("computed_prefill_tokens", 0)
            now = time.time()
            if self.last_counters is not None:
                dt = now - self.last_counters["t"]
                dd = cur_decode - self.last_counters["decode"]
                dp = cur_prefill - self.last_counters["prefill"]
                # Counter-reset guard: a server restart zeroes the counters, so
                # a negative delta means "skip this interval" rather than a
                # spike.
                if dt > 0 and dd >= 0 and dp >= 0:
                    self.last_rates = {"decode_tps": dd / dt, "prefill_tps": dp / dt}
                else:
                    self.last_rates = None
            self.last_counters = {"t": now, "decode": cur_decode, "prefill": cur_prefill}
        # Relaxed liveness: the server is "up" while the last successful poll
        # is within STALE_AFTER_MS. A laggy server that fails a few polls
        # stays up; only a sustained outage flips it down.
        last = self.server_info.get("last_stats_unix_ms")
        now_ms = int(time.time() * 1000)
        # /stats takes the engine's execution mutex and can lag for tens of
        # seconds under load (measured 3.7s at C=2 with a full queue). Fall
        # back to the serve-log heartbeat: fresh "throughput interval=" lines
        # prove the engine loop is alive even when /stats is starved.
        log_alive = False
        try:
            with open(self.cfg.serve_log, "rb") as f:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                f.seek(max(0, size - 4096))
                tail = f.read(4096)
            log_alive = b"throughput interval=" in tail
        except OSError:
            pass
        self.server_info["up"] = (
            (last is not None and now_ms - last <= STALE_AFTER_MS) or log_alive
        )

    # -- system pollers -----------------------------------------------------

    def read_gpu(self) -> dict | None:
        try:
            out = subprocess.run(
                [
                    "nvidia-smi",
                    "--query-gpu=memory.used,memory.total,utilization.gpu,power.draw,temperature.gpu",
                    "--format=csv,noheader,nounits",
                ],
                capture_output=True,
                text=True,
                timeout=5,
            )
            p = [x.strip() for x in out.stdout.split(",")]
            return {
                "mem_used_mb": float(p[0]),
                "mem_total_mb": float(p[1]),
                "util_pct": float(p[2]),
                "power_w": float(p[3]),
                "temp_c": float(p[4]),
            }
        except Exception:
            return None

    def read_cpu(self) -> float | None:
        try:
            with open("/proc/stat") as f:
                first = f.readline().split()
            vals = [int(x) for x in first[1:]]
            idle = vals[3] + (vals[4] if len(vals) > 4 else 0)  # idle + iowait
            total = sum(vals)
            pct = 0.0
            if self.last_cpu_total is not None and self.last_cpu_idle is not None:
                dt = total - self.last_cpu_total
                di = idle - self.last_cpu_idle
                if dt > 0:
                    pct = 100.0 * (1.0 - di / dt)
            self.last_cpu_idle = idle
            self.last_cpu_total = total
            return max(0.0, min(100.0, pct))
        except Exception:
            return None

    def read_ram(self) -> dict | None:
        try:
            info: dict[str, int] = {}
            with open("/proc/meminfo") as f:
                for line in f:
                    key, _, rest = line.partition(":")
                    parts = rest.strip().split()
                    if parts:
                        info[key] = int(parts[0])  # kB
            total = info.get("MemTotal", 0)
            avail = info.get("MemAvailable", 0)
            return {"used_mb": (total - avail) / 1024.0, "total_mb": total / 1024.0}
        except Exception:
            return None

    # -- serve-log rotation (copytruncate) ----------------------------------

    def maybe_rotate_serve_log(self) -> None:
        path = self.cfg.serve_log
        try:
            size = os.path.getsize(path)
        except OSError:
            return
        if size < self.cfg.serve_log_max_bytes:
            return
        for i in range(self.cfg.serve_log_keep - 1, 0, -1):
            src = f"{path}.{i}"
            dst = f"{path}.{i + 1}"
            if os.path.exists(src):
                if os.path.exists(dst):
                    os.remove(dst)
                os.rename(src, dst)
        try:
            with open(path, "rb") as src, open(path + ".1", "wb") as dst:
                dst.write(src.read())
            open(path, "w").close()  # truncate in place (server holds the fd)
            self.serve_tail._open()
        except OSError:
            pass

    # -- sampler tick -------------------------------------------------------

    def _rolling(self) -> dict:
        # Rolling MTP accept rate + TTFT percentiles over the most recent done
        # events, so the dashboard can plot them as time series (the windowed
        # aggregates in compute_aggregates() are a single current value).
        done = [e for e in list(self.events)[-60:] if e["kind"] == "done"]
        acc = sum(e.get("mtp_accepted", 0) for e in done)
        dr = sum(e.get("mtp_drafted", 0) for e in done)
        ttfts = sorted(e["ttft"] for e in done if e.get("ttft") is not None)
        return {
            "mtp_accept_rate": (acc / dr) if dr else None,
            "ttft_p50": self._pct(ttfts, 50),
            "ttft_p95": self._pct(ttfts, 95),
        }

    def tick(self) -> None:
        for line in self.jsonl_tail.read_lines():
            self.handle_jsonl_line(line)
        for line in self.serve_tail.read_lines():
            self.handle_serve_line(line)
        self.poll_stats()
        gpu = self.read_gpu()
        cpu = self.read_cpu()
        ram = self.read_ram()
        self.maybe_rotate_serve_log()
        # Snapshot the host-KV event counts for this tick, then reset.
        kv_events = dict(self._kv_tick)
        for key, count in kv_events.items():
            self.kv_totals[key] += count
        self._kv_tick = {"park": 0, "evict": 0, "restore": 0, "miss": 0}
        stats = self.server_info.get("last_good_stats")
        slim = None
        if stats:
            slim = {
                "scheduler": stats.get("scheduler"),
                "kv_cache": stats.get("kv_cache"),
                "counters": stats.get("counters"),
            }
        sample = {
            "t": int(time.time() * 1000),
            "server_up": self.server_info["up"],
            "stats": slim,
            "rates": self.last_rates,
            "kv_events": kv_events,
            "rolling": self._rolling(),
            "gpu": gpu,
            "cpu_pct": cpu,
            "ram": ram,
        }
        with self._lock:
            self.samples.append(sample)

    def start(self) -> None:
        t = threading.Thread(target=self._loop, name="sampler", daemon=True)
        t.start()

    def _loop(self) -> None:
        while True:
            try:
                self.tick()
            except Exception:
                pass  # a bad tick must never kill the sampler
            time.sleep(self.cfg.interval)

    # -- aggregates ---------------------------------------------------------

    @staticmethod
    def _pct(vals: list[float], p: float) -> float | None:
        if not vals:
            return None
        k = (len(vals) - 1) * p / 100.0
        lo = int(k)
        hi = min(lo + 1, len(vals) - 1)
        return vals[lo] + (vals[hi] - vals[lo]) * (k - lo)

    def compute_aggregates(self) -> dict:
        now_ms = int(time.time() * 1000)
        window_ms = self.cfg.window_seconds * 1000
        evs = [e for e in self.events if now_ms - (e.get("t") or 0) <= window_ms]
        done = [e for e in evs if e["kind"] == "done"]
        errors = [e for e in evs if e["kind"] == "error"]
        rejected = [e for e in evs if e["kind"] == "rejected"]
        ttfts = sorted(e["ttft"] for e in done if e.get("ttft") is not None)
        mtp_acc = sum(e.get("mtp_accepted", 0) for e in done)
        mtp_dr = sum(e.get("mtp_drafted", 0) for e in done)
        mtp_rd = sum(e.get("mtp_rounds", 0) for e in done)
        prefix_hits = sum(1 for e in done if (e.get("prefix_hit") or 0) > 0)
        by_path: dict[str, int] = {}
        for e in done:
            p = e.get("prefix_path")
            if p:
                by_path[p] = by_path.get(p, 0) + 1
        smp = [s for s in self.samples if now_ms - s["t"] <= window_ms and s.get("rates")]
        dec = [s["rates"]["decode_tps"] for s in smp]
        pre = [s["rates"]["prefill_tps"] for s in smp]
        return {
            "window_seconds": self.cfg.window_seconds,
            "requests": {"done": len(done), "errors": len(errors), "rejected": len(rejected)},
            "ttft": {
                "p50": self._pct(ttfts, 50),
                "p95": self._pct(ttfts, 95),
                "max": ttfts[-1] if ttfts else None,
            },
            "mtp": {
                "accept_rate": (mtp_acc / mtp_dr) if mtp_dr else None,
                "tokens_per_round": (mtp_acc / mtp_rd) if mtp_rd else None,
                "requests_with_mtp": sum(1 for e in done if (e.get("mtp_drafted") or 0) > 0),
            },
            "prefix": {
                "hit_rate": (prefix_hits / len(done)) if done else None,
                "by_path": by_path,
            },
            "throughput": {
                "decode_tps_avg": (sum(dec) / len(dec)) if dec else None,
                "prefill_tps_avg": (sum(pre) / len(pre)) if pre else None,
            },
        }

    # -- HTTP payloads ------------------------------------------------------

    def api_samples(self) -> dict:
        with self._lock:
            samples = list(self.samples)
            events = list(self.events)
            kv_log = list(self.kv_log)
            host_kv = dict(self.host_kv_status)
        # Attach host-KV reuse status to each request event by id.
        for e in events:
            e["host_kv"] = host_kv.get(e.get("id"))
        return {
            "generated_unix_ms": int(time.time() * 1000),
            "server": {
                "up": self.server_info["up"],
                "last_stats_unix_ms": self.server_info.get("last_stats_unix_ms"),
                "instance_id": self.server_info.get("instance_id"),
                "model": self.server_info.get("model"),
            },
            "latest": samples[-1] if samples else None,
            "samples": samples,
            "events": events,
            "kv_log": kv_log,
            "aggregates": self.compute_aggregates(),
        }

    def metrics_text(self) -> str:
        latest = None
        with self._lock:
            if self.samples:
                latest = self.samples[-1]
        agg = self.compute_aggregates()
        out: list[str] = []

        def gauge(name: str, help_: str, value) -> None:
            out.append(f"# HELP {name} {help_}")
            out.append(f"# TYPE {name} gauge")
            out.append(f"{name} {value if value is not None else 'NaN'}")

        def counter(name: str, help_: str, value) -> None:
            out.append(f"# HELP {name} {help_}")
            out.append(f"# TYPE {name} counter")
            out.append(f"{name} {value if value is not None else 0}")

        stats = (latest or {}).get("stats") or {}
        sched = stats.get("scheduler") or {}
        kv = stats.get("kv_cache") or {}
        counters = stats.get("counters") or {}
        rates = (latest or {}).get("rates") or {}
        gpu = (latest or {}).get("gpu") or {}
        ram = (latest or {}).get("ram") or {}

        gauge("ninfer_server_up", "Whether the inference server /stats is reachable",
              1 if self.server_info["up"] else 0)
        gauge("ninfer_scheduler_running", "Scheduler running requests", sched.get("running", 0))
        gauge("ninfer_scheduler_prefilling", "Scheduler prefilling requests", sched.get("prefilling", 0))
        gauge("ninfer_scheduler_decode_ready", "Scheduler decode-ready requests",
              sched.get("decode_ready", 0))
        gauge("ninfer_scheduler_waiting", "Scheduler waiting (queued) requests",
              sched.get("waiting", 0))
        gauge("ninfer_decode_tokens_per_second", "Decode throughput (tok/s)",
              rates.get("decode_tps"))
        gauge("ninfer_prefill_tokens_per_second", "Prefill throughput (tok/s)",
              rates.get("prefill_tps"))
        text = kv.get("text") or {}
        mtp = kv.get("mtp") or {}
        host = kv.get("host") or {}
        gauge("ninfer_kv_text_mapped_pages", "Text KV pool mapped pages", text.get("mapped_pages", 0))
        gauge("ninfer_kv_text_entitled_pages", "Text KV pool entitled pages",
              text.get("entitled_pages", 0))
        gauge("ninfer_kv_text_free_pages", "Text KV pool free pages", text.get("free_pages", 0))
        gauge("ninfer_kv_mtp_mapped_pages", "MTP KV pool mapped pages", mtp.get("mapped_pages", 0))
        gauge("ninfer_kv_mtp_free_pages", "MTP KV pool free pages", mtp.get("free_pages", 0))
        host_budget = host.get("budget_bytes", 0)
        host_used = host.get("used_bytes", 0)
        gauge("ninfer_kv_host_entries", "Host KV parked entries", host.get("entries", 0))
        gauge("ninfer_kv_host_budget_bytes", "Host KV pinned budget (bytes)", host_budget)
        gauge("ninfer_kv_host_used_bytes", "Host KV bytes used by parked entries", host_used)
        gauge("ninfer_kv_host_largest_free_range",
              "Largest contiguous free range in the host KV budget (bytes)",
              host.get("largest_free_range", 0))
        gauge("ninfer_kv_host_used_pct", "Host KV budget used (percent)",
              host_used / host_budget * 100 if host_budget else 0)
        for kind in ("park", "evict", "restore", "miss"):
            counter(f"ninfer_kv_host_{kind}_total",
                    f"Cumulative host-KV {kind} events (since sidecar start)",
                    self.kv_totals.get(kind, 0))
        gauge("ninfer_mtp_accept_rate", "MTP draft acceptance rate (window)",
              agg["mtp"]["accept_rate"])
        gauge("ninfer_prefix_hit_rate", "Prefix-cache hit rate (window)",
              agg["prefix"]["hit_rate"])
        gauge("ninfer_ttft_p50_seconds", "Median time-to-first-token (window)",
              agg["ttft"]["p50"])
        gauge("ninfer_gpu_memory_used_bytes", "GPU memory used",
              (gpu.get("mem_used_mb") or 0) * 1024 * 1024)
        gauge("ninfer_gpu_utilization_percent", "GPU utilization", gpu.get("util_pct"))
        gauge("ninfer_cpu_percent", "Host CPU utilization", (latest or {}).get("cpu_pct"))
        gauge("ninfer_ram_used_bytes", "Host RAM used",
              (ram.get("used_mb") or 0) * 1024 * 1024)
        counter("ninfer_committed_decode_tokens_total", "Cumulative committed decode tokens",
                counters.get("committed_decode_tokens"))
        counter("ninfer_computed_prefill_tokens_total", "Cumulative computed prefill tokens",
                counters.get("computed_prefill_tokens"))
        counter("ninfer_decode_rounds_total", "Cumulative decode rounds",
                counters.get("decode_rounds"))
        out.append("")
        return "\n".join(out)


# ---------------------------------------------------------------------------
# HTML dashboard (self-contained, no CDN)
# ---------------------------------------------------------------------------

HTML = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>NInfer Monitor</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCA2NCA2NCIgd2lkdGg9IjY0IiBoZWlnaHQ9IjY0Ij4KICA8ZyB0cmFuc2Zvcm09InRyYW5zbGF0ZSg2NCwwKSBzY2FsZSgtMSwxKSI+CiAgICA8cmVjdCB3aWR0aD0iNjQiIGhlaWdodD0iNjQiIHJ4PSIxNCIgZmlsbD0iIzBGMTcyQSIvPgogICAgPCEtLSBMZWZ0IHBvc3QgLS0+CiAgICA8cmVjdCB4PSIxMiIgeT0iMTQiIHdpZHRoPSI4IiBoZWlnaHQ9IjM2IiByeD0iNCIgZmlsbD0iIzM4QkRGOCIvPgogICAgPCEtLSBSaWdodCBwb3N0IC0tPgogICAgPHJlY3QgeD0iNDQiIHk9IjE0IiB3aWR0aD0iOCIgaGVpZ2h0PSIzNiIgcng9IjQiIGZpbGw9IiMzOEJERjgiLz4KICAgIDwhLS0gQ2VudHJhbCBQdWxzZSAvIERpYWdvbmFsIC0tPgogICAgPHBvbHlsaW5lIHBvaW50cz0iMTYsMzIgMjYsNDQgMzgsMjAgNDgsMzIiIGZpbGw9Im5vbmUiIHN0cm9rZT0iIzIyQzU1RSIgc3Ryb2tlLXdpZHRoPSI3IiBzdHJva2UtbGluZWNhcD0icm91bmQiIHN0cm9rZS1saW5lam9pbj0icm91bmQiLz4KICA8L2c+Cjwvc3ZnPg==">
<style>
:root{
  --bg:#0d1117; --panel:#161b22; --panel2:#1c2333; --border:#2b3444;
  --text:#e6edf3; --muted:#8b949e; --accent:#58a6ff; --good:#3fb950;
  --warn:#d29922; --bad:#f85149; --grid:#21262d;
  --c1:#58a6ff; --c2:#3fb950; --c3:#d29922; --c4:#bc8cff; --c5:#f778ba;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);
  font:14px/1.5 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;align-items:center;gap:14px;padding:14px 20px;
  border-bottom:1px solid var(--border);background:var(--panel);
  position:sticky;top:0;z-index:5}
header h1{font-size:16px;margin:0;font-weight:600}
.pill{padding:2px 10px;border-radius:20px;font-size:12px;font-weight:600}
.pill.up{background:#12291c;color:var(--good);border:1px solid #238639}
.pill.stale{background:#2a230c;color:var(--warn);border:1px solid #9e6a03}
.pill.down{background:#2d1215;color:var(--bad);border:1px solid #b62324}
.meta{color:var(--muted);font-size:12px;margin-left:auto}
.banner{display:none;background:#2d1215;color:var(--bad);padding:8px 20px;
  font-size:13px;border-bottom:1px solid #b62324}
main{padding:16px 20px 40px;max-width:1400px;margin:0 auto}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px}
.tile{background:var(--panel);border:1px solid var(--border);border-radius:10px;
  padding:12px 14px}
.tile .k{color:var(--muted);font-size:11px;text-transform:uppercase;letter-spacing:.04em}
.tile .v{font-size:22px;font-weight:650;margin-top:4px;font-variant-numeric:tabular-nums}
.tile .s{color:var(--muted);font-size:11px;margin-top:2px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(360px,1fr));gap:14px;margin-top:14px}
.card{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:14px}
.card h2{font-size:13px;margin:0 0 8px;color:var(--muted);font-weight:600;
  text-transform:uppercase;letter-spacing:.04em}
canvas{width:100%;height:180px;display:block}
table{width:100%;border-collapse:collapse;font-size:12.5px}
th,td{text-align:left;padding:5px 8px;border-bottom:1px solid var(--grid);
  font-variant-numeric:tabular-nums;white-space:nowrap}
th{color:var(--muted);font-weight:600;font-size:11px;text-transform:uppercase}
td.msg{white-space:normal;max-width:420px;color:var(--muted)}
.kvlog{font:12px/1.6 ui-monospace,SFMono-Regular,Menlo,monospace;color:var(--muted);
  max-height:260px;overflow:auto}
.kvlog div{padding:2px 0;border-bottom:1px solid var(--grid)}
.legend{display:flex;gap:14px;flex-wrap:wrap;font-size:11px;color:var(--muted);margin-top:6px}
.legend span{display:inline-flex;align-items:center;gap:5px}
.dot{width:9px;height:9px;border-radius:2px;display:inline-block}
.bar{display:flex;height:14px;border-radius:4px;overflow:hidden;margin:6px 0 2px;
  background:var(--panel2)}
.bar div{height:100%}
.barlabel{display:flex;justify-content:space-between;font-size:11px;color:var(--muted)}
.empty{color:var(--muted);font-size:12px;padding:20px;text-align:center}
</style>
</head>
<body>
<header>
  <h1>NInfer Monitor</h1>
  <span id="status" class="pill down">…</span>
  <span class="meta" id="meta"></span>
</header>
<div class="banner" id="banner">Inference server unreachable — showing last good data.</div>
<main>
  <div class="tiles" id="tiles"></div>
  <div class="grid">
    <div class="card"><h2>Scheduler occupancy</h2>
      <canvas id="c-sched"></canvas><div class="legend" id="l-sched"></div></div>
    <div class="card"><h2>Concurrent queries</h2>
      <canvas id="c-conc"></canvas><div class="legend" id="l-conc"></div></div>
    <div class="card"><h2>Decode throughput (tok/s)</h2>
      <canvas id="c-decode"></canvas><div class="legend" id="l-decode"></div></div>
    <div class="card"><h2>Prefill throughput (tok/s)</h2>
      <canvas id="c-prefill"></canvas><div class="legend" id="l-prefill"></div></div>
    <div class="card"><h2>KV cache occupancy</h2>
      <canvas id="c-kv"></canvas><div class="legend" id="l-kv"></div></div>
    <div class="card"><h2>MTP acceptance</h2>
      <canvas id="c-mtp"></canvas><div class="legend" id="l-mtp"></div></div>
    <div class="card"><h2>TTFT (s)</h2>
      <canvas id="c-ttft"></canvas><div class="legend" id="l-ttft"></div></div>
    <div class="card"><h2>GPU</h2>
      <canvas id="c-gpu"></canvas><div class="legend" id="l-gpu"></div></div>
    <div class="card"><h2>CPU &amp; RAM</h2>
      <canvas id="c-cpura"></canvas><div class="legend" id="l-cpura"></div></div>
  </div>
  <div class="grid">
    <div class="card"><h2>Device KV (current)</h2><div id="kvbar"></div></div>
    <div class="card"><h2>Host KV (current)</h2><div id="hostbar"></div></div>
    <div class="card"><h2>Host-KV events (per interval)</h2>
      <canvas id="c-kvev"></canvas><div class="legend" id="l-kvev"></div></div>
  </div>
  <div class="grid">
    <div class="card"><h2>Recent requests</h2>
      <div style="overflow:auto;max-height:260px"><table id="reqs"></table></div></div>
    <div class="card"><h2>Host-KV activity</h2><div class="kvlog" id="kvlog"></div></div>
  </div>
</main>
<script>
const $=id=>document.getElementById(id);
const C={sched:['#58a6ff','#3fb950','#d29922','#bc8cff'],
         concurrent:['#58a6ff'],
         decode:['#58a6ff'],prefill:['#3fb950'],
         kv:['#58a6ff','#3fb950','#d29922'],
         kvev:['#f85149','#d29922','#58a6ff','#3fb950'],
         mtp:['#3fb950','#bc8cff'],ttft:['#58a6ff','#d29922'],
         gpu:['#58a6ff','#3fb950'],cpura:['#58a6ff','#d29922']};
function decimate(pts,max){if(pts.length<=max)return pts;
  const step=Math.ceil(pts.length/max),out=[];
  for(let i=0;i<pts.length;i+=step)out.push(pts[i]);
  if(out[out.length-1]!==pts[pts.length-1])out.push(pts[pts.length-1]);
  return out;}
function lineChart(cv,series,opt={}){
  const dpr=window.devicePixelRatio||1;
  const w=cv.clientWidth,h=cv.clientHeight;
  cv.width=w*dpr;cv.height=h*dpr;
  const ctx=cv.getContext('2d');ctx.scale(dpr,dpr);
  ctx.clearRect(0,0,w,h);
  const pad={l:42,r:10,t:8,b:18};
  const all=[];series.forEach(s=>s.points.forEach(p=>all.push(p)));
  if(!all.length){ctx.fillStyle='#8b949e';ctx.font='12px sans-serif';
    ctx.textAlign='center';ctx.fillText('no data',w/2,h/2);return;}
  let tmin=Math.min(...all.map(p=>p[0])),tmax=Math.max(...all.map(p=>p[0]));
  if(tmax===tmin)tmax=tmin+1;
  let vmin=0,vmax=0;
  series.forEach(s=>s.points.forEach(p=>{vmin=Math.min(vmin,p[1]);vmax=Math.max(vmax,p[1]);}));
  if(opt.vmax!=null)vmax=opt.vmax;
  if(vmin<0)vmin=0;
  if(vmax===vmin)vmax=vmin+1;
  const X=t=>pad.l+(t-tmin)/(tmax-tmin)*(w-pad.l-pad.r);
  const Y=v=>h-pad.b-(v-vmin)/(vmax-vmin)*(h-pad.t-pad.b);
  ctx.strokeStyle='#21262d';ctx.fillStyle='#8b949e';ctx.font='10px sans-serif';
  ctx.lineWidth=1;ctx.textAlign='right';
  for(let g=0;g<=4;g++){const v=vmin+(vmax-vmin)*g/4,y=Y(v);
    ctx.beginPath();ctx.moveTo(pad.l,y);ctx.lineTo(w-pad.r,y);ctx.stroke();
    ctx.fillText(fmtNum(v),pad.l-5,y+3);}
  ctx.textAlign='center';
  for(let g=0;g<=3;g++){const t=tmin+(tmax-tmin)*g/3;
    ctx.fillText(fmtT(t),X(t),h-4);}
  series.forEach((s,si)=>{
    const pts=decimate(s.points,720);
    ctx.strokeStyle=s.color;ctx.lineWidth=1.6;ctx.beginPath();
    pts.forEach((p,i)=>{const x=X(p[0]),y=Y(p[1]);
      if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);});
    ctx.stroke();});
}
function fmtNum(v){if(v==null)return'–';
  if(v>=1000)return(v/1000).toFixed(1)+'k';
  if(v>=100)return v.toFixed(0);
  if(v>=10)return v.toFixed(1);
  return v.toFixed(2);}
function fmtT(ms){const d=new Date(ms);
  return d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',second:'2-digit'});}
function fmtAgo(ms){const s=Math.max(0,Math.round((Date.now()-ms)/1000));
  if(s<60)return s+'s ago';if(s<3600)return Math.round(s/60)+'m ago';
  return Math.round(s/3600)+'h ago';}
function seriesFrom(samples,fn,color,label){
  const pts=[];samples.forEach(s=>{const v=fn(s);if(v!=null)pts.push([s.t,v]);});
  return{label,color,points:pts};}
function setLegend(id,series){$(id).innerHTML=series.map(s=>
  '<span><i class="dot" style="background:'+s.color+'"></i>'+s.label+'</span>').join('');}
function draw(cvId,legendId,series,opt){lineChart($(cvId),series,opt||{});
  setLegend(legendId,series);}
function tile(k,v,s){return '<div class="tile"><div class="k">'+k+'</div>'+
  '<div class="v">'+(v==null?'–':v)+'</div>'+(s?'<div class="s">'+s+'</div>':'')+'</div>';}
function bar(segments){ // segments: [{pct,color,label}]
  let h='<div class="bar">';
  segments.forEach(sg=>{h+='<div style="width:'+sg.pct+'%;background:'+sg.color+'"></div>';});
  h+='</div><div class="barlabel">';
  h+=segments.map(sg=>sg.label+' '+(sg.pct==null?'–':sg.pct.toFixed(0)+'%')).join(' &nbsp; ');
  h+='</div>';return h;}
function render(d){
  const srv=d.server,latest=d.latest||{},agg=d.aggregates;
  // status pill
  const st=$('status');
  if(srv.up){st.className='pill up';st.textContent='LIVE';}
  else if(srv.last_stats_unix_ms){st.className='pill stale';st.textContent='STALE';}
  else{st.className='pill down';st.textContent='DOWN';}
  $('banner').style.display=srv.up?'none':'block';
  $('meta').textContent=(srv.model||'?')+' · '+(srv.instance_id||'no instance')+
    ' · updated '+fmtAgo(d.generated_unix_ms);
  // tiles
  const rates=latest.rates||{};
  const gpu=latest.gpu||{};const ram=latest.ram||{};
  const tiles=[
    tile('Decode',fmt0(rates.decode_tps),'tok/s'),
    tile('Prefill',fmt0(rates.prefill_tps),'tok/s'),
    tile('MTP accept',pct(agg.mtp.accept_rate),'tok/round '+(agg.mtp.tokens_per_round?agg.mtp.tokens_per_round.toFixed(2):'–')),
    tile('Prefix hit',pct(agg.prefix.hit_rate),Object.keys(agg.prefix.by_path||{}).length+' paths'),
    tile('TTFT p50',agg.ttft.p50?agg.ttft.p50.toFixed(2)+'s':'–','p95 '+(agg.ttft.p95?agg.ttft.p95.toFixed(1)+'s':'–')),
    tile('GPU util',gpu.util_pct!=null?gpu.util_pct.toFixed(0)+'%':'–',
      gpu.mem_used_mb?(gpu.mem_used_mb/1024).toFixed(1)+' / '+(gpu.mem_total_mb/1024).toFixed(0)+' GB':''),
    tile('CPU',latest.cpu_pct!=null?latest.cpu_pct.toFixed(0)+'%':'–',''),
    tile('RAM',ram.used_mb?(ram.used_mb/1024).toFixed(1)+' GB':'–',
      ram.total_mb?'of '+(ram.total_mb/1024).toFixed(0)+' GB':''),
  ];
  $('tiles').innerHTML=tiles.join('');
  // charts
  const S=d.samples;
  draw('c-sched','l-sched',[
    seriesFrom(S,s=>s.stats?.scheduler?.running,C.sched[0],'running'),
    seriesFrom(S,s=>s.stats?.scheduler?.prefilling,C.sched[1],'prefilling'),
    seriesFrom(S,s=>s.stats?.scheduler?.decode_ready,C.sched[2],'decode_ready'),
    seriesFrom(S,s=>s.stats?.scheduler?.waiting,C.sched[3],'waiting'),
  ],{vmax:8});
  draw('c-conc','l-conc',[
    seriesFrom(S,s=>{const sc=s.stats?.scheduler;
      return sc?(sc.running||0)+(sc.prefilling||0):null;},C.concurrent[0],'in flight'),
  ]);
  draw('c-decode','l-decode',[
    seriesFrom(S,s=>s.rates?.decode_tps,C.decode[0],'decode tok/s'),
  ]);
  draw('c-prefill','l-prefill',[
    seriesFrom(S,s=>s.rates?.prefill_tps,C.prefill[0],'prefill tok/s'),
  ]);
  draw('c-kvev','l-kvev',[
    seriesFrom(S,s=>s.kv_events?.evict,C.kvev[0],'evict'),
    seriesFrom(S,s=>s.kv_events?.miss,C.kvev[1],'miss'),
    seriesFrom(S,s=>s.kv_events?.park,C.kvev[2],'park'),
    seriesFrom(S,s=>s.kv_events?.restore,C.kvev[3],'restore'),
  ]);
  draw('c-kv','l-kv',[
    seriesFrom(S,s=>kvPct(s,'text','mapped'),C.kv[0],'text mapped %'),
    seriesFrom(S,s=>kvPct(s,'mtp','mapped'),C.kv[1],'mtp mapped %'),
    seriesFrom(S,s=>{const h=s.stats?.kv_cache?.host;return h&&h.budget_bytes?h.used_bytes/h.budget_bytes*100:null;},C.kv[2],'host used %'),
  ]);
  draw('c-mtp','l-mtp',[
    seriesFrom(S,s=>s.rolling?.mtp_accept_rate!=null?s.rolling.mtp_accept_rate*100:null,C.mtp[0],'accept %'),
  ],{vmax:100});
  draw('c-ttft','l-ttft',[
    seriesFrom(S,s=>s.rolling?.ttft_p50,C.ttft[0],'p50'),
    seriesFrom(S,s=>s.rolling?.ttft_p95,C.ttft[1],'p95'),
  ]);
  draw('c-gpu','l-gpu',[
    seriesFrom(S,s=>s.gpu?.util_pct,C.gpu[0],'GPU util %'),
    seriesFrom(S,s=>s.gpu?.mem_total_mb?s.gpu.mem_used_mb/s.gpu.mem_total_mb*100:null,C.gpu[1],'GPU mem %'),
  ],{vmax:100});
  draw('c-cpura','l-cpura',[
    seriesFrom(S,s=>s.cpu_pct,C.cpura[0],'CPU %'),
    seriesFrom(S,s=>s.ram?.total_mb?s.ram.used_mb/s.ram.total_mb*100:null,C.cpura[1],'RAM %'),
  ],{vmax:100});
  // KV bars
  renderKvBars(latest);
  // recent requests
  renderReqs(d.events);
  // host-kv log
  $('kvlog').innerHTML=d.kv_log.slice(-20).reverse().map(e=>
    '<div>'+esc(e.line)+'</div>').join('')||'<div class="empty">no host-KV events yet</div>';
}
function fmt0(v){return v==null?'–':(v>=100?v.toFixed(0):v.toFixed(1));}
function pct(v){return v==null?'–':(v*100).toFixed(0)+'%';}
function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
function timeCell(ms){return ms?'<td title="'+fmtAgo(ms)+'">'+fmtT(ms)+'</td>':'<td>–</td>';}
function renderKvBars(latest){
  const kv=latest.stats?.kv_cache||{};
  const text=kv.text||{},mtp=kv.mtp||{},host=kv.host||{};
  const tpg=text.page_groups||0;
  const mapped=text.mapped_pages||0,entitled=text.entitled_pages||0,free=text.free_pages||0;
  const mp=tpg?mapped/tpg*100:0,ep=tpg?entitled/tpg*100:0,fp=tpg?free/tpg*100:0;
  $('kvbar').innerHTML=
    '<div style="font-size:12px;color:var(--muted);margin-bottom:4px">text pool ('+tpg+' groups)</div>'+
    bar([{pct:mp,color:C.kv[0],label:'mapped'},{pct:ep-mapped>0?ep-mp:0,color:C.kv[2],label:'reserved'},{pct:fp,color:'#30363d',label:'free'}])+
    '<div style="font-size:12px;color:var(--muted);margin:10px 0 4px">mtp pool ('+(mtp.page_groups||0)+' groups)</div>'+
    bar([{pct:mtp.page_groups?(mtp.mapped_pages||0)/mtp.page_groups*100:0,color:C.kv[1],label:'mapped'},{pct:100-(mtp.page_groups?(mtp.mapped_pages||0)/mtp.page_groups*100:0),color:'#30363d',label:'free'}]);
  const budget=host.budget_bytes||0,used=host.used_bytes||0;
  const usedPct=budget?used/budget*100:0;
  const gb=v=>(v/1073741824).toFixed(1)+' GB';
  $('hostbar').innerHTML=host.enabled?
    '<div style="font-size:12px;color:var(--muted);margin-bottom:4px">'+(host.entries||0)+' parked · '+gb(used)+' / '+gb(budget)+' ('+usedPct.toFixed(0)+'%)</div>'+
    bar([{pct:usedPct,color:C.kv[2],label:'used'},{pct:100-usedPct,color:'#30363d',label:'free'}])
    :'<div class="empty">host KV cache disabled</div>';
}
function renderReqs(events){
  const rows=events.slice(-20).reverse();
  let h='<tr><th>time</th><th>id</th><th>proto</th><th>kind</th><th>prompt</th><th>gen</th><th>ttft</th><th>prefix</th><th>host kv</th><th>finish</th><th></th></tr>';
  rows.forEach(e=>{
    if(e.kind==='done'){
      const hk=e.host_kv;
      // A host-KV "miss" is only a problem when nothing was reused. If the
      // prefix was served from the GPU cache (prefix_hit>0), the host tier
      // never had a parked copy, so the miss is expected — green, not red.
      const prefixHit=(e.prefix_hit||0)>0;
      const green=hk&&(hk.startsWith('hit')||prefixHit);
      const hkCell=hk
        ?'<span style="color:'+(green?'#3fb950':'#f85149')+'"'+(green&&!hk.startsWith('hit')?' title="prefix served from VRAM; host miss expected"':'')+'>'+esc(hk)+'</span>'
        :'<span style="color:#8b949e">–</span>';
      h+='<tr>'+timeCell(e.t)+'<td>'+e.id+'</td><td>'+esc(e.protocol||'')+'</td><td>done</td>'+
        '<td>'+e.prompt_tokens+'</td><td>'+e.completion_tokens+'</td>'+
        '<td>'+(e.ttft!=null?e.ttft.toFixed(1)+'s':'')+'</td>'+
        '<td>'+(e.prefix_hit||0)+'</td>'+
        '<td>'+hkCell+'</td>'+
        '<td>'+esc(e.finish_reason||'')+'</td><td class="msg"></td></tr>';
    }else{
      h+='<tr>'+timeCell(e.t)+'<td>'+(e.id??'')+'</td><td>'+esc(e.protocol||'')+'</td><td>'+e.kind+'</td>'+
        '<td colspan="6"></td><td class="msg">'+esc(e.message||'')+'</td></tr>';
    }
  });
  $('reqs').innerHTML=h;
}
// per-sample derived series: KV occupancy comes from the embedded /stats
// snapshot; MTP/TTFT come from the rolling values computed in the sampler.
function kvPct(s,pool,key){const kv=s.stats?.kv_cache?.[pool];
  if(!kv||!kv.page_groups)return null;
  return (kv[key]||0)/kv.page_groups*100;}
async function refresh(){
  try{
    const r=await fetch('/api/samples');
    const d=await r.json();
    render(d);
  }catch(e){console.error(e);}
}
refresh();setInterval(refresh,5000);
</script>
</body>
</html>
"""


# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    server_version = "ninfer-monitor/1.0"

    # Quiet: suppress per-request access logging (the dashboard polls
    # every 5s, so each poll would otherwise land in the sidecar log).
    # The parameters exist only to match the base signature; deleting
    # them marks them as intentionally unused.
    def log_message(self, format: str, *args) -> None:
        del format, args

    def do_GET(self) -> None:  # noqa: N802
        monitor: Monitor = self.server.monitor  # type: ignore[attr-defined]
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            self._send(HTML.encode("utf-8"), "text/html; charset=utf-8")
        elif path == "/api/samples":
            self._send(json.dumps(monitor.api_samples()).encode("utf-8"), "application/json")
        elif path == "/metrics":
            self._send(monitor.metrics_text().encode("utf-8"),
                       "text/plain; version=0.0.4; charset=utf-8")
        elif path == "/healthz":
            self._send(b"ok", "text/plain")
        else:
            self._send(b"not found", "text/plain", 404)

    def _send(self, body: bytes, content_type: str, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def parse_args() -> argparse.Namespace:
    here = os.path.dirname(os.path.abspath(__file__))
    p = argparse.ArgumentParser(description="NInfer monitoring sidecar")
    p.add_argument("--port", type=int, default=8090)
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--server-url", default="http://127.0.0.1:8080")
    p.add_argument("--jsonl", default=os.path.expanduser("~/ninfer-requests.jsonl"))
    p.add_argument("--serve-log", default=os.path.expanduser("~/ninfer-serve.log"))
    p.add_argument("--interval", type=float, default=5.0)
    p.add_argument("--samples", type=int, default=4320,
                   help="ring-buffer length (6h at 5s)")
    p.add_argument("--serve-log-max-mb", type=int, default=200)
    p.add_argument("--serve-log-keep", type=int, default=4)
    p.add_argument("--pidfile", default=os.path.join(here, "monitor.pid"))
    return p.parse_args()


def main() -> None:
    args = parse_args()
    cfg = Config(args)
    monitor = Monitor(cfg)
    monitor.bootstrap_server_info()
    monitor.start()
    if cfg.pidfile:
        with open(cfg.pidfile, "w") as f:
            f.write(str(os.getpid()))
    httpd = ThreadingHTTPServer((cfg.bind, cfg.port), Handler)
    httpd.monitor = monitor  # type: ignore[attr-defined]
    print(f"ninfer-monitor listening on {cfg.bind}:{cfg.port} "
          f"(server={cfg.server_url}, jsonl={cfg.jsonl})", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if cfg.pidfile and os.path.exists(cfg.pidfile):
            os.remove(cfg.pidfile)


if __name__ == "__main__":
    main()