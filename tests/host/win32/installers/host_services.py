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
import select
import socket
import shutil
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
REQUEST_TIMEOUT = 30
FILE, SYSTEM, PROCESS = 0x109, 0x10a, 0x10b
OBJECT, WAIT = 0x100, 0x101


# fx-hlrun: the Liberation faces gdi32 loads as its system fonts.
FONTS = ROOT / 'assets/fonts/liberation'


def build_bridge(out, fonts=False):
    """The fixture w32srv library. With fonts, the production font service
    (apps/w32srv/fonts.c, libfont, FreeType) answers gdi32's glyph requests,
    so windows render their text (Services(fonts=True) stages the faces)."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    sources = [ROOT / 'apps/w32srv' / (n + '.c') for n in
               ('registry', 'storage', 'directory', 'defaults', 'posix_io', 'progman', 'restart', 'shell_fs')]
    sources += [ROOT / 'apps/desktop/installer_registry.c', ROOT / 'apps/desktop/protocol.c', HERE / 'service_bridge.c']
    flags = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include')]
    extra, link = [], []
    if fonts:
        import sys
        if str(ROOT) not in sys.path:
            sys.path.insert(0, str(ROOT))
        from sdk.freetype import build as freetype
        archive = freetype.build(out / 'freetype', flags=['-O2', '-fPIC'])
        extra = ['-DH1_FONTS', '-D_GNU_SOURCE', '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include'),
                 *freetype.includes()]
        sources += [ROOT / 'apps/w32srv/fonts.c', ROOT / 'sdk/libfont/font.c']
        link = [str(archive), '-lm']
    library = out / ('installer-services-fonts.so' if fonts else 'installer-services.so')
    subprocess.run([*flags, *extra, '-fPIC', '-shared', '-Wl,--no-undefined', *map(str, sources), *link,
                    '-o', str(library)], check=True)
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
            'snapshot': (C.c_uint32, [C.c_void_p, C.c_void_p, C.c_uint32]),
            'fault': (None, [C.c_void_p, C.c_uint32]),
        }
        for name, (result, args) in signatures.items():
            function = getattr(self.lib, 'installer_service_' + name)
            function.restype, function.argtypes = result, args
            setattr(self, '_' + name, function)
        self.lock = threading.RLock()
        self.trace = []
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
            if len(raw) >= 40 and struct.unpack_from('<I', raw, 4)[0] in (2, 4, 6, 7):
                self.trace.append(bytes(raw))
            return error, out.raw[:used.value]

    def observe(self, stage):
        with self.lock:
            return self._observe(self.ctx, stage)

    def snapshot(self):
        with self.lock:
            out = C.create_string_buffer(LIMIT)
            used = self._snapshot(self.ctx, out, len(out))
        if not used: raise RuntimeError('registry snapshot bounds')
        raw, at, result = out.raw[:used], 0, {}
        while at < len(raw):
            kind, hive, pn, nn, type_, dn = struct.unpack_from('<6I', raw, at)
            at += 24
            path = raw[at:at + pn].decode(); at += pn
            name = raw[at:at + nn].decode(); at += nn
            data = raw[at:at + dn]; at += dn
            key = ('HKLM', 'HKU', 'HKCC', 'HKDD')[hive] + '\\' + path
            if kind: key += '@' + name
            result[key.casefold()] = [type_, data.hex()] if kind else 'key'
        return result

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


# fx-hlis5: FAT 8.3 aliases (Microsoft FAT32 File System Specification 1.03,
# "The Basis-Name Generation Algorithm" and "The Numeric-Tail Generation
# Algorithm"), which the kernel's GetShortPathName returns for long names
# (src/kernel/proc/win32/files.c W32_F_FULLPATH flags 1, px_node alias).
SHORT_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")


def short_aliases(names):
    """{name: 8.3 spelling} for one directory: a valid 8.3 name is its own
    spelling; a long name gets BASIS~N.EXT with the first numeric tail not
    already taken, the names taken in sorted order."""
    def valid(part, limit):
        return 0 < len(part) <= limit and all(c.upper() in SHORT_CHARS for c in part)

    def clean(part):
        return ''.join(c if c in SHORT_CHARS else '_' for c in part.upper().replace(' ', '').replace('.', ''))
    result, taken = {}, set()
    ordered = sorted(names, key=str.casefold)
    for name in ordered:
        stem, dot, ext = name.rpartition('.') if '.' in name.lstrip('.') else (name, '', '')
        if valid(stem, 8) and (not dot or valid(ext, 3)):
            result[name] = name
            taken.add(name.casefold())
    for name in ordered:
        if name in result:
            continue
        stem, dot, ext = name.rpartition('.') if '.' in name.lstrip('.') else (name, '', '')
        basis, extension = clean(stem.lstrip('.')) or '_', clean(ext)[:3]
        for tail in range(1, 1000000):
            suffix = '~%d' % tail
            alias = basis[:8 - len(suffix)] + suffix + ('.' + extension if extension else '')
            if alias.casefold() not in taken:
                break
        result[name] = alias
        taken.add(alias.casefold())
    return result


def winerror(error):
    return {errno.ENOENT: 2, errno.ENOTDIR: 3, errno.EACCES: 5, errno.EPERM: 5,
            errno.EBADF: 6, errno.EEXIST: 183, errno.ENOSPC: 112,
            errno.ENOTEMPTY: 145, errno.EROFS: 19, errno.ENAMETOOLONG: 206,
            errno.EINVAL: 87}.get(error.errno, 31)


def ready(sock, event, deadline):
    poll = select.poll()
    poll.register(sock.fileno(), event)
    while True:
        milliseconds = None
        if deadline is not None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('H1 transport deadline')
            milliseconds = max(1, int(remaining * 1000))
        if poll.poll(milliseconds):
            return


def receive(sock, count, deadline=None, partial=None):
    if deadline is None and sock.gettimeout() is not None:
        deadline = time.monotonic() + sock.gettimeout()
    result = bytearray() if partial is None else partial
    while len(result) < count:
        ready(sock, select.POLLIN, deadline)
        try:
            part = os.read(sock.fileno(), count - len(result))
        except BlockingIOError:
            continue
        if not part:
            if not result:
                return None
            raise EOFError('truncated H1 service message')
        result += part
    return bytes(result)


def send(sock, raw):
    deadline = time.monotonic() + (sock.gettimeout() or REQUEST_TIMEOUT)
    remaining = memoryview(raw)
    while remaining:
        ready(sock, select.POLLOUT, deadline)
        try:
            count = os.write(sock.fileno(), remaining)
        except BlockingIOError:
            continue
        if count <= 0:
            raise EOFError('closed H1 service transport')
        remaining = remaining[count:]


class Services:
    def __init__(self, root, library, program=None, fonts=False):
        self.root = Path(root).resolve()
        self.program = program
        for name in ('C/WINDOWS/TEMP', 'C/WINDOWS/SYSTEM', 'C/WINDOWS/SYSTEM32',
                     'C/WINDOWS/Desktop', 'C/WINDOWS/Start Menu/Programs',
                     'C/Program Files', 'C/System/Win32/Fixtures', 'D', 'hives'):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        if fonts:
            # fx-hlrun: as the image stages them, the pinned system faces
            # in C:\WINDOWS\FONTS for gdi32 (a library built with
            # build_bridge(fonts=True) rasterises them from FONTS).
            (self.root / 'C/WINDOWS/FONTS').mkdir(parents=True, exist_ok=True)
            for face in FONTS.glob('*.ttf'):
                target = self.root / 'C/WINDOWS/FONTS' / face.name
                if not target.is_file():
                    shutil.copyfile(face, target)
            os.environ['CIUKI_H1_FONTS'] = str(FONTS)
        self.registry = Registry(library, self.root)
        self.sessions, self.children, self.failures, self.trace = [], [], [], []
        self.named = {}
        self.metadata = {}
        self.lock = threading.RLock()
        self.next_pid = 100
        self.disk_full = False
        self.wait_timeout_once = False
        self.request_timeout = REQUEST_TIMEOUT
        self.spawning = threading.local()   # fx-hlis5: STARTUPINFO of the spawn in progress
        self.environment = None             # fx-hlis5: session environment block, None: fixed

    def session(self, cwd='C:\\WINDOWS\\TEMP'):
        with self.lock:
            self.next_pid += 1
            result = Session(self, self.next_pid, cwd)
            self.sessions.append(result)
            return result

    def start(self, image, command, cwd='C:\\WINDOWS\\TEMP', flags=0):
        Session(self, 0, cwd).path(image).stat()  # fail before allocating child handles
        return self.start_adapter(['installer-exec', image, command], image, cwd, flags)

    def start_adapter(self, arguments, label, cwd='C:\\WINDOWS\\TEMP', flags=0):
        if self.program is None:
            raise RuntimeError('an H1 executable is required for child creation')
        parent, child = socket.socketpair()
        session = self.session(cwd)
        log = tempfile.TemporaryFile(dir=self.root)
        try:
            process = subprocess.Popen([str(self.program), *arguments,
                                        str(child.fileno())], pass_fds=(child.fileno(),),
                                       stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=bool(flags & 0x200))
        except BaseException:
            parent.close(); log.close()
            raise
        finally:
            child.close()
        # The PE learns its process identity from the adapter (its Linux
        # PID); w32srv authenticates window records by the caller's PID, so
        # the serving session carries that same identity. Nothing has been
        # dispatched for this session yet.
        with self.lock:
            session.pid = process.pid
            session.image = label  # the parent image directory of its own spawns
            session.startup = getattr(getattr(self, 'spawning', None), 'startup', None)
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

    def restart(self, checkpoint=lambda: None, interrupt=lambda event: None):
        """Production journal/Wininit/RunOnce sequence; observe while durable."""
        reg = self.registry
        with reg.lock:
            error = reg._restart_begin(reg.ctx)
        if error:
            raise RuntimeError(f'restart begin {error}')
        checkpoint()
        interrupt('journal_persisted')
        for _ in range(1024):
            with reg.lock:
                if reg._restart_phase(reg.ctx) != 1:
                    break
                reg._restart_tick(reg.ctx)
            interrupt('wininit_tick')
        else:
            raise RuntimeError('restart WININIT made no progress')
        interrupt('wininit_retired')
        for _ in range(128):
            command = C.create_string_buffer(2048)
            with reg.lock:
                error = reg._restart_next(reg.ctx, command, len(command))
            if error == 259:
                break
            if error:
                raise RuntimeError(f'RunOnce handoff {error}')
            text = command.value.decode('utf-8')
            interrupt('runonce_consumed')
            image = text[1:text.index('"', 1)] if text.startswith('"') else text.split()[0]
            code, output = self.start(image, text).finish(60)
            if code:
                raise RuntimeError(f'RunOnce child {code}: {output}')
            interrupt('runonce_executed')
        else:
            raise RuntimeError('unbounded RunOnce queue')
        with reg.lock:
            error = reg._restart_end(reg.ctx)
        if error:
            raise RuntimeError(f'restart end {error}')

    def close(self):
        errors = []
        # Kill every live descendant before joining any service thread: a
        # parent's thread may be serving an INFINITE wait on a child (the
        # IS5 bootstrap waits for its engine) and only returns once that
        # child has exited too.
        for child in list(self.children):
            try:
                if child.process.poll() is None:
                    child.session.terminating = True
                    child.process.kill()
            except Exception as error:
                errors.append(repr(error))
        for child in list(self.children):
            try:
                child.finish(10)
            except Exception as error:
                errors.append(repr(error))
        for session in self.sessions:
            try:
                session.close()
            except Exception as error:
                errors.append(repr(error))
        try:
            temporary = list((self.root / 'C/WINDOWS/TEMP').iterdir())
        except Exception as error:
            errors.append(repr(error)); temporary = []
        for path in temporary:
            try:
                if path.is_dir() and not path.is_symlink(): shutil.rmtree(path)
                else: path.unlink()
            except Exception as error:
                errors.append(repr(error))
        try:
            self.registry.close()
        except Exception as error:
            errors.append(repr(error))
        if errors or self.failures:
            raise RuntimeError(str(errors + self.failures))

    def assert_quiescent(self, temporary=()):
        for child in self.children:
            if child.process.poll() is not None: child.finish(10)
        alive = [c.process.pid for c in self.children if c.process.poll() is None or c.worker.is_alive()]
        if alive:
            raise AssertionError(f'surviving installer descendants: {alive}')
        actual = {p.relative_to(self.root / 'C/WINDOWS/TEMP').as_posix()
                  for p in (self.root / 'C/WINDOWS/TEMP').rglob('*')}
        if actual != set(temporary):
            raise AssertionError(f'unexpected temporary files: {actual ^ set(temporary)}')

    def finish_descendants(self, timeout=60):
        deadline = time.monotonic() + timeout
        index = 0
        while index < len(self.children):
            code, output = self.children[index].finish(max(0.001, deadline - time.monotonic()))
            if code: raise AssertionError(f'descendant exit {code}: {output}')
            index += 1


class Child:
    def __init__(self, process, worker, log, session):
        self.process, self.worker, self.log, self.session = process, worker, log, session
        self.output = None

    def finish(self, timeout):
        try:
            self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            self.session.terminating = True
            self.process.kill(); self.process.wait()
            error.add_note('H1 timeout: ' + self.session.diagnostic())
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
        self.terminating = False
        self.last_operation = 'none'
        self.last_reply = 'none'
        self.pending_header, self.pending_payload = bytearray(), bytearray()
        self.expected_payload = None

    def search(self, image):
        """The kernel's CreateProcess search (src/kernel/proc/win32/process.c,
        the Windows order): an explicit path is used as given; a bare name,
        with .exe appended when it has no extension, is looked up in the
        parent's image directory, the parent's current directory,
        C:\\WINDOWS\\SYSTEM and C:\\WINDOWS. The adapter's loader would treat a
        bare name as a DLL search (fx-is5boot: the IS5 engine's final launch)."""
        if not image or any(c in image for c in '\\/:'):
            return image
        name = image if '.' in image else image + '.exe'
        parent = getattr(self, 'image', '') or ''
        directories = [parent.rsplit('\\', 1)[0]] if '\\' in parent else []
        directories += [self.cwd, 'C:\\WINDOWS\\SYSTEM', 'C:\\WINDOWS']
        for directory in directories:
            candidate = directory.rstrip('\\') + '\\' + name
            try:
                if self.path(candidate).is_file():
                    return self.fullpath(candidate)
            except OSError:
                continue
        return image

    def short_path(self, full):
        """GetShortPathName of an existing path: each component in its 8.3
        spelling, no trailing separator (the kernel walks the resolved
        nodes, src/kernel/proc/win32/files.c W32_F_FULLPATH)."""
        parts = [x for x in full[3:].split('\\') if x]
        spelled = full[:3]
        for i, part in enumerate(parts):
            directory = self.path(spelled)
            if not directory.is_dir():
                raise FileNotFoundError(errno.ENOENT, 'short path')
            names = [p.name for p in directory.iterdir()]
            match = [n for n in names if n.casefold() == part.casefold()]
            if not match:
                match = [n for n, alias in short_aliases(names).items() if alias.casefold() == part.casefold()]
            if not match:
                raise FileNotFoundError(errno.ENOENT, 'short path')
            spelled += ('\\' if i else '') + short_aliases(names)[match[0]]
        return spelled

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
        if writing and (full[3:].casefold() == 'system' or full[3:].casefold().startswith('system\\') or full[3:] == ''):
            raise PermissionError(errno.EACCES, 'protected system store')
        current = self.service.root / full[0]
        for part in full[3:].split('\\'):
            if not part:
                continue
            if current.is_dir():
                names = [p.name for p in current.iterdir()]
                matches = [n for n in names if n.casefold() == part.casefold()]
                if matches:
                    part = matches[0]
                elif '~' in part:
                    # fx-hlis5: an 8.3 alias (GetShortPathName) names its long entry.
                    matches = [n for n, alias in short_aliases(names).items() if alias.casefold() == part.casefold()]
                    if matches:
                        part = matches[0]
            current /= part
            if current.is_symlink():
                raise PermissionError(errno.EACCES, 'symlink outside fixture')
        return current

    def handle_path(self, h, writing=False):
        # A handle follows the object through renames. Never trust its old
        # spelling for authority; /proc is adapter-only, not PE I/O. An
        # unlinked open object still has valid descriptor-based metadata.
        current = os.readlink(f'/proc/self/fd/{h.value}')
        if not os.fstat(h.value).st_nlink and current.endswith(' (deleted)'):
            current = current[:-10]
        current = Path(current)
        try:
            relative = current.relative_to(self.service.root)
        except ValueError:
            raise PermissionError(errno.EACCES, 'handle outside fixture') from None
        parts = relative.parts
        if not parts or parts[0] not in ('C', 'D'):
            raise PermissionError(errno.EACCES, 'handle outside fixture drive')
        return self.path(parts[0] + ':\\' + '\\'.join(parts[1:]), writing)

    def close(self):
        errors = []
        with self.service.lock:
            for handle in list(self.handles):
                try: self.close_handle(handle)
                except Exception as error: errors.append(repr(error))
        if self.service.registry.ctx:
            with self.service.registry.lock:
                self.service.registry._exit(self.service.registry.ctx, self.pid)
        if errors: raise RuntimeError(str(errors))

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

    def describe(self, op, raw):
        """Names in a complete kind-1 request: the FILE path, the spawned image."""
        try:
            if op == FILE and len(raw) >= 72:
                named = self.string(raw, struct.unpack_from('<I', raw, 24)[0])
                return ' path=' + repr(named) if named is not None else ''
            if op == PROCESS and len(raw) >= 72:
                spawned = self.string(raw, struct.unpack_from('<I', raw, 16)[0])
                return ' image=' + repr(spawned) if spawned is not None else ''
        except ValueError:
            pass
        return ''

    @staticmethod
    def describe_registry(raw):
        if len(raw) < 40:
            return ''
        fields = struct.unpack_from('<10I', raw)
        text = f' key=0x{fields[2]:x}'
        if len(raw) >= 40 + fields[7] + fields[8]:
            text += (' path=' + repr(bytes(raw[40:40 + fields[7]]).rstrip(b'\0').decode('utf-8', 'replace')) +
                     ' name=' + repr(bytes(raw[40 + fields[7]:40 + fields[7] + fields[8]]).rstrip(b'\0').decode('utf-8', 'replace')))
        return text

    @staticmethod
    def reply_error(out, status):
        if status != 0:
            return ''
        if len(out) >= 72:
            op = struct.unpack_from('<I', out, 8)[0]
            index = {FILE: 17, PROCESS: 15, OBJECT: 15, WAIT: 12, SYSTEM: 11}.get(op)
            if index is not None and len(out) >= (index + 1) * 4:
                return f' error={struct.unpack_from("<I", out, index * 4)[0]}'
        elif len(out) >= 8:
            return f' error={struct.unpack_from("<I", out, 4)[0]}'
        return ''

    def diagnostic(self):
        return (f'last served operation={self.last_operation}; last reply={self.last_reply}; '
                f'partial header={len(self.pending_header)}/16:{self.pending_header[:128].hex()}; '
                f'partial payload={len(self.pending_payload)}/{self.expected_payload}:'
                f'{self.pending_payload[:128].hex()}')

    def serve(self, sock):
        try:
            with sock:
                sock.setblocking(False)
                while True:
                    self.pending_header, self.pending_payload = bytearray(), bytearray()
                    self.expected_payload = None
                    complete = False
                    detail = ''
                    try:
                        # One deadline covers the header and the entire body;
                        # a peer dribbling bytes cannot reset it indefinitely.
                        deadline = time.monotonic() + self.service.request_timeout
                        header = receive(sock, 16, deadline, self.pending_header)
                        if header is None:
                            break
                        magic, kind, size, cap = struct.unpack('<4I', header)
                        self.expected_payload = size
                        if magic != MAGIC or size > LIMIT or cap > LIMIT:
                            raise ValueError('invalid H1 frame')
                        raw = receive(sock, size, deadline, self.pending_payload)
                        if raw is None:
                            raise EOFError('missing H1 payload')
                        complete = True
                        operation = f'kind={kind} size={size} cap={cap}'
                        if kind == 1 and len(raw) >= 20:
                            op, action = struct.unpack_from('<I', raw, 8)[0], struct.unpack_from('<I', raw, 16)[0]
                            operation += f' op=0x{op:x} action={action}'
                            detail = self.describe(op, raw)
                        elif kind == 2 and len(raw) >= 8:
                            operation += f' registry_op={struct.unpack_from("<I", raw, 4)[0]}'
                            detail = self.describe_registry(raw)
                        self.last_operation = operation
                        status, out = self.dispatch(kind, raw, cap)
                        if len(out) > cap:
                            raise ValueError('oversized H1 reply')
                    except Exception as error:
                        # A session the test is ending may be killed between
                        # two requests (fx-hlrun: a polling wait loop).
                        if self.terminating and isinstance(error, (BrokenPipeError, ConnectionResetError, EOFError)):
                            break
                        status, out = (-errno.ETIMEDOUT if isinstance(error, TimeoutError)
                                       else -errno.EINVAL if isinstance(error, (ValueError, struct.error))
                                       else -errno.EIO), b''
                        with self.service.lock:
                            self.service.failures.append((self.pid, repr(error), self.diagnostic()))
                    # Even a malformed/truncated/timed-out request receives one
                    # error reply. After incomplete framing close the stream:
                    # late bytes cannot safely become another request header.
                    sock.settimeout(self.service.request_timeout)
                    send(sock, struct.pack('<iI', status, len(out)) + out)
                    self.last_reply = f'status={status} bytes={len(out)}'
                    with self.service.lock:
                        # f5-15B: the sixth element names the request (paths,
                        # spawned image, registry path/name) and the reply's
                        # Win32 error; the operation string is unchanged.
                        self.service.trace.append(('reply', self.pid, self.last_operation, status, len(out),
                                                   detail + self.reply_error(out, status)))
                    if not complete:
                        break
        except BaseException as error:
            if not (self.terminating and isinstance(error, (BrokenPipeError, ConnectionResetError, EOFError))):
                with self.service.lock:
                    self.service.failures.append((self.pid, repr(error)))
        finally:
            try:
                self.close()
            except Exception as error:
                with self.service.lock:
                    self.service.failures.append((self.pid, repr(error)))

    def dispatch(self, kind, raw, cap):
        if not 0 <= cap <= LIMIT or len(raw) > LIMIT or (kind == 1 and cap < len(raw)):
            return -22, b''
        if kind == 2:
            return self.service.registry.exchange(self.pid, raw, cap)
        if kind == 3:  # startup cwd, copied before any PE executes
            return 0, self.cwd.encode() + b'\0'
        if kind == 4:
            # fx-hlis5: the creator's STARTUPINFO flags/show and the session
            # environment (h1_services.c h1_installer_init); none: fixed values.
            startup = getattr(self, 'startup', None)
            environment = getattr(self.service, 'environment', None)
            if not startup and environment is None:
                return -22, b''
            flags, show = startup or (0, 0)
            return 0, struct.pack('<II', flags, show) + (environment or b'')
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
                # The reused advpack launches its waited child with
                # CREATE_DEFAULT_ERROR_MODE | CREATE_NEW_PROCESS_GROUP.
                # Each adapter starts with fresh core DLL error-mode state.
                if a[10] & ~(0x08000000 | 0x04000000 | 0x200) or a[9]:
                    raise OSError(errno.EINVAL, 'unsupported spawn flags/inheritance')
                # fx-hlis5: the caller's STARTUPINFO (flags, show) for the
                # child's parameters; bit 31 marks a supplied record.
                spawning = getattr(self.service, 'spawning', None)
                if spawning is not None:
                    spawning.startup = (a[16] & 0xffff, (a[16] >> 16) & 0x7fff) if a[16] & 0x80000000 else None
                try:
                    child = self.service.start(self.search(image), command, cwd or self.cwd, a[10])
                finally:
                    if spawning is not None:
                        spawning.startup = None
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
            if a[13] & 0x04000000:
                a[17] = 120  # H1 explicitly refuses FILE_FLAG_DELETE_ON_CLOSE.
                return
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
            if action in (3, 5):
                self.handle_path(h, True)
            if action in (2, 3):
                if action == 2:
                    data = os.pread(h.value, count, a[14] | a[15] << 32) if a[13] & 0x10000 else os.read(h.value, count)
                    # ReadFile with nNumberOfBytesToRead == 0 succeeds with 0
                    # bytes (Windows; the kernel's W32_F_READ does the same).
                    # The adapter copies no buffer for it, so there is no
                    # output region to validate. NSIS 2.51's MoveFileOnReboot
                    # reads the whole freshly created, empty WININIT.INI.
                    if count: output(data)
                    a[16] = len(data)
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
            fd = None
            if p is None:
                h = self.handles[handle]
                if h.kind != 7:
                    raise KeyError(handle)
                p = self.handle_path(h, action in (8, 9))
                fd = h.value
            st = p.stat() if fd is None else os.fstat(fd)
            target = (st.st_dev, st.st_ino)
            metadata = self.service.metadata.get(target)
            if action == 7:
                output(self.info(st, p.name if p else '', metadata))
            elif action == 8:
                mode = st.st_mode
                mode = mode & ~0o222 if a[13] & 1 else mode | 0o200
                if fd is None: os.chmod(p, mode)
                else: os.fchmod(fd, mode)
            else:
                if not offset or count not in (832, 844) or offset > len(raw) or count > len(raw) - offset:
                    raise OSError(errno.EINVAL, 'timestamp input')
                # 844 = sizeof(struct ciuki_w32_file_info) since f5-12B amendment 4
                # (file_id and volume_serial appended); 832 is the pre-identity layout.
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
            full = self.cwd if action == 15 else self.fullpath(name)
            if action == 16 and a[13] & 1:
                full = self.short_path(full)
            text = full.encode() + b'\0'
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
        if self.service.wait_timeout_once and any(h.kind == 1 for h in handles):
            self.service.wait_timeout_once = False
            a[11], a[12] = 258, 0
            return
        deadline = float('inf') if timeout == 0xffffffff else time.monotonic() + timeout / 1000
        while True:
            ready = [(h.value.process.poll() is not None if h.kind in (1, 2) else bool(h.value['set'])) for h in handles]
            if handles and (all(ready) if all_ else any(ready)):
                selected = list(range(count)) if all_ else [ready.index(True)]
                for index in selected:
                    h = handles[index]
                    if h.kind in (1, 2): h.value.finish(10)
                    if h.kind == 3 and not h.value['manual']: h.value['set'] = False
                a[11], a[12] = selected[0], 0
                self.service.trace.append(('wait', self.pid, selected[0]))
                return
            if time.monotonic() >= deadline:
                a[11], a[12] = 258, 0
                return
            time.sleep(0.001)

    def system(self, a):
        a[11] = 0
        if a[4] == 1:
            value = (time.time_ns() // 100) + 116444736000000000
            a[7], a[8] = value & 0xffffffff, value >> 32
        elif a[4] == 2:
            a[7], a[8], a[9] = 65536, 32768, 0
        elif a[4] == 6:
            time.sleep(0)
        elif a[4] == 3:
            child = self.handles[a[5]]
            if child.kind != 1: raise KeyError(a[5])
            child.value.session.terminating = True
            child.value.process.kill()
            a[10] = 1
        else: a[11] = 120
