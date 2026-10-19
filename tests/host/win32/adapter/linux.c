/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Linux host evidence only; no libc. */
#include "linux.h"
#include <asm/sigcontext.h>
struct host_state host;
#define PTR(p) ((ULONG)(uintptr_t)(p))
LONG host_syscall(ULONG n, ULONG a, ULONG b, ULONG c, ULONG d, ULONG e, ULONG f) {
    LONG result;
    __asm__ volatile("pushl %%ebp; movl %7,%%ebp; int $0x80; popl %%ebp"
                     : "=a"(result)
                     : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "m"(f)
                     : "memory", "cc");
    return result;
}
void host_print(const char *s) {
    host_syscall(4, 1, PTR(s), w32_strlen(s), 0, 0, 0);
}
void host_number(ULONG n) {
    char text[11];
    ULONG i = 10;
    text[i] = 0;
    do {
        text[--i] = '0' + n % 10;
        n /= 10;
    } while (n);
    host_print(text + i);
}
void host_exit(ULONG code) {
    host_syscall(1, code, 0, 0, 0, 0, 0);
    __builtin_trap();
}
static ULONG round_page(ULONG n) {
    return (n + 4095) & ~4095u;
}
static void *mapping(ULONG address, ULONG size, ULONG protection, ULONG flags, LONG fd) {
    LONG result = host_syscall(192, address, round_page(size), protection, flags, fd, 0);
    return (ULONG)result >= 0xfffff001u ? 0 : (void *)(uintptr_t)result;
}
static void *map(void *opaque, ULONG preferred, ULONG size, int exact) {
    struct host_state *s = opaque;
    void *p = 0;
    if (preferred)
        p = mapping(preferred, size, 3, 0x100022, -1);
    if (!p && !exact)
        for (ULONG at = 0x01000000; at < 0x70000000 && size <= 0x70000000 - at; at += 65536) {
            p = mapping(at, size, 3, 0x100022, -1);
            if (p)
                break;
        }
    if (p)
        s->mappings++;
    return p;
}
static void unmap(void *opaque, void *p, ULONG n) {
    struct host_state *s = opaque;
    if (p) {
        host_syscall(91, PTR(p), round_page(n), 0, 0, 0, 0);
        s->mappings--;
    }
}
static void *allocate(void *opaque, ULONG n) {
    struct host_state *s = opaque;
    if (!n || n > 0x20000000 || (s->fail_after >= 0 && s->fail_after-- == 0))
        return 0;
    for (ULONG at = s->next_allocation; at < 0x60000000 && n <= 0x60000000 - at; at += 65536) {
        void *p = mapping(at, n, 3, 0x100022, -1);
        if (p) {
            s->next_allocation = at + ((n + 65535) & ~65535u);
            s->allocations++;
            return p;
        }
    }
    return 0;
}
static void release(void *opaque, void *p, ULONG n) {
    struct host_state *s = opaque;
    if (!p)
        return;
    host_syscall(91, PTR(p), round_page(n), 0, 0, 0, 0);
    s->allocations--;
    if (PTR(p) < s->next_allocation)
        s->next_allocation = PTR(p);
}
static int protect(void *opaque, void *p, ULONG n, ULONG prot) {
    (void)opaque;
    return !host_syscall(125, PTR(p), n, prot, 0, 0, 0);
}
static int open_file(void *opaque, const char *name, struct w32_file *f) {
    (void)opaque;
    char path[W32_PATH_MAX];
    ULONG i = 0;
    if (w32_strlen(name) >= sizeof(path))
        return 0;
    for (; name[i]; i++)
        path[i] = name[i] == '\\' ? '/' : name[i];
    path[i] = 0;
    LONG fd = host_syscall(5, PTR(path), 0, 0, 0, 0, 0);
    if (fd < 0)
        return 0;
    LONG n = host_syscall(19, fd, 0, 2, 0, 0, 0);
    if (n <= 0 || n > 0x20000000) {
        host_syscall(6, fd, 0, 0, 0, 0, 0);
        return 0;
    }
    void *bytes = mapping(0, n, 1, 2, fd);
    host_syscall(6, fd, 0, 0, 0, 0, 0);
    if (!bytes)
        return 0;
    f->bytes = bytes;
    f->size = n;
    f->cookie = bytes;
    return 1;
}
static void close_file(void *opaque, struct w32_file *f) {
    struct host_state *s = opaque;
    host_syscall(91, PTR(f->cookie), round_page(f->size), 0, 0, 0, 0);
    s->closes++;
}
static ULONG tid(void *opaque) {
    (void)opaque;
    return host_syscall(224, 0, 0, 0, 0, 0, 0);
}
static void wait_word(void *opaque, volatile ULONG *p, ULONG value) {
    (void)opaque;
    host_syscall(240, PTR(p), 128, value, 0, 0, 0);
}
static void wake_word(void *opaque, volatile ULONG *p) {
    (void)opaque;
    host_syscall(240, PTR(p), 129, 0x7fffffff, 0, 0, 0);
}
static void diagnostic(void *opaque, const char *event, const char *module, const char *symbol) {
    struct host_state *s = opaque;
    w32_copy_string(s->last_event, sizeof(s->last_event), event);
    w32_copy_string(s->last_module, sizeof(s->last_module), module);
    w32_copy_string(s->last_symbol, sizeof(s->last_symbol), symbol);
    host_print("[win32] ");
    host_print(event);
    host_print(" ");
    host_print(module);
    host_print(" ");
    host_print(symbol);
    host_print("\n");
}
static void continue_context(void *opaque, struct w32_context *c) {
    (void)opaque;
    /* Host stand-in for the kernel's continuation check: user-modifiable
     * EFLAGS bits only (arithmetic, TF, DF, AC); IF stays set. */
    c->EFlags = (c->EFlags & 0x40dd5u) | 0x202u;
    host_resume(c);
}
static void terminate(void *opaque, ULONG code) {
    (void)opaque;
    host_print("TERMINATE ");
    host_number(code);
    host_print("\n");
    host_exit(90);
}
static int invoke(void *opaque, void *proc, void *module, ULONG reason, int dll) {
    struct host_state *s = opaque;
    s->invocations++;
    if (s->observer) {
        host_print("CALL ");
        host_number(PTR(module));
        host_print(" ");
        host_number(reason);
        host_print(dll ? " DLL\n" : " TLS\n");
        return !(s->fail_attach && dll && reason == 1);
    }
    if (dll)
        return ((int(W32API *)(void *, ULONG, void *))proc)(module, reason, 0);
    ((void(W32API *)(void *, ULONG, void *))proc)(module, reason, 0);
    return 1;
}
void host_fs(struct w32_teb *teb) {
    static ULONG entry = 0xffffffffu;
    struct {
        ULONG entry, base, limit, flags;
    } descriptor = {entry, PTR(teb), 8191, 0x41};
    if (host_syscall(243, PTR(&descriptor), 0, 0, 0, 0, 0))
        host_exit(124);
    entry = descriptor.entry;
    uint16_t selector = (entry << 3) | 3;
    __asm__ volatile("movw %0,%%fs" ::"rm"(selector));
    teb->Tib.Self = &teb->Tib;
}
void host_init(void) {
    w32_fill(&host, 0, sizeof(host));
    host.fail_after = -1;
    host.next_allocation = 0x20000000;
    host.services = (struct w32_services){
        &host, allocate,  release,   map,        protect,          unmap,     open_file, close_file,
        tid,   wait_word, wake_word, diagnostic, continue_context, terminate, invoke, 0, 0, 0, 0, 0, 0};
    host.teb.Tib.ExceptionList = (void *)~(uintptr_t)0;
    ULONG esp;
    __asm__ volatile("movl %%esp,%0" : "=r"(esp));
    host.teb.Tib.StackBase = (void *)(uintptr_t)0xffffe000u;
    host.teb.Tib.StackLimit = (void *)(uintptr_t)(esp - 8 * 1024 * 1024);
    host.teb.ClientId.UniqueThread = (void *)(uintptr_t)tid(0);
    host.teb.ClientId.UniqueProcess = (void *)(uintptr_t)host_syscall(20, 0, 0, 0, 0, 0, 0);
    host.teb.ProcessEnvironmentBlock = &host.peb;
    host.peb.ProcessParameters = &host.parameters;
    static WCHAR cwd[] = {'C', ':', '\\', 0};
    RtlInitUnicodeString(&host.parameters.CurrentDirectory.DosPath, cwd);
    host_fs(&host.teb);
    host.dispatcher = (void *)KiUserExceptionDispatcher;
    w32_set_services(&host.services);
}
struct host_ucontext {
    ULONG flags, link;
    struct {
        ULONG sp, flags, size;
    } stack;
    struct sigcontext sc;
};
extern void host_sigreturn(void);
/* Translate a host fault as the contract's exception table prescribes. */
static ULONG fpu_code(ULONG sw) {
    if (sw & 0x40 && sw & 1)
        return 0xc0000092; /* stack check */
    if (sw & 1)
        return 0xc0000090; /* invalid operation */
    if (sw & 4)
        return 0xc000008e; /* divide by zero */
    if (sw & 2)
        return 0xc000008d; /* denormal operand */
    if (sw & 8)
        return 0xc0000091; /* overflow */
    if (sw & 16)
        return 0xc0000093; /* underflow */
    return 0xc000008f;     /* inexact result */
}
static int privileged(const uint8_t *ip) {
    ULONG skip = 0;
    while (skip < 4 && (ip[skip] == 0x66 || ip[skip] == 0xf3 || ip[skip] == 0xf2))
        skip++;
    uint8_t op = ip[skip];
    return op == 0xfa || op == 0xfb || op == 0xf4 || (op >= 0xe4 && op <= 0xe7) ||
           (op >= 0xec && op <= 0xef) || (op >= 0x6c && op <= 0x6f) ||
           (op == 0x0f && (ip[skip + 1] & 0xfc) == 0x20);
}
/* Kernel-like delivery (contract W7): the record and context go on the user
 * stack below the interrupted ESP, then EIP enters the registered dispatcher
 * with the two pointers as stdcall arguments. The handler then returns, and
 * sigreturn starts the dispatcher outside signal context. */
