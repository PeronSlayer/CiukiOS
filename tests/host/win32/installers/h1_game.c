/* f7: the H1 adapter's game mode ("game-exec"). Test-only, never in an image.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * A full-screen Win32 game (Quake III Arena) needs more of the kernel and
 * the desktop than an installer: a desktop connection with window surfaces,
 * the thread message queue, the display query and the full-screen lease.
 * This file answers them inside the confined i386 adapter, so the original
 * executable runs on the production PE DLLs (user32, gdi32, opengl32 with
 * the software renderer, dsound, dinput, wsock32) up to rendered frames:
 *
 * - CIUKI_SYS_* gate calls (the 0x7ffe0100 page): SLEEP_MS, CLOCK_GETTIME,
 *   DISPLAY query (1024x768x32 desktop; 640x480, 800x600, 1024x768 at 16
 *   and 32 bits), SURFACE_CREATE/INFO/MAP, MUNMAP, CLOSE, CHANNEL_SEND/RECV
 *   on the desktop endpoint, and CIUKI_SYS_W32 records (as int 0x80 would).
 * - Kernel records: W32_SERVICE desktop connection, W32_MESSAGE (one
 *   thread's posted queue, WM_QUIT, wake bits, status), W32_WAIT without
 *   handles (message wait, at most 10 ms per slice) and W32_DISPLAY frames.
 * - A fake desktop with the v3 wire format of apps/desktop/protocol.c: every
 *   request gets its RESULT; ACTIVATE gives the focus (DirectInput needs the
 *   foreground window); LEASE grants the requested mode with the 72-byte
 *   tuple, as the desktop does, before the RESULT.
 *
 * Threads stay unavailable (W32_THREAD ENOSYS): DirectSound's mixer thread
 * cannot start, so a game runs without sound here (the audio path has its
 * own host tests). Frames are counted with a checksum; when the directory
 * q3-frames exists in the adapter's working directory, frames 1, 2, 4, 8 ...
 * and every 256th are written there as PPM files. */
#include "h1_services.h"
#include <ciuki/abi.h>
#include <ciuki/w32_core.h>
#include <ciuki/w32_transport.h>
#include <ciuki/present_abi.h>
#include "../../../../apps/desktop/protocol.h"

#define DESKTOP_FD 900
#define SURFACE_FD 1000
#define SURFACES 64
#define QUEUE 2048
#define INBOX 512

static int32_t (*kernel_call)(void *, uint32_t);
static int enabled;
static struct { uint8_t *pixels; uint32_t width, height, pitch, bytes; int used; } surfaces[SURFACES];
static struct { uint32_t id, generation, fd; } windows[64];
static struct ciuki_message inbox[INBOX];
static uint32_t in_head, in_tail, focus_id, focus_generation;
static uint32_t lease_token = 0x40, lease_generation = 0x80, lease_serial, lease_w, lease_h, lease_bpp;
static int lease_eio;
static unsigned lease_failures, close_after_eio, close_sent;
static struct ciuki_w32_message queue[QUEUE];
static uint32_t queue_bits[QUEUE], queue_count, quit_pending, quit_code, marked, changed;
static uint32_t frames, presents, requests, refused, gamma_records, first_ms, last_ms, checksum;
static uint64_t frame_sum;
/* Diagnostics: the last 64 gate calls and kernel records, and a watchdog
 * that reports them and ends the run after WATCHDOG_MS (a hung program
 * would otherwise be killed by the driver with nothing to show). */
#define WATCHDOG_MS 3600000u /* a 640x480 timedemo of the software renderer takes tens of minutes */
static uint32_t trace_ring[64], trace_count, start_ms, opcode_log;
static void trace(uint32_t v);
static void watchdog(void);

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void hex(uint32_t v) { char t[11] = "0x00000000"; for (int i = 9; i >= 2; i--, v >>= 4) t[i] = "0123456789abcdef"[v & 15]; host_print(t); }
static uint32_t now_ms(void) {
    struct { uint32_t sec, nsec; } t = {0, 0};
    host_syscall(265, 1, (ULONG)(uintptr_t)&t, 0, 0, 0, 0); /* clock_gettime(CLOCK_MONOTONIC) */
    return t.sec * 1000u + t.nsec / 1000000u;
}
static void sleep_ms(uint32_t ms) {
    struct { uint32_t sec, nsec; } t = {ms / 1000u, (ms % 1000u) * 1000000u};
    host_syscall(162, (ULONG)(uintptr_t)&t, 0, 0, 0, 0, 0); /* nanosleep */
}

static void trace(uint32_t v) {
    trace_ring[trace_count++ % 64] = v;
    if (!(trace_count & 1023))
        watchdog();
}
static void watchdog(void) {
    if (!start_ms || now_ms() - start_ms < WATCHDOG_MS)
        return;
    host_print("GAME watchdog: last events (gate number, or kernel op<<16|action)");
    for (uint32_t i = trace_count > 64 ? trace_count - 64 : 0; i < trace_count; i++) { host_print(" "); hex(trace_ring[i % 64]); }
    host_print("\n");
    h1_game_report();
    host_exit(124);
}

