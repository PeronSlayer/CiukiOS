"""Pure host reference for docs/design/boot-memory.md; no firmware accesses."""
import re

F0_PROBES = ('boot', 'bootinfo', 'allocator', 'protection', 'isolation', 'preempt',
          'localfault', 'syslife', 'panic', 'fpu', 'runner')

F1_PROBES = ('registry', 'input', 'input-fault', 'framebuffer', 'ata', 'ata-fault',
             'partition', 'fat-read', 'fat-write', 'cache', 'mount-crash', 'safe', 'bootlog')
F2_PROBES = ('elf-load', 'spawn-wait', 'fd-table', 'mmap', 'signals-fault',
             'threads-wait', 'crash-isolation', 'libc-smoke', 'app-gate')
F3_PROBES = ('vm-boot', 'vm-memory', 'vm-devices', 'vm-video', 'vm-window', 'vm-audio-state', 'dpmi',
             'dos4gw', 'doom', 'doom-sound', 'wolf3d', 'vm-starve', 'quake', 'vm-fullscreen', 'dos-conformance')
F4_PROBES = ('optical', 'audio-native', 'ntfs-ro', 'vm-audio', 'media-remove', 'vm-audio-jitter')  # F4 probes run by selector only; not in the sweep yet
F5_PROBES = ('f5-core', 'w32-exceptions', 'w32-memory', 'w32-objects', 'w32-dialogs', 'w32-upper-dlls', 'w32-media', 'w32-fixtures', 'w32-kernel32', 'w32-fullscreen', 'w32-registry', 'w32-audio', 'w32-gdi32', 'w32-crash', 'w32-user32', 'w32-codecs', 'w32-ddraw', 'w32-input', 'w32-restart', 'w32-gl')
F6_PROBES = ('net-arp',)  # explicit selector; no F6 hardware sweep yet
F3_SELECTOR_PROBES = ('title',)  # fixture and explicit title ID required
PROBES = (*F0_PROBES, *F1_PROBES, *F2_PROBES, *F3_PROBES)
SELECTOR_RE = r'f([0123456]):([a-z][a-z0-9]*(?:-[a-z0-9]+)*) run=([0-9a-fA-F]{8})( platform=e500)?( safe=1)?'
DOS_SELECTOR_RE = (r'f3:dos-conformance run=([0-9a-fA-F]{8}) '
                   r'guest=(freedos|ciukidos)( kernel=unified)?')

CPU_MINIMUM_ERROR = 'CIUKI: this PC needs an i686 CPU with CMOV'
E820_MINIMUM_ERROR = 'CIUKI: this PC needs a valid BIOS E820 memory map (L:E820)'
VBE_MINIMUM_ERROR = 'CIUKI: this PC needs VBE 2.0 or later for a linear framebuffer'


class LoaderUART:
    """Discovery/disable outcomes in boot-info byte 0x00F (F0-38)."""
    def __init__(self, bda=0x3f8, scratch=True, lsr=0x60,
                 divisor_low=3, divisor_high=0, flags=0):
        self.base = self.divisor = 0
        self.flags = flags & ~2
        self.budget = 16384
        self.sent = bytearray()
        self.state = (2 if bda != 0x3f8 else 3 if not scratch else
                      4 if lsr == 0xff else
                      5 if (divisor_low, divisor_high) != (3, 0) else 1)
        if self.state == 1:
            self.base, self.divisor = 0x3f8, 3
            self.flags |= 2

    def putc(self, value, lsr=0x60):
        if not self.base:
            return
        if not self.budget:
            self.state = 7
        else:
            self.budget -= 1
            if lsr != 0xff and lsr & 0x20:
                self.sent.append(value)
                return
            self.state = 6
        self.base = self.divisor = 0
        self.flags &= ~2


def cpu_minimum(cpuid_present, max_basic_leaf=1, signature=0, features_edx=0):
    """ID-bit probe precedes CPUID; leaf 1 CMOV is the instruction baseline.

    Signature uses display family/model decoding, not a vendor whitelist.
    The diagnostic values do not extend the frozen boot-info v1 ABI.
    """
    if not cpuid_present or max_basic_leaf < 1 or not features_edx & (1 << 15):
        raise ValueError(CPU_MINIMUM_ERROR)
    family = (signature >> 8) & 15
    model = (signature >> 4) & 15
    display_family = family + ((signature >> 20) & 255) if family == 15 else family
    if family in (6, 15):
        model |= ((signature >> 16) & 15) << 4
    return {'signature': signature, 'family': display_family,
            'model': model, 'stepping': signature & 15}


