"""Amendment 1 regressions. Run from this directory, one process at a time."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))  # fx-14: pages.py

def recipe():
    spec=importlib.util.spec_from_file_location("netsurf_amendment_recipe",ROOT / "sdk/netsurf/build.py")
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module

class CacheTests(unittest.TestCase):
    def test_output_miss_discards_unverified_source(self):
        build=recipe()
        folder=ROOT / "build/external/netsurf-cache-regression"
        expected={"verified":"inputs"}
        class Prepared(Exception): pass
        def fresh(directory, source):
            self.assertEqual(directory,folder.resolve())
            self.assertFalse((directory / "source").exists())
            self.assertFalse((directory / "prefix").exists())
            raise Prepared()
        try:
            for missing in (True,False):
                folder.mkdir(parents=True,exist_ok=True)
                (folder / "source").mkdir()
                (folder / "source/dirty.c").write_text("unverified working source")
                (folder / "prefix").mkdir()
                (folder / "manifest.json").write_text(json.dumps({"inputs":expected,"files":{}}))
                if not missing:
                    (folder / "stage/Programs").mkdir(parents=True)
                    (folder / "stage/Programs/netsurf").write_text("stale output")
                with patch.object(build,"sources",return_value={"archive":Path("verified")}), \
                     patch.object(build,"inputs",return_value=expected),patch.object(build,"prepare",side_effect=fresh):
                    with self.assertRaises(Prepared):build.build(host=True,directory=folder)
                shutil.rmtree(folder)
        finally:
            shutil.rmtree(folder,ignore_errors=True)

class SanitizerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        recipe().build(host=True)
        cls.base=ROOT / "build/external/netsurf-host"
        cls.source=cls.base / "source/netsurf"
        cls.out=ROOT / "build/host/netsurf";cls.out.mkdir(parents=True,exist_ok=True)

    def compile_run(self,name,sources,extra=(),args=(),env=None,build=True):
        flags=["clang","-std=c17","-D_POSIX_C_SOURCE=200809L","-O1","-g","-Wall","-Wextra",
            "-Wno-unused-parameter","-Wno-unused-function","-ffunction-sections","-fdata-sections",
            "-fsanitize=address,undefined","-fno-sanitize-recover=undefined",
            "-I"+str(ROOT),"-I"+str(self.source),"-I"+str(self.source / "include"),
            "-I"+str(self.source / "frontends"),"-I"+str(self.source / "content/handlers"),"-I"+str(self.base / "native-headers"),
            "-I"+str(self.base / "prefix/include")]
        executable=self.out / name
        if build:
            subprocess.run([*flags,*map(str,sources),"-Wl,--gc-sections","-L"+str(self.base / "prefix/lib"),
                            *extra,"-o",str(executable)],check=True)
        subprocess.run([str(executable),*map(str,args)],check=True,timeout=120,
                       env=dict(os.environ,ASAN_OPTIONS="detect_leaks=1",UBSAN_OPTIONS="halt_on_error=1",**(env or {})))

    def test_layout_budget(self):
        self.compile_run("budget",[HERE / "budget_test.c",ROOT / "sdk/netsurf/port/budget.c"],
                         ["-Wl,--wrap=clock_gettime"])

    def test_stderr_record_lines(self):
        """f6-22: the layout slow and exit reason lines are single writes."""
        self.compile_run("logline",[HERE / "logline_test.c",ROOT / "sdk/netsurf/port/budget.c"],
                         ["-Wl,--wrap=clock_gettime,--wrap=write"])

    def test_real_frontend_input(self):
        objects=self.source / "build/Linux-framebuffer"
        excluded={"frontends_framebuffer_gui.o","frontends_framebuffer_ciuki_chrome.o","frontends_framebuffer_fbtk_event.o","content_handlers_html_layout.o","content_handlers_text_textplain.o","frontends_framebuffer_bitmap.o"}
        linked=[p for p in sorted(objects.glob("*.o")) if p.name not in excluded]
        libraries=["-Wl,--start-group",*[str(p) for p in sorted((self.base / "prefix/lib").glob("*.a")) if p.name!="libnsfb.a"],"-Wl,--end-group",
                   "-Wl,--whole-archive","-lnsfb","-Wl,--no-whole-archive","-lm","-lpthread",
                   "-Wl,--wrap=nsfb_event,--wrap=browser_window_mouse_click,--wrap=browser_window_access_url,--wrap=clock_gettime,--wrap=malloc,--wrap=calloc,--wrap=realloc",
                   "-Wl,--wrap=browser_window_navigate,--wrap=browser_window_back_available,--wrap=browser_window_forward_available,--wrap=browser_window_history_back,--wrap=browser_window_history_forward,--wrap=browser_window_reload,--wrap=browser_window_stop",
                   "-include",str(ROOT / "tests/host/desktop/replay_base.h"),"-Dnsframebuffer","-DCIUKI_HOST","-DCIUKI_NETSURF","-DWITH_NSLOG"]
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            page=Path(directory) / "interaction.html"
            page.write_text('<!doctype html><title>Input</title><a href="data:text/plain,clicked">link</a><div style="height:2400px">scroll</div>')
            self.compile_run("frontend-input",[HERE / "frontend_test.c",HERE / "layout_deadline_test.c",self.source / "frontends/framebuffer/bitmap.c",*linked],libraries,
                ["-w","640","-h","480","--ciuki-resources="+str(self.base / "stage/system/netsurf"),
                 "--ciuki-profile="+directory,page.as_uri()])

    def test_short_data_mime(self):
        self.compile_run("short-data",[HERE / "data_test.c"],["-lnsutils","-lwapcaplet"])

    def test_parser_admission_and_oom(self):
        """fx-14: synthetic ordinary pages (pages.py) are admitted by the
        binding under the 128 MiB baseline model, their DOM's measured live
        bytes stay within the ledger's charge; with 4 MiB free the same pages
        are refused for memory before allocating."""
        import pages
        dom=self.base / "source/libdom"
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            files=[]
            for name,make in pages.PAGES.items():
                files.append(Path(directory) / name);files[-1].write_text(make())
            oversized=Path(directory) / "oversized.html";oversized.write_text(pages.oversized())
            extra=["-iquote"+str(dom / "src"),"-I"+str(dom / "bindings/hubbub"),"-I"+str(dom / "src"),"-ldom","-lhubbub","-lparserutils","-lwapcaplet",
                   "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc"]
            self.compile_run("admission",[HERE / "admission_test.c"],extra,[*files,"--",oversized])
            # 2 MiB free: a 512 KiB DOM budget, below these pages' charge.
            self.compile_run("admission",[],extra,["--",*[f for f in files if f.name in ("news-front.html","article.html","forum-thread.html")]],
                             env={"CIUKI_NETSURF_FREE_BYTES":str(2*1024*1024)},build=False)

    def test_gif_memory_and_oom(self):
        gif=self.base / "source/libnsgif/src"
        self.compile_run("gif-memory",[HERE / "gif_memory_test.c",gif / "lzw.c"],[
            "-I"+str(gif),"-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc"],args=[HERE / "fixtures/colours.gif"])

if __name__=="__main__":unittest.main()
