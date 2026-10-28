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
 * fx-q3vise: with "+set ciuki_installer <ms>" the same mode runs a wizard
 * installer (the Quake III demo's Installer VISE), pressing the keys of its
 * pages; launch requests (DESK_V3_LAUNCH) and window creations are printed
 * with their times.
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
static struct { uint32_t id, generation, fd, gdi_frames; int titled; } windows[64];
static uint32_t launches;
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
static uint32_t trace_ring[64], trace_count, start_ms, opcode_log, watchdog_ms = WATCHDOG_MS;
/* fx-hlrun harness options: "gdiframes=1" also writes sampled GDI window
 * frames as PPM files (q3-frames/gNNNNN.ppm, local only); "stacks=1" prints
 * the image addresses on the stack at long waits (as ciuki_installer_stacks). */
static int gdi_frame_files, stacks_every;
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
/* fx-hlrun: Win32 threads run as Linux threads of the adapter under one
 * FIFO (ticket) lock: one thread at a time executes PE and adapter code; a
 * thread gives the lock up only where a Windows thread would block or yield
 * (sleeps, waits, critical-section waits, message polls). See "threads". */
static volatile uint32_t lock_next, lock_serving, lock_waiters;
static int threads_on;
static void futex(volatile uint32_t *word, uint32_t op, uint32_t value) {
    host_syscall(240, (ULONG)(uintptr_t)word, op | 128 /* FUTEX_PRIVATE_FLAG */, value, 0, 0, 0);
}
static void lock_acquire(void) {
    uint32_t ticket = __atomic_fetch_add(&lock_next, 1, __ATOMIC_ACQ_REL);
    for (;;) {
        uint32_t serving = __atomic_load_n(&lock_serving, __ATOMIC_ACQUIRE);
        if (serving == ticket)
            return;
        __atomic_fetch_add(&lock_waiters, 1, __ATOMIC_ACQ_REL);
        if (__atomic_load_n(&lock_serving, __ATOMIC_ACQUIRE) == serving)
            futex(&lock_serving, 0 /* FUTEX_WAIT */, serving);
        __atomic_fetch_sub(&lock_waiters, 1, __ATOMIC_ACQ_REL);
    }
}
static void lock_release(void) {
    __atomic_fetch_add(&lock_serving, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&lock_waiters, __ATOMIC_ACQUIRE))
        futex(&lock_serving, 1 /* FUTEX_WAKE */, 0x7fffffff);
}
static void thread_checkpoint(void);
static uint32_t current_tid(void) { return (uint32_t)host_syscall(224, 0, 0, 0, 0, 0, 0); }
static void sleep_ms(uint32_t ms) {
    struct { uint32_t sec, nsec; } t = {ms / 1000u, (ms % 1000u) * 1000000u};
    if (threads_on)
        lock_release();
    host_syscall(162, (ULONG)(uintptr_t)&t, 0, 0, 0, 0, 0); /* nanosleep */
    if (threads_on) {
        lock_acquire();
        thread_checkpoint();
    }
}
/* a turn for every other runnable thread, as a Windows thread switch */
static void yield_turn(void) {
    if (!threads_on || !__atomic_load_n(&lock_waiters, __ATOMIC_ACQUIRE))
        return;
    lock_release();
    host_syscall(158, 0, 0, 0, 0, 0, 0); /* sched_yield */
    lock_acquire();
    thread_checkpoint();
}

