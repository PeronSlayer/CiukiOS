/* Lazy FPU switching through #NM (docs/design/execution-abi.md).
 * CR0.MP=1, CR0.NE=1, CR0.EM=0; CR4.OSFXSR/OSXMMEXCPT when supported.
 * First use restores a fully initialised image, never FNINIT alone.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>

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
    cr0 |= CR0_MP | CR0_NE;
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
