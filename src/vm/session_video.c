#include "session_video.h"

/* Original CiukiOS implementation. See session_video.h for the calling
 * environment. Only the pinned JLOAD page-table self-map is dereferenced
 * besides validated conventional/ROM pages, the shared block and the owned
 * framebuffer mapping. */

#define PAGE_MAP ((const volatile uint32_t *)(uintptr_t)0xFF800000UL)
#define LINEAR(address) ((volatile uint8_t *)(uintptr_t)(address))
#define GUEST_BUDGET 4096UL      /* string elements per trapped fault */
#define HOST_BUDGET 16384UL
#define STATUS_DISPLAY_OFF 1u
#define STATUS_RETRACE 8u

static cvvid_shared *shared;
static cvga_state *vga;
static cvbios bios;
static cvp_state presenter, band_presenter;
static cvvid_config config;
static uint32_t attached, share_count, host_mode, host_offset, host_granule;
static const cvvid_fb *host_fb;
static cvvid_fb host_fb_copy;
static uint8_t saved_bda_a[30], saved_bda_b[7], saved_vectors[8];
static uint8_t row_buffer[CVP_MAX_WIDTH];
static cvvid_present_packet armed;
static struct {
    uint32_t valid, display_changes, phases;
    cvga_geometry geometry;
} damage;
static uint8_t damage_taken[4][CVGA_DIRTY_BYTES];
static uint32_t armed_linear, armed_interval, armed_last_lo, armed_last_hi, armed_valid;

static struct {
    uint32_t faults, instructions, elements, unsupported, bus_faults;
    uint32_t port_reads, port_writes, port_rejects, status_polls;
    uint32_t presents, present_rows, present_pixels, frames, full_redraws, passes;
    uint32_t last_present_tsc, max_present_tsc, total_present_lo, total_present_hi;
    uint32_t host_faults, host_bytes, host_enters, fault_tsc_lo, fault_tsc_hi;
    uint32_t string_io, string_elements, host_display_sets, damage_queries;
    uint32_t fatal_csip, last_fault_csip;
    uint8_t fatal_bytes[16];
} st;

static struct {
    uint32_t changes, valid, line_cycles, hde_cycles, total_lines, vde, vrs, vr_lines;
    uint32_t poll_position;
} timing;

/* ---- Guest memory ---- */

static int page_usable(uint32_t linear, int write)
{
    uint32_t pte;
    if (linear >= 0x110000UL) return 0;
    pte = PAGE_MAP[linear >> 12];
    if ((pte & 5) != 5) return 0;
    return !write || (pte & 2);
}

static int aperture(uint32_t linear)
{
    return linear >= 0xa0000UL && linear < 0xc0000UL;
}

/* Host-mode stores go to an uncached framebuffer mapping, where every store
 * is a bus transaction. Consecutive bytes (the compositor copies scanlines
 * with REP MOVSD) are combined and written as aligned dwords. The buffer is
 * flushed before any host read and at the end of every emulated instruction,
 * so the host's bus order is preserved. */
#define HOST_COMBINE 64u
static uint8_t host_pending[HOST_COMBINE];
static uint32_t host_pending_offset, host_pending_bytes;

static void host_flush(void)
{
    uint8_t *dst;
    const uint8_t *src = host_pending;
    uint32_t n = host_pending_bytes;
    if (!n || !host_fb) {
        host_pending_bytes = 0;
        return;
    }
    dst = host_fb->linear + host_pending_offset;
    while (n && ((uintptr_t)dst & 3)) {
        *(volatile uint8_t *)dst++ = *src++;
        --n;
    }
    while (n >= 4) {
        *(volatile uint32_t *)dst = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                                    ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
        dst += 4;
        src += 4;
        n -= 4;
    }
    while (n--) *(volatile uint8_t *)dst++ = *src++;
    host_pending_bytes = 0;
}

static uint8_t host_read(uint32_t linear)
{
    uint32_t offset;
    host_flush();
    if (linear >= 0xb0000UL || !host_fb) return 0xff;
    offset = host_offset + (linear - 0xa0000UL);
    if (offset >= host_fb->bytes) return 0xff;
    ++st.host_bytes;
    return host_fb->linear[offset];
}

static void host_write(uint32_t linear, uint8_t value)
{
    uint32_t offset;
    if (linear >= 0xb0000UL || !host_fb) return;
    offset = host_offset + (linear - 0xa0000UL);
    if (offset >= host_fb->bytes) return;
    ++st.host_bytes;
    if (host_pending_bytes && (offset != host_pending_offset + host_pending_bytes ||
                               host_pending_bytes == HOST_COMBINE))
        host_flush();
    if (!host_pending_bytes) host_pending_offset = offset;
    host_pending[host_pending_bytes++] = value;
}

static int CVGA_CALL guest_read(void *context, uint32_t linear, uint8_t *value)
{
    (void)context;
    if (aperture(linear)) {
        *value = host_mode ? host_read(linear) : cvga_read_vram(vga, linear);
        return 0;
    }
    if (!page_usable(linear, 0)) return 1;
    *value = *LINEAR(linear);
    return 0;
}

