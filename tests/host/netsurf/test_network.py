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
    openssl("req","-new","-newkey","ec","-pkeyopt","ec_paramgen_curve:P-256","-nodes",
        "-keyout",folder / "intermediate-ca.key","-out",folder / "intermediate-ca.csr","-subj","/CN=Ciuki Intermediate")
    (folder / "intermediate.ext").write_text("basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n")
    config=folder / "ca.conf"
    config.write_text(config.read_text()+"\n[intermediate]\n"+(folder / "intermediate.ext").read_text())
    openssl("ca","-batch","-config",config,"-extensions","intermediate","-startdate","20260101000000Z","-enddate","20350101000000Z",
        "-in",folder / "intermediate-ca.csr","-out",folder / "intermediate-ca.pem")
    openssl("req","-new","-newkey","ec","-pkeyopt","ec_paramgen_curve:P-256","-nodes",
        "-keyout",folder / "intermediate.key","-out",folder / "intermediate.csr","-subj","/CN=fixture.example")
    (folder / "leaf.ext").write_text("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:fixture.example\n")
    config.write_text(config.read_text().replace(str(folder / "ca.pem"),str(folder / "intermediate-ca.pem")).replace(str(folder / "ca.key"),str(folder / "intermediate-ca.key")))
    openssl("ca","-batch","-config",config,"-extensions","leaf","-startdate","20260101000000Z","-enddate","20350101000000Z",
        "-in",folder / "intermediate.csr","-out",folder / "intermediate.pem")
    openssl("x509","-in",folder / "intermediate-ca.pem","-outform","DER","-out",folder / "intermediate-chain.der")
    for name in ("valid","wrong","expired","future","self","intermediate"):
        openssl("x509","-in",folder / (name+".pem"),"-outform","DER","-out",folder / (name+".der"))
        openssl("pkey","-in",folder / (name+".key"),"-outform","DER","-out",folder / (name+"-key.der"))

