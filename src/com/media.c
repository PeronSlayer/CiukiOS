/* Read-only removable-media browser. No installed filesystem/interrupt hooks.
 * FAT layout: Microsoft FAT specification 1.03; CD: ECMA-119, clauses 8/9.
 * Firmware USB disks require BIOS exposure; this is not a USB host stack.
 */
#ifdef MEDIA_HOST
#include <stdint.h>
typedef uint32_t u32;
#else
typedef unsigned long u32;
#endif
typedef unsigned short u16;
typedef unsigned char u8;
#ifdef MEDIA_MODULE
#include "media_driver_abi.h"
/* Initialized storage permits the host to populate this exported packet
 * before the module's first call clears its private BSS. */
MediaRequest media_request = { .version = MD_ABI };
typedef char media_request_size_check[(sizeof(MediaRequest) == 1192) ? 1 : -1];
static u16 mounted_device;
#endif

u16 bi_ax, bi_bx, bi_cx, bi_dx, bi_si, bi_di, bi_flags;
extern void bios13(void), dos21(void), key16(void);
extern u16 own_segment(void), timer_ticks(void), port_in(u16), port_inw(u16);
extern void port_out(u16, u16), port_outw(u16, u16);
static u8 block[2048], fatblock[512], dap[16], parameters[74];
static u8 bios_storage[4096];
static u8 drive, edd, optical, atapi_device, fatbits, spc;
static u16 spt, heads, atapi_base, bytes_per_sector, iso_volume;
static u32 base, total, fat_start, fat_sectors, root_start, root_sectors;
static u32 data_start, clusters, root_cluster, iso_root_size;
static u32 capacity;
static const char *error;
static char cwd[192], input[192], command_tail[128];

typedef struct { char name[40]; u32 cluster, size; u8 directory; } Entry;
typedef struct { u32 cluster, size, position, steps, anchor, power, period; u8 fixed_root; } Reader;

