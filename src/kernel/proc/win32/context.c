/* Integer-only i386 Windows exception contexts and fault classification.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/win32.h>
/* Arithmetic flags, TF, DF, AC; IF and bit 1 are fixed. */
#define W32_FLAGS 0x00040dd5u
void w32_context_capture(struct ciuki_w32_context *c, const struct trap_frame *tf)
{
    memset(c, 0, sizeof(*c));
    c->flags = W32_CONTEXT_FULL | W32_CONTEXT_FLOAT;
    c->gs = tf->gs; c->fs = tf->fs; c->es = tf->es; c->ds = tf->ds;
    c->edi = tf->edi; c->esi = tf->esi; c->ebx = tf->ebx; c->edx = tf->edx;
    c->ecx = tf->ecx; c->eax = tf->eax; c->ebp = tf->ebp; c->eip = tf->eip;
    c->cs = tf->cs; c->eflags = (tf->eflags & W32_FLAGS) | 0x202;
    c->esp = tf->user_esp; c->ss = tf->user_ss;
}
void w32_context_restore(struct trap_frame *tf, const struct ciuki_w32_context *c)
{
    tf->gs = c->gs; tf->fs = c->fs; tf->es = c->es; tf->ds = c->ds;
    tf->edi = c->edi; tf->esi = c->esi; tf->ebx = c->ebx; tf->edx = c->edx;
    tf->ecx = c->ecx; tf->eax = c->eax; tf->ebp = c->ebp; tf->eip = c->eip;
    tf->cs = c->cs; tf->eflags = c->eflags; tf->user_esp = c->esp; tf->user_ss = c->ss;
}
bool w32_context_validate(struct ciuki_w32_context *c, const struct uaddr *u, bool extended, uint32_t mask)
{
    uint32_t required = W32_CONTEXT_FULL | W32_CONTEXT_FLOAT;
    if ((c->flags & required) != required || (c->flags & ~(required | W32_CONTEXT_EXTENDED)) ||
        (!extended && (c->flags & 0x20)) || c->cs != SEL_UCODE || c->ss != SEL_UDATA ||
        c->ds != SEL_UDATA || c->es != SEL_UDATA || c->gs != SEL_UDATA || c->fs != CIUKI_W32_FS_SELECTOR ||
        (c->eflags & ~W32_FLAGS) != 0x202 || c->eip < CIUKI_W32_LOW || c->eip >= CIUKI_W32_BUILTIN_LIMIT ||
        c->esp < CIUKI_W32_LOW || c->esp >= CIUKI_W32_APP_LIMIT ||
        !ua_range(u, c->eip, 1, PROT_READ | PROT_EXEC) || !ua_range(u, c->esp, 1, PROT_READ | PROT_WRITE))
        return false;
    for (unsigned i = 0; i < 6; i++) if (c->debug[i]) return false;
    /* Sanitize x87 control/reserved fields; no CR0/segment/debug authority. */
    c->floating.control = (c->floating.control & 0x1f3f) | 0x40;
    c->floating.status &= 0xffff; c->floating.tag &= 0xffff;
    c->floating.error_selector &= 0x07ff0000; c->floating.data_selector = 0; c->floating.cr0_npx = 0;
    if (c->flags & 0x20) {
        uint8_t *a = c->extended;
        uint16_t cw; memcpy(&cw, a, 2); cw = (cw & 0x1f3f) | 0x40; memcpy(a, &cw, 2);
        a[5] = 0; a[7] &= 7;
        a[12] = a[13] = a[14] = a[15] = 0;
        a[20] = a[21] = a[22] = a[23] = 0;
        uint32_t mxcsr; memcpy(&mxcsr, a + 24, 4); mxcsr &= mask ? mask : 0xffbf;
        memcpy(a + 24, &mxcsr, 4); memset(a + 28, 0, 4);
        for (unsigned i = 0; i < 8; i++) memset(a + 32 + i * 16 + 10, 0, 6);
        memset(a + 288, 0, 224);
    } else memset(c->extended, 0, 512);
    return true;
}

static unsigned skip_prefix(const uint8_t *code, unsigned bytes)
{
    unsigned i = 0;
    while (i < bytes && i < 15 && (code[i] == 0x66 || code[i] == 0x67 || code[i] == 0xf2 ||
        code[i] == 0xf3 || code[i] == 0x2e || code[i] == 0x36 || code[i] == 0x3e ||
        code[i] == 0x26 || code[i] == 0x64 || code[i] == 0x65)) i++;
    return i;
}
bool w32_sysenter(const uint8_t *code, unsigned bytes)
{
    unsigned i = skip_prefix(code, bytes);
    return i < 14 && i + 1 < bytes && code[i] == 0x0f && code[i + 1] == 0x34;
}
bool w32_privileged(const uint8_t *code, unsigned bytes)
{
    unsigned i = skip_prefix(code, bytes);
    if (i >= bytes || i == 15) return false;
    uint8_t op = code[i++];
    if (op == 0xfa || op == 0xfb || op == 0xf4 || (op >= 0xe4 && op <= 0xe7) ||
        (op >= 0xec && op <= 0xef) || (op >= 0x6c && op <= 0x6f)) return true;
    return op == 0x0f && i < bytes && (code[i] == 0x20 || code[i] == 0x21 || code[i] == 0x22 || code[i] == 0x23);
}
uint32_t w32_exception_code(uint32_t v, bool privileged, uint16_t control, uint16_t status, uint32_t mxcsr)
{
    switch (v) {
    case 0: return W32_EXCEPTION_DIVIDE;
    case 1: return W32_EXCEPTION_SINGLE_STEP;
    case 3: return W32_EXCEPTION_BREAKPOINT;
    case 4: return W32_EXCEPTION_OVERFLOW;
    case 5: return W32_EXCEPTION_BOUNDS;
    case 6: return W32_EXCEPTION_ILLEGAL;
    case 13: return privileged ? W32_EXCEPTION_PRIVILEGED : W32_STATUS_ACCESS_VIOLATION;
    case 16: {
        unsigned faults = status & ~control & 63;
        if ((faults & 1) && (status & 0x40)) return 0xc0000092; /* stack check */
        static const uint32_t codes[6] = {0xc0000090,0xc000008d,0xc000008e,0xc0000091,0xc0000093,0xc000008f};
        for (unsigned i = 0; i < 6; i++) if (faults & (1u << i)) return codes[i];
        return 0xc0000090;
    }
    case 17: return W32_EXCEPTION_ALIGNMENT;
    case 19: {
        unsigned faults = (mxcsr & 63) & ~((mxcsr >> 7) & 63);
        return faults ? W32_EXCEPTION_SSE : 0xc00002b5;
    }
    default: return W32_STATUS_ACCESS_VIOLATION;
    }
}
