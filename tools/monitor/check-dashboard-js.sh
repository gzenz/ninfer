#!/usr/bin/env bash
# The dashboard page is a JAVASCRIPT template embedded in a PYTHON file. A mistake in that embedded JS is
# invisible to Python -- it imports fine, the service starts fine, the HTML serves fine -- and only a
# browser parsing it fails, taking the WHOLE page with it: no fetch, no tiles, every card empty, for every
# viewer, with no error anywhere a log would show.
#
# That is not hypothetical. On 2026-09-27 a shared pinned-pool panel was added with PYTHON comment lines
# inside the JS string literal (commit ecefa2fe). The string never closed, the script threw "Invalid or
# unexpected token", and the dashboard was dead for hours while its service reported "active" and its
# /api/samples endpoint answered perfectly. It was mis-diagnosed twice as a timeout/liveness problem before
# anything rendered the page.
#
# So: extract the script block from the source and parse it. No server, no browser, no network.
set -euo pipefail
src="${1:-$(dirname "$0")/monitor.py}"
tmp="$(mktemp /tmp/ninfer-dashboard-js-XXXXXX.js)"
trap 'rm -f "$tmp"' EXIT
python3 - "$src" >"$tmp" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
blocks = re.findall(r"<script[^>]*>(.*?)</script>", text, re.S)
if not blocks:
    sys.exit("check-dashboard-js: no <script> block found in " + sys.argv[1])
sys.stdout.write("\n".join(blocks))
PY
if ! command -v node >/dev/null 2>&1; then
    echo "check-dashboard-js: node not available -- JS NOT checked (absence of node is not a pass)" >&2
    exit 77
fi
node --check "$tmp"
echo "dashboard JS: parses OK ($(wc -c <"$tmp") bytes from $src)"