def boot_minimum(cpu, entries, controller=None, complete=True, signature='SMAP'):
    """Pre-disk gate: CPU, normalized E820, then VBE controller query.

    No controller response preserves text/serial recovery. A successful query
    with malformed signature or pre-2.0 version refuses before disk discovery.
    LFB mode eligibility/readback remains the later video selection's job.
    """
    diagnostic = cpu_minimum(**cpu)
    try:
        memory = normalize_e820(entries, complete, signature)
    except ValueError as error:
        raise ValueError(E820_MINIMUM_ERROR) from error
    responding = controller is not None and controller.get('query_ax', 0x4f) == 0x4f
    if responding and (controller.get('signature') != 'VESA' or controller.get('version', 0) < 0x200):
        raise ValueError(VBE_MINIMUM_ERROR)
    return {'cpu': diagnostic, 'memory': memory, 'vbe_present': responding}


def selector(request, source='menu', validated_fw_cfg=False):
    if not isinstance(request, str) or not request.isascii() or len(request) > 64:
        raise ValueError('selector must be at most 64 ASCII bytes')
    if request.startswith('f3:dos-conformance '):
        dos = re.fullmatch(DOS_SELECTOR_RE, request)
        if not dos or (dos[3] and dos[2] != 'ciukidos'):
            raise ValueError('invalid DOS conformance selector')
        return {'phase': 3, 'probe': 'dos-conformance', 'run': dos[1],
                'platform': None, 'safe': False, 'guest': dos[2],
                'kernel': 'unified' if dos[3] else dos[2]}
    title = re.fullmatch(r'f3:title run=([0-9a-fA-F]{8}) title=([a-z][a-z0-9-]{0,22})', request)
    if title:
        return {'phase': 3, 'probe': 'title', 'run': title[1], 'title': title[2], 'platform': None, 'safe': False}
    match = re.fullmatch(SELECTOR_RE, request)
    if not match:
        raise ValueError('invalid selector grammar or probe')
    names = {0:F0_PROBES, 1:F1_PROBES, 2:F2_PROBES, 3:F3_PROBES, 4:F4_PROBES, 5:F5_PROBES, 6:F6_PROBES}[int(match[1])]
    if match[2] not in (*names, *(('all', 'core') if match[1] in ('0', '1') else ())):
        raise ValueError('invalid selector grammar or probe')
    forced = bool(match[4]); safe = bool(match[5])
    if match[1] == '3' and safe:
        raise ValueError('F3 selector has no safe suffix')
    if (forced or safe) and not (source == 'fw_cfg' and validated_fw_cfg):
        raise ValueError('platform override and safe mode require validated QEMU fw_cfg')
    return {'phase': int(match[1]), 'probe': match[2], 'run': match[3], 'platform': 'e500' if forced else None, 'safe': safe}


def menu_choice(choice, safe=False):
    """Normal leaves earlier positive sources intact; S adds safe mode."""
    if choice.lower() not in ('n', 's'):
        raise ValueError('invalid boot menu choice')
    return safe or choice.lower() == 's'


