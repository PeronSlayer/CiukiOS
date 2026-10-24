#!/usr/bin/env python3
"""Sequential ASan/UBSan production Setup tests, no image or guest build."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[3]
sys.dont_write_bytecode = True
SOURCES = [*(f"apps/setup/{n}.c" for n in ("app", "pages", "hw", "catalog", "text", "view", "inf", "rescue", "native", "media", "setup_info", "report_link")),
           "sdk/libsetup/fake/fake.c", "src/kernel/lib/tzrule.c", "apps/hwreport/report.c", "apps/hwreport/textlink.c", "tests/host/setup/setup_test.c", "tests/host/setup/native_test.c", "tests/host/setup/link_test.c",
           *(f"apps/libdeskui/{n}.c" for n in ("draw", "widgets", "list", "menu", "dialog", "controls", "commdlg", "textarea", "icons", "icon_map")),
           *(f"apps/desktop/{n}.c" for n in ("theme", "settings", "compositor", "sounds"))]
def main():
    with tempfile.TemporaryDirectory(prefix="ciuki-setup-") as temp:
        folder=Path(temp)
        spec=importlib.util.spec_from_file_location("portrait",ROOT/"apps/desktop/convert_portrait.py")
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        portrait=folder/"portrait.xrgb";portrait.write_bytes(module.convert(ROOT/"assets/brand/ciuki-logo.png"))
        binary=folder/"setup-test"
        subprocess.run(["clang","-std=c17","-O1","-g","-Wall","-Wextra","-Werror","-D_POSIX_C_SOURCE=200809L",
                        ("-fsanitize=undefined" if "--ubsan" in sys.argv else "-fsanitize=address,undefined"),"-fno-sanitize-recover=undefined","-ffunction-sections","-fdata-sections","-Wl,--gc-sections", "-Wl,--wrap=malloc,--wrap=calloc,--wrap=waitpid",
                        f'-DST_NATIVE_ROOT="{temp}"', "-idirafter",str(ROOT/"sdk/sysroot-overlay/include"),
                        "-I",str(ROOT/"src/kernel/include"),"-I",str(ROOT/"sdk/libsetup/include"),
                        *(str(ROOT/s) for s in SOURCES),"-o",str(binary)],check=True)
        names=[line.split()[-1] for line in subprocess.check_output(["nm","-g","--defined-only",str(binary)],text=True).splitlines() if line.split()]
        assert len(names)==len(set(names)), "duplicate exported symbol"
        subprocess.run([str(binary),str(portrait),str(folder)],check=True)
        review_spec=importlib.util.spec_from_file_location("setup_frame_review",ROOT/"tests/host/desktop/frame_review.py")
        review=importlib.util.module_from_spec(review_spec);review_spec.loader.exec_module(review)
        for frame in folder.glob("*.frame"): review.capture("setup-"+frame.stem,frame.read_bytes(),640,480)
        actual={p.stem:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.glob("*.frame"))}
        gold=ROOT/"tests/host/setup/goldens.json"
        if "--record" in sys.argv: gold.write_text(json.dumps(actual,indent=2,sort_keys=True)+"\n")
        else:
            expected=json.loads(gold.read_text())
            assert actual==expected, f"Setup render mismatch: {actual}"
        print(f"PASS Setup: {len(actual)} 640x480 frame goldens; " + ("UBSan only" if "--ubsan" in sys.argv else "ASan/UBSan, leak detection enabled"))
if __name__=="__main__":main()
