#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
current="$(git config --local --get core.hooksPath || true)"
if [[ -n "$current" && "$current" != ".githooks" ]]; then
    echo "[release-hook] existing core.hooksPath=$current; keep it and install the CiukiOS hook manually" >&2
    exit 1
fi
default_hook="$(git rev-parse --git-path hooks/pre-push)"
if [[ -e "$default_hook" && "$current" != ".githooks" ]]; then
    echo "[release-hook] existing $default_hook; keep it and install the CiukiOS hook manually" >&2
    exit 1
fi
source_hook="scripts/hooks/pre-push"
local_hook=".githooks/pre-push"
if [[ -e "$local_hook" || -L "$local_hook" ]]; then
    if [[ ! -f "$local_hook" ]] || ! cmp -s "$source_hook" "$local_hook"; then
        echo "[release-hook] existing $local_hook differs from the CiukiOS hook; preserve it and install manually" >&2
        exit 1
    fi
else
    mkdir -p .githooks
    install -m 0755 "$source_hook" "$local_hook"
fi
chmod +x "$local_hook"
git config --local core.hooksPath .githooks
echo "[release-hook] installed: pushes of origin/main build, test, then publish the Windows ZIP"