static int CVGA_CALL guest_write(void *context, uint32_t linear, uint8_t value)
{
    (void)context;
    if (aperture(linear)) {
        if (host_mode) host_write(linear, value);
        else cvga_write_vram(vga, linear, value);
        return 0;
    }
    if (!page_usable(linear, 1)) return 1;
    *LINEAR(linear) = value;
    return 0;
}

static int CVGA_CALL guest_fetch(void *context, uint32_t linear, uint8_t *value)
{
    (void)context;
    if (aperture(linear) || !page_usable(linear, 0)) return 1;
    *value = *LINEAR(linear);
    return 0;
}

/* Firmware-level accesses (BDA, IVT, ES:DX tables). The video aperture is
 * never a valid BIOS buffer here. */
static int CVGA_CALL bios_read(void *context, uint32_t linear, uint8_t *value)
{
    (void)context;
    if (aperture(linear) || !page_usable(linear, 0)) return 1;
    *value = *LINEAR(linear);
    return 0;
}

static int CVGA_CALL bios_write(void *context, uint32_t linear, uint8_t value)
{
    (void)context;
    if (aperture(linear) || !page_usable(linear, 1)) return 1;
    *LINEAR(linear) = value;
    return 0;
}

static const cvx_bus guest_bus = {guest_read, guest_write, guest_fetch, 0};
static const cvx_bus firmware_bus = {bios_read, bios_write, bios_read, 0};

/* A conventional-memory extent the caller owns: 10000h-9FFFFh, present,
 * user and (for output) writable pages, no wrap. */
static int conventional(uint32_t linear, uint32_t bytes, int write)
{
    uint32_t page, last;
    if (!bytes || linear < 0x10000UL || linear + bytes < linear || linear + bytes > 0xa0000UL)
        return 0;
    last = (linear + bytes - 1) >> 12;
    for (page = linear >> 12; page <= last; ++page)
        if (!page_usable(page << 12, write)) return 0;
    return 1;
}

static uint32_t client_buffer(const cvvid_client *c)
{
    return ((c->es & 0xffff) << 4) + (c->edi & 0xffff);
}

/* 16-bit ES:DI buffers must not wrap their segment offset. */
static int client_extent(const cvvid_client *c, uint32_t bytes, int write)
{
    if ((c->edi & 0xffff) + bytes > 0x10000UL) return 0;
    return conventional(client_buffer(c), bytes, write);
}

static void copy_out(uint32_t linear, const void *source, uint32_t bytes)
{
    const uint8_t *s = (const uint8_t *)source;
    volatile uint8_t *d = LINEAR(linear);
    while (bytes--) *d++ = *s++;
}

static void copy_in(void *dest, uint32_t linear, uint32_t bytes)
{
    uint8_t *d = (uint8_t *)dest;
    const volatile uint8_t *s = LINEAR(linear);
    while (bytes--) *d++ = *s++;
}

static void zero(void *p, uint32_t bytes)
{
    uint8_t *b = (uint8_t *)p;
    while (bytes--) *b++ = 0;
}

/* ---- Time ---- */

static uint32_t mod64(uint32_t high, uint32_t low, uint32_t divisor)
{
    uint32_t top;
    int i;
    high %= divisor;
    for (i = 0; i < 32; ++i) {
        top = high >> 31;
        high = (high << 1) | (low >> 31);
        low <<= 1;
        if (top || high >= divisor) high -= divisor;
    }
    return high;
}

static void add64(uint32_t *low, uint32_t *high, uint32_t value)
{
    uint32_t old = *low;
    *low += value;
    if (*low < old) ++*high;
}

static uint32_t elapsed(uint32_t start_low, uint32_t start_high)
{
    uint32_t low, high;
    cvvid_rdtsc(&low, &high);
    if (high != start_high && (high - start_high) > 1) return 0xffffffffUL;
    return low - start_low;
}

/* CRTC-derived beam timing: dot clock from MISC bits 2-3 (25.175/28.322
 * MHz), SR01 clock/2 and 8/9-dot characters, CR00/CR01 horizontal totals and
 * CR06/CR10-CR12 vertical totals with their CR07 overflow bits. */
static void timing_update(void)
{
    const uint8_t *cr = vga->crtc;
    uint32_t htotal = cr[0] + 5u, hde = cr[1] + 1u, dots = (vga->seq[1] & 1) ? 8 : 9;
    uint32_t clock_khz = ((vga->misc >> 2) & 3) == 1 ? 28322 : 25175, line_ns, per_us;
    uint32_t vtotal = cr[6] | ((cr[7] & 1u) << 8) | ((cr[7] & 0x20u) << 4);
    uint32_t vrs = cr[16] | ((cr[7] & 4u) << 6) | ((cr[7] & 0x80u) << 2);
    uint32_t vde = cr[18] | ((cr[7] & 2u) << 7) | ((cr[7] & 0x40u) << 3);
    if (vga->seq[1] & 8) clock_khz /= 2;
    vtotal += 2;
    ++vde;
    line_ns = htotal * dots * 1000000UL / clock_khz;
    if (vtotal < 100 || vtotal > 2048 || line_ns < 10000 || line_ns > 100000 || hde >= htotal) {
        vtotal = 449; vrs = 412; vde = 400; line_ns = 31778; hde = 80; htotal = 100;
    }
    per_us = shared->tsc_khz / 1000;
    timing.line_cycles = per_us * line_ns / 1000;
    if (!timing.line_cycles) timing.line_cycles = 1;
    timing.hde_cycles = timing.line_cycles * hde / htotal;
    timing.total_lines = vtotal;
    timing.vde = vde;
    timing.vrs = vrs;
    timing.vr_lines = ((cr[17] & 15u) - (vrs & 15u)) & 15u;
    if (!timing.vr_lines) timing.vr_lines = 16;
    timing.changes = vga->display_changes;
    timing.valid = 1;
}

