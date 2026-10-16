/* Synthetic firmware only. Nothing here invokes a physical BIOS or port.
 * BIOS conventions researched against SeaBIOS; independent implementation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include "dosvm_internal.h"

static void low(uint32_t *reg, uint16_t value) { *reg = (*reg & 0xFFFF0000u) | value; }
static void ah(struct v86_frame *f, uint8_t value) { f->tf.eax = (f->tf.eax & ~0xFF00u) | ((uint32_t)value << 8); }
static uint16_t u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p + 2) << 16); }
static void w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(uint8_t *p, uint32_t v) { w16(p, v); w16(p + 2, v >> 16); }
static void carry(struct v86_frame *f, bool set)
{ f->tf.eflags = (f->tf.eflags & ~V86_CF) | (set ? V86_CF : 0); }
static uint32_t segptr(uint32_t segment, uint32_t offset)
{ return segment <= 0xFFFF && offset <= 0xFFFF ? (segment << 4) + offset : UINT32_MAX; }
static uint8_t bcd(unsigned value) { return (value / 10) * 16 + value % 10; }
static void vm_date(uint64_t elapsed_days, unsigned *year, unsigned *month, unsigned *day)
{
    /* Synthetic epoch 2026-10-11, advanced only by this VM's elapsed time.
     * Gregorian repeats after 400 years; bound the conversion work. */
    static const uint8_t lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned y = 2026, m = 10, d = 11;
    unsigned days = elapsed_days % 146097;
    while (days) {
        unsigned n = lengths[m - 1] + (m == 2 && !(y % 4) && (y % 100 || !(y % 400)));
        if (days <= n - d) { d += days; break; }
        days -= n - d + 1; d = 1;
        if (++m == 13) { m = 1; y++; }
    }
    *year = y; *month = m; *day = d;
}
static void bda16(struct dosvm *v, uint32_t address, uint16_t value)
{ dosvm_write(v, address, &value, sizeof(value)); }
void dosvm_putchar(struct dosvm *v, uint8_t c, uint8_t attr)
{
    struct v86_frame f = {0};
    f.tf.eax = 0x0E00 | c; f.tf.ebx = attr;
    dosvm_vga_bios(v, &f);
}

static int disk_result(struct dosvm *v, struct v86_frame *f, uint8_t status)
{
    v->disk_status = status;
    ah(f, status); carry(f, status != 0);
    uint8_t b = status;
    dosvm_write(v, 0x474, &b, 1);
    return 0;
}

