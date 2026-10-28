/* SPDX-License-Identifier: LGPL-2.1-or-later
 * H1: one reused Wine upper DLL loaded by the production loader on the i386
 * Linux adapter, its whole import closure bound to the CiukiOS core PEs, and
 * every DllMain executed (attach, then detach on free). The PE ntdll is the
 * adapter build (fail-closed CIUKI_SYS_W32 transport): this is not the
 * CiukiOS kernel. The default mode refuses unimplemented kernel/w32srv
 * requests; the opt-in H1_INSTALLERS mode uses the fixture services below.
 *
 * Safety: PE code never reaches a Linux system call. A seccomp filter allows
 * system calls only from the adapter ELF's own text; any int 0x80 from PE
 * code (the CiukiOS gate) returns -ENOSYS. The CiukiOS user gate page
 * (0x7ffe0100, used by USER/ADVAPI32) is a host stub counting calls and
 * returning -ENOSYS.
 *
 * Default adapter answers (h1_transport.c in the PE ntdll): the kernel's W32_IDENTITY
 * record as src/kernel/proc/win32/records.c answers it (Windows 98, 4.10.2222
 * " A "), and the w32srv atom directory (ATOM_ADD/FIND/DELETE/NAME, the
 * apps/w32srv/directory.c rules: integer atoms #1..#0xbfff, string atoms from
 * 0xc000, case-insensitive names, global or per-process scope, reference
 * counts). Every other kernel record is ENOSYS and every other w32srv
 * operation fails with 120; both are counted and reported. */
#include "linux.h"
#include <ciuki/abi.h>
#include <ciuki/w32_core.h>
#include <w32/server.h>
#include "upper_h1_store.h"
#include <stdarg.h>
#ifdef H1_INSTALLERS
#include "h1_services.h"
static int installer_exec;
static int installer_fixture;
static int game_exec; /* f7: installer-exec plus the h1_game.c desktop, queue and display */
#endif
/* f5-20D: the guest fixture's logic (src/probes/win32_upper/exercise.c, the
 * same source that is linked as upper-exercise.exe) on the adapter. The test
 * writes upper_fixture_src.h (an include of that source) and upper_table.h
 * (scripts/win32/upper_fixture.py table_header) next to this file. */
#define W32_UPPER_HOST 1
#include "upper_fixture_src.h"
#include "upper_table.h"

static struct w32_loader loader;
static int (*base_invoke)(void *, void *, void *, uint32_t, int);
static volatile ULONG gate_calls;
static ULONG attach_true, attach_false, detach_calls;
extern char __ehdr_start[], etext[];

static void print_hex(ULONG v) {
    char t[11] = "0x00000000";
    for (int i = 9; i >= 2; i--, v >>= 4)
        t[i] = "0123456789abcdef"[v & 15];
    host_print(t);
}

static const char *module_name(void *base) {
    for (unsigned i = 0; i < sizeof(h1_store) / sizeof(h1_store[0]); i++)
        if (h1_store[i].base == (ULONG)(uintptr_t)base)
            return h1_store[i].name;
    return "?";
}

static int logged_invoke(void *opaque, void *proc, void *module, uint32_t reason, int dll) {
    ULONG before = gate_calls;
    host_print(dll ? "DLLMAIN " : "TLS ");
    host_print(module_name(module));
    host_print(" reason=");
    host_number(reason);
    host_print(" enter\n");
    int r = base_invoke(opaque, proc, module, reason, dll);
    host_print(dll ? "DLLMAIN " : "TLS ");
    host_print(module_name(module));
    host_print(" reason=");
    host_number(reason);
    host_print(" return=");
    host_number((ULONG)r);
    host_print(" gate=");
    host_number(gate_calls - before);
    host_print("\n");
    if (dll && reason == 1)
        r ? attach_true++ : attach_false++;
#ifdef H1_INSTALLERS
    if (dll && reason == 1 && r && game_exec)
        h1_game_module(module_name(module), module); /* f7: GL call profile of the game mode */
#endif
    if (dll && reason == 0)
        detach_calls++;
    return r;
}

/* Allow system calls only from the adapter ELF text (host_syscall, the
 * signal restorer). Everything else gets -ENOSYS without a signal. */
#ifdef H1_INSTALLERS
#include <asm/sigcontext.h>
struct h1_ucontext {
    ULONG flags, link;
    struct {
        ULONG sp, flags, size;
    } stack;
    struct sigcontext sc;
};
extern void host_sigreturn(void);
/* fx-hlrun: SIGSYS from SECCOMP_RET_TRAP. siginfo's _sigsys.syscall (word
 * 4) is the CiukiOS call number; the arguments are still in ebx, ecx, edx
 * and esi; the result goes to eax and the PE resumes after its int 0x80. */