/* Status register 1 from host time: bit 0 while the beam is outside the
 * displayed area, bit 3 during vertical sync. Without a TSC rate, every poll
 * advances a virtual beam by a quarter line so polling loops still progress
 * through real display/retrace phases; nothing else advances it. */
static uint8_t status1(void)
{
    uint32_t low, high, position, line, in_line, frame;
    uint8_t status = 0;
    ++st.status_polls;
    if (!timing.valid || timing.changes != vga->display_changes) timing_update();
    if (shared->tsc_khz) {
        cvvid_rdtsc(&low, &high);
        if (low < shared->tsc_base_lo) --high;
        low -= shared->tsc_base_lo;
        high -= shared->tsc_base_hi;
        frame = timing.line_cycles * timing.total_lines;
        position = mod64(high, low, frame);
        line = position / timing.line_cycles;
        in_line = position % timing.line_cycles;
        if (line >= timing.vde || in_line >= timing.hde_cycles) status |= STATUS_DISPLAY_OFF;
    } else {
        timing.poll_position = (timing.poll_position + 1) % (timing.total_lines * 4);
        line = timing.poll_position / 4;
        if (line >= timing.vde || (timing.poll_position & 3) == 3) status |= STATUS_DISPLAY_OFF;
    }
    if (line >= timing.vrs && line < timing.vrs + timing.vr_lines) status |= STATUS_RETRACE;
    return status;
}

/* ---- Lifecycle ---- */

static void snapshot(int restore)
{
    uint32_t i;
    for (i = 0; i < sizeof(saved_bda_a); ++i) {
        if (restore) bios_write(0, 0x449 + i, saved_bda_a[i]);
        else bios_read(0, 0x449 + i, &saved_bda_a[i]);
    }
    for (i = 0; i < sizeof(saved_bda_b); ++i) {
        if (restore) bios_write(0, 0x484 + i, saved_bda_b[i]);
        else bios_read(0, 0x484 + i, &saved_bda_b[i]);
    }
    for (i = 0; i < 4; ++i) {
        if (restore) {
            bios_write(0, 0x1f * 4 + i, saved_vectors[i]);
            bios_write(0, 0x43 * 4 + i, saved_vectors[4 + i]);
        } else {
            bios_read(0, 0x1f * 4 + i, &saved_vectors[i]);
            bios_read(0, 0x43 * 4 + i, &saved_vectors[4 + i]);
        }
    }
}

int cvvid_attach(cvvid_shared *block, uint32_t generation)
{
    cvbios_regs r;
    uint32_t i;
    if (attached) return CVVID_E_OPERATION;
    zero(block, CVVID_BLOCK_BYTES);
    shared = block;
    vga = (cvga_state *)((uint8_t *)block + CVVID_MODEL_OFFSET);
    shared->magic = CVVID_SHARED_MAGIC;
    shared->version = 0x0100;
    shared->bytes = (uint32_t)CVVID_BLOCK_BYTES;
    shared->generation = generation;
    shared->tsc_khz = config.magic ? config.tsc_khz : 0;
    cvvid_rdtsc(&shared->tsc_base_lo, &shared->tsc_base_hi);
    zero(&st, sizeof(st));
    zero(&timing, sizeof(timing));
    cvp_init(&presenter);
    cvp_init(&band_presenter);
    host_mode = 0;
    host_fb = 0;
    host_offset = host_granule = 0;
    share_count = 0;
    snapshot(0);
    cvga_init(vga);
    zero(&bios, sizeof(bios));
    bios.vga = vga;
    bios.bus = &firmware_bus;
    if (config.magic)
        for (i = 0; i < CVBIOS_FONT_COUNT; ++i) bios.font[i] = config.font[i];
    /* The guest starts in a freshly set text mode 03h, like a new DOS box. */
    zero(&r, sizeof(r));
    r.eax = 0x0003;
    cvbios_int10(&bios, &r);
    attached = 1;
    return CVVID_OK;
}

int cvvid_attached(void)
{
    return (int)attached;
}

int cvvid_share_count(void)
{
    return (int)share_count;
}

void cvvid_detach(void)
{
    if (!attached) return;
    armed_valid = 0;
    snapshot(1);
    host_mode = 0;
    host_fb = 0;
    attached = 0;
}

/* ---- Trapped memory cycles ---- */

