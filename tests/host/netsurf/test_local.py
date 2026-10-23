"""The real framebuffer frontend on libnsfb RAM; no SDL or network."""
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))  # fx-14: pages.py
import pages  # noqa: E402
# f6-20 T23 model for the port's layout budget clock: host layout time is
# charged 32x, a conservative model of a 1.1 GHz Pentium III-M (i686 code,
# 512 KiB L2, PC133 SDRAM) against this host class (Ryzen 7 7800X3D):
# published single-thread integer ratios are about 13-25x; the factor rounds
# the upper end up for the T23's memory hierarchy. Rationale and measured
# margins: build/codex/f6-20-report.md.
T23_CLOCK_SCALE=32
LAYOUT_LIMIT_NS=75_000_000

def recipe():
    spec=importlib.util.spec_from_file_location("netsurf_recipe",ROOT / "sdk/netsurf/build.py")
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result)
    return result

class LocalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=recipe()
        cls.build.build(host=True)
        cls.base=ROOT / "build/external/netsurf-host"
        cls.out=ROOT / "build/host/netsurf"
        cls.out.mkdir(parents=True,exist_ok=True)

    def test_charset_and_font(self):
        source=self.base / "source/netsurf"
        prefix=self.base / "prefix"
        flags=["clang","-std=c17","-D_POSIX_C_SOURCE=200809L","-O1","-g","-Wall","-Wextra","-Wno-unused-parameter",
               "-fsanitize=address,undefined","-fno-sanitize-recover=undefined","-ffunction-sections","-fdata-sections",
               "-DCIUKI_NETSURF","-DCIUKI_HOST",'-DCIUKI_ROOT="'+str(ROOT)+'"',"-Dnsframebuffer",
               "-I"+str(ROOT),"-I"+str(source),"-I"+str(source / "include"),"-I"+str(source / "frontends"),
               "-I"+str(self.base / "native-headers"),"-I"+str(prefix / "include")]
        sources=[HERE / "charset_font_test.c",ROOT / "sdk/netsurf/port/charset.c",ROOT / "sdk/netsurf/port/font.c",ROOT / "sdk/netsurf/port/budget.c",source / "utils/utf8.c"]
        subprocess.run([*flags,*map(str,sources),"-Wl,--gc-sections,--wrap=ciuki_font_measure","-L"+str(prefix / "lib"),"-lparserutils","-lfont","-lfreetype",
                        "-Wl,--whole-archive","-lnsfb","-Wl,--no-whole-archive","-ldeskui","-lm","-lpthread","-o",str(self.out / "charset-font")],check=True)
        subprocess.run([str(self.out / "charset-font")],check=True,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=1",UBSAN_OPTIONS="halt_on_error=1"),timeout=30)

    def test_render_authored_pages(self):
        pins=json.loads((HERE / "goldens.json").read_text())
        for name in ("layout","images","table-form"):
            with self.subTest(page=name):
                out=self.out / (name+".xrgb")
                args=[str(self.base / "stage/Programs/netsurf"),"-w","640","-h","480","-f","ram",
                      "--ciuki-resources="+str(self.base / "stage/system/netsurf"),"--ciuki-capture="+str(out),
                      (HERE / "fixtures" / (name+".html")).as_uri()]
                completed=subprocess.run(args,capture_output=True,text=True,timeout=30)
                self.assertEqual(completed.returncode,0,completed.stdout+completed.stderr)
                raw=out.read_bytes();self.assertEqual(len(raw),640*480*4)
                self.assertEqual(hashlib.sha256(raw).hexdigest(),pins[name])
                metric=re.search(r"layout_max_ns=(\d+)",completed.stderr)
                self.assertIsNotNone(metric,completed.stderr)
                self.assertGreater(int(metric[1]),0)
                self.assertLessEqual(int(metric[1]),100_000_000)
                print(name,metric[0])
                # These goldens demonstrate supported features, not standards conformance.
                self.assertGreater(len(set(raw)),100)
                out.unlink()

    def test_streaming_file(self):
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            page=Path(directory) / "stream.html"
            page.write_text("<!doctype html><title>Stream</title>"+"<!-- bounded local stream -->"*4096+"<h1>Ciuki</h1>")
            out=Path(directory) / "stream.xrgb"
            completed=subprocess.run([str(self.base / "stage/Programs/netsurf"),"-w","640","-h","480",
                 "--ciuki-resources="+str(self.base / "stage/system/netsurf"),"--ciuki-capture="+str(out),page.as_uri()],capture_output=True,text=True,timeout=30)
            self.assertEqual(completed.returncode,0,completed.stdout+completed.stderr)
            metric=re.search(r"file_slices=(\d+)",completed.stderr)
            self.assertIsNotNone(metric,completed.stderr)
            self.assertGreater(int(metric[1]),4)
            self.assertEqual(out.stat().st_size,640*480*4)

    def test_data_and_about(self):
        for name,url in (("data","data:text/html;charset=windows-1252,%3Ctitle%3ELocal%3C/title%3E%3Ch1%3ECiuki%20%80%3C/h1%3E"),
                         ("about","about:testament")):
            out=self.out / (name+".xrgb")
            completed=subprocess.run([str(self.base / "stage/Programs/netsurf"),"-w","640","-h","480",
                 "--ciuki-resources="+str(self.base / "stage/system/netsurf"),"--ciuki-capture="+str(out),url],capture_output=True,text=True,timeout=30)
            self.assertEqual(completed.returncode,0,completed.stdout+completed.stderr)
            self.assertEqual(out.stat().st_size,640*480*4);out.unlink()

    def test_about_pages(self):
        for name in ("about","welcome","credits","licence","config","Choices","testament","blank","imagecache"):
            with self.subTest(page=name), tempfile.TemporaryDirectory(dir=self.out) as directory:
                out=Path(directory) / "about.xrgb"
                completed=subprocess.run([str(self.base / "stage/Programs/netsurf"),"-w","640","-h","480",
                    "--ciuki-resources="+str(self.base / "stage/system/netsurf"),"--ciuki-capture="+str(out),"about:"+name],
                    capture_output=True,text=True,timeout=15)
                self.assertEqual(completed.returncode,0,completed.stdout+completed.stderr)
                self.assertEqual(out.stat().st_size,640*480*4)

    def run_page(self,url,*extra,width=640,height=480,timeout=30):
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            out=Path(directory) / "page.xrgb"
            completed=subprocess.run([str(self.base / "stage/Programs/netsurf"),"-w",str(width),"-h",str(height),
                "--ciuki-resources="+str(self.base / "stage/system/netsurf"),"--ciuki-capture="+str(out),*extra,url],
                capture_output=True,text=True,timeout=timeout)
            self.assertEqual(completed.returncode,0,completed.stdout+completed.stderr)
            self.assertEqual(out.stat().st_size,width*height*4)
        return completed

    def assert_exit_reason(self,completed,reason):
        """One exit-reason line, before netsurf_exit's lwc leak report."""
        lines=completed.stderr.splitlines()
        reasons=[i for i,line in enumerate(lines) if line.startswith("[netsurf] exit reason=")]
        self.assertEqual(len(reasons),1,completed.stderr)
        self.assertTrue(lines[reasons[0]].startswith(f"[netsurf] exit reason={reason} "),lines[reasons[0]])
        leaks=[i for i,line in enumerate(lines) if "netsurf_lwc_iterator" in line]
        self.assertTrue(all(i>reasons[0] for i in leaks),completed.stderr)
        return lines[reasons[0]]

    def test_welcome_t23_clock(self):
        """f6-20: about:welcome lays out under the T23 budget clock; local
        content is never rejected by the time limit, however slow."""
        samples=[]
        for _ in range(3):
            # The T23 window's content area: 800x542 surface minus chrome.
            completed=self.run_page("about:welcome","--ciuki-clock-scale="+str(T23_CLOCK_SCALE),width=800,height=484)
            self.assertNotIn("rejected",completed.stderr)
            self.assertIn("NetSurf local title: Welcome to NetSurf",completed.stderr)
            metrics=re.search(r"local_max_ns=(\d+) remote_max_ns=(\d+) clock_scale=(\d+)",completed.stderr)
            self.assertIsNotNone(metrics,completed.stderr)
            self.assertEqual((int(metrics[2]),int(metrics[3])),(0,T23_CLOCK_SCALE))
            self.assertGreater(int(metrics[1]),0)
            work=re.search(r"layout work elapsed_us=(\d+) class=local pass=1 font_opens=(\d+) font_open_us=(\d+) font_measures=(\d+) font_measure_us=(\d+) width_hits=(\d+) measured_bytes=(\d+)",completed.stderr)
            self.assertIsNotNone(work,completed.stderr)
            self.assertGreater(int(work[2]),0)
            self.assertGreater(int(work[4]),0)
            self.assertGreater(int(work[6]),0)
            self.assertGreater(int(work[7]),0)
            print(work[0],flush=True)
            line=self.assert_exit_reason(completed,"capture")
            self.assertIn("layout_rejections=0",line)
            samples.append(int(metrics[1]))
        best=min(samples)
        print(f"welcome layout at T23 scale {T23_CLOCK_SCALE}: {sorted(samples)} ns; best {best/1e6:.1f} ms = "
              f"{LAYOUT_LIMIT_NS/best:.1f}x under the {LAYOUT_LIMIT_NS//1_000_000} ms remote budget; "
              f"break-even scale {LAYOUT_LIMIT_NS*T23_CLOCK_SCALE//best}",flush=True)
        # Margin even against the remote budget (min of three: robust to host load).
        self.assertLess(best,LAYOUT_LIMIT_NS)
        # f6-23 requirement: the inherited host CPU model must also remain
        # below 300 ms. This excludes native font I/O/mutex latency; the port's
        # layout-work record is required before making a physical T23 claim.
        self.assertLess(max(samples),300_000_000)
        # Deterministic exemption: charged far beyond the remote budget, the
        # welcome page and a local file still lay out and the browser exits normally.
        for url in ("about:welcome",(HERE / "fixtures/layout.html").as_uri()):
            with self.subTest(url=url):
                completed=self.run_page(url,"--ciuki-clock-scale=1000000")
                self.assertNotIn("rejected",completed.stderr)
                self.assertGreater(int(re.search(r"local_max_ns=(\d+)",completed.stderr)[1]),LAYOUT_LIMIT_NS)
                self.assertIn("layout_rejections=0",self.assert_exit_reason(completed,"capture"))

    def test_netsurf_log(self):
        """f6-22: every exit appends one stamped line to NETSURF.LOG (the
        stderr exit line plus the network state); the file restarts past
        64 KiB; an early failure is logged although stderr has no exit line."""
        stamp=r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ "
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            log=Path(directory) / "NETSURF.LOG"
            for run in range(2):
                completed=self.run_page("about:welcome","--ciuki-log="+str(log))
                line=self.assert_exit_reason(completed,"capture")
                self.assertTrue(line.endswith(" network=unknown"),line)
                lines=log.read_text().splitlines()
                self.assertEqual(len(lines),run+1,lines)
                self.assertRegex(lines[-1],"^"+stamp+re.escape(line)+"$")
            log.write_text("x"*(64*1024)+"\n")
            self.run_page("about:blank","--ciuki-log="+str(log))
            lines=log.read_text().splitlines()
            self.assertEqual(len(lines),1,lines[:2])
            self.assertRegex(lines[0],"^"+stamp+"\\[netsurf\\] exit reason=capture ")
            log.unlink()
            completed=subprocess.run([str(self.base / "stage/Programs/netsurf"),"--ciuki-log="+str(log),"--channel-fd=1","about:blank"],
                capture_output=True,text=True,timeout=30)
            self.assertEqual(completed.returncode,2,completed.stdout+completed.stderr)
            self.assertNotIn("[netsurf] exit reason=",completed.stderr)
            self.assertRegex(log.read_text(),"^"+stamp+"\\[netsurf\\] exit reason=bad-arguments detail=0 .* network=unknown\n$")

    def run_file(self,directory,name,body,env=None,limit_stack=False):
        """Lay out one local page; the capture ends the run (f6-20)."""
        page=Path(directory) / name;page.write_text(body)
        out=Path(directory) / (name+".xrgb")
        def stack():
            import resource
            resource.setrlimit(resource.RLIMIT_STACK,(1024*1024,resource.getrlimit(resource.RLIMIT_STACK)[1]))
        completed=subprocess.run([str(self.base / "stage/Programs/netsurf"),"-w","800","-h","600",
            "--ciuki-resources="+str(self.base / "stage/system/netsurf"),"--ciuki-profile="+str(Path(directory) / "profile"),
            "--ciuki-capture="+str(out),page.as_uri()],capture_output=True,text=True,timeout=60,
            env=dict(os.environ,**(env or {})),preexec_fn=stack if limit_stack else None)
        self.assertEqual(completed.returncode,0,completed.stdout+completed.stderr)
        self.assertEqual(out.stat().st_size,800*600*4)
        self.assert_exit_reason(completed,"capture")
        return page,completed

    def test_ordinary_pages_load(self):
        """fx-14: pages shaped like a search engine home page (more than 48 KB
        of inline script), news front pages (about 9,000 and 20,000 DOM nodes
        with a 60 KB JSON script), a 300 KB encyclopedia article, a forum
        thread and constructs the f6-14 constants refused (a 20 KB unbroken run,
        10,000 paragraphs, colspan 5000, 150 KB of plain text) load under the
        128 MiB baseline model. Host layout times x32 model the T23."""
        cases=[(name,make(),"NetSurf local title: "+title) for (name,make),title in zip(pages.PAGES.items(),pages.TITLES)]
        cases+=[("run.html","<!doctype html><title>Run</title><p>"+"x"*20000+"</p>","NetSurf local title: Run"),
                ("wide.html","<title>Wide</title>"+"<p>x</p>"*10000,"NetSurf local title: Wide"),
                ("colspan.html","<title>Colspan</title><table><tr><td colspan=5000>x</td></tr><tr><td>a</td><td>b</td></tr></table>","NetSurf local title: Colspan"),
                ("large.txt","x"*150000+"\n"+"plain text line\n"*4000,None)]
        for name,body,title in cases:
            with self.subTest(page=name),tempfile.TemporaryDirectory(dir=self.out) as directory:
                page,completed=self.run_file(directory,name,body)
                self.assertNotIn("rejected",completed.stderr)
                self.assertNotIn("NetSurf error page:",completed.stderr)
                if title:self.assertIn(title,completed.stderr)
                self.assertIn("layout_rejections=0",completed.stderr)
                self.assertIn(page.as_uri(),(Path(directory) / "profile/History.html").read_text())
                layout=int(re.search(r"layout_max_ns=(\d+)",completed.stderr)[1])
                print(f"{name}: {len(body)} bytes, host layout {layout/1e6:.1f} ms, T23 model {layout*T23_CLOCK_SCALE/1e9:.2f} s",flush=True)
                self.assertLess(layout*T23_CLOCK_SCALE,10_000_000_000)  # fx-11 top-level document limit

    def test_deep_pages_within_main_stack(self):
        """fx-14: nesting just inside the new DOM depth (128) lays out and
        paints with the main stack limited to CiukiOS's 1 MiB."""
        cases={"divs.html":pages.nested(124),
               "tables.html":"<title>Tables</title>"+"<table><tr><td>"*30+"cell"+"</td></tr></table>"*30,
               "inline.html":"<title>Inline</title>"+"<span style='display:inline-block;border:1px solid'>"*120+"x"+"</span>"*120}
        for name,body in cases.items():
            with self.subTest(page=name),tempfile.TemporaryDirectory(dir=self.out) as directory:
                _,completed=self.run_file(directory,name,body,limit_stack=True)
                self.assertNotIn("rejected",completed.stderr)

    def test_admission_rejections(self):
        """fx-14: only an exceeded memory budget, nesting beyond 128 levels or
        a table wider than 1000 columns refuse a page, each with an error page
        that says why; the browser stays on it."""
        memory="This page is too large for the memory available to the browser."
        cases=[("oversized.html",pages.oversized(),None,"page too large for available memory",memory),
               ("budget.html",pages.news_front(),{"CIUKI_NETSURF_FREE_BYTES":str(4*1024*1024)},"page too large for available memory",memory),
               ("deep.html","<div>"*20000+"x"+"</div>"*20000,None,"nesting depth","This page nests its elements more deeply than the browser supports."),
               ("columns.html","<table><tr><td colspan=600>a</td><td colspan=600>b</td></tr></table>",None,"table columns","This page has a table with more columns than the browser supports."),
               ("large.txt","plain text line\n"*20000,{"CIUKI_NETSURF_FREE_BYTES":str(1024*1024)},"page too large for available memory",memory)]
        for name,body,env,cause,message in cases:
            with self.subTest(page=name),tempfile.TemporaryDirectory(dir=self.out) as directory:
                page,completed=self.run_file(directory,name,body,env=env)
                self.assertIn("NetSurf structure rejected: "+cause,completed.stderr)
                self.assertIn("NetSurf error page: "+message,completed.stderr)
                self.assertIn("NetSurf local title: Error occurred fetching page",completed.stderr)
                self.assertNotIn(page.as_uri(),(Path(directory) / "profile/History.html").read_text())
                if name=="large.txt":
                    self.assertIn("NetSurf layout rejected: plain text, local, admission limit",completed.stderr)

    def test_completed_history_restart_and_page(self):
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            folder=Path(directory)
            page=folder / "local.html";page.write_text("<!doctype html><title>Visit</title><h1>Local history</h1>")
            urls=[page.as_uri(),page.as_uri(),"data:text/html,<title>Second</title>second"]
            def visit(url):
                out=folder / "history.xrgb"
                completed=subprocess.run([str(self.base / "stage/Programs/netsurf"),"-w","640","-h","480",
                    "--ciuki-resources="+str(self.base / "stage/system/netsurf"),"--ciuki-profile="+directory,
                    "--ciuki-capture="+str(out),url],capture_output=True,text=True,timeout=15)
                self.assertEqual(completed.returncode,0,completed.stdout+completed.stderr)
                self.assertEqual(out.stat().st_size,640*480*4)
                return completed
            for url in urls:visit(url)
            saved=(folder / "History.html").read_text()
            self.assertEqual(saved.count('<p><a href="'),2)
            self.assertIn(page.as_uri(),saved)
            result=visit((folder / "History.html").as_uri())
            self.assertIn("NetSurf History",result.stderr)

class ProfileTests(unittest.TestCase):
    def test_profile(self):
        out=ROOT / "build/host/netsurf";out.mkdir(parents=True,exist_ok=True)
        subprocess.run(["clang","-std=c17","-D_POSIX_C_SOURCE=200809L","-O1","-g","-Wall","-Wextra","-Werror",
             "-fsanitize=address,undefined","-fno-sanitize-recover=undefined","-I"+str(ROOT),
             str(ROOT / "sdk/netsurf/port/profile.c"),str(HERE / "profile_test.c"),"-o",str(out / "profile")],check=True)
        with tempfile.TemporaryDirectory(dir=out) as profile:
            subprocess.run([str(out / "profile"),profile],check=True,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=1",UBSAN_OPTIONS="halt_on_error=1"),timeout=30)

class ReproducibilityTests(unittest.TestCase):
    def test_two_clean_target_builds(self):
        build=recipe()
        records=[];libraries=[]
        for suffix in ("a","b"):
            directory=ROOT / ("build/external/netsurf-repro-"+suffix)
            records.append(build.build(clean=True,directory=directory))
            libraries.append({p.name:build.digest(p) for p in sorted((directory / "prefix/lib").glob("*.a"))})
        self.assertEqual(records[0]["files"],records[1]["files"])
        self.assertEqual(libraries[0],libraries[1])
        out=ROOT / "build/codex";out.mkdir(parents=True,exist_ok=True)
        (out / "f6-14-repro.json").write_text(json.dumps({"files":records[0]["files"],"libraries":libraries[0],
             "sdk_manifest_sha256":records[0]["inputs"]["sdk"],"result":"two clean builds identical"},indent=2,sort_keys=True)+"\n")

if __name__=="__main__":
    unittest.main()