static void sys_handler(int signal, const ULONG *info, struct h1_ucontext *uc) {
    (void)signal;
    struct sigcontext *s = &uc->sc;
    int32_t result = -38;
    if (!h1_game_syscall(info[4], s->ebx, s->ecx, s->edx, s->esi, &result))
        result = -38;
    s->eax = (ULONG)result;
}
static int sys_trap(void) {
    struct {
        void *handler;
        ULONG flags;
        void *restorer;
        ULONG mask[2];
    } action = {(void *)sys_handler, 0x44000004, host_sigreturn, {0, 0}};
    return !host_syscall(174, 31 /* SIGSYS */, (ULONG)(uintptr_t)&action, 0, 8, 0, 0);
}
#endif
static int sandbox(void) {
    struct {
        uint16_t code;
        uint8_t jt, jf;
        uint32_t k;
    } filter[] = {
        {0x20, 0, 0, 4},                                   /* A = arch */
        {0x15, 0, 4, 0x40000003u},                         /* i386 */
        {0x20, 0, 0, 8},                                   /* A = IP */
        {0x35, 0, 2, (uint32_t)(uintptr_t)__ehdr_start},   /* IP >= ELF start */
        {0x35, 1, 0, (uint32_t)(uintptr_t)etext},          /* IP < etext */
        {0x06, 0, 0, 0x7fff0000u},                         /* allow */
        {0x06, 0, 0, 0x00050000u | 38u},                   /* ERRNO ENOSYS */
    };
#ifdef H1_INSTALLERS
    /* fx-hlrun: in game mode a PE int 0x80 traps (SIGSYS) into the
     * adapter's emulation of the CiukiOS calls it serves (h1_game_syscall:
     * the /dev/media CD drive); everything else still gets -ENOSYS and no
     * PE call ever reaches Linux. */
    if (game_exec)
        filter[6].k = 0x00030000u; /* SECCOMP_RET_TRAP */
#endif
    struct {
        uint16_t len;
        void *filter;
    } program = {sizeof(filter) / sizeof(filter[0]), filter};
    if (host_syscall(172, 38, 1, 0, 0, 0, 0))          /* PR_SET_NO_NEW_PRIVS */
        return 0;
    return !host_syscall(172, 22, 2, (ULONG)(uintptr_t)&program, 0, 0, 0); /* SECCOMP_MODE_FILTER */
}

static ULONG kernel_records, kernel_enosys, server_requests, server_unsupported, console_writes;
#ifdef H1_INSTALLERS
static struct {ULONG op,action,size;} recent_kernel[32];
static struct {ULONG op,key,status;} recent_server[16];
#endif
/* Exec mode (Wine conformance programs): the process's standard handles are
 * a console the adapter answers itself (write to stdout, empty input). */
#define H1_STDIN 0x40000010u
#define H1_STDOUT 0x40000014u
#define H1_STDERR 0x40000018u
static ULONG enosys_ops[16][2];

static void count_enosys(ULONG op) {
    for (unsigned i = 0; i < 16; i++)
        if (enosys_ops[i][0] == op || !enosys_ops[i][1]) {
            enosys_ops[i][0] = op;
            enosys_ops[i][1]++;
            return;
        }
}

static int32_t h1_console(struct ciuki_w32_file_args *a) {
    if (a->handle != H1_STDIN && a->handle != H1_STDOUT && a->handle != H1_STDERR)
        return 0;
    a->error = 0;
    a->result = 0;
    if (a->action == W32_F_WRITE && a->handle != H1_STDIN) {
        console_writes++;
        host_syscall(4, 1, a->buffer, a->bytes, 0, 0, 0);
        a->result = a->bytes;
    } else if (a->action == W32_F_STAT && a->buffer && a->bytes)
        w32_fill((void *)(uintptr_t)a->buffer, 0, a->bytes);
    else if (a->action != W32_F_READ)
        a->error = 1; /* ERROR_INVALID_FUNCTION on the console */
    return 1;
}