/* ---- CPU profile ---------------------------------------------------------
 * SIGPROF every millisecond of process CPU time; the interrupted EIP is
 * counted per 64-byte block. The driver maps the blocks to modules (and the
 * opengl32 map file to functions). Read-only handler: no FS/TEB use. */
#define PROFILE_SLOTS 8192
static uint32_t profile_eip[PROFILE_SLOTS], profile_hits[PROFILE_SLOTS], profile_total, profile_lost;
extern void host_sigreturn(void);
static void profile_signal(int sig, void *info, void *context) {
    (void)sig; (void)info;
    uint32_t eip = *(const uint32_t *)((const uint8_t *)context + 76) & ~63u; /* uc_mcontext.gregs[REG_EIP] */
    uint32_t h = (eip >> 6) * 2654435761u >> 19;
    profile_total++;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t k = (h + i) & (PROFILE_SLOTS - 1);
        if (profile_hits[k] && profile_eip[k] != eip)
            continue;
        profile_eip[k] = eip;
        profile_hits[k]++;
        return;
    }
    profile_lost++;
}
static void profile_start(void) {
    struct { void *handler; ULONG flags; void *restorer; ULONG mask[2]; } action =
        {(void *)profile_signal, 0x14000004u /* SA_SIGINFO|SA_RESTORER|SA_RESTART */, host_sigreturn, {0, 0}};
    if (host_syscall(174, 27 /* SIGPROF */, (ULONG)(uintptr_t)&action, 0, 8, 0, 0))
        return;
    struct { uint32_t is, iu, vs, vu; } timer = {0, 1000, 0, 1000};
    host_syscall(104, 2 /* ITIMER_PROF */, (ULONG)(uintptr_t)&timer, 0, 0, 0, 0);
}
static void profile_report(void) {
    struct { uint32_t is, iu, vs, vu; } off = {0, 0, 0, 0};
    host_syscall(104, 2, (ULONG)(uintptr_t)&off, 0, 0, 0, 0);
    host_print("GAME profile samples="); host_number(profile_total); host_print(" lost="); host_number(profile_lost); host_print("\n");
    for (unsigned rank = 0; rank < 60; rank++) {
        uint32_t best = 0, at = 0;
        for (uint32_t k = 0; k < PROFILE_SLOTS; k++)
            if (profile_hits[k] > best) { best = profile_hits[k]; at = k; }
        if (!best)
            break;
        host_print("GAME hot eip="); hex(profile_eip[at]); host_print(" samples="); host_number(best); host_print("\n");
        profile_hits[at] = 0;
    }
}

/* ---- GL call profile ------------------------------------------------------
 * Test-only. After opengl32.dll's DllMain(DLL_PROCESS_ATTACH) the export
 * table of the mapped image is redirected: each exported function's RVA
 * points to an 8-byte thunk (call h1_gl_count, three bytes of padding)
 * written into the image's .reloc section, which is dead after the load
 * (the builtin is mapped at its preferred base) and is the only part of
 * the image large enough: GetProcAddress accepts only RVAs inside a section
 * (sdk/pe/pe.c pe_span). h1_gl_count counts the call and continues to the
 * original function with the caller's stack intact. wglGetProcAddress is
 * routed through a wrapper, so the extension functions a program fetches
 * (Quake III: glMultiTexCoord2fARB, glLockArraysEXT, ...) get thunks too.
 * Calls made inside opengl32 do not pass through the export table and are
 * not counted; GDI's SwapBuffers is counted as the lease frames above. */
#define GL_SLOTS 448
#define GL_NAME 40
uint32_t h1_gl_thunks, h1_gl_counts[GL_SLOTS], h1_gl_originals[GL_SLOTS];
static char gl_names[GL_SLOTS][GL_NAME];
static uint32_t gl_slots, gl_capacity, gl_loads;
static void *(W32API *real_wgl_get_proc)(const char *);
extern void h1_gl_count(void);
__asm__(".text\n.globl h1_gl_count\n.type h1_gl_count,@function\n"
        "h1_gl_count:\n"
        "  movl (%esp),%eax\n"           /* return address: thunk + 5 */
        "  subl h1_gl_thunks,%eax\n"
        "  subl $5,%eax\n"
        "  shrl $3,%eax\n"               /* slot */
        "  incl h1_gl_counts(,%eax,4)\n"
        "  movl h1_gl_originals(,%eax,4),%eax\n"
        "  movl %eax,(%esp)\n"
        "  ret\n");