static int disk_bios(struct dosvm *v, struct v86_frame *f)
{
    uint8_t fn = f->tf.eax >> 8, count = f->tf.eax;
    uint8_t packet[26];
    uint32_t lba = 0, buffer = 0, dap = 0;
    unsigned amount = 0;
    bool write = fn == 3 || fn == 0x43, verify = fn == 4 || fn == 0x44;
    if ((uint8_t)f->tf.edx != 0x80) return disk_result(v, f, 1);
    switch (fn) {
    case 0: return disk_result(v, f, 0);
    case 1: return disk_result(v, f, v->disk_status);
    case 8: {
        unsigned cylinder = (DOSVM_SECTORS + 1007) / 1008 - 1;
        low(&f->tf.ecx, ((cylinder & 255) << 8) | ((cylinder >> 2) & 0xC0) | 63);
        low(&f->tf.edx, 0x0F01); /* max head 15, one hard disk */
        f->es = 0; low(&f->tf.edi, 0);
        return disk_result(v, f, 0);
    }
    case 0x15:
        low(&f->tf.ecx, DOSVM_SECTORS >> 16); low(&f->tf.edx, (uint16_t)DOSVM_SECTORS);
        carry(f, false); ah(f, 3); return 0;
    case 0x41:
        if ((uint16_t)f->tf.ebx != 0x55AA) return disk_result(v, f, 1);
        low(&f->tf.ebx, 0xAA55); low(&f->tf.ecx, 5);
        disk_result(v, f, 0); ah(f, 0x30); return 0;
    case 0x48: {
        uint16_t offset = f->tf.esi;
        uint32_t address = segptr(f->ds, offset);
        uint8_t length[2];
        if (dosvm_read(v, address, length, 2) || u16(length) < 26 || offset > 0x10000u - 26 ||
            !dosvm_range(v, address, 26, true)) return disk_result(v, f, 1);
        memset(packet, 0, sizeof(packet));
        w16(packet, 26); w16(packet + 2, 2);
        w32(packet + 4, (DOSVM_SECTORS + 1007) / 1008);
        w32(packet + 8, 16); w32(packet + 12, 63);
        w32(packet + 16, DOSVM_SECTORS); w16(packet + 24, 512);
        return disk_result(v, f, dosvm_write(v, address, packet, 26) ? 1 : 0);
    }
    case 2: case 3: case 4: {
        unsigned cylinder = ((f->tf.ecx >> 8) & 255) | ((f->tf.ecx & 0xC0) << 2);
        unsigned sector = f->tf.ecx & 63, head = (f->tf.edx >> 8) & 255;
        if (!sector || sector > 63 || head >= 16 || cylinder >= (DOSVM_SECTORS + 1007) / 1008)
            return disk_result(v, f, 4);
        lba = (cylinder * 16 + head) * 63 + sector - 1;
        amount = count;
        uint16_t off = f->tf.ebx;
        buffer = segptr(f->es, off);
        if (amount * 512u > 0x10000u - off) return disk_result(v, f, 9);
        break;
    }
    case 0x42: case 0x43: case 0x44: {
        uint16_t off = f->tf.esi;
        dap = segptr(f->ds, off);
        if (off > 0xFFF0 || dosvm_read(v, dap, packet, 16) || packet[0] != 16 || packet[1])
            return disk_result(v, f, 1);
        amount = u16(packet + 2);
        if (u32(packet + 12)) return disk_result(v, f, 4);
        lba = u32(packet + 8);
        uint16_t offset = u16(packet + 4);
        buffer = segptr(u16(packet + 6), offset);
        if (amount * 512u > 0x10000u - offset || !dosvm_range(v, dap, 16, true))
            return disk_result(v, f, 9);
        break;
    }
    default: return disk_result(v, f, 1);
    }
    /* Complete preflight before any sector is read/written or any DAP changed. */
    if (!amount || amount > 127 || lba >= DOSVM_SECTORS || amount > DOSVM_SECTORS - lba)
        return disk_result(v, f, 4);
    if (!verify && !dosvm_range(v, buffer, amount * 512u, !write))
        return disk_result(v, f, 9);
    unsigned completed = 0;
    int error = 0;
    for (; completed < amount; completed++) {
        if (write) {
            error = dosvm_read(v, buffer + completed * 512, v->sector, 512);
            if (!error) error = dosvm_disk_write(v, lba + completed, v->sector);
        } else {
            error = dosvm_disk_read(v, lba + completed, v->sector);
            if (!error && !verify) error = dosvm_write(v, buffer + completed * 512, v->sector, 512);
        }
        if (error) break;
    }
    if (dap) {
        uint16_t done = completed;
        dosvm_write(v, dap + 2, &done, 2);
    } else f->tf.eax = (f->tf.eax & ~255u) | completed;
    return disk_result(v, f, error ? 0x20 : 0);
}

