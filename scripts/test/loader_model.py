"""Pure host reference for docs/design/boot-memory.md; no firmware accesses."""
import re

PROBES = ('boot', 'bootinfo', 'allocator', 'protection', 'isolation', 'preempt',
          'localfault', 'syslife', 'panic', 'fpu', 'runner')


def selector(request, source='menu', validated_fw_cfg=False):
    if not isinstance(request, str) or not request.isascii() or len(request) > 64:
        raise ValueError('selector must be at most 64 ASCII bytes')
    match = re.fullmatch(r'f0:([a-z]+) run=([0-9a-fA-F]{8})( platform=e500)?( safe=1)?', request)
    if not match or match[1] not in (*PROBES, 'all', 'core'):
        raise ValueError('invalid selector grammar or probe')
    forced = bool(match[3]); safe = bool(match[4])
    if (forced or safe) and not (source == 'fw_cfg' and validated_fw_cfg):
        raise ValueError('platform override and safe mode require validated QEMU fw_cfg')
    return {'probe': match[1], 'run': match[2], 'platform': 'e500' if forced else None, 'safe': safe}


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
        if not (0 <= bits <= bpp and 0 <= position <= bpp and bits+position <= bpp):
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
            return {'policy': 1, 'forced': True}
    ati = any(d['vendor'] == 0x1002 and d['device'] == 0x4c4d for d in pci)
    ess = any(d['vendor'] == 0x125d and d['device'] == 0x1978 and
              d.get('subsystem_vendor') == 0x0e11 and d.get('subsystem_device') == 0xb112 for d in pci)
    return {'policy': int(pci_bios_ok and ati and ess), 'forced': False}
