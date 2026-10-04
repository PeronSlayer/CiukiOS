#!/usr/bin/env python3
"""Push main and publish its locally built Windows ZIP as a GitHub Release."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

from package_windows_portable import NAME, RELEASE_DIR


ROOT = Path(__file__).resolve().parent.parent
REPO = os.environ.get("CIUKIOS_GITHUB_REPO", "PeronSlayer/CiukiOS")


def command(*args: str, check: bool = True, env: dict[str, str] | None = None) -> str:
    result = subprocess.run(args, cwd=ROOT, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and result.returncode:
        raise SystemExit(f"[push-release] {' '.join(args)} failed:\n{result.stderr.strip()}")
    return result.stdout.strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def clean_tree() -> None:
    changes = command("git", "status", "--porcelain", "--untracked-files=normal")
    if changes:
        raise SystemExit("[push-release] commit all project changes before pushing; "
                         "the ZIP must match the pushed commit")


def plan() -> dict[str, str]:
    commit = command("git", "rev-parse", "HEAD")
    version = NAME.split("-")[1]
    count = command("git", "rev-list", "--count", "HEAD")
    timestamp = datetime.fromisoformat(command("git", "show", "-s", "--format=%cI", "HEAD"))
    stamp = timestamp.astimezone(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    tag = f"v{version}-{stamp}-b{count}-g{commit[:8]}"
    return {
        "commit": commit,
        "version": version,
        "build": count,
        "stamp": stamp,
        "tag": tag,
        "title": f"CiukiOS {version} | Build {count} | {stamp}",
        "asset": f"CiukiOS-{version}-{stamp}-build{count}-Windows-portable.zip",
    }


def verify_bundle(bundle: Path, commit: str) -> None:
    if not bundle.is_file():
        raise SystemExit(f"[push-release] Windows ZIP is missing: {bundle}")
    with zipfile.ZipFile(bundle) as archive:
        manifest = json.loads(archive.read(f"{NAME}/MANIFEST.json"))
        if archive.testzip() is not None:
            raise SystemExit("[push-release] Windows ZIP failed CRC validation")
    if manifest.get("source_commit") != commit or manifest.get("source_dirty") is not False:
        raise SystemExit("[push-release] Windows ZIP does not match a clean HEAD")
    if manifest.get("windows_runtime_tested") is not False:
        raise SystemExit("[push-release] Windows test status in manifest is unexpected")


def remote_commit() -> str:
    line = command("git", "ls-remote", "origin", "refs/heads/main")
    return line.split()[0] if line else ""


def github_release(tag: str) -> dict | None:
    result = subprocess.run(
        ["gh", "api", f"repos/{REPO}/releases/tags/{tag}"], cwd=ROOT,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode == 0:
        return json.loads(result.stdout)
    if "HTTP 404" in result.stderr:
        return None
    raise SystemExit(f"[push-release] cannot inspect GitHub Release:\n{result.stderr.strip()}")


def verify_uploaded_asset(tag: str, asset: Path, expected_sha: str) -> str:
    release = github_release(tag)
    if release is None:
        raise SystemExit("[push-release] release is missing after upload")
    uploaded = next((entry for entry in release.get("assets", [])
                     if entry.get("name") == asset.name), None)
    if uploaded is None or uploaded.get("size") != asset.stat().st_size \
            or uploaded.get("state") != "uploaded":
        raise SystemExit("[push-release] release asset is missing or incomplete")
    remote_digest = uploaded.get("digest")
    if remote_digest and remote_digest != f"sha256:{expected_sha}":
        raise SystemExit("[push-release] release asset SHA-256 differs from local ZIP")
    return release["html_url"]


def publish(info: dict[str, str], bundle: Path) -> str:
    with tempfile.TemporaryDirectory(dir=RELEASE_DIR, prefix="publish-") as scratch:
        asset = Path(scratch) / info["asset"]
        shutil.copyfile(bundle, asset)
        digest = sha256(asset)
        notes = Path(scratch) / "release-notes.md"
        subject = command("git", "show", "-s", "--format=%s", info["commit"])
        notes.write_text(
            f"CiukiOS {info['version']} · build {info['build']} · "
            f"{info['stamp']} (UTC)\n\n"
            f"Commit: `{info['commit']}` — {subject}\n\n"
            "The ZIP contains CiukiOS and portable Windows QEMU. "
            "Local commercial DOS game data is excluded. "
            "The full image passed the Linux QEMU boot smoke test; "
            "the Windows launcher has not been runtime-tested on Windows.\n\n"
            f"ZIP SHA-256: `{digest}`\n", encoding="utf-8")
        existing = github_release(info["tag"])
        if existing:
            if existing.get("target_commitish") != info["commit"]:
                raise SystemExit("[push-release] existing release targets another commit")
            uploaded = next((item for item in existing.get("assets", [])
                             if item.get("name") == asset.name), None)
            if uploaded:
                if uploaded.get("size") == asset.stat().st_size \
                        and uploaded.get("digest") in (None, f"sha256:{digest}"):
                    return existing["html_url"]
                raise SystemExit("[push-release] release already has a different ZIP asset")
            command("gh", "release", "upload", info["tag"], str(asset), "-R", REPO)
        else:
            command("gh", "release", "create", info["tag"], str(asset),
                    "--target", info["commit"], "--title", info["title"],
                    "--notes-file", str(notes), "--prerelease", "-R", REPO)
        return verify_uploaded_asset(info["tag"], asset, digest)


def prepare(info: dict[str, str]) -> Path:
    if command("git", "branch", "--show-current") != "main":
        raise SystemExit("[push-release] releases are published from main only")
    clean_tree()
    if shutil.which("gh") is None:
        raise SystemExit("[push-release] install and authenticate GitHub CLI first")
    command("gh", "auth", "status")
    actual_repo = command("gh", "repo", "view", "--json", "nameWithOwner",
                          "--jq", ".nameWithOwner")
    if actual_repo.lower() != REPO.lower():
        raise SystemExit(f"[push-release] origin is {actual_repo}, expected {REPO}")
    print("[push-release] building the canonical full image and Windows ZIP", flush=True)
    env = dict(os.environ)
    env["CIUKIOS_VM_WINDOW"] = "1"
    env["CIUKIOS_WINDOWS_PORTABLE"] = "1"
    # Keep the hook's rebuild under the same workstation cap as interactive
    # builds. It must finish before the runtime smoke starts.
    env["CIUKIOS_BUILD_JOBS"] = "1"
    available_kib = next(int(line.split()[1]) for line in
                         Path("/proc/meminfo").read_text().splitlines()
                         if line.startswith("MemAvailable:"))
    if available_kib < 4 * 1024 * 1024:
        raise SystemExit("[push-release] less than 4 GiB host memory available; build deferred")
    subprocess.run(["systemd-run", "--user", "--scope", "-p", "MemoryMax=3G",
                    "-p", "MemorySwapMax=1G", "-p", "CPUQuota=100%", "--",
                    "bash", "scripts/build_full.sh"], cwd=ROOT, env=env, check=True)
    clean_tree()
    bundle = RELEASE_DIR / f"{NAME}.zip"
    verify_bundle(bundle, info["commit"])
    print("[push-release] testing full image with one light QEMU boot", flush=True)
    env["QEMU_MEMORY_MB"] = "128"
    env["QEMU_TIMEOUT_SEC"] = "12"
    subprocess.run(["bash", "scripts/qemu_run_full.sh", "--test", "--no-build"],
                   cwd=ROOT, env=env, check=True)
    pending = RELEASE_DIR / "pending" / info["tag"]
    pending.mkdir(parents=True, exist_ok=True)
    snapshot = pending / info["asset"]
    shutil.copyfile(bundle, snapshot)
    (pending / "release.json").write_text(json.dumps(info, indent=2) + "\n",
                                          encoding="utf-8")
    return snapshot


def wait_and_publish(tag: str) -> None:
    pending = RELEASE_DIR / "pending" / tag
    lock = pending / "publish.lock"
    with lock.open("w") as lock_file:
        try:
            fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("[push-release] publisher already running", flush=True)
            return
        info = json.loads((pending / "release.json").read_text(encoding="utf-8"))
        if info["tag"] != tag:
            raise SystemExit("[push-release] pending release tag mismatch")
        asset = pending / info["asset"]
        verify_bundle(asset, info["commit"])
        deadline = time.monotonic() + 300
        while time.monotonic() < deadline:
            if remote_commit() == info["commit"]:
                print(f"[push-release] confirmed origin/main={info['commit']}", flush=True)
                url = publish(info, asset)
                (pending / "status.json").write_text(
                    json.dumps({"state": "published", "url": url}) + "\n",
                    encoding="utf-8")
                print(f"[push-release] published: {url}", flush=True)
                return
            time.sleep(2)
        (pending / "status.json").write_text(
            json.dumps({"state": "not-published", "reason": "commit not found on origin/main"}) + "\n",
            encoding="utf-8")
        raise SystemExit("[push-release] push was not confirmed within five minutes")


def start_background_publisher(info: dict[str, str]) -> None:
    pending = RELEASE_DIR / "pending" / info["tag"]
    log_path = pending / "publish.log"
    with log_path.open("a", encoding="utf-8") as log:
        subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                          "--publish-after-push", info["tag"]],
                         cwd=ROOT, stdin=subprocess.DEVNULL, stdout=log,
                         stderr=subprocess.STDOUT, start_new_session=True,
                         close_fds=True)
    print(f"[push-release] release will publish after origin/main updates; log: {log_path}",
          flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", action="store_true", help="show tag and asset without pushing")
    parser.add_argument("--resume", action="store_true", help="resume upload from a prepared ZIP")
    parser.add_argument("--from-hook", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--publish-after-push", metavar="TAG", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.publish_after_push:
        try:
            wait_and_publish(args.publish_after_push)
        except BaseException as exc:
            pending = RELEASE_DIR / "pending" / args.publish_after_push
            pending.mkdir(parents=True, exist_ok=True)
            (pending / "status.json").write_text(
                json.dumps({"state": "failed", "reason": str(exc)}) + "\n",
                encoding="utf-8")
            raise
        return
    info = plan()
    if args.plan:
        print(json.dumps(info, indent=2))
        return
    if args.resume:
        pending = RELEASE_DIR / "pending" / info["tag"]
        snapshot = pending / info["asset"]
        verify_bundle(snapshot, info["commit"])
        if remote_commit() != info["commit"]:
            raise SystemExit("[push-release] origin/main does not match the prepared commit")
        print(f"[push-release] published: {publish(info, snapshot)}")
        return
    snapshot = prepare(info)
    if args.from_hook:
        start_background_publisher(info)
        return
    push_env = dict(os.environ)
    push_env["CIUKIOS_RELEASE_INNER_PUSH"] = "1"
    print(f"[push-release] pushing {info['commit']} to origin/main", flush=True)
    subprocess.run(["git", "push", "origin", "HEAD:refs/heads/main"],
                   cwd=ROOT, env=push_env, check=True)
    if remote_commit() != info["commit"]:
        raise SystemExit("[push-release] remote main does not match the built commit")
    url = publish(info, snapshot)
    print(f"[push-release] published: {url}")


if __name__ == "__main__":
    main()
