"""Host binding to the freestanding production NE parser; never executes NE code."""
# SPDX-License-Identifier: MIT
import ctypes as C
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
U8, U16, U32 = C.c_uint8, C.c_uint16, C.c_uint32


class Error(C.Structure):
    _fields_ = [(key, U32) for key in ('rule', 'file_offset', 'segment', 'record')]


class String(C.Structure):
    _fields_ = [('offset', U32), ('length', U32)]


class Segment(C.Structure):
    _fields_ = [(key, U32) for key in ('file_offset', 'stored_size', 'initialized_size',
                                     'allocation', 'reloc_first', 'reloc_count')]
    _fields_ += [('sector', U16), ('flags', U16)]


class Entry(C.Structure):
    _fields_ = [(key, U16) for key in ('kind', 'flags', 'segment', 'offset')]


class Name(C.Structure):
    _fields_ = [('text', String), ('ordinal', U16), ('nonresident', U16)]


class ID(C.Structure):
    _fields_ = [('integer', U32), ('text', String)]


class Resource(C.Structure):
    _fields_ = [('type', ID), ('name', ID), ('file_offset', U32), ('size', U32),
                ('flags', U16), ('reserved', U16)]


class Reloc(C.Structure):
    _fields_ = [(key, U16) for key in ('segment', 'source', 'target1', 'target2')]
    _fields_ += [('source_type', U8), ('flags', U8), ('reserved', U16)]


class Image(C.Structure):
    _fields_ = [('bytes', C.POINTER(U8))]
    _fields_ += [(key, U32) for key in ('size', 'ne_offset', 'valid')]
    _fields_ += [(key, U16) for key in ('flags', 'autodata', 'heap', 'stack', 'ip', 'cs', 'sp', 'ss',
                                     'alignment', 'resource_alignment', 'expected_version', 'target',
                                     'linker_version', 'other_flags', 'alignment_stored',
                                     'selfload_version')]
    _fields_ += [(key, U32) for key in ('autodata_bytes', 'selfload_boot', 'selfload_loadseg',
                                     'selfload_exit', 'selfload_state')]
    _fields_ += [(key, U32) for key in ('segment_count', 'entry_count', 'name_count', 'module_count',
                                     'resource_count', 'reloc_count')]
    _fields_ += [('segments', Segment * 254), ('entries', Entry * 1024), ('names', Name * 512),
                ('modules', String * 256), ('resources', Resource * 256), ('relocs', Reloc * 8192)]


class Buffer(C.Structure):
    _fields_ = [('bytes', C.POINTER(U8)), ('size', U32), ('selector', U16)]


class Address(C.Structure):
    _fields_ = [('offset', U32), ('selector', U16)]


class Workspace(C.Structure):
    _fields_ = [('bytes', U8 * 65536), ('seen', U8 * 8192)]


Resolver = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(Image), U16, U16, String, C.POINTER(Address))


def library():
    sources = [ROOT/'sdk/win16/ne/ne.c', ROOT/'sdk/win16/ne/ne.h']
    out = ROOT/'build/host/win16/ne-native.so'
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists() or any(s.stat().st_mtime_ns > out.stat().st_mtime_ns for s in sources):
        # Atomic replacement lets read-only census processes share the cache.
        import os
        temp = out.with_name(f'.ne-native-{os.getpid()}.so')
        try:
            subprocess.run(['nice', '-n', '19', 'cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                            '-Werror', '-ffreestanding', '-fno-builtin', '-fPIC', '-shared',
                            str(sources[0]), '-o', str(temp)], check=True, timeout=60)
            temp.replace(out)
        finally:
            temp.unlink(missing_ok=True)
    lib = C.CDLL(str(out))
    lib.ne_parse.argtypes = [C.POINTER(U8), U32, C.POINTER(Image), C.POINTER(Error)]
    lib.ne_parse.restype = C.c_int
    lib.ne_image_bytes.argtypes = []
    lib.ne_image_bytes.restype = U32
    if lib.ne_image_bytes() != C.sizeof(Image):
        raise RuntimeError('ctypes ne_image layout differs from sdk/win16/ne/ne.h')
    lib.ne_rule_name.argtypes = [U32]
    lib.ne_rule_name.restype = C.c_char_p
    lib.ne_load_policy.argtypes = [C.POINTER(Image), C.POINTER(Error)]
    lib.ne_load_segment.argtypes = [C.POINTER(Image), U16, Buffer, C.POINTER(Error)]
    lib.ne_relocate_segment.argtypes = [C.POINTER(Image), U16, C.POINTER(Buffer), U32,
                                      Resolver, C.c_void_p, C.POINTER(Workspace), C.POINTER(Error)]
    return lib


class NEError(ValueError):
    def __init__(self, rule, error=None):
        super().__init__(rule)
        self.rule = rule
        self.location = {} if error is None else {k: getattr(error, k) for k in ('file_offset', 'segment', 'record')}


class Parsed:
    def __init__(self, data):
        if len(data) > 0xffffffff:
            raise NEError('file-size-limit')
        self.data = bytes(data)
        self.backing = (U8 * len(data)).from_buffer_copy(data)
        self.image = Image()
        self.lib = library()
        e = Error()
        if self.lib.ne_parse(self.backing, len(data), C.byref(self.image), C.byref(e)):
            raise NEError(self.lib.ne_rule_name(e.rule).decode(), e)

    def text(self, s):
        # Keep arbitrary bytes deterministic and let census escape control bytes.
        return self.data[s.offset:s.offset+s.length].decode('latin-1')

    def policy(self):
        e = Error()
        if self.lib.ne_load_policy(C.byref(self.image), C.byref(e)):
            return self.lib.ne_rule_name(e.rule).decode()
        return 'ok'
