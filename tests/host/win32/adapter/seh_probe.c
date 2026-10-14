/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Self-authored x86 SEH probe.
 *
 * Runs unchanged on Wine (behavioural oracle) and on the Linux i386 adapter
 * with the PE ntdll. Returns 0 when every check passes, otherwise
 * 100 * test + check. No Windows SDK, CRT, compiler SEH helpers, payloads or
 * copied Wine tests: registrations are pushed by hand on FS:[0]. */
typedef unsigned long U32;
struct rec {
    U32 code, flags;
    struct rec *next;
    void *address;
    U32 count, info[15];
};
struct reg {
    struct reg *prev;
    void *handler;
};
#define IMPORT __declspec(dllimport)
IMPORT void __stdcall RtlRaiseException(struct rec *);
IMPORT void __stdcall RtlUnwind(void *, void *, struct rec *, void *);
IMPORT void *__stdcall NtCurrentTeb(void);
extern char __ImageBase;
#define CTX_EIP(c) (*(U32 *)((c) + 184))
#define CTX_ESP(c) (*(U32 *)((c) + 196))
enum { CONTINUE_EXECUTION, CONTINUE_SEARCH };
typedef U32(__cdecl *handler_fn)(struct rec *, struct reg *, unsigned char *, void *);

U32 saved_esp, saved_ebp;
/* int guarded(void (*body)(void), handler_fn outer): registers `outer` on
 * FS:[0], runs body; returns 0 when body returns, 7 when a handler escaped to
 * the landing pad after RtlUnwind (the __except pattern). */
__asm__(".text\n"
        ".globl _guarded\n"
        "_guarded:\n"
        "  pushl %ebp\n  movl %esp,%ebp\n  pushl %ebx\n  pushl %esi\n  pushl %edi\n"
        "  pushl 12(%ebp)\n  pushl %fs:0\n  movl %esp,%fs:0\n"
        "  movl %esp,_saved_esp\n  movl %ebp,_saved_ebp\n"
        "  call *8(%ebp)\n"
        "  xorl %eax,%eax\n  jmp 1f\n"
        ".globl _landing\n"
        "_landing:\n"
        "  movl _saved_esp,%esp\n  movl _saved_ebp,%ebp\n  movl $7,%eax\n"
        "1:\n  popl %fs:0\n  addl $4,%esp\n"
        "  popl %edi\n  popl %esi\n  popl %ebx\n  popl %ebp\n  ret\n"
        /* int fault_registers(int kind): fault with known nonvolatile values;
         * the handler skips the 5-byte instruction; returns 1 if preserved. */
        ".globl _fault_registers\n"
        "_fault_registers:\n"
        "  pushl %ebx\n  pushl %esi\n  pushl %edi\n  pushl %ebp\n"
        "  movl $0x11111111,%ebx\n  movl $0x22222222,%esi\n  movl $0x33333333,%edi\n"
        "  movl $0x44444444,%ebp\n  movl $0x55555555,%eax\n"
        "  cmpl $0,20(%esp)\n  jne 2f\n"
        "  .byte 0xa1,0,0,0,0\n" /* movl 0x0,%eax: read fault */
        "  jmp 3f\n"
        "2:\n  .byte 0xa3,0,0,0,0\n" /* movl %eax,0x0: write fault */
        "3:\n  xorl %eax,%eax\n"
        "  cmpl $0x11111111,%ebx\n  jne 4f\n  cmpl $0x22222222,%esi\n  jne 4f\n"
        "  cmpl $0x33333333,%edi\n  jne 4f\n  cmpl $0x44444444,%ebp\n  jne 4f\n"
        "  incl %eax\n"
        "4:\n  popl %ebp\n  popl %edi\n  popl %esi\n  popl %ebx\n  ret\n"
        ".globl _do_int3\n_do_int3:\n  int3\n  ret\n"
        ".globl _do_divide\n_do_divide:\n  xorl %edx,%edx\n  movl $1,%eax\n"
        "  xorl %ecx,%ecx\n  divl %ecx\n  ret\n"
        ".globl _do_ud2\n_do_ud2:\n  ud2\n  ret\n"
        ".globl _do_cli\n_do_cli:\n  cli\n  ret\n");
int guarded(void (*)(void), handler_fn);
int fault_registers(int);
void landing(void);
void do_int3(void), do_divide(void), do_ud2(void), do_cli(void);

static struct reg *fs0(void) {
    struct reg *r;
    __asm__ volatile("movl %%fs:0,%0" : "=r"(r));
    return r;
}
static void set_fs0(struct reg *r) {
    __asm__ volatile("movl %0,%%fs:0" ::"r"(r) : "memory");
}
static volatile U32 failed, calls, inner_search, inner_unwind, observed, unwound_to_frame;
#define CHECK(c, n)                                                                                \
    do {                                                                                           \
        if (!(c) && !failed)                                                                       \
            failed = (n);                                                                          \
    } while (0)