class BootMenu:
    """Replay menu.inc polls with BIOS ticks and optional keyboard/COM1 bytes.

    A poll consumes at most one character before checking the deadline, as the
    loader does. Configured requests are already validated by the cfg parser;
    validated fw_cfg bypasses the menu. No firmware or disk access occurs here.
    `output` is the serial copy; `screen_quiet` models the quiet normal boot
    (docs/boot.md): only MENU_HINT on screen until S, L, H or P is pressed.
    """
    DAY_TICKS = 0x1800b0
    MENU_TICKS = 55
    ENTRY_TICKS = 548
    POLL_LIMIT = 0x2000000
    SAFE_CONFIRMATION = 'Safe mode selected; P for a probe, or wait'
    MENU_HINT = 'Press S for safe mode, L for serial log, H for Hardware check'

    def __init__(self, tick=0, safe=False, serial=True, request=None, source='cfg'):
        self.safe = safe
        self.serial = serial
        self.request = request
        self.source = source if request is not None else 'menu'
        self.line = ''
        self.error = False
        self.output = []
        self.state = 'menu'
        self.verbose = False
        self.hardware_check = False
        self.screen_quiet = False
        if request is not None and source == 'fw_cfg':
            selected = selector(request, source, validated_fw_cfg=True)
            self.safe |= selected['safe']
            self._finish()
        else:
            self.screen_quiet = request is None and not safe
            self.output.append('SELECT_READY')
            self._restart(tick, self.MENU_TICKS)

    def _restart(self, tick, ticks):
        self.start_tick = tick
        self.deadline_ticks = ticks
        self.polls_left = self.POLL_LIMIT

    def _cancel_cfg(self):
        self.request = None
        self.source = 'menu'

    def _finish(self, error=False):
        self.error = error
        if error:
            self.output.append('SELECT_ERROR')
        self.state = 'done'
        self.output.append('L:SELECT_SOURCE=' + self.source)

    def poll(self, tick, char=None):
        if self.state == 'done':
            return
        if char is not None:
            if self.state == 'menu':
                key = char.lower()
                if key == 'n':
                    self._cancel_cfg()
                    self._finish()
                    return
                if key == 's':
                    self._cancel_cfg()
                    self.safe = True
                    self.screen_quiet = False
                    self.output.extend((self.SAFE_CONFIRMATION, 'L:SAFE=1'))
                    self._restart(tick, self.MENU_TICKS)
                    return
                if key == 'p':
                    self._cancel_cfg()
                    self.screen_quiet = False
                    self.state = 'entry'
                    self._restart(tick, self.ENTRY_TICKS)
                    return
                if key == 'h':
                    self._cancel_cfg()
                    self.hardware_check = self.verbose = True
                    self.screen_quiet = False
                    self._finish()
                    return
                if key == 'l':
                    self.serial = not self.serial
                    self.verbose = True
                    self.screen_quiet = False
            elif char in ('\r', '\n'):
                try:
                    selector(self.line)
                except ValueError:
                    self._finish(error=True)
                else:
                    self.request = self.line
                    self._finish()
                return
            elif char == '\b':
                self.line = self.line[:-1]
                return
            elif 32 <= ord(char) <= 126:
                if len(self.line) == 64:
                    self._finish(error=True)
                    return
                self.line += char
        self.polls_left -= 1
        elapsed = (tick - self.start_tick) % self.DAY_TICKS
        if self.polls_left == 0 or elapsed >= self.deadline_ticks:
            self._finish(error=self.state == 'entry')


def boot_options(options, safe=False, serial=True):
    """BOOT.CFG exact tokens, spaces/line endings; positive safe sources persist."""
    if not isinstance(options,str) or not options.isascii() or len(options)>127:
        raise ValueError('SELECT_ERROR: invalid BOOT.CFG extent')
    mode=None
    for token in re.split(r'[ \r\n]+',options):
        if not token:continue
        if token in ('safe=0','safe=1'):
            safe=safe or token=='safe=1'
        elif token in ('serial=0','serial=1'):
            serial=serial and token=='serial=1'
        elif token=='accel=0':
            pass  # Preserved boot-info options; consumed by the kernel.
        elif re.fullmatch(r'mode=0x[0-9a-fA-F]{4}',token) and int(token[7:],16)<=0x3fff:
            mode=int(token[7:],16)
        else:raise ValueError('SELECT_ERROR: malformed BOOT.CFG token')
    return {'safe':safe,'serial':serial,'mode':mode}