static int memory_bios(struct dosvm *v, struct v86_frame *f)
{
    unsigned ax=(uint16_t)f->tf.eax, fn=ax>>8;
    carry(f,false);
    if (ax>=0x2400 && ax<=0x2403) {
        if (ax==0x2400 || ax==0x2401) {
            if (dosvm_set_a20(v,ax==0x2401)) { ah(f,0x86); carry(f,true); }
            else ah(f,0);
        } else if (ax==0x2402) low(&f->tf.eax,v->a20);
        else { low(&f->tf.ebx,3); ah(f,0); }
        return 0;
    }
    if (fn==0x88 || ax==0xE801) {
        unsigned kb=64+v->memory.quota/1024;
        if (fn==0x88) low(&f->tf.eax,kb);
        else {
            unsigned below=kb>15360 ? 15360 : kb, above=kb>15360 ? (kb-15360)/64 : 0;
            low(&f->tf.eax,below); low(&f->tf.ecx,below); low(&f->tf.ebx,above); low(&f->tf.edx,above);
        }
        return 0;
    }
    if (ax==0xE820) {
        /* Describe only private guest RAM; no host E820 or allocator frames. */
        static const uint32_t starts[]={0,0xA0000,0x100000,0x10FFF0,VM_POOL_BASE};
        uint32_t lengths[]={0xA0000,0x60000,0xFFF0,0x10,v->memory.quota};
        unsigned index=f->tf.ebx, count=v->memory.quota ? 5 : 4;
        if (f->tf.edx!=0x534D4150 || f->tf.ecx<20 || index>=count ||
            !vm_buffer(v,f->es,f->tf.edi,f->tf.ecx>=24 ? 24 : 20,true)) goto unsupported;
        uint8_t map[24]={0};
        vm_w32(map,starts[index]); vm_w32(map+8,lengths[index]);
        vm_w32(map+16,index==1 || index==3 ? 2 : 1); vm_w32(map+20,1);
        unsigned size=f->tf.ecx>=24 ? 24 : 20;
        if (dosvm_write(v,vm_ptr(f->es,f->tf.edi),map,size)) goto unsupported;
        f->tf.eax=0x534D4150; f->tf.ecx=size; f->tf.ebx=++index<count ? index : 0;
        return 0;
    }
    if (fn==0x87) {
        uint8_t table[32]; uint32_t base[2];
        unsigned bytes=(uint16_t)f->tf.ecx*2u;
        if (!vm_buffer(v,f->es,f->tf.esi,sizeof(table),false) ||
            dosvm_read(v,vm_ptr(f->es,f->tf.esi),table,sizeof(table))) goto unsupported;
        for (unsigned i=0;i<2;i++) {
            const uint8_t *d=table+16+i*8;
            uint32_t limit=vm_u16(d)|((uint32_t)(d[6]&15)<<16);
            if (d[6]&0x80) limit=(limit<<12)|0xFFF;
            unsigned access=d[5];
            if ((access&0x90)!=0x90 || (access&8) || (access&4) || (i && !(access&2)) ||
                (bytes && bytes-1>limit)) goto unsupported;
            base[i]=vm_u16(d+2)|((uint32_t)d[4]<<16)|((uint32_t)d[7]<<24);
        }
        bool enabled=v->a20;
        if (dosvm_set_a20(v,true)) goto unsupported;
        int e=dosvm_copy_region(v,base[0],base[1],bytes,false);
        dosvm_set_a20(v,enabled);
        if (e) goto unsupported;
        ah(f,0); return 0;
    }
unsupported:
    ah(f,0x86); carry(f,true); return 0;
}

