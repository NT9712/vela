#!/usr/bin/env bash
# scripts/deploy.sh — publish this repository to GitHub.
#
#   ./scripts/deploy.sh [repo-name] [--private]
#
# Requires `gh` to be authenticated:  gh auth login
set -euo pipefail

NAME="${1:-vela}"
VIS="--public"
[ "${2:-}" = "--private" ] && VIS="--private"

cd "$(dirname "${BASH_SOURCE[0]}")/.."

command -v gh >/dev/null || { echo "gh is not installed"; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "run 'gh auth login' first"; exit 1; }

DESC="A small, fast, statically typed language that compiles straight to native x86-64 executables — no VM, no libc, no linker."

echo "==> verifying the tree builds and tests clean"
make >/dev/null
./tests/run.sh >/dev/null

echo "==> creating github repository '$NAME'"
gh repo create "$NAME" $VIS --source=. --remote=origin --description "$DESC" --push

OWNER="$(gh api user --jq .login)"

echo "==> setting repository topics"
gh api -X PUT "repos/$OWNER/$NAME/topics" \
  -H "Accept: application/vnd.github+json" \
  -f names[]=programming-language -f names[]=compiler -f names[]=x86-64 \
  -f names[]=code-generation -f names[]=garbage-collection -f names[]=elf \
  -f names[]=systems-programming -f names[]=language-design >/dev/null

echo
echo "published: https://github.com/$OWNER/$NAME"