class AnchorTests(unittest.TestCase):
    def test_production_rsa_ec_anchors(self):
        """Independent DER/SPKI extraction against every generated production root."""
        import base64
        import re
        recipe().build(host=True)
        generated=(BASE / "source/netsurf/frontends/framebuffer/ciuki/anchors.h").read_text()
        arrays={name:bytes(map(int,values.split(","))) for name,values in
            re.findall(r"static const unsigned char (ca_\d+_\w+)\[\] = \{([0-9,]+)\};",generated)}
        def elements(data):
            result=[];offset=0
            while offset<len(data):
                begin=offset;tag=data[offset];length=data[offset+1];offset+=2
                if length&128:
                    count=length&127;self.assertGreater(count,0)
                    length=int.from_bytes(data[offset:offset+count],"big");offset+=count
                end=offset+length;self.assertLessEqual(end,len(data))
                result.append((tag,data[offset:end],data[begin:end]));offset=end
            return result
        roots=re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----",
            (ROOT / "third_party/cacert/CACERT.PEM").read_text(),re.S)
        types=set()
        for i,pem in enumerate(roots):
            der=base64.b64decode(pem);tbs=elements(elements(der)[0][1])[0][1];fields=elements(tbs)
            version=1 if fields[0][0]==0xa0 else 0
            subject=fields[version+4][2];spki=fields[version+5][2]
            # OpenSSL independently re-encodes each SubjectPublicKeyInfo.
            cert=subprocess.run(["openssl","x509","-inform","DER","-pubkey","-noout"],input=der,capture_output=True,check=True)
            public=subprocess.run(["openssl","pkey","-pubin","-outform","DER"],input=cert.stdout,capture_output=True,check=True)
            self.assertEqual(spki,public.stdout)
            self.assertEqual(arrays[f"ca_{i}_dn"],subject)
            key=elements(elements(spki)[0][1]);algorithm=elements(key[0][1]);bits=key[1][1]
            self.assertEqual(bits[0],0)
            if algorithm[0][1]==bytes.fromhex("2a864886f70d010101"):
                types.add("RSA");numbers=elements(elements(bits[1:])[0][1]);n=numbers[0][1].lstrip(b"\0");e=numbers[1][1].lstrip(b"\0")
                self.assertEqual(arrays[f"ca_{i}_n"],n);self.assertEqual(arrays[f"ca_{i}_e"],e)
                entry=f"{{{{(unsigned char *)ca_{i}_dn,{len(subject)}}},BR_X509_TA_CA,{{1,{{.rsa={{(unsigned char *)ca_{i}_n,{len(n)},(unsigned char *)ca_{i}_e,{len(e)}}}}}}}}},"
            else:
                self.assertEqual(algorithm[0][1],bytes.fromhex("2a8648ce3d0201"));types.add("EC")
                curve={"2a8648ce3d030107":23,"2b81040022":24,"2b81040023":25}[algorithm[1][1].hex()]
                self.assertEqual(arrays[f"ca_{i}_q"],bits[1:])
                entry=f"{{{{(unsigned char *)ca_{i}_dn,{len(subject)}}},BR_X509_TA_CA,{{2,{{.ec={{{curve},(unsigned char *)ca_{i}_q,{len(bits)-1}}}}}}}}},"
            self.assertIn(entry,generated)
        self.assertEqual(types,{"RSA","EC"})
        self.assertIn(f"#define CIUKI_ANCHOR_COUNT {len(roots)}",generated)
        self.assertEqual(len(arrays),sum(3 if f"ca_{i}_n" in arrays else 2 for i in range(len(roots))))
        print(f"PASS independent OpenSSL/DER production anchor equivalence: {len(roots)} RSA/EC roots")

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
        cls.port_flags=["-DCIUKI_HOST","-include",str(ROOT / "sdk/netsurf/port/host_network.h"),
            "-I"+str(BASE / "native-headers"),"-I"+str(BASE / "source/netsurf"),
            "-I"+str(BASE / "source/netsurf/include"),"-I"+str(BASE / "source/netsurf/frontends")]
        cls.libs=[str(BASE / "prefix/lib/libbearssl.a"),str(BASE / "prefix/lib/libz.a"),"-pthread"]
        def compile(name,sources,extra=()):
            subprocess.run([*cls.flags,*(cls.port_flags if name in ("network","tls-limits") else []),*map(str,sources),*cls.libs,*extra,"-o",str(OUT / name)],check=True)
        compile("http",[HERE / "http_test.c",ROOT / "sdk/netsurf/port/http.c"])
        compile("tls-limits",[HERE / "tls_limits_test.c"])
        compile("dns",[HERE / "dns_test.c",ROOT / "sdk/netsurf/port/dns.c"],
            ["-Wl,--wrap=getaddrinfo,--wrap=freeaddrinfo,--wrap=calloc,--wrap=free,--wrap=pthread_create"])
        cls.wraps=["-Wl,"+",".join("--wrap="+n for n in ("socket","connect","send","recv","close","fcntl","getsockopt","poll","getaddrinfo","freeaddrinfo","open"))]
        cls.stack=cls.compile_stack()
        compile("network",[HERE / "network_test.c",*[ROOT / ("sdk/netsurf/port/"+s+".c") for s in ("http","net","tls","dns")],*cls.stack],
            [*cls.wraps,"-Wl,--wrap=clock_gettime,--wrap=getentropy"])
        source=BASE / "source/netsurf"
        objects=[p for p in sorted((source / "build/Linux-framebuffer").glob("*.o"))]
        libraries=["-Wl,--start-group",*[str(p) for p in sorted((BASE / "prefix/lib").glob("*.a")) if p.name!="libnsfb.a"],"-Wl,--end-group",
                   "-Wl,--whole-archive","-lnsfb","-Wl,--no-whole-archive","-lm","-pthread"]
        subprocess.run([*cls.flags,*cls.port_flags,str(HERE / "browser_network_test.c"),*map(str,objects),*cls.stack,
            "-L"+str(BASE / "prefix/lib"),*libraries,*cls.wraps,"-Wl,--wrap=main,--wrap=nsfb_event,--wrap=clock_gettime,--wrap=pthread_cond_wait","-o",str(OUT / "browser")],check=True)

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
        secure=mode.startswith("tls-") or mode in ("valid","expired","future","wrong","self","tls10","intermediate","missing-intermediate","malformed-chain","cert-depth","cert-buffer")
        cert="intermediate" if mode in ("intermediate","missing-intermediate","malformed-chain") else mode if mode in ("valid","expired","future","wrong","self") else "valid"
        if mode=="malformed-chain":
            (self.folder / "intermediate-chain.der").write_bytes(b"\x30\x03\x02\x01")
        elif mode=="intermediate":
            openssl("x509","-in",self.folder / "intermediate-ca.pem","-outform","DER","-out",self.folder / "intermediate-chain.der")
        args=[str(self.folder / (cert+".der")),str(self.folder / (cert+"-key.der"))] if secure else ["-","-"]
        try:
            result=subprocess.run([str(OUT / "network"),mode,"8443" if secure else "9" if mode=="refused" else "8080",path,*args],text=True,capture_output=True,env=SAN,timeout=25)
        except subprocess.TimeoutExpired as error:
            print(error.stdout,error.stderr,flush=True)
            raise
        print(result.stdout.strip(),flush=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_parser(self):subprocess.run([str(OUT / "http")],check=True,env=SAN,timeout=20)
    def test_certificate_capture_limits(self):subprocess.run([str(OUT / "tls-limits")],check=True,env=SAN,timeout=10)
    def test_dns_worker(self):subprocess.run([str(OUT / "dns")],check=True,env=SAN,timeout=10)

    def test_http_engine(self):
        for mode,path in (("ok","/"),("reuse","/"),("post","/"),("ok","/chunked"),("ok","/gzip"),("ok","/deflate"),
            ("stale","/stale"),("post-stale","/stale"),("truncated","/truncated"),("limit","/large-header"),
            ("timeout","/slow"),("callback-cancel","/slow"),("cancel","/slow")):
            with self.subTest(mode=mode,path=path):self.engine(mode,path)
        for mode in ("dns","offline","hostname-offline","loopback-offline","dated","fallback-clock","entropy","numeric","refused","pool","fanout","dns-cache","idle-expiry","idle-reuse-expiry","unsolicited","unsolicited-recheck"):
            with self.subTest(mode=mode):self.engine(mode,"/unsolicited" if mode=="unsolicited" else "/")

    def test_bearssl(self):
        for mode in ("valid","expired","future","wrong","self","tls10","intermediate","missing-intermediate","malformed-chain","cert-depth","cert-buffer","tls-reuse","tls-truncated","tls-unsolicited","tls-buffered","tls-recheck"):
            with self.subTest(mode=mode):self.engine(mode,"/tls-truncated" if mode=="tls-truncated" else "/unsolicited" if mode in ("tls-unsolicited","tls-buffered") else "/")

    def test_tls_first_transmission_and_partial_records(self):
        for mode in ("tls-submit-recheck","tls-send-recheck","tls-partial-header","tls-partial-body"):
            with self.subTest(mode=mode):self.engine(mode)

    def test_admitted_transport_retry_and_no_replay(self):
        for mode in ("stale-retry","post-no-replay","tls-stale-retry","tls-post-no-replay"):
            with self.subTest(mode=mode):self.engine(mode)

    def browser(self,path,mode="browser",scheme="http",host="fixture.example",error=None,expected=None):
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            capture=Path(folder) / "page.xrgb"
            result=subprocess.run([str(OUT / "browser"),"-w","640","-h","480",
                "--ciuki-resources="+str(BASE / "stage/system/netsurf"),"--ciuki-profile="+folder,
                "--ciuki-capture="+str(capture),f"{scheme}://{host}:8080{path}"],capture_output=True,text=True,timeout=25,env=dict(SAN,CIUKI_FIXTURE_MODE=mode))
            self.assertTrue(capture.exists(),result.stdout+result.stderr)
            self.assertEqual(capture.stat().st_size,640*480*4)
            if error:self.assert_page_error(result,error)
            if expected:self.assertIn(expected,result.stdout,result.stdout+result.stderr)
            print(f"PASS rendered fixture {mode} {path}",flush=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            return result

    def test_browser_redirect_cookies_and_errors(self):
        cases=[("/redirect",{}),
            ("/cookie-cap/0",{"expected":"fixture_cookie_retention=200"}),
            ("/loop",{"expected":"fixture_loop_requests=6"}),
            ("/truncated",{"error":"Truncated HTTP response."}),
            ("/",{"mode":"hostname-offline","error":"Offline: no usable network address or route."}),
            ("/",{"mode":"fallback-clock","scheme":"https","error":"HTTPS requires a qualified RTC clock and a correct date (2026 or later). Please set the system clock."})]
        for path,options in cases:
            with self.subTest(path=path,options=options):self.browser(path,**options)

    def assert_page_error(self,result,message):
        from html.parser import HTMLParser
        class Text(HTMLParser):
            def __init__(self):super().__init__();self.parts=[]
            def handle_data(self,data):self.parts.append(data)
        self.assertIn("fixture_page_begin",result.stdout,result.stdout+result.stderr)
        page=result.stdout.split("fixture_page_begin\n",1)[1].split("\nfixture_page_end",1)[0]
        parser=Text();parser.feed(page)
        self.assertIn(message," ".join(parser.parts))

    def test_navigation_during_blocked_lookup(self):
        self.browser("/",mode="browser-blocked",host="blocked.example",expected="fixture_navigation_blocked=1 completed=1 late_ignored=1")

if __name__=="__main__":unittest.main()