int dosvm_bios(struct dosvm *v, struct v86_frame *f, uint8_t vector)
{
    uint16_t ax = f->tf.eax;
    uint8_t fn = ax >> 8;
    switch (vector) {
    case 8: {
        if (++v->bios_ticks >= 0x1800B0) { v->bios_ticks = 0; v->midnight++; }
        dosvm_write(v, 0x46C, &v->bios_ticks, 4); dosvm_write(v, 0x470, &v->midnight, 1);
        return 0;
    }
    case 9: return dosvm_kbd_bios(v);
    case 0x33: return dosvm_mouse_bios(v, f);
    case 0x1C: return 0;
    case 0x10: return dosvm_vga_bios(v, f);
    case 0x11: low(&f->tf.eax, 0x22); return 0;
    case 0x12: low(&f->tf.eax, 640); return 0;
    case 0x13: return disk_bios(v, f);
    case 0x15:
        carry(f, false);
        if (fn == 0x86) {
            uint32_t us = ((uint32_t)(uint16_t)f->tf.ecx << 16) | (uint16_t)f->tf.edx;
            if (us > 60000000) { ah(f, 0x86); carry(f, true); return 0; }
            return dosvm_wait(v, false, deadline_after_ms((us + 999) / 1000));
        }
        return memory_bios(v,f);
    case 0x16:
        dosvm_input_poll(v);
        if (dosvm_keyboard_refresh(v)) return -EFAULT;
        if (fn == 0 || fn == 0x10 || fn == 1 || fn == 0x11) {
            bool peek = fn == 1 || fn == 0x11;
            if (!peek && !v->key_count) {
                int e = dosvm_kbd_irq_ready(v) ? 0 : dosvm_wait(v, true, 0);
                if (e) return e;
                /* A PC BIOS INT 16h AH=00h/10h enables interrupts and loops
                 * until IRQ1's INT 9 has stored a key (f3-acceptance.md,
                 * vm-window first QEMU run). The monitor
                 * cannot run that INT 9 while it blocks here, so with a key
                 * byte in the virtual 8042 (window input, f3-02) the call
                 * re-executes its own INT FEh at F000:0160 with the guest's
                 * IF set: the IRQ is delivered first, the guest INT 9 (ROM or
                 * DOS/TSR hook) fills the buffer, and the retried read takes
                 * the key. Timer/device/presentation wakes also retry with
                 * IF set so a guest timer handler runs even without input.
                 * Kernel typing fills the BIOS buffer directly. */
                if (!v->key_count) {
                    v->monitor.vif = true;
                    f->tf.eip -= 2;
                    return 0;
                }
            }
            f->tf.eflags = (f->tf.eflags & ~V86_ZF) | (!v->key_count ? V86_ZF : 0);
            if (v->key_count) {
                low(&f->tf.eax, v->keyboard[v->key_head]);
                if (!peek && v->launch_armed && !v->launch_exec && (uint8_t)f->tf.eax == '\r')
                    v->launch_enter_consumed = true;
                if (!peek) {
                    v->key_head = (v->key_head + 1) % 16; v->key_count--;
                    uint16_t head = 0x1E + v->key_head * 2;
                    if (dosvm_write(v, 0x41A, &head, 2)) return -EFAULT;
                }
            }
            return 0;
        }
        if (fn == 2 || fn == 0x12) { low(&f->tf.eax, v->kbd.shift); return 0; }
        if (fn == 5) { low(&f->tf.eax, dosvm_keyboard(v->info.key, f->tf.ecx) ? 0 : 1); return 0; }
        carry(f, true); return 0;
    case 0x19: dosvm_stop(v->info.key, DOSVM_RESET); return 0;
    case 0x1A:
        if (fn == 0) {
            low(&f->tf.ecx, v->bios_ticks >> 16); low(&f->tf.edx, v->bios_ticks);
            f->tf.eax = (f->tf.eax & ~255u) | v->midnight; v->midnight = 0; carry(f, false); return 0;
        }
        if (fn == 1) {
            v->bios_ticks = (((uint32_t)(uint16_t)f->tf.ecx << 16) | (uint16_t)f->tf.edx) % 0x1800B0;
            dosvm_write(v, 0x46C, &v->bios_ticks, 4); carry(f, false); return 0;
        }
        if (fn == 2) {
            uint32_t seconds = (v->clock_ms / 1000) % 86400;
            low(&f->tf.ecx, ((uint16_t)bcd(seconds / 3600) << 8) | bcd(seconds / 60 % 60));
            low(&f->tf.edx, (uint16_t)bcd(seconds % 60) << 8); carry(f, false); return 0;
        }
        if (fn == 4) {
            unsigned year, month, day;
            vm_date(v->clock_ms / 86400000, &year, &month, &day);
            low(&f->tf.ecx, ((uint16_t)bcd(year / 100) << 8) | bcd(year % 100));
            low(&f->tf.edx, ((uint16_t)bcd(month) << 8) | bcd(day)); carry(f, false); return 0;
        }
        ah(f, 0x86); carry(f, true); return 0;
    case 0x28: return 0; /* DOS idle callout: the default handler preserves all registers */
    case 0x2F:
        if (ax == 0x1680) f->tf.eax &= ~255u; /* supported; slice released at INT reflection */
        else if (ax == 0x1687) low(&f->tf.eax, 0xFFFF); /* DPMI not implemented/advertised */
        else if (ax == 0x1686) low(&f->tf.eax, 1);
        else if (ax == 0x4300) f->tf.eax = (f->tf.eax & ~255u) | 0x80;
        else if (ax == 0x4310) { f->es=VM_XMS_SEG; low(&f->tf.ebx,0); }
        return 0;
    case 0x67: return dosvm_ems(v,f);
    default:
        if (vector < 8) return -EFAULT;
        carry(f, true); ah(f, 0x86); return 0;
    }
}