def normalize_e820(entries, complete=True, signature='SMAP'):
    """Resolve before rounding. Unknown/conflicting reserved types become type 2.

    ACPI does not order nonusable types by numeric value. Preserve an unambiguous
    nonusable type; conservatively canonicalize conflicts to reserved (type 2).
    All nonusable types outrank usable RAM. Output never exposes their pages.
    """
    if signature != 'SMAP':raise ValueError('invalid E820 signature')
    if not complete or not entries or len(entries) > 128:
        raise ValueError('missing, truncated or oversized E820 map')
    raw = []
    for e in entries:
        size = e.get('record_size', 24)
        try:base, length, kind = e['base'], e['length'], e['type']
        except (KeyError,TypeError) as error:raise ValueError('malformed E820 fields') from error
        ext = 1 if size == 20 else e.get('ext', e.get('attributes', 1))
        if size not in (20, 24) or any(type(v) is not int for v in (base, length, kind, ext)):
            raise ValueError('malformed E820 record')
        if not (0 <= base < 2**64 and 0 < length < 2**64 and base + length < 2**64):
            raise ValueError('zero-length or overflowing E820 range')
        if not (0 <= kind <= 0xffffffff and 0 <= ext <= 0xffffffff):
            raise ValueError('invalid E820 type or attributes')
        if ext & 1:
            raw.append((base, base + length, kind))
    points = sorted({p for e in raw for p in e[:2]})
    output = []
    for start, end in zip(points, points[1:]):
        kinds = {k for a, b, k in raw if a <= start and end <= b}
        if not kinds:
            continue
        reserved = kinds - {1}
        kind = next(iter(reserved)) if len(reserved) == 1 else 2 if reserved else 1
        if start >= end:
            continue
        if output and output[-1]['base'] + output[-1]['length'] == start and output[-1]['type'] == kind:
            output[-1]['length'] += end - start
        else:
            output.append(dict(base=start, length=end-start, type=kind, ext=1))
    rounded = []
    for e in output:
        if e['type'] == 1:
            end = (e['base'] + e['length']) & ~4095
            e['base'] = (e['base'] + 4095) & ~4095
            e['length'] = end - e['base']
        if e['length'] > 0:
            rounded.append(e)
    output = rounded
    if not any(e['type'] == 1 for e in output) or len(output) > 128:
        raise ValueError('no usable pages or normalized map exceeds 128 entries')
    return output


def page_candidates(entries, limit=768*1024*1024):
    return [(max(e['base'], 0x100000), min(e['base']+e['length'], limit))
            for e in normalize_e820(entries) if e['type'] == 1
            and max(e['base'], 0x100000) < min(e['base']+e['length'], limit)]


