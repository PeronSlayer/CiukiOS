#!/usr/bin/env bash
# Pinned upstream Legacy BIOS release; never accidentally select UEFI assets.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
cache=build/tools/grub4dos
archive="$cache/grub4dos-0.4.6a-2020-08-09.7z"
source_archive="$cache/source-2020-08-09.tar.gz"
mkdir -p "$cache"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 \
        https://github.com/chenall/grub4dos/releases/download/2020-08-09-0da21fe/grub4dos-0.4.6a-2020-08-09.7z \
        --output "$archive.tmp"
    mv "$archive.tmp" "$archive"
fi
printf '%s  %s\n' 73fd88d7eb231bf0321f6159e0f40e7d4dd18529d8b6f6dde2a0f1b2555c571f "$archive" | sha256sum --check --status
if [[ ! -f "$source_archive" ]]; then
    curl --fail --location --retry 3 \
        https://codeload.github.com/chenall/grub4dos/tar.gz/refs/tags/2020-08-09-0da21fe \
        --output "$source_archive.tmp"
    mv "$source_archive.tmp" "$source_archive"
fi
printf '%s  %s\n' 64a6b5b7a543d9e316ad3e4320a14b1110cb0a966bf2e22a48e3392d063941bc "$source_archive" | sha256sum --check --status
7z x -y "-o$cache/release" "$archive" >/dev/null
printf '%s  %s\n' dece3f8d20f84ae0d0fb892b5c3a2d19e7233d0d8885b0027a6f43d77239128d "$cache/release/grub4dos-0.4.6a/grldr" | sha256sum --check --status
echo '[grub4dos] verified Legacy BIOS 0.4.6a, 2020-08-09; matching source included'

python3 scripts/patch_grub4dos_irq.py "$cache/release/grub4dos-0.4.6a/grldr" "$cache/grldr-ciukios"
