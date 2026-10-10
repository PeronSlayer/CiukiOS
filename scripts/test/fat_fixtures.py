"""Bounded, deterministic FAT fixture edits; all LBAs are volume-relative.

Microsoft FAT specification v1.03, pp. 9, 12, 15-18, 21-23:
https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf
FAT[1] clean/error bits are cleared to signal dirty/error, respectively.
FAT32 entries retain their reserved upper nibble; ExtFlags bit 7 must be
clear for the deliberately divergent mirrored-copy fixture. FSInfo signatures
and hints remain intact: these fixtures do not change allocation counts.
"""
import struct
import re

from resources import Refusal

CORRUPTIONS = ('bad-bpb', 'dirty', 'error-flag', 'fat-divergence',
               'chain-corruption', 'torn-sector')
SPEC = 'https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf'


def validate_checker(defect, checked):
    """Classify every diagnostic; fsck 4.2 does not test HrdErrBitMask.

    https://github.com/dosfstools/dosfstools/blob/v4.2/src/check.c
    check_dirty_bits() inspects ClnShutBitMask only. Do not add a second
    corruption merely to get a nonzero checker exit for the error fixture.
    """
    common = [r'fsck\.fat [0-9.]+ \([0-9-]+\)', r'Leaving filesystem unchanged\.',
              r'.*fixture-[0-9]+\.img: [12] files, [0-9]+/[0-9]+ clusters']
    specific = {
        'bad-bpb':[r'Cluster size is zero\.'],
        'dirty':[r'Dirty bit is set\. Fs was not properly unmounted and some data may be corrupt\.',
                 r' Automatically removing dirty bit\.'],
        'error-flag':[],
        'fat-divergence':[r'FATs differ but appear to be intact\.', r'  Using first FAT\.'],
        'chain-corruption':[r'/Ciuki long fixture\.txt', r'  Circular cluster chain\. Truncating to [0-9]+ clusters\.'],
        'torn-sector':[r'/TORNSECT\.BIN', r'  Start cluster beyond limit \([0-9]+ > [0-9]+\)\. Truncating file\.',
                       r'  File size is 1 bytes, cluster chain length is 0 bytes\.', r'  Truncating file to 0 bytes\.']}
    expected = 0 if defect == 'error-flag' else 1
    patterns = common+specific[defect]
    lines = [line for line in checked['output'].splitlines() if line.strip()]
    if checked['returncode'] != expected or not lines or any(not any(re.fullmatch(p, line) for p in patterns) for line in lines):
        raise Refusal('unexpected corruption checker result: '+defect)
    if any(not any(re.fullmatch(p, line) for line in lines) for p in specific[defect]):
        raise Refusal('corruption checker did not detect intended defect: '+defect)
    return dict(classified=True, detects_intended_defect=defect!='error-flag',
                limitation='fsck.fat ignores HrdErrBitMask; verify FAT[1] and guest refusal' if defect=='error-flag' else None)


def geometry(boot):
    u16 = lambda offset: struct.unpack_from('<H', boot, offset)[0]
    u32 = lambda offset: struct.unpack_from('<I', boot, offset)[0]
    spc, reserved, fats = boot[13], u16(14), boot[16]
    total, fat = u16(19) or u32(32), u16(22) or u32(36)
    roots = (u16(17)*32 + 511)//512
    data = reserved + fats*fat + roots
    if u16(11) != 512 or not spc or spc & (spc-1) or not reserved or fats != 2 or not fat or total <= data:
        raise Refusal('invalid generated FAT geometry')
    clusters = (total-data)//spc
    kind = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
    return dict(kind=kind, spc=spc, reserved=reserved, fats=fats, fat_sectors=fat,
                clusters=clusters, data=data, root=data+(u32(44)-2)*spc if kind == 32 else reserved+fats*fat,
                root_cluster=u32(44) if kind == 32 else 0,
                fsinfo=u16(48) if kind == 32 else 0, backup=u16(50) if kind == 32 else 0)