static void trace(uint32_t v) {
    trace_ring[trace_count++ % 64] = v;
    if (!(trace_count & 1023))
        watchdog();
}
static void watchdog(void) {
    if (!start_ms || now_ms() - start_ms < watchdog_ms)
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

/* ---- import call counts (fx-hlrun) --------------------------------------
 * Test-only. With the harness option "apicount=1" every import address of
 * the EXE is redirected, before its entry point, to an 8-byte thunk (call
 * h1_api_count) in adapter memory that counts the call and continues to the
 * bound function with the caller's stack intact. The report lists the
 * imports by calls ("GAME api name=DLL!symbol calls=N"): the program's own
 * API use, without arguments or text. Calls through GetProcAddress pointers
 * and from DLLs are not counted. */
#define API_SLOTS 2048
#define API_NAME 56
#define API_DEPTH 4096
uint32_t h1_api_thunks, h1_api_counts[API_SLOTS], h1_api_originals[API_SLOTS], h1_api_track;
static uint32_t api_zero[API_SLOTS], api_depth, api_ret[API_DEPTH], api_loc[API_DEPTH], api_slot[API_DEPTH];
static char api_names[API_SLOTS][API_NAME];
static uint32_t api_slots;
static int api_count;
/* "apicount=2" also counts calls that returned 0 (FALSE/NULL), through a
 * shadow stack of return addresses: the caller's return address is replaced
 * by h1_api_ret, which restores it. A frame abandoned by an exception is
 * dropped at the next return above it (its slot address lies deeper). */
/* "apiargs=DLL!name,DLL!name": print the first eight argument words (and,
 * with apicount=2, the result) of up to 40 calls of each named import. */
static char api_args[256];
static int api_text, api_frame;
static uint32_t api_peek;
static uint16_t api_shown[API_SLOTS];
static uint8_t api_watch[API_SLOTS];
static uint32_t api_limit = 40;
static uint32_t api_watch_address, api_watch_value, api_watch_changes; /* "apiwatch=ADDRESS" (decimal) */
uint32_t __attribute__((cdecl, used)) h1_api_enter(uint32_t slot, uint32_t *ret_slot) {
    if (api_watch_address && api_watch_changes < 32) {
        uint32_t now = *(const uint32_t *)(uintptr_t)api_watch_address;
        if (now != api_watch_value) {
            api_watch_changes++;
            host_print("GAME apiwatch "); hex(api_watch_value); host_print(" -> "); hex(now);
            host_print(" before "); host_print(api_names[slot]); host_print(" ret="); hex(ret_slot[0]); host_print("\n");
            api_watch_value = now;
        }
    }
    if (api_watch[slot] && api_shown[slot] < api_limit) {
        api_shown[slot]++;
        host_print("GAME apicall "); host_print(api_names[slot]);
        for (unsigned i = 1; i <= 12; i++) { host_print(" "); hex(ret_slot[i]); }
        host_print(" ret="); hex(ret_slot[0]);
        if (api_frame) {
            /* "apiframe=1" (transient diagnosis): the caller's frame pointer
             * (h1_api_count keeps EBP) and the word at EBP-0x14. */
            const uint32_t *ebp = (const uint32_t *)(uintptr_t)*(const uint32_t *)__builtin_frame_address(0);
            host_print(" ebp="); hex((uint32_t)(uintptr_t)ebp);
            if (api_peek) { /* "apipeek=ADDRESS": a data word of the program (decimal address) */
                uint32_t g = *(const uint32_t *)(uintptr_t)api_peek;
                host_print(" peek="); hex(g);
                if (g > 0x10000u && g < 0x7ffe0000u) { host_print(" peek+c0="); hex(((const uint32_t *)(uintptr_t)g)[0xc0 / 4]); }
            }
            if ((uintptr_t)ebp > 0x10000u && (uintptr_t)ebp < 0x7ffe0000u) {
                host_print(" local14="); hex(ebp[-5]);
                const uint32_t *object = (const uint32_t *)(uintptr_t)ebp[-5];
                if ((uintptr_t)object > 0x10000u && (uintptr_t)object < 0x7ffe0000u && object[0] > 0x400000u && object[0] < 0x600000u) {
                    host_print(" vtable="); hex(object[0]); host_print(" slot56="); hex(((const uint32_t *)(uintptr_t)object[0])[56]);
                }
            }
        }
        if (!w32_strcmp(api_names[slot], "GDI32!SetDIBitsToDevice") && ret_slot[11] && ret_slot[11] < 0x7ffe0000u) {
            /* the BITMAPINFOHEADER (sizes, depth, compression) and a bits digest */
            const uint32_t *bi = (const uint32_t *)(uintptr_t)ret_slot[11];
            host_print(" bih="); hex(bi[0]); host_print(","); hex(bi[1]); host_print(","); hex(bi[2]);
            host_print(","); hex(bi[3]); host_print(","); hex(bi[4]);
            const uint8_t *bits = (const uint8_t *)(uintptr_t)ret_slot[10];
            uint32_t sum = 0, distinct = 0, first = bits ? bits[0] : 0;
            for (unsigned i = 0; bits && i < 75 * 23 * 4; i++) { sum += bits[i]; distinct += bits[i] != first; }
            host_print(" bitsum="); hex(sum); host_print(" distinct="); hex(distinct);
        }
        if (!w32_strcmp(api_names[slot], "USER32!DispatchMessageA") && ret_slot[1] && ret_slot[1] < 0x7ffe0000u) {
            const uint32_t *msg = (const uint32_t *)(uintptr_t)ret_slot[1];
            host_print(" hwnd="); hex(msg[0]); host_print(" message="); hex(msg[1]);
        }
        /* A font request names a typeface (system font names only). */
        const char *n = api_names[slot];
        unsigned text_arg = !w32_strcmp(n, "USER32!SetWindowTextA") || !w32_strcmp(n, "ADVAPI32!RegQueryValueExA") ||
                            !w32_strcmp(n, "ADVAPI32!RegSetValueExA") || !w32_strcmp(n, "ADVAPI32!RegOpenKeyExA") ||
                            !w32_strcmp(n, "ADVAPI32!RegCreateKeyExA") || !w32_strcmp(n, "KERNEL32!CreateFileA") ||
                            !w32_strcmp(n, "KERNEL32!LoadLibraryA") ? 2 : 0;
        if (!w32_strcmp(n, "KERNEL32!CreateFileA") || !w32_strcmp(n, "KERNEL32!LoadLibraryA") || !w32_strcmp(n, "WINMM!mciSendStringA") ||
            !w32_strcmp(n, "KERNEL32!GetProcAddress"))
            text_arg = !w32_strcmp(n, "KERNEL32!GetProcAddress") ? 2 : 1;
        if (api_text && text_arg && ret_slot[text_arg] > 0xffff && ret_slot[text_arg] < 0x7ffe0000u) {
            /* "apitext=1" (transient diagnosis only, never in a test): the
             * string argument (window text, registry or file name, export
             * name), cut to 60 bytes. */
            char t[61]; const char *f = (const char *)(uintptr_t)ret_slot[text_arg]; unsigned k = 0;
            for (; k < 60 && f[k]; k++) t[k] = f[k] >= ' ' && f[k] < 127 ? f[k] : '?';
            t[k] = 0;
            host_print(" text="); host_print(t);
        }
        if (!w32_strcmp(n, "GDI32!CreateFontA") && ret_slot[14] && ret_slot[14] < 0x7ffe0000u) {
            char face[32]; const char *f = (const char *)(uintptr_t)ret_slot[14]; unsigned k = 0;
            for (; k < 31 && f[k]; k++) face[k] = f[k];
            face[k] = 0;
            host_print(" weight="); host_number(ret_slot[5]); host_print(" face="); host_print(face);
        }
        host_print("\n");
    }
    /* One shadow stack: only the main thread's results are tracked (a
     * worker thread's calls are counted, fx-hlrun threads). */
    if (api_depth < API_DEPTH && NtCurrentTeb() == &host.teb) {
        api_ret[api_depth] = *ret_slot; api_loc[api_depth] = (uint32_t)(uintptr_t)ret_slot; api_slot[api_depth] = slot;
        api_depth++;
        extern void h1_api_ret(void);
        *ret_slot = (uint32_t)(uintptr_t)h1_api_ret;
    }
    return h1_api_originals[slot];
}
uint32_t __attribute__((cdecl, used)) h1_api_leave(uint32_t esp, uint32_t result) {
    /* esp: the stack pointer after the callee's return (its own arguments
     * popped); the call's return slot was at or below esp - 4. */
    while (api_depth > 1 && api_loc[api_depth - 2] <= esp - 4)
        api_depth--;
    if (!api_depth) {
        host_print("GAME api shadow stack underflow\n");
        host_exit(125);
    }
    api_depth--;
    if (!result)
        api_zero[api_slot[api_depth]]++;
    if (api_watch[api_slot[api_depth]] && api_shown[api_slot[api_depth]] <= api_limit) {
        host_print("GAME apiresult "); host_print(api_names[api_slot[api_depth]]); host_print(" = "); hex(result); host_print("\n");
    }
    return api_ret[api_depth];
}
extern void h1_api_count(void);
__asm__(".text\n.globl h1_api_count\n.type h1_api_count,@function\n"
        "h1_api_count:\n"
        "  pushl %ecx\n"
        "  pushl %edx\n"
        "  movl 8(%esp),%eax\n"            /* thunk + 5 */
        "  subl h1_api_thunks,%eax\n"
        "  subl $5,%eax\n"
        "  shrl $3,%eax\n"                 /* slot */
        "  incl h1_api_counts(,%eax,4)\n"
        "  cmpl $0,h1_api_track\n"
        "  jne 1f\n"
        "  movl h1_api_originals(,%eax,4),%eax\n"
        "  jmp 2f\n"
        "1:\n"
        "  leal 12(%esp),%ecx\n"           /* the caller's return address slot */
        "  pushl %ecx\n"
        "  pushl %eax\n"
        "  call h1_api_enter\n"
        "  addl $8,%esp\n"
        "2:\n"
        "  movl %eax,8(%esp)\n"            /* return into the bound function */
        "  popl %edx\n"
        "  popl %ecx\n"
        "  ret\n"
        ".globl h1_api_ret\n.type h1_api_ret,@function\n"
        "h1_api_ret:\n"
        "  subl $4,%esp\n"                 /* the restored return address */
        "  pushl %eax\n"
        "  pushl %ecx\n"
        "  pushl %edx\n"
        "  pushl %eax\n"
        "  leal 20(%esp),%ecx\n"           /* esp as the callee left it */
        "  pushl %ecx\n"
        "  call h1_api_leave\n"
        "  addl $8,%esp\n"
        "  movl %eax,12(%esp)\n"
        "  popl %edx\n"
        "  popl %ecx\n"
        "  popl %eax\n"
        "  ret\n");
static int gl_protect(uint32_t address, uint32_t bytes);
void h1_game_exe(void *base) {
    if (!enabled || !api_count)
        return;
    const uint8_t *b = base;
    uint32_t pe = get32(b + 0x3c), imports = get32(b + pe + 0x80);
    uint8_t *thunks = host.services.alloc(&host, API_SLOTS * 8);
    if (!imports || !thunks || !gl_protect((uint32_t)(uintptr_t)thunks, API_SLOTS * 8)) {
        host_print("GAME api-count unavailable\n");
        return;
    }
    h1_api_thunks = (uint32_t)(uintptr_t)thunks;
    for (const uint8_t *d = b + imports; get32(d + 16) && api_slots < API_SLOTS; d += 20) {
        const char *dll = (const char *)(b + get32(d + 12));
        uint32_t names = get32(d) ? get32(d) : get32(d + 16);
        uint8_t *iat = (uint8_t *)(uintptr_t)(b + get32(d + 16));
        for (uint32_t i = 0; get32(b + names + i * 4) && api_slots < API_SLOTS; i++) {
            uint32_t entry = get32(b + names + i * 4), slot = api_slots++;
            char *n = api_names[slot];
            unsigned k = 0;
            for (; dll[k] && dll[k] != '.' && k < 16; k++) n[k] = dll[k];
            n[k++] = '!';
            if (entry & 0x80000000u) {
                uint32_t ordinal = entry & 0xffff, digits = 1;
                for (uint32_t v = ordinal; v >= 10; v /= 10) digits++;
                n[k++] = '#';
                for (uint32_t j = digits; j--; ordinal /= 10) n[k + j] = (char)('0' + ordinal % 10);
                k += digits;
            } else
                for (const char *s = (const char *)(b + entry + 2); *s && k < API_NAME - 1; s++) n[k++] = *s;
            n[k] = 0;
            uint8_t *t = thunks + slot * 8;
            t[0] = 0xe8;
            put32(t + 1, (uint32_t)(uintptr_t)h1_api_count - (uint32_t)(uintptr_t)(t + 5));
            t[5] = t[6] = t[7] = 0x90;
            if (!gl_protect((uint32_t)(uintptr_t)(iat + i * 4), 4))
                continue;
            for (const char *w = api_args; *w; ) {
                unsigned m = 0;
                while (w[m] && w[m] != ',' && n[m] == w[m]) m++;
                if ((!w[m] || w[m] == ',') && !n[m]) api_watch[slot] = 1;
                while (*w && *w != ',') w++;
                if (*w == ',') w++;
            }
            h1_api_originals[slot] = get32(iat + i * 4);
            put32(iat + i * 4, (uint32_t)(uintptr_t)t);
        }
    }
    host_print("GAME api-count hooked="); host_number(api_slots); host_print("\n");
}
static void api_report(void) {
    for (unsigned rank = 0; rank < 120; rank++) {
        uint32_t best = 0, at = 0;
        for (uint32_t i = 0; i < api_slots; i++)
            if (h1_api_counts[i] > best) { best = h1_api_counts[i]; at = i; }
        if (!best)
            break;
        host_print("GAME api name="); host_print(api_names[at]); host_print(" calls="); host_number(best);
        if (h1_api_track) { host_print(" zero="); host_number(api_zero[at]); }
        host_print("\n");
        h1_api_counts[at] = 0;
    }
}

/* ---- frames ------------------------------------------------------------ */
/* fx-hlrun: with "gdiframes=1" every file name also carries the adapter's
 * process id (q3-frames/gNNNNN-PID.ppm): an installer's processes share the
 * directory and number their frames from 1 each. */
static uint32_t frame_pid;
static void dump_named(const uint8_t *pixels, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp, char letter, uint32_t n) {
    char name[48] = "q3-frames/f00000.ppm";
    name[10] = letter;
    for (int i = 15; i >= 11; i--, n /= 10) name[i] = (char)('0' + n % 10);
    if (gdi_frame_files && frame_pid) {
        unsigned at = 16, d = 0;
        char digits[10];
        for (uint32_t v = frame_pid; v; v /= 10) digits[d++] = (char)('0' + v % 10);
        name[at++] = '-';
        while (d) name[at++] = digits[--d];
        w32_copy(name + at, ".ppm", 5);
    }
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
static void dump(const uint8_t *pixels, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp) {
    dump_named(pixels, w, h, pitch, bpp, 'f', frames);
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
/* fx-q3vise: a wizard-style installer (VISE, InstallShield) run in game mode.
 * "+set ciuki_installer <idle_ms>" at the end of the command line (cut off
 * before the program sees it) presses Enter on the focused window whenever
 * nothing has been presented for idle_ms, idle_ms passed since the last
 * press and the program has read the previous key: a static wizard page
 * takes its default button (Next, Install, Close), a progress page keeps
 * repainting and is left alone. "+set ciuki_installer_max N" bounds the
 * presses (default 24). Every press and window creation is printed with its
 * time in ms since the start, and the focused window is written to
 * q3-frames/eNNNNN.ppm before each press when that directory exists. */
static uint32_t installer_idle_ms, installer_max = 24, installer_presses, installer_key_ms, installer_present_ms;
static int installer_up_pending;
/* "+set ciuki_installer_keys <letters>": the key of each press in order (a
 * lower-case letter is that key, a dialog mnemonic; anything else and every
 * press past the end is Enter). */
static char installer_keys[64];
static uint8_t installer_scan;
/* "+set ciuki_installer_untitled 1": press only on windows created without
 * a title (VISE wizard pages; its progress window and message boxes are
 * created with one). */
static int installer_untitled;
static void key_event(uint32_t type, uint32_t code, uint32_t value, uint32_t ms);
/* "+set ciuki_installer_stacks 1": a wait or sleep of a second or more and
 * every 100000th kernel record print the words of the caller's stack that
 * point into an executable image (the EXE at 0x400000, an installer DLL at
 * 0x10000000, the builtins above 0x70000000), mapped offline to modules
 * and exports. This found the VISE engine's FindWindow loop (fx-q3vise). */
static unsigned installer_waits;
static int installer_stacks;
static void installer_stack(const char *what, uint32_t a, uint32_t b) {
    if (!installer_stacks || installer_waits >= 120)
        return;
    installer_waits++;
    host_print("GAME installer "); host_print(what); host_print(" a="); host_number(a); host_print(" b="); host_number(b);
    host_print(" ms="); host_number(now_ms() - start_ms);
    if (threads_on) { host_print(" tid="); host_number(current_tid()); }
    host_print(" stack");
    const uint32_t *sp = __builtin_frame_address(0);
    unsigned shown = 0;
    /* fx-hlrun: never past the top of this thread's stack */
    const uint32_t *end = (const uint32_t *)NtCurrentTeb()->Tib.StackBase;
    for (unsigned i = 0; i < 2048 && shown < 40 && sp + i < end; i++) {
        uint32_t v = sp[i];
        if ((v >= 0x400000u && v < 0x500000u) || (v >= 0x10000000u && v < 0x10080000u) || (v >= 0x70000000u && v < 0x7ffe0000u)) {
            host_print(" "); hex(v); shown++;
        }
    }
    host_print("\n");
}
static void installer_tick(void) {
    if (!installer_idle_ms || !focus_id)
        return;
    uint32_t t = now_ms();
    if (installer_up_pending) {
        if (t - installer_key_ms < 40)
            return;
        key_event(CIUKI_INPUT_KEY, installer_scan, 0, t - start_ms);
        installer_up_pending = 0;
        return;
    }
    if (in_head != in_tail)
        return; /* the program has not read the previous key yet */
    if (installer_untitled)
        for (unsigned i = 0; i < 64; i++)
            if (windows[i].id == focus_id && windows[i].generation == focus_generation && windows[i].titled)
                return; /* a progress window or a message box: never its default button */
    if (installer_presses >= installer_max || !installer_present_ms ||
        t - installer_present_ms < installer_idle_ms || t - installer_key_ms < installer_idle_ms)
        return;
    installer_presses++; installer_key_ms = t; installer_up_pending = 1;
    for (unsigned i = 0; i < 64; i++)
        if (windows[i].id == focus_id && windows[i].generation == focus_generation) {
            uint32_t k = windows[i].fd - SURFACE_FD;
            if (k < SURFACES && surfaces[k].used)
                dump_named(surfaces[k].pixels, surfaces[k].width, surfaces[k].height, surfaces[k].pitch, 32, 'e', installer_presses);
        }
    host_print("GAME installer enter n="); host_number(installer_presses); host_print(" window="); host_number(focus_id);
    host_print(" idle_ms="); host_number(t - installer_present_ms); host_print(" ms="); host_number(t - start_ms); host_print("\n");
    char c = installer_presses <= sizeof(installer_keys) ? installer_keys[installer_presses - 1] : 0;
    installer_scan = c >= 'a' && c <= 'z' ? key_scan[(uint8_t)c] : 0x1c;
    key_event(CIUKI_INPUT_KEY, installer_scan, 1, t - start_ms);
    key_event(CIUKI_INPUT_TEXT, installer_scan == 0x1c ? '\r' : (uint8_t)c, 0, t - start_ms);
}
void h1_game_script(const char *command_line) {
    lease_eio = after(command_line, "+set ciuki_lease_eio 1") != 0;
    close_after_eio = after(command_line, "+set ciuki_close_after_eio 1") != 0;
    const char *installer = after(command_line, "+set ciuki_installer ");
    if (installer) {
        for (; *installer >= '0' && *installer <= '9'; installer++) installer_idle_ms = installer_idle_ms * 10 + (uint32_t)(*installer - '0');
        const char *max = after(command_line, "+set ciuki_installer_max ");
        if (max) { installer_max = 0; for (; *max >= '0' && *max <= '9'; max++) installer_max = installer_max * 10 + (uint32_t)(*max - '0'); }
        installer_untitled = after(command_line, "+set ciuki_installer_untitled 1") != 0;
        installer_stacks = after(command_line, "+set ciuki_installer_stacks 1") != 0;
        const char *keys = after(command_line, "+set ciuki_installer_keys ");
        for (unsigned k = 0; keys && k < sizeof(installer_keys) - 1 && keys[k] > ' '; k++) installer_keys[k] = keys[k];
        opcode_log = 0; /* the window log stays open for the whole install (see desktop_send) */
        host_print("GAME installer idle_ms="); host_number(installer_idle_ms); host_print(" max="); host_number(installer_max); host_print("\n");
        /* The program's command line ends before the first installer option. */
        char *cut = (char *)after(command_line, " +set ciuki_installer");
        if (cut) cut[-21] = 0;
    }
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
static int quiet_keys; /* fx-hlrun: secret text (a CD key) is typed without key records */
static void key_event(uint32_t type, uint32_t code, uint32_t value, uint32_t ms) {
    struct ciuki_message *m = event(DESK_KEY, 40, 0, focus_id, focus_generation);
    if (!quiet_keys) {
        host_print("GAME key frame="); host_number(frames); host_print(" type="); host_number(type); host_print(" code="); hex(code);
        host_print(" value="); host_number(value); host_print(" ms="); host_number(ms); host_print(m ? "\n" : " DROPPED\n");
    }
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
/* fx-is5boot: the game-exec harness option "defaults=IDLE_MS,MAX" is the
 * guest operator's "defaults" action (scripts/win32/operator_program.py):
 * once no window or file activity happened for IDLE_MS, Enter (key down,
 * TEXT CR, key up) goes to the focused window, at most MAX times, so each
 * dialog takes its default button. It is a separate adapter argument,
 * never part of the program's command line. */
static uint32_t defaults_idle, defaults_max, defaults_sent, last_activity;
static void click_options(const char *text);
/* fx-hlrun: "secret=PATH" reads up to 47 characters from a local file (the
 * owner's CD key, supplied by the test from the environment and removed
 * after the run); the answer '*' types them and Enter. Neither the text nor
 * its key codes are printed. */
static char secret[48];
/* "answers=..y": the Nth default action types the Nth character instead of
 * Enter ('.' keeps Enter): a letter is a dialog mnemonic (&Yes, &No). */
static char answers[32];
/* ---- CD drive (fx-hlrun) ------------------------------------------------
 * Test-only. "cdtoc=FIRST,LAST,LEADOUT,LBA:CTRL,..." (the TOC the test read
 * from the host's drive) makes the adapter answer the CiukiOS /dev/media CD
 * calls that winmm's MCI cdaudio driver (sdk/win32/audio/mcicd.c) issues by
 * int 0x80, as the kernel does for an ATAPI drive (src/kernel/include/ciuki
 * abi.h MEDIA_CD_*): slot 0 holds a ready disc mounted as D:, its TOC, and
 * the analog Audio Play path (no CD-DA reads), whose position advances in
 * real time and completes at the end. Without the option the calls keep
 * failing with ENOSYS, as before. */
#define MEDIA_FD 1900
static struct ciuki_media_cd_toc cd_toc;
static int cd_present, cd_open;
static uint32_t cd_play_start, cd_play_end, cd_play_ms, cd_paused_at, cd_audio = CIUKI_CD_AUDIO_NONE;
static uint32_t cd_lba_now(void) {
    if (cd_audio == CIUKI_CD_AUDIO_PAUSED)
        return cd_paused_at;
    if (cd_audio != CIUKI_CD_AUDIO_PLAYING)
        return cd_play_start;
    uint32_t lba = cd_play_start + (now_ms() - cd_play_ms) * CIUKI_CD_FRAMES_PER_SECOND / 1000;
    if (lba >= cd_play_end) {
        cd_audio = CIUKI_CD_AUDIO_COMPLETED;
        lba = cd_play_end;
    }
    return lba;
}
static void cd_options(const char *text) {
    const char *v = after(text, "cdtoc=");
    if (!v)
        return;
    uint32_t n[3 + 2 * CIUKI_CD_TRACKS_MAX], count = 0;
    for (; count < sizeof(n) / sizeof(n[0]); v++) {
        uint32_t x = 0;
        if (*v < '0' || *v > '9')
            break;
        for (; *v >= '0' && *v <= '9'; v++) x = x * 10 + (uint32_t)(*v - '0');
        n[count++] = x;
        if (*v != ',' && *v != ':')
            break;
    }
    if (count < 5 || n[0] < 1 || n[1] > 99 || n[1] < n[0] || count != 3 + 2 * (n[1] - n[0] + 1))
        return;
    cd_toc = (struct ciuki_media_cd_toc){.version = CIUKI_MEDIA_CD_VERSION, .size = sizeof(cd_toc), .generation = 1,
                                          .first = n[0], .last = n[1], .count = n[1] - n[0] + 1, .leadout_lba = n[2]};
    for (uint32_t i = 0; i < cd_toc.count; i++)
        cd_toc.tracks[i] = (struct ciuki_media_cd_track){(uint8_t)(n[0] + i), (uint8_t)n[4 + 2 * i], 1, 0, n[3 + 2 * i]};
    cd_present = 1;
    host_print("GAME cd tracks="); host_number(cd_toc.count); host_print("\n");
}
static int32_t cd_control(uint32_t op, void *p, uint32_t n) {
    uint32_t *h = p;
    if (!p || n < 12 || h[0] != CIUKI_MEDIA_CD_VERSION)
        return -22;
    if (h[2] != 0)
        return op == MEDIA_CD_STATE && n == sizeof(struct ciuki_media_cd_state) ? (w32_fill(h + 3, 0, n - 12), 0) : -6;
    switch (op) {
    case MEDIA_CD_STATE: {
        struct ciuki_media_cd_state *s = p;
        if (n != sizeof(*s)) return -22;
        s->present = 1; s->state = CIUKI_CD_READY; s->generation = 1; s->audio_ready = 0; s->mounted = 1; s->drive = 3;
        return 0;
    }
    case MEDIA_CD_INFO: {
        struct ciuki_media_cd_info *i = p;
        if (n != sizeof(*i)) return -22;
        i->generation = 1; i->capabilities = CIUKI_CD_CAP_AUDIO_PLAY | CIUKI_CD_CAP_EJECT | CIUKI_CD_CAP_REPORTED;
        i->volume_levels = 256; i->packet_bytes = 12; i->max_read_sectors = CIUKI_CD_READ_MAX_SECTORS;
        w32_copy(i->model, "CIUKI H1 CD-ROM", 16);
        return 0;
    }
    case MEDIA_CD_TOC:
        if (n != sizeof(cd_toc)) return -22;
        w32_copy((uint8_t *)p + 12, (const uint8_t *)&cd_toc + 12, sizeof(cd_toc) - 12);
        return 0;
    case MEDIA_CD_PLAY: {
        struct ciuki_media_cd_play *r = p;
        if (n != sizeof(*r) || r->start_lba >= r->end_lba || r->end_lba > cd_toc.leadout_lba) return -22;
        cd_play_start = r->start_lba; cd_play_end = r->end_lba; cd_play_ms = now_ms(); cd_audio = CIUKI_CD_AUDIO_PLAYING;
        host_print("GAME cd play from="); host_number(r->start_lba); host_print(" to="); host_number(r->end_lba); host_print("\n");
        return 0;
    }
    case MEDIA_CD_PAUSE: {
        struct ciuki_media_cd_pause *r = p;
        if (n != sizeof(*r)) return -22;
        uint32_t at = cd_lba_now();
        if (r->action == CIUKI_CD_ACTION_PAUSE && cd_audio == CIUKI_CD_AUDIO_PLAYING) { cd_paused_at = at; cd_audio = CIUKI_CD_AUDIO_PAUSED; }
        else if (r->action == CIUKI_CD_ACTION_RESUME && cd_audio == CIUKI_CD_AUDIO_PAUSED) {
            cd_play_start = cd_paused_at; cd_play_ms = now_ms(); cd_audio = CIUKI_CD_AUDIO_PLAYING;
        } else if (r->action == CIUKI_CD_ACTION_STOP) { cd_play_start = at; cd_audio = CIUKI_CD_AUDIO_NONE; }
        return 0;
    }
    case MEDIA_CD_POSITION: {
        struct ciuki_media_cd_position *r = p;
        if (n != sizeof(*r)) return -22;
        uint32_t lba = cd_lba_now(), track = 0;
        for (uint32_t i = 0; i < cd_toc.count; i++)
            if (cd_toc.tracks[i].start_lba <= lba) track = i;
        r->audio_status = cd_audio; r->track = cd_toc.tracks[track].number; r->index = 1;
        r->absolute_lba = lba; r->relative_lba = lba - cd_toc.tracks[track].start_lba;
        return 0;
    }
    case MEDIA_CD_TRAY:
        return 0;
    default:
        return -95; /* MEDIA_CD_READ_AUDIO: the drive reports no CD-DA reads */
    }
}
int h1_game_syscall(uint32_t call, uint32_t a, uint32_t b, uint32_t c, uint32_t d, int32_t *out) {
    (void)d;
    if (!enabled || !cd_present)
        return 0;
    if (call == CIUKI_SYS_OPEN && a) {
        const char *path = (const char *)(uintptr_t)a, *want = "/dev/media";
        unsigned k = 0;
        while (want[k] && path[k] == want[k]) k++;
        if (want[k] || path[k])
            return 0;
        (void)b;
        cd_open++;
        *out = MEDIA_FD;
        return 1;
    }
    if (call == CIUKI_SYS_CLOSE && a == MEDIA_FD && cd_open) {
        cd_open--;
        *out = 0;
        return 1;
    }
    if (call == CIUKI_SYS_DEVCTL && a == MEDIA_FD && cd_open) {
        *out = cd_control(b, (void *)(uintptr_t)c, d);
        return 1;
    }
    return 0;
}
void h1_game_options(const char *text) {
    cd_options(text);
    click_options(text);
    const char *w = after(text, "watchdog=");
    if (w) {
        watchdog_ms = 0;
        for (; *w >= '0' && *w <= '9'; w++) watchdog_ms = watchdog_ms * 10 + (uint32_t)(*w - '0');
    }
    gdi_frame_files = after(text, "gdiframes=1") != 0;
    api_count = after(text, "apicount=1") || after(text, "apicount=2");
    h1_api_track = after(text, "apicount=2") != 0;
    api_text = after(text, "apitext=1") != 0;
    api_frame = after(text, "apiframe=1") != 0;
    const char *watched = after(text, "apiwatch=");
    for (; watched && *watched >= '0' && *watched <= '9'; watched++) api_watch_address = api_watch_address * 10 + (uint32_t)(*watched - '0');
    const char *peek = after(text, "apipeek=");
    for (; peek && *peek >= '0' && *peek <= '9'; peek++) api_peek = api_peek * 10 + (uint32_t)(*peek - '0');
    const char *limit = after(text, "apilimit=");
    if (limit) { api_limit = 0; for (; *limit >= '0' && *limit <= '9'; limit++) api_limit = api_limit * 10 + (uint32_t)(*limit - '0'); }
    if (api_limit > 60000) api_limit = 60000;
    const char *watch = after(text, "apiargs=");
    for (unsigned k = 0; watch && k < sizeof(api_args) - 1 && watch[k] > ' '; k++) api_args[k] = watch[k];
    if (after(text, "stacks=1"))
        installer_stacks = 1;
    if (after(text, "stacks=all"))
        installer_stacks = stacks_every = 1; /* every message wait, not only long ones */
    const char *v = after(text, "defaults=");
    if (!v)
        return;
    uint32_t idle = 0, max = 0;
    while (*v >= '0' && *v <= '9')
        idle = idle * 10 + (uint32_t)(*v++ - '0');
    if (*v == ',')
        for (v++; *v >= '0' && *v <= '9'; v++)
            max = max * 10 + (uint32_t)(*v - '0');
    defaults_idle = idle; defaults_max = max;
    const char *a = after(text, "answers=");
    const char *path = after(text, "secret=");
    if (path) {
        char name[256]; unsigned k = 0;
        for (; k < sizeof(name) - 1 && path[k] > ' '; k++) name[k] = path[k];
        name[k] = 0;
        LONG fd = host_syscall(5, (ULONG)(uintptr_t)name, 0, 0, 0, 0, 0); /* O_RDONLY */
        if (fd >= 0) {
            LONG got = host_syscall(3, (ULONG)fd, (ULONG)(uintptr_t)secret, sizeof(secret) - 1, 0, 0, 0);
            host_syscall(6, (ULONG)fd, 0, 0, 0, 0, 0);
            unsigned end = got > 0 ? (unsigned)got : 0;
            secret[end] = 0;
            for (unsigned i = 0; i < end; i++)
                if (secret[i] == '\n' || secret[i] == '\r') { secret[i] = 0; break; }
        }
        host_print("GAME secret loaded="); host_number(secret[0] != 0); host_print("\n");
    }
    for (unsigned i = 0; a && i < sizeof(answers) - 1 && ((a[i] >= 'a' && a[i] <= 'z') || a[i] == '.' || a[i] == '!' || a[i] == '*'); i++)
        answers[i] = a[i];
    host_print("GAME defaults idle_ms="); host_number(idle); host_print(" max="); host_number(max);
    host_print(" answers="); host_print(answers); host_print("\n");
}
/* fx-hlis5: "click=X,Y,W,H" moves the pointer to (X,Y) of the focused
 * window and clicks the left button once, when that window's surface is
 * WxH and nothing happened for 3 s: the Half-Life disc menu (AUTORUN.EXE)
 * draws its items as text and starts the setup on a click, as the user
 * does. Every child adapter gets the same options; the size keeps the
 * click on the menu window. */
static uint32_t click_x, click_y, click_w, click_h, click_done;
static void click_options(const char *text) {
    const char *v = after(text, "click=");
    uint32_t n[4] = {0, 0, 0, 0};
    for (unsigned k = 0; v && k < 4; k++) {
        for (; *v >= '0' && *v <= '9'; v++) n[k] = n[k] * 10 + (uint32_t)(*v - '0');
        if (*v == ',') v++;
        else if (k < 3) return;
    }
    if (!v || !n[2] || !n[3]) return;
    click_x = n[0]; click_y = n[1]; click_w = n[2]; click_h = n[3];
    host_print("GAME click x="); host_number(click_x); host_print(" y="); host_number(click_y);
    host_print(" window="); host_number(click_w); host_print("x"); host_number(click_h); host_print("\n");
}
static void pointer_event(uint32_t type, int32_t code, int32_t value, int32_t value2, uint32_t ms) {
    struct ciuki_message *m = event(type == CIUKI_INPUT_MOTION ? DESK_MOTION : DESK_BUTTON, 40, 0, focus_id, focus_generation);
    if (!m) return;
    uint8_t *e = m->data + 16;
    put32(e + 12, type); put32(e + 16, ms * 1000000u); put32(e + 20, 0);
    put32(e + 24, (uint32_t)code); put32(e + 28, (uint32_t)value); put32(e + 32, (uint32_t)value2);
}
static void click_tick(void) {
    if (!click_w || click_done || !focus_id || INBOX - (in_tail - in_head) < 3)
        return;
    uint32_t t = now_ms();
    if (t - last_activity < 3000)
        return;
    for (unsigned i = 0; i < 64; i++) {
        if (windows[i].id != focus_id || windows[i].generation != focus_generation)
            continue;
        uint32_t k = windows[i].fd - SURFACE_FD;
        if (k >= SURFACES || !surfaces[k].used || surfaces[k].width != click_w || surfaces[k].height != click_h)
            return;
        click_done = 1;
        last_activity = t;
        host_print("GAME click window="); host_number(focus_id); host_print(" ms="); host_number(t - start_ms); host_print("\n");
        pointer_event(CIUKI_INPUT_MOTION, 0, (int32_t)click_x, (int32_t)click_y, t - start_ms);
        pointer_event(CIUKI_INPUT_BUTTON, 1, 1, 0, t - start_ms + 10);
        pointer_event(CIUKI_INPUT_BUTTON, 1, 0, 0, t - start_ms + 90);
        return;
    }
}
static void default_key(void) {
    if (defaults_sent >= defaults_max || !focus_id || INBOX - (in_tail - in_head) < 3)
        return;
    uint32_t t = now_ms();
    if (t - last_activity < defaults_idle)
        return;
    int open = 0;
    for (unsigned i = 0; i < 64; i++)
        open |= windows[i].id == focus_id && windows[i].generation == focus_generation;
    if (!open)
        return;
    char c = defaults_sent < sizeof(answers) ? answers[defaults_sent] : 0;
    if (c == '*' && INBOX - (in_tail - in_head) < 3 * sizeof(secret) + 3)
        return;
    defaults_sent++;
    last_activity = t;
    host_print("GAME default n="); host_number(defaults_sent); host_print(" window="); host_number(focus_id);
    host_print(" generation="); host_number(focus_generation); host_print("\n");
    /* fx-hlrun: with "gdiframes=1", every open window as it stood before
     * the press (q3-frames/eNNWW: press NN, window slot WW). */
    for (unsigned i = 0; gdi_frame_files && i < 64; i++)
        if (windows[i].id) {
            uint32_t k = windows[i].fd - SURFACE_FD;
            if (k < SURFACES && surfaces[k].used)
                dump_named(surfaces[k].pixels, surfaces[k].width, surfaces[k].height, surfaces[k].pitch, 32, 'e',
                           defaults_sent * 100 + i);
        }
    if (c == '*') {
        quiet_keys = 1;
        unsigned n = 0;
        for (; secret[n]; n++) {
            char k = secret[n], lower = k >= 'A' && k <= 'Z' ? (char)(k + 32) : k;
            uint8_t code = (uint8_t)lower < 128 ? key_scan[(uint8_t)lower] : 0;
            key_event(CIUKI_INPUT_KEY, code, 1, t - start_ms);
            key_event(CIUKI_INPUT_TEXT, (uint8_t)k, 0, t - start_ms);
            key_event(CIUKI_INPUT_KEY, code, 0, t - start_ms);
        }
        quiet_keys = 0;
        host_print("GAME secret typed characters="); host_number(n); host_print("\n");
        c = '.';
    }
    /* fx-hlrun: '!' is Escape (IDCANCEL of a dialog). */
    uint8_t scan = c == '!' ? 0x01 : c && c != '.' ? key_scan[(uint8_t)c] : 0x1c;
    key_event(CIUKI_INPUT_KEY, scan, 1, t - start_ms);
    key_event(CIUKI_INPUT_TEXT, c == '!' ? 0x1b : c && c != '.' ? (uint8_t)c : '\r', 0, t - start_ms);
    key_event(CIUKI_INPUT_KEY, scan, 0, t - start_ms + 40);
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
        /* fx-hlrun: the second word says whether the window taking the
         * focus belongs to the same process (apps/desktop/input.c
         * desk_focus); here it always does. USER keeps a full-screen lease
         * across a focus change inside the process. */
        if (off) { put32(off->data + 16, 0); put32(off->data + 20, 1); }
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
    /* Window activity: a blinking caret's DAMAGE and the input-settings
     * poll of an idle GUI are not. fx-hlrun: neither are the cursor shape,
     * the pointer query and a renewed activation of the focused window,
     * which Half-Life's menu repeats while it waits for input. */
    int idle_op = op == DESK_SET_CURSOR || op == DESK_V3_CURSOR_BITMAP || op == DESK_V3_POINTER ||
                  (op == DESK_V3_ACTIVATE && id == focus_id && generation == focus_generation);
    if (op == DESK_V3_PLACE && m->length >= DESK_V3_HEADER + 28) {
        /* a placement that repeats the window's last one changes nothing */
        static struct { uint32_t id, generation; uint8_t place[28]; } placed[16];
        unsigned slot = id % 16;
        int same = placed[slot].id == id && placed[slot].generation == generation;
        for (unsigned k = 0; same && k < 28; k++)
            same = placed[slot].place[k] == m->data[DESK_V3_HEADER + k];
        idle_op = same;
        placed[slot].id = id; placed[slot].generation = generation;
        w32_copy(placed[slot].place, m->data + DESK_V3_HEADER, 28);
    }
    if (op != DESK_INPUT_SETTINGS && op != DESK_DAMAGE && !idle_op)
        last_activity = now_ms();
    /* DESK_INPUT_SETTINGS is the input-settings query user32 repeats while
     * a GUI program idles; it would use up the record budget (fx-is5boot). */
    if ((opcode_log < 400 || installer_idle_ms) && op != DESK_DAMAGE && op != DESK_V3_PRESENT && op != DESK_V3_PLACE &&
        op != DESK_INPUT_SETTINGS) {
        opcode_log++;
        host_print("GAME desktop op="); host_number(op); host_print(" window="); host_number(id);
        host_print(" generation="); host_number(generation); host_print(" bytes="); host_number(m->length);
        if (op == DESK_CREATE_WINDOW && m->length >= DESK_V3_HEADER + 36) {
            /* Wire fields only (fx-is5boot); the title stays last so a
             * caller can cut the program's own text from the record. */
            host_print(" style="); host_number(get32(m->data + DESK_V3_HEADER));
            host_print(" owner="); host_number(get32(m->data + DESK_V3_HEADER + 4));
            host_print(" size="); host_number(get32(m->data + DESK_V3_HEADER + 20));
            host_print("x"); host_number(get32(m->data + DESK_V3_HEADER + 24));
        }
        if (op == DESK_V3_WIN32_HWND && m->length >= DESK_V3_HEADER + 4) {
            host_print(" hwnd="); host_number(get32(m->data + DESK_V3_HEADER));
        }
        if (op == DESK_V3_PLACE && m->length >= DESK_V3_HEADER + 28) {
            host_print(" state="); host_number(get32(m->data + DESK_V3_HEADER + 20));
            host_print(" style="); host_number(get32(m->data + DESK_V3_HEADER + 24));
        }
        if (op == DESK_CREATE_WINDOW && m->length > DESK_V3_HEADER + 36) {
            if (get32(m->data + DESK_V3_HEADER) & DESK_V3_MODAL)
                host_print(" modal=1");
            char title[80]; unsigned n = m->length - DESK_V3_HEADER - 36;
            if (n > 79) n = 79;
            w32_copy(title, m->data + DESK_V3_HEADER + 36, n); title[n] = 0;
            host_print(" title="); host_print(title);
        }
        if (installer_idle_ms) { host_print(" ms="); host_number(now_ms() - start_ms); }
        host_print("\n");
    }
    int32_t result = 0;
    if (op == DESK_CREATE_WINDOW && m->fd_count == 1) {
        for (unsigned i = 0; i < 64; i++)
            if (!windows[i].id || (windows[i].id == id && windows[i].generation != generation)) {
                windows[i].id = id; windows[i].generation = generation; windows[i].fd = (uint32_t)m->fds[0];
                windows[i].titled = m->length > DESK_V3_HEADER + 36;
                windows[i].gdi_frames = 0;
                break;
            }
    }
    /* fx-q3vise: ShellExecute(Ex) of a folder asks the desktop for Files there. */
    if (op == DESK_V3_LAUNCH && m->length >= DESK_V3_HEADER + 8) {
        uint32_t t = get32(m->data + DESK_V3_HEADER), n = t < 64 ? get32(m->data + DESK_V3_HEADER + 4 + t) : 0;
        char text[256];
        host_print("GAME launch title=");
        if (t < 64 && DESK_V3_HEADER + 8 + t + n == m->length && n < sizeof(text)) {
            w32_copy(text, m->data + DESK_V3_HEADER + 4, t); text[t] = 0; host_print(text);
            w32_copy(text, m->data + DESK_V3_HEADER + 8 + t, n); text[n] = 0; host_print(" argument="); host_print(text);
        } else
            host_print("? malformed");
        host_print(" ms="); host_number(now_ms() - start_ms); host_print("\n");
        launches++;
    }
    /* fx-hlrun: the desktop answers every placement with DESK_CONFIGURE
     * (apps/desktop/client.c v3_place -> v3_configure): the content size
     * and origin (the frame's origin plus the 3x24 title frame unless the
     * window is a popup), a fresh resize serial, the state and the style.
     * USER resizes a window's surface only on that answer, so without it a
     * window resized after creation (MessageBox lays itself out in
     * WM_INITDIALOG) kept its first size. */
    if (op == DESK_V3_PLACE && m->length >= DESK_V3_HEADER + 28) {
        static uint32_t place_serial;
        const uint8_t *b = m->data + DESK_V3_HEADER;
        uint32_t style = get32(b + 24), state = get32(b + 20);
        int popup = (style & 3) == DESK_V3_POPUP;
        struct ciuki_message *c = state <= 3 ? event(DESK_CONFIGURE, 32, 0, id, generation) : 0;
        if (c) {
            if (!++place_serial) ++place_serial;
            put32(c->data + 16, get32(b + 8)); put32(c->data + 20, get32(b + 12)); put32(c->data + 24, 1);
            put32(c->data + 28, place_serial);
            put32(c->data + 32, get32(b) + (popup ? 0 : 3)); put32(c->data + 36, get32(b + 4) + (popup ? 0 : 24));
            put32(c->data + 40, state); put32(c->data + 44, style);
        }
    }
    if (op == DESK_V3_RESIZE && m->fd_count == 1)
        for (unsigned i = 0; i < 64; i++)
            if (windows[i].id == id && windows[i].generation == generation)
                windows[i].fd = (uint32_t)m->fds[0];
    if (op == DESK_CLOSE) {
        for (unsigned i = 0; i < 64; i++)
            if (windows[i].id == id && windows[i].generation == generation)
                windows[i].id = 0;
        if (installer_idle_ms && focus_id == id && focus_generation == generation)
            focus_id = focus_generation = 0; /* the next window created takes the focus */
    }
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
        installer_present_ms = now_ms();
        int shown = 0;
        for (unsigned i = 0; i < 64 && op == DESK_V3_PRESENT; i++)
            if (windows[i].id == id && windows[i].generation == generation) {
                uint32_t k = windows[i].fd - SURFACE_FD;
                if (k < SURFACES && surfaces[k].used) {
                    frame(surfaces[k].pixels, surfaces[k].width, surfaces[k].height, surfaces[k].pitch, 32, "window");
                    shown = 1;
                }
            }
        /* fx-is5boot: GDI windows (dialogs, installer pages) publish
         * DAMAGE, not PRESENT. Their frames get their own counter and kind,
         * so lease/GL frame numbering and the summary stay unchanged. No
         * pixels are written: only the surface checksum is recorded. */
        for (unsigned i = 0; i < 64 && op == DESK_DAMAGE; i++)
            if (windows[i].id == id && windows[i].generation == generation) {
                uint32_t k = windows[i].fd - SURFACE_FD;
                if (k >= SURFACES || !surfaces[k].used)
                    continue;
                static uint32_t gdi_frames;
                gdi_frames++;
                shown = 1;
                /* Each window's first frame, then the global sampling. */
                if (windows[i].gdi_frames++ && gdi_frames > 4 && (gdi_frames & (gdi_frames - 1)))
                    continue;
                uint32_t sum = 2166136261u;
                for (uint32_t y = 0; y < surfaces[k].height; y += 7)
                    for (uint32_t x = 0; x < surfaces[k].width * 4; x += 13)
                        sum = (sum ^ surfaces[k].pixels[y * surfaces[k].pitch + x]) * 16777619u;
                host_print("GAME frame n="); host_number(gdi_frames); host_print(" kind=gdi window=");
                host_number(id); host_print(" generation="); host_number(generation);
                host_print(" size="); host_number(surfaces[k].width); host_print("x"); host_number(surfaces[k].height);
                host_print(" checksum="); hex(sum); host_print("\n");
                if (gdi_frame_files)
                    dump_named(surfaces[k].pixels, surfaces[k].width, surfaces[k].height, surfaces[k].pitch, 32, 'g', gdi_frames);
            }
        static unsigned unmatched;
        if (op == DESK_V3_PRESENT && !shown && unmatched < 8) {
            unmatched++;
            host_print("GAME present without surface window="); host_number(id);
            host_print(" generation="); host_number(generation); host_print("\n");
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
static int other_thread(void);
static int32_t message(struct ciuki_w32_message_args *a) {
    a->result = a->error = 0;
    /* fx-hlrun: the one GUI queue belongs to the main thread; a worker
     * thread's queue stays empty (it never owns the game's windows). */
    if (other_thread() && (a->action == W32_MSG_SELECT || a->action == W32_MSG_STATUS || a->action == W32_MSG_MARK ||
                           a->action == W32_MSG_GET || a->action == W32_MSG_INPUT_IDLE)) {
        if (a->action == W32_MSG_INPUT_IDLE) a->result = 1;
        return 0;
    }
    if (a->action == W32_MSG_SELECT)
        yield_turn();
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

/* fx-hlrun: with "stacks=1" every raised exception (first chance) prints its
 * code, address, parameters and the image addresses on the stack at the
 * raise; delivery continues on the normal dispatch (no adapter raise path). */
static unsigned raises;
static int game_raise(void *opaque, struct w32_exception *r, struct w32_context *c) {
    (void)opaque;
    if (!installer_stacks || raises >= 64)
        return 0;
    raises++;
    host_print("GAME raise code="); hex(r->ExceptionCode); host_print(" flags="); hex(r->ExceptionFlags);
    host_print(" eip="); hex(c->Eip); host_print(" params");
    for (uint32_t i = 0; i < r->NumberParameters && i < 4; i++) { host_print(" "); hex(r->ExceptionInformation[i]); }
    host_print(" stack");
    const uint32_t *sp = (const uint32_t *)(uintptr_t)c->Esp;
    unsigned shown = 0;
    const uint32_t *end = (const uint32_t *)NtCurrentTeb()->Tib.StackBase;
    for (unsigned i = 0; i < 1024 && shown < 32 && sp + i < end; i++) {
        uint32_t v = sp[i];
        if ((v >= 0x400000u && v < 0x600000u) || (v >= 0x10000000u && v < 0x11000000u) ||
            (v >= 0x60000000u && v < 0x61000000u) || (v >= 0x70000000u && v < 0x7ffe0000u)) {
            host_print(" "); hex(v); shown++;
        }
    }
    host_print("\n");
    return 0;
}

/* ---- threads (fx-hlrun) --------------------------------------------------
 * Test-only. Half-Life (as many Windows 98 games) starts worker threads: its
 * CD-audio thread opens the drive the disc check measures. The kernel's
 * W32_THREAD record (src/kernel/proc/win32/syscall.c create_thread, TEB of
 * personality.c) is answered with a Linux thread (clone with CLONE_VM,
 * CLONE_THREAD and a TLS descriptor for the new TEB at the main thread's
 * %fs entry): its own TEB and stack, created suspended until W32_CTX_RESUME,
 * entering the kernel32 trampoline. Thread handles are the adapter's own
 * (OPEN of W32_OBJ_THREAD by tid, DUPLICATE of the current-thread pseudo
 * handle): QUERY gives the exit code, waits complete at exit, suspend counts,
 * priorities and W32_S_TERMINATE_THREAD are kept here. A wait on fixture
 * handles is polled (timeout 0) with the lock released between polls, so the
 * thread that sets an event can run; critical-section waits (the ntdll wait
 * words) release the lock too. W32_S_THREAD_EXIT ends the Linux thread. */
#define THREADS 32
#define THREAD_HANDLE 0x6f000000u
struct game_thread {
    volatile uint32_t state;   /* 0 free, 1 suspended, 2 running, 3 exited */
    volatile uint32_t suspend;
    volatile uint32_t alive;   /* the Linux tid until the kernel clears it at exit (CLONE_CHILD_CLEARTID) */
    uint32_t tid, exit_code, priority, terminate;
    uint32_t entry, argument;
    struct w32_teb *teb;
    uint8_t *stack;
    uint32_t stack_bytes;
};
static struct game_thread threads[THREADS];
static uint32_t thread_handles[256]; /* index + 1 per handle slot, 0 free */
static uint16_t thread_handle_gen[256];
static int fs_entry = -1;
static void (*base_wait_word)(void *, volatile ULONG *, ULONG);
static struct game_thread *thread_self(void) {
    uint32_t tid = current_tid();
    for (unsigned i = 0; i < THREADS; i++)
        if (threads[i].state && threads[i].tid == tid)
            return &threads[i];
    return 0;
}
static int other_thread(void) { return threads_on && thread_self() != &threads[0]; }
static void __attribute__((noreturn)) thread_end(struct game_thread *t, uint32_t code) {
    t->exit_code = code;
    __atomic_store_n(&t->state, 3, __ATOMIC_RELEASE);
    host_print("GAME thread exit tid="); host_number(t->tid); host_print(" code="); host_number(code); host_print("\n");
    lock_release();
    for (;;)
        host_syscall(1, 0, 0, 0, 0, 0, 0); /* exit: this Linux thread only */
}
/* A thread asked to terminate ends when it next takes the lock. */
static void thread_checkpoint(void) {
    struct game_thread *t = threads_on ? thread_self() : 0;
    if (t && t != &threads[0] && t->terminate)
        thread_end(t, t->exit_code);
}
static void locked_wait_word(void *opaque, volatile ULONG *p, ULONG value) {
    lock_release();
    base_wait_word(opaque, p, value);
    lock_acquire();
    thread_checkpoint();
}
static void __attribute__((cdecl, noreturn, used)) thread_main(struct game_thread *t) {
    uint16_t selector;
    __asm__ volatile("movw %%fs,%0\n\tmovw %0,%%fs" : "=r"(selector)); /* load this thread's TLS descriptor */
    while (__atomic_load_n(&t->suspend, __ATOMIC_ACQUIRE))
        futex(&t->suspend, 0, __atomic_load_n(&t->suspend, __ATOMIC_ACQUIRE));
    lock_acquire();
    thread_checkpoint();
    __atomic_store_n(&t->state, 2, __ATOMIC_RELEASE);
    host_print("GAME thread start tid="); host_number(t->tid); host_print("\n");
    ((void(W32API *)(uint32_t))(uintptr_t)t->entry)(t->argument);
    thread_end(t, 0);
}
/* clone(flags, stack, 0, tls, ctid): the child pops thread_main's argument
 * frame from its own stack and never returns here. */
extern int32_t h1_clone(uint32_t flags, void *stack, void *tls, volatile uint32_t *ctid);
__asm__(".text\n.globl h1_clone\n.type h1_clone,@function\n"
        "h1_clone:\n"
        "  pushl %ebx\n  pushl %esi\n  pushl %edi\n  pushl %ebp\n"
        "  movl 20(%esp),%ebx\n  movl 24(%esp),%ecx\n  xorl %edx,%edx\n  movl 28(%esp),%esi\n  movl 32(%esp),%edi\n"
        "  movl $120,%eax\n  int $0x80\n"
        "  testl %eax,%eax\n  jnz 1f\n"
        "  xorl %ebp,%ebp\n  popl %eax\n  call *%eax\n" /* thread_main(t): t is at the new stack top */
        "  movl $1,%eax\n  xorl %ebx,%ebx\n  int $0x80\n"
        "1:\n  popl %ebp\n  popl %edi\n  popl %esi\n  popl %ebx\n  ret\n");
static uint32_t handle_for(unsigned index) {
    for (unsigned h = 0; h < 256; h++)
        if (!thread_handles[h]) {
            thread_handles[h] = index + 1;
            thread_handle_gen[h]++;
            return THREAD_HANDLE | (uint32_t)(thread_handle_gen[h] & 0x3fff) << 10 | h << 2;
        }
    return 0;
}
static struct game_thread *handle_thread(uint32_t h, unsigned *slot) {
    if (h == 0xfffffffeu) /* GetCurrentThread() */
        return thread_self();
    if ((h & 0xff000000u) != THREAD_HANDLE)
        return 0;
    unsigned k = (h >> 2) & 255;
    if (!thread_handles[k] || ((h >> 10) & 0x3fff) != (thread_handle_gen[k] & 0x3fffu))
        return 0;
    if (slot) *slot = k;
    return &threads[thread_handles[k] - 1];
}
static int create_thread(struct ciuki_w32_thread *a) {
    unsigned i = 1;
    while (i < THREADS && threads[i].state && (threads[i].state != 3 || __atomic_load_n(&threads[i].alive, __ATOMIC_ACQUIRE))) i++;
    if (i == THREADS) return (int)0xC000009Au; /* STATUS_INSUFFICIENT_RESOURCES */
    struct game_thread *t = &threads[i];
    if (t->state == 3) { /* reuse an exited thread's slot */
        for (unsigned h = 0; h < 256; h++) if (thread_handles[h] == i + 1) return (int)0xC000009Au;
        host.services.free(&host, t->stack, t->stack_bytes);
        host.services.free(&host, t->teb, 4096);
    }
    uint32_t bytes = a->stack_bytes > (1u << 20) ? (a->stack_bytes + 65535) & ~65535u : 1u << 20;
    w32_fill(t, 0, sizeof(*t));
    t->stack = host.services.alloc(&host, bytes);
    t->teb = host.services.alloc(&host, 4096);
    if (!t->stack || !t->teb) return (int)0xC0000017u; /* STATUS_NO_MEMORY */
    t->stack_bytes = bytes; t->entry = a->entry; t->argument = a->argument; t->suspend = 1; t->state = 1;
    struct w32_teb *teb = t->teb;
    w32_fill(teb, 0, 4096);
    teb->Tib.ExceptionList = (void *)~(uintptr_t)0;
    teb->Tib.StackBase = t->stack + bytes;
    teb->Tib.StackLimit = t->stack;
    teb->Tib.Self = &teb->Tib;
    teb->ProcessEnvironmentBlock = host.teb.ProcessEnvironmentBlock;
    teb->ClientId.UniqueProcess = host.teb.ClientId.UniqueProcess;
    uint32_t *top = (uint32_t *)(void *)(t->stack + bytes) - 4;
    top[0] = (uint32_t)(uintptr_t)thread_main; top[1] = (uint32_t)(uintptr_t)t;
    struct { uint32_t entry, base, limit, flags; } tls = {(uint32_t)fs_entry, (uint32_t)(uintptr_t)teb, 8191, 0x41};
    t->alive = 1;
    int32_t tid = h1_clone(0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x200000, top, &tls, &t->alive);
    if (tid <= 0) { t->state = 0; t->alive = 0; return (int)0xC000009Au; }
    t->tid = (uint32_t)tid;
    teb->ClientId.UniqueThread = (void *)(uintptr_t)t->tid;
    a->tid = t->tid;
    host_print("GAME thread create tid="); host_number(t->tid); host_print(" by="); host_number(current_tid());
    host_print(" start="); hex(a->argument ? *(const uint32_t *)(uintptr_t)a->argument : 0); host_print("\n");
    return 0;
}
static void resume(struct game_thread *t) {
    if (t->suspend && !--t->suspend)
        futex(&t->suspend, 1, 1);
}
/* W32_OBJECT, W32_CONTEXT and W32_SYSTEM records naming a thread handle;
 * 1 when handled here. */
static int thread_object(struct ciuki_w32_object_args *a) {
    unsigned slot = 0;
    if (a->type == W32_OBJ_THREAD && a->action == W32_OBJ_OPEN) {
        a->result = 0; a->error = 87;
        for (unsigned i = 0; i < THREADS; i++)
            if (threads[i].state && threads[i].tid == a->value) {
                a->result = handle_for(i);
                a->error = a->result ? 0 : 4;
            }
        return 1;
    }
    struct game_thread *t = handle_thread(a->handle, &slot);
    if (!t)
        return 0;
    a->error = 0;
    switch (a->action) {
    case W32_OBJ_CLOSE:
        if ((a->handle & 0xff000000u) == THREAD_HANDLE) thread_handles[slot] = 0;
        a->result = 1;
        return 1;
    case W32_OBJ_QUERY:
        a->type = W32_OBJ_THREAD; a->flags = 0;
        a->result = t->state == 3 ? t->exit_code : 259; /* STILL_ACTIVE */
        return 1;
    case W32_OBJ_DUPLICATE:
        a->result = handle_for((unsigned)(t - threads));
        a->error = a->result ? 0 : 4;
        if ((a->flags & 1 /* DUPLICATE_CLOSE_SOURCE */) && (a->handle & 0xff000000u) == THREAD_HANDLE)
            thread_handles[slot] = 0;
        return 1;
    default:
        a->result = 0; a->error = 120;
        return 1;
    }
}
static int thread_context(struct ciuki_w32_context_args *a) {
    struct game_thread *t = handle_thread(a->thread, 0);
    if (!t)
        return 0;
    a->error = 0;
    if (a->action == W32_CTX_RESUME) { a->result = t->suspend; resume(t); }
    else if (a->action == W32_CTX_SUSPEND) {
        a->result = t->suspend;
        if (t != thread_self()) { a->result = ~0u; a->error = 5; } /* a running Linux thread is not stopped */
        else a->error = 5;
    } else { a->result = 0; a->error = 5; }
    return 1;
}
static int thread_system(struct ciuki_w32_system_args *a) {
    if (a->action == W32_S_THREAD_EXIT) {
        struct game_thread *t = thread_self();
        if (!t || t == &threads[0])
            return 0; /* the main thread: the process ends as before */
        thread_end(t, a->value);
    }
    if (a->action == W32_S_YIELD) {
        a->result = 1; a->error = 0;
        yield_turn();
        return 1;
    }
    if (a->action != W32_S_TERMINATE_THREAD && a->action != W32_S_THREAD_PRIORITY && a->action != W32_S_QUERY_PRIORITY)
        return 0;
    struct game_thread *t = handle_thread(a->handle, 0);
    if (!t && a->action == W32_S_TERMINATE_THREAD && a->extra)
        for (unsigned i = 1; i < THREADS; i++)
            if (threads[i].state && threads[i].tid == a->extra) t = &threads[i];
    if (!t)
        return 0;
    a->error = 0; a->result = 1;
    if (a->action == W32_S_THREAD_PRIORITY) t->priority = a->value;
    else if (a->action == W32_S_QUERY_PRIORITY) a->result = a->value == 1 ? t->priority : 8;
    else if (t == &threads[0] || t == thread_self()) a->error = 5;
    else if (t->state != 3) {
        t->exit_code = a->value; t->terminate = 1;
        resume(t); t->suspend = 0; futex(&t->suspend, 1, 1);
    }
    return 1;
}
/* WaitForSingle/MultipleObjects: thread handles here, the others polled at
 * the fixture service; the lock is free between polls. */
static int thread_wait(struct ciuki_w32_wait_args *a, int32_t *status) {
    const uint32_t *handles = (const uint32_t *)(uintptr_t)a->handles;
    unsigned local = 0, workers = 0;
    for (uint32_t i = 0; i < a->count; i++)
        local += handle_thread(handles[i], 0) != 0;
    for (unsigned i = 1; i < THREADS; i++)
        workers += threads[i].state == 1 || threads[i].state == 2;
    if (!local && !workers)
        return 0; /* no other thread to wait for here: the fixture service waits, as before */
    uint32_t start = now_ms();
    for (;;) {
        if (local) {
            uint32_t done = 0, first = ~0u;
            for (uint32_t i = 0; i < a->count; i++) {
                struct game_thread *t = handle_thread(handles[i], 0);
                int ready = t && t->state == 3;
                if (ready) { done++; if (first == ~0u) first = i; }
            }
            if (local == a->count && (a->all ? done == a->count : done != 0)) {
                a->result = a->all ? 0 : first; a->error = 0; *status = 0;
                return 1;
            }
        }
        if (local < a->count && !local) {
            struct ciuki_w32_wait_args poll = *a;
            poll.timeout = 0;
            int32_t e = 0;
            if (!h1_installer_kernel(&poll, sizeof(poll), &e)) return 0;
            if (e || poll.result != W32_WAIT_TIMEOUT) {
                *a = poll; a->timeout = 0; *status = e;
                return 1;
            }
        }
        if (a->timeout != 0xffffffffu && now_ms() - start >= a->timeout) {
            a->result = W32_WAIT_TIMEOUT; a->error = 0; *status = 0;
            return 1;
        }
        sleep_ms(1);
    }
}
static void threads_start(void) {
    uint16_t selector;
    __asm__ volatile("movw %%fs,%0" : "=r"(selector));
    fs_entry = selector >> 3;
    threads[0].state = 2; threads[0].tid = current_tid(); threads[0].teb = &host.teb;
    lock_acquire();
    base_wait_word = host.services.wait_word;
    host.services.wait_word = locked_wait_word;
    threads_on = 1;
}
/* ---- entry points ------------------------------------------------------- */
void h1_game_init(int32_t (*kernel)(void *, uint32_t)) {
    kernel_call = kernel; enabled = 1; start_ms = now_ms();
    threads_start();
    frame_pid = (uint32_t)host_syscall(20, 0, 0, 0, 0, 0, 0); /* getpid */
    host.services.raise = game_raise;
    profile_start();
}
int h1_game_enabled(void) { return enabled; }
int h1_game_kernel(void *record, uint32_t size, int32_t *status) {
    struct ciuki_w32_header *h = record;
    if (!enabled || size < sizeof(*h))
        return 0;
    trace(h->operation << 16 | (size >= 20 ? ((const uint32_t *)record)[4] & 0xffff : 0));
    static uint32_t sampled;
    if (installer_stacks && !stacks_every && !(++sampled % 100000u))
        installer_stack("record", h->operation, size >= 20 ? ((const uint32_t *)record)[4] : 0);
    /* A copy without repaints is not idle; user32's periodic settings-file
     * open is (only writes and process operations count). */
    if ((h->operation == W32_FILE && size >= 20 && ((const uint32_t *)record)[4] == W32_F_WRITE) ||
        h->operation == W32_PROCESS)
        last_activity = now_ms();
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
    if (h->operation == W32_THREAD && size == sizeof(struct ciuki_w32_thread)) {
        *status = create_thread(record);
        return 1;
    }
    if (h->operation == W32_OBJECT && size == sizeof(struct ciuki_w32_object_args) && thread_object(record)) {
        *status = 0;
        return 1;
    }
    if (h->operation == W32_CONTEXT && size == sizeof(struct ciuki_w32_context_args) && thread_context(record)) {
        *status = 0;
        return 1;
    }
    if (h->operation == W32_SYSTEM && size == sizeof(struct ciuki_w32_system_args) && thread_system(record)) {
        *status = 0;
        return 1;
    }
    if (h->operation == W32_WAIT && size == sizeof(struct ciuki_w32_wait_args)) {
        struct ciuki_w32_wait_args *a = record;
        if (a->count) {
            if (a->timeout >= 1000) installer_stack("wait-handles", a->timeout, a->count);
            return thread_wait(a, status); /* thread handles here, object handles polled at the fixture service */
        }
        a->error = 0;
        if (other_thread()) {
            /* fx-hlrun: a worker thread's Sleep: the whole time, the lock
             * free (the GUI queue and its wake bits are the main thread's) */
            if (!a->timeout)
                yield_turn();
            for (uint32_t began = now_ms(); a->timeout && (a->timeout == 0xffffffffu || now_ms() - began < a->timeout);) {
                uint32_t left = a->timeout == 0xffffffffu ? 50 : a->timeout - (now_ms() - began);
                sleep_ms(left > 50 ? 50 : left);
            }
            a->result = W32_WAIT_TIMEOUT; *status = 0;
            return 1;
        }
        if (queue_count || quit_pending || (marked & a->wake_mask)) { a->result = 0; *status = 0; return 1; }
        /* A fatal renderer error in id's Win32 implementation runs the
         * console's GetMessage loop. Act as the user closing that window;
         * never synthesize success or bypass the production window proc. */
        if (close_after_eio && lease_failures && !close_sent && focus_id && a->timeout) {
            event(DESK_CLOSE_REQUEST,0,0,focus_id,focus_generation);
            close_sent=1;
            host_print("GAME close after failed lease\n");
        }
        if (a->timeout >= 1000 || stacks_every) installer_stack("wait", a->timeout, a->wake_mask);
        installer_tick();   /* a key goes to the desktop channel, read after this wait */
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
    case CIUKI_SYS_SLEEP_MS: if (a >= 100) installer_stack("sleep", a, 0); sleep_ms(a); *out = 0; return 1;
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
        if (in_head == in_tail) installer_tick();
        if (in_head == in_tail && click_w)
            click_tick();
        if (in_head == in_tail && defaults_max)
            default_key();
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
    /* fx-hlrun: with "gdiframes=1", the final pixels of every open window
     * (q3-frames/wNNNNN.ppm by wire window id; local only). */
    for (unsigned i = 0; gdi_frame_files && i < 64; i++)
        if (windows[i].id) {
            uint32_t k = windows[i].fd - SURFACE_FD;
            if (k < SURFACES && surfaces[k].used)
                dump_named(surfaces[k].pixels, surfaces[k].width, surfaces[k].height, surfaces[k].pitch, 32, 'w',
                           windows[i].id);
        }
    host_print("GAME summary frames="); host_number(frames);
    host_print(" presents="); host_number(presents);
    host_print(" desktop_requests="); host_number(requests);
    host_print(" refused_frames="); host_number(refused);
    host_print(" gamma_records="); host_number(gamma_records);
    host_print(" first_to_last_ms="); host_number(frames ? last_ms - first_ms : 0);
    host_print(" last_checksum="); hex(checksum);
    host_print(" lease="); host_number(lease_w); host_print("x"); host_number(lease_h); host_print("x"); host_number(lease_bpp);
    host_print(" launches="); host_number(launches);
    host_print("\n");
    (void)frame_sum;
    api_report();
    gl_report();
    profile_report();
}
