/* Lazy FPU switching through #NM (docs/design/execution-abi.md).
 * CR0.MP=1, CR0.NE=1, CR0.EM=0; CR4.OSFXSR/OSXMMEXCPT when supported.
 * First use restores a fully initialised image, never FNINIT alone.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/signal.h>
#include <ciuki/win32.h>
extern uint32_t w32_task_cw(const struct task *) __attribute__((weak));

void fpu_fxsave(void *p);
void fpu_fxrstor(const void *p);
void fpu_fnsave(void *p);
void fpu_frstor(const void *p);
void fpu_reset_state(int sse);

uint32_t g_fpu_policy = 1;
static struct task *owner;
static uint8_t init_image[512] __attribute__((aligned(16)));
static uint8_t w32_init_image[512] __attribute__((aligned(16)));
static uint32_t mxcsr_mask = 0xffbf;
static uint32_t policy_cr4(const struct task *t)
{
    return t && t->win32 && g_cpu_fxsr && g_cpu_sse ? CR4_OSFXSR | CR4_OSXMMEXCPT : 0;
}
static const uint8_t *initial(const struct task *t) { return t && t->win32 ? w32_init_image : init_image; }

static void save_to(uint8_t *a)
{
    if (g_cpu_fxsr) {
        uint32_t cr4 = read_cr4();
        write_cr4(cr4 | CR4_OSFXSR);
        fpu_fxsave(a);
        write_cr4(cr4);
    } else fpu_fnsave(a);
}
static void restore_from(const uint8_t *a)
{
    if (g_cpu_fxsr) {
        uint32_t cr4 = read_cr4();
        write_cr4(cr4 | CR4_OSFXSR);
        fpu_fxrstor(a);
        write_cr4(cr4);
    } else fpu_frstor(a);
}

void fpu_init(void)
{
    uint32_t cr0 = read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE | (1u << 18); /* CR0.AM: #AC only with CPL3/AC */
    write_cr0(cr0);
    /* Native policy: SSE disabled. Saves temporarily enable OSFXSR so an
     * outgoing Win32 lazy owner always retains its XMM/MXCSR state. */
    write_cr4(read_cr4() & ~(CR4_OSFXSR | CR4_OSXMMEXCPT));
    fpu_reset_state(0);
    save_to(init_image);                  /* FNSAVE also re-initialises */
    /* FNINIT leaves the data register payloads unchanged (only the tags
     * become empty), so make the image architectural: zero payloads,
     * FCW 037F, FSW 0, all tags empty, MXCSR 1F80. */
    if (g_cpu_fxsr) {
        memset(init_image + 32, 0, 256);  /* ST0-7 and XMM0-7 */
        *(uint16_t *)(init_image + 0) = 0x037F;
        *(uint16_t *)(init_image + 2) = 0;
        init_image[4] = 0;                /* abridged FTW: all empty */
        *(uint32_t *)(init_image + 24) = 0x1F80;
    } else {
        memset(init_image + 28, 0, 80);   /* ST0-7 in the 108-byte FNSAVE image */
        *(uint32_t *)(init_image + 0) = 0x037F;
        *(uint32_t *)(init_image + 4) = 0;
        *(uint32_t *)(init_image + 8) = 0xFFFF;
    }
    if (g_cpu_fxsr) {
        memcpy(&mxcsr_mask, init_image + 28, 4);
        if (!mxcsr_mask) mxcsr_mask = 0xffbf;
    }
    memcpy(w32_init_image, init_image, sizeof(init_image));
    *(uint16_t *)w32_init_image = 0x027f;
    owner = 0;
    write_cr0(read_cr0() | CR0_TS);
}

void fpu_task_switch(struct task *next)
{
    uint32_t cr4 = read_cr4(), want = (cr4 & ~(CR4_OSFXSR | CR4_OSXMMEXCPT)) | policy_cr4(next);
    if (want != cr4) write_cr4(want); /* serializing: only on a personality change */
    if (next == owner)
        clts();
    else
        write_cr0(read_cr0() | CR0_TS);
}

void fpu_handle_nm(void)
{
    struct task *cur = g_current;
    clts();
    if (owner == cur)
        return;
    if (owner && owner->fpu_area) {
        save_to(owner->fpu_area);
        owner->fpu_valid = true;
    }
    if (!cur->fpu_area)
        panic("#NM in a task without an FPU area (%s)", cur->name);
    restore_from(cur->fpu_valid ? cur->fpu_area : initial(cur));
    if(!cur->fpu_valid&&cur->win32&&w32_task_cw){
        /* Populate the lazy image using integer stores; no kernel FLDCW. */
        save_to(cur->fpu_area);
        *(uint16_t *)cur->fpu_area=(uint16_t)w32_task_cw(cur);
        restore_from(cur->fpu_area);cur->fpu_valid=true;
    }
    owner = cur;
}

bool fpu_is_owner(const struct task *t) { return owner == t; }

void fpu_task_release(struct task *t)
{
    if (owner == t) {
        owner = 0;
        write_cr0(read_cr0() | CR0_TS);
    }
    t->fpu_valid = false;
}

/* Intel SDM Vol. 2A, FXSAVE, "Recreating FSAVE Format". Data slots are
 * logical ST(i), whereas both tag words index physical R(i). Integer only.
 * Research and the restore policy: build/f2-04/research.md. */
static uint16_t fp_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static void fp_put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