int cvvid_v86_fault(cvvid_client *c, uint32_t cr2, const cvvid_fb *fb)
{
    cvx_cpu cpu;
    cvx_result result;
    int status;
    uint32_t i, t_low, t_high, cycles;
    if (!attached || !aperture(cr2) || !(c->eflags & 0x20000UL)) return 1;
    if (shared->fatal && !host_mode) return 1;
    cvvid_rdtsc(&t_low, &t_high);
    cpu.gpr[CVX_EAX] = c->eax; cpu.gpr[CVX_ECX] = c->ecx;
    cpu.gpr[CVX_EDX] = c->edx; cpu.gpr[CVX_EBX] = c->ebx;
    cpu.gpr[CVX_ESP] = c->esp; cpu.gpr[CVX_EBP] = c->ebp;
    cpu.gpr[CVX_ESI] = c->esi; cpu.gpr[CVX_EDI] = c->edi;
    cpu.eip = c->eip;
    cpu.eflags = c->eflags;
    cpu.sreg[CVX_ES] = (uint16_t)c->es; cpu.sreg[CVX_CS] = (uint16_t)c->cs;
    cpu.sreg[CVX_SS] = (uint16_t)c->ss; cpu.sreg[CVX_DS] = (uint16_t)c->ds;
    cpu.sreg[CVX_FS] = (uint16_t)c->fs; cpu.sreg[CVX_GS] = (uint16_t)c->gs;
    for (i = 0; i < 6; ++i) cpu.seg_base[i] = (uint32_t)cpu.sreg[i] << 4;
    cpu.code32 = 0;
    st.last_fault_csip = ((c->cs & 0xffff) << 16) | (c->eip & 0xffff);
    if (host_mode) {
        host_fb_copy = *fb;
        host_fb = fb->linear ? &host_fb_copy : 0;
        ++st.host_faults;
    } else ++st.faults;
    status = cvx_execute(&cpu, &guest_bus, host_mode ? HOST_BUDGET : GUEST_BUDGET, &result);
    if (host_mode) host_flush();
    if (status == CVX_DONE || status == CVX_PARTIAL) {
        c->eax = cpu.gpr[CVX_EAX]; c->ecx = cpu.gpr[CVX_ECX];
        c->edx = cpu.gpr[CVX_EDX]; c->ebx = cpu.gpr[CVX_EBX];
        c->ebp = cpu.gpr[CVX_EBP]; c->esi = cpu.gpr[CVX_ESI];
        c->edi = cpu.gpr[CVX_EDI];
        c->eip = cpu.eip;
        c->eflags = (c->eflags & ~0x8d5UL) | (cpu.eflags & 0x8d5UL);
        if (!host_mode) {
            ++st.instructions;
            st.elements += result.elements;
        }
        cycles = elapsed(t_low, t_high);
        add64(&st.fault_tsc_lo, &st.fault_tsc_hi, cycles);
        return 0;
    }
    /* Unsupported or failed instruction: never guess. Record the exact bytes,
     * poison the session and let Jemm reflect the fault to the guest. */
    if (!host_mode && !shared->fatal) {
        shared->fatal = status == CVX_BUS_FAULT ? CVVID_FATAL_BUS :
                        status == CVX_DIVIDE_ERROR ? CVVID_FATAL_DIVIDE : CVVID_FATAL_INSTRUCTION;
        st.fatal_csip = st.last_fault_csip;
        for (i = 0; i < 16; ++i) st.fatal_bytes[i] = result.bytes[i];
    }
    if (status == CVX_BUS_FAULT) ++st.bus_faults;
    else ++st.unsupported;
    return 1;
}

/* ---- Ports ---- */

uint32_t cvvid_port_read(uint32_t port)
{
    ++st.port_reads;
    if (!attached) return 0xff;
    if (host_mode) return cvvid_io_in(port, 1) & 0xff;
    return cvga_read_port(vga, (uint16_t)port, port == 0x3da || port == 0x3ba ? status1() : 0);
}

void cvvid_port_write(uint32_t port, uint32_t value)
{
    ++st.port_writes;
    if (!attached) return;
    if (host_mode) {
        cvvid_io_out(port, value & 0xff, 1);
        return;
    }
    cvga_write_port(vga, (uint16_t)port, (uint8_t)value);
}

uint32_t cvvid_port_string(uint32_t port, uint32_t type, cvvid_client *c)
{
    uint32_t size = (type & 0x10) ? 4 : (type & 8) ? 2 : 1, count, i, index, segment, linear, value;
    uint32_t delta, j;
    uint8_t byte;
    int output = (type & 4) != 0;
    if (!attached || host_mode || port < 0x3b0 || port + size > 0x3e0) return 1;
    count = (type & 0x40) ? (c->ecx & 0xffff) : 1;
    segment = type >> 16;
    delta = (c->eflags & 0x400) ? 0 - size : size;
    ++st.string_io;
    for (i = 0; i < count; ++i) {
        index = output ? (c->esi & 0xffff) : (c->edi & 0xffff);
        linear = (segment << 4) + index;
        if (output) {
            value = 0;
            for (j = 0; j < size; ++j) {
                if (guest_read(0, (segment << 4) + ((index + j) & 0xffff), &byte)) return 1;
                value |= (uint32_t)byte << (8 * j);
            }
            for (j = 0; j < size; ++j) cvvid_port_write(port + j, (value >> (8 * j)) & 0xff);
            c->esi = (c->esi & 0xffff0000UL) | ((index + delta) & 0xffff);
        } else {
            for (j = 0; j < size; ++j) {
                byte = (uint8_t)cvvid_port_read(port + j);
                if (guest_write(0, (segment << 4) + ((index + j) & 0xffff), byte)) return 1;
            }
            c->edi = (c->edi & 0xffff0000UL) | ((index + delta) & 0xffff);
        }
        (void)linear;
        ++st.string_elements;
        if (type & 0x40) c->ecx = (c->ecx & 0xffff0000UL) | ((count - i - 1) & 0xffff);
    }
    return 0;
}