static int gl_slot(const char *name) {
    for (uint32_t i = 0; i < gl_slots; i++)
        if (!w32_strcmp(gl_names[i], name))
            return (int)i;
    if (gl_slots >= gl_capacity)
        return -1;
    uint32_t n = 0;
    while (name[n] && n < GL_NAME - 1) { gl_names[gl_slots][n] = name[n]; n++; }
    gl_names[gl_slots][n] = 0;
    return (int)gl_slots++;
}
static uint32_t gl_thunk(int slot, uint32_t target) {
    uint8_t *t = (uint8_t *)(uintptr_t)(h1_gl_thunks + (uint32_t)slot * 8);
    t[0] = 0xe8;
    put32(t + 1, (uint32_t)(uintptr_t)h1_gl_count - (h1_gl_thunks + (uint32_t)slot * 8 + 5));
    t[5] = t[6] = t[7] = 0x90;
    h1_gl_originals[slot] = target;
    return (uint32_t)(uintptr_t)t;
}
static void *W32API wrapped_wgl_get_proc(const char *name) {
    void *p = real_wgl_get_proc ? real_wgl_get_proc(name) : 0;
    if (!p || !name)
        return p;
    int slot = gl_slot(name);
    return slot < 0 ? p : (void *)(uintptr_t)gl_thunk(slot, (uint32_t)(uintptr_t)p);
}
static int gl_protect(uint32_t address, uint32_t bytes) {
    uint32_t start = address & ~4095u, end = (address + bytes + 4095u) & ~4095u;
    return host.services.protect(&host, (void *)(uintptr_t)start, end - start, 7);
}
void h1_game_module(const char *name, void *base) {
    if (!enabled || w32_strcmp(name, "opengl32.dll"))
        return;
    const uint8_t *b = base;
    uint32_t pe = get32(b + 0x3c), sections = b[pe + 6] | (uint32_t)b[pe + 7] << 8,
             optional = b[pe + 20] | (uint32_t)b[pe + 21] << 8;
    uint32_t export_rva = get32(b + pe + 0x78), export_size = get32(b + pe + 0x7c), reloc_rva = 0, reloc_span = 0;
    for (uint32_t i = 0; i < sections; i++) {
        const uint8_t *s = b + pe + 24 + optional + 40 * i;
        if (s[0] == '.' && s[1] == 'r' && s[2] == 'e' && s[3] == 'l' && s[4] == 'o' && s[5] == 'c' && !s[6]) {
            uint32_t virtual_size = get32(s + 8), raw = get32(s + 16);
            reloc_rva = get32(s + 12);
            reloc_span = virtual_size > raw ? virtual_size : raw;
        }
    }
    if (!export_rva || !reloc_rva || reloc_span < 8 * 64) {
        host_print("GAME gl-profile unavailable (no export table or .reloc section)\n");
        return;
    }
    h1_gl_thunks = (uint32_t)(uintptr_t)b + reloc_rva;
    gl_capacity = reloc_span / 8 < GL_SLOTS ? reloc_span / 8 : GL_SLOTS;
    const uint8_t *e = b + export_rva;
    uint32_t nf = get32(e + 20), nn = get32(e + 24);
    uint8_t *funcs = (uint8_t *)(uintptr_t)(b + get32(e + 28));
    const uint8_t *names = b + get32(e + 32), *ords = b + get32(e + 36);
    if (!gl_protect(h1_gl_thunks, reloc_span) || !gl_protect((uint32_t)(uintptr_t)funcs, nf * 4)) {
        host_print("GAME gl-profile unavailable (protect)\n");
        return;
    }
    uint32_t hooked = 0;
    for (uint32_t j = 0; j < nn; j++) {
        uint32_t index = ords[j * 2] | (uint32_t)ords[j * 2 + 1] << 8;
        if (index >= nf)
            continue;
        uint32_t rva = get32(funcs + index * 4);
        if (!rva || (rva >= export_rva && rva - export_rva < export_size))
            continue; /* absent or a forwarder */
        const char *symbol = (const char *)(b + get32(names + j * 4));
        int slot = gl_slot(symbol);
        if (slot < 0)
            break;
        uint32_t target = (uint32_t)(uintptr_t)b + rva;
        if (!w32_strcmp(symbol, "wglGetProcAddress")) {
            real_wgl_get_proc = (void *(W32API *)(const char *))(uintptr_t)target;
            target = (uint32_t)(uintptr_t)wrapped_wgl_get_proc;
        }
        put32(funcs + index * 4, gl_thunk(slot, target) - (uint32_t)(uintptr_t)b);
        hooked++;
    }
    gl_loads++;
    host_print("GAME gl-profile hooked="); host_number(hooked); host_print(" load="); host_number(gl_loads); host_print("\n");
}
static void gl_report(void) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < gl_slots; i++)
        total += h1_gl_counts[i];
    host_print("GAME gl total="); host_number(total); host_print(" names="); host_number(gl_slots);
    host_print(" frames="); host_number(frames); host_print("\n");
    for (;;) {
        uint32_t best = 0, at = 0;
        for (uint32_t i = 0; i < gl_slots; i++)
            if (h1_gl_counts[i] > best) { best = h1_gl_counts[i]; at = i; }
        if (!best)
            break;
        host_print("GAME gl name="); host_print(gl_names[at]); host_print(" calls="); host_number(best); host_print("\n");
        h1_gl_counts[at] = 0;
    }
}