void fpu_signal_to_fnsave(uint8_t *out, const uint8_t *in, bool fxsr)
{
    memset(out, 0, sizeof(((struct ciuki_ucontext *)0)->fp_state));
    if (!fxsr) {
        memcpy(out, in, sizeof(((struct ciuki_ucontext *)0)->fp_state));
    } else {
        memcpy(out, in, 2);           /* FCW */
        memcpy(out + 4, in + 2, 2);  /* FSW */
        memcpy(out + 12, in + 8, 6); /* FIP/FCS */
        memcpy(out + 18, in + 6, 2); /* FOP */
        memcpy(out + 20, in + 16, 6); /* FDP/FDS */
        unsigned top = (fp_u16(in + 2) >> 11) & 7;
        uint16_t tags = 0;
        for (unsigned i = 0; i < 8; i++) {
            const uint8_t *reg = in + 32 + i * 16;
            memcpy(out + 28 + i * 10, reg, 10);
            unsigned physical = (top + i) & 7, tag = 3;
            if (in[4] & (1u << physical)) {
                unsigned exponent = fp_u16(reg + 8) & 0x7fff;
                bool zero = true;
                for (unsigned b = 0; b < 8; b++)
                    zero = zero && !reg[b];
                tag = !exponent && zero ? 1 :
                      exponent && exponent != 0x7fff && (reg[7] & 0x80) ? 0 : 2;
            }
            tags |= (uint16_t)(tag << (physical * 2));
        }
        fp_put16(out + 8, tags);
    }
    sigframe_fp_sanitize(out);
}

void fpu_signal_from_fnsave(uint8_t *out, const uint8_t *in, bool fxsr)
{
    /* Start from the clean private template; user SSE/MXCSR never enter. */
    memcpy(out, init_image, sizeof(init_image));
    if (!fxsr) {
        memcpy(out, in, sizeof(((struct ciuki_ucontext *)0)->fp_state));
        return;
    }
    memcpy(out, in, 2);
    memcpy(out + 2, in + 4, 2);
    memcpy(out + 6, in + 18, 2);
    memcpy(out + 8, in + 12, 6);
    memcpy(out + 16, in + 20, 6);
    uint16_t tags = fp_u16(in + 8);
    out[4] = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (((tags >> (i * 2)) & 3) != 3)
            out[4] |= (uint8_t)(1u << i);
        memcpy(out + 32 + i * 16, in + 28 + i * 10, 10);
    }
}

void fpu_signal_export(struct task *t, uint8_t *image)
{
    uint32_t flags = irq_save();
    /* Saving the real owner must precede inspection of any resident image.
     * Drop ownership even for FNSAVE, which resets the hardware as it saves. */
    if (owner) {
        clts();
        save_to(owner->fpu_area);
        owner->fpu_valid = true;
        owner = 0;
    }
    write_cr0(read_cr0() | CR0_TS);
    fpu_signal_to_fnsave(image, t->fpu_valid ? t->fpu_area : initial(t), g_cpu_fxsr);
    irq_restore(flags);
}

void fpu_signal_reset(struct task *t)
{
    uint32_t flags = irq_save();
    fpu_task_release(t);
    memcpy(t->fpu_area, initial(t), sizeof(init_image));
    t->fpu_valid = true;
    irq_restore(flags);
}

void fpu_signal_import(struct task *t, const uint8_t *image)
{
    uint32_t flags = irq_save();
    fpu_task_release(t);              /* discard the handler's hardware state */
    fpu_signal_from_fnsave(t->fpu_area, image, g_cpu_fxsr);
    t->fpu_valid = true;              /* #NM will perform the hardware restore */
    irq_restore(flags);
}

uint32_t fpu_mxcsr_mask(void) { return mxcsr_mask; }
void fpu_w32_export(struct task *t, struct ciuki_w32_context *c)
{
    uint8_t legacy[108];
    fpu_signal_export(t, legacy); /* saves/detaches the actual lazy owner */
    memset(&c->floating, 0, sizeof(c->floating));
    c->floating.control = fp_u16(legacy);
    c->floating.status = fp_u16(legacy + 4);
    c->floating.tag = fp_u16(legacy + 8);
    memcpy(&c->floating.error_offset, legacy + 12, 4);
    c->floating.error_selector = fp_u16(legacy + 16) | (uint32_t)fp_u16(legacy + 18) << 16;
    memcpy(&c->floating.data_offset, legacy + 20, 4);
    c->floating.data_selector = fp_u16(legacy + 24);
    memcpy(c->floating.registers, legacy + 28, 80);
    if (g_cpu_fxsr) {
        c->flags |= W32_CONTEXT_EXTENDED;
        memcpy(c->extended, t->fpu_valid ? t->fpu_area : initial(t), 512);
    }
}
void fpu_w32_import(struct task *t, const struct ciuki_w32_context *c)
{
    uint8_t legacy[108] = {0};
    uint32_t flags = irq_save();
    fpu_task_release(t);
    if (g_cpu_fxsr && (c->flags & W32_CONTEXT_EXTENDED)) memcpy(t->fpu_area, c->extended, 512);
    else {
        fp_put16(legacy, (uint16_t)c->floating.control);
        fp_put16(legacy + 4, (uint16_t)c->floating.status);
        fp_put16(legacy + 8, (uint16_t)c->floating.tag);
        memcpy(legacy + 12, &c->floating.error_offset, 4);
        memcpy(legacy + 16, &c->floating.error_selector, 4);
        memcpy(legacy + 20, &c->floating.data_offset, 4);
        fp_put16(legacy + 24, (uint16_t)c->floating.data_selector);
        memcpy(legacy + 28, c->floating.registers, 80);
        sigframe_fp_sanitize(legacy);
        fpu_signal_from_fnsave(t->fpu_area, legacy, g_cpu_fxsr);
    }
    t->fpu_valid = true;
    irq_restore(flags);
}