static int32_t h1_kernel(void *record, uint32_t size) {
    struct ciuki_w32_header *h = record;
#ifdef H1_INSTALLERS
    recent_kernel[kernel_records%32].op=h->operation;
    recent_kernel[kernel_records%32].size=size;
    recent_kernel[kernel_records%32].action=
        (h->operation==W32_FILE || h->operation==W32_OBJECT || h->operation==W32_VIEW) && size>=20?
        ((const ULONG *)record)[4]:0;
#endif
    kernel_records++;
    if (h->operation == W32_IDENTITY && size >= sizeof(struct ciuki_w32_identity)) {
        struct ciuki_w32_identity *a = record;
        w32_fill((uint8_t *)a + 16, 0, sizeof(*a) - 16);
        a->major = 4;
        a->minor = 10;
        a->build = 2222;
        a->platform = 1;
        a->get_version = 0x80000000u | (2222u << 16) | (10u << 8) | 4;
        a->directx = 0x0007000a;
        w32_copy(a->csd, " A ", 4);
        return 0;
    }
    if (h->operation == W32_FILE && size >= sizeof(struct ciuki_w32_file_args) && h1_console(record))
        return 0;
#ifdef H1_INSTALLERS
    int32_t status;
    if (game_exec && (h1_game_kernel(record, size, &status) || h1_installer_kernel(record, size, &status)))
        return status; /* game mode: no per-record trace (thousands per second) */
    if ((installer_exec || installer_fixture) && h1_installer_kernel(record, size, &status)){
        host_print("KERNEL op=");print_hex(h->operation);
        host_print(" action=");host_number(recent_kernel[(kernel_records-1)%32].action);
        host_print(" status=");print_hex((ULONG)status);
        if(size>=sizeof(struct ciuki_w32_vm) &&
           (h->operation==W32_VM_ALLOC || h->operation==W32_VM_FREE || h->operation==W32_VM_PROTECT)){
            const struct ciuki_w32_vm *a=record;host_print(" address=");print_hex(a->address);
            host_print(" bytes=");host_number(a->bytes);host_print(" flags=");print_hex(a->flags);
        }
        host_print("\n");
        return status;
    }
#endif
    kernel_enosys++;
    count_enosys(h->operation);
    host_print("KERNEL ENOSYS op=");
    print_hex(h->operation);
    host_print(" size=");
    host_number(size);
    host_print("\n");
    return -38;
}

static struct {
    ULONG id, scope, refs;
    char name[256];
} atoms[256];
static ULONG next_atom = 0xc000;

static int same_name(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

static ULONG integer_atom(const char *s) {
    ULONG n = 0;
    if (*s++ != '#' || !*s)
        return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n >= 0xc000)
            return 0;
        n = n * 10 + (ULONG)(*s - '0');
    }
    return n < 0xc000 ? n : 0;
}

static ULONG atom_call(const struct w32r_request *r, const char *name, struct w32r_reply *out, uint8_t *bytes,
                       uint32_t room) {
    if (r->index > 1)
        return 87;
    ULONG scope = r->index ? 1 : 0;
    int by_name = r->op == W32R_ATOM_ADD || r->op == W32R_ATOM_FIND;
    ULONG integer = by_name ? integer_atom(name) : r->key;
    if (integer && integer < 0xc000) {
        out->key = integer;
        return 0;
    }
    int slot = -1;
    for (unsigned i = 0; i < sizeof(atoms) / sizeof(atoms[0]); i++)
        if (atoms[i].refs && atoms[i].scope == scope &&
            (by_name ? same_name(atoms[i].name, name) : atoms[i].id == r->key))
            slot = (int)i;
    if (r->op == W32R_ATOM_ADD) {
        if (!*name || w32_strlen(name) >= sizeof(atoms[0].name))
            return 87;
        if (slot < 0) {
            for (unsigned i = 0; i < sizeof(atoms) / sizeof(atoms[0]) && slot < 0; i++)
                if (!atoms[i].refs)
                    slot = (int)i;
            if (slot < 0 || next_atom > 0xffff)
                return 8;
            atoms[slot].id = next_atom++;
            atoms[slot].scope = scope;
            w32_copy(atoms[slot].name, name, w32_strlen(name) + 1);
        }
        atoms[slot].refs++;
        out->key = atoms[slot].id;
        return 0;
    }
    if (slot < 0)
        return 6;
    out->key = atoms[slot].id;
    if (r->op == W32R_ATOM_DELETE)
        atoms[slot].refs--;
    else if (r->op == W32R_ATOM_NAME) {
        ULONG n = w32_strlen(atoms[slot].name) + 1;
        if (n > room)
            return 122;
        w32_copy(bytes, atoms[slot].name, n);
        out->data_bytes = n;
    }
    return 0;
}

