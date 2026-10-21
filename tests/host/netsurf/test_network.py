"""M2 fixtures. Run in this directory, sequentially with nice -n 19."""
import importlib.util
import os
import runpy
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
OUT=ROOT / "build/host/netsurf/m2"
BASE=ROOT / "build/external/netsurf-host"
SAN=dict(os.environ,ASAN_OPTIONS="detect_leaks=1",UBSAN_OPTIONS="halt_on_error=1")

def recipe():
    spec=importlib.util.spec_from_file_location("netsurf_m2",ROOT / "sdk/netsurf/build.py")
    build=importlib.util.module_from_spec(spec);spec.loader.exec_module(build)
    return build

def openssl(*args):
    result=subprocess.run(["openssl",*map(str,args)],capture_output=True,text=True)
    if result.returncode:raise RuntimeError(result.stderr)

def certificates(folder):
    openssl("req","-x509","-newkey","ec","-pkeyopt","ec_paramgen_curve:P-256","-nodes","-sha256",
        "-keyout",folder / "ca.key","-out",folder / "ca.pem","-subj","/CN=Ciuki M2 Fixture CA","-days","36500")
    # Fixed validation time for the client, independent of host date.
    (folder / "index").write_text("");(folder / "serial").write_text("01\n")
    for name,host,start,end in (("valid","fixture.example","20260101000000Z","20350101000000Z"),
        ("wrong","other.example","20260101000000Z","20350101000000Z"),
        ("expired","fixture.example","20200101000000Z","20210101000000Z"),
        ("future","fixture.example","20350101000000Z","20360101000000Z")):
        openssl("req","-new","-newkey","ec","-pkeyopt","ec_paramgen_curve:P-256","-nodes",
            "-keyout",folder / (name+".key"),"-out",folder / (name+".csr"),"-subj","/CN="+host)
        config=folder / "ca.conf"
        config.write_text(f"""[ca]
default_ca=fixture
[fixture]
database={folder / 'index'}
unique_subject=no
serial={folder / 'serial'}
new_certs_dir={folder}
certificate={folder / 'ca.pem'}
private_key={folder / 'ca.key'}
default_md=sha256
policy=policy
x509_extensions=leaf
[policy]
commonName=supplied
[leaf]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=DNS:{host}
""")
        openssl("ca","-batch","-config",config,"-startdate",start,"-enddate",end,
            "-in",folder / (name+".csr"),"-out",folder / (name+".pem"))
    openssl("req","-x509","-newkey","ec","-pkeyopt","ec_paramgen_curve:P-256","-nodes",
        "-keyout",folder / "self.key","-out",folder / "self.pem","-subj","/CN=fixture.example","-days","36500",
        "-addext","subjectAltName=DNS:fixture.example")
    for name in ("valid","wrong","expired","future","self"):
        openssl("x509","-in",folder / (name+".pem"),"-outform","DER","-out",folder / (name+".der"))
        openssl("pkey","-in",folder / (name+".key"),"-outform","DER","-out",folder / (name+"-key.der"))

class NetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build=recipe();build.build(host=True)
        OUT.mkdir(parents=True,exist_ok=True)
        cls.fixture=tempfile.TemporaryDirectory(dir=OUT);cls.folder=Path(cls.fixture.name)
        cls.addClassCleanup(cls.fixture.cleanup)
        certificates(cls.folder)
        with (OUT / "fixture-anchors.h").open("w") as stream:
            subprocess.run([str(BASE / "ca-generator"),str(cls.folder / "ca.pem")],check=True,stdout=stream)
        cls.flags=["clang","-std=c17","-D_DEFAULT_SOURCE","-D_POSIX_C_SOURCE=200809L","-O1","-g","-Wall","-Wextra","-Werror",
            "-fsanitize=address,undefined","-fno-sanitize-recover=undefined","-fno-omit-frame-pointer",
            "-I"+str(ROOT),"-I"+str(OUT),"-I"+str(BASE / "prefix/include"),
            "-I"+str(BASE / "source/netsurf/frontends/framebuffer/ciuki")]
        cls.libs=[str(BASE / "prefix/lib/libbearssl.a"),str(BASE / "prefix/lib/libz.a"),"-pthread"]
        def compile(name,sources,extra=()):
            subprocess.run([*cls.flags,*map(str,sources),*cls.libs,*extra,"-o",str(OUT / name)],check=True)
        compile("http",[HERE / "http_test.c",ROOT / "sdk/netsurf/port/http.c"])
        compile("dns",[HERE / "dns_test.c",ROOT / "sdk/netsurf/port/dns.c"],
            ["-Wl,--wrap=getaddrinfo,--wrap=freeaddrinfo,--wrap=calloc,--wrap=free,--wrap=pthread_create"])
        cls.wraps=["-Wl,"+",".join("--wrap="+n for n in ("socket","connect","send","recv","close","fcntl","getsockopt","poll","getaddrinfo","freeaddrinfo"))]
        cls.stack=cls.compile_stack()
        compile("network",[HERE / "network_test.c",*[ROOT / ("sdk/netsurf/port/"+s+".c") for s in ("http","net","tls","dns")],*cls.stack],
            [*cls.wraps,"-Wl,--wrap=clock_gettime,--wrap=getentropy"])
        source=BASE / "source/netsurf"
        objects=[p for p in sorted((source / "build/Linux-framebuffer").glob("*.o"))]
        libraries=["-Wl,--start-group",*[str(p) for p in sorted((BASE / "prefix/lib").glob("*.a")) if p.name!="libnsfb.a"],"-Wl,--end-group",
                   "-Wl,--whole-archive","-lnsfb","-Wl,--no-whole-archive","-lm","-pthread"]
        subprocess.run([*cls.flags,str(HERE / "browser_network_test.c"),*map(str,objects),*cls.stack,
            "-L"+str(BASE / "prefix/lib"),*libraries,*cls.wraps,"-Wl,--wrap=main","-o",str(OUT / "browser")],check=True)

    @classmethod
    def compile_stack(cls):
        port=ROOT / "src/kernel/net/lwip"
        build=runpy.run_path(str(port / "build.py"));source=build["source"]()
        flags=[*cls.flags,"-DLWIP_HOST","-pthread","-Wno-macro-redefined","-Wno-unused-function","-Wno-unused-parameter",
            "-ffunction-sections","-fdata-sections",*build["includes"](source),"-I"+str(ROOT / "src/kernel/include")]
        files=[source / "src" / name for name in build["SOURCES"]]+[port / name for name in ("sys_arch.c","stack.c","sockets.c","syscalls.c")]
        files += [HERE / "stack_fixture.c",ROOT / "tests/host/lwip/channel_fixture.c"]
        objects=[]
        for i,path in enumerate(files):
            obj=OUT / ("stack-"+str(i)+".o")
            subprocess.run([*flags,"-c",str(path),"-o",str(obj)],check=True)
            objects.append(str(obj))
        return [*objects,"-Wl,--gc-sections"]

    @classmethod
    def tearDownClass(cls):cls.fixture.cleanup()

    def engine(self,mode,path="/"):
        secure=mode in ("valid","expired","future","wrong","self","tls10")
        cert="valid" if mode=="tls10" else mode
        args=[str(self.folder / (cert+".der")),str(self.folder / (cert+"-key.der"))] if secure else ["-","-"]
        try:
            result=subprocess.run([str(OUT / "network"),mode,"8443" if secure else "9" if mode=="refused" else "8080",path,*args],text=True,capture_output=True,env=SAN,timeout=25)
        except subprocess.TimeoutExpired as error:
            print(error.stdout,error.stderr,flush=True)
            raise
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        print(result.stdout.strip())

    def test_parser(self):subprocess.run([str(OUT / "http")],check=True,env=SAN,timeout=20)
    def test_dns_worker(self):subprocess.run([str(OUT / "dns")],check=True,env=SAN,timeout=10)

    def test_http_engine(self):
        for mode,path in (("ok","/"),("reuse","/"),("post","/"),("ok","/chunked"),("ok","/gzip"),("ok","/deflate"),
            ("stale","/stale"),("post-stale","/stale"),("truncated","/truncated"),("limit","/large-header"),
            ("timeout","/slow"),("callback-cancel","/slow"),("cancel","/slow")):
            with self.subTest(mode=mode,path=path):self.engine(mode,path)
        for mode in ("dns","offline","dated","entropy","numeric","refused","pool"):
            with self.subTest(mode=mode):self.engine(mode)

    def test_bearssl(self):
        for mode in ("valid","expired","future","wrong","self","tls10"):
            with self.subTest(mode=mode):self.engine(mode)

    def browser(self,path):
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            capture=Path(folder) / "page.xrgb"
            result=subprocess.run([str(OUT / "browser"),"-w","640","-h","480",
                "--ciuki-resources="+str(BASE / "stage/system/netsurf"),"--ciuki-profile="+folder,
                "--ciuki-capture="+str(capture),f"http://192.0.2.1:8080{path}"],capture_output=True,text=True,timeout=25,env=SAN)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(capture.stat().st_size,640*480*4)
            return result

    def test_browser_redirect_cookies_and_errors(self):
        self.browser("/redirect")
        result=self.browser("/cookie-cap/0")
        self.assertIn("fixture_cookie_retention=200",result.stdout)
        result=self.browser("/loop")
        self.assertIn("fixture_loop_requests=6",result.stdout)
        self.browser("/truncated")

if __name__=="__main__":unittest.main()
