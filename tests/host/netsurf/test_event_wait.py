"""F2-98c DNS publication and connection deadlines; no browser/SDK build."""
# SPDX-License-Identifier: MIT
import os
import re
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class EventWaitTests(unittest.TestCase):
    def test_dns_completion(self):
        self.compile_run("dns", ["tests/host/netsurf/dns_test.c", "sdk/netsurf/port/dns.c"],
                         ["-Wl,--wrap=getaddrinfo,--wrap=freeaddrinfo,--wrap=calloc,--wrap=free,--wrap=pthread_create"])

    def test_connection_deadlines(self):
        self.compile_run("deadlines", ["tests/host/netsurf/net_wait_test.c"],
                         ["-DCIUKI_HOST", "-include", str(ROOT / "sdk/netsurf/port/host_network.h"),
                          "-I", str(ROOT / "build/external/netsurf-host/native-headers"),
                          "-I", str(ROOT / "build/external/netsurf-host/prefix/include")])

    def compile_run(self, name, sources, extra):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="netsurf-wait-", dir=parent) as tmp:
            binary = Path(tmp) / name
            if name == "deadlines":
                source = (ROOT / "sdk/netsurf/port/fetch_ciuki.c").read_text()
                match = re.search(r"^static int event_wait\([^\n]+\)\n\{", source, re.M)
                self.assertIsNotNone(match)
                end, depth = match.end(), 1
                while depth:
                    depth += (source[end] == "{") - (source[end] == "}")
                    end += 1
                (Path(tmp) / "fetch_event_wait.inc").write_text(source[match.start():end])
                sources = [*sources, "tests/host/desktop/native_event_channel_host.c"]
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-pthread",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-I", tmp, "-I", str(ROOT), "-I", str(ROOT / "src/kernel/include"),
                            *extra, *(str(ROOT / p) for p in sources), "-o", str(binary)], check=True)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=15, env={**os.environ, "TMPDIR":tmp})
            print(result.stdout, end="", flush=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
