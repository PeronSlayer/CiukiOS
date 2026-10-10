/* Signal frames: whole-record validation before modifying live machine state.
 * Intel image layout and SysV background: build/f2-04/research.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/signal.h>

#define USER_FLAGS_EDITABLE 0x00040dd5u /* CF PF AF ZF SF TF DF OF AC */

static uint32_t sf_word(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

void sigframe_fp_sanitize(uint8_t *image)
{
    /* FNSAVE writes implementation-defined reserved bits. ABI output has
     * zero padding and a canonical FCW (bit 6 fixed one). */
    image[0] = (image[0] & 0x3f) | 0x40;
    image[1] &= 0x1f;
    image[2] = image[3] = image[6] = image[7] = 0;
    image[10] = image[11] = image[26] = image[27] = 0;
    image[19] &= 7;
    const unsigned ptrs[] = { 12, 20 };
    for (unsigned i = 0; i < ARRAY_SIZE(ptrs); i++) {
        unsigned off = ptrs[i];
        uint32_t address = sf_word(image + off);
        uint16_t selector;
        memcpy(&selector, image + off + 4, sizeof(selector));
        bool flat = selector == SEL_UCODE || selector == SEL_UDATA;
        bool tls = selector == CIUKI_TLS_SELECTOR && off == 20;
        if (!((flat && address >= CIUKI_IMAGE_BASE && address < CIUKI_MAIN_STACK_LIMIT) ||
              (tls && address < CIUKI_TLS_SIZE)))
            memset(image + off, 0, 6);
    }
}

bool sigframe_fp_validate(uint8_t *image)
{
    const unsigned reserved[] = { 2, 3, 6, 7, 10, 11, 26, 27 };
    for (unsigned i = 0; i < ARRAY_SIZE(reserved); i++)
        if (image[reserved[i]])
            return false;
    if ((image[19] & ~7u) || (image[0] & 0xc0) != 0x40 || (image[1] & ~0x1fu))
        return false;
    sigframe_fp_sanitize(image);
    return true;
}

bool sigframe_executable(const struct uaddr *u, uint32_t address, bool image_only)
{
    const struct ua_extent *e = ua_find(u, address);
    return e && (!image_only || e->kind == UA_IMAGE) &&
           ua_range(u, address, 1, PROT_READ | PROT_EXEC);
}

static bool signal_stack(const struct proc_thread *t, uint32_t base, uint32_t bytes)
{
    if (base < t->stack_base || base - t->stack_base >= t->stack_bytes ||
        bytes > t->stack_bytes - (base - t->stack_base))
        return false;
    const struct ua_extent *e = ua_find(t->process->memory, base);
    return e && e->kind == UA_STACK && e->owner == t->tid &&
           ua_range(t->process->memory, base, bytes, PROT_READ | PROT_WRITE);
}

int sigframe_build(struct proc_thread *t, const struct trap_frame *tf,
                   const struct ciuki_sigaction *action, const struct ciuki_siginfo *info,
                   uint64_t token, struct ciuki_signal_frame *frame, uint32_t *address)
{
    if (tf->user_esp < sizeof(*frame))
        return -EFAULT;
    uint32_t base = (tf->user_esp - sizeof(*frame)) & ~3u;
    if (!signal_stack(t, base, sizeof(*frame)) ||
        !sigframe_executable(t->process->memory, action->handler, true) ||
        !sigframe_executable(t->process->memory, action->restorer, true))
        return -EFAULT;
    memset(frame, 0, sizeof(*frame));
    frame->restorer = action->restorer;
    frame->signo = info->signo;
    frame->siginfo_ptr = base + offsetof(struct ciuki_signal_frame, info);
    frame->context_ptr = base + offsetof(struct ciuki_signal_frame, context);
    frame->size = sizeof(*frame);
    frame->version = CIUKI_ABI_VERSION;
    frame->token = token;
    frame->info = *info;
    struct ciuki_ucontext *c = &frame->context;
    c->size = sizeof(*c);
    c->stack_base = t->stack_base;
    c->stack_bytes = t->stack_bytes;
    c->mask = t->mask;
    _Static_assert(sizeof(c->gregs) == sizeof(*tf), "trap/context register layout");
    memcpy(c->gregs, tf, sizeof(*tf));
    /* Segment pushes on i386 need not define the high half of the slot. */
    for (unsigned i = CIUKI_REG_GS; i <= CIUKI_REG_DS; i++)
        c->gregs[i] &= 0xffff;
    c->gregs[CIUKI_REG_CS] &= 0xffff;
    c->gregs[CIUKI_REG_SS] &= 0xffff;
    c->gregs[CIUKI_REG_ESP] = tf->user_esp;
    c->cr2 = info->vector == 14 ? info->fault_addr : 0;
    c->fp_format = CIUKI_FP_FNSAVE;
    fpu_signal_export(t->task, c->fp_state);
    *address = base;
    return 0;
}

int sigframe_validate(const struct proc_thread *t, const struct sig_active *a,
                      uint32_t address, struct ciuki_signal_frame *f, struct trap_frame *tf)
{
    const struct ciuki_signal_frame *o = &a->original;
    struct ciuki_ucontext *c = &f->context;
    const struct ciuki_ucontext *old = &o->context;
    if (!t->in_handler || a->tid != t->tid || a->address != address || (address & 3) ||
        a->tls != t->tls || memcmp(f, o, offsetof(struct ciuki_signal_frame, context)) ||
        memcmp(c, old, offsetof(struct ciuki_ucontext, mask)) ||
        c->cr2 != old->cr2 || c->fp_format != old->fp_format ||
        memcmp(c->reserved, old->reserved, sizeof(c->reserved)) ||
        (c->mask & ~PROC_SIGNAL_SET))
        return -EINVAL;
    const unsigned fixed[] = { CIUKI_REG_GS, CIUKI_REG_FS, CIUKI_REG_ES, CIUKI_REG_DS,
        CIUKI_REG_VECTOR, CIUKI_REG_ERROR, CIUKI_REG_CS, CIUKI_REG_SS };
    for (unsigned i = 0; i < ARRAY_SIZE(fixed); i++)
        if (c->gregs[fixed[i]] != old->gregs[fixed[i]])
            return -EINVAL;
    uint32_t flags = c->gregs[CIUKI_REG_EFLAGS];
    /* Only the documented user bits may differ; reject privileged/reserved
     * bits even if a corrupted supervisor snapshot happened to contain them. */
    if (((flags ^ old->gregs[CIUKI_REG_EFLAGS]) & ~USER_FLAGS_EDITABLE) ||
        (flags & CIUKI_INITIAL_EFLAGS) != CIUKI_INITIAL_EFLAGS ||
        (flags & ~(USER_FLAGS_EDITABLE | CIUKI_INITIAL_EFLAGS | 0x00210000u)) ||
        !sigframe_executable(t->process->memory, c->gregs[CIUKI_REG_EIP], false) ||
        c->gregs[CIUKI_REG_ESP] != c->gregs[CIUKI_REG_USER_ESP] ||
        !signal_stack(t, c->gregs[CIUKI_REG_USER_ESP], 1) ||
        !sigframe_fp_validate(c->fp_state))
        return -EINVAL;
    c->mask &= ~CIUKI_SIGBIT(SIGKILL);
    memcpy(tf, c->gregs, sizeof(*tf));
    return 0;
}
