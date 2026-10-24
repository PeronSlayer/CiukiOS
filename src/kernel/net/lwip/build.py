"""Offline lwIP 2.2.1 source recipe, shared by kernel and host tests."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
SHA256 = "7b622662dba2383d71f874f2e494b54ae948531559c17acbe75797966d646878"
SOURCES = tuple("core/" + n + ".c" for n in (
    "init", "def", "inet_chksum", "ip", "mem", "memp", "netif", "pbuf",
    "stats", "sys", "timeouts", "dns", "raw", "udp", "tcp", "tcp_in", "tcp_out")) + tuple(
    "core/ipv4/" + n + ".c" for n in (
        "ip4", "ip4_addr", "icmp", "etharp", "dhcp", "acd")) + (
    "netif/ethernet.c", "api/tcpip.c", "api/api_lib.c", "api/api_msg.c",
    "api/netbuf.c", "api/err.c")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source():
    record = json.loads((HERE / "import.json").read_text())
    if record["sources"] != list(SOURCES) or record["sha256"] != SHA256:
        raise ValueError("lwIP source list/hash differs from the import record")
    archive = ROOT / "build/downloads/lwip/lwip-2.2.1.zip"
    if not archive.is_file() or archive.stat().st_size != 5087893 or digest(archive) != SHA256:
        raise ValueError("missing/changed pinned lwIP 2.2.1 archive; no download/fallback")
    folder = ROOT / "build/external/lwip-2.2.1"
    stamp = folder / "ciuki-source-hashes.json"
    patches = sorted((HERE / "patches").glob("*.patch"))
    patch_hashes = {p.name: digest(p) for p in patches}
    expected = record["patches"]
    if patch_hashes != expected:
        raise ValueError("unrecorded/changed lwIP patch")
    if stamp.exists():
        record = json.loads(stamp.read_text())
        hashes = record.get("files", {})
        names = {str(p.relative_to(folder)) for p in folder.rglob("*") if p.is_file() and p != stamp}
        if record.get("patches") == patch_hashes and names == set(hashes) and all(digest(folder / n) == h for n, h in hashes.items()):
            return folder
    if folder.exists():
        shutil.rmtree(folder)
    folder.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as stream:
        for item in stream.infolist():
            path = Path(item.filename)
            if path.is_absolute() or ".." in path.parts or path.parts[0] != "lwip-2.2.1":
                raise ValueError("unsafe lwIP archive member")
            if (item.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError("lwIP archive contains a symbolic link")
        stream.extractall(folder.parent)
    for patch in patches:
        subprocess.run(["patch", "--binary", "--batch", "--fuzz=0", "-p1", "-i", str(patch)], cwd=folder,
                       check=True, stdout=subprocess.DEVNULL)
    stamp.write_text(json.dumps({"patches": patch_hashes, "files": {
        str(p.relative_to(folder)): digest(p) for p in sorted(folder.rglob("*")) if p.is_file()}}, sort_keys=True) + "\n")
    return folder


def includes(src):
    return ["-I", str(HERE), "-I", str(src / "src/include")]


if __name__ == "__main__":
    print(source())
