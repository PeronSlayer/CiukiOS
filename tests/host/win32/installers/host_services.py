"""H1 installer service over copied records; no host shell, no PE emulation.

The i386 process retains its seccomp filter. A native parent supplies fixture
files, the real w32srv model, and actual child adapter processes. This module
is test-only and is never linked into CiukiOS or staged in an image.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
import ctypes as C
from dataclasses import dataclass
from datetime import datetime, timezone
import errno
import fnmatch
import os
from pathlib import Path
import socket
import stat
import struct
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
LIMIT = 1024 * 1024
MAGIC = 0x32494643
FILE, SYSTEM, PROCESS = 0x109, 0x10a, 0x10b
OBJECT, WAIT = 0x100, 0x101


def build_bridge(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    sources = [ROOT / 'apps/w32srv' / (n + '.c') for n in
               ('registry', 'storage', 'directory', 'defaults', 'posix_io', 'progman', 'restart', 'shell_fs')]
    sources += [ROOT / 'apps/desktop/installer_registry.c', ROOT / 'apps/desktop/protocol.c', HERE / 'service_bridge.c']
    flags = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include')]
    library = out / 'installer-services.so'
    subprocess.run([*flags, '-fPIC', '-shared', '-Wl,--no-undefined', *map(str, sources), '-o', str(library)], check=True)
    return library, flags, sources


class Registry:
    def __init__(self, library, root):
        self.lib = C.CDLL(str(library))
        signatures = {
            'create': (C.c_void_p, [C.c_char_p]),
            'destroy': (C.c_uint32, [C.c_void_p]),
            'exchange': (C.c_uint32, [C.c_void_p, C.c_uint32, C.c_void_p, C.c_uint32,
                                     C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32)]),
            'exit': (C.c_uint32, [C.c_void_p, C.c_uint32]),
            'observe': (C.c_uint32, [C.c_void_p, C.c_uint32]),
            'restart_begin': (C.c_uint32, [C.c_void_p]),
            'restart_phase': (C.c_uint32, [C.c_void_p]),
            'restart_tick': (C.c_int, [C.c_void_p]),
            'restart_next': (C.c_uint32, [C.c_void_p, C.c_void_p, C.c_uint32]),
            'restart_end': (C.c_uint32, [C.c_void_p]),
            'probe_reset': (None, []),
            'probe_checkpoint': (C.c_int, [C.c_void_p, C.c_uint32, C.c_uint32, C.c_void_p, C.c_uint32]),
            'probe_publish': (C.c_uint32, [C.c_void_p, C.c_uint32]),
        }
        for name, (result, args) in signatures.items():
            function = getattr(self.lib, 'installer_service_' + name)
            function.restype, function.argtypes = result, args
            setattr(self, '_' + name, function)
        self.lock = threading.RLock()
        self.ctx = self._create(os.fsencode(root))
        if not self.ctx:
            raise RuntimeError('w32srv fixture initialization failed')

    def exchange(self, pid, raw, cap):
        if not 0 <= cap <= LIMIT or len(raw) > LIMIT:
            return 87, b''
        with self.lock:
            out, used = C.create_string_buffer(max(cap, 1)), C.c_uint32()
            error = self._exchange(self.ctx, pid, raw, len(raw), out, cap, C.byref(used))
            if used.value > cap:
                raise RuntimeError('registry reply exceeds copied buffer')
            return error, out.raw[:used.value]

    def observe(self, stage):
        with self.lock:
            return self._observe(self.ctx, stage)

    def close(self):
        with self.lock:
            if self.ctx:
                error = self._destroy(self.ctx)
                self.ctx = None
                if error:
                    raise RuntimeError(f'hive flush failed: {error}')


@dataclass
class Handle:
    kind: int
    value: object
    name: str = ''
    share: int = 7
    access: int = 0
    flags: int = 0


def winerror(error):
    return {errno.ENOENT: 2, errno.ENOTDIR: 3, errno.EACCES: 5, errno.EPERM: 5,
            errno.EBADF: 6, errno.EEXIST: 183, errno.ENOSPC: 112,
            errno.ENOTEMPTY: 145, errno.EROFS: 19, errno.ENAMETOOLONG: 206,
            errno.EINVAL: 87}.get(error.errno, 31)


def receive(sock, count):
    result = bytearray()
    while len(result) < count:
        part = os.read(sock.fileno(), count - len(result))
        if not part:
            if not result:
                return None
            raise EOFError('truncated H1 service message')
        result += part
    return bytes(result)


def send(sock, raw):
    remaining = memoryview(raw)
    while remaining:
        count = os.write(sock.fileno(), remaining)
        if count <= 0:
            raise EOFError('closed H1 service transport')
        remaining = remaining[count:]


class Services:
    def __init__(self, root, library, program=None):
        self.root = Path(root).resolve()
        self.program = program
        for name in ('C/WINDOWS/TEMP', 'C/WINDOWS/SYSTEM', 'C/WINDOWS/SYSTEM32',
                     'C/WINDOWS/Desktop', 'C/WINDOWS/Start Menu/Programs',
                     'C/Program Files', 'C/System/Win32/Fixtures', 'D', 'hives'):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        self.registry = Registry(library, self.root)
        self.sessions, self.children, self.failures, self.trace = [], [], [], []
        self.named = {}
        self.metadata = {}
        self.lock = threading.RLock()
        self.next_pid = 100
        self.disk_full = False

    def session(self, cwd='C:\\WINDOWS\\TEMP'):
        with self.lock:
            self.next_pid += 1
            result = Session(self, self.next_pid, cwd)
            self.sessions.append(result)
            return result

    def start(self, image, command, cwd='C:\\WINDOWS\\TEMP'):
        Session(self, 0, cwd).path(image).stat()  # fail before allocating child handles
        return self.start_adapter(['installer-exec', image, command], image, cwd)

    def start_adapter(self, arguments, label, cwd='C:\\WINDOWS\\TEMP'):
        if self.program is None:
            raise RuntimeError('an H1 executable is required for child creation')
        parent, child = socket.socketpair()
        session = self.session(cwd)
        log = tempfile.TemporaryFile(dir=self.root)
        try:
            process = subprocess.Popen([str(self.program), *arguments,
                                        str(child.fileno())], pass_fds=(child.fileno(),),
                                       stdout=log, stderr=subprocess.STDOUT)
        except BaseException:
            parent.close(); log.close()
            raise
        finally:
            child.close()
        worker = threading.Thread(target=session.serve, args=(parent,), daemon=True)
        result = Child(process, worker, log, session)
        with self.lock:
            self.children.append(result)
            self.trace.append(('spawn', label, process.pid))
        worker.start()
        return result

    def run(self, image, args='', timeout=120):
        command = '"' + image + '"' + (' ' + args if args else '')
        child = self.start(image, command)
        code, output = child.finish(timeout)
        if self.failures:
            raise RuntimeError(str(self.failures))
        return code, output

    def restart(self, checkpoint):
        """Production journal/Wininit/RunOnce sequence; observe while durable."""
        reg = self.registry
        with reg.lock:
            error = reg._restart_begin(reg.ctx)
        if error:
            raise RuntimeError(f'restart begin {error}')
        checkpoint()
        for _ in range(1024):
            with reg.lock:
                if reg._restart_phase(reg.ctx) != 1:
                    break
                reg._restart_tick(reg.ctx)
        else:
            raise RuntimeError('restart WININIT made no progress')
        for _ in range(128):
            command = C.create_string_buffer(2048)
            with reg.lock:
                error = reg._restart_next(reg.ctx, command, len(command))
            if error == 259:
                break
            if error:
                raise RuntimeError(f'RunOnce handoff {error}')
            text = command.value.decode('utf-8')
            image = text[1:text.index('"', 1)] if text.startswith('"') else text.split()[0]
            code, output = self.start(image, text).finish(60)
            if code:
                raise RuntimeError(f'RunOnce child {code}: {output}')
        else:
            raise RuntimeError('unbounded RunOnce queue')
        with reg.lock:
            error = reg._restart_end(reg.ctx)
        if error:
            raise RuntimeError(f'restart end {error}')

    def close(self):
        for child in self.children:
            if child.process.poll() is None:
                child.process.kill()
            child.finish(10)
        for session in self.sessions:
            session.close()
        self.registry.close()
        if self.failures:
            raise RuntimeError(str(self.failures))


class Child:
    def __init__(self, process, worker, log, session):
        self.process, self.worker, self.log, self.session = process, worker, log, session
        self.output = None

    def finish(self, timeout):
        try:
            self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill(); self.process.wait()
            raise
        self.worker.join(timeout=10)
        if self.worker.is_alive():
            raise RuntimeError('H1 service thread did not stop')
        if self.output is None:
            self.log.seek(0)
            self.output = self.log.read().decode('utf-8', errors='replace')
            self.log.close()
        return self.process.returncode, self.output


class Session:
    def __init__(self, service, pid, cwd):
        self.service, self.pid, self.cwd = service, pid, cwd
        self.handles, self.next_handle = {}, 0x100

    def add(self, handle):
        self.next_handle += 4
        self.handles[self.next_handle] = handle
        return self.next_handle

    def fullpath(self, name):
        name = name.replace('/', '\\')
        if len(name) >= 778 or '\0' in name:
            raise OSError(errno.ENAMETOOLONG, 'guest path')
        if name.startswith('\\\\'):
            raise PermissionError(errno.EACCES, 'UNC outside fixture')
        if len(name) >= 2 and name[1] == ':':
            drive, rest = name[0].upper(), name[2:]
            if rest and not rest.startswith('\\'):
                raise OSError(errno.EINVAL, 'drive-relative path unsupported')
        else:
            drive = self.cwd[0]
            rest = name if name.startswith('\\') else self.cwd[2:] + '\\' + name
        if drive not in 'CD':
            raise PermissionError(errno.EACCES, 'fixture drive')
        parts = []
        for part in rest.split('\\'):
            if part in ('', '.'):
                continue
            if part == '..':
                if parts:
                    parts.pop()
            else:
                if ':' in part:
                    raise OSError(errno.EINVAL, 'invalid component')
                parts.append(part)
        return drive + ':\\' + '\\'.join(parts)

    def path(self, name, writing=False):
        full = self.fullpath(name)
        if writing and full[0] == 'D':
            raise OSError(errno.EROFS, 'read-only fixture drive')
        if writing and full[3:].casefold().startswith('system\\'):
            raise PermissionError(errno.EACCES, 'protected system store')
        current = self.service.root / full[0]
        for part in full[3:].split('\\'):
            if not part:
                continue
            if current.is_dir():
                matches = [p.name for p in current.iterdir() if p.name.casefold() == part.casefold()]
                if matches:
                    part = matches[0]
            current /= part
            if current.is_symlink():
                raise PermissionError(errno.EACCES, 'symlink outside fixture')
        return current

    def close(self):
        with self.service.lock:
            for handle in list(self.handles):
                self.close_handle(handle)
        if self.service.registry.ctx:
            with self.service.registry.lock:
                self.service.registry._exit(self.service.registry.ctx, self.pid)

    def close_handle(self, handle):
        h = self.handles.pop(handle)
        if h.kind == 7:
            os.close(h.value)
        if h.kind in (3, 4) and h.name:
            if not any(other.value is h.value for session in self.service.sessions
                       for other in session.handles.values()):
                self.service.named.pop((h.kind, h.name.casefold()), None)

    @staticmethod
    def string(raw, offset):
        if not offset:
            return None
        if offset >= len(raw):
            raise ValueError('pointer offset outside message')
        end = raw.find(0, offset)
        if end < 0 or end - offset >= 32768:
            raise ValueError('unterminated copied string')
        return bytes(raw[offset:end]).decode('utf-8')

    def serve(self, sock):
        try:
            with sock:
                while True:
                    header = receive(sock, 16)
                    if header is None:
                        break
                    magic, kind, size, cap = struct.unpack('<4I', header)
                    if magic != MAGIC or size > LIMIT or cap > LIMIT:
                        raise ValueError('invalid H1 frame')
                    raw = receive(sock, size)
                    if raw is None:
                        raise EOFError('missing H1 payload')
                    status, out = self.dispatch(kind, raw, cap)
                    if len(out) > cap:
                        raise ValueError('oversized H1 reply')
                    send(sock, struct.pack('<iI', status, len(out)) + out)
        except BaseException as error:
            with self.service.lock:
                self.service.failures.append((self.pid, repr(error)))
        finally:
            self.close()

    def dispatch(self, kind, raw, cap):
        if not 0 <= cap <= LIMIT or len(raw) > LIMIT or (kind == 1 and cap < len(raw)):
            return -22, b''
        if kind == 2:
            return self.service.registry.exchange(self.pid, raw, cap)
        if kind == 3:  # startup cwd, copied before any PE executes
            return 0, self.cwd.encode() + b'\0'
        if kind != 1 or len(raw) < 16:
            return -22, b''
        version, size, op, reserved = struct.unpack_from('<4I', raw)
        expected = {FILE: 72, PROCESS: 72, OBJECT: 72, WAIT: 56, SYSTEM: 48}
        if version != 1 or reserved or expected.get(op) != size or len(raw) < size:
            return -22, b''
        data = bytearray(raw)
        a = list(struct.unpack_from('<' + 'I' * (size // 4), raw))
        try:
            if op == FILE:
                with self.service.lock:
                    self.file(a, data)
            elif op == PROCESS:
                image, command, cwd = (self.string(raw, a[i]) for i in (4, 5, 6))
                if a[10] & ~0x08000000 or a[9]:
                    raise OSError(errno.EINVAL, 'unsupported spawn flags/inheritance')
                child = self.service.start(image, command, cwd or self.cwd)
                a[11], a[12] = self.add(Handle(1, child)), self.add(Handle(2, child))
                a[13], a[14], a[15] = child.process.pid, child.process.pid, 0
            elif op == OBJECT:
                with self.service.lock:
                    self.object(a, raw)
            elif op == WAIT:
                self.wait(a, raw)
            elif op == SYSTEM:
                self.system(a)
        except (OSError, KeyError) as error:
            index = {FILE: 17, PROCESS: 15, OBJECT: 15, WAIT: 12, SYSTEM: 11}[op]
            a[index] = 6 if isinstance(error, KeyError) else winerror(error)
        struct.pack_into('<' + 'I' * len(a), data, 0, *a)
        return 0, bytes(data)

    def file(self, a, raw):
        action, handle, path, path2, offset, count = a[4:10]
        writing = action in (8, 9, 10, 11, 12, 13) or (action == 1 and
            (bool(a[10] & 0x40000000) or a[12] in (1, 2, 4, 5)))
        name = self.string(raw, path)
        p = self.path(name, writing) if name is not None else None
        def output(data):
            if not offset or offset > len(raw) or len(data) > count or count > len(raw) - offset:
                raise OSError(errno.EINVAL, 'output outside copied buffer')
            raw[offset:offset + len(data)] = data
        a[16], a[17] = 0, 0
        if action == 1:
            access, share, disposition = a[10:13]
            exists = p.exists()
            if disposition not in range(1, 6):
                raise OSError(errno.EINVAL, 'disposition')
            for session in self.service.sessions:
                for h in session.handles.values():
                    if h.kind == 7 and h.name == str(p) and ((access & 0x80000000 and not h.share & 1) or
                        (access & 0x40000000 and not h.share & 2) or
                        (h.access & 0x80000000 and not share & 1) or (h.access & 0x40000000 and not share & 2)):
                        a[17] = 32
                        return
            flags = os.O_RDWR if access & 0x40000000 and access & 0x80000000 else os.O_WRONLY if access & 0x40000000 else os.O_RDONLY
            flags |= {1: os.O_CREAT | os.O_EXCL, 2: os.O_CREAT | os.O_TRUNC,
                      3: 0, 4: os.O_CREAT, 5: os.O_TRUNC}[disposition]
            try:
                fd = os.open(p, flags, 0o600)
            except FileExistsError:
                a[17] = 80
                return
            a[16] = self.add(Handle(7, fd, str(p), share, access, a[13]))
            if exists and disposition in (2, 4):
                a[17] = 183
            self.service.trace.append(('open', str(p), disposition))
        elif action in (2, 3, 4, 5, 6):
            h = self.handles[handle]
            if h.kind != 7:
                raise KeyError(handle)
            if action in (2, 3):
                if action == 2:
                    data = os.pread(h.value, count, a[14] | a[15] << 32) if a[13] & 0x10000 else os.read(h.value, count)
                    output(data); a[16] = len(data)
                else:
                    if self.service.disk_full:
                        raise OSError(errno.ENOSPC, 'injected full fixture volume')
                    if offset > len(raw) or count > len(raw) - offset:
                        raise OSError(errno.EINVAL, 'input outside copied buffer')
                    data = bytes(raw[offset:offset + count])
                    a[16] = os.pwrite(h.value, data, a[14] | a[15] << 32) if a[13] & 0x10000 else os.write(h.value, data)
                    self.service.trace.append(('write', h.name, a[16]))
            elif action == 4:
                position = a[14] | a[15] << 32
                if position & (1 << 63): position -= 1 << 64
                position = os.lseek(h.value, position, a[13])
                a[14], a[15] = position & 0xffffffff, position >> 32
            elif action == 5:
                os.ftruncate(h.value, os.lseek(h.value, 0, os.SEEK_CUR))
            else:
                os.fsync(h.value)
        elif action in (7, 8, 9):
            st = p.stat() if p is not None else os.fstat(self.handles[handle].value)
            target = str(p) if p is not None else self.handles[handle].name
            metadata = self.service.metadata.get(target)
            if action == 7:
                output(self.info(st, p.name if p else '', metadata))
            elif action == 8:
                mode = st.st_mode
                os.chmod(p or self.handles[handle].name, mode & ~0o222 if a[13] & 1 else mode | 0o200)
            else:
                if not offset or count != 832 or offset > len(raw) or count > len(raw) - offset:
                    raise OSError(errno.EINVAL, 'timestamp input')
                current = list(struct.unpack_from('<5H', self.info(st, '', metadata), 12))
                incoming = struct.unpack_from('<5H', raw, offset + 12)
                for mask, indices in ((1, (0, 1)), (2, (2,)), (4, (3, 4))):
                    if a[13] & mask:
                        for index in indices: current[index] = incoming[index]
                self.service.metadata[target] = current
        elif action in (10, 11):
            for session in self.service.sessions:
                for h in session.handles.values():
                    if h.kind == 7 and h.name == str(p) and not h.share & 4:
                        a[17] = 32
                        return
            if action == 10:
                p.unlink()
            else:
                target = self.path(self.string(raw, path2), True)
                if target.exists() and not a[13] & 1:
                    a[17] = 183
                    return
                p.rename(target)
        elif action == 12: p.mkdir()
        elif action == 13: p.rmdir()
        elif action == 14:
            if not p.is_dir(): raise NotADirectoryError(errno.ENOTDIR, 'cwd')
            self.cwd = self.fullpath(name)
        elif action in (15, 16):
            text = (self.cwd if action == 15 else self.fullpath(name)).encode() + b'\0'
            a[16] = len(text)
            if len(text) > count: a[17] = 122
            else: output(text)
        elif action in (17, 18):
            if action == 17:
                pattern = p.name.casefold()
                # Win32 *.* also matches names with no dot.
                matches = sorted((q for q in p.parent.iterdir() if fnmatch.fnmatchcase(q.name.casefold(),
                    '*' if pattern == '*.*' else pattern)), key=lambda q: q.name.casefold())
                if not matches: a[17] = 2; return
                handle = self.add(Handle(13, iter(matches)))
                a[16] = handle
            try: entry = next(self.handles[handle].value)
            except StopIteration: a[17] = 18; return
            output(self.info(entry.stat(), entry.name))
        elif action == 19:
            free = 128 * 1024 * 1024
            output(struct.pack('<10I64s16s', 12, 3, 0x4349554b, 4096, 512, 255,
                               free, 0, 256 * 1024 * 1024, 0, b'CIUKI-FIXTURE', b'FAT32'))
        else:
            a[17] = 120

    @staticmethod
    def info(st, name, metadata=None):
        size = st.st_size if not stat.S_ISDIR(st.st_mode) else 0
        attributes = 16 if stat.S_ISDIR(st.st_mode) else 128
        if not st.st_mode & 0o222: attributes |= 1
        def dos_date(value):
            t = datetime.fromtimestamp(value, timezone.utc)
            if t.year < 1980: return 33, 0
            return (min(t.year, 2107) - 1980) << 9 | t.month << 5 | t.day, t.hour << 11 | t.minute << 5 | t.second // 2
        create_date, create_time = dos_date(st.st_ctime)
        access_date, _ = dos_date(st.st_atime)
        write_date, write_time = dos_date(st.st_mtime)
        dates = metadata or (create_date, create_time, access_date, write_date, write_time)
        return struct.pack('<III5HBB768s40s', attributes, size & 0xffffffff, size >> 32,
                           *dates, 0, 0, name.encode(), b'')

    def object(self, a, raw):
        action, kind, handle = a[4:7]
        a[15] = 0
        if action in (1, 2) and kind in (3, 4):
            name = self.string(raw, a[11])
            value = {'set': bool(a[12]) or kind == 4, 'manual': bool(a[13]) or kind == 4}
            if name:
                key = (kind, name.casefold())
                if action == 2 and key not in self.service.named:
                    a[15] = 2
                    return
                if key in self.service.named:
                    value = self.service.named[key]
                    if action == 1: a[15] = 183
                else: self.service.named[key] = value
            a[14] = self.add(Handle(kind, value, name or '', flags=a[10] & 1))
        elif action == 3:
            self.close_handle(handle); a[14] = 1
        elif action == 9:
            h = self.handles[handle]
            a[5], a[10] = h.kind, h.flags
            if h.kind in (1, 2):
                code = h.value.process.poll()
                a[14], a[16] = 259 if code is None else code & 0xffffffff, h.value.process.pid
            else: a[14] = 0
        elif action in (5, 6, 8):
            h = self.handles[handle]
            h.value['set'] = action != 6
            a[14] = 1
        elif action == 11:
            self.handles[handle].flags = a[12] & 1
            a[14] = 1
        else: a[15] = 120

    def wait(self, a, raw):
        pointer, count, all_, timeout = a[4], a[5], a[6], a[8]
        if count > 64 or pointer > len(raw) or count * 4 > len(raw) - pointer:
            raise OSError(errno.EINVAL, 'wait array')
        handles = [self.handles[v] for v in struct.unpack_from('<' + 'I' * count, raw, pointer)]
        deadline = float('inf') if timeout == 0xffffffff else time.monotonic() + timeout / 1000
        while True:
            ready = [(h.value.process.poll() is not None if h.kind in (1, 2) else bool(h.value['set'])) for h in handles]
            if handles and (all(ready) if all_ else any(ready)):
                selected = list(range(count)) if all_ else [ready.index(True)]
                for index in selected:
                    h = handles[index]
                    if h.kind == 3 and not h.value['manual']: h.value['set'] = False
                a[11], a[12] = selected[0], 0
                self.service.trace.append(('wait', self.pid, selected[0]))
                return
            if time.monotonic() >= deadline:
                a[11], a[12] = 258, 0
                return
            time.sleep(0.001)

    @staticmethod
    def system(a):
        a[11] = 0
        if a[4] == 1:
            value = (time.time_ns() // 100) + 116444736000000000
            a[7], a[8] = value & 0xffffffff, value >> 32
        elif a[4] == 2:
            a[7], a[8], a[9] = 65536, 32768, 0
        elif a[4] == 6:
            time.sleep(0)
        else: a[11] = 120
