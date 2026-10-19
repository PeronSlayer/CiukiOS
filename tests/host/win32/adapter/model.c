/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors.
 * H0 model adapter only: no PE execution, no i386 ABI/FS evidence. */
#define _GNU_SOURCE
#include "linux.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
struct host_state host;
_Thread_local struct w32_teb *w32_model_teb;
void host_print(const char *s) {
    fputs(s, stdout);
    fflush(stdout);
}
void host_number(ULONG n) {
    printf("%u", n);
    fflush(stdout);
}
void host_exit(ULONG n) {
    exit(n);
}
static void *allocate(void *opaque, ULONG n) {
    struct host_state *s = opaque;
    if (s->fail_after >= 0 && s->fail_after-- == 0)
        return 0;
    void *p = 0;
    if (posix_memalign(&p, 65536, n ? n : 1))
        return 0;
    s->allocations++;
    return p;
}
static void release(void *opaque, void *p, ULONG n) {
    (void)n;
    if (p) {
        free(p);
        ((struct host_state *)opaque)->allocations--;
    }
}
static void *map(void *opaque, ULONG preferred, ULONG size, int exact) {
    struct host_state *s = opaque;
    void *p = MAP_FAILED;
    if (preferred)
        p = mmap((void *)(uintptr_t)preferred, (size + 4095) & ~4095u, 3,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED && !exact)
        for (ULONG at = 0x01000000; at < 0x70000000 && size <= 0x70000000 - at; at += 65536) {
            p = mmap((void *)(uintptr_t)at, (size + 4095) & ~4095u, 3,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED)
                break;
        }
    if (p == MAP_FAILED)
        return 0;
    s->mappings++;
    return p;
}
static void unmap(void *opaque, void *p, ULONG size) {
    if (p) {
        munmap(p, (size + 4095) & ~4095u);
        ((struct host_state *)opaque)->mappings--;
    }
}
static int protect(void *opaque, void *p, ULONG n, ULONG mode) {
    (void)opaque;
    return !mprotect(p, n, mode);
}
static int open_file(void *opaque, const char *path, struct w32_file *f) {
    (void)opaque;
    char name[W32_PATH_MAX];
    ULONG i = 0;
    if (w32_strlen(path) >= sizeof(name))
        return 0;
    for (; path[i]; i++)
        name[i] = path[i] == '\\' ? '/' : path[i];
    name[i] = 0;
    int fd = open(name, O_RDONLY);
    if (fd < 0)
        return 0;
    struct stat st;
    if (fstat(fd, &st) || st.st_size <= 0 || st.st_size > 0x20000000) {
        close(fd);
        return 0;
    }
    void *p = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        return 0;
    *f = (struct w32_file){p, st.st_size, p};
    return 1;
}
static void close_file(void *opaque, struct w32_file *f) {
    munmap(f->cookie, f->size);
    ((struct host_state *)opaque)->closes++;
}
static ULONG tid(void *opaque) {
    (void)opaque;
    return syscall(SYS_gettid);
}
static void wait_word(void *opaque, volatile ULONG *p, ULONG v) {
    (void)opaque;
    syscall(SYS_futex, p, FUTEX_WAIT_PRIVATE, v, 0, 0, 0);
}
static void wake_word(void *opaque, volatile ULONG *p) {
    (void)opaque;
    syscall(SYS_futex, p, FUTEX_WAKE_PRIVATE, 0x7fffffff, 0, 0, 0);
}
static void diagnostic(void *opaque, const char *event, const char *module, const char *symbol) {
    struct host_state *s = opaque;
    w32_copy_string(s->last_event, sizeof(s->last_event), event);
    w32_copy_string(s->last_module, sizeof(s->last_module), module);
    w32_copy_string(s->last_symbol, sizeof(s->last_symbol), symbol);
    printf("[win32] %s %s %s\n", event, module, symbol);
}
static void terminate(void *opaque, ULONG code) {
    (void)opaque;
    printf("TERMINATE %u\n", code);
    exit(90);
}
static int observer(void *opaque, void *proc, void *module, ULONG reason, int dll) {
    (void)proc;
    struct host_state *s = opaque;
    s->invocations++;
    printf("CALL %u %u %s\n", (unsigned)(uintptr_t)module, reason, dll ? "DLL" : "TLS");
    return !(s->fail_attach && dll && reason == 1);
}
void host_fs(struct w32_teb *teb) {
    w32_model_teb = teb;
    teb->Tib.Self = &teb->Tib;
}
void host_init(void) {
    w32_fill(&host, 0, sizeof(host));
    host.fail_after = -1;
    host.observer = 1;
    host.services = (struct w32_services){&host,     allocate,   release,    map,       protect,
                                          unmap,     open_file,  close_file, tid,       wait_word,
                                          wake_word, diagnostic, 0,          terminate, observer, 0, 0, 0, 0, 0, 0};
    host.teb.Tib.ExceptionList = (void *)~(uintptr_t)0;
    uintptr_t sp = (uintptr_t)&sp;
    host.teb.Tib.StackBase = (void *)(sp + 1024 * 1024);
    host.teb.Tib.StackLimit = (void *)(sp - 8 * 1024 * 1024);
    host.teb.ProcessEnvironmentBlock = &host.peb;
    host.peb.ProcessParameters = &host.parameters;
    static WCHAR cwd[] = {'C', ':', '\\', 0};
    RtlInitUnicodeString(&host.parameters.CurrentDirectory.DosPath, cwd);
    host_fs(&host.teb);
    w32_set_services(&host.services);
}
/* State-machine callers supply explicit contexts. Native wrappers intentionally
 * cannot resume i386 execution. The real capture/raise/unwind stays in context.S. */
void W32API RtlCaptureContext(struct w32_context *c) {
    w32_fill(c, 0, sizeof(*c));
}
extern void W32API w32_raise_c(struct w32_exception *, struct w32_context *);
void W32API RtlRaiseException(struct w32_exception *r) {
    struct w32_context c;
    RtlCaptureContext(&c);
    w32_raise_c(r, &c);
}
extern void W32API w32_unwind_c(void *, void *, struct w32_exception *, void *,
                                struct w32_context *);
void W32API RtlUnwind(void *t, void *ip, struct w32_exception *r, void *v) {
    struct w32_context c;
    RtlCaptureContext(&c);
    w32_unwind_c(t, ip, r, v, &c);
}
int host_entry(void *entry, void *stack) {
    (void)entry;
    (void)stack;
    host_print("H0 CANNOT EXECUTE PE\n");
    exit(91);
}
int main(int argc, char **argv) {
    host_init();
    return host_main(argc, argv);
}