/* ---- frames ------------------------------------------------------------ */
static void dump(const uint8_t *pixels, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp) {
    char name[40] = "q3-frames/f00000.ppm";
    uint32_t n = frames;
    for (int i = 15; i >= 11; i--, n /= 10) name[i] = (char)('0' + n % 10);
    LONG fd = host_syscall(5, (ULONG)(uintptr_t)name, 01 | 0100 | 01000, 0644, 0, 0, 0); /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd < 0)
        return;
    char header[32] = "P6\n";
    unsigned at = 3;
    uint32_t values[2] = {w, h};
    for (int k = 0; k < 2; k++) {
        char digits[10]; int d = 0; uint32_t v = values[k];
        do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (d) header[at++] = digits[--d];
        header[at++] = k ? '\n' : ' ';
    }
    w32_copy(header + at, "255\n", 4); at += 4;
    host_syscall(4, (ULONG)fd, (ULONG)(uintptr_t)header, at, 0, 0, 0);
    static uint8_t row[2048 * 3];
    for (uint32_t y = 0; y < h && w <= 2048; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = bpp == 16 ? (uint32_t)(pixels[y * pitch + x * 2] | pixels[y * pitch + x * 2 + 1] << 8)
                                   : get32(pixels + y * pitch + x * 4);
            if (bpp == 16)
                p = ((p >> 11) & 31) * 255 / 31 << 16 | ((p >> 5) & 63) * 255 / 63 << 8 | (p & 31) * 255 / 31;
            row[x * 3] = (uint8_t)(p >> 16); row[x * 3 + 1] = (uint8_t)(p >> 8); row[x * 3 + 2] = (uint8_t)p;
        }
        host_syscall(4, (ULONG)fd, (ULONG)(uintptr_t)row, w * 3, 0, 0, 0);
    }
    host_syscall(6, (ULONG)fd, 0, 0, 0, 0, 0);
}
static void frame(const uint8_t *pixels, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp, const char *kind) {
    uint32_t t = now_ms(), sum = 2166136261u;
    if (!frames)
        first_ms = t;
    last_ms = t;
    frames++;
    for (uint32_t y = 0; y < h; y += 7)
        for (uint32_t x = 0; x < w * (bpp / 8); x += 13)
            sum = (sum ^ pixels[y * pitch + x]) * 16777619u;
    checksum = sum;
    frame_sum += sum;
    if (frames <= 4 || !(frames & 255) || !(frames & (frames - 1))) {
        host_print("GAME frame n="); host_number(frames); host_print(" kind="); host_print(kind);
        host_print(" size="); host_number(w); host_print("x"); host_number(h); host_print("x"); host_number(bpp);
        host_print(" checksum="); hex(sum); host_print(" ms="); host_number(t - first_ms); host_print("\n");
        dump(pixels, w, h, pitch, bpp);
    }
}

/* ---- keyboard script ---------------------------------------------------- */
/* Quake III console typing through the production user32 (QEMU 2026-10-26:
 * every other keystroke lost). The game's command line may carry
 * +set ciuki_keys "text" (an unknown cvar Quake accepts); after the first
 * lease frames the stand-in desktop types ` (console), the text and Enter as
 * the kernel would deliver them: KEY down with its TEXT on one lease frame,
 * KEY up on the next, three idle frames between keys; the console echoes the
 * line as "]text" in qconsole.log. */
static struct ciuki_message *event(unsigned op, unsigned payload, uint32_t token, uint32_t id, uint32_t generation);
static char key_script[200]; static unsigned key_at, key_phase, key_idle, key_gap = 3; static int key_active, key_burst;
static const uint8_t key_scan[128] = {
    ['a']=0x1e,['b']=0x30,['c']=0x2e,['d']=0x20,['e']=0x12,['f']=0x21,['g']=0x22,['h']=0x23,['i']=0x17,['j']=0x24,['k']=0x25,
    ['l']=0x26,['m']=0x32,['n']=0x31,['o']=0x18,['p']=0x19,['q']=0x10,['r']=0x13,['s']=0x1f,['t']=0x14,['u']=0x16,['v']=0x2f,
    ['w']=0x11,['x']=0x2d,['y']=0x15,['z']=0x2c,['0']=0x0b,['1']=0x02,['2']=0x03,['3']=0x04,['4']=0x05,['5']=0x06,['6']=0x07,
    ['7']=0x08,['8']=0x09,['9']=0x0a,[' ']=0x39,['`']=0x29,['\n']=0x1c,['-']=0x0c,['.']=0x34,['/']=0x35};
/* +set ciuki_keys_gap N: idle lease frames between keys (default 3); N=0
 * puts the key up on the frame of its key down; +set ciuki_keys_burst 1
 * delivers the whole line within one frame (a slow frame of the renderer). */
