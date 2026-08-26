#!/usr/bin/env bash
# scripts/ship.sh — format, test, commit and push.
#
#   ./scripts/ship.sh "commit message"
#   ./scripts/ship.sh "message" --tag v1.0.1     also tag, which publishes a release
#
# Refuses to push if the formatter or the test suite is unhappy, because a
# green main branch is worth more than a fast one.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

MSG="${1:?usage: ship.sh \"commit message\" [--tag vX.Y.Z]}"
TAG=""
[ "${2:-}" = "--tag" ] && TAG="${3:?--tag needs a version}"

echo "==> building"
make >/dev/null

echo "==> formatting"
make fmt >/dev/null
make >/dev/null            # a formatting change can affect the built tools

echo "==> testing"
./tests/run.sh | tail -2

if [ -n "$(git status --porcelain)" ]; then
    git add -A
    git commit -qm "$MSG"
    echo "==> committed: $(git log -1 --oneline)"
else
    echo "==> nothing to commit"
fi

echo "==> pushing"
git push origin HEAD

if [ -n "$TAG" ]; then
    echo "==> tagging $TAG"
    git tag -a "$TAG" -m "$MSG"
    git push origin "$TAG"
    echo "    the release workflow will publish artifacts for $TAG"
fi

echo
echo "https://github.com/$(git remote get-url origin | sed 's#.*github.com[:/]##; s#\.git$##')"
