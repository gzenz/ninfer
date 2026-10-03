#!/usr/bin/env bash
# EVIDENCE LOGS ARE GITIGNORED ON PURPOSE, AND `git add -f` HAS ALREADY PUT 57 OF THEM IN A COMMIT.
#
#   bash tools/ops/check-no-staged-logs.sh     # exit 1 if any *.log is staged -- use as a pre-commit check
#
# `.gitignore:45` ignores `*.log`, and CLAUDE.md's Observability section states the policy: what is committed
# is the MANIFEST beside the evidence -- binary sha256, artifact, git-head, and each log's own sha256 and
# rc -- never the log itself. The operator had to undo 57 force-added logs once. A policy that is enforced
# by remembering is enforced until the first busy night, which is the whole story of this directory.
#
# To install as a hook (does not touch anyone else's config):
#   ln -sf ../../tools/ops/check-no-staged-logs.sh .git/hooks/pre-commit
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 2

STAGED="$(git diff --cached --name-only --diff-filter=ACMR)"
LOGS="$(printf '%s\n' "$STAGED" | grep -E '\.log$' || true)"

if [ -n "$LOGS" ]; then
  echo "REFUSED: .log files are staged. *.log is gitignored by policy; the committed artifact is the"
  echo "          run's MANIFEST beside the evidence, not the log. Unstage them with:"
  echo
  printf '%s\n' "$LOGS" | sed 's/^/    git restore --staged /'
  echo
  echo "If a log is genuinely required, that is a policy change and the operator's call, not a -f."
  exit 1
fi
echo "ok: no .log staged ($(printf '%s\n' "$STAGED" | grep -c . ) files staged)"