static const char *after(const char *line, const char *marker) {
    for (const char *p = line; p && *p; p++) {
        unsigned k = 0;
        while (marker[k] && p[k] == marker[k]) k++;
        if (!marker[k]) return p + k;
    }
    return 0;
}
void h1_game_script(const char *command_line) {
    lease_eio = after(command_line, "+set ciuki_lease_eio 1") != 0;
    close_after_eio = after(command_line, "+set ciuki_close_after_eio 1") != 0;
    const char *text = after(command_line, "+set ciuki_keys \"");
    if (!text) return;
    unsigned n = 0;
    key_script[n++] = '`';
    while (text[n - 1] && text[n - 1] != '"' && n < sizeof(key_script) - 2) { key_script[n] = text[n - 1]; n++; }
    key_script[n++] = '\n'; key_script[n] = 0; key_active = 1;
    const char *gap = after(command_line, "+set ciuki_keys_gap ");
    if (gap) key_gap = (unsigned)(*gap - '0');
    const char *burst = after(command_line, "+set ciuki_keys_burst ");
    if (burst) key_burst = *burst == '1';
    host_print("GAME keys script="); host_print(key_script); host_print(" gap="); host_number(key_gap);
    host_print(" burst="); host_number(key_burst); host_print("\n");
}
static void key_event(uint32_t type, uint32_t code, uint32_t value, uint32_t ms) {
    struct ciuki_message *m = event(DESK_KEY, 40, 0, focus_id, focus_generation);
    host_print("GAME key frame="); host_number(frames); host_print(" type="); host_number(type); host_print(" code="); hex(code);
    host_print(" value="); host_number(value); host_print(" ms="); host_number(ms); host_print(m ? "\n" : " DROPPED\n");
    if (!m) return;
    uint8_t *e = m->data + 16;
    put32(e + 12, type); put32(e + 16, ms * 1000000u); put32(e + 20, 0); put32(e + 24, code); put32(e + 28, value);
}
static void key_frame(void) {
    if (!key_active || !focus_id || frames < 40) return;
    if (key_idle) { key_idle--; return; }
    for (;;) {
        char c = key_script[key_at];
        if (!c) { key_active = 0; host_print("GAME keys done\n"); return; }
        uint8_t scan = key_scan[(uint8_t)c];
        uint32_t ms = now_ms() - start_ms + key_at * 400;
        if (!key_phase) {
            key_event(CIUKI_INPUT_KEY, scan, 1, ms);
            key_event(CIUKI_INPUT_TEXT, c == '\n' ? '\r' : (uint8_t)c, 0, ms);
            key_phase = 1;
            if (key_gap && !key_burst) return;        /* the key up on the next frame */
        }
        key_event(CIUKI_INPUT_KEY, scan, 0, ms + 40);
        key_phase = 0; key_at++; key_idle = key_gap;
        if (!key_burst) return;
    }
}
/* ---- fake desktop ------------------------------------------------------ */
static struct ciuki_message *event(unsigned op, unsigned payload, uint32_t token, uint32_t id, uint32_t generation) {
    if (in_tail - in_head >= INBOX)
        return 0;
    struct ciuki_message *m = &inbox[in_tail++ % INBOX];
    w32_fill(m, 0, sizeof(*m));
    m->length = DESK_V3_HEADER + payload;
    m->data[0] = DESK_V3_VERSION;
    m->data[1] = (uint8_t)op;
    put32(m->data + 4, token); put32(m->data + 8, id); put32(m->data + 12, generation);
    return m;
}
static void activate(uint32_t id, uint32_t generation) {
    if (focus_id && (focus_id != id || focus_generation != generation)) {
        struct ciuki_message *off = event(DESK_V3_ACTIVATE, 8, 0, focus_id, focus_generation);
        if (off) { put32(off->data + 16, 0); put32(off->data + 20, 0); }
    }
    focus_id = id; focus_generation = generation;
    struct ciuki_message *on = event(DESK_V3_ACTIVATE, 8, 0, id, generation);
    if (on) { put32(on->data + 16, 1); put32(on->data + 20, 1); }
}
static void lease_event(uint32_t id, uint32_t generation, uint32_t state, uint32_t w, uint32_t h, uint32_t bpp) {
    struct ciuki_message *m = event(DESK_V3_LEASE_EVENT, 72, 0, id, generation);
    if (!m)
        return;
    uint32_t pitch = (w * (bpp / 8) + 3) & ~3u;
    uint32_t words[18] = {state, ++lease_serial, w, h, bpp};
    if (state == 1) {
        uint32_t tuple[13] = {++lease_token, ++lease_generation, 7, 1, w, h, pitch, pitch * h,
                              bpp == 8 ? 4u : bpp == 16 ? 2u : 1u, 1024, 768, 32, 0};
        w32_copy(words + 5, tuple, sizeof(tuple));
        lease_w = w; lease_h = h; lease_bpp = bpp;
    } else
        lease_w = lease_h = lease_bpp = 0;
    for (unsigned i = 0; i < 18; i++)
        put32(m->data + 16 + i * 4, words[i]);
}
static int32_t desktop_send(const struct ciuki_message *m) {
    if (m->length < 2 || m->length > sizeof(m->data))
        return -22;
    unsigned op = m->data[1];
    if (m->data[0] != DESK_V3_VERSION || m->length < DESK_V3_HEADER)
        return 0; /* HELLO and other legacy-form messages need no answer */
    uint32_t token = get32(m->data + 4), id = get32(m->data + 8), generation = get32(m->data + 12);
    requests++;
    if (opcode_log < 200 && op != DESK_DAMAGE && op != DESK_V3_PRESENT) {
        opcode_log++;
        host_print("GAME desktop op="); host_number(op); host_print(" window="); host_number(id);
        host_print(" generation="); host_number(generation); host_print(" bytes="); host_number(m->length);
        if (op == DESK_CREATE_WINDOW && m->length > DESK_V3_HEADER + 36) {
            char title[80]; unsigned n = m->length - DESK_V3_HEADER - 36;
            if (n > 79) n = 79;
            w32_copy(title, m->data + DESK_V3_HEADER + 36, n); title[n] = 0;
            host_print(" title="); host_print(title);
        }
        host_print("\n");
    }
    int32_t result = 0;
    if (op == DESK_CREATE_WINDOW && m->fd_count == 1) {
        for (unsigned i = 0; i < 64; i++)
            if (!windows[i].id || (windows[i].id == id && windows[i].generation != generation)) {
                windows[i].id = id; windows[i].generation = generation; windows[i].fd = (uint32_t)m->fds[0];
                break;
            }
    }
    if (op == DESK_V3_RESIZE && m->fd_count == 1)
        for (unsigned i = 0; i < 64; i++)
            if (windows[i].id == id && windows[i].generation == generation)
                windows[i].fd = (uint32_t)m->fds[0];
    if (op == DESK_CLOSE)
        for (unsigned i = 0; i < 64; i++)
            if (windows[i].id == id && windows[i].generation == generation)
                windows[i].id = 0;
    if (op == DESK_V3_ACTIVATE && m->length >= DESK_V3_HEADER + 8 && get32(m->data + 16))
        activate(id, generation);
    if (op == DESK_V3_LEASE && m->length >= DESK_V3_HEADER + 16) {
        uint32_t action = get32(m->data + 16);
        if (action && lease_eio) {
            lease_failures++;
            host_print("GAME lease injected result=-5\n");
            result = -5;
        } else if (action)
            lease_event(id, generation, 1, get32(m->data + 20), get32(m->data + 24), get32(m->data + 28));
        else if (lease_w)
            lease_event(id, generation, 3, lease_w, lease_h, lease_bpp);
    }
    if (op == DESK_V3_PRESENT || op == DESK_DAMAGE) {
        presents++;
        for (unsigned i = 0; i < 64 && op == DESK_V3_PRESENT; i++)
            if (windows[i].id == id && windows[i].generation == generation) {
                uint32_t k = windows[i].fd - SURFACE_FD;
                if (k < SURFACES && surfaces[k].used)
                    frame(surfaces[k].pixels, surfaces[k].width, surfaces[k].height, surfaces[k].pitch, 32, "window");
            }
    }
    if (op == DESK_INPUT_SETTINGS) {
        struct ciuki_message *s = event(DESK_INPUT_STATE, DESK_INPUT_VALUES * 4 + 8, token, id, generation);
        static const uint32_t input[DESK_INPUT_VALUES] = {0, 1, 31, 530, 0, 0, 3, 0, 10, 500, 1};
        for (unsigned k = 0; s && k < DESK_INPUT_VALUES; k++)
            put32(s->data + 16 + k * 4, input[k]);
        if (s) { put32(s->data + 16 + DESK_INPUT_VALUES * 4, get32(m->data + 16)); put32(s->data + 20 + DESK_INPUT_VALUES * 4, get32(m->data + 24)); }
    }
    if (token) {
        struct ciuki_message *r = event(DESK_V3_RESULT, 8, token, id, generation);
        if (r) { put32(r->data + 16, op); put32(r->data + 20, (uint32_t)result); }
    }
    if (op == DESK_CREATE_WINDOW && !focus_id)
        activate(id, generation);
    return 0;
}

