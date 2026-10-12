#!/usr/bin/env python3
"""Build the provisional pinned Kenney CC0 desktop WAV scheme (under 1 MiB)."""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import urllib.request
import wave
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EVENTS = {
    "startup": "confirmation_004", "shutdown": "close_004",
    "menu_open": "open_001", "menu_select": "select_001",
    "window_open": "open_002", "window_close": "close_002",
    "minimise": "minimize_001", "restore": "maximize_001",
    "error": "error_001", "information": "confirmation_001",
    "question": "question_002", "end_task": "close_003",
    "volume": "tick_001", "empty_recycle": "drop_001",
}
OUTPUT = ROOT / "build/external/system-sounds"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def archive():
    upstream = ROOT / "assets/sounds/kenney-interface"
    pin = json.loads((upstream / "UPSTREAM.json").read_text())
    cache = ROOT / "build/downloads/kenney_interface-sounds.zip"
    if not cache.exists():
        existing = upstream / "upstream.zip"
        if existing.exists():
            # Use the pinned vendored archive directly, including in worktrees
            # whose shared download cache is read-only.
            cache = existing
        else:
            data = urllib.request.urlopen(pin["download"], timeout=60).read()
            if sha(data) != pin["archive_sha256"]:
                raise ValueError("Kenney archive SHA-256 mismatch")
            cache.parent.mkdir(parents=True, exist_ok=True)
            cache.write_bytes(data)
    if sha(cache.read_bytes()) != pin["archive_sha256"]:
        raise ValueError("Kenney cached archive SHA-256 mismatch")
    return pin, zipfile.ZipFile(cache)


def build(output=OUTPUT):
    pin, pack = archive()
    output.mkdir(parents=True, exist_ok=True)
    manifest = {"provisional": True, "author": pin["author"], "license": pin["license"],
                "source": pin["source"], "archive_sha256": pin["archive_sha256"], "files": {}, "events": EVENTS}
    with pack:
        for event, source in EVENTS.items():
            name = next(n for n in pack.namelist() if n.endswith("Audio/" + source + ".ogg"))
            data = pack.read(name)
            if sha(data) != pin["files"][source + ".ogg"]:
                raise ValueError("Kenney member SHA-256 mismatch: " + source)
            # Fixed integer PCM output, no metadata/timestamps or dithering. Python
            # emits the canonical RIFF header instead of ffmpeg's version metadata.
            pcm = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-bitexact", "-i", "pipe:0",
                                  "-ac", "1", "-ar", "22050", "-af", "aresample=dither_method=none",
                                  "-f", "s16le", "pipe:1"], input=data, capture_output=True, check=True).stdout
            if not pcm or len(pcm) % 2 or len(pcm) > 22050 * 2 * 2:
                raise ValueError("event exceeds two seconds: " + event)
            with wave.open(str(output / (event + ".wav")), "wb") as wav:
                wav.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
                wav.writeframes(pcm)
        shutil.copyfile(ROOT / "assets/sounds/LICENSE.txt", output / "LICENSE.txt")
        shutil.copyfile(ROOT / "assets/sounds/CREDITS.TXT", output / "CREDITS.TXT")
        (output / "sounds.cfg").write_text("# Provisional Kenney CC0 system sound scheme\nenabled=1\n" +
                                          "".join(f"{e}={e}.wav\n" for e in EVENTS), encoding="ascii")
    manifest["files"] = {n: sha((output / n).read_bytes()) for n in
                         [*(e + ".wav" for e in EVENTS), "LICENSE.txt", "CREDITS.TXT", "sounds.cfg"]}
    total = sum((output / n).stat().st_size for n in manifest["files"])
    if total >= 1024 * 1024:
        raise ValueError("sound pack exceeds 1 MiB")
    manifest["bytes"] = total
    manifest["converter"] = subprocess.check_output(["ffmpeg", "-version"], text=True).splitlines()[0]
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def validate(output=OUTPUT):
    m = json.loads((output / "manifest.json").read_text())
    pin = json.loads((ROOT / "assets/sounds/kenney-interface/UPSTREAM.json").read_text())
    if m["archive_sha256"] != pin["archive_sha256"] or m["events"] != EVENTS:
        raise ValueError("stale sound pack provenance")
    expected = {*(e + ".wav" for e in EVENTS), "LICENSE.txt", "CREDITS.TXT", "sounds.cfg"}
    if set(m["files"]) != expected or not m["provisional"] or m["license"] != pin["license"] or m["source"] != pin["source"]:
        raise ValueError("invalid sound inventory or provenance")
    for name in ("LICENSE.txt", "CREDITS.TXT"):
        if sha((ROOT / "assets/sounds" / name).read_bytes()) != m["files"][name]:
            raise ValueError("stale sound notice: " + name)
    if sum((output / n).stat().st_size for n in expected) >= 1024 * 1024:
        raise ValueError("sound pack exceeds 1 MiB")
    for n, digest in m["files"].items():
        if sha((output / n).read_bytes()) != digest:
            raise ValueError("stale sound payload: " + n)
    return m


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    print(json.dumps(validate(args.output) if args.check else build(args.output), indent=2))
