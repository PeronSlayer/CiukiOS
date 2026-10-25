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
            subprocess.run([*cls.flags,*(cls.port_flags if name in ("network","tls-limits","tls-entropy") else []),*map(str,sources),*cls.libs,*extra,"-o",str(OUT / name)],check=True)
        compile("http",[HERE / "http_test.c",ROOT / "sdk/netsurf/port/http.c"])
        compile("tls-limits",[HERE / "tls_limits_test.c"])
        compile("tls-entropy",[HERE / "tls_entropy_test.c",ROOT / "sdk/netsurf/port/tls.c"],
            ["-Wl,--wrap=getentropy,--wrap=clock_gettime,--wrap=br_ssl_engine_inject_entropy,--wrap=br_ssl_client_reset"])
        # dns.c reads the native resolver header only outside CIUKI_HOST (G26b).
        compile("dns",[HERE / "dns_test.c",ROOT / "sdk/netsurf/port/dns.c"],
            ["-DCIUKI_HOST","-Wl,--wrap=getaddrinfo,--wrap=freeaddrinfo,--wrap=calloc,--wrap=free,--wrap=pthread_create"])
        cls.wraps=["-Wl,"+",".join("--wrap="+n for n in ("socket","connect","send","recv","close","fcntl","getsockopt","poll","getaddrinfo","freeaddrinfo","open"))]
        cls.stack=cls.compile_stack()
        compile("network",[HERE / "network_test.c",*[ROOT / ("sdk/netsurf/port/"+s+".c") for s in ("http","net","tls","dns")],*cls.stack],
            [*cls.wraps,"-Wl,--wrap=clock_gettime,--wrap=getentropy"])
        source=BASE / "source/netsurf"
        objects=[p for p in sorted((source / "build/Linux-framebuffer").glob("*.o"))]
        libraries=["-Wl,--start-group",*[str(p) for p in sorted((BASE / "prefix/lib").glob("*.a")) if p.name!="libnsfb.a"],"-Wl,--end-group",
                   "-Wl,--whole-archive","-lnsfb","-Wl,--no-whole-archive","-lm","-pthread"]
        subprocess.run([*cls.flags,*cls.port_flags,str(HERE / "browser_network_test.c"),*map(str,objects),*cls.stack,
            "-L"+str(BASE / "prefix/lib"),*libraries,*cls.wraps,"-Wl,--wrap=main,--wrap=nsfb_event,--wrap=clock_gettime,--wrap=pthread_cond_wait,--wrap=ciuki_tls_create,--wrap=getentropy","-o",str(OUT / "browser")],check=True)

    @classmethod
    def compile_stack(cls):
        port=ROOT / "src/kernel/net/lwip"
        build=runpy.run_path(str(port / "build.py"));source=build["source"]()
        flags=[*cls.flags,"-DLWIP_HOST","-pthread","-Wno-macro-redefined","-Wno-unused-function","-Wno-unused-parameter",
            "-ffunction-sections","-fdata-sections",*build["includes"](source),"-I"+str(ROOT / "src/kernel/include")]
        # The host file list of tests/host/lwip/run.py (f2-100 added control.c
        # and the settings/bridge units the stack links against).
        files=[source / "src" / name for name in build["SOURCES"]]+[port / name for name in ("sys_arch.c","stack.c","sockets.c","syscalls.c","control.c")]
        files += [ROOT / "src/kernel/net" / name for name in ("bridge.c","packet_pool.c","config.c")]+[ROOT / "src/kernel/lib/fmt.c"]
        files += [HERE / "stack_fixture.c",ROOT / "tests/host/lwip/channel_fixture.c"]
        objects=[]
        for i,path in enumerate(files):
            obj=OUT / ("stack-"+str(i)+".o")
            subprocess.run([*flags,"-c",str(path),"-o",str(obj)],check=True)
            objects.append(str(obj))
        return [*objects,"-Wl,--gc-sections"]

    @classmethod
    def tearDownClass(cls):cls.fixture.cleanup()

    def engine(self,mode,path="/",inspect=None):
        secure=mode.startswith("tls-") or mode in ("valid","expired","future","wrong","self","tls10","intermediate","missing-intermediate","malformed-chain","cert-depth","cert-buffer")
        cert="intermediate" if mode in ("intermediate","missing-intermediate","malformed-chain") else mode if mode in ("valid","expired","future","wrong","self") else "valid"
        if mode=="malformed-chain":
            (self.folder / "intermediate-chain.der").write_bytes(b"\x30\x03\x02\x01")
        elif mode=="intermediate":
            openssl("x509","-in",self.folder / "intermediate-ca.pem","-outform","DER","-out",self.folder / "intermediate-chain.der")
        args=[str(self.folder / (cert+".der")),str(self.folder / (cert+"-key.der"))] if secure else ["-","-"]
        try:
            port="443" if mode=="tls-web" else "80" if mode=="web-http" else "8443" if secure else "9" if mode=="refused" else "8080"
            result=subprocess.run([str(OUT / "network"),mode,port,path,*args],text=True,capture_output=True,env=SAN,timeout=25)
        except subprocess.TimeoutExpired as error:
            print(error.stdout,error.stderr,flush=True)
            raise
        print(result.stdout.strip(),flush=True)
        if inspect:inspect(result)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        return result

    def test_gateway_mtu_and_syn_retry(self):
        for mode in ("web-http","tls-web"):
            with self.subTest(mode=mode):self.engine(mode,"/wan")

    def test_trial_423b803a_guest_fcntl_connect(self):
        """fx-11, T23 trial 423b803a: 'fetch url=https://www.w3.org/ stage=connect
        ... ms=225', then 'stage=error ... errno=22 ... ms=226 ... at=connect'
        and no SYN on the wire. open_connection sets O_NONBLOCK with
        fcntl(F_SETFL,fcntl(F_GETFL)|O_NONBLOCK); F_GETFL reports O_RDWR for a
        socket and the guest's file_fcntl refused that bit (EINVAL). The
        fixture's fcntl now applies the production rule (ciuki/files.h
        file_setfl_bits): with the old rule both fetches end at connect with
        errno=22; with the fix they complete through the gateway."""
        for mode,stages in (("web-http",["dns","connect","request","response","done"]),
                            ("tls-web",["dns","connect","tls","request","response","done"])):
            def inspect(result,stages=stages):
                lines=self.fetch_records(result)
                self.assertFalse([line for line in lines if " stage=error " in line],"\n".join(lines))
                self.assertEqual([line.split(" stage=")[1].split()[0] for line in lines],stages,"\n".join(lines))
                self.assertIn(" code=200 ",lines[-1]);self.assertIn(" errno=0 ",lines[-1])
            with self.subTest(mode=mode):self.engine(mode,"/wan",inspect=inspect)

    def test_idle_clock_step(self):
        for mode in ("idle-expiry","idle-reuse-expiry"):
            with self.subTest(mode=mode):self.engine(mode)

    def fetch_records(self,result):
        import re
        lines=[line for line in result.stderr.splitlines() if line.startswith("[netsurf] fetch ")]
        self.assertTrue(lines,result.stderr)
        seen=set()
        for line in lines:
            self.assertLessEqual(len(line)+1,512,line)
            match=re.fullmatch(r"\[netsurf\] fetch url=(https?://[^ ?#]+) stage=(dns|connect|tls|request|response|redirect|done|error) code=(\d+) errno=(\d+) tls_err=(\d+) bytes=(\d+) ms=(\d+) id=(\d+) at=\w+ tls_code=\d+ rtc=[01] utc=-?\d+",line)
            self.assertIsNotNone(match,line)
            key=(match[8],match[2]);self.assertNotIn(key,seen,line);seen.add(key)
        return lines

    def test_fetch_stage_records_and_privacy(self):
        def success(result):
            lines=self.fetch_records(result)
            self.assertFalse(any("TOPSECRET" in line for line in lines))
            self.assertEqual([line.split(" stage=")[1].split()[0] for line in lines],
                             ["dns","connect","request","response","done"])
            self.assertIn("code=200",lines[-1]);self.assertIn("bytes=5",lines[-1])
            # fx-11: only failures reach NETSURF.LOG; the exit summary counts.
            self.assertNotIn("netsurf_log:",result.stdout)
            self.assertIn("fetch_summary: fetches=1 fetch_ok=1 fetch_errors=0 last_error=none\n",result.stdout)
        with self.subTest(mode="ok"):self.engine("ok","/?TOPSECRET=value",inspect=success)
        for mode,stage in (("dns","dns"),("refused","connect"),("timeout","response"),("wrong","tls"),("fallback-clock","tls"),("entropy","tls")):
            with self.subTest(mode=mode):
                def failure(result):
                    lines=self.fetch_records(result)
                    self.assertEqual(sum(" stage=error " in line for line in lines),1)
                    self.assertTrue(any(" stage=error " in line and " at="+stage+" " in line for line in lines))
                    self.assertIn("Fetch failed during "+stage+":",result.stdout)
                    logged=[line for line in result.stdout.splitlines() if line.startswith("netsurf_log: ")]
                    self.assertEqual(len(logged),1,result.stdout)
                    self.assertIn(logged[0][len("netsurf_log: "):],[line for line in lines if " stage=error " in line])
                    self.assertRegex(result.stdout,r"fetch_summary: fetches=1 fetch_ok=0 fetch_errors=1 last_error="+stage+r":\d+:\d+\n")
                self.engine(mode,"/slow" if mode=="timeout" else "/",inspect=failure)

    def test_parser(self):subprocess.run([str(OUT / "http")],check=True,env=SAN,timeout=20)
    def test_certificate_capture_limits(self):subprocess.run([str(OUT / "tls-limits")],check=True,env=SAN,timeout=10)
    def test_entropy_seeding(self):subprocess.run([str(OUT / "tls-entropy")],check=True,env=SAN,timeout=10)
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

    def test_tls_renegotiation_and_pre_request_input(self):
        for mode in ("tls-hello-request","tls-before-first-send"):
            with self.subTest(mode=mode):self.engine(mode)

    def test_admitted_transport_retry_and_no_replay(self):
        for mode in ("stale-retry","post-no-replay","tls-stale-retry","tls-post-no-replay"):
            with self.subTest(mode=mode):self.engine(mode)

    def browser(self,path,mode="browser",scheme="http",host="fixture.example",error=None,expected=None,port=None,inspect=None,test_mode=False,extra_env=None):
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            capture=Path(folder) / "page.xrgb"
            log=Path(folder) / "NETSURF.LOG"
            url=f"{scheme}://{host}:{port or (8443 if scheme=='https' else 8080)}{path}"
            env=dict(SAN,CIUKI_FIXTURE_MODE=mode,CIUKI_FIXTURE_CERT=str(self.folder / "valid.der"),CIUKI_FIXTURE_KEY=str(self.folder / "valid-key.der"),**(extra_env or {}))
            # f6-18 test profile: --test=browser plus the kernel test request
            # (probe call 5 on CiukiOS; this environment variable on the host).
            if test_mode:env["CIUKI_TEST_REQUEST"]="f6:browser run=0123abcd url="+url;self.assertLessEqual(len(env["CIUKI_TEST_REQUEST"]),64)
            result=subprocess.run([str(OUT / "browser"),"-w","640","-h","480",
                "--ciuki-resources="+str(BASE / "stage/system/netsurf"),"--ciuki-profile="+folder,"--ciuki-log="+str(log),
                *(["--test=browser"] if test_mode else []),"--ciuki-capture="+str(capture),url],capture_output=True,text=True,timeout=25,env=env)
            result.netsurf_log=log.read_text() if log.exists() else ""
            self.assertTrue(capture.exists(),result.stdout+result.stderr)
            self.assertEqual(capture.stat().st_size,640*480*4)
            result.capture=capture.read_bytes()
            if error:self.assert_page_error(result,error)
            if expected:self.assertIn(expected,result.stdout,result.stdout+result.stderr)
            if inspect:inspect(result)
            print(f"PASS rendered fixture {mode} {path}",flush=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            return result

    def test_browser_test_mode_reports_the_requested_page(self):
        """fx-11c: the first QEMU run of e100-http-page reported 'case=page
        status=0 bytes=0 tls=none url=about:blank' and ended while NetSurf's
        fetch was at stage=dns. browser_window_navigate stops the (empty)
        window before fetching, the stop raises the frontend's stop-throbber
        event, and the test profile took that for the completed page. The one
        report must name the requested navigation's outcome: the page, its
        redirect target, or the error page after a failed fetch."""
        cases=[("/page",{},r"status=200 bytes=\d+ tls=none url=http://fixture.example:8080/page$"),
               ("/page",{"scheme":"https"},r"status=200 bytes=\d+ tls=ok url=https://fixture.example:8443/page$"),
               ("/upgrade",{},r"status=200 bytes=\d+ tls=ok url=https://fixture.example:8443/tls-cookie-end$"),
               ("/page",{"port":9},r"status=0 bytes=0 tls=none url=about:query/fetcherror$"),
               ("/page",{"scheme":"https","mode":"browser-untrusted"},r"status=0 bytes=0 tls=cert url=about:query/fetcherror$")]
        for path,options,pattern in cases:
            with self.subTest(path=path,options=options):
                result=self.browser(path,test_mode=True,**options)
                reports=[line for line in result.stderr.splitlines() if line.startswith("[browser-test] case=browser-page ")]
                self.assertEqual(len(reports),1,result.stderr)
                self.assertNotIn("url=about:blank",reports[0])
                self.assertRegex(reports[0],pattern)

    def test_fetch_failure_pages_name_the_stage(self):
        for options in (
            {"mode":"dns","error":"Fetch failed during dns: DNS lookup failed"},
            {"port":9,"error":"Fetch failed during connect: The server refused the connection."},
            {"scheme":"https","mode":"browser-untrusted","error":"Fetch failed during tls: TLS certificate validation failed"},
            {"scheme":"https","mode":"fallback-clock","error":"Fetch failed during tls: HTTPS requires a qualified RTC clock"},
            {"scheme":"https","mode":"browser-entropy","error":"Fetch failed during tls: Secure connections are not ready yet."}):
            stage=options["error"].split("Fetch failed during ")[1].split(":")[0]
            def inspect(result,stage=stage):
                import re
                errors=[line for line in self.fetch_records(result) if " stage=error " in line]
                # fx-11: the error record and the exit summary reach NETSURF.LOG.
                logged=[line.split(" ",1)[1] for line in result.netsurf_log.splitlines() if " [netsurf] fetch " in " "+line]
                self.assertTrue(logged and set(logged)<=set(errors) and all(" at="+stage+" " in line for line in logged),result.netsurf_log)
                exits=[line for line in result.netsurf_log.splitlines() if "[netsurf] exit reason=" in line]
                self.assertEqual(len(exits),1,result.netsurf_log)
                self.assertRegex(exits[0],r" fetch_errors=[1-9]\d* last_error="+stage+r":\d+:\d+ network=")
            with self.subTest(options=options):self.browser("/page",inspect=inspect,**options)
        self.browser("/slow",mode="browser-timeout",error="Fetch failed during response: The network request timed out",inspect=self.fetch_records)

    def test_redirect_fetch_records(self):
        for path,code in (("/redirect",302),("/upgrade",302),("/upgrade301",301)):
            def inspect(result):
                self.assertTrue(any(f" stage=redirect code={code} " in line for line in self.fetch_records(result)))
            with self.subTest(path=path):self.browser(path,inspect=inspect)

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

    def test_remote_layout_rejection_keeps_browser(self):
        """f6-20: a remote page rejected by the layout time limit leaves the
        browser running on the error page; its Try Again button re-fetches."""
        result=self.browser("/page",mode="browser-budget",expected="fixture_budget_retry=1")
        self.assertIn("fixture_budget_rejected requests=",result.stdout)
        self.assertIn("NetSurf layout rejected: remote page, time limit",result.stderr)
        self.assertIn("<h1>Ciuki</h1>",result.stdout)
        lines=result.stderr.splitlines()
        reasons=[line for line in lines if line.startswith("[netsurf] exit reason=")]
        self.assertEqual(len(reasons),1,result.stderr)
        self.assertTrue(reasons[0].startswith("[netsurf] exit reason=capture ") and "layout_rejections=1" in reasons[0],reasons[0])
        page_requests=[int(n) for n in __import__("re").findall(r"fixture_budget_(?:rejected|retry=1) requests=(\d+)",result.stdout)]
        self.assertEqual(len(page_requests),2)
        self.assertGreater(page_requests[1],page_requests[0])

    def test_top_level_document_layout_limit(self):
        """fx-11, lead decision 2026-10-23: the top-level remote document gets a
        10 s layout limit; an iframe's document (a sub-resource window) keeps
        75 ms. The budget clock is charged x20000, so both small documents
        cost far more than 75 ms and far less than 10 s. fx-24: the iframe's
        document is 200 short lines (/frame-content); a single heading now
        lays out in about 3.6 us of host time, 72 ms charged."""
        result=self.browser("/framed",mode="browser-document",expected="<h1>Ciuki</h1>")
        page=result.stdout.split("fixture_page_begin\n",1)[1].split("\nfixture_page_end",1)[0]
        self.assertIn("<iframe src=\"/frame-content\"",page)
        self.assertNotIn("needs more layout time",page)
        self.assertRegex(result.stderr,r"\[netsurf\] layout slow elapsed_us=\d+ class=remote accepted=1")
        self.assertIn("NetSurf layout rejected: remote page, time limit",result.stderr)

    def test_styled_page_over_http(self):
        """fx-24, T23 trial f441c5bd: remote pages showed no images (and the
        owner saw no styling) although every image was fetched with 200.
        The styled fixture over HTTP, as a server sends it (HTML/CSS/SVG
        gzip-encoded, images chunked, media types in Content-Type): linked
        and @import stylesheets, a style element and attribute apply; JPEG,
        PNG, GIF, WebP and SVG images are fetched, decoded and drawn."""
        import styled
        site=str(HERE / "fixtures/fx24")
        def inspect(result):
            records=self.fetch_records(result)
            done=[line for line in records if " stage=done " in line]
            for name in ("styled.html","styled.css","imported.css","quadrants.jpg","disc.png","halves.gif","stripes.webp",
                         "shapes.svg","tile.png","large.jpg","banner.svg"):
                self.assertTrue(any(f"/fx24/{name} stage=done code=200 " in line for line in done),name+"\n"+"\n".join(records))
            self.assertEqual(styled.failures(result.capture,640),[])
        self.browser("/fx24/styled.html",inspect=inspect,extra_env={"CIUKI_FIXTURE_SITE":site})

    def test_page_with_40_images_under_a_socket_limit(self):
        """fx-nsperf, T23 trial 2ca3b0b6: images of one HTTPS page failed at
        connect with errno 105 (ENOBUFS) after about 5 s in the fetch queue,
        five in a row. A refused connect gave up the image and freed its slot
        for the next queued image, which was refused too. With the system
        holding one TCP socket (stack_fixture.c browser-socket-limit), a page
        with 40 images over HTTPS must still show every image: refusals wait
        and retry, the browser never holds more sockets than the system
        accepted, and later images use the kept-alive connection."""
        import re
        with tempfile.TemporaryDirectory(dir=OUT) as site:
            image=(HERE / "fixtures/fx24/disc.png").read_bytes()
            names=[f"img{i:02d}.png" for i in range(40)]
            for name in names:(Path(site) / name).write_bytes(image)
            (Path(site) / "many.html").write_text("<!doctype html><title>Images</title><h1>Ciuki</h1>"+
                "".join(f'<img src="{name}" width="20" height="20" alt="{name}">' for name in names))
            def inspect(result):
                records=self.fetch_records(result)
                self.assertEqual([line for line in records if " stage=error " in line],[])
                done=[line for line in records if " stage=done code=200 " in line]
                for name in names:
                    self.assertTrue(any(f"/fx24/{name} stage=done code=200 " in line for line in done),name+"\n"+result.stderr)
                limit=re.search(r"fixture_socket_limit refusals=(\d+) max_live=(\d+) connections=(\d+)",result.stdout)
                self.assertIsNotNone(limit,result.stdout)
                print(limit[0],flush=True)
                self.assertGreater(int(limit[1]),0)
                self.assertEqual(int(limit[2]),1)
                self.assertIn("[netsurf] connect-wait url=https://fixture.example/fx24/",result.stderr)
                retries=re.search(r"\[netsurf\] exit reason=\S+ .* connect_retries=(\d+) fetches=",result.stderr)
                self.assertTrue(retries and int(retries[1])>0,result.stderr)
            self.browser("/fx24/many.html",mode="browser-socket-limit",scheme="https",inspect=inspect,extra_env={"CIUKI_FIXTURE_SITE":site})

    def test_images_fetched_once_with_slow_ui_turns(self):
        """t23-render, T23 trial c42f556e: an encyclopedia main page made 49
        fetches and never appeared; 18 of the fetches were its thumbnails a
        second time. The parser fetches an img element's image when the
        element is inserted (dom_event.c); box construction, 10 elements per
        UI turn, reached the same element seconds later and fetched it again:
        the thumbnail URLs carry a query and no explicit expiry, so the
        completed first response was not reusable (llcache). With UI turns
        of 20 ms (the trial's were about 100 ms) and 1,500 elements before
        twelve such images, each image must be fetched once, the box must
        take the parser's fetch, and box construction must run in a few
        slices of at most CIUKI_BOX_SLICE_NS model time, not 150."""
        import re
        with tempfile.TemporaryDirectory(dir=OUT) as site:
            image=(HERE / "fixtures/fx24/disc.png").read_bytes()
            names=[f"thumb{i:02d}.png" for i in range(12)]
            for name in names:(Path(site) / name).write_bytes(image)
            (Path(site) / "main.html").write_text("<!doctype html><title>Main</title><h1>Ciuki</h1><p>"+"<i>w</i>"*1500+"</p>"+
                "".join(f'<img src="{name}?utm_source=ciuki&amp;utm_content=thumbnail" width="20" height="20" alt="{name}">' for name in names))
            def inspect(result):
                records=self.fetch_records(result)
                for name in names:
                    # Records name the path only (privacy: no query).
                    done=[line for line in records if f"/fx24/{name} stage=done code=200 " in line]
                    self.assertEqual(len(done),1,name+"\n"+"\n".join(records))
                exit_line=re.search(r"\[netsurf\] exit reason=\S+ .* box_slices=(\d+) box_elements=(\d+) box_slice_max_us=(\d+) image_reuse=(\d+) ",result.stderr)
                self.assertIsNotNone(exit_line,result.stderr)
                slices,elements,slice_max,reuse=map(int,exit_line.groups())
                print(f"slow-ui page: box_slices={slices} box_elements={elements} box_slice_max_us={slice_max} image_reuse={reuse}",flush=True)
                self.assertGreaterEqual(reuse,len(names))
                self.assertGreaterEqual(elements,1500)
                self.assertLessEqual(slices,elements//10//5)
                # One 10-element step past the budget at most (model time).
                self.assertLess(slice_max,12000)
            self.browser("/fx24/main.html",mode="browser-slow-ui",scheme="https",inspect=inspect,extra_env={"CIUKI_FIXTURE_SITE":site})

    def test_navigation_during_blocked_lookup(self):
        self.browser("/",mode="browser-blocked",host="blocked.example",expected="fixture_navigation_blocked=1 completed=1 late_ignored=1")

    def test_browser_https_acceptance(self):
        cases=[("/page",{"scheme":"https","expected":"<h1>Ciuki</h1>"}),
            ("/page",{"scheme":"https","mode":"browser-untrusted","error":"TLS certificate validation failed: untrusted, malformed, or unsupported certificate."}),
            ("/upgrade",{"expected":"fixture_tls_cookie_roundtrip=1"}),
            ("/tls-cookie-start",{"scheme":"https","expected":"fixture_tls_cookie_roundtrip=1"}),
            ("/gzip64",{"scheme":"https","expected":"fixture_tls_gzip_bytes=65536"}),
            ("/page",{"scheme":"https","mode":"browser-entropy","error":"Secure connections are not ready yet. Estimated initial wait remaining: up to ","expected":"fixture_entropy_estimate_ok=1"}),
            ("/page",{"scheme":"https","mode":"browser-entropy-late","error":"The initial one-minute estimate has elapsed; readiness is not guaranteed.","expected":"fixture_entropy_estimate_ok=1"}),
            ("/page",{"scheme":"https","mode":"browser-entropy-reload","expected":"fixture_entropy_reload=1"})]
        for path,options in cases:
            with self.subTest(path=path,options=options):self.browser(path,**options)

    def test_failure_and_readiness_pages_state_the_network(self):
        """f6-22: the T23 had a running e100 with link and no DHCP lease; the
        pages a user sees then say so (no address / address with DNS state)."""
        cases=[("/page",{"mode":"hostname-offline","error":"Offline: no usable network address or route. Network: no address yet (e100, link up); waiting for a DHCP server."}),
            ("/page",{"mode":"dns","error":"DNS lookup failed: the server name could not be resolved. Network: address 192.0.2.100 (e100); DNS lookups failing."}),
            ("/page",{"scheme":"https","mode":"browser-entropy","error":"Reload to retry. Network: address 192.0.2.100 (e100); DNS not tried yet."})]
        for path,options in cases:
            with self.subTest(path=path,options=options):self.browser(path,**options)

if __name__=="__main__":unittest.main()
