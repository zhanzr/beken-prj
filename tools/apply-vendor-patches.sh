#!/bin/bash
# Apply local (non-upstreamable) patches to the vendor submodule.
# Idempotent-ish: git apply fails loudly if a patch is already applied.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IDK="$ROOT/vendor/bk_aidk/bk_avdk/bk_idk"

if [ ! -d "$IDK/.git" ] && [ ! -f "$IDK/.git" ]; then
    echo "ERROR: vendor submodule not found at $IDK" >&2
    echo "Run:  git submodule update --init --recursive" >&2
    exit 1
fi

shopt -s nullglob
for p in "$ROOT"/patches/*.patch; do
    echo ">> applying $(basename "$p")"
    # skip patches already applied (worktree already contains them)
    if git -C "$IDK" apply --reverse --check --whitespace=nowarn "$p" 2>/dev/null; then
        echo "   already applied, skipping"
        continue
    fi
    git -C "$IDK" apply --whitespace=nowarn "$p"
done
echo ">> vendor patches OK"
