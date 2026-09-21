# NInfer Monitor

Real-time dashboard for the NInfer inference engine. Tails the server's
JSONL request log and stderr log, polls the /stats endpoint, nvidia-smi,
/proc, and the 12VHPWR connector temperature.

## Setup

1. Install the monitor on the same machine as the inference server (or
   a machine that can reach both the server and the GPU host).
2. Create `read_12vhpwr.sh` next to `monitor.py` — a shell script that
   reads the 12VHPWR connector temperature and prints a bare Celsius value
   (e.g. "49.0"). It is resolved relative to the script's own directory,
   never the launch cwd, and when absent the tile degrades to "–".
   This script is NOT checked in because it contains host-specific
   SSH credentials. On the GPU host (Windows side), use
   LibreHardwareMonitorLib.dll to read the mainboard sensor that
   measures the 12VHPWR plug cabling temperature.

   Example `read_12vhpwr.sh`:
   ```bash
   #!/bin/bash
   ssh -o BatchMode=yes -o ConnectTimeout=5 user@gpu-host read_12vhpwr.cmd
   ```

3. Run the monitor:
   ```bash
   python3 monitor.py --server http://localhost:8080 --jsonl ~/ninfer-requests.jsonl \
     --serve-log ~/ninfer-serve.log --port 8090
   ```

## Features

- Request table with TTFT, queue wait, processing time, cache hit/miss
- Processing time chart (p50/p95) — excludes queue wait
- Device KV occupancy (live page count)
- Host KV usage (bytes used / capacity)
- D2H/H2D transfer charts
- Pressure events (evictions, demotes, fallbacks)
- GPU stats (power, temperature, memory, utilization)
- Windows host memory (via WSL interop when running inside WSL on the
  GPU box; reported as `stats.windows` in `/api/samples`)
- 12VHPWR connector temperature monitoring with auto-kill on overheat
