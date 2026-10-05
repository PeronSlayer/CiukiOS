#!/usr/bin/env python3
"""Bounded Linux integration of the native browser and its real DOS worker.

Run serially inside a capped systemd scope; never invoke this alongside a build.
Only a reflink test disk receives the temporary CA; binaries come from the
canonical full build so the runtime evidence covers the shipping image.
"""
import datetime
import functools
import http.server
import json
import os
import re
from pathlib import Path
import signal
import shutil
import ssl
import subprocess
import sys
import threading
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID
import ipaddress
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from qemu_test_native_windows import WindowVM


def main():
    os.chdir(ROOT)
    out = Path(sys.argv[1]).resolve()
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    fixture = out / 'fixture'
    fixture.mkdir()
    (fixture / 'index.html').write_text('''<!doctype html><html><head>
<title>CiukWeb: HTTPS, CSS and JavaScript</title>
<style>p {color: red} .hidden {display: none} h1 {color: blue}</style>
<link rel="stylesheet" href="/screen.css"></head><body>
<h1>CiukWeb graphical browser</h1><p id="result" class="result">PENDING</p>
<p class="hidden">THIS MUST BE HIDDEN</p>
<script>var sequence = 1; document.getElementById('result').textContent = 'Inline JavaScript OK';
document.write('<p>Document write OK</p>');</script>
<script src="/external.js"></script>
<p>PNG, JPEG and GIF images load in the native desktop window.</p>
<img src="/gradient.png" alt="RGB gradient"><img src="/sample.jpg" alt="JPEG"><img src="/sample.gif" alt="GIF">
<p><a href="/loop.html">Bounded script test</a></p></body></html>''')
    (fixture / 'screen.css').write_text('#result {color: #008000 !important; background-color: #ffff00; margin-left: 12px} h1 {margin-bottom: 8px}')
    (fixture / 'external.js').write_text("if(sequence !== 1) throw new Error('Script order'); document.getElementById('result').textContent = 'External JavaScript OK';")
    (fixture / 'loop.html').write_text('<html><title>Script limit</title><body><p>Desktop remains available</p><script>while(true) {}</script><p>Page survives script timeout</p></body></html>')
    img = Image.new('RGB', (96,64))
    for y in range(64):
        for x in range(96): img.putpixel((x,y),(x*255//95,y*255//63,112))
    img.save(fixture/'gradient.png'); img.save(fixture/'sample.jpg'); img.save(fixture/'sample.gif')
    now = datetime.datetime.now(datetime.timezone.utc)
    ca_key = rsa.generate_private_key(public_exponent=65537,key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'CiukiOS ephemeral integration CA')])
    ca = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name).public_key(ca_key.public_key())
          .serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(days=1))
          .not_valid_after(now+datetime.timedelta(days=2)).add_extension(x509.BasicConstraints(ca=True,path_length=0),critical=True)
          .add_extension(x509.KeyUsage(False,False,False,False,False,True,True,False,False),critical=True)
          .sign(ca_key,hashes.SHA256()))
    server_key = rsa.generate_private_key(public_exponent=65537,key_size=2048)
    key_path = out/'server-key.pem'
    key_path.write_bytes(server_key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    requests = []
    class Handler(http.server.SimpleHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def log_message(self,fmt,*args): requests.append(self.path)
    servers = []
    def server(label, address, expired=False):
        cert = (x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'integration.invalid')]))
                .issuer_name(ca_name).public_key(server_key.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(now-datetime.timedelta(days=2))
                .not_valid_after(now+datetime.timedelta(days=-1 if expired else 1))
                .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address(address))]),critical=False)
                .add_extension(x509.BasicConstraints(ca=False,path_length=None),critical=True).sign(ca_key,hashes.SHA256()))
        cert_path=out/(label+'.pem');cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        httpd=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Handler,directory=str(fixture)))
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.minimum_version=ssl.TLSVersion.TLSv1_2
        ctx.load_cert_chain(cert_path,key_path);httpd.socket=ctx.wrap_socket(httpd.socket,server_side=True)
        threading.Thread(target=httpd.serve_forever,daemon=True).start();servers.append(httpd)
        return 'https://10.0.2.2:'+str(httpd.server_port)
    valid=server('valid','10.0.2.2');wrong=server('wrong-ip','10.0.2.3');expired=server('expired','10.0.2.2',True)
    trust=out/'CACERT.PEM';trust.write_bytes((ROOT/'third_party/cacert/CACERT.PEM').read_bytes()+b'\n'+ca.public_bytes(serialization.Encoding.PEM))
    disk=out/'disk.img'
    subprocess.run(['cp','--reflink=always','build/full/ciukios-full.img',str(disk)],check=True)
    if subprocess.run(['mdir','-i',str(disk),'::SYSTEM/WEB'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode != 0:
        subprocess.run(['mmd','-i',str(disk),'::SYSTEM/WEB'],check=True)
    subprocess.run(['mcopy','-o','-i',str(disk),str(trust),'::SYSTEM/WEB/CACERT.PEM'],check=True)
    os.environ['SDL_VIDEODRIVER']='x11'
    class LiveVM(WindowVM):
        def shot(self,name):
            path=self.output/(name+'.png')
            win=subprocess.check_output(['xdotool','search','--onlyvisible','--pid',str(self.process.pid)],timeout=5,stderr=subprocess.DEVNULL).split()[-1].decode()
            subprocess.run(['xdotool','windowactivate','--sync',win],timeout=5,check=True,stderr=subprocess.DEVNULL)
            subprocess.run(['spectacle','--activewindow','--background','--nonotify','--no-decoration','--no-shadow','--output',str(path)],timeout=10,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            with Image.open(path) as image: rgb=image.convert('RGB')
            rgb.save(path);return path
    report={};vm=None
    def expire(*args): raise TimeoutError('web stack validation deadline')
    signal.signal(signal.SIGALRM,expire);signal.signal(signal.SIGTERM,expire);signal.alarm(260)
    try:
        vm=LiveVM(disk,out/'run','none',memory=256,palette='platinum',extra_qemu_args=[
            '-device','virtio-vga-gl','-display','sdl,gl=on','-audiodev','none,id=snd','-device','AC97,audiodev=snd',
            '-device','virtio-rng-pci,disable-modern=on,disable-legacy=off',
            '-netdev','user,id=n','-device','ne2k_pci,netdev=n',
            '-object','filter-dump,id=webtrace,netdev=n,file='+str(out/'network.pcap')])
        vm.ready();vm.wait('[ABOUT] open',0,10);vm.key('alt-f4')
        assert 'NIC interrupts owned by the desktop VM' in vm.serial.read_bytes().decode(errors='replace')
        (out/'pci.txt').write_bytes(vm.hmp('info pci'))
        def cursor_samples(label):
            samples=[]
            for _ in range(4):
                before=vm.pointer();started=time.monotonic()
                vm.hmp('mouse_move '+('13' if before[0]<100 else '-13')+' 0 0')
                while vm.pointer()==before:
                    if time.monotonic()-started>=6:
                        report.setdefault('pointer_timeouts',[]).append(label)
                        (out/(label+'-registers.txt')).write_bytes(vm.hmp('info registers'))
                        (out/(label+'-pic.txt')).write_bytes(vm.hmp('info pic'))
                        boxes=re.findall(rb'\[WEBWORK\] mailbox ([0-9A-Fa-f]{8})',vm.serial.read_bytes())
                        if boxes:(out/(label+'-mailbox.txt')).write_bytes(vm.hmp('xp /16wx 0x'+boxes[-1].decode()))
                        raise AssertionError('Desktop pointer missed its response deadline during '+label)
                samples.append(time.monotonic()-started)
            return samples
        report['cursor_idle_capture_seconds']=cursor_samples('idle')
        def navigate(url):
            offset=vm.offset();vm.key('ctrl-l');vm.text(url);return offset
        offset=vm.offset();started=time.monotonic();vm.key('f3');vm.text('CIUKWEB '+valid+'/index.html')
        report['cursor_loading_capture_seconds']=cursor_samples('loading')
        deadline=time.monotonic()+90
        while '[CIUKWEB] images loaded=3 failed=0' not in vm.serial.read_bytes()[offset:].decode(errors='replace'):
            latest=vm.serial.read_bytes()[offset:].decode(errors='replace')
            assert '[CIUKWEB] error' not in latest,latest[-1800:]
            assert time.monotonic()<deadline,'Page load deadline'
            time.sleep(.1)
        report['valid_tls_page_seconds']=time.monotonic()-started;vm.shot('https-css-javascript-images')
        vm.hmp('stop')
        try: subprocess.run(['mcopy','-o','-i',str(disk),'::NET/CWRENDER.HTM',str(out/'rendered.htm')],check=True)
        finally: vm.hmp('cont')
        rendered=(out/'rendered.htm').read_text()
        assert '<p id="result" class="result">External JavaScript OK</p>' in rendered,rendered
        assert '</script><p>Document write OK</p>' in rendered,rendered
        assert '/screen.css' in requests and '/external.js' in requests,requests
        report['external_script_dom']=report['external_css_requested']=True
        for name,url in [('wrong_ip',wrong),('expired',expired)]:
            offset=navigate(url+'/index.html');vm.wait('[CIUKWEB] error',offset,55);vm.shot(name)
            log=vm.serial.read_bytes()[offset:].decode(errors='replace')
            assert 'HTTPS certificate or handshake failed' in log,log
            report[name+'_rejected']=True
        if '--public' in sys.argv[2:]:
            offset=navigate('https://example.com/');vm.wait('[CIUKWEB] rendered',offset,55)
            public_log=vm.serial.read_bytes()[offset:].decode(errors='replace')
            assert '[CIUKWEB] HTTP status=200' in public_log,public_log
            assert '[CIUKWEB] error' not in public_log,public_log
            vm.shot('public-https');report['public_dns_tls_ca_bundle']=True
            offset=navigate('https://www.google.com/');vm.wait('[CIUKWEB] rendered',offset,55)
            google_log=vm.serial.read_bytes()[offset:].decode(errors='replace')
            vm.shot('google-https')
            assert '[CIUKWEB] HTTP status=200' in google_log,google_log
            report['google_https']=True
        offset=navigate(valid+'/loop.html');vm.wait('[CIUKWEB] rendered',offset,55);vm.shot('bounded-script')
        loop_log=vm.serial.read_bytes()[offset:].decode(errors='replace')
        assert '[CIUKWEB] script warning Script 1 failed (code 4)' in loop_log,loop_log
        report['script_limit_page_survives']=True
        offset=vm.offset();vm.key('alt-f4');vm.wait('[CIUKWEB] closed',offset,20)
        close_log=vm.serial.read_bytes()[offset:].decode(errors='replace')
        assert '[WEBWORK] shutdown complete' in close_log,close_log
        assert '[WEBWORK] forced shutdown' not in close_log,close_log
        yields=re.findall(r'\[DPMIRUN\] background yields ([0-9A-Fa-f]{4})',close_log)
        assert yields and int(yields[-1],16)>0,close_log
        report['graceful_worker_exit']=True
        report['worker_idle_yields']=int(yields[-1],16)
        report['cursor_after_close_capture_seconds']=cursor_samples('closed')
        offset=vm.offset();vm.key('f3');vm.text('ABOUT');vm.wait('[ABOUT] open',offset,15)
        report['desktop_after_close']=True
        if '--doom' in sys.argv[2:]:
            # Optional local licensed fixture, never copied into release ZIPs.
            vm.key('alt-f4');offset=vm.offset();vm.key('f3');vm.text(r'FILES C:\DESKTOP\TestGames')
            vm.wait('[FILES] geometry',offset,10);time.sleep(.4);vm.key('home');vm.key('ret')
            vm.wait('ST_Init: Init status bar.',offset,65);time.sleep(1)
            for key in ('esc','ret','ret','ret'):
                vm.hmp('sendkey '+key+' 150');time.sleep(.5)
            vm.shot('doom-after-https-new-game')
            for _ in range(3):vm.hmp('sendkey ctrl 200');time.sleep(2)
            vm.hmp('sendkey f10 250');time.sleep(2);vm.hmp('sendkey y 250')
            vm.wait('[DOSVM] ended',offset,20)
            report['doom_after_https_normal_exit']=True
            report['cursor_after_doom_capture_seconds']=cursor_samples('after-doom')
        assert not report.get('pointer_timeouts'),'Desktop pointer missed its response deadline'
        report['pass']=True
    except BaseException as error:
        report['error']=repr(error)
        if vm:
            try:
                vm.hmp('stop')
                (out/'failure-registers.txt').write_bytes(vm.hmp('info registers'))
                (out/'failure-pic.txt').write_bytes(vm.hmp('info pic'))
                (out/'failure-ne2k.txt').write_bytes(vm.hmp('i /b 0xc500')+vm.hmp('i /b 0xc507'))
                # The monitor's first linear page is intentionally unmapped.
                # Save only mapped Jemm code/data and its 4 KiB active stack.
                (out/'failure-save.txt').write_bytes(
                    vm.hmp('memsave 0x110000 0x6000 "'+str(out/'failure-monitor.bin')+'"')+
                    vm.hmp('memsave 0xf8004000 0x1000 "'+str(out/'failure-stack.bin')+'"')+
                    vm.hmp('memsave 0 0xa0000 "'+str(out/'failure-dos.bin')+'"'))
                vm.hmp('memsave 0 0x1000 "'+str(out/'failure-ivt.bin')+'"')
                vm.hmp('cont')
            except Exception:pass
    finally:
        if vm:
            try:vm.shot('final')
            except Exception:pass
            vm.close()
        for httpd in servers:httpd.shutdown();httpd.server_close()
        report['requests']=requests;(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2),flush=True)
    assert report.get('pass'),report.get('error')


if __name__=='__main__':main()