static uint32_t h1_server(const void *in, uint32_t n, void *out, uint32_t cap, uint32_t *used) {
#ifdef H1_INSTALLERS
    if (installer_exec || installer_fixture){
        unsigned slot=server_requests++%16;
        const struct w32r_request *request=in;
        recent_server[slot].op=n>=sizeof(*request)?request->op:0;
        recent_server[slot].key=n>=sizeof(*request)?request->key:0;
        uint32_t status=h1_installer_server(in,n,out,cap,used);
        recent_server[slot].status=status;
        if(!status && *used>=sizeof(struct w32r_reply))
            recent_server[slot].status=((const struct w32r_reply *)out)->error;
        return status;
    }
#endif
    const struct w32r_request *r = in;
    struct w32r_reply *reply = out;
    server_requests++;
    if (n < sizeof(*r) || cap < sizeof(*reply) || r->version != 1 ||
        sizeof(*r) + r->path_bytes + r->name_bytes + r->data_bytes != n || !r->name_bytes)
        return 87;
    const char *name = (const char *)(r + 1) + r->path_bytes;
    if (name[r->name_bytes - 1])
        return 87;
    w32_fill(reply, 0, sizeof(*reply));
    reply->version = 1;
    if (r->op >= W32R_ATOM_ADD && r->op <= W32R_ATOM_NAME)
        reply->error = atom_call(r, name, reply, (uint8_t *)(reply + 1), cap - sizeof(*reply));
    else {
        server_unsupported++;
        reply->error = 120;
    }
    if (reply->error)
        reply->data_bytes = 0;
    *used = sizeof(*reply) + reply->data_bytes;
    return 0;
}

#ifdef H1_INSTALLERS
/* f5-15B: the USER gate stays a refusing stub (-ENOSYS), but the installer
 * runs record which gate numbers (CIUKI_SYS_*) the PE attempted, so the
 * acceptance table names the first unavailable adapter service exactly.
 * Called from the gate page with the number in eax; no system call here. */
static ULONG gate_ops[16][2];
static ULONG __attribute__((cdecl, used)) h1_gate_log(ULONG number, ULONG a, ULONG b, ULONG c) {
    int32_t handled;
    if (game_exec && h1_game_gate(number, a, b, c, &handled))
        return (ULONG)handled;
    gate_calls++;
    for (unsigned i = 0; i < 16; i++)
        if (!gate_ops[i][1] || gate_ops[i][0] == number) {
            gate_ops[i][0] = number;
            gate_ops[i][1]++;
            break;
        }
    return (ULONG)-38;
}
#endif

static int gate_page(void) {
    LONG page = host_syscall(192, 0x7ffe0000u, 4096, 3, 0x32, -1, 0);
    if (page != (LONG)0x7ffe0000u)
        return 0;
    uint8_t *gate = (uint8_t *)(uintptr_t)0x7ffe0100u;
#ifdef H1_INSTALLERS
    /* push ecx; push edx; push edx; push ecx; push ebx; push eax;
     * call h1_gate_log(eax, ebx, ecx, edx); add esp,16; pop edx; pop ecx;
     * ret -> eax = its result (-38 unless the f7 game mode serves it) */
    ULONG rel = (ULONG)(uintptr_t)h1_gate_log - (0x7ffe0100u + 11);
    const uint8_t code[] = {0x51, 0x52, 0x52, 0x51, 0x53, 0x50, 0xe8, rel, rel >> 8, rel >> 16, rel >> 24,
                            0x83, 0xc4, 0x10, 0x5a, 0x59, 0xc3};
#else
    ULONG counter = (ULONG)(uintptr_t)&gate_calls;
    /* incl counter; movl $-38,%eax; ret */
    const uint8_t code[] = {0xff, 0x05, counter, counter >> 8, counter >> 16, counter >> 24,
                            0xb8, 0xda, 0xff, 0xff, 0xff, 0xc3};
#endif
    for (unsigned i = 0; i < sizeof(code); i++)
        gate[i] = code[i];
    void **hooks = (void **)(uintptr_t)0x7ffe0200u;
    hooks[0] = (void *)h1_kernel;
    hooks[1] = (void *)h1_server;
    return !host_syscall(125, 0x7ffe0000u, 4096, 5, 0, 0, 0);
}