static void signal_handler(int signal, void *info, struct host_ucontext *uc) {
    (void)info;
    struct sigcontext *s = &uc->sc;
    struct w32_context c;
    struct w32_exception r;
    w32_fill(&c, 0, sizeof(c));
    w32_fill(&r, 0, sizeof(r));
    c.ContextFlags = 0x10007;
    c.Edi = s->edi;
    c.Esi = s->esi;
    c.Ebp = s->ebp;
    c.Esp = s->esp;
    c.Ebx = s->ebx;
    c.Edx = s->edx;
    c.Ecx = s->ecx;
    c.Eax = s->eax;
    c.Eip = s->eip;
    c.EFlags = s->eflags;
    c.SegCs = s->cs;
    c.SegSs = s->ss;
    c.SegDs = s->ds;
    c.SegEs = s->es;
    c.SegFs = s->fs;
    c.SegGs = s->gs;
    if (s->fpstate) {
        w32_copy(&c.FloatSave, s->fpstate, 108);
        c.FloatSave.Cr0NpxState = 0;
        c.ContextFlags |= 8;
        if (s->fpstate->magic == 0) {
            w32_copy(c.ExtendedRegisters, s->fpstate->_fxsr_env, 24);
            w32_put32(c.ExtendedRegisters + 24, s->fpstate->mxcsr);
            w32_copy(c.ExtendedRegisters + 32, s->fpstate->_fxsr_st, 128);
            w32_copy(c.ExtendedRegisters + 160, s->fpstate->_xmm, 128);
            c.ContextFlags |= 32;
        }
    }
    if (signal == 4)
        r.ExceptionCode = 0xc000001d;
    else if (signal == 8) {
        if (s->trapno == 0)
            r.ExceptionCode = 0xc0000094;
        else if (s->trapno == 19)
            r.ExceptionCode = 0xc00002b4;
        else
            r.ExceptionCode = fpu_code(s->fpstate ? s->fpstate->sw : 0);
    } else if (signal == 5) {
        if (s->trapno == 3) {
            /* EXCEPTION_BREAKPOINT reports the int3 byte; resume EIP too. */
            r.ExceptionCode = 0x80000003;
            c.Eip--;
            r.NumberParameters = 1;
        } else
            r.ExceptionCode = 0x80000004;
    } else if (signal == 7 && s->trapno == 17)
        r.ExceptionCode = 0x80000002;
    else if (s->trapno == 13 && privileged((const uint8_t *)(uintptr_t)c.Eip))
        r.ExceptionCode = 0xc0000096;
    else {
        r.ExceptionCode = 0xc0000005;
        r.NumberParameters = 2;
        if (s->trapno == 14) {
            r.ExceptionInformation[0] = (s->err >> 1) & 1;
            r.ExceptionInformation[1] = s->cr2;
        } else
            r.ExceptionInformation[1] = 0xffffffffu;
    }
    r.ExceptionAddress = (void *)(uintptr_t)c.Eip;
    struct w32_teb *teb = NtCurrentTeb();
    ULONG frame = ((c.Esp - sizeof(c) - sizeof(r)) & ~3u) - 12;
    if (!host.dispatcher || frame < (ULONG)(uintptr_t)teb->Tib.StackLimit || frame > c.Esp) {
        diagnostic(&host, "crash", "host-signal", "no-dispatch-stack");
        terminate(&host, 0xc00000fd);
    }
    struct w32_context *uc_ctx = (void *)(uintptr_t)(frame + 12);
    struct w32_exception *uc_rec = (void *)(uintptr_t)(frame + 12 + sizeof(c));
    w32_copy(uc_ctx, &c, sizeof(c));
    w32_copy(uc_rec, &r, sizeof(r));
    ULONG *args = (ULONG *)(uintptr_t)frame;
    args[0] = 0; /* the dispatcher never returns */
    args[1] = (ULONG)(uintptr_t)uc_rec;
    args[2] = (ULONG)(uintptr_t)uc_ctx;
    s->esp = frame;
    s->eip = (ULONG)(uintptr_t)host.dispatcher;
    s->eflags &= ~(0x100u | 0x400u | 0x40000u); /* TF, DF, AC clear */
}
void host_signals(void) {
    struct {
        void *handler;
        ULONG flags;
        void *restorer;
        ULONG mask[2];
    } action = {signal_handler, 0x44000004, host_sigreturn, {0, 0}};
    ULONG signals[] = {4, 5, 7, 8, 11};
    for (ULONG i = 0; i < 5; i++)
        if (host_syscall(174, signals[i], PTR(&action), 0, 8, 0, 0))
            host_exit(125);
}
void host_start(ULONG *stack) {
    host_init();
    host_signals();
    host_exit(host_main(stack[0], (char **)(stack + 1)));
}