/* ---- INT 10h ---- */

static void set_word(uint32_t *reg, uint32_t value)
{
    *reg = (*reg & 0xffff0000UL) | (value & 0xffff);
}

void cvvid_int10(cvvid_client *c)
{
    cvbios_regs r;
    if (!attached) return;
    if (host_mode) {
        /* Host compositor bank switches become window offsets in the owned
         * LFB mapping. No firmware call and no physical bank register. */
        if ((c->eax & 0xffff) == 0x4f05 && !(c->ebx & 0xfe00)) {
            if ((c->ebx & 0xff00) == 0) {
                host_offset = (c->edx & 0xffff) * host_granule;
            } else set_word(&c->edx, host_granule ? host_offset / host_granule : 0);
            set_word(&c->eax, 0x004f);
        } else if ((c->eax & 0xffff) == 0x4f07 && (c->ebx & 0xff) == 1) {
            /* Display start is not moved while the host paints around a
             * guest: report the current origin consistently. */
            set_word(&c->ebx, 0);
            set_word(&c->ecx, 0);
            set_word(&c->edx, 0);
            set_word(&c->eax, 0x004f);
        } else if ((c->eax & 0xffff) == 0x4f07 && ((c->ebx & 0x7f) == 0)) {
            /* Only a set to the current origin is honoured; any page flip
             * fails cleanly (AX=014Fh) instead of claiming a flip. */
            set_word(&c->eax, (c->ecx & 0xffff) || (c->edx & 0xffff) ? 0x014f : 0x004f);
            ++st.host_display_sets;
        } else ++st.port_rejects;
        return;
    }
    r.eax = c->eax; r.ebx = c->ebx; r.ecx = c->ecx; r.edx = c->edx;
    r.esi = c->esi; r.edi = c->edi; r.ebp = c->ebp;
    r.es = (uint16_t)c->es; r.ds = (uint16_t)c->ds;
    cvbios_int10(&bios, &r);
    c->eax = r.eax; c->ebx = r.ebx; c->ecx = r.ecx; c->edx = r.edx;
    c->ebp = r.ebp; c->es = r.es;
}

uint32_t cvvid_readback(uint32_t offset, uint32_t count, uint32_t destination)
{
    uint32_t i;
    if (!attached) return CVVID_E_INACTIVE;
    for (i = 0; i < count; ++i)
        *LINEAR(destination + i) = cvga_peek_vram(vga, 0xa0000UL + offset + i);
    return CVVID_OK;
}

/* ---- Operations ---- */

static void state_packet(uint32_t *p)
{
    cvga_geometry g;
    uint32_t i;
    zero(p, 256);
    p[0] = 0x53565643UL;
    p[1] = 0x0100UL | (256UL << 16);     /* version 0100h, 256 bytes */
    p[2] = attached;
    if (!attached) return;
    p[3] = shared->generation;
    p[4] = st.faults; p[5] = st.instructions; p[6] = st.elements;
    p[7] = st.unsupported; p[8] = st.bus_faults;
    p[9] = st.port_reads; p[10] = st.port_writes;
    p[11] = bios.calls; p[12] = bios.unsupported; p[13] = bios.last_unsupported;
    p[14] = bios.mode_sets; p[15] = cvbios_current_mode(&bios);
    if (cvga_get_geometry(vga, &g)) {
        p[16] = g.width; p[17] = g.height; p[18] = g.scan_repeat; p[19] = g.text;
    }
    p[20] = vga->display_changes; p[21] = vga->changes;
    p[22] = shared->fatal; p[23] = st.fatal_csip;
    for (i = 0; i < 16; ++i) ((uint8_t *)&p[24])[i] = st.fatal_bytes[i];
    p[28] = st.presents; p[29] = st.present_rows; p[30] = st.present_pixels;
    p[31] = st.frames; p[32] = st.full_redraws;
    p[33] = st.last_present_tsc; p[34] = st.max_present_tsc;
    p[35] = st.total_present_lo; p[36] = st.total_present_hi;
    p[37] = host_mode; p[38] = host_offset; p[39] = st.host_faults; p[40] = st.host_bytes;
    p[41] = shared->tsc_khz; p[42] = st.status_polls;
    p[43] = st.fault_tsc_lo; p[44] = st.fault_tsc_hi;
    p[45] = ((uint32_t)vga->crtc[12] << 8) | vga->crtc[13];
    p[46] = share_count; p[47] = (uint32_t)sizeof(cvga_state);
    p[48] = st.host_enters; p[49] = st.port_rejects; p[50] = st.last_fault_csip;
    p[51] = shared->pm_faults; p[52] = shared->pm_instructions;
    p[53] = shared->pm_unsupported; p[54] = shared->pm_attached;
    p[55] = st.passes;
    p[56] = st.string_io; p[57] = st.string_elements;
    p[58] = st.host_display_sets; p[59] = st.damage_queries;
}

