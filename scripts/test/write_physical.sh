#!/usr/bin/env bash
# Write the canonical F0 image to an expendable physical disk, once, with
# flush, read-back and SHA-256 comparison (docs/design/f0-acceptance.md,
# "Physical procedure"). Run with sudo. The target is named by its stable
# /dev/disk/by-id path so a renumbered device can never be hit by mistake.
#
#   sudo scripts/test/write_physical.sh /dev/disk/by-id/ata-TS64GMSA230S_<serial> [image]
#
set -euo pipefail
target="${1:-}"
image="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/build/f0/ciukios.img}"
[[ -n "$target" ]] || { echo "usage: sudo $0 /dev/disk/by-id/<stable-name> [image]" >&2; exit 2; }
[[ "$target" == /dev/disk/by-id/* ]] || { echo "[write] name the target by its /dev/disk/by-id path" >&2; exit 2; }
[[ -b "$target" ]] || { echo "[write] $target is not a block device (disk not connected?)" >&2; exit 1; }
[[ -f "$image" ]] || { echo "[write] image $image missing; run make build-full" >&2; exit 1; }
[[ $EUID -eq 0 ]] || { echo "[write] run with sudo" >&2; exit 1; }
dev="$(readlink -f "$target")"
root_dev="$(findmnt -no SOURCE / | sed -E 's/\[.*//')"
root_disk="/dev/$(lsblk -no PKNAME "$root_dev" 2>/dev/null || true)"
[[ "$dev" != "$root_disk" && "$dev" != "$root_dev" ]] || { echo "[write] $dev holds the host root filesystem: refused" >&2; exit 1; }
if lsblk -no MOUNTPOINTS "$dev" | grep -q .; then
    echo "[write] $dev has mounted filesystems: unmount them first" >&2; lsblk "$dev"; exit 1
fi
size=$(blockdev --getsize64 "$dev"); image_size=$(stat -c %s "$image")
(( size >= image_size )) || { echo "[write] disk ($size bytes) smaller than the image ($image_size)" >&2; exit 1; }
model="$(lsblk -dno MODEL,SERIAL,SIZE "$dev" | tr -s ' ')"
echo "[write] target: $dev ($model), stable name $target"
echo "[write] image : $image ($image_size bytes)"
image_sha="$(sha256sum "$image" | cut -d' ' -f1)"
echo "[write] image SHA-256: $image_sha"
echo "[write] ALL DATA ON $dev WILL BE DESTROYED. Type the disk's serial to continue:"
read -r answer
[[ -n "$answer" && "$target" == *"$answer"* ]] || { echo "[write] serial mismatch: aborted" >&2; exit 1; }
dd if="$image" of="$dev" bs=1M conv=fsync status=progress
sync
blockdev --flushbufs "$dev"
echo "[write] reading back $image_size bytes"
back_sha="$(head -c "$image_size" "$dev" | sha256sum | cut -d' ' -f1)"
echo "[write] read-back SHA-256: $back_sha"
[[ "$back_sha" == "$image_sha" ]] || { echo "[write] MISMATCH: do not use this disk for evidence" >&2; exit 1; }
out="$(dirname "$image")/physical-write-$(date -u +%Y%m%dT%H%M%SZ).json"
printf '{"target":"%s","device":"%s","model":"%s","image":"%s","image_sha256":"%s","readback_sha256":"%s","written_utc":"%s"}\n' \
    "$target" "$dev" "$model" "$image" "$image_sha" "$back_sha" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$out"
chown --reference="$image" "$out" 2>/dev/null || true
echo "[write] OK, record: $out"