/* The __except pattern: unwind to our own frame, then leave through the
 * landing pad, which restores the guarded() frame. */
static __attribute__((noreturn)) void escape(struct reg *frame, struct rec *r) {
    RtlUnwind(frame, 0, r, 0);
    unwound_to_frame = fs0() == frame;
    __asm__ volatile("jmp _landing");
    __builtin_unreachable();
}
/* Known oracle differences are accepted only on Wine, detected by the
 * wine_get_version export of the ntdll found through PEB->Ldr. */
static int on_wine;
static int same(const char *a, const char *b) {
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static int detect_wine(void) {
    unsigned char *peb = *(unsigned char **)((unsigned char *)NtCurrentTeb() + 0x30);
    /* PEB_LDR_DATA.InLoadOrderModuleList; struct reg doubles as LIST_ENTRY
     * (prev = Flink). BaseDllName.Buffer is at 0x30, DllBase at 0x18. */
    struct reg *head = (struct reg *)(*(unsigned char **)(peb + 0x0c) + 0x0c);
    for (struct reg *e = head->prev; e != head; e = e->prev) {
        unsigned char *base = *(unsigned char **)((unsigned char *)e + 0x18);
        const unsigned short *name = *(unsigned short **)((unsigned char *)e + 0x30);
        if (!name || (name[0] | 0x20) != 'n' || (name[1] | 0x20) != 't' || (name[2] | 0x20) != 'd')
            continue;
        unsigned char *nt = base + *(U32 *)(base + 60);
        U32 rva = *(U32 *)(nt + 120), count = rva ? *(U32 *)(base + rva + 24) : 0;
        U32 *names = rva ? (U32 *)(base + *(U32 *)(base + rva + 32)) : 0;
        for (U32 i = 0; i < count; i++)
            if (same((const char *)base + names[i], "wine_get_version"))
                return 1;
        return 0;
    }
    return 0;
}
static int in_image(void *p) {
    return (char *)p > &__ImageBase && (char *)p < &__ImageBase + 0x100000;
}

/* 1: continuable software exception, continue at the raise site. */
static U32 __cdecl h1(struct rec *r, struct reg *f, unsigned char *c, void *d) {
    (void)f;
    (void)d;
    calls++;
    CHECK(r->code == 0xe0000001 && r->flags == 0 && r->count == 2, 101);
    CHECK(r->info[0] == 0x44 && r->info[1] == 0x55, 102);
    CHECK(in_image(r->address) && CTX_EIP(c) == (U32)r->address, 103);
    return CONTINUE_EXECUTION;
}
static int test1(void) {
    struct reg f = {fs0(), (void *)h1};
    set_fs0(&f);
    struct rec r = {0xe0000001, 0, 0, 0, 2, {0x44, 0x55}};
    volatile U32 marker = 0x1234;
    RtlRaiseException(&r);
    set_fs0(f.prev);
    CHECK(marker == 0x1234, 104);
    CHECK(calls == 1, 105);
    return 0;
}

/* 2: a handler continuing a noncontinuable exception gets
 * STATUS_NONCONTINUABLE_EXCEPTION linked to the original. */
static U32 __cdecl h2(struct rec *r, struct reg *f, unsigned char *c, void *d) {
    (void)c;
    (void)d;
    if (r->flags & 6)
        return CONTINUE_SEARCH;
    calls++;
    if (r->code == 0xe0000002) {
        CHECK(r->flags == 1, 201);
        return CONTINUE_EXECUTION;
    }
    CHECK(r->code == 0xc0000025, 202);
    CHECK(r->flags & 1, 203);
    /* Windows chains the original record; Wine 11.18 passes NULL. */
    CHECK(r->next ? r->next->code == 0xe0000002 : on_wine, 204);
    escape(f, r);
}
static void body2(void) {
    struct rec r = {0xe0000002, 1, 0, 0, 0, {0}};
    RtlRaiseException(&r);
    failed = failed ? failed : 205; /* must not return */
}

/* 3: unwinding calls the inner handler with EXCEPTION_UNWINDING and stops at
 * the target frame, leaving it installed. */
static U32 __cdecl h3_inner(struct rec *r, struct reg *f, unsigned char *c, void *d) {
    (void)f;
    (void)c;
    (void)d;
    if (r->flags & 2) {
        inner_unwind++;
        CHECK(!(r->flags & 4), 301);
    } else
        inner_search++;
    return CONTINUE_SEARCH;
}
static U32 __cdecl h3(struct rec *r, struct reg *f, unsigned char *c, void *d) {
    (void)c;
    (void)d;
    if (r->flags & 6) {
        CHECK(0, 302); /* the target frame is never unwound */
        return CONTINUE_SEARCH;
    }
    CHECK(r->code == 0xe0000003 && inner_search == 1 && !inner_unwind, 303);
    escape(f, r);
}
static void body3(void) {
    struct reg inner = {fs0(), (void *)h3_inner};
    set_fs0(&inner);
    struct rec r = {0xe0000003, 0, 0, 0, 0, {0}};
    RtlRaiseException(&r);
    failed = failed ? failed : 304;
}

/* 4: hardware faults with the contract's codes and information. */
static U32 expected_code, expected_write, skip_bytes, eip_offset;
static U32 __cdecl h4(struct rec *r, struct reg *f, unsigned char *c, void *d) {
    (void)f;
    (void)d;
    calls++;
    observed = r->code;
    if (r->code == 0xc0000005) {
        CHECK(r->count == 2 && r->info[0] == expected_write && r->info[1] == 0, 401);
    }
    eip_offset = CTX_EIP(c) - (U32)r->address;
    CHECK(in_image(r->address), 402);
    CTX_EIP(c) = (U32)r->address + skip_bytes;
    return r->code == expected_code ? CONTINUE_EXECUTION : CONTINUE_SEARCH;
}
static int fault(void (*fn)(void), U32 code, U32 skip, U32 base) {
    struct reg f = {fs0(), (void *)h4};
    set_fs0(&f);
    calls = 0;
    expected_code = code;
    skip_bytes = skip;
    fn();
    set_fs0(f.prev);
    CHECK(calls == 1, base + 3);
    CHECK(observed == code, base + 4);
    return 0;
}
static int test4(void) {
    for (U32 write = 0; write < 2; write++) {
        struct reg f = {fs0(), (void *)h4};
        set_fs0(&f);
        calls = 0;
        expected_code = 0xc0000005;
        expected_write = write;
        skip_bytes = 5;
        int ok = fault_registers(write);
        set_fs0(f.prev);
        CHECK(ok == 1, 405 + write);
        CHECK(calls == 1, 407 + write);
    }
    return 0;
}

/* 6: an exception raised inside a handler during the search pass: the frames
 * up to the protected one see EXCEPTION_NESTED_CALL, outer frames do not. */
static U32 nested_inner_flags, nested_outer_flags;
static U32 __cdecl h6_inner(struct rec *r, struct reg *f, unsigned char *c, void *d) {
    (void)f;
    (void)c;
    (void)d;
    if (r->flags & 6)
        return CONTINUE_SEARCH;
    if (r->code == 0xe0000004) {
        struct rec second = {0xe0000005, 0, 0, 0, 0, {0}};
        RtlRaiseException(&second);
        return CONTINUE_SEARCH;
    }
    if (r->code == 0xe0000005)
        nested_inner_flags = r->flags | 0x80000000u;
    return CONTINUE_SEARCH;
}
static U32 __cdecl h6(struct rec *r, struct reg *f, unsigned char *c, void *d) {
    (void)c;
    (void)d;
    if (r->flags & 6)
        return CONTINUE_SEARCH;
    if (r->code == 0xe0000005) {
        nested_outer_flags = r->flags | 0x80000000u;
        return CONTINUE_EXECUTION;
    }
    CHECK(r->code == 0xe0000004, 601);
    escape(f, r);
}
static void body6(void) {
    struct reg inner = {fs0(), (void *)h6_inner};
    set_fs0(&inner);
    struct rec r = {0xe0000004, 0, 0, 0, 0, {0}};
    RtlRaiseException(&r);
    failed = failed ? failed : 602;
}

int entry(void) {
    struct reg *top = fs0();
    void *teb = NtCurrentTeb();
    U32 self;
    __asm__ volatile("movl %%fs:0x18,%0" : "=r"(self));
    if (!teb || self != (U32)teb)
        return 1;
    on_wine = detect_wine();
    test1();
    if (failed)
        return failed;
    calls = 0;
    CHECK(guarded(body2, h2) == 7 && unwound_to_frame && calls == 2, 206);
    if (failed)
        return failed;
    CHECK(guarded(body3, h3) == 7 && unwound_to_frame && inner_unwind == 1, 305);
    if (failed)
        return failed;
    test4();
    if (failed)
        return failed;
    fault(do_int3, 0x80000003, 1, 500);
    CHECK(eip_offset == 0, 510); /* CONTEXT.Eip and ExceptionAddress at int3 */
    fault(do_divide, 0xc0000094, 2, 520);
    fault(do_ud2, 0xc000001d, 2, 530);
    fault(do_cli, 0xc0000096, 1, 540);
    if (failed)
        return failed;
    CHECK(guarded(body6, h6) == 7, 603);
    CHECK(nested_inner_flags == (0x80000000u | 0x10), 604);
    CHECK(nested_outer_flags == 0x80000000u, 605);
    CHECK(fs0() == top, 900);
    return failed;
}