static uint32_t text_phases(void)
{
    uint32_t low, high, frame, frame_cycles;
    if (!shared->tsc_khz) return CVP_BLINK_VISIBLE | CVP_CURSOR_VISIBLE;
    cvvid_rdtsc(&low, &high);
    /* VGA toggles the cursor every 8 and blink every 16 frames of 70 Hz:
     * a 32-frame period fits 32 bits for TSC rates below 9 GHz. */
    frame_cycles = shared->tsc_khz * 14u + shared->tsc_khz / 1000u * 286u;
    frame = mod64(high, low, frame_cycles * 32u) / frame_cycles;
    return (frame & 16 ? 0 : CVP_BLINK_VISIBLE) | (frame & 8 ? 0 : CVP_CURSOR_VISIBLE);
}

static void account_present(const cvp_stats *s, uint32_t cycles)
{
    ++st.presents;
    st.present_rows += s->rows_drawn;
    st.present_pixels += s->pixels_written;
    st.frames = s->frames_completed;
    st.full_redraws = s->full_redraws;
    st.passes = s->passes;
    st.last_present_tsc = cycles;
    if (cycles > st.max_present_tsc) st.max_present_tsc = cycles;
    add64(&st.total_present_lo, &st.total_present_hi, cycles);
}

void cvvid_timer(const cvvid_fb *fb)
{
    uint32_t low, high, cycles;
    cvp_request request;
    if (!attached) return;
    state_packet(shared->live);
    if (!armed_valid || host_mode || !fb->linear) return;
    ++armed.ticks;
    cvvid_rdtsc(&low, &high);
    if (armed_interval && high == armed_last_hi && low - armed_last_lo < armed_interval) return;
    armed_last_lo = low;
    armed_last_hi = high;
    if (armed.format.pitch * armed.format.height > fb->bytes) return;
    request = armed.request;
    request.flags = (request.flags & ~(CVP_BLINK_VISIBLE | CVP_CURSOR_VISIBLE)) | text_phases();
    cvp_present(&presenter, vga, &armed.format, &request, fb->linear, &armed.stats);
    cycles = elapsed(low, high);
    account_present(&armed.stats, cycles);
    ++armed.presents;
    armed.last_tsc = cycles;
    if (cycles > armed.max_tsc) armed.max_tsc = cycles;
    /* Report to the owner's packet only while its pages remain owned. */
    if (conventional(armed_linear, sizeof(armed), 1))
        copy_out(armed_linear + 180, &armed.stats, sizeof(armed) - 180);
}

static uint32_t present(cvvid_client *c, const cvvid_fb *fb, int band)
{
    cvvid_present_packet packet;
    cvp_state *state = band ? &band_presenter : &presenter;
    uint32_t start_lo, start_hi, cycles, i, target, extent;
    int result;
    uint8_t *surface;
    if (!client_extent(c, sizeof(packet), 1)) return CVVID_E_ADDRESS;
    copy_in(&packet, client_buffer(c), sizeof(packet));
    if (packet.magic != 0x50565643UL || packet.version != 0x0100 || packet.bytes != sizeof(packet))
        return CVVID_E_ABI;
    if (packet.format.height && packet.format.pitch > 0xffffffffUL / packet.format.height)
        return CVVID_E_FORMAT;
    extent = packet.format.pitch * packet.format.height;
    if (band) {
        target = (uint32_t)packet.band_segment << 4;
        if (packet.band_bytes > 0x10000UL || extent > packet.band_bytes ||
            !conventional(target, packet.band_bytes, 1))
            return CVVID_E_ADDRESS;
        surface = (uint8_t *)(uintptr_t)target;
        /* Screen coordinates become band-relative. */
        packet.request.window.top = (int16_t)(packet.request.window.top - packet.band_top);
        packet.request.window.bottom = (int16_t)(packet.request.window.bottom - packet.band_top);
        for (i = 0; i < packet.request.clip_count && i < CVP_MAX_CLIPS; ++i) {
            packet.request.clip[i].top = (int16_t)(packet.request.clip[i].top - packet.band_top);
            packet.request.clip[i].bottom = (int16_t)(packet.request.clip[i].bottom - packet.band_top);
        }
    } else {
        if (!fb->linear) return CVVID_E_FB_UNBOUND;
        if (extent > fb->bytes) return CVVID_E_FORMAT;
        surface = fb->linear;
    }
    cvvid_rdtsc(&start_lo, &start_hi);
    result = cvp_present(state, vga, &packet.format, &packet.request, surface, &packet.stats);
    cycles = elapsed(start_lo, start_hi);
    if (!band) account_present(&packet.stats, cycles);
    copy_out(client_buffer(c) + 180, &packet.stats, sizeof(packet.stats));
    if (result == CVP_BAD_FORMAT || result == CVP_BAD_RECT) return CVVID_E_FORMAT;
    if (result == CVP_UNSUPPORTED) return CVVID_E_GEOMETRY;
    return CVVID_OK;
}

