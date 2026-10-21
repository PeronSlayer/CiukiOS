#!/usr/bin/env python3
"""Compile F6 SDK wrappers/records using an existing pinned SDK's headers."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
from pathlib import Path
import runpy
import subprocess
ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument('--headers', type=Path, required=True)
args = parser.parse_args()
flags = runpy.run_path(str(ROOT / 'scripts/build_kernel.py'))['CFLAGS']
flags = [f for f in flags if f != '-mno-80387']
resource = subprocess.check_output(['clang', '-print-resource-dir'], text=True).strip()
flags += ['-nostdinc', '-isystem', resource + '/include', '-I', str(ROOT / 'src/kernel/include'),
          '-I', str(ROOT / 'sdk/sysroot-overlay/include'), '-I', str(args.headers.resolve()),
          '-D_DEFAULT_SOURCE=1', '-D__DYNAMIC_REENT__']
out = ROOT / 'build/host/lwip'
out.mkdir(parents=True, exist_ok=True)
for name in ('socket', 'inet', 'netdb'):
    subprocess.run(['clang', *flags, '-c', str(ROOT / f'sdk/libciuki/{name}.c'),
                    '-o', str(out / f'sdk-{name}.o')], check=True)
source = out / 'sdk-layout.c'
source.write_text('''#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/select.h>
_Static_assert(sizeof(struct sockaddr)==16,"sockaddr");
_Static_assert(sizeof(struct sockaddr_in)==16,"sockaddr_in");
_Static_assert(__builtin_offsetof(struct sockaddr_in,sin_addr)==4,"sin_addr");
_Static_assert(sizeof(socklen_t)==4 && sizeof(sa_family_t)==2,"socket scalars");
_Static_assert(sizeof(nfds_t)==4 && sizeof(struct pollfd)==8,"poll scalars");
_Static_assert(sizeof(fd_set)==16 && FD_SETSIZE==128,"select bits");
_Static_assert(sizeof(struct timeval)==16,"timeval");
_Static_assert(sizeof(struct addrinfo)==32,"addrinfo");
''')
subprocess.run(['clang', *flags, '-c', str(source), '-o', str(out / 'sdk-layout.o')], check=True)
print('PASS F6 SDK wrapper compilation and public i686 layouts')