static void print_stats(void) {
#ifdef H1_INSTALLERS
    h1_game_report();
#endif
    host_print("STATS kernel_records=");
    host_number(kernel_records);
    host_print(" kernel_enosys=");
    host_number(kernel_enosys);
    host_print(" server_requests=");
    host_number(server_requests);
    host_print(" server_unsupported=");
    host_number(server_unsupported);
    host_print(" console_writes=");
    host_number(console_writes);
    host_print(" gate=");
    host_number(gate_calls);
    for (unsigned i = 0; i < 16 && enosys_ops[i][1]; i++) {
        host_print(" enosys:");
        print_hex(enosys_ops[i][0]);
        host_print("=");
        host_number(enosys_ops[i][1]);
    }
#ifdef H1_INSTALLERS
    for (unsigned i = 0; i < 16 && gate_ops[i][1]; i++) {
        host_print(" gate:");
        host_number(gate_ops[i][0]);
        host_print("=");
        host_number(gate_ops[i][1]);
    }
#endif
    host_print("\n");
}

#ifdef H1_INSTALLERS
/* Keep the original fault context: the terse diagnostic alone loses both
 * the failing instruction and its callers. Read only the owned PE stack. */
static void installer_report(void *opaque, uint32_t action, uint32_t reason,
                             struct w32_context *c, const char *name, uint32_t code) {
    (void)opaque;(void)name;(void)code;
    host_print("REPORT action=");host_number(action);
    host_print(" reason=");print_hex(reason);
    if(c){
        host_print(" eip=");print_hex(c->Eip);
        host_print(" esp=");print_hex(c->Esp);
        host_print(" ebp=");print_hex(c->Ebp);
        host_print(" eax=");print_hex(c->Eax);
        host_print(" ecx=");print_hex(c->Ecx);
        host_print(" edx=");print_hex(c->Edx);
        host_print(" stack=");print_hex((ULONG)(uintptr_t)host.teb.Tib.StackLimit);
        host_print("-");print_hex((ULONG)(uintptr_t)host.teb.Tib.StackBase);
        host_print("\nSTACK");
        uintptr_t at=c->Esp,low=(uintptr_t)host.teb.Tib.StackLimit,high=(uintptr_t)host.teb.Tib.StackBase;
        for(unsigned i=0;i<32 && at>=low && at<=high && high-at>=4;i++,at+=4){
            host_print(" ");print_hex(*(const uint32_t *)at);
        }
    }
    host_print("\n");
    for(ULONG i=kernel_records>32?kernel_records-32:0;i<kernel_records;i++){
        unsigned slot=i%32;host_print("KERNEL recent=");host_number(i+1);
        host_print(" op=");print_hex(recent_kernel[slot].op);
        host_print(" action=");host_number(recent_kernel[slot].action);
        host_print(" size=");host_number(recent_kernel[slot].size);host_print("\n");
    }
    for(ULONG i=server_requests>16?server_requests-16:0;i<server_requests;i++){
        unsigned slot=i%16;host_print("SERVER recent=");host_number(i+1);
        host_print(" op=");host_number(recent_server[slot].op);
        host_print(" key=");print_hex(recent_server[slot].key);
        host_print(" status=");host_number(recent_server[slot].status);host_print("\n");
    }
}
#endif

static void (*base_terminate)(void *, uint32_t);
static void stats_terminate(void *opaque, uint32_t code) {
    print_stats();
#ifdef H1_INSTALLERS
    if (installer_exec) {
        host_print("TERMINATE ");
        host_number(code);
        host_print("\n");
        host_exit(code);
    }
#endif
    base_terminate(opaque, code);
}

/* Exactly as the production services (services/ciuki.c unmap): builtin
 * views are not released by the loader, since ExitProcess still runs in
 * KERNEL32 and NTDLL while it detaches every DLL; the kernel releases them
 * after the thread has left user mode. */
static void (*base_unmap)(void *, void *, uint32_t);
static void exit_unmap(void *opaque, void *p, uint32_t n) {
    uintptr_t address = (uintptr_t)p;
    if (address >= CIUKI_W32_APP_LIMIT && address < CIUKI_W32_BUILTIN_LIMIT)
        return;
    base_unmap(opaque, p, n);
}

static WCHAR command_line[512];