def mode_eligibility(controller, mode, allow_low_bpp=False):
    """Return an explicit rejection reason, or None. Never repair bad firmware."""
    if controller.get('signature') != 'VESA' or controller.get('version', 0) < 0x200:
        return 'controller'
    if mode.get('query_ax', 0x4f) != 0x4f:
        return 'query'
    attrs = mode['attributes']
    if attrs & 0x91 != 0x91:
        return 'attributes'
    width, height, bpp = mode['width'], mode['height'], mode['bpp']
    if width <= 0 or height <= 0 or mode['memory_model'] != 6 or mode.get('planes', 1) != 1:
        return 'geometry_or_memory_model'
    if bpp not in ((32, 24, 16) if allow_low_bpp else (32, 24)):
        return 'bpp'
    linear = controller['version'] >= 0x300
    pitch = mode['linear_pitch' if linear else 'banked_pitch']
    masks = mode['linear_masks' if linear else 'masks']
    if pitch < width * ((bpp+7)//8):
        return 'scanline'
    capacity = controller.get('total_memory_64k')
    if capacity is None:
        return 'unknown_capacity'
    size = pitch * height
    if size > capacity * 65536:
        return 'capacity'
    base = mode['physical_base']
    if not 0 < base < 2**32 or base + size >= 2**32:
        return 'address'
    occupied = 0
    for name in ('red', 'green', 'blue', 'reserved'):
        bits, position = masks[name]
        if not (0 <= bits <= 8 and 0 <= position <= bpp and bits+position <= bpp):
            return 'masks'
        if name != 'reserved' and bits == 0:
            return 'masks'
        field = ((1 << bits)-1) << position
        if occupied & field:
            return 'masks'
        occupied |= field
    return None


def select_video(controller, modes, configured=None, safe=False):
    rejected = {str(m['mode']): mode_eligibility(controller, m) for m in modes
                if mode_eligibility(controller, m)}
    eligible = [m for m in modes if not mode_eligibility(controller, m)]
    preferences = [(640,480), (800,600), (1024,768)] if safe else [(1024,768), (800,600), (640,480)]
    order = []
    if configured is not None and not safe:
        order.extend(m for m in eligible if m['mode'] == configured)
    for w,h in preferences:
        for bpp in (32,24):
            order.extend(m for m in eligible if (m['width'],m['height'],m['bpp']) == (w,h,bpp)
                         and m not in order)
    for m in order:
        if m.get('set_ok', True) and m.get('readback_mode', m['mode']) & 0x3fff == m['mode']:
            after = m.get('readback_info', m)
            if not mode_eligibility(controller, after) and all(after[k] == m[k] for k in
                    ('width','height','bpp','physical_base','banked_pitch','linear_pitch','masks','linear_masks')):
                return {'mode': m['mode'], 'text': False, 'rejected': rejected}
        rejected[str(m['mode'])] = 'set_or_readback'
    return {'mode': 3, 'text': True, 'rejected': rejected}


def input_policy(pci, pci_bios_ok=True, request=None, validated_fw_cfg=False):
    if request is not None:
        selected = selector(request, 'fw_cfg' if validated_fw_cfg else 'menu', validated_fw_cfg)
        if selected['platform'] == 'e500':
            return {'policy': 1, 'forced': True, 'detected': False}
    ati = any(d['vendor'] == 0x1002 and d['device'] == 0x4c4d for d in pci)
    ess = any(d['vendor'] == 0x125d and d['device'] == 0x1978 and
              d.get('subsystem_vendor') == 0x0e11 and d.get('subsystem_device') == 0xb112 for d in pci)
    detected = bool(pci_bios_ok and ati and ess)
    return {'policy': int(detected), 'forced': False, 'detected': detected}


def display_inventory(controller, modes, current=None, firmware_memory=None):
    """v2 DISP block: bounded copies, first 32 unique eligible ids, boot mode retained.

    Firmware far pointers are converted in the loader, not consumed by the kernel.
    firmware_memory optionally supplies the one-MiB real-mode byte address space.
    """
    result = dict(tag=0x50534944, size=596, vbe_version=0, vram_bytes=0,
                  oem='', vendor='', product='', modes=[])
    if not controller or controller.get('signature') != 'VESA' or controller.get('version', 0) < 0x200:
        return result
    result['vbe_version'] = controller['version']
    result['vram_bytes'] = controller.get('total_memory_64k', 0) * 65536
    for field in ('oem', 'vendor', 'product'):
        text = controller.get(field, '')
        if firmware_memory is not None:
            ptr = controller.get(field + '_ptr', 0)
            at = (ptr >> 16) * 16 + (ptr & 0xffff)
            text = bytes(firmware_memory[at:min(at + 63, 0x100000)]).split(b'\0', 1)[0] if 0 < at < 0x100000 else b''
        if isinstance(text, str): text = text.encode('ascii', 'replace')
        result[field] = ''.join(chr(c) if 32 <= c <= 126 else '?' for c in text[:63].split(b'\0', 1)[0])
    for m in modes:
        if (mode_eligibility(controller, m, allow_low_bpp=True) or not 0x100 <= m['mode'] < 0x200 or
                not 640 <= m['width'] <= 1600 or not 480 <= m['height'] <= 1200): continue
        item = dict(id=m['mode'], width=m['width'], height=m['height'], bpp=m['bpp'],
                    pitch=m['linear_pitch' if result['vbe_version'] >= 0x300 else 'banked_pitch'], linear=1, reserved=0)
        if any(x['id'] == item['id'] for x in result['modes']): continue
        if len(result['modes']) < 32: result['modes'].append(item)
        elif item['id'] == current: result['modes'][-1] = item
    return result


def pack_display_inventory(inventory):
    """Exact NASM/C packed extension, including zero unused entries."""
    import struct
    d = inventory
    strings = [d[k].encode('ascii') + b'\0' for k in ('oem', 'vendor', 'product')]
    data = struct.pack('<IIII64s64s64sI', d['tag'], d['size'], d['vbe_version'], d['vram_bytes'],
                       *strings, len(d['modes']))
    data += b''.join(struct.pack('<HHHHHBB', *(m[k] for k in
                      ('id', 'width', 'height', 'bpp', 'pitch', 'linear', 'reserved'))) for m in d['modes'])
    return data + bytes((32 - len(d['modes'])) * 12)
