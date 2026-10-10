/* Lazy FPU switching through #NM (docs/design/execution-abi.md).
 * CR0.MP=1, CR0.NE=1, CR0.EM=0; CR4.OSFXSR/OSXMMEXCPT when supported.
 * First use restores a fully initialised image, never FNINIT alone.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/signal.h>

void fpu_fxsave(void *p);
void fpu_fxrstor(const void *p);
void fpu_fnsave(void *p);
void fpu_frstor(const void *p);
void fpu_reset_state(int sse);

uint32_t g_fpu_policy = 1;
static struct task *owner;
static uint8_t init_image[512] __attribute__((aligned(16)));

static void save_to(uint8_t *a) { if (g_cpu_fxsr) fpu_fxsave(a); else fpu_fnsave(a); }
static void restore_from(const uint8_t *a) { if (g_cpu_fxsr) fpu_fxrstor(a); else fpu_frstor(a); }

void fpu_init(void)
{
    uint32_t cr0 = read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE | (1u << 18); /* CR0.AM: #AC only with CPL3/AC */
    write_cr0(cr0);
    /* F0 policy: SSE stays disabled for ring 3 (CR4.OSFXSR clear); FXSAVE
     * still saves the x87 state when FXSR exists. */
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
    owner = 0;
    write_cr0(read_cr0() | CR0_TS);
}

void fpu_task_switch(struct task *next)
{
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
    restore_from(cur->fpu_valid ? cur->fpu_area : init_image);
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
    fpu_signal_to_fnsave(image, t->fpu_valid ? t->fpu_area : init_image, g_cpu_fxsr);
    irq_restore(flags);
}

void fpu_signal_reset(struct task *t)
{
    uint32_t flags = irq_save();
    fpu_task_release(t);
    memcpy(t->fpu_area, init_image, sizeof(init_image));
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