static int run_exe(const char *exe, const char *test) {
    ULONG n = 0;
    for (const char *s = exe; *s && n < 400; s++)
        command_line[n++] = (uint8_t)*s;
    command_line[n++] = ' ';
    for (const char *s = test; *s && n < 510; s++)
        command_line[n++] = (uint8_t)*s;
    command_line[n] = 0;
    RtlInitUnicodeString(&host.parameters.CommandLine, command_line);
#ifdef H1_INSTALLERS
    if (installer_exec)
        h1_installer_parameters(test);
#endif
    host.parameters.StandardInput = (void *)(uintptr_t)H1_STDIN;
    host.parameters.StandardOutput = (void *)(uintptr_t)H1_STDOUT;
    host.parameters.StandardError = (void *)(uintptr_t)H1_STDERR;
    base_terminate = host.services.terminate;
    host.services.terminate = stats_terminate;
    base_unmap = host.services.unmap;
    host.services.unmap = exit_unmap;
    struct w32_module *m;
#ifdef H1_INSTALLERS
    /* f5-15B: the same DirectSetup admission as sdk/win32/services/start.c,
     * before import binding or any callback of the redistributable. */
    if (installer_exec && w32_installer_intercept(&loader, exe)) {
        host_print("RESULT intercepted dxsetup\n");
        host_print("TERMINATE 0\n");
        return 0;
    }
#endif
    NTSTATUS start = w32_ldr_start_process(&loader, exe, &m);
    if (start) {
        host_print("REFUSE ");
        host_print(loader.error.reason ? loader.error.reason : "-");
        host_print(" ");
        host_print(loader.error.module[0] ? loader.error.module : "-");
        host_print(" ");
        host_print(loader.error.symbol[0] ? loader.error.symbol : "-");
        host_print("\n");
        return 1;
    }
    ULONG bytes = 8u << 20;
    void *stack = host.services.alloc(&host, bytes);
    if (!stack)
        return 8;
    host.teb.Tib.StackLimit = stack;
    host.teb.Tib.StackBase = (uint8_t *)stack + bytes;
#ifdef H1_INSTALLERS
    if (game_exec)
        h1_game_exe(m->pe.image);
#endif
    int value = host_entry(m->pe.image + m->pe.entry, host.teb.Tib.StackBase);
    host_print("RESULT entry-returned ");
    host_number((ULONG)value);
    host_print("\n");
    print_stats();
    return value ? 1 : 0;
}

/* DebugBreak through the CiukiOS KERNEL32 PE: int 3 becomes
 * EXCEPTION_BREAKPOINT on the normal dispatch (host signal path standing in
 * for the kernel's vector 3); the SEH frame skips the int 3 and continues. */
static ULONG break_code, break_address, break_calls;
static LONG break_handler(struct w32_exception *r, struct w32_seh *f, struct w32_context *c,
                          struct w32_seh **dispatch) {
    (void)f;
    (void)dispatch;
    break_calls++;
    break_code = r->ExceptionCode;
    break_address = (ULONG)(uintptr_t)r->ExceptionAddress;
    if (r->ExceptionCode != 0x80000003u)
        return ExceptionContinueSearch;
    c->Eip += 1;
    return ExceptionContinueExecution;
}

static int run_debugbreak(void) {
    struct w32_module *k32;
    if (!w32_ldr_load(&loader, "kernel32.dll", 0, &k32))
        return 10;
    void(W32API * debug_break)(void) = w32_ldr_proc(&loader, k32, "DebugBreak", 0, 0xffffffffu);
    if (!debug_break)
        return 11;
    struct w32_seh frame = {(struct w32_seh *)host.teb.Tib.ExceptionList, break_handler};
    host.teb.Tib.ExceptionList = &frame;
    debug_break();
    debug_break();
    host.teb.Tib.ExceptionList = frame.Prev;
    ULONG base = (ULONG)(uintptr_t)k32->pe.image;
    int ok = break_calls == 2 && break_code == 0x80000003u && break_address >= base &&
             break_address < base + k32->pe.size && *(const uint8_t *)(uintptr_t)break_address == 0xcc;
    host_print(ok ? "PASS DebugBreak EXCEPTION_BREAKPOINT at " : "FAIL DebugBreak code ");
    print_hex(ok ? break_address : break_code);
    host_print("\n");
    w32_ldr_shutdown(&loader);
    return ok ? 0 : 12;
}

/* Minimal formatter for the controller.h record grammar: %s, %u, %x and
 * %0Nx (N <= 8), one line per record: RECORD <probe> <event> <fields>. */