uint32_t cvvid_operation(uint32_t op, cvvid_client *c, const cvvid_fb *fb)
{
    uint32_t packet[64], offset, count, i, first;
    switch (op) {
    case 0x20:                                            /* VIDEO_CONFIG */
        if (!client_extent(c, sizeof(config), 0)) return CVVID_E_ADDRESS;
        copy_in(packet, client_buffer(c), sizeof(config));
        if (packet[0] != 0x43565643UL || (packet[1] & 0xffff) != 0x0100 ||
            (packet[1] >> 16) != sizeof(config))
            return CVVID_E_ABI;
        for (i = 0; i < CVBIOS_FONT_COUNT; ++i) {
            uint32_t fp = packet[3 + i];
            first = ((fp >> 16) << 4) + (fp & 0xffff);
            /* Firmware fonts live in ROM (C0000-FFFFF) or owned RAM. */
            if (fp && !(first >= 0xc0000UL && first + 4096 <= 0x100000UL) &&
                !conventional(first, 2048, 0))
                return CVVID_E_ADDRESS;
        }
        if (packet[2] && (packet[2] < 1000 || packet[2] > 10000000UL)) return CVVID_E_ABI;
        copy_in(&config, client_buffer(c), sizeof(config));
        if (attached) shared->tsc_khz = config.tsc_khz;
        return CVVID_OK;
    case 0x21:                                            /* VIDEO_STATE */
        if (!client_extent(c, 256, 1)) return CVVID_E_ADDRESS;
        state_packet(packet);
        copy_out(client_buffer(c), packet, 256);
        return CVVID_OK;
    case 0x22:                                            /* VIDEO_READ */
        if (!attached) return CVVID_E_INACTIVE;
        offset = c->edx;
        count = c->ecx & 0xffff;
        if (!count || count > 4096 || offset >= sizeof(cvga_state) ||
            count > sizeof(cvga_state) - offset || !client_extent(c, count, 1))
            return CVVID_E_ADDRESS;
        copy_out(client_buffer(c), (const uint8_t *)vga + offset, count);
        return CVVID_OK;
    case 0x23: {                                          /* VIDEO_RENDER */
        unsigned width, flags = (c->ebx & 3);
        if (!attached) return CVVID_E_INACTIVE;
        count = c->ecx & 0xffff;
        if (!count || count > CVP_MAX_WIDTH || !client_extent(c, count, 1)) return CVVID_E_ADDRESS;
        width = cvga_render_row8(vga, c->edx, row_buffer, count, flags);
        if (!width) return CVVID_E_GEOMETRY;
        copy_out(client_buffer(c), row_buffer, width);
        set_word(&c->ecx, width);
        return CVVID_OK;
    }
    case 0x24: case 0x25:                                 /* PRESENT / BAND */
        if (!attached) return CVVID_E_INACTIVE;
        return present(c, fb, op == 0x25);
    case 0x26:                                            /* HOST_ENTER */
        if (!attached) return CVVID_E_INACTIVE;
        if (host_mode || !fb->linear) return CVVID_E_HOST;
        if (!client_extent(c, 16, 0)) return CVVID_E_ADDRESS;
        copy_in(packet, client_buffer(c), 16);
        if (packet[0] != 0x48565643UL || !packet[1] || packet[1] > 0x10000UL ||
            (packet[1] & (packet[1] - 1)) || (packet[2] > 0xffff && packet[2] != 0xffffffffUL))
            return CVVID_E_ABI;
        /* FFFFFFFFh keeps the window the host selected during its previous
         * host period (its cached VBE bank must stay valid across ticks). */
        if (packet[2] != 0xffffffffUL || packet[1] != host_granule) host_offset =
            (packet[2] == 0xffffffffUL ? 0 : packet[2]) * packet[1];
        host_granule = packet[1];
        host_mode = 1;
        ++st.host_enters;
        return CVVID_OK;
    case 0x27:                                            /* HOST_LEAVE */
        if (!host_mode) return CVVID_E_HOST;
        host_mode = 0;
        host_fb = 0;
        return CVVID_OK;
    case 0x28: {                                          /* SHARE */
        uint32_t pages = (uint32_t)(CVVID_BLOCK_BYTES / 4096), base = (uint32_t)(uintptr_t)shared;
        if (!attached) return CVVID_E_INACTIVE;
        if (!client_extent(c, 8 + pages * 4, 1)) return CVVID_E_ADDRESS;
        packet[0] = 0x48535643UL;
        packet[1] = pages;
        copy_out(client_buffer(c), packet, 8);
        for (i = 0; i < pages; ++i) {
            uint32_t physical = PAGE_MAP[(base >> 12) + i] & 0xfffff000UL;
            copy_out(client_buffer(c) + 8 + i * 4, &physical, 4);
        }
        ++share_count;
        return CVVID_OK;
    }
    case 0x29:                                            /* UNSHARE */
        if (!share_count) return CVVID_E_SHARED;
        --share_count;
        return CVVID_OK;
    case 0x2a: {                                          /* ARM */
        if (!attached) return CVVID_E_INACTIVE;
        if (!fb->linear) return CVVID_E_FB_UNBOUND;
        if (!client_extent(c, sizeof(armed), 1)) return CVVID_E_ADDRESS;
        copy_in(&armed, client_buffer(c), sizeof(armed));
        armed_valid = 0;
        if (armed.magic != 0x50565643UL || armed.version != 0x0100 || armed.bytes != sizeof(armed))
            return CVVID_E_ABI;
        if (armed.format.height && armed.format.pitch > 0xffffffffUL / armed.format.height)
            return CVVID_E_FORMAT;
        if (armed.format.pitch * armed.format.height > fb->bytes) return CVVID_E_FORMAT;
        /* Validate once with a zero-row budget probe: format and rectangle. */
        if (armed.request.window.right <= armed.request.window.left ||
            armed.request.window.bottom <= armed.request.window.top ||
            armed.request.clip_count > CVP_MAX_CLIPS ||
            armed.format.bytes < 2 || armed.format.bytes > 4)
            return CVVID_E_FORMAT;
        armed_linear = client_buffer(c);
        armed_interval = armed.interval_us * (shared->tsc_khz / 1000);
        armed.ticks = armed.presents = armed.last_tsc = armed.max_tsc = 0;
        cvvid_rdtsc(&armed_last_lo, &armed_last_hi);
        armed_last_hi -= 1;                       /* first tick presents */
        armed_valid = 1;
        return CVVID_OK;
    }
    case 0x2b:                                            /* DISARM */
        armed_valid = 0;
        return CVVID_OK;
    case 0x2c: {                                          /* ACCESS */
        uint8_t entries[8 + 64 * 8];
        uint32_t n, k, address;
        if (!attached) return CVVID_E_INACTIVE;
        if (host_mode) return CVVID_E_HOST;
        if (!client_extent(c, 8, 1)) return CVVID_E_ADDRESS;
        copy_in(entries, client_buffer(c), 8);
        n = entries[4] | ((uint32_t)entries[5] << 8);
        if (entries[0] != 'C' || entries[1] != 'V' || entries[2] != 'V' || entries[3] != 'A' ||
            !n || n > 64 || entries[6] || entries[7])
            return CVVID_E_ABI;
        if (!client_extent(c, 8 + n * 8, 1)) return CVVID_E_ADDRESS;
        copy_in(entries, client_buffer(c), 8 + n * 8);
        for (k = 0; k < n; ++k) {
            uint8_t *e = entries + 8 + k * 8;
            address = e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
            if (!aperture(address) || e[4] > 1) return CVVID_E_ADDRESS;
        }
        for (k = 0; k < n; ++k) {
            uint8_t *e = entries + 8 + k * 8;
            address = e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
            if (e[4]) cvga_write_vram(vga, address, e[5]);
            else e[5] = cvga_read_vram(vga, address);
        }
        ++shared->pm_faults;
        shared->pm_instructions += n;
        copy_out(client_buffer(c), entries, 8 + n * 8);
        return CVVID_OK;
    }
    case 0x2e: {                                          /* DAMAGE */
        /* EDX = destination window height, ECX = band height. Returns in
         * EDX the bands (bit n = rows n*band..) whose pixels may differ from
         * the last query. Consumes the model's dirty bits: use either this
         * compositor-driven path or ARM, not both. */
        cvga_geometry g;
        uint32_t height = c->edx & 0xffff, band = c->ecx & 0xffff, mask = 0, y, row, i, j;
        uint32_t phases, full, physical;
        if (!attached) return CVVID_E_INACTIVE;
        if (!height || height > CVP_MAX_HEIGHT || !band || (height + band - 1) / band > 32)
            return CVVID_E_FORMAT;
        ++st.damage_queries;
        if (!cvga_get_geometry(vga, &g)) {
            damage.valid = 0;
            c->edx = 0;
            return CVVID_E_GEOMETRY;
        }
        phases = g.text ? text_phases() : 0;
        full = !damage.valid || vga->display_changes != damage.display_changes ||
               g.width != damage.geometry.width || g.height != damage.geometry.height ||
               g.scan_repeat != damage.geometry.scan_repeat || g.text != damage.geometry.text ||
               g.blank != damage.geometry.blank;
        for (i = 0; i < 4; ++i)
            for (j = 0; j < CVGA_DIRTY_BYTES; ++j) {
                damage_taken[i][j] = vga->dirty[i][j];
                vga->dirty[i][j] = 0;
            }
        if (!full && g.text)
            for (j = 0; j < CVGA_DIRTY_BYTES && !full; ++j) full = damage_taken[2][j] != 0;
        physical = g.height * g.scan_repeat;
        for (y = 0; y < height; ++y) {
            if (mask & (1UL << (y / band))) continue;
            row = ((y * physical) / height) / g.scan_repeat;
            if (full || cvga_row_reads_dirty(vga, row, (const uint8_t (*)[CVGA_DIRTY_BYTES])damage_taken) ||
                (phases != damage.phases && cvga_row_phase_sensitive(vga, row)))
                mask |= 1UL << (y / band);
        }
        damage.valid = 1;
        damage.display_changes = vga->display_changes;
        damage.geometry = g;
        damage.phases = phases;
        c->edx = mask;
        return CVVID_OK;
    }
    case 0x2d: {                                          /* HDPMI INT 10h bridge */
        uint32_t operation = c->eax & 0xffff;
        c->eax >>= 16;
        cvvid_int10(c);
        c->eax = (c->eax << 16) | operation;
        return CVVID_OK;
    }
    default:
        return CVVID_E_OPERATION;
    }
}
