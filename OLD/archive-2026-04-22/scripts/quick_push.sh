#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

REMOTE_NAME="${REMOTE_NAME:-origin}"
REMOTE_URL="${REMOTE_URL:-git@github.com:PeronSlayer/CiukiOS.git}"
BRANCH="${BRANCH:-main}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/ciukios_github}"
GIT_USER_NAME="${GIT_USER_NAME:-PeronSlayer}"
GIT_USER_EMAIL="${GIT_USER_EMAIL:-peronslayer@users.noreply.github.com}"
COMMIT_MSG="${1:-chore: quick sync}"

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Error: missing command: $1" >&2
        exit 1
    }
}

require_cmd git
cd "$PROJECT_DIR"

if [[ ! -d .git ]]; then
    echo "[quick-push] initializing git repository..."
    git init
fi

echo "[quick-push] configuring git identity..."
git config user.name "$GIT_USER_NAME"
git config user.email "$GIT_USER_EMAIL"

if [[ -f "$SSH_KEY" ]]; then
    echo "[quick-push] configuring SSH key: $SSH_KEY"
    git config core.sshCommand "ssh -i $SSH_KEY -o IdentitiesOnly=yes"

    if ! ssh-keygen -F github.com >/dev/null 2>&1; then
        echo "[quick-push] adding github.com to known_hosts..."
        mkdir -p "$HOME/.ssh"
        ssh-keyscan -H github.com >> "$HOME/.ssh/known_hosts" 2>/dev/null
    fi
else
    echo "[quick-push] warning: SSH key not found at $SSH_KEY"
    echo "[quick-push] push may fail unless SSH is configured globally"
fi

current_remote_url="$(git remote get-url "$REMOTE_NAME" 2>/dev/null || true)"
if [[ -z "$current_remote_url" ]]; then
    echo "[quick-push] adding remote $REMOTE_NAME -> $REMOTE_URL"
    git remote add "$REMOTE_NAME" "$REMOTE_URL"
elif [[ "$current_remote_url" != "$REMOTE_URL" ]]; then
    echo "[quick-push] updating remote $REMOTE_NAME -> $REMOTE_URL"
    git remote set-url "$REMOTE_NAME" "$REMOTE_URL"
fi

echo "[quick-push] staging changes..."
git add -A

if git diff --cached --quiet; then
    echo "[quick-push] no staged changes to commit"
else
    echo "[quick-push] committing: $COMMIT_MSG"
    git commit -m "$COMMIT_MSG"
fi

echo "[quick-push] syncing with $REMOTE_NAME/$BRANCH..."
git fetch "$REMOTE_NAME" "$BRANCH" >/dev/null 2>&1 || true
if git show-ref --quiet "refs/remotes/$REMOTE_NAME/$BRANCH"; then
    git pull --no-rebase "$REMOTE_NAME" "$BRANCH" --allow-unrelated-histories
else
    echo "[quick-push] remote branch $REMOTE_NAME/$BRANCH not found, skipping pull"
fi

echo "[quick-push] pushing to $REMOTE_NAME/$BRANCH..."
git push -u "$REMOTE_NAME" "$BRANCH"

echo "[quick-push] done"