static u16 word(const u8 *p) { return p[0] | (u16)p[1] << 8; }
void *memcpy(void *destination, const void *source, unsigned int count) {
    u8 *d = destination; const u8 *s = source;
    while (count--) *d++ = *s++;
    return destination;
}
static u32 dword(const u8 *p) { return word(p) | (u32)word(p + 2) << 16; }
static void putword(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
static void putdword(u8 *p, u32 v) { putword(p, (u16)v); putword(p+2, (u16)(v>>16)); }
static void zero(void *p, u16 n) { u8 *q = p; while (n--) *q++ = 0; }
static u16 length(const char *s) { u16 n = 0; while (s[n]) ++n; return n; }
static void copy(char *d, const char *s) { while ((*d++ = *s++) != 0) {} }
static u8 upper(u8 c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int same(const char *a, const char *b) {
    while (*a && upper(*a) == upper(*b)) { ++a; ++b; }
    return upper(*a) == upper(*b);
}
static int prefix(const char *s, const char *p) {
    while (*p) if (upper(*s++) != upper(*p++)) return 0;
    return 1;
}
static int fail(const char *s) { error = s; return 0; }
static void out(char c) {
#ifdef MEDIA_MODULE
    (void)c; /* GUI service must never write to the BIOS/DOS text console. */
#else
    bi_ax = 0x0200; bi_dx = (u8)c; dos21();
#endif
}
static void text(const char *s) { while (*s) out(*s++); }
static void newline(void) { text("\r\n"); }
static void decimal(u32 n) {
    char digits[11]; u16 i = 0;
    do { digits[i++] = '0' + n % 10; n /= 10; } while (n);
    while (i) out(digits[--i]);
}
static void hexbyte(u8 n) { const char *h = "0123456789ABCDEF"; out(h[n>>4]); out(h[n&15]); }
static u16 key(void) { bi_ax = 0; key16(); return bi_ax; }
static int line(char *s, u16 limit) {
    u16 n = 0, k;
    for (;;) {
        k = key() & 255;
        if (k == 27) { s[0] = 0; newline(); return 0; }
        if (k == 13) { s[n] = 0; newline(); return 1; }
        if (k == 8 && n) { --n; text("\b \b"); }
        else if (k >= 32 && k < 127 && n + 1 < limit) { s[n++] = k; out(k); }
    }
}
static char *token(char **cursor) {
    char *p = *cursor, *result;
    while (*p == ' ') ++p;
    result = p;
    if (*p == '"') {
        result = ++p;
        while (*p && *p != '"') ++p;
    } else while (*p && *p != ' ') ++p;
    if (*p) *p++ = 0;
    *cursor = p;
    return result;
}

/* Every call rebuilds the DAP including count; firmware may modify it. */
static int bios_read(u32 lba, u8 *dest) {
    u16 attempt;
    /* ISA DMA and older BIOS implementations reject transfers crossing a
     * physical 64 KiB boundary, regardless of the segment:offset spelling.
     * A 2 KiB physical alignment also covers floppy's 512-byte DMA sector. */
    u16 physical_low = (u16)(own_segment() << 4) + (u16)bios_storage;
    u8 *io = bios_storage + ((0-physical_low) & 2047);
    if (capacity && lba >= capacity) return fail("Sector is outside this device.");
    for (attempt = 0; attempt < 3; ++attempt) {
        if (edd) {
            zero(dap, sizeof(dap)); dap[0] = 16;
            putword(dap + 2, 1); putword(dap + 4, (u16)io);
            putword(dap + 6, own_segment()); putdword(dap + 8, lba);
            bi_ax = 0x4200; bi_si = (u16)dap; bi_dx = drive;
        } else {
            u32 track, cylinder;
            u16 sector, head;
            if (!spt || !heads) return fail("BIOS disk geometry is unavailable.");
            track = lba / spt; sector = lba % spt + 1;
            cylinder = track / heads; head = track % heads;
            if (cylinder > 1023) return fail("Sector exceeds the BIOS CHS limit.");
            bi_ax = 0x0201; bi_bx = (u16)io;
            bi_cx = ((u16)cylinder & 255) << 8 | sector | ((u16)cylinder >> 2 & 0xc0);
            bi_dx = head << 8 | drive;
        }
        bios13();
        if (!(bi_flags & 1)) { memcpy(dest, io, bytes_per_sector); return 1; }
        bi_ax = 0; bi_dx = drive; bios13();
    }
    return fail("BIOS read failed. Check that the medium is inserted.");
}

static int bios_device(u8 id) {
    drive = id; edd = 0; capacity = 0; bytes_per_sector = 512;
    bi_ax = 0x4100; bi_bx = 0x55aa; bi_dx = id; bios13();
    if (!(bi_flags & 1) && bi_bx == 0xaa55 && (bi_cx & 1)) edd = 1;
    if (edd) {
        zero(parameters, sizeof(parameters)); putword(parameters, sizeof(parameters));
        bi_ax = 0x4800; bi_dx = id; bi_si = (u16)parameters; bios13();
        if (!(bi_flags & 1)) {
            bytes_per_sector = word(parameters + 24);
            if (!dword(parameters + 20)) capacity = dword(parameters + 16);
        }
        return 1;
    }
    bi_ax = 0x0800; bi_dx = id; bios13();
    if (bi_flags & 1) return 0;
    spt = bi_cx & 63; heads = (bi_dx >> 8) + 1;
    capacity = ((bi_cx >> 8) | ((bi_cx & 0xc0) << 2)) + 1;
    capacity *= (u32)spt * heads;
    return spt && heads;
}

/* Bounded PIO packet command. No channel reset and no writes to disk media.
 * Reading the status register acknowledges device interrupts while polling.
 */
static int atapi_wait(int want_drq) {
    u32 n = 20000000UL;
    u16 status, start = timer_ticks();
    do {
        status = port_in(atapi_base + 7);
        if (!status || status == 255) return 0;
        if (!(status & 0x80)) {
            /* An earlier CHECK CONDITION leaves ERR set until the next
             * command; REQUEST SENSE must still be issuable in that state. */
            if (want_drq < 0 && !(status & 8)) return 1;
            if (status & 0x21) return 0;
            if (!!(status & 8) == want_drq) return 1;
        }
        /* Spindle spin-up and an actual seek take much longer than QEMU's
         * immediate DRQ. Five seconds, including wrap-safe tick arithmetic;
         * the poll budget still terminates if firmware disables the timer. */
        if (!(n & 63) && (u16)(timer_ticks()-start) >= 91) return 0;
    } while (--n);
    return 0;
}
static int atapi_packet(const u8 *packet, u8 *dest, u16 expected) {
    u16 i, count, value, received = 0;
    port_out(atapi_base + 6, atapi_device);
    for (i = 0; i < 4; ++i) port_in(atapi_base + 7);
    if (!atapi_wait(-1)) return 0;
    port_out(atapi_base + 1, 0);
    port_out(atapi_base + 4, expected & 255);
    port_out(atapi_base + 5, expected >> 8);
    port_out(atapi_base + 7, 0xa0);
    if (!atapi_wait(1) || (port_in(atapi_base + 2) & 3) != 1) return 0;
    for (i = 0; i < 12; i += 2) port_outw(atapi_base, word(packet+i));
    while (received < expected) {
        if (!atapi_wait(1) || (port_in(atapi_base + 2) & 3) != 2) return 0;
        count = port_in(atapi_base + 4) | port_in(atapi_base + 5) << 8;
        if (!count || count > expected - received) return 0;
        for (i = 0; i < count; i += 2) {
            value = port_inw(atapi_base);
            dest[received++] = value;
            if (i + 1 < count) dest[received++] = value >> 8;
        }
    }
    return atapi_wait(0);
}
static int cd_read(u32 lba, u8 *dest) {
    u8 packet[12], sense[18]; u16 attempt;
    if (!atapi_base) return bios_read(lba, dest);
    for (attempt = 0; attempt < 3; ++attempt) {
        zero(packet, sizeof(packet)); packet[0] = 0xa8;
        packet[2] = lba >> 24; packet[3] = lba >> 16;
        packet[4] = lba >> 8; packet[5] = lba; packet[9] = 1;
        if (atapi_packet(packet, dest, 2048)) return 1;
        zero(packet, sizeof(packet)); packet[0] = 3; packet[4] = sizeof(sense);
        /* Unit-attention is acknowledged by REQUEST SENSE before retry. */
        atapi_packet(packet, sense, sizeof(sense));
    }
    return fail("CD read failed or no data CD is inserted.");
}
static int read_block(u32 lba) { return optical ? cd_read(lba, block) : bios_read(lba, block); }

static int mount_fat(void) {
    u32 fatsize, sectors, overhead, partition_length = 0;
    u16 reserved, roots, fats, i;
    u8 part[16];
    optical = 0; base = 0;
    if (bytes_per_sector != 512) return fail("Only 512-byte FAT sectors are supported.");
    if (!read_block(0)) return 0;
    if (word(block + 510) != 0xaa55) return fail("No FAT boot-sector signature.");
    if (word(block + 11) != 512) {
        for (i = 0; i < 4; ++i) {
            u8 type = block[446 + i * 16 + 4];
            if (type == 1 || type == 4 || type == 6 || type == 0x0b || type == 0x0c || type == 0x0e) break;
        }
        if (i == 4) return fail("No primary FAT12/16/32 partition. GPT and extended partitions are unsupported.");
        for (fats = 0; fats < 16; ++fats) part[fats] = block[446 + i*16 + fats];
        base = dword(part + 8); partition_length = dword(part + 12);
        if (!base || !partition_length || base + partition_length < base ||
            (capacity && base + partition_length > capacity)) return fail("Invalid partition boundaries.");
        if (!read_block(base)) return 0;
    }
    if (word(block + 510) != 0xaa55 || word(block + 11) != 512) return fail("Invalid FAT boot sector.");
    spc = block[13]; reserved = word(block+14); fats = block[16]; roots = word(block+17);
    sectors = word(block+19); if (!sectors) sectors = dword(block+32);
    fatsize = word(block+22); if (!fatsize) fatsize = dword(block+36);
    if (!spc || spc > 128 || (spc & (spc-1)) || !reserved || !fats || fats > 2 ||
        !fatsize || fatsize > 0x1000000UL || !sectors || sectors > 0xfffffffUL)
        return fail("Invalid FAT geometry.");
    root_sectors = ((u32)roots * 32 + 511) / 512;
    overhead = reserved + (u32)fats * fatsize + root_sectors;
    if (sectors <= overhead || base + sectors < base ||
        (capacity && base + sectors > capacity) || (partition_length && sectors > partition_length))
        return fail("FAT volume exceeds the device.");
    clusters = (sectors - overhead) / spc;
    fatbits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    if ((fatbits == 32 && (roots || word(block+22) || word(block+42))) ||
        (fatbits != 32 && (!roots || !word(block+22)))) return fail("FAT layout/type mismatch.");
    if ((clusters + 2) * (fatbits == 32 ? 4UL : fatbits == 16 ? 2UL : 3UL) >
        fatsize * (fatbits == 12 ? 1024UL : 512UL)) return fail("FAT table is too short.");
    total = sectors; fat_start = base + reserved; fat_sectors = fatsize;
    root_start = base + reserved + (u32)fats * fatsize;
    data_start = base + overhead; root_cluster = fatbits == 32 ? dword(block + 44) & 0x0fffffffUL : 0;
    if (fatbits == 32) {
        u16 flags = word(block+40);
        if (root_cluster < 2 || root_cluster >= clusters+2) return fail("Invalid FAT32 root cluster.");
        if (flags & 0x80) {
            if ((flags & 15) >= fats) return fail("Invalid active FAT.");
            fat_start += (flags & 15) * fatsize;
        }
    }
    return 1;
}
static int mount_iso(void) {
    u16 i;
    optical = 1;
    for (i = 16; i < 80; ++i) {
        if (!read_block(i)) return 0;
        if (block[1] != 'C' || block[2] != 'D' || block[3] != '0' || block[4] != '0' || block[5] != '1' || block[6] != 1)
            return fail("This disc has no ISO 9660 volume descriptor.");
        if (block[0] == 255) break;
        if (block[0] == 1) {
            if (word(block+128) != 2048 || block[156] < 34 || !(block[181] & 2))
                return fail("Unsupported ISO 9660 volume layout.");
            total = dword(block+80); root_cluster = dword(block+158) + block[157]; iso_root_size = dword(block+166);
            iso_volume = word(block+124);
            if (!total || !iso_root_size || root_cluster >= total ||
                (iso_root_size-1)/2048 >= total-root_cluster) return fail("Invalid CD root extent.");
            return 1;
        }
    }
    return fail("Primary ISO 9660 volume descriptor not found.");
}
static int select_cd(int force_ide) {
    u16 i, j, status;
    atapi_base = 0;
    if (!force_ide) for (i = 0xe0; i < 0xe8; ++i)
        if (bios_device(i) && edd && bytes_per_sector == 2048 && mount_iso()) return 1;
    /* Legacy ATA channels cover the T23 and ordinary IDE CD-ROM machines.
     * Native-mode PCI/SATA/AHCI and USB optical controllers need drivers. */
    for (i = 0; i < 2; ++i) for (j = 0; j < 2; ++j) {
        atapi_base = i ? 0x170 : 0x1f0; atapi_device = j ? 0xb0 : 0xa0;
        port_out(atapi_base+6, atapi_device);
        port_in(atapi_base+7); port_in(atapi_base+7); port_in(atapi_base+7); port_in(atapi_base+7);
        status = port_in(atapi_base+7);
        if (!status || status == 255 || (status & 0x80)) continue;
        /* IDENTIFY PACKET is non-destructive and rejected by ATA disks. */
        port_out(atapi_base+7, 0xa1);
        if (!atapi_wait(1)) continue;
        for (status = 0; status < 256; ++status) port_inw(atapi_base);
        if (mount_iso()) return 1;
    }
    return fail("No readable BIOS/legacy IDE ISO 9660 CD. Insert a data CD and try again.");
}

static u32 next_cluster(u32 cluster) {
    u32 offset, value; u16 index; u8 first;
    if (cluster < 2 || cluster >= clusters + 2) { fail("Invalid cluster in FAT chain."); return 0; }
    offset = fatbits == 12 ? cluster + cluster/2 : cluster * (fatbits == 16 ? 2 : 4);
    if (offset/512 >= fat_sectors || !bios_read(fat_start+offset/512, fatblock)) return 0;
    index = offset % 512;
    if (fatbits == 12) {
        first = fatblock[index];
        if (index == 511) {
            if (offset/512 + 1 >= fat_sectors || !bios_read(fat_start+offset/512+1, fatblock)) return 0;
            value = first | (u16)fatblock[0] << 8;
        } else value = word(fatblock+index);
        value = cluster & 1 ? value >> 4 : value & 0xfff;
        if (value >= 0xff8) return 0xffffffffUL;
        if (value >= 0xff0) { fail("Reserved or bad FAT12 cluster."); return 0; }
    } else if (fatbits == 16) {
        value = word(fatblock+index);
        if (value >= 0xfff8) return 0xffffffffUL;
        if (value >= 0xfff0) { fail("Reserved or bad FAT16 cluster."); return 0; }
    } else {
        value = dword(fatblock+index) & 0x0fffffffUL;
        if (value >= 0x0ffffff8UL) return 0xffffffffUL;
        if (value >= 0x0ffffff0UL) { fail("Reserved or bad FAT32 cluster."); return 0; }
    }
    if (value < 2 || value >= clusters+2) { fail("FAT chain points outside the volume."); return 0; }
    return value;
}
static Entry root(void) {
    Entry e;
    zero(&e, sizeof(e)); copy(e.name, "/"); e.directory = 1; e.cluster = root_cluster;
    e.size = optical ? iso_root_size : root_cluster ? 0xffffffffUL : root_sectors * 512;
    return e;
}
static void reader_open(Reader *r, const Entry *e) {
    r->cluster = e->cluster; r->size = e->size; r->position = 0; r->steps = 0;
    r->anchor = e->cluster; r->power = 1; r->period = 0;
    r->fixed_root = !optical && e->directory && !e->cluster;
}
/* Read one 512/2048-byte logical chunk into block; 0=EOF, -1=error. */
static int reader_next(Reader *r) {
    u32 sector, within; u16 count;
    if (r->position >= r->size) return 0;
    count = optical ? 2048 : 512;
    if (optical) {
        sector = r->cluster + r->position/2048;
        if (sector < r->cluster || sector >= total) { fail("CD extent is outside the volume."); return -1; }
    } else if (r->fixed_root) sector = root_start + r->position/512;
    else {
        within = r->position / 512 % spc;
        if (r->position && !within) {
            if (++r->steps > clusters) { fail("FAT chain is cyclic."); return -1; }
            r->cluster = next_cluster(r->cluster);
            if (!r->cluster) return -1;
            if (r->cluster == 0xffffffffUL) {
                if (r->size == 0xffffffffUL) return 0;
                fail("File ends before its declared size."); return -1;
            }
            /* Brent cycle detection uses constant memory and no extra I/O. */
            if (r->cluster == r->anchor) { fail("FAT chain is cyclic."); return -1; }
            if (++r->period == r->power) {
                r->anchor = r->cluster; r->period = 0; r->power *= 2;
            }
        }
        if (r->cluster < 2 || r->cluster >= clusters+2) { fail("Invalid starting cluster."); return -1; }
        sector = data_start + (r->cluster-2)*spc + within;
    }
    if (!read_block(sector)) return -1;
    if (r->size-r->position < count) count = (u16)(r->size-r->position);
    r->position += count;
    return count;
}

/* Directory walk never retains pointers into the shared I/O buffer. */
static int directory(const Entry *dir, const char *wanted, Entry *result, int listing) {
    Reader r; Entry e; u32 records = 0; u16 index, n, i, k; int count;
    if (!dir->directory) return fail("This path is not a directory.");
    reader_open(&r, dir);
    while ((count = reader_next(&r)) > 0) {
        index = 0;
        while (index < (u16)count) {
            u8 *p = block + index;
            if (++records > 65536UL) return fail("Directory is too large or cyclic.");
            zero(&e, sizeof(e));
            if (optical) {
                n = p[0]; if (!n) break;
                if (n < 34 || index+n > (u16)count || p[32] > n-33) return fail("Malformed CD directory record.");
                index += n;
                if (p[32] == 1 && p[33] <= 1) continue;
                if (p[25] & 0x80) return fail("Multi-extent CD files are unsupported.");
                if (p[26] || p[27] || word(p+28) != iso_volume)
                    return fail("Interleaved files and multi-volume CDs are unsupported.");
                e.cluster = dword(p+2) + p[1]; e.size = dword(p+10); e.directory = !!(p[25]&2);
                if (e.cluster < dword(p+2)) return fail("Invalid CD file extent.");
                n = p[32]; if (n >= sizeof(e.name)) continue;
                for (i = 0; i < n && p[33+i] != ';'; ++i) e.name[i] = p[33+i];
                if (i && e.name[i-1] == '.') --i;
                e.name[i] = 0;
            } else {
                if (!p[0]) return wanted ? fail("File or directory not found.") : 1;
                index += 32;
                if (p[0] == 0xe5 || p[11] == 0x0f || (p[11]&8) || p[0] == '.') continue;
                k = 0;
                for (i = 0; i < 8 && p[i] != ' '; ++i) e.name[k++] = p[i];
                if (p[8] != ' ') {
                    e.name[k++] = '.';
                    for (i = 8; i < 11 && p[i] != ' '; ++i) e.name[k++] = p[i];
                }
                e.name[k] = 0; e.cluster = word(p+26);
                if (fatbits == 32) e.cluster |= (u32)(word(p+20)&0x0fff) << 16;
                e.size = dword(p+28); e.directory = !!(p[11]&16);
                if (e.directory) e.size = 0xffffffffUL;
            }
            if (wanted && same(wanted, e.name)) { *result = e; return 1; }
            if (listing) {
#ifdef MEDIA_MODULE
                u16 item = media_request.total;
                if (item == 0xffff) return fail("Directory contains too many items.");
                ++media_request.total;
                if (item >= media_request.first && media_request.count < MD_ROWS) {
                    MediaRow *row = &media_request.rows[media_request.count++];
                    copy(row->name, e.name);
                    row->size = e.directory ? 0 : e.size;
                    row->directory = e.directory;
                }
#else
                text(e.directory ? " <DIR>  " : "        "); text(e.name);
                if (!e.directory) { text("  "); decimal(e.size); text(" bytes"); }
                newline();
#endif
            }
        }
    }
    if (count < 0) return 0;
    return wanted ? fail("File or directory not found.") : 1;
}

static int full_path(const char *path, char *dest) {
    u16 n, i; char component[40];
    if (*path == '/' || *path == '\\') { copy(dest, "/"); ++path; }
    else copy(dest, cwd);
    while (*path) {
        while (*path == '/' || *path == '\\') ++path;
        if (!*path) break;
        i = 0;
        while (*path && *path != '/' && *path != '\\') {
            if (i+1 >= sizeof(component)) return fail("Path component is too long.");
            component[i++] = *path++;
        }
        component[i] = 0;
        if (same(component, ".")) continue;
        n = length(dest);
        if (same(component, "..")) {
            if (n > 1) { while (--n && dest[n] != '/') {} dest[n ? n : 1] = 0; }
            continue;
        }
        if (n + i + 2 >= sizeof(cwd)) return fail("Path is too long.");
        if (n > 1) dest[n++] = '/';
        copy(dest+n, component);
    }
    return 1;
}
static int resolve(const char *path, Entry *e) {
    char absolute[192], part[40]; u16 i; const char *p;
    if (!full_path(path, absolute)) return 0;
    *e = root(); p = absolute+1;
    while (*p) {
        i = 0;
        while (*p && *p != '/') part[i++] = *p++;
        part[i] = 0;
        if (!directory(e, part, e, 0)) return 0;
        if (*p) ++p;
    }
    return 1;
}
static int transfer(const Entry *e, const char *destination) {
    Reader r; int n; u16 handle = 0xffff, i; u8 previous = 0;
    if (e->directory) return fail("Select a file, not a directory.");
    if (destination) {
        /* CiukiDOS has no AH=5Bh. In its synchronous single-task execution,
         * probe then create cannot race another process. Refuse any failure
         * other than file-not-found, and never truncate an existing file. */
        bi_ax = 0x3d00; bi_dx = (u16)destination; dos21();
        if (!(bi_flags & 1)) {
            bi_bx = bi_ax; bi_ax = 0x3e00; dos21();
            return fail("Destination already exists; COPY will not overwrite it.");
        }
        if (bi_ax != 2) return fail("Cannot access the destination path.");
        bi_ax = 0x3c00; bi_cx = 0; bi_dx = (u16)destination; dos21();
        if (bi_flags & 1) return fail("Cannot create destination (it may already exist).");
        handle = bi_ax;
    }
    reader_open(&r, e);
    while ((n = reader_next(&r)) > 0) {
        if (destination) {
            bi_ax = 0x4000; bi_bx = handle; bi_cx = n; bi_dx = (u16)block; dos21();
            if ((bi_flags & 1) || bi_ax != (u16)n) { fail("Destination write failed or disk is full."); n = -1; break; }
        } else {
            for (i = 0; i < (u16)n; ++i) {
                u8 c = block[i];
                if ((c < 32 && c != 9 && c != 10 && c != 13) || c > 126) {
                    fail("Binary file: use COPY to read it with a suitable application."); n = -1; break;
                }
                if (c == 10 && previous != 13) out(13);
                out(c);
                previous = c;
            }
            if (n < 0) break;
        }
    }
    if (destination) {
        bi_ax = 0x3e00; bi_bx = handle; dos21();
        if (bi_flags & 1) { fail("Destination close failed."); n = -1; }
        if (n < 0) { bi_ax = 0x4100; bi_dx = (u16)destination; dos21(); }
        else { text("Copied "); decimal(e->size); text(" bytes to "); text(destination); newline(); }
    } else newline();
    return n >= 0;
}

static void help(void) {
    text("DIR [path]     CD path     TYPE file\r\n"
         "COPY file destination    HELP    EXIT\r\n"
         "Use quoted paths for names containing spaces. COPY never overwrites.\r\n"
         "Source media are read-only. FAT uses DOS short names; CD uses ISO 9660 names.\r\n");
}
static int run(char *s) {
    char *cursor = s, *cmd = token(&cursor), *path = token(&cursor), *dest;
    Entry e; char absolute[192];
    if (!*cmd) return 1;
    if (same(cmd, "HELP")) { help(); return 1; }
    if (same(cmd, "EXIT")) return 2;
    if (same(cmd, "DIR")) return resolve(path, &e) && directory(&e, 0, 0, 1);
    if (same(cmd, "CD")) {
        if (!*path) { text(cwd); newline(); return 1; }
        if (!resolve(path, &e)) return 0;
        if (!e.directory) return fail("This path is not a directory.");
        if (!full_path(path, absolute)) return 0;
        copy(cwd, absolute); return directory(&e, 0, 0, 1);
    }
    if (same(cmd, "TYPE")) return resolve(path, &e) && transfer(&e, 0);
    if (same(cmd, "COPY")) {
        dest = token(&cursor);
        if (!*path || !*dest) return fail("Usage: COPY source destination");
        return resolve(path, &e) && transfer(&e, dest);
    }
    return fail("Unknown command. Type HELP.");
}

#ifndef MEDIA_MODULE
int media_main(void) {
    u16 i; int ok = 0, rc; char *cursor, *device; Entry e;
    const u8 *psp = (const u8 *)0x80;
    for (i = 0; i < psp[0] && i < sizeof(command_tail)-1; ++i) command_tail[i] = psp[i+1];
    command_tail[i] = 0; cursor = command_tail; device = token(&cursor);
    copy(cwd, "/"); error = "No readable medium was found.";
    text("CiukiOS Media\r\n");
    if (same(device, "FLOPPY")) {
        if (bios_device(0)) ok = mount_fat();
    } else if (same(device, "CD") || same(device, "CD:IDE")) ok = select_cd(same(device, "CD:IDE"));
    else if (prefix(device, "USB")) {
        u16 first = 0x80, last = 0x88;
        text("BIOS disks: USB storage must be exposed by firmware before boot.\r\n");
        if (device[3] == ':' && length(device) == 6) {
            u8 a = upper(device[4]), b = upper(device[5]);
            if (a >= '0' && a <= '9' && ((b >= '0' && b <= '9') || (b >= 'A' && b <= 'F'))) {
                first = (a-'0')*16 + (b <= '9' ? b-'0' : b-'A'+10); last = first + 1;
            }
        }
        for (i = first; i < last; ++i) {
            if (bios_device(i) && mount_fat()) {
                /* Avoid automatically selecting the installed/live system.
                 * An explicit USB:80 is available for an intentional read. */
                if (last-first > 1 && block[3]=='C' && block[4]=='I' && block[5]=='U' && block[6]=='K') continue;
                text("Selected BIOS disk "); hexbyte(i); newline(); ok = 1; break;
            }
        }
        if (!ok && last-first > 1) error = "No additional BIOS FAT disk. Enable USB legacy storage and connect it before boot.";
    } else {
        text("Usage: MEDIA FLOPPY|USB[:80..87]|CD [DIR|TYPE|COPY ...]\r\n");
        help(); return 1;
    }
    while (*cursor == ' ') ++cursor;
    if (!ok) {
        text("Error: "); text(error); newline();
        /* Desktop device icons have no subcommand. Keep their exact error
         * readable until dismissed instead of erasing it on GUI return. */
        if (!*cursor) { text("Press any key to return to CiukiOS."); key(); newline(); }
        return 1;
    }
    text(optical ? "ISO 9660 CD, read-only.\r\n" : "FAT source, read-only.\r\n");
    if (*cursor) {
        rc = run(cursor);
        if (!rc) { text("Error: "); text(error); newline(); }
        return !rc;
    }
    help(); e = root();
    if (!directory(&e, 0, 0, 1)) { text("Error: "); text(error); newline(); }
    for (;;) {
        text("Media "); text(cwd); text("> ");
        if (!line(input, sizeof(input))) break;
        rc = run(input);
        if (rc == 2) break;
        if (!rc) { text("Error: "); text(error); newline(); }
    }
    return 0;
}
#else
/* Native GUI entry. No PSP ownership changes, DOS shell, text-mode BIOS calls,
 * keyboard waits or source writes. Each invocation completes synchronously.
 * The desktop must call from foreground, not IRQ/painting context. */
void media_service(void) {
    Entry e;
    int ok = 0;
    u16 i;
    u16 requested = media_request.operation;
    char selected[40];
    u8 selected_directory = 0;
    selected[0] = 0;
    if ((requested == MD_OPEN_SELECTED || requested == MD_IMPORT_SELECTED) &&
        media_request.preview_more < media_request.count) {
        MediaRow *row = &media_request.rows[media_request.preview_more];
        copy(selected, row->name);
        selected_directory = row->directory;
    }
    media_request.status = 1;
    media_request.count = media_request.total = 0;
    media_request.preview_bytes = media_request.preview_more = 0;
    zero(media_request.rows, sizeof(media_request.rows));
    zero(media_request.preview, sizeof(media_request.preview));
    zero(media_request.message, sizeof(media_request.message));
    error = "No readable medium was found.";
    if (media_request.version != MD_ABI) { error = "Incompatible media service version."; goto finished; }
    if (!mounted_device) copy(cwd, "/");
    /* Never allow an unbounded caller string to escape into path parsers. */
    if (media_request.path[MD_PATH-1] || media_request.destination[63]) {
        error = "The selected path is too long."; goto finished;
    }
    if (requested == MD_OPEN_SELECTED || requested == MD_IMPORT_SELECTED) {
        if (!selected[0]) { error = "Select a file or folder first."; goto finished; }
        if (!full_path(selected, media_request.path)) goto finished;
        media_request.operation = requested == MD_IMPORT_SELECTED ? MD_IMPORT_FILE :
            selected_directory ? MD_LIST : MD_PREVIEW_FILE;
        if (requested == MD_OPEN_SELECTED && selected_directory) media_request.first = 0;
    } else if (requested == MD_UP || requested == MD_REFRESH) {
        if (!full_path(requested == MD_UP ? ".." : "", media_request.path)) goto finished;
        media_request.operation = MD_LIST;
        if (requested == MD_UP) media_request.first = 0;
    }
    if (media_request.operation == MD_MOUNT) {
        mounted_device = 0;
        switch (media_request.device) {
        case MD_FLOPPY: ok = bios_device(0) && mount_fat(); break;
        case MD_CD: case MD_CD_IDE: ok = select_cd(media_request.device == MD_CD_IDE); break;
        case MD_USB:
            for (i = 0x80; i < 0x88; ++i) {
                if (!bios_device(i) || !mount_fat()) continue;
                /* A BIOS-exposed disk is not necessarily USB. Skip the
                 * CiukiOS system volume; show the actual BIOS id in UI. */
                if (block[3]=='C' && block[4]=='I' && block[5]=='U' && block[6]=='K') continue;
                ok = 1; break;
            }
            if (!ok) error = "No additional BIOS FAT disk. Connect USB before boot and enable legacy storage in BIOS.";
            break;
        default: error = "Unknown removable device."; break;
        }
        if (!ok) goto finished;
        mounted_device = media_request.device;
        copy(cwd, "/");
        copy(media_request.path, "/");
    } else if (!mounted_device || mounted_device != media_request.device) {
        error = "Open the device before browsing it."; goto finished;
    }
    media_request.bios_drive = atapi_base && optical ? 0xffff : drive;
    if (!resolve(media_request.path, &e)) { ok = 0; goto finished; }
    switch (media_request.operation) {
    case MD_MOUNT: case MD_LIST:
        ok = directory(&e, 0, 0, 1);
        if (ok) copy(cwd, media_request.path);
        break;
    case MD_PREVIEW_FILE: {
        Reader r; int n; u16 used = 0;
        if (e.directory) { error = "Open the folder to view its contents."; ok = 0; break; }
        reader_open(&r, &e); ok = 1;
        while (used < MD_PREVIEW-1 && (n = reader_next(&r)) > 0) {
            for (i = 0; i < (u16)n && used < MD_PREVIEW-1; ++i) {
                u8 c = block[i];
                if ((c < 32 && c != 9 && c != 10 && c != 13) || c > 126) {
                    error = "This is a binary file. Import it to use an appropriate application.";
                    ok = 0; break;
                }
                media_request.preview[used++] = c;
            }
            if (!ok) break;
        }
        if (ok && used < MD_PREVIEW-1 && n < 0) ok = 0;
        if (ok) {
            media_request.preview_bytes = used;
            media_request.preview_more = e.size > used;
        }
        break;
    }
    case MD_IMPORT_FILE:
        if (!media_request.destination[0]) { error = "Choose a destination file on the system disk."; ok = 0; }
        else if (media_request.destination[1] != ':' || media_request.destination[2] != '\\') {
            error = "Use a full destination path, for example C:\\IMPORT.TXT."; ok = 0;
        } else ok = transfer(&e, media_request.destination);
        if (ok) {
            copy(media_request.path, cwd);
            /* Preserve the visible source directory after a successful
             * import. A medium removed here still reports its read error. */
            ok = resolve(cwd, &e) && directory(&e, 0, 0, 1);
        }
        break;
    default: error = "This media operation is unavailable."; ok = 0; break;
    }
finished:
    if (ok) {
        media_request.status = 0;
        error = media_request.operation == MD_IMPORT_FILE ? "File imported. Source media are read-only." :
            media_request.device == MD_USB ? "Read-only BIOS disk. USB must be connected before boot." : "Source media are read-only. Import files to edit them.";
        if (media_request.operation == MD_PREVIEW_FILE)
            error = media_request.preview_more ? "Text preview (first 511 bytes). Refresh to return to files." :
                "Text preview. Refresh to return to files.";
        /* Packed native Files view: six 13-byte display labels, DOS attr,
         * size. Exact original names remain in rows[] for selection/import. */
        if (media_request.operation != MD_PREVIEW_FILE) {
            for (i = 0; i < media_request.count; ++i) {
                u8 *view = (u8 *)media_request.preview + i*18;
                const MediaRow *row = &media_request.rows[i];
                u16 j;
                for (j = 0; j < 12 && row->name[j]; ++j) view[j] = row->name[j];
                if (row->name[j] && j == 12) view[11] = '~';
                view[13] = row->directory ? 16 : 0;
                putdword(view+14, row->size);
            }
        }
    } else {
        media_request.count = media_request.total = media_request.preview_bytes = 0;
        zero(media_request.rows, sizeof(media_request.rows));
        zero(media_request.preview, sizeof(media_request.preview));
    }
    for (i = 0; error[i] && i+1 < MD_ERROR; ++i) media_request.message[i] = error[i];
    media_request.message[i] = 0;
}
#endif
