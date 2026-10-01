#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
current="$(git config --local --get core.hooksPath || true)"
if [[ -n "$current" && "$current" != ".githooks" ]]; then
    echo "[release-hook] existing core.hooksPath=$current; keep it and install the CiukiOS hook manually" >&2
    exit 1
fi
if [[ -e .git/hooks/pre-push && "$current" != ".githooks" ]]; then
    echo "[release-hook] existing .git/hooks/pre-push; keep it and install the CiukiOS hook manually" >&2
    exit 1
fi
chmod +x .githooks/pre-push
git config --local core.hooksPath .githooks
echo "[release-hook] installed: pushes of origin/main build, test, then publish the Windows ZIP"