int dosvm_bios_init(struct dosvm *v)
{
    uint8_t *rom = P2V(v->frames[0xF0]);
    for (unsigned i = 0; i < 256; i++) {
        uint16_t entry[2] = {i * 16, 0xF000};
        if (dosvm_write(v, i * 4, entry, sizeof(entry))) return -EFAULT;
        /* All 256 stubs fit the first ROM page, kept supervisor-write-protected. */
        rom[i * 16] = 0xCD; rom[i * 16 + 1] = 0xFE; rom[i * 16 + 2] = 0xCF;
    }
    static const uint8_t timer[] = {0xCD,0xFE,0x50,0xB0,0x20,0xE6,0x20,0x58,0xCD,0x1C,0xCF};
    memcpy(rom + 8 * 16, timer, sizeof(timer));
    bda16(v, 0x410, 0x22); bda16(v, 0x413, 640);
    bda16(v, 0x41A, 0x1E); bda16(v, 0x41C, 0x1E);
    bda16(v, 0x480, 0x1E); bda16(v, 0x482, 0x3E);
    uint8_t mode = 3, rows = 24, disks = 1;
    dosvm_write(v, 0x449, &mode, 1); bda16(v, 0x44A, 80); bda16(v, 0x44C, 4000);
    dosvm_write(v, 0x484, &rows, 1); dosvm_write(v, 0x475, &disks, 1);
    bda16(v, 0x485, 16);
    for (unsigned i = 0; i < 2000; i++) v->text[i] = 0x0720;
    /* Text output belongs to the VGA model (dosvm_vga_init); no text buffer flush. */
    static const uint8_t keyboard_irq[] = {0xCD,0xFE,0x50,0xB0,0x20,0xE6,0x20,0x58,0xCF};
    memcpy(rom + 9 * 16, keyboard_irq, sizeof(keyboard_irq));
    uint8_t *callback_rom = (uint8_t *)P2V(v->frames[0xF1]) + (DOSVM_MOUSE_RETURN & 0xFFF);
    callback_rom[0] = 0xCD; callback_rom[1] = 0xFD; callback_rom[2] = 0xF4;
    return 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool ready_record(struct dosvm *v)
{
    /* Fixed positional guest report. Values come from DOS calls, not guesses. */
    if (v->line_length != 26 || memcmp(v->line, "DOSREADY ", 9) ||
        v->line[11] != '.' || memcmp(v->line + 14, " free=", 6) ||
        v->line[24] != ' ' || v->line[25] < 'A' || v->line[25] > 'Z') return false;
    unsigned major = 0, minor = 0, paragraphs = 0;
    for (unsigned i = 9; i < 24; i++) {
        if (i == 11 || i == 14 || (i >= 15 && i < 20)) continue;
        int d = hex_digit(v->line[i]);
        if (d < 0) return false;
        if (i < 11) major = major * 16 + d;
        else if (i < 14) minor = minor * 16 + d;
        else paragraphs = paragraphs * 16 + d;
    }
    if (!major || !paragraphs || paragraphs > 0xA000) return false;
    v->info.dos_major = major; v->info.dos_minor = minor;
    v->info.free_paragraphs = paragraphs; v->info.drive = v->line[25] - 'A';
    v->info.dos_ready = true;
    v->info.state = DOSVM_RUNNABLE;
    return true;
}
void dosvm_expect(struct dosvm *v, const char *name)
{
    unsigned n = 0;
    while (name && name[n] && n + 1 < sizeof(v->verdict_name)) { v->verdict_name[n] = name[n]; n++; }
    v->verdict_name[n] = 0;
    v->info.verdict_done = v->info.verdict_pass = false;
    memset(v->info.verdict_detail, 0, sizeof(v->info.verdict_detail));
    v->verdict_text_valid = false;
}
/* The verdict line is the payload's own record-port write: the screen is
 * exactly what the payload left (RUNREC: the child's output and its line),
 * whatever the controller's polling cadence. Kernel reads peek (no latch). */
static void verdict_screen(struct dosvm *v)
{
    v->verdict_text_valid = !dosvm_read(v, 0xB8000, v->verdict_text, sizeof(v->verdict_text));
}
const uint16_t *dosvm_verdict_text(const struct dosvm *v)
{
    return v->verdict_text_valid ? v->verdict_text : 0;
}
static void verdict_line(struct dosvm *v)
{
    unsigned n = 0;
    while (v->verdict_name[n]) n++;
    if (!n || v->info.verdict_done || v->line_length < n + 5 || memcmp(v->line, v->verdict_name, n) || v->line[n] != ' ')
        return;
    const char *rest = v->line + n + 1;
    unsigned left = v->line_length - n - 1;
    if (left == 4 && !memcmp(rest, "PASS", 4)) {
        v->info.verdict_done = v->info.verdict_pass = true;
        verdict_screen(v);
        return;
    }
    if (left < 4 || memcmp(rest, "FAIL", 4)) return;
    verdict_screen(v);
    v->info.verdict_done = true;
    v->info.verdict_pass = false;
    unsigned k = 0;
    for (unsigned i = 5; i < left && k + 1 < sizeof(v->info.verdict_detail); i++) {
        char c = rest[i];
        v->info.verdict_detail[k++] = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ? c : '_';
    }
}
int dosvm_record_byte(struct dosvm *v, uint8_t byte)
{
    if (v->info.state >= DOSVM_STOPPING) return -V86_EIO;
    if (byte == 13) return 0;
    if (byte != 10) {
        if (byte < 32 || byte > 126 || v->line_length == 80) v->line_bad = true;
        else if (!v->line_bad) v->line[v->line_length++] = byte;
        return 0;
    }
    if (g_ticks - v->record_window >= 1000) { v->record_window = g_ticks; v->record_lines = 0; }
    if (v->line_bad || !v->line_length || v->record_lines >= 16) v->info.dropped_records++;
    else {
        static const char hex[] = "0123456789abcdef";
        ready_record(v);
        /* A fixture result is data, not an injected CIUKI_TEST envelope.
         * The controller also checks kernel-side dispatch coverage/cleanup. */
        if (v->line_length == 13 && !memcmp(v->line,"VMMEMORY PASS",13)) {
            v->info.memory_done=true; v->info.memory_pass=true;
        } else if (v->line_length >= 13 && !memcmp(v->line,"VMMEMORY FAIL",13)) {
            v->info.memory_done=true; v->info.memory_pass=false;
        }
        /* f3-02 VMVIDEO.COM stage latch: "VMVIDEO s f", s 1-6, f hex flags. */
        if (v->line_length == 11 && !memcmp(v->line, "VMVIDEO ", 8) && v->line[9] == ' ' &&
            v->line[8] >= '1' && v->line[8] <= '6' && hex_digit(v->line[10]) >= 0) {
            v->info.video_flags = (uint32_t)hex_digit(v->line[10]);
            v->info.video_stage = (uint32_t)(v->line[8] - '0');
        }
        verdict_line(v);
        unsigned parts = (v->line_length + 47) / 48;
        for (unsigned part = 0; part < parts; part++) {
            char encoded[97];
            unsigned n = v->line_length - part * 48;
            if (n > 48) n = 48;
            for (unsigned i = 0; i < n; i++) {
                uint8_t b = v->line[part * 48 + i];
                encoded[i * 2] = hex[b >> 4]; encoded[i * 2 + 1] = hex[b & 15];
            }
            encoded[n * 2] = 0;
            if (rec_active()) rec_emit(dosvm_probe(v), "DATA", "group=guest vm=%u generation=%u part=%u parts=%u data=%s",
                     v->info.key.id, v->info.key.generation, part + 1, parts, encoded);
            else rec_diag(dosvm_probe(v), "group=guest vm=%u generation=%u part=%u parts=%u data=%s",
                     v->info.key.id, v->info.key.generation, part + 1, parts, encoded);
        }
        v->info.guest_records++; v->record_lines++;
    }
    v->line_length = 0; v->line_bad = false;
    return 0;
}

int dosvm_io(void *arg, uint16_t port, unsigned width, bool write, uint32_t *value)
{
    struct dosvm *v = arg;
    if (v->info.state >= DOSVM_STOPPING) return -V86_EIO;
    /* Attribute a batched exit to its first port. No extra clock per OUT. */
    if (v->perf.trap_active && !v->perf.io_valid) {
        v->perf.io_port = port; v->perf.io_valid = true;
    }
    if (port == 0x20 || port == 0x21 || port == 0xA0 || port == 0xA1) {
        v->perf.cause = DOSVM_COST_PIT_PIC;
        if (width != 1) return -V86_EPERM;
        uint32_t flags = irq_save();
        uint8_t b = dosvm_pic_register(&v->monitor.pic[port >= 0xA0], port & 1, write, (uint8_t)*value);
        dosvm_pic_cascade(&v->monitor);
        v->monitor.stats.io[V86_PIC]++;
        irq_restore(flags);
        if (!write) *value = b;
        return 0;
    }
    /* The default PIT registration is stable across reads/latches. Keep
     * registration/unregistration authoritative (including custom handlers)
     * while avoiding the generic registry search for its cached owner. */
    unsigned pit = v->port_hint[0];
    if (port >= 0x40 && port <= 0x43 && pit < DOSVM_PORTS &&
        v->ports[pit].handler == dosvm_pit_port &&
        port >= v->ports[pit].first && port <= v->ports[pit].last) {
        v->perf.cause = DOSVM_COST_PIT_PIC;
        uint32_t flags = irq_save();
        int result = dosvm_pit_port(v, port, width, write, value, v->ports[pit].arg);
        irq_restore(flags);
        if (!result && write && (port != 0x43 || (((uint8_t)*value >> 6) < 3 && (*value & 0x30))))
            dosvm_trace(v, DOSVM_TRACE_PIT, &v->frame, port, (uint8_t)*value, 0);
        return result;
    }
    unsigned kind = (port >= 0x40 && port <= 0x43) || port == 0x61 ? 0 :
        port == 0x60 || port == 0x64 ? 1 : port >= 0x3B0 && port <= 0x3DF ? 2 : 3;
    v->perf.cause = kind == 0 ? DOSVM_COST_PIT_PIC : kind == 1 ? DOSVM_COST_KBD :
        kind == 2 ? DOSVM_COST_VGA_IO : DOSVM_COST_IO;
    unsigned hint = v->port_hint[kind];
    if (!v->ports[hint].handler || port < v->ports[hint].first || port > v->ports[hint].last) hint = DOSVM_PORTS;
    for (unsigned n = 0; n < DOSVM_PORTS; n++) {
        unsigned i = hint < DOSVM_PORTS ? hint : n;
        if (v->ports[i].handler && port >= v->ports[i].first && port <= v->ports[i].last)
        {
            v->port_hint[kind] = (uint8_t)i;
            /* Device state shared with the dosvm_tick timer hook. */
            bool shared = v->ports[i].handler == dosvm_pit_port || v->ports[i].handler == dosvm_kbd_port ||
                          v->ports[i].handler == dosvm_audio_port;
            uint32_t flags = shared ? irq_save() : 0;
            int result = v->ports[i].handler(v, port, width, write, value, v->ports[i].arg);
            if (shared) irq_restore(flags);
            if (!result && write && ((port >= 0x40 && port <= 0x42) ||
                (port == 0x43 && ((uint8_t)*value >> 6) < 3 && (*value & 0x30))))
                dosvm_trace(v, DOSVM_TRACE_PIT, &v->frame, port, (uint8_t)*value, 0);
            return result;
        }
    }
    if (port == DOSVM_FAULT_PORT) return -V86_EPERM;
    if (port == DOSVM_RECORD_PORT || port == DOSVM_CONTROL_PORT) {
        if (width != 1 || !write) return -V86_EPERM;
        /* OUT passes the whole EAX (v86_emulate); only the written byte counts. */
        if (port == DOSVM_RECORD_PORT) return dosvm_record_byte(v, (uint8_t)*value);
        if ((uint8_t)*value != 0) return -V86_EPERM;
        return dosvm_stop(v->info.key, DOSVM_EXIT);
    }
    if (port == 0x92) {
        if (width != 1) return -V86_EPERM;
        if (!write) { *value = v->a20 ? 2 : 0; return 0; }
        if (*value & 1) return dosvm_stop(v->info.key, DOSVM_RESET);
        return dosvm_set_a20(v, *value & 2);
    }
    /* Absent devices: all-one reads, discarded writes, at the CPU width.
     * This includes UART, ATA, DMA, PCI, VGA and CMOS, never real access. */
    if (!write) *value = width == 4 ? UINT32_MAX : width == 2 ? 0xFFFF : 0xFF;
    return 0;
}