def normalize_seed(image, torn=False):
    """mcopy's creation/access clock is not controlled by the source mtime."""
    with image.open('r+b') as stream:
        g = geometry(stream.read(512))
        stream.seek(g['root']*512); root = bytearray(stream.read(512))
        for offset in range(0, 512, 32):
            if root[offset] == 0:break
            if root[offset] == 0xe5 or root[offset+11] == 15:continue
            root[offset+13:offset+20] = bytes.fromhex('00000021282128')
            root[offset+22:offset+26] = bytes.fromhex('00002128')  # 2000-01-01 UTC
        if torn:
            # Keep the seeded file and its LFN; the half-sector being torn is
            # unused. Deleted padding lets scanners reach that half-sector.
            for offset in range(0, 256, 32):
                if root[offset] == 0:root[offset] = 0xe5
        stream.seek(g['root']*512); stream.write(root)
    return g


def corrupt(image, defect):
    if defect not in CORRUPTIONS:raise Refusal('unknown FAT corruption fixture')
    patches = []
    with image.open('r+b') as stream:
        boot = stream.read(512); g = geometry(boot)
        if g['kind'] not in (16, 32):raise Refusal('corruption fixtures require FAT16/32')
        width = g['kind']//8

        def read(offset, size):
            stream.seek(offset); data = stream.read(size)
            if len(data) != size:raise Refusal('short FAT fixture read')
            return data

        def patch(offset, data, field):
            before = read(offset, len(data))
            if offset//512 != (offset+len(data)-1)//512:raise Refusal('fixture patch crosses sector')
            stream.seek(offset); stream.write(data)
            patches.append(dict(sector=offset//512, offset=offset%512, bytes=len(data),
                                before_hex=before.hex(), after_hex=data.hex(), field=field))

        def fat_offset(copy, cluster):
            return (g['reserved']+copy*g['fat_sectors'])*512+cluster*width

        def entry(copy, cluster):
            return int.from_bytes(read(fat_offset(copy, cluster), width), 'little')

        def set_entry(copy, cluster, value, field):
            if width == 4:value = (entry(copy, cluster)&0xf0000000) | (value&0x0fffffff)
            patch(fat_offset(copy, cluster), value.to_bytes(width, 'little'), field)

        root = read(g['root']*512, 512)
        short = next((root[i:i+32] for i in range(0, 512, 32)
                      if root[i] not in (0, 0xe5) and root[i+11] == 32), None)
        if short is None:raise Refusal('seeded fixture file absent')
        first = struct.unpack_from('<H', short, 26)[0]
        if width == 4:first |= struct.unpack_from('<H', short, 20)[0]<<16
        last = first; seen = set(); mask = 0xffff if width == 2 else 0x0fffffff
        while entry(0, last)&mask < mask-7:
            if last in seen or not 2 <= last <= g['clusters']+1:raise Refusal('invalid seeded file chain')
            seen.add(last); last = entry(0, last)&mask

        if defect == 'bad-bpb':
            for sector in (0, g['backup']) if g['backup'] else (0,):
                patch(sector*512+13, b'\x00', 'BPB_SecPerClus')
        elif defect in ('dirty', 'error-flag'):
            bit = (0x8000 if defect == 'dirty' else 0x4000) if width == 2 else (0x08000000 if defect == 'dirty' else 0x04000000)
            for copy in range(2):set_entry(copy, 1, entry(copy, 1)&~bit, 'FAT[1]')
        elif defect == 'fat-divergence':
            if width == 4 and struct.unpack_from('<H', boot, 40)[0]&0x80:
                raise Refusal('divergence fixture requires FAT mirroring')
            # A different valid EOC in copy 1 produces only copy divergence,
            # without an extra allocation, orphan, or bad chain in either FAT.
            set_entry(1, last, mask-7, 'secondary FAT file EOC')
        elif defect == 'chain-corruption':
            for copy in range(2):set_entry(copy, last, first, 'file tail loops to first cluster')
        else:
            # Exactly half a directory sector: one live entry with an invalid
            # cluster and seven deleted 0xa5-pattern slots. No real file or
            # allocated chain is destroyed by the tear.
            half = bytearray((b'\xe5'+b'\xa5'*31)*8)
            half[:32] = bytes(32); half[:11] = b'TORNSECTBIN'; half[11] = 32
            invalid = g['clusters']+2
            struct.pack_into('<H', half, 26, invalid&0xffff)
            if width == 4:struct.pack_into('<H', half, 20, invalid>>16)
            struct.pack_into('<I', half, 28, 1)
            patch(g['root']*512+256, half, 'torn directory half-sector')
    return dict(defect=defect, specification=SPEC, geometry=g, patches=patches,
                patched_sectors=sorted({p['sector'] for p in patches}))