/* ---- message queue ------------------------------------------------------ */
static int matches(const struct ciuki_w32_message_args *a, uint32_t k) {
    const struct ciuki_w32_message *m = &queue[k];
    uint32_t wanted = a->message.window, lo = a->message.message, hi = a->message.wparam;
    if (a->bits && !(queue_bits[k] & a->bits))
        return 0;
    if (wanted == 0xffffffffu) {
        if (m->window)
            return 0;
    } else if (wanted) {
        int found = m->window == wanted;
        const uint32_t *list = (const uint32_t *)(uintptr_t)a->buffer;
        for (uint32_t i = 0; !found && list && i < a->bytes / 4; i++)
            found = list[i] == m->window;
        if (!found)
            return 0;
    }
    return (!lo && !hi) || (m->message >= lo && m->message <= hi);
}
static int32_t message(struct ciuki_w32_message_args *a) {
    a->result = a->error = 0;
    switch (a->action) {
    case W32_MSG_INPUT_IDLE:
        /* Game mode has one GUI queue. Accept yielding PeekMessage's
         * publication; cross-process GUI waits belong to the kernel fixture. */
        if (a->tid && !a->token) a->result = !queue_count && !quit_pending && !marked;
        else { a->result = W32_WAIT_FAILED; a->error = 1471; }
        return 0;
    case W32_MSG_POST:
        if (key_active || key_at) if (a->message.message >= 0x100 && a->message.message <= 0x107) {
            host_print("GAME post msg="); hex(a->message.message); host_print(" wp="); hex(a->message.wparam);
            host_print(" scan="); hex((a->message.lparam >> 16) & 0x1ff); host_print(" time="); host_number(a->message.time);
            host_print(" queued="); host_number(queue_count); host_print("\n");
        }
        if (queue_count == QUEUE) { a->error = 1816; return 0; }
        queue[queue_count] = a->message;
        queue_bits[queue_count++] = a->bits ? a->bits : W32_QS_POSTMESSAGE;
        changed |= a->bits ? a->bits : W32_QS_POSTMESSAGE;
        a->result = 1;
        return 0;
    case W32_MSG_QUIT:
        quit_pending = 1; quit_code = a->message.wparam; changed |= W32_QS_POSTMESSAGE; a->result = 1;
        return 0;
    case W32_MSG_SELECT:
        /* As the kernel (proc/win32/message.c): posted messages precede
         * hardware input, so a WM_CHAR posted by TranslateMessage is taken
         * before the key messages still queued behind it. */
        for (unsigned pass = 0; pass < 2; pass++)
        for (uint32_t k = 0; k < queue_count; k++)
            if (matches(a, k) && (!!(queue_bits[k] & (W32_QS_POSTMESSAGE | W32_QS_HOTKEY))) == (pass == 0)) {
                a->message = queue[k];
                a->bits = queue_bits[k];
                if (a->token & W32_SELECT_REMOVE) {
                    for (uint32_t j = k + 1; j < queue_count; j++) { queue[j - 1] = queue[j]; queue_bits[j - 1] = queue_bits[j]; }
                    queue_count--;
                }
                a->result = 1;
                return 0;
            }
        if (quit_pending && !(a->token & W32_SELECT_SENT_ONLY)) {
            a->message = (struct ciuki_w32_message){0, 0x12 /* WM_QUIT */, quit_code, 0, now_ms(), 0, 0};
            if (a->token & W32_SELECT_REMOVE)
                quit_pending = 0;
            a->result = 1;
        }
        return 0;
    case W32_MSG_GET: /* sent messages: no other thread sends here */
        return 0;
    case W32_MSG_STATUS: {
        uint32_t now = (queue_count ? W32_QS_POSTMESSAGE : 0) | marked;
        for (uint32_t k = 0; k < queue_count; k++) now |= queue_bits[k];
        a->result = now;
        a->message.wparam = changed & a->bits;
        changed &= ~a->bits;
        return 0;
    }
    case W32_MSG_MARK:
        marked = (marked & ~a->message.wparam) | (a->bits & a->message.wparam);
        a->result = 1;
        return 0;
    default:
        a->error = 1400; /* cross-thread sends: no other thread exists here */
        return 0;
    }
}