static void emit_number(ULONG v, unsigned base, unsigned width) {
    char t[12];
    unsigned n = 0;
    do {
        t[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v && n < sizeof(t));
    while (n < width && n < sizeof(t))
        t[n++] = '0';
    while (n)
        host_print((char[]){t[--n], 0});
}

static void emit_record(const char *probe, const char *event, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    host_print("RECORD ");
    host_print(probe);
    host_print(" ");
    host_print(event);
    host_print(" ");
    for (const char *f = format; *f; f++) {
        if (*f != '%') {
            host_print((char[]){*f, 0});
            continue;
        }
        unsigned width = 0;
        if (*++f == '0')
            for (f++; *f >= '0' && *f <= '9'; f++)
                width = width * 10 + (unsigned)(*f - '0');
        if (*f == 's')
            host_print(va_arg(ap, const char *));
        else if (*f == 'u')
            emit_number(va_arg(ap, ULONG), 10, width);
        else if (*f == 'x')
            emit_number(va_arg(ap, ULONG), 16, width);
        else
            host_print("?");
    }
    host_print("\n");
    va_end(ap);
}

/* The fixture over the PE KERNEL32 loaded by the production loader: its
 * LoadLibraryA / GetProcAddress / FreeLibrary / GetModuleHandleA /
 * GetLastError / LocalFree are the functions the guest PE imports. */
static struct w32_upper_result fixture_result;
static int run_fixture(void) {
    struct w32_module *k32;
    host.peb.ProcessHeap = RtlCreateHeap(2 /* HEAP_GROWABLE */, 0, 0, 0, 0, 0);
    if (!host.peb.ProcessHeap)
        return 7;
    if (!w32_ldr_load(&loader, "kernel32.dll", 0, &k32))
        return 10;
    struct w32_upper_env env;
    env.load = w32_ldr_proc(&loader, k32, "LoadLibraryA", 0, 0xffffffffu);
    env.proc = w32_ldr_proc(&loader, k32, "GetProcAddress", 0, 0xffffffffu);
    env.free = w32_ldr_proc(&loader, k32, "FreeLibrary", 0, 0xffffffffu);
    env.handle = w32_ldr_proc(&loader, k32, "GetModuleHandleA", 0, 0xffffffffu);
    env.last_error = w32_ldr_proc(&loader, k32, "GetLastError", 0, 0xffffffffu);
    env.local_free = w32_ldr_proc(&loader, k32, "LocalFree", 0, 0xffffffffu);
    if (!env.load || !env.proc || !env.free || !env.handle || !env.last_error || !env.local_free)
        return 11;
    struct w32_upper_result *r = &fixture_result;
    r->magic = W32_UPPER_MAGIC;
    r->version = W32_UPPER_VERSION;
    w32_upper_run(&env, W32_UPPER_TABLE, W32_UPPER_TABLE_COUNT, r);
    r->done = 1;
    if (w32_upper_valid(r))
        w32_upper_publish(r, 0, emit_record);
    else
        host_print("FAIL fixture result block invalid\n");
    host_print("RESULT fixture errors=");
    host_number(r->errors);
    host_print(" first_error=");
    host_number(r->first_error);
    host_print("\n");
    w32_ldr_shutdown(&loader);
    RtlDestroyHeap(host.peb.ProcessHeap);
    host.peb.ProcessHeap = 0;
    host_print("DONE detach=");
    host_number(detach_calls);
    host_print(" attach_true=");
    host_number(attach_true);
    host_print(" attach_false=");
    host_number(attach_false);
    host_print(" kernel_enosys=");
    host_number(kernel_enosys);
    host_print(" server_unsupported=");
    host_number(server_unsupported);
    host_print(" allocations=");
    host_number(host.allocations);
    host_print(" mappings=");
    host_number(host.mappings);
    host_print("\n");
    print_stats();
    return r->errors ? 1 : 0;
}

int host_main(int argc, char **argv) {
    int exec = argc == 4 && !w32_strcmp(argv[1], "exec");
#ifdef H1_INSTALLERS
    /* game-exec IMAGE COMMAND [OPTIONS] FD: OPTIONS are harness settings
     * (h1_game_options), never part of the program's command line. */
    game_exec = (argc == 5 || argc == 6) && !w32_strcmp(argv[1], "game-exec");
    installer_exec = game_exec || (argc == 5 && !w32_strcmp(argv[1], "installer-exec"));
    installer_fixture = argc == 3 && !w32_strcmp(argv[1], "installer-fixture");
    exec = exec || installer_exec;
#endif
    int preflight = argc == 2 && !w32_strcmp(argv[1], "sandbox-check");
    if (argc != 2 && !exec
#ifdef H1_INSTALLERS
        && !installer_fixture
#endif
        )
        return 2;
    /* No PE is mapped in this mode. An outer filter may kill the i386
     * prctl rather than returning an error; the host test can distinguish
     * that execution blocker from a SIGSYS after PE execution began. */
    if (preflight)
        host_print("H1-SANDBOX enter\n");
#ifdef H1_INSTALLERS
    if (game_exec && !sys_trap()) {
        host_print("SANDBOX-UNAVAILABLE SIGSYS handler refused\n");
        return 77;
    }
#endif
    if (!sandbox()) {
        host_print("SANDBOX-UNAVAILABLE seccomp filter refused\n");
        return 77;
    }
    if (preflight) {
        host_print("H1-SANDBOX ready\n");
        return 0;
    }
#ifdef H1_INSTALLERS
    h1_installer_providers(h1_store,sizeof(h1_store)/sizeof(h1_store[0]));
    if ((installer_exec && !h1_installer_init(argv[argc - 1])) ||
        (installer_fixture && !h1_installer_init(argv[2])))
        return 9;
    host.services.report = installer_report;
    if (game_exec) {
        h1_game_init(h1_kernel);
        h1_game_script(argv[3]);   /* +set ciuki_keys "..." in the game's command line */
        if (argc == 6)
            h1_game_options(argv[4]);
        host_exec_heap = 1;
    }
#endif
    if (!gate_page())
        return 3;
    base_invoke = host.services.invoke;
    host.services.invoke = logged_invoke;
    host.observer = 0;
    w32_ldr_init(&loader, &host.services, &host.peb);
    w32_ldr_bind(&loader);
    loader.builtins = h1_store;
    loader.builtin_count = sizeof(h1_store) / sizeof(h1_store[0]);
    if (!w32_ldr_thread_attach(&loader, &host.teb))
        return 4;
    struct w32_module *core, *module;
    if (!w32_ldr_load(&loader, "ntdll.dll", 0, &core))
        return 5;
    void (*bind_services)(const struct w32_services *) =
        w32_ldr_proc(&loader, core, "w32_set_services", 0, 0xffffffffu);
    void (*bind_loader)(struct w32_loader *) = w32_ldr_proc(&loader, core, "w32_ldr_bind", 0, 0xffffffffu);
    void *dispatcher = w32_ldr_proc(&loader, core, "KiUserExceptionDispatcher", 0, 0xffffffffu);
    if (!bind_services || !bind_loader || !dispatcher)
        return 6;
    host.dispatcher = dispatcher;
    bind_services(&host.services);
    bind_loader(&loader);
    if (exec)
        return run_exe(argv[2], argv[3]);
    if (!w32_strcmp(argv[1], "debugbreak"))
        return run_debugbreak();
    if (!w32_strcmp(argv[1], "fixture")
#ifdef H1_INSTALLERS
        || installer_fixture
#endif
        )
        return run_fixture();
    /* The process heap, as w32_ldr_start_process creates it: by the adapter's
     * own ntdll code (the PE ntdll is unmapped at loader shutdown). */
    host.peb.ProcessHeap = RtlCreateHeap(2 /* HEAP_GROWABLE */, 0, 0, 0, 0, 0);
    if (!host.peb.ProcessHeap)
        return 7;
    int loaded = w32_ldr_load(&loader, argv[1], 0, &module);
    host_print("RESULT ");
    host_print(argv[1]);
    if (loaded) {
        host_print(" LOADED base=");
        print_hex((ULONG)(uintptr_t)module->pe.image);
        host_print(" attach_true=");
        host_number(attach_true);
        host_print(" attach_false=");
        host_number(attach_false);
        host_print("\n");
        if (!w32_ldr_free(&loader, module))
            return 8;
    } else {
        host_print(" FAIL ");
        host_number(loader.error.code);
        host_print(" ");
        host_print(loader.error.reason ? loader.error.reason : "-");
        host_print(" ");
        host_print(loader.error.module[0] ? loader.error.module : "-");
        host_print(" ");
        host_print(loader.error.symbol[0] ? loader.error.symbol : "-");
        host_print(" attach_false=");
        host_number(attach_false);
        host_print("\n");
    }
    w32_ldr_shutdown(&loader);
    RtlDestroyHeap(host.peb.ProcessHeap);
    host.peb.ProcessHeap = 0;
    host_print("DONE detach=");
    host_number(detach_calls);
    host_print(" gate_total=");
    host_number(gate_calls);
    host_print(" kernel_records=");
    host_number(kernel_records);
    host_print(" kernel_enosys=");
    host_number(kernel_enosys);
    host_print(" server_requests=");
    host_number(server_requests);
    host_print(" server_unsupported=");
    host_number(server_unsupported);
    host_print(" console_writes=");
    host_number(console_writes);
    for (unsigned i = 0; i < 16 && enosys_ops[i][1]; i++) {
        host_print(" enosys:");
        print_hex(enosys_ops[i][0]);
        host_print("=");
        host_number(enosys_ops[i][1]);
    }
    host_print(" allocations=");
    host_number(host.allocations);
    host_print(" mappings=");
    host_number(host.mappings);
    host_print("\n");
    return loaded ? 0 : 1;
}