/* ---- entry points ------------------------------------------------------- */
void h1_game_init(int32_t (*kernel)(void *, uint32_t)) {
    kernel_call = kernel; enabled = 1; start_ms = now_ms();
    profile_start();
}
int h1_game_enabled(void) { return enabled; }
int h1_game_kernel(void *record, uint32_t size, int32_t *status) {
    struct ciuki_w32_header *h = record;
    if (!enabled || size < sizeof(*h))
        return 0;
    trace(h->operation << 16 | (size >= 20 ? ((const uint32_t *)record)[4] & 0xffff : 0));
    if (h->operation == W32_SERVICE && size == sizeof(struct ciuki_w32_service_args)) {
        struct ciuki_w32_service_args *a = record;
        if (a->action != W32_SVC_CONNECT || a->service != W32_SERVICE_DESKTOP)
            return 0;
        a->endpoint = DESKTOP_FD; a->result = 1; a->error = 0; *status = 0;
        return 1;
    }
    if (h->operation == W32_MESSAGE && size == sizeof(struct ciuki_w32_message_args)) {
        *status = message(record);
        return 1;
    }
    if (h->operation == W32_WAIT && size == sizeof(struct ciuki_w32_wait_args)) {
        struct ciuki_w32_wait_args *a = record;
        if (a->count)
            return 0; /* object handles: the fixture service */
        a->error = 0;
        if (queue_count || quit_pending || (marked & a->wake_mask)) { a->result = 0; *status = 0; return 1; }
        /* A fatal renderer error in id's Win32 implementation runs the
         * console's GetMessage loop. Act as the user closing that window;
         * never synthesize success or bypass the production window proc. */
        if (close_after_eio && lease_failures && !close_sent && focus_id && a->timeout) {
            event(DESK_CLOSE_REQUEST,0,0,focus_id,focus_generation);
            close_sent=1;
            host_print("GAME close after failed lease\n");
        }
        sleep_ms(a->timeout > 10 ? 10 : a->timeout);
        a->result = W32_WAIT_TIMEOUT;
        *status = 0;
        return 1;
    }
    if (h->operation == W32_DISPLAY && size == sizeof(struct ciuki_w32_display_args)) {
        struct ciuki_w32_display_args *a = record;
        a->result = 0; a->error = 0; *status = 0;
        if (a->token != lease_token || a->generation != lease_generation || !lease_w) {
            refused++; a->error = 1;
            return 1;
        }
        if (a->action == W32_DISPLAY_GAMMA)
            gamma_records++; /* the full-screen device gamma ramp (SetDeviceGammaRamp -> opengl32 present) */
        if (a->action == W32_DISPLAY_FRAME) {
            uint32_t pitch = (lease_w * (lease_bpp / 8) + 3) & ~3u;
            frame((const uint8_t *)(uintptr_t)a->address, lease_w, lease_h, pitch, lease_bpp, "lease");
            key_frame();
        }
        a->result = 1;
        return 1;
    }
    return 0;
}
int h1_game_gate(uint32_t call, uint32_t a, uint32_t b, uint32_t c, int32_t *out) {
    (void)c;
    if (!enabled)
        return 0;
    trace(call);
    switch (call) {
    case CIUKI_SYS_SLEEP_MS: sleep_ms(a); *out = 0; return 1;
    case CIUKI_SYS_CLOCK_GETTIME: {
        struct ciuki_timespec *t = (void *)(uintptr_t)b;
        struct { uint32_t sec, nsec; } now = {0, 0};
        host_syscall(265, a == 0 ? 0 : 1, (ULONG)(uintptr_t)&now, 0, 0, 0, 0);
        t->tv_sec = now.sec; t->tv_nsec = (int32_t)now.nsec; t->reserved = 0; *out = 0;
        return 1;
    }
    case CIUKI_SYS_DISPLAY: {
        if (a != CIUKI_DISPLAY_QUERY) { *out = -95; return 1; }
        struct ciuki_display_info *d = (void *)(uintptr_t)b;
        w32_fill(d, 0, sizeof(*d));
        d->version = CIUKI_DISPLAY_VERSION; d->size = sizeof(*d);
        d->width = 1024; d->height = 768; d->bpp = 32; d->pitch = 4096; d->vram_bytes = 16u << 20;
        static const uint16_t sizes[3][2] = {{640, 480}, {800, 600}, {1024, 768}};
        for (unsigned i = 0; i < 6; i++) {
            uint16_t bpp = i & 1 ? 32 : 16, w = sizes[i / 2][0], h = sizes[i / 2][1];
            d->modes[i] = (struct ciuki_display_mode){(uint16_t)(0x111 + i), w, h, bpp, (uint16_t)(w * bpp / 8), 1, i == 5};
        }
        d->mode_count = 6;
        *out = 0;
        return 1;
    }
    case CIUKI_SYS_SURFACE_CREATE:
        for (unsigned i = 0; i < SURFACES; i++)
            if (!surfaces[i].used) {
                if (!a || !b || a > 2048 || b > 2048) { *out = -22; return 1; }
                uint32_t bytes = (a * 4 * b + 4095u) & ~4095u;
                uint8_t *p = host.services.alloc(&host, bytes);
                if (!p) { *out = -12; return 1; }
                surfaces[i] = (typeof(surfaces[i])){p, a, b, a * 4, bytes, 1};
                *out = SURFACE_FD + (int32_t)i;
                return 1;
            }
        *out = -24;
        return 1;
    case CIUKI_SYS_SURFACE_INFO:
    case CIUKI_SYS_SURFACE_MAP: {
        uint32_t k = a - SURFACE_FD;
        if (k >= SURFACES || !surfaces[k].used) { *out = -9; return 1; }
        if (call == CIUKI_SYS_SURFACE_MAP) { *out = (int32_t)(uintptr_t)surfaces[k].pixels; return 1; }
        *(struct ciuki_surface_info *)(uintptr_t)b = (struct ciuki_surface_info){
            sizeof(struct ciuki_surface_info), surfaces[k].width, surfaces[k].height, surfaces[k].pitch,
            CIUKI_SURFACE_XRGB8888, surfaces[k].bytes};
        *out = 0;
        return 1;
    }
    case CIUKI_SYS_MUNMAP:
        for (unsigned i = 0; i < SURFACES; i++)
            if (surfaces[i].used && (uint32_t)(uintptr_t)surfaces[i].pixels == a) { *out = 0; return 1; }
        return 0;
    case CIUKI_SYS_CLOSE: {
        uint32_t k = a - SURFACE_FD;
        if (a == DESKTOP_FD) { *out = 0; return 1; }
        if (k < SURFACES && surfaces[k].used) {
            /* A window still names it until DESK_CLOSE; keep the pixels. */
            int named = 0;
            for (unsigned i = 0; i < 64; i++) named |= windows[i].id && windows[i].fd == a;
            if (!named) { host.services.free(&host, surfaces[k].pixels, surfaces[k].bytes); surfaces[k].used = 0; }
            *out = 0;
            return 1;
        }
        return 0;
    }
    case CIUKI_SYS_CHANNEL_SEND:
        if (a != DESKTOP_FD) return 0;
        *out = desktop_send((const struct ciuki_message *)(uintptr_t)b);
        return 1;
    case CIUKI_SYS_CHANNEL_RECV:
        if (a != DESKTOP_FD) return 0;
        if (in_head == in_tail) { *out = -11; return 1; }
        *(struct ciuki_message *)(uintptr_t)b = inbox[in_head++ % INBOX];
        *out = 1;
        return 1;
    case CIUKI_SYS_W32:
        if (!kernel_call) return 0;
        *out = kernel_call((void *)(uintptr_t)a, b);
        return 1;
    default:
        return 0;
    }
}
void h1_game_report(void) {
    if (!enabled)
        return;
    host_print("GAME summary frames="); host_number(frames);
    host_print(" presents="); host_number(presents);
    host_print(" desktop_requests="); host_number(requests);
    host_print(" refused_frames="); host_number(refused);
    host_print(" gamma_records="); host_number(gamma_records);
    host_print(" first_to_last_ms="); host_number(frames ? last_ms - first_ms : 0);
    host_print(" last_checksum="); hex(checksum);
    host_print(" lease="); host_number(lease_w); host_print("x"); host_number(lease_h); host_print("x"); host_number(lease_bpp);
    host_print("\n");
    (void)frame_sum;
    gl_report();
    profile_report();
}
