/* Serialized F1 firmware worker. Caller owns the request mutex while
 * sleeping; the separate VM thread enters firmware with NO mutex held.
 * Intel V86 entry/return and software IF: SDM Vol. 3B ch. 23,
 * https://cdrdv2-public.intel.com/874250/253669-090-sdm-vol-3b.pdf
 * Firmware input dependencies (not copied code):
 * https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c
 * https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/hw/ps2port.c
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/registry.h>
#include <ciuki/vga_ports.h>
#include <ciuki/biosvm.h>
#include <ciuki/abi.h>
#include <ciuki/display.h>
#include <ciuki/init.h>
#include <ciuki/apm_ports.h>
#include <ciuki/dos_video_modes.h>

extern void v86_enter(const struct v86_frame *f, uint32_t *continuation);
extern void v86_leave(uint32_t continuation) __attribute__((noreturn));
extern const uint8_t biosvm_mouse_stub[], biosvm_mouse_stub_end[];

static struct v86 firmware;
static struct v86_frame frame;
static struct task *vm_thread;
static struct aspace vm_as;
static uint32_t low_pt, continuation;
static struct kmutex request_mutex;
static struct kwait wake, completion;
static volatile uint16_t pending_irqs;
static bool initialized, quarantined, synthetic, input_enabled, video_active, video_claimed;
/* Execution faults are terminal for their service, not the shared broker.
 * quarantined above is reserved for invalid shared initialization/ownership. */
enum bios_family { BIOS_INPUT, BIOS_APM, BIOS_VIDEO, BIOS_FAMILIES };
static bool family_quarantined[BIOS_FAMILIES];
static bool fullscreen_video;
static bool family_disabled(enum bios_family family)
{
    return quarantined || family_quarantined[family] ||
        (synthetic && firmware.state == V86_DISABLED);
}
/* Firmware only: do not widen the generic monitor or DOS I/O policy. */
static int delay_io(void *arg, uint16_t port, unsigned width, bool write, uint32_t *value)
{
    (void)arg;
    if ((port != 0x80 && port != 0xED) || width != 1) return -ENOSYS;
    /* See docs/validation/fx-biosvm.md: never forward POST/delay cycles. */
    if (!write) *value = 0xFF;
    return 0;
}
static struct { int handle; gen_t generation; } video_leases[4];
static unsigned video_lease_count;
static bool video_dispi;
static bool request_pending, request_done;
static unsigned request_test;
static struct biosvm_regs request_regs;
static uint32_t request_ms;
static uint64_t apm_deadline;
static bool apm_deadline_active;
static int request_result;
static void (*input_service)(void);
static void (*input_observer)(uint8_t status, uint8_t byte, bool keyboard_irq);
static uint8_t controller_status;
static bool controller_status_valid;
static struct biosvm_mapping mappings[BIOSVM_MAP_COUNT];
static struct { int handle; gen_t generation; } leases[4];
static unsigned lease_count;
static uint16_t mouse_lost;
static struct kwait *input_wait;
static uint64_t input_reflections;
static struct biosvm_input_diag delivery;
static int submit(struct biosvm_regs *regs, uint32_t ms, unsigned test);

static void notify_input(void)
{
    if (input_wait) {
        delivery.signals++;
        kwait_wake_all(input_wait);
    }
}

static int reflect_input_irq(struct v86_frame *f)
{
    if (family_disabled(BIOS_INPUT) && !synthetic) return 0;
    int irq = v86_irq_pending(&firmware);
    int rc = v86_irq_deliver(&firmware, f);
    if (rc > 0 && !synthetic && (irq == 1 || irq == 12)) {
        delivery.reflected[irq == 12]++;
        input_reflections++;
        notify_input();
    }
    return rc;
}

static int init_error(const char *step, int error)
{
    klog("[biosvm] step=%s error=%d leases=%u", step, error, lease_count);
    return error;
}

static void log_regs(const char *step, const char *direction, const struct biosvm_regs *r)
{
    klog("[biosvm] step=%s regs=%s int=%02x eax=%08x ebx=%08x ecx=%08x edx=%08x flags=%04x",
         step, direction, r->interrupt, r->eax, r->ebx, r->ecx, r->edx, r->flags);
    klog("[biosvm] step=%s regs=%s esi=%08x edi=%08x ebp=%08x ds=%04x es=%04x fs=%04x gs=%04x",
         step, direction, r->esi, r->edi, r->ebp, r->ds, r->es, r->fs, r->gs);
}

static void *vm_memory(void *arg, uint32_t linear, unsigned bytes, bool write)
{
    (void)arg;
    if (!bytes || linear >= 0x100000 || bytes > 0x100000u - linear || !low_pt)
        return 0;
    const uint32_t *pt = P2V(low_pt);
    for (uint32_t p = linear >> 12; p <= (linear + bytes - 1) >> 12; p++) {
        uint32_t entry = pt[p];
        if ((entry & (PTE_P | PTE_U)) != (PTE_P | PTE_U) || (write && !(entry & PTE_W)))
            return 0;
        if ((entry & ~0xFFFu) != p * PAGE_SIZE)
            return 0;
    }
    return P2V(linear);
}

static uint8_t vm_in(void *arg, uint16_t port)
{
    (void)arg;
    uint8_t byte = inb(port);
    if (port == 0x64) {
        controller_status = byte;
        controller_status_valid = true;
    } else if (port == 0x60) {
        if (!synthetic) {
            delivery.port60++;
            delivery.last_status = controller_status_valid ? controller_status : 0;
            delivery.last_byte = byte;
        }
        if (input_observer && !synthetic)
            input_observer(controller_status_valid ? controller_status : 0, byte,
                           !!(firmware.pic[0].isr & (1u << 1)));
        controller_status_valid = false;
    }
    return byte;
}

static void vm_out(void *arg, uint16_t port, uint8_t value)
{
    (void)arg;
    controller_status_valid = false;
    outb(port, value);
}

static void quarantine(void)
{
    if (quarantined)
        return;
    quarantined = true;
    /* Do not touch PIC/PIT here: physical dispatcher retains EOI ownership.
     * Disabled callbacks consume nothing and the lease stays pinned. */
    for (unsigned i = 0; i < video_lease_count; i++)
        registry_quarantine(video_leases[i].handle, video_leases[i].generation);
    for (unsigned i = 0; i < lease_count; i++) {
        int rc = registry_quarantine(leases[i].handle, leases[i].generation);
        klog("[biosvm] step=quarantine handle=%d generation=%u result=%d",
             leases[i].handle, leases[i].generation, rc);
    }
}

static void quarantine_family(enum bios_family family, const char *reason)
{
    if (family_quarantined[family]) return;
    family_quarantined[family] = true;
    klog("[biosvm] step=family_quarantine family=%s reason=%s",
         family == BIOS_INPUT ? "input" : family == BIOS_APM ? "apm" : "video", reason);
    /* APM has no broker-owned hardware lease. Never reclaim an affected
     * input/video lease without reset, even though other services may run. */
    if (family == BIOS_INPUT)
        for (unsigned i = 0; i < lease_count; i++)
            registry_quarantine(leases[i].handle, leases[i].generation);
    if (family == BIOS_VIDEO)
        for (unsigned i = 0; i < video_lease_count; i++)
            registry_quarantine(video_leases[i].handle, video_leases[i].generation);
}

static void capture_pending(void)
{
    uint32_t f = irq_save();
    uint16_t bits = pending_irqs;
    pending_irqs = 0;
    irq_restore(f);
    for (unsigned i = 0; i < 16; i++)
        if (bits & (1u << i))
            v86_irq_raise(&firmware, i);
}

static bool vm_woken(void *arg)
{
    (void)arg;
    return pending_irqs != 0 || firmware.state == V86_DISABLED;
}

static void irq_input(struct trap_frame *tf)
{
    if (!initialized || !input_enabled || family_disabled(BIOS_INPUT) || synthetic)
        return;
    unsigned irq = tf->vector - 0x20;
    delivery.queued[irq == 12]++;
    pending_irqs |= 1u << irq;
    kwait_wake_all(&wake);
}

uint32_t biosvm_task_cr3(const struct task *t, uint32_t normal)
{
    return t == vm_thread && continuation ? vm_as.pd_phys : normal;
}

uint32_t biosvm_task_esp0(const struct task *t, uint32_t normal)
{
    return t == vm_thread && continuation ? continuation : normal;
}

void biosvm_trap(struct trap_frame *tf)
{
    if (g_current != vm_thread || !continuation)
        panic_frame(tf, "unowned_v86_frame");
    struct v86_frame *f = (struct v86_frame *)tf;
    /* isr.asm has already saved every register. All monitor work is
     * preemptible by physical IRQs; only the ring-3 return is scheduled. */
    crit_end();
    sti();
    int rc = v86_check_deadline(&firmware, deadline_after_ms(0));
    if (rc < 0)
        v86_abort(&firmware, f, rc);
    if (!rc) {
        if (tf->vector == 13) {
            rc = v86_emulate(&firmware, f);
        } else if (tf->vector == 1 && firmware.shadow) {
            rc = v86_debug_step(&firmware, f);
        } else if (tf->vector == 3 || tf->vector == 4) {
            /* INT3/INTO are traps: saved IP already names the next insn. */
            rc = v86_reflect(&firmware, f, (uint8_t)tf->vector);
        } else if (tf->vector < 0x20 || tf->vector >= 0x30) {
            if (video_active) {
                klog("[biosvm] fullscreen_fault vector=%u cs=%04x ip=%04x address=%08x",
                    tf->vector,tf->cs,tf->eip,tf->vector==14 ? read_cr2() : 0);
                display_fullscreen_checkpoint("bios-fault",-EFAULT);
            }
            rc = v86_abort(&firmware, f, -EFAULT);
            if (tf->vector == 14)
                firmware.fault.address = read_cr2();
        }
    }
    while (rc >= 0 && firmware.state != V86_DONE) {
        capture_pending();
        rc = v86_check_deadline(&firmware, deadline_after_ms(0));
        if (rc < 0) {
            v86_abort(&firmware, f, rc);
            break;
        }
        rc = reflect_input_irq(f);
        if (rc < 0 || firmware.state != V86_HALTED)
            break;
        kwait_wait_until(&wake, vm_woken, 0, firmware.deadline);
    }
    if (rc < 0 || firmware.state == V86_DONE) {
        frame = *f;
        cli();
        v86_leave(continuation);
    }
    if (g_need_resched)
        schedule();
    cli();
}

static bool reserved_ram(uint32_t start, uint32_t end)
{
    for (uint32_t p = start; p < end; p += PAGE_SIZE)
        if (!pmm_is_reserved(p))
            return false;
    for (unsigned i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if (e->type == CBI_E820_RAM && start >= e->base && (uint64_t)end <= e->base + e->length)
            return true;
    }
    return false;
}

static int map_firmware(void)
{
    /* boot-memory.md dedicates 10000-1FFFF physically to BIOS calls. The
     * PMM already excludes ALL low pages; no general LOW allocation exists.
     * Reuse two pages of that explicit reservation, never allocator RAM. */
    if (!reserved_ram(BIOSVM_SCRATCH, BIOSVM_STACK + PAGE_SIZE))
        return init_error("scratch_reservation", -EFAULT);
    uint16_t ebda_seg, conventional_kib;
    memcpy(&ebda_seg, P2V(0x40E), 2);
    memcpy(&conventional_kib, P2V(0x413), 2);
    uint32_t ebda = (uint32_t)ebda_seg << 4;
    uint32_t lowmem = (uint32_t)conventional_kib * 1024;
    if (ebda < 0x80000 || ebda >= 0xA0000 || lowmem < 0x20000 || lowmem > ebda) {
        klog("[biosvm] step=ebda_base error=%d segment=%04x conventional_kib=%u", -EFAULT, ebda_seg, conventional_kib);
        return -EFAULT;
    }
    /* RBIL BDA 40:0E / INT15 C1: the byte is KiB, not an authority for
     * reservation. Linux arch/x86/kernel/ebda.c preserves the lowmem tail
     * without using it. Keep the whole BDA-reserved extent mapped even if
     * the byte is zero, too small, or extends into the VGA aperture. */
    uint32_t claimed_end = ebda + *(const uint8_t *)P2V(ebda) * 1024u;
    uint32_t end = 0xA0000;
    if (claimed_end != end)
        klog("[biosvm] step=ebda_size fallback=lowmem start=%08x end=%08x", lowmem, end);
    int rc = as_create(&vm_as);
    if (rc)
        return init_error("address_space", rc);
    low_pt = pmm_alloc();
    mm_claim(low_pt,1,MM_TABLE);
    if (!low_pt) {
        mm_free(vm_as.pd_phys,MM_TABLE);
        vm_as.pd_phys = 0;
        return init_error("page_table", -ENOMEM);
    }
    uint32_t *pt = P2V(low_pt), *pd = P2V(vm_as.pd_phys);
    memset(pt, 0, PAGE_SIZE);
    pd[0] = low_pt | PTE_P | PTE_W | PTE_U;
    /* Page granularity exposes 500-FFF too. These remain reserved firmware
     * state, not user/allocator data. Ordinary tasks never get this PDE. */
    uint32_t rw = PTE_P | PTE_W | PTE_U, rom = PTE_P | PTE_U | PTE_PCD | PTE_PWT;
    mappings[0] = (struct biosvm_mapping){0, PAGE_SIZE, rw};
    mappings[1] = (struct biosvm_mapping){BIOSVM_SCRATCH, BIOSVM_SCRATCH + PAGE_SIZE, rw};
    mappings[2] = (struct biosvm_mapping){BIOSVM_STACK, BIOSVM_STACK + PAGE_SIZE, rw};
    mappings[3] = (struct biosvm_mapping){PAGE_ALIGN_DOWN(lowmem), PAGE_ALIGN_UP(end), rw};
    /* f1-07b: SeaBIOS VARLOW/extra stack are reserved firmware workspace.
     * https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/Kconfig
     * Only this private PDE exposes their physical identity as user RW. */
    mappings[4] = (struct biosvm_mapping){0xC0000, 0xF0000, rom | PTE_W};
    mappings[5] = (struct biosvm_mapping){0xF0000, 0x100000, rom};
    for (unsigned i = 0; i < BIOSVM_MAP_COUNT; i++)
        for (uint32_t p = mappings[i].start; p < mappings[i].end; p += PAGE_SIZE)
            pt[p >> 12] = p | mappings[i].flags;
    memset(P2V(BIOSVM_SCRATCH), 0, PAGE_SIZE);
    memset(P2V(BIOSVM_STACK), 0, PAGE_SIZE);
    size_t stub_bytes = (size_t)(biosvm_mouse_stub_end - biosvm_mouse_stub);
    if (stub_bytes > BIOSVM_MOUSE_RING - BIOSVM_MOUSE_OFFSET)
        return init_error("mouse_stub_size", -EFAULT);
    memcpy(P2V(BIOSVM_SCRATCH + BIOSVM_MOUSE_OFFSET), biosvm_mouse_stub, stub_bytes);
    *(uint8_t *)P2V(BIOSVM_SCRATCH) = 0xF4;
    return 0;
}

static int claim_input(void)
{
    static const struct { enum res_type type; uint32_t start; } wanted[] = {
        {RES_PORT, 0x60}, {RES_PORT, 0x64}, {RES_IRQ, 1}, {RES_IRQ, 12},
    };
    int handles[4] = {-1, -1, -1, -1};
    gen_t generations[4] = {0};
    /* Preflight all resources before transferring anything. Firmware IRQ
     * reservations, when published, belong to the same input parent lease.
     * registry_init currently publishes only the two port reservations;
     * absent IRQs still require a new claim, never an overlapping claim. */
    for (unsigned i = 0; i < registry_count(); i++) {
        const struct resource *r = registry_get(i);
        if (r->state == RS_RELEASED)
            continue;
        for (unsigned n = 0; n < ARRAY_SIZE(wanted); n++) {
            uint32_t start = wanted[n].start;
            if (r->type == wanted[n].type && r->start <= start && r->end > start) {
                if (r->state != RS_FIRMWARE || r->start != start || r->end != start + 1 ||
                    r->shareable || !r->owner || strncmp(r->owner, "input", 6) || handles[n] >= 0) {
                    klog("[biosvm] step=lease_conflict error=%d owner=firmware-input type=%u start=%x end=%x handle=%u state=%u",
                         -V86_EPERM, r->type, r->start, r->end, i, r->state);
                    char owner[64];
                    unsigned j = 0;
                    if (r->owner)
                        for (; j < sizeof(owner) - 1 && r->owner[j]; j++) owner[j] = r->owner[j];
                    owner[j] = 0;
                    klog("[biosvm] step=lease_conflict held_by=%s generation=%u", owner, r->generation);
                    return -V86_EPERM;
                }
                handles[n] = (int)i;
                generations[n] = r->generation;
            }
        }
    }
    if (handles[0] < 0 || handles[1] < 0)
        return init_error("input_reservations_missing", -V86_EPERM);
    for (unsigned n = 0; n < ARRAY_SIZE(wanted); n++) {
        int h = handles[n], rc;
        if (h >= 0) {
            rc = registry_claim_reserved(h, generations[n], "firmware-input");
        } else {
            unsigned irq = wanted[n].start;
            h = registry_claim(RES_IRQ, irq, irq + 1, "firmware-input", false);
            rc = h < 0 ? h : 0;
        }
        klog("[biosvm] step=lease_%s type=%u start=%x handle=%d old_generation=%u result=%d",
             handles[n] >= 0 ? "transfer" : "claim", wanted[n].type, wanted[n].start, h, generations[n], rc);
        if (rc)
            return rc;
        leases[lease_count].handle = h;
        leases[lease_count++].generation = registry_get(h)->generation;
        rc = registry_activate(h, registry_get(h)->generation);
        klog("[biosvm] step=lease_activate handle=%d generation=%u result=%d", h, registry_get(h)->generation, rc);
        if (rc)
            return rc;
    }
    return 0;
}

static void load_regs(const struct biosvm_regs *r)
{
    memset(&frame, 0, sizeof(frame));
    frame.tf.eax = r->eax; frame.tf.ebx = r->ebx; frame.tf.ecx = r->ecx;
    frame.tf.edx = r->edx; frame.tf.esi = r->esi; frame.tf.edi = r->edi; frame.tf.ebp = r->ebp;
    frame.ds = r->ds; frame.es = r->es; frame.fs = r->fs; frame.gs = r->gs;
    frame.tf.cs = firmware.sentinel_cs = BIOSVM_SCRATCH >> 4;
    frame.tf.eip = firmware.sentinel_ip = 0;
    frame.tf.user_ss = firmware.sentinel_ss = BIOSVM_STACK >> 4;
    frame.tf.user_esp = firmware.sentinel_sp = PAGE_SIZE;
    frame.tf.eflags = V86_VM | V86_IF | 2 | (r->flags & 0xCD5);
}

static void save_regs(struct biosvm_regs *r)
{
    r->eax = frame.tf.eax; r->ebx = frame.tf.ebx; r->ecx = frame.tf.ecx;
    r->edx = frame.tf.edx; r->esi = frame.tf.esi; r->edi = frame.tf.edi; r->ebp = frame.tf.ebp;
    r->ds = frame.ds; r->es = frame.es; r->fs = frame.fs; r->gs = frame.gs;
    r->flags = (uint16_t)((frame.tf.eflags & ~V86_IF) | (firmware.vif ? V86_IF : 0));
}

/* fx-25: one exact record per failed call: why, where, which port, and
 * the time actually used against the caller's budget. */
static struct biosvm_failure last_failure;
static uint32_t call_budget_ms;
static const char *failure_reason(int rc)
{
    if (rc == -V86_EPERM && firmware.denied) return "port_denied";
    if (rc == -V86_ETIMEDOUT) {
        if (firmware.abort_state != V86_HALTED) return "timeout";
        bool sentinel = firmware.fault.cs == firmware.sentinel_cs &&
                        firmware.fault.ip == (uint32_t)firmware.sentinel_ip + 1;
        return sentinel ? "halted_at_sentinel" : "halted";
    }
    if (rc == -EFAULT && firmware.fault.vector == 14) return "page_fault";
    if (rc == -EFAULT && firmware.fault.vector == 13) return "emulation";
    if (rc == -EFAULT) return "cpu_fault";
    return "error";
}
static void record_failure(const struct biosvm_regs *regs, int rc, uint32_t elapsed, uint32_t ms)
{
    last_failure = (struct biosvm_failure){
        .error = rc, .function = (uint16_t)regs->eax, .interrupt = regs->interrupt, .reason = failure_reason(rc),
        .cs = (uint16_t)firmware.fault.cs, .ip = (uint16_t)firmware.fault.ip, .ss = firmware.abort_ss,
        .sp = firmware.abort_sp, .vector = (uint8_t)firmware.fault.vector,
        .port = firmware.denied ? firmware.denied_port : 0, .width = firmware.denied ? firmware.denied_width : 0,
        .write = firmware.denied && firmware.denied_write, .elapsed_ms = elapsed, .deadline_ms = ms,
        .budget_ms = call_budget_ms ? call_budget_ms : ms};
    const struct biosvm_failure *x = &last_failure;
    klog("[biosvm] step=failure int=%02x fn=%04x error=%d reason=%s cs=%04x ip=%04x ss=%04x sp=%04x vector=%u "
         "port=%04x width=%u write=%u elapsed_ms=%u deadline_ms=%u budget_ms=%u",
         x->interrupt, x->function, x->error, x->reason, x->cs, x->ip, x->ss, x->sp, x->vector,
         x->port, x->width, (unsigned)x->write, x->elapsed_ms, x->deadline_ms, x->budget_ms);
}
void biosvm_last_failure(struct biosvm_failure *out)
{
    if (out) *out = last_failure;
}
static int run_vm_body(struct biosvm_regs *regs, uint32_t ms, unsigned test)
{
    int entry_irq = -1;
    struct biosvm_regs input = *regs;
    uint64_t start = deadline_after_ms(0);
    if (!test && regs->interrupt == 0x16 && ((regs->eax >> 8) & 0xFF) == 0x10) {
        uint16_t head, tail;
        memcpy(&head, P2V(0x41A), 2);
        memcpy(&tail, P2V(0x41C), 2);
        if (head == tail) {
            klog("[biosvm] step=keyboard_empty int=16 function=10 error=%d", -EINVAL);
            return -EINVAL; /* recheck at execution, after mailbox scheduling */
        }
    }
    int rc = v86_begin(&firmware, deadline_after_ms(0), ms);
    if (rc) {
        log_regs("begin", "in", regs);
        return init_error("begin", rc);
    }
    load_regs(regs);
    if (test) {
        /* Separate scripted entries: a refusal terminates that call, so the
         * deadline cannot be tested later in the same instruction stream. */
        static const uint8_t policy[] = {0xFA, 0xFB, 0x90, 0xB0, 0xFF, 0xE6, 0x21,
                                         0xB0, 0x36, 0xE6, 0x43, 0xCD, 0x1C, 0xCF};
        static const uint8_t denied[] = {0xB0, 0, 0xE6, 0x81, 0xCF};
        static const uint8_t timeout[] = {0xFA, 0xF4, 0xEB, 0xFD};
        const uint8_t *code = test == 1 ? policy : test == 2 ? denied : timeout;
        unsigned size = test == 1 ? sizeof(policy) : test == 2 ? sizeof(denied) : sizeof(timeout);
        memcpy(P2V(BIOSVM_SCRATCH + 0x200), code, size);
        uint8_t *stack = P2V(BIOSVM_STACK + PAGE_SIZE - 6);
        uint16_t initial[] = {0, BIOSVM_SCRATCH >> 4, 0x202};
        memcpy(stack, initial, sizeof(initial));
        frame.tf.user_esp -= 6;
        frame.tf.eip = 0x200;
    } else if (regs->interrupt) {
        rc = v86_reflect(&firmware, &frame, regs->interrupt);
    } else {
        capture_pending();
        entry_irq = v86_irq_pending(&firmware);
        rc = reflect_input_irq(&frame);
        if (!rc) {
            firmware.state = V86_DONE;
            return 0;
        }
    }
    if (rc >= 0) {
        if (!synthetic && (entry_irq == 1 || entry_irq == 12))
            delivery.entries[entry_irq == 12]++;
        uint32_t old_cr3 = read_cr3();
        write_cr3(vm_as.pd_phys);
        v86_enter(&frame, &continuation);
        cli();
        continuation = 0;
        write_cr3(old_cr3);
        tss_set_kernel_stack((uint32_t)(uintptr_t)vm_thread->kstack + KSTACK_SIZE);
        sti();
        rc = firmware.result;
        if (!synthetic && firmware.state == V86_DONE && (entry_irq == 1 || entry_irq == 12))
            delivery.done[entry_irq == 12]++;
    }
    save_regs(regs);
    if (rc < 0) {
        uint64_t elapsed = deadline_after_ms(0) - start;
        log_regs("execute", "in", &input);
        log_regs("execute", "out", regs);
        klog("[biosvm] step=execute error=%d vector=%u cs=%04x ip=%04x address=%08x elapsed=%llu deadline_ms=%u",
             rc, firmware.fault.vector, firmware.fault.cs, firmware.fault.ip, firmware.fault.address,
             elapsed, ms);
        klog("[biosvm] step=fault bytes=%02x%02x%02x%02x count=%u state=%u",
             firmware.fault.bytes[0], firmware.fault.bytes[1], firmware.fault.bytes[2], firmware.fault.bytes[3],
             firmware.fault.count, firmware.state);
        record_failure(&input, rc, (uint32_t)elapsed, ms);
    } else last_failure = (struct biosvm_failure){0};
    return rc;
}

/* fx-18: the APM BIOS's chipset window (core/apm_ports.c), installed by
 * the APM worker before its first call. Only APM calls see it. */
static struct apm_port_window apm_window;
static bool apm_window_set;
static uint16_t apm_function;
static struct biosvm_apm_io apm_io_stats;
static uint16_t apm_logged[16];
static unsigned apm_logged_count, apm_denials_logged;
int biosvm_apm_set_window(const struct apm_port_window *w)
{
    if (!w || w->count > APM_PORT_RANGES) return -EINVAL;
    /* Only the APM worker sets it, and its calls are synchronous: no APM
     * call can be reading the window while it changes. */
    uint32_t f = irq_save(); apm_window = *w; apm_window_set = true; irq_restore(f);
    return 0;
}
void biosvm_apm_io_snapshot(struct biosvm_apm_io *out)
{
    if (!out) return;
    uint32_t f = irq_save(); *out = apm_io_stats; irq_restore(f);
}
static void apm_note(uint16_t port, unsigned width, bool write, uint8_t kind)
{
    for (unsigned i = 0; i < apm_logged_count; i++) if (apm_logged[i] == port) return;
    if (apm_logged_count == sizeof(apm_logged) / sizeof(apm_logged[0])) return;
    apm_logged[apm_logged_count++] = port;
    klog("[apm] io port=%04x width=%u write=%u fn=%04x kind=%s", port, width, (unsigned)write,
         apm_function, apm_port_kind_name(kind));
}
static int apm_io(void *arg, uint16_t port, unsigned width, bool write, uint32_t *value)
{
    (void)arg;
    /* APM has no physical resource claim of its own.
     * Native i8042 ownership must not be bypassed by the firmware monitor's
     * input port rules. PIC/PIT/RTC remain virtual. fx-18: the chipset
     * power-management window the APM BIOS needs is admitted, bounded and
     * logged; every other port stays denied. */
    if ((!input_enabled || family_disabled(BIOS_INPUT)) && (port == 0x60 || port == 0x64)) return -V86_EPERM;
    if (!delay_io(arg, port, width, write, value)) {
        apm_io_stats.absorbed++;
        return 0;
    }
    if (!apm_window_set) return -ENOSYS;
    uint8_t kind = APM_PORT_NONE; const char *reason = "outside_window";
    enum apm_port_verdict v = apm_port_check(&apm_window, port, width, write, *value, apm_function, &kind, &reason);
    if (kind == APM_PORT_NONE) return -ENOSYS; /* virtual devices, then the generic denial */
    if (v == APM_PORT_DENY) {
        apm_io_stats.denied++; firmware.stats.disallowed_io++;
        firmware.denied = true; firmware.denied_port = port; firmware.denied_width = (uint8_t)width;
        firmware.denied_write = write;
        if (apm_denials_logged < 8) {
            apm_denials_logged++;
            klog("[apm] io denied port=%04x width=%u write=%u value=%08x fn=%04x kind=%s reason=%s",
                 port, width, (unsigned)write, *value, apm_function, apm_port_kind_name(kind), reason);
        }
        return -V86_EPERM;
    }
    apm_note(port, width, write, kind);
    if (v == APM_PORT_ABSORB) {
        apm_io_stats.absorbed++;
        if (!write) *value = width == 1 ? 0xffu : width == 2 ? 0xffffu : 0xffffffffu;
        return 0;
    }
    apm_io_stats.allowed++;
    if (write) {
        apm_io_stats.writes++;
        if (width == 1) outb(port, (uint8_t)*value);
        else if (width == 2) cpu_outw(port, (uint16_t)*value);
        else outl(port, *value);
    } else *value = width == 1 ? inb(port) : width == 2 ? cpu_inw(port) : inl(port);
    return 0;
}

static int run_vm(struct biosvm_regs *regs, uint32_t ms, unsigned test)
{
    bool apm = regs->interrupt == 0x15 &&
               ((regs->eax >= 0x5300 && regs->eax <= 0x5310) || regs->eax == 0x5380);
    enum bios_family family = apm ? BIOS_APM : video_active ? BIOS_VIDEO : BIOS_INPUT;
    if (family_disabled(family)) return -V86_EIO;
    if (apm && firmware.io) return -EBUSY;
    if (apm && apm_deadline_active) {
        uint64_t now = deadline_after_ms(0);
        if (deadline_passed(apm_deadline)) return -V86_ETIMEDOUT;
        uint64_t left = apm_deadline - now;
        if (left < ms) ms = (uint32_t)left;
    }
    uint32_t saved_limit = firmware.max_call_ms;
    if (apm) {
        apm_function = (uint16_t)regs->eax; firmware.io = apm_io;
        /* The dedicated broker admits <=2000 ms, including connect/off.
         * Scope that ceiling here; generic input keeps its 500 ms bound.
         * ms above already excludes time spent in the request queue. */
        firmware.max_call_ms = 2000;
    }
    /* v86_abort stops one execution. A healthy family may start only here,
     * after the previous worker entry returned. No hardware lease is reset. */
    if (firmware.state == V86_DISABLED) firmware.state = V86_IDLE;
    struct v86_pic saved_pic[2];
    memcpy(saved_pic, firmware.pic, sizeof(saved_pic));
    if (!firmware.io) firmware.io = delay_io;
    int e = run_vm_body(regs, ms, test);
    if (firmware.io == delay_io) firmware.io = 0;
    if (firmware.state == V86_DISABLED && !synthetic) {
        quarantine_family(family, failure_reason(e));
        if (family != BIOS_INPUT) {
            /* Discard a failed service's virtual masks/in-service state;
             * preserve queued IRQ edges for the healthy input family. */
            for (unsigned i = 0; i < 2; i++) {
                saved_pic[i].irr |= firmware.pic[i].irr;
                firmware.pic[i] = saved_pic[i];
            }
        }
    }
    if (apm) { firmware.io = 0; firmware.max_call_ms = saved_limit; }
    return e;
}

static bool input_irq_pending(void)
{
    /* capture_pending transfers edges into the virtual PIC. At an ISR's
     * IRET to the sentinel another line can still be waiting there, even
     * though the physical mailbox is empty. Honor IMR/ISR/cascade priority;
     * checking raw IRR would spin forever on a firmware-masked request.
     * SeaBIOS rel-1.16.3 src/hw/ps2port.c: handle_09/74 issue virtual EOIs. */
    return pending_irqs || v86_irq_pending(&firmware) >= 0;
}

static bool worker_ready(void *arg)
{
    (void)arg;
    return request_pending || (input_enabled && !family_disabled(BIOS_INPUT) && !synthetic && input_irq_pending());
}

static void worker_main(void *arg)
{
    (void)arg;
    uint64_t poll = deadline_after_ms(10);
    for (;;) {
        bool input_irq = false;
        uint64_t reflected = firmware.stats.reflected_irqs;
        if (request_pending) {
            request_result = run_vm(&request_regs, request_ms, request_test);
            request_pending = false;
            request_done = true;
            kwait_wake_all(&completion);
        }
        if (input_enabled && !family_disabled(BIOS_INPUT) && !synthetic && input_irq_pending()) {
            struct biosvm_regs r = {0};
            run_vm(&r, 100, 0);
            input_irq = true;
        }
        input_irq |= firmware.stats.reflected_irqs != reflected;
        if (input_enabled && !family_disabled(BIOS_INPUT) && !synthetic && input_service && (input_irq || deadline_passed(poll))) {
            input_service();
            notify_input();
            poll = deadline_after_ms(10);
        }
        if (deadline_passed(poll))
            poll = deadline_after_ms(10);
        /* Bound each batch to one standalone reflection. Even a stream of
         * individually short ISRs must let lower priorities run. Firmware
         * calls retain their own deadline; backlog blocks for one tick. */
        if (input_enabled && !family_disabled(BIOS_INPUT) && !synthetic && input_irq_pending()) {
            delivery.budget_yields++;
            task_sleep_ms(1);
        }
        kwait_wait_until(&wake, worker_ready, 0, family_disabled(BIOS_INPUT) ? deadline_after_ms(1000) : poll);
    }
}

static int biosvm_init_for(bool input)
{
    if (quarantined)
        return init_error("already_quarantined", -V86_EIO);
    if (initialized)
        return input && !input_enabled ? -V86_EPERM : 0;
    if (!g_current || !(read_eflags() & V86_IF))
        return init_error("thread_context", -V86_EPERM);
    if (input && !drivers_input_firmware())
        return init_error("input_policy", -V86_EPERM);
    if (read_cr4() & 3)
        return init_error("vme_pvi", -V86_EPERM); /* no alternate VME/PVI monitor path */
    const struct v86_ops ops = {vm_memory, vm_in, vm_out, 0};
    v86_init(&firmware, &ops);
    firmware.qemu_pmtimer = !!(g_boot.flags & CBI_F_SMBIOS_QEMU);
    int rc = map_firmware();
    if (rc)
        goto failed;
    vm_thread = task_create_kernel("bios-input", worker_main, 0, P_DEVICE);
    if (!vm_thread) {
        rc = init_error("worker_create", -ENOMEM);
        goto failed;
    }
    kmutex_init(&request_mutex);
    if (input) {
        rc = claim_input();
        if (rc) goto failed;
    }
    kwait_init(&wake);
    kwait_init(&completion);
    uint32_t f = irq_save();
    initialized = true;
    input_enabled = input;
    if (input) { irq_set_handler(1, irq_input); irq_set_handler(12, irq_input); }
    task_start(vm_thread);
    if (input) { pic_unmask(1); pic_unmask(12); }
    irq_restore(f);
    klog("[biosvm] step=ready leases=%u qemu_pmtimer=%u", lease_count, firmware.qemu_pmtimer);
    return 0;
failed:
    /* No firmware ran. Free only privately allocated pages, not borrowed
     * IVT/BDA/ROM. Failed lease transfers stay quarantined. */
    if (lease_count)
        quarantine();
    if (vm_thread) {
        task_kill(vm_thread, rc);
        task_reap(vm_thread);
        vm_thread = 0;
    }
    if (low_pt)
        mm_free(low_pt,MM_TABLE);
    if (vm_as.pd_phys)
        mm_free(vm_as.pd_phys,MM_TABLE);
    low_pt = vm_as.pd_phys = 0;
    return rc;
}

int biosvm_init(void)
{
    if (family_disabled(BIOS_INPUT)) return init_error("input_quarantined", -V86_EIO);
    return biosvm_init_for(true);
}

int biosvm_apm_call(struct biosvm_regs *regs, uint32_t ms)
{
    if (!regs || !ms || ms > 2000) return -EINVAL;
    uint32_t fn = regs->eax;
    if (regs->interrupt != 0x15 ||
        !((fn >= 0x5300 && fn <= 0x5310) || fn == 0x5380) ||
        fn == 0x5302 || fn == 0x5303 ||
        (fn == 0x5380 && ((regs->ebx >> 8) & 0xff) != 0x7f)) return -V86_EPERM;
    if (!g_current || !(read_eflags() & V86_IF)) return -V86_EPERM;
    uint64_t end = deadline_after_ms(ms);
    int e = biosvm_init_for(drivers_input_firmware());
    if (e) return e;
    if (g_current == vm_thread) return -EBUSY;
    if (!kmutex_lock_until(&request_mutex, end)) return -V86_ETIMEDOUT;
    if (family_disabled(BIOS_APM)) e = -V86_EIO;
    else if ((video_claimed && !family_disabled(BIOS_VIDEO)) || fullscreen_video || firmware.io) e = -EBUSY;
    else if (deadline_passed(end)) e = -V86_ETIMEDOUT;
    else {
        /* The worker subtracts queue time before entering V86. Expired
         * requests never execute; live faults/timeouts retain quarantine. */
        apm_deadline = end;
        apm_deadline_active = true;
        call_budget_ms = ms;
        e = submit(regs, ms, 0);
        call_budget_ms = 0;
        apm_deadline_active = false;
    }
    kmutex_unlock(&request_mutex);
    return e;
}

int biosvm_apm_probe(struct biosvm_regs *out)
{
    if (!out) return -EINVAL;
    *out = (struct biosvm_regs){.interrupt = 0x15, .eax = 0x5300};
    return biosvm_apm_call(out, 100);
}

static bool input_fallback_idle(int handle, gen_t generation)
{
    (void)handle; (void)generation;
    /* No PS/2 DMA. Detached software producers cannot access the ports;
     * native init performs its existing inhibit/drain before replies. */
    return !input_enabled && !continuation && !request_pending;
}

int biosvm_input_fallback(void)
{
    if (!g_current || !(read_eflags() & V86_IF)) return -EINVAL;
    if (!lease_count) return 0;       /* mapping/preflight failed before ownership */
    kmutex_lock(&request_mutex);
    uint32_t f = irq_save();
    input_enabled = false;
    irq_set_handler(1, 0);
    irq_set_handler(12, 0);
    input_service = 0;
    input_observer = 0;
    input_wait = 0;
    pending_irqs = 0;
    bool busy = continuation || request_pending;
    if (!busy) {
        firmware.pic[0].irr = firmware.pic[0].isr = 0;
        firmware.pic[1].irr = firmware.pic[1].isr = 0;
    }
    irq_restore(f);
    int rc = family_disabled(BIOS_INPUT) ? -V86_EIO : busy ? -EBUSY : 0;
    for (unsigned i = 0; !rc && i < lease_count; i++)
        rc = registry_quiesce(leases[i].handle, leases[i].generation, input_fallback_idle);
    for (unsigned i = 0; !rc && i < lease_count; i++)
        rc = registry_release(leases[i].handle, leases[i].generation);
    if (!rc) lease_count = 0;
    else quarantine_family(BIOS_INPUT, "input_lease_transfer");
    kmutex_unlock(&request_mutex);
    klog("[biosvm] step=input_fallback error=%d leases=%u", rc, lease_count);
    return rc;
}

static bool finished(void *arg)
{
    (void)arg;
    return request_done;
}

static int submit(struct biosvm_regs *regs, uint32_t ms, unsigned test)
{
    request_regs = *regs;
    request_ms = ms;
    request_test = test;
    request_done = false;
    request_pending = true;
    kwait_wake_all(&wake);
    /* Register storage is pinned in the mailbox. Only the worker publishes
     * completion, including timeout; no pointer to a caller stack survives. */
    while (!kwait_wait_until(&completion, finished, 0, deadline_after_ms(1000)))
        ;
    *regs = request_regs;
    return request_result;
}

static int reject_call(const char *step, const struct biosvm_regs *regs, int rc)
{
    if (regs)
        log_regs(step, "in", regs);
    klog("[biosvm] step=%s error=%d state=%u quarantined=%u", step, rc, firmware.state, quarantined);
    return rc;
}

int biosvm_call(struct biosvm_regs *regs, uint32_t deadline_ms)
{
    if (!initialized || family_disabled(BIOS_INPUT))
        return reject_call("call_disabled", regs, -V86_EIO);
    if (!regs || !g_current || !(read_eflags() & V86_IF) || !deadline_ms)
        return reject_call("call_context", regs, -EINVAL);
    uint8_t ah = regs->eax >> 8;
    bool keyboard = regs->interrupt == 0x16 && (ah == 0x10 || ah == 0x11);
    bool mouse = regs->interrupt == 0x15 && ah == 0xC2 && (regs->eax & 0xFF) <= 7;
    if (!input_enabled || (!keyboard && !mouse) || deadline_ms > (mouse ? 500u : 100u))
        return reject_call("call_policy", regs, -V86_EPERM);
    if (g_current != vm_thread)
        kmutex_lock(&request_mutex);
    int rc;
    if (family_disabled(BIOS_INPUT)) {
        rc = reject_call("call_quarantined", regs, -V86_EIO);
        goto done;
    }
    if (keyboard && ah == 0x10) {
        /* AH=10 is intrinsically blocking. The serialized sole consumer
         * may use it only while the real BDA has a queued word. */
        uint16_t head, tail;
        memcpy(&head, P2V(0x41A), 2);
        memcpy(&tail, P2V(0x41C), 2);
        if (head == tail) {
            rc = reject_call("call_keyboard_empty", regs, -EINVAL);
            goto done;
        }
    }
    if (g_current == vm_thread)
        return run_vm(regs, deadline_ms, 0);
    rc = submit(regs, deadline_ms, 0);
done:
    if (g_current != vm_thread)
        kmutex_unlock(&request_mutex);
    return rc;
}

enum biosvm_backend biosvm_backend_state(void)
{
    return family_disabled(BIOS_INPUT) ? BIOSVM_DISABLED_BACKEND :
           initialized ? BIOSVM_READY : BIOSVM_OFF;
}

void biosvm_stats(struct v86_stats *out)
{
    uint32_t f = irq_save();
    *out = firmware.stats;
    irq_restore(f);
}

int biosvm_set_rtc_cache(const uint8_t values[128])
{
    if (!initialized || !values || continuation)
        return -EINVAL;
    memcpy(firmware.rtc, values, 128);
    firmware.rtc_valid = true;
    return 0;
}

void biosvm_set_input_service(void (*service)(void)) { input_service = service; }

void biosvm_set_input_wait(struct kwait *wait) { input_wait = wait; }

uint64_t biosvm_input_reflections(void)
{
    uint32_t f = irq_save();
    uint64_t count = input_reflections;
    irq_restore(f);
    return count;
}

void biosvm_account_irq(unsigned irq, bool eoi, bool handled)
{
    if (irq != 1 && irq != 12) return;
    unsigned slot = irq == 12;
    if (eoi) delivery.eois[slot]++;
    else {
        delivery.arrivals[slot]++;
        if (handled) delivery.dispatched[slot]++;
    }
}

void biosvm_input_snapshot(struct biosvm_input_diag *out)
{
    uint32_t f = irq_save();
    *out = delivery;
    out->pending = pending_irqs;
    out->state = firmware.state;
    out->active = continuation != 0;
    out->disabled = family_disabled(BIOS_INPUT);
    for (unsigned i = 0; i < 2; i++) {
        out->imr[i] = firmware.pic[i].imr;
        out->irr[i] = firmware.pic[i].irr;
        out->isr[i] = firmware.pic[i].isr;
        out->base[i] = firmware.pic[i].base;
    }
    out->physical_imr[0] = inb(0x21);
    out->physical_imr[1] = inb(0xA1);
    if (initialized) {
        out->bda_head = *(volatile uint16_t *)P2V(0x41A);
        out->bda_tail = *(volatile uint16_t *)P2V(0x41C);
        volatile struct biosvm_mouse_ring *r = P2V(BIOSVM_SCRATCH + BIOSVM_MOUSE_RING);
        out->mouse_head = r->head; out->mouse_tail = r->tail; out->mouse_lost = r->lost;
    }
    irq_restore(f);
}

bool biosvm_keyboard_pending(void)
{
    if (!initialized || family_disabled(BIOS_INPUT)) return false;
    /* Same borrowed physical BDA page as run_vm, with volatile loads because
     * the firmware producer writes it outside the C abstract machine. */
    volatile uint16_t *head = P2V(0x41A), *tail = P2V(0x41C);
    return *head != *tail;
}

bool biosvm_mouse_pending(void)
{
    if (!initialized || family_disabled(BIOS_INPUT)) return false;
    volatile struct biosvm_mouse_ring *r = P2V(BIOSVM_SCRATCH + BIOSVM_MOUSE_RING);
    return r->head != r->tail || r->lost != mouse_lost ||
           r->head >= BIOSVM_MOUSE_CAP || r->tail >= BIOSVM_MOUSE_CAP;
}

void biosvm_set_input_observer(void (*observer)(uint8_t status, uint8_t byte, bool keyboard_irq))
{
    input_observer = observer;
}

static bool check_mappings(struct biosvm_selftest_report *r)
{
    memcpy(r->mappings, mappings, sizeof(mappings));
    const uint32_t *pd = P2V(vm_as.pd_phys);
    if ((pd[0] & ~0x20u) != (low_pt | PTE_P | PTE_U | PTE_W))
        return false;
    const uint32_t *pt = P2V(low_pt);
    for (unsigned page = 0; page < 1024; page++) {
        uint32_t address = page * PAGE_SIZE, expected = 0;
        for (unsigned i = 0; i < BIOSVM_MAP_COUNT; i++)
            if (address >= mappings[i].start && address < mappings[i].end)
                expected = address | mappings[i].flags;
        /* Hardware sets accessed/dirty; neither changes the permissions. */
        if ((pt[page] & ~0x60u) != expected)
            return false;
    }
    return true;
}

unsigned biosvm_mouse_packets(uint8_t (*out)[3], unsigned max, unsigned *lost)
{
    if (!initialized || g_current != vm_thread)
        return 0;
    volatile struct biosvm_mouse_ring *r = P2V(BIOSVM_SCRATCH + BIOSVM_MOUSE_RING);
    unsigned n = 0;
    *lost = (uint16_t)(r->lost - mouse_lost);
    mouse_lost = r->lost;
    if (r->head >= BIOSVM_MOUSE_CAP || r->tail >= BIOSVM_MOUSE_CAP) {
        (*lost)++;
        r->head = r->tail = 0;
        return 0;
    }
    while (n < max && r->tail != r->head) {
        unsigned t = r->tail;
        for (unsigned i = 0; i < 3; i++)
            out[n][i] = r->packets[t][i];
        n++;
        r->tail = (t + 1) % BIOSVM_MOUSE_CAP;
    }
    return n;
}

static bool test_selected(void)
{
    static const char selector[] = "f1:input-fault ";
    return (g_boot.flags & (CBI_F_SMBIOS_QEMU | CBI_F_TEST_REQUEST | CBI_F_INPUT_FORCED)) ==
           (CBI_F_SMBIOS_QEMU | CBI_F_TEST_REQUEST | CBI_F_INPUT_FORCED) &&
           g_boot.test_request_len >= sizeof(selector) - 1 &&
           !strncmp(g_boot.test_request, selector, sizeof(selector) - 1);
}

int biosvm_reset_for_test(void)
{
    if (!test_selected() || !synthetic || quarantined || continuation || request_pending)
        return -V86_EPERM;
    firmware.state = V86_IDLE;
    firmware.result = 0;
    return 0;
}

static uint8_t pit_status(void)
{
    /* 8254 read-back STATUS only: output/count changes are not configuration
     * changes. Command E2 latches channel 0 status without reprogramming it. */
    uint32_t f = irq_save();
    outb(0x43, 0xE2);
    uint8_t status = inb(0x40) & 0x3F;
    irq_restore(f);
    return status;
}

int biosvm_selftest(struct biosvm_selftest_report *r)
{
    if (!r || !test_selected() || !initialized || quarantined || g_current == vm_thread)
        return -V86_EPERM;
    kmutex_lock(&request_mutex);
    memset(r, 0, sizeof(*r));
    r->mappings_ok = check_mappings(r);
    /* Synthetic code never consumes the controller or calls real firmware.
     * INT1C temporarily targets a private IRET; restore the physical IVT.
     * No registry reset is involved; production faults still quarantine. */
    synthetic = true;
    uint32_t old_vector;
    memcpy(&old_vector, P2V(0x1C * 4), 4);
    uint32_t vector = ((BIOSVM_SCRATCH >> 4) << 16) | 0x300;
    *(uint8_t *)P2V(BIOSVM_SCRATCH + 0x300) = 0xCF;
    memcpy(P2V(0x1C * 4), &vector, 4);
    struct v86_pic saved_pic[2];
    memcpy(saved_pic, firmware.pic, sizeof(saved_pic));
    firmware.pic[0].imr = firmware.pic[1].imr = 0xFF;
    r->pic_before[0] = inb(0x21); r->pic_before[1] = inb(0xA1);
    r->pit_before = pit_status();
    uint64_t denied = firmware.stats.disallowed_io, timeouts = firmware.stats.timeouts;
    struct biosvm_regs regs = {0};
    r->policy_result = submit(&regs, 100, 1);
    r->denied_result = submit(&regs, 100, 2);
    biosvm_reset_for_test();
    r->timeout_result = submit(&regs, 10, 3);
    r->disabled = firmware.state == V86_DISABLED;
    r->later_result = biosvm_call(&regs, 100);
    r->disallowed = (uint32_t)(firmware.stats.disallowed_io - denied);
    r->timeouts = (uint32_t)(firmware.stats.timeouts - timeouts);
    r->pic_after[0] = inb(0x21); r->pic_after[1] = inb(0xA1);
    r->pit_after = pit_status();
    r->pic_unchanged = !memcmp(r->pic_before, r->pic_after, 2);
    r->pit_unchanged = r->pit_before == r->pit_after;
    biosvm_reset_for_test();
    memcpy(firmware.pic, saved_pic, sizeof(saved_pic));
    memcpy(P2V(0x1C * 4), &old_vector, 4);
    synthetic = false;
    kmutex_unlock(&request_mutex);
    return !r->policy_result && r->denied_result == -V86_EPERM &&
           r->timeout_result == -V86_ETIMEDOUT && r->later_result == -V86_EIO &&
           r->pic_unchanged && r->pit_unchanged && r->disabled && r->mappings_ok &&
           r->disallowed == 1 && r->timeouts == 1 ? 0 : -EFAULT;
}

/* Runtime video gets exclusive leases only after fbdev stops acceleration.
 * Release after the serialized call and temporary aperture are inactive;
 * unexpected BAR/chipset I/O stays denied.
 * QEMU std VGA uses Bochs DISPI index/data words at 1CEh/1CFh. */
static int claim_video(void)
{
    if (video_claimed) return 0;
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *p = pci_get(i);
        if (p && p->class_code == 3 && p->vendor == 0x1234 && p->device == 0x1111 &&
            (g_boot.flags & CBI_F_SMBIOS_QEMU)) video_dispi = true;
    }
    /* VGA I/O as two ranges (Windows assignment): 3BCh-3BFh is a parallel port. */
    const enum res_type types[] = {RES_PORT, RES_PORT, RES_MMIO, RES_PORT};
    const uint32_t start[] = {CIUKI_VGA_MONO_FIRST, CIUKI_VGA_COLOR_FIRST, 0xA0000, 0x1CE};
    const uint32_t end[] = {CIUKI_VGA_MONO_END, CIUKI_VGA_COLOR_END, 0xC0000, 0x1D2};
    for (unsigned i = 0; i < (video_dispi ? 4u : 3u); i++) {
        int h = registry_claim(types[i], start[i], end[i], "firmware-video", false);
        if (h < 0) { quarantine_family(BIOS_VIDEO, "video_lease_claim"); return h; }
        gen_t gen = registry_get((unsigned)h)->generation;
        video_leases[video_lease_count].handle = h;
        video_leases[video_lease_count++].generation = gen;
        if (registry_activate(h, gen)) { quarantine_family(BIOS_VIDEO, "video_lease_claim"); return -V86_EIO; }
    }
    video_claimed = true;
    return 0;
}

int biosvm_video_prepare(void)
{
    if (!g_current || g_current == vm_thread || !(read_eflags() & V86_IF)) return -EINVAL;
    int e = biosvm_init_for(drivers_input_firmware());
    if (e) return e;
    kmutex_lock(&request_mutex);
    /* Display preparation precedes fbdev_change_begin: Savage may still own
     * the VGA ports. Only prepare the VM here; switch claims after stop. */
    e = family_disabled(BIOS_VIDEO) ? -V86_EIO : 0;
    kmutex_unlock(&request_mutex);
    return e;
}
static bool video_idle(int handle, gen_t generation)
{
    (void)handle; (void)generation;
    /* request_mutex held, worker returned, callback and VGA mappings revoked. */
    return !video_active && !request_pending && !firmware.io;
}
static int release_video(void)
{
    if (family_disabled(BIOS_VIDEO)) return -V86_EIO;
    for (unsigned i = 0; i < video_lease_count; i++) {
        int h = video_leases[i].handle;
        gen_t gen = video_leases[i].generation;
        if (registry_quiesce(h, gen, video_idle) || registry_release(h, gen)) {
            quarantine_family(BIOS_VIDEO, "video_lease_release");
            return -V86_EIO;
        }
    }
    video_claimed = false;
    video_lease_count = 0;
    return 0;
}

static int video_io(void *arg, uint16_t port, unsigned width, bool write, uint32_t *value)
{
    (void)arg;
    if (!video_active) return -ENOSYS;
    if (!delay_io(arg, port, width, write, value)) return 0;
    if (port == 0x500 && width == 1 && write) return 0; /* VGA ROM diagnostic sink */
    bool vga = width <= 2 && ciuki_vga_port_span(port, width);
    bool dispi = video_dispi && (port == 0x1CE || port == 0x1CF || port == 0x1D0) && width == 2;
    if (vga || dispi) {
        if (write) {
            if (width == 1) outb(port, (uint8_t)*value); else cpu_outw(port, (uint16_t)*value);
        } else *value = width == 1 ? inb(port) : cpu_inw(port);
        return 0;
    }
    /* Native input remains its sole consumer, even during INT10. */
    bool allowed=width==1;
    if ((!input_enabled || family_disabled(BIOS_INPUT)) && (port==0x60 || port==0x64)) allowed=false;
    else {
        bool rule=false;
        for (unsigned i=0;i<firmware.nports;i++)
            if (port>=firmware.ports[i].first && port<=firmware.ports[i].last) rule=true;
        allowed&=rule;
    }
    if (firmware.qemu_pmtimer && port==0x608 && width==4 && !write) allowed=true;
    if (!allowed) {
        klog("[biosvm] fullscreen_port_fault port=%04x width=%u write=%u value=%08x",
            port,width,(unsigned)write,*value);
        display_fullscreen_checkpoint("bios-port-fault",-V86_EPERM);
        if ((!input_enabled || family_disabled(BIOS_INPUT)) && (port==0x60 || port==0x64)) return -V86_EPERM;
    }
    return -ENOSYS; /* PIC/PIT remain virtual; unknown dependencies fault */
}

int biosvm_video_switch(uint16_t mode, uint8_t info[256])
{
    if (!info || mode < 0x100 || mode >= 0x200 || !g_current || g_current == vm_thread ||
        !(read_eflags() & V86_IF)) return -EINVAL;
    int e = biosvm_init_for(drivers_input_firmware());
    if (e) return e;
    kmutex_lock(&request_mutex);
    if (fullscreen_video) { e=-EBUSY; goto done; }
    if (family_disabled(BIOS_VIDEO)) { e = -V86_EIO; goto done; }
    if ((e = claim_video())) goto done;
    uint32_t *pt = P2V(low_pt);
    for (uint32_t p = 0xA0000; p < 0xC0000; p += PAGE_SIZE)
        pt[p >> 12] = p | PTE_P | PTE_U | PTE_W | PTE_PCD | PTE_PWT;
    video_active = true;
    firmware.io = video_io;
    firmware.max_call_ms = 2000;
    uint64_t deadline = deadline_after_ms(2000);
    struct biosvm_regs r = {.interrupt = 0x10, .eax = 0x4F02, .ebx = mode | 0x4000};
    e = submit(&r, 2000, 0);
    if (e || (uint16_t)r.eax != 0x004F) { e = -V86_EIO; goto revoke; }
    memset(P2V(BIOSVM_SCRATCH + 0x800), 0, 256);
    r = (struct biosvm_regs){.interrupt = 0x10, .eax = 0x4F01, .ecx = mode,
        .es = BIOSVM_SCRATCH >> 4, .edi = 0x800};
    if (deadline_passed(deadline)) { e = -V86_EIO; goto revoke; }
    e = submit(&r, (uint32_t)(deadline - deadline_after_ms(0)), 0);
    if (e || (uint16_t)r.eax != 0x004F) { e = -V86_EIO; goto revoke; }
    memcpy(info, P2V(BIOSVM_SCRATCH + 0x800), 256);
    r = (struct biosvm_regs){.interrupt = 0x10, .eax = 0x4F03};
    if (deadline_passed(deadline)) { e = -V86_EIO; goto revoke; }
    e = submit(&r, (uint32_t)(deadline - deadline_after_ms(0)), 0);
    if (e || (uint16_t)r.eax != 0x004F || (r.ebx & 0x7FFF) != (mode | 0x4000)) e = -V86_EIO;
revoke:
    video_active = false;
    firmware.io = 0;
    firmware.max_call_ms = 0;
    for (uint32_t p = 0xA0000; p < 0xC0000; p += PAGE_SIZE) pt[p >> 12] = 0;
    if (!family_disabled(BIOS_VIDEO) && release_video()) e = -V86_EIO;
done:
    kmutex_unlock(&request_mutex);
    return e;
}

/* G10: caller has stopped acceleration and suspended scanout ownership.
 * Query and set are bounded by the existing firmware VM call budget. */
int biosvm_video_dpms(unsigned state)
{
    if (state!=0 && state!=1 && state!=4) return -EINVAL;
    int e=biosvm_video_prepare();
    if (e) return e;
    kmutex_lock(&request_mutex);
    if (fullscreen_video) { e=-EBUSY; goto done; }
    if ((e=claim_video())) goto done;
    uint32_t *pt=P2V(low_pt);
    for (uint32_t p=0xA0000;p<0xC0000;p+=PAGE_SIZE)
        pt[p>>12]=p|PTE_P|PTE_U|PTE_W|PTE_PCD|PTE_PWT;
    video_active=true; firmware.io=video_io; firmware.max_call_ms=1000;
    struct biosvm_regs r={.interrupt=0x10,.eax=0x4f10};
    e=submit(&r,1000,0);
    if (e || (uint16_t)r.eax!=0x004f) e=-EOPNOTSUPP;
    else if (state && !((r.ebx>>8)&state)) e=-EOPNOTSUPP;
    if (!e) {
        r=(struct biosvm_regs){.interrupt=0x10,.eax=0x4f10,.ebx=(state<<8)|1};
        e=submit(&r,1000,0);
        if (!e && (uint16_t)r.eax!=0x004f) e=-EIO;
    }
    video_active=false; firmware.io=0; firmware.max_call_ms=0;
    for (uint32_t p=0xA0000;p<0xC0000;p+=PAGE_SIZE) pt[p>>12]=0;
    if (!family_disabled(BIOS_VIDEO) && release_video()) e=-EIO;
done:
    kmutex_unlock(&request_mutex); return e;
}

/* f3-20: persistent display ownership, not generic guest firmware forwarding.
 * The existing 2 KiB private scratch tail bounds the firmware state buffer. */
static uint8_t fullscreen_saved[2048];
static unsigned fullscreen_bytes;
static void fullscreen_aperture(bool active)
{
    uint32_t *pt = P2V(low_pt);
    for (uint32_t p=0xA0000;p<0xC0000;p+=PAGE_SIZE)
        pt[p>>12]=active ? p|PTE_P|PTE_U|PTE_W|PTE_PCD|PTE_PWT : 0;
    video_active=active;
    firmware.io=active ? video_io : 0;
    firmware.max_call_ms=active ? 2000 : 0;
}
static int fullscreen_call(struct biosvm_regs *r, uint32_t *budget, bool vbe, bool recovery)
{
    if (!(g_boot.flags&CBI_F_SMBIOS_QEMU)) log_regs("fullscreen","in",r);
    int e=display_fullscreen_checkpoint("bios-call",0);
    if (e && !recovery) return e;
    if (!*budget) return -V86_ETIMEDOUT;
    /* Durable logging is outside the firmware's cumulative 2 s budget. */
    uint64_t begin=deadline_after_ms(0);
    e=submit(r,*budget,0);
    uint64_t elapsed=deadline_after_ms(0)-begin;
    *budget=elapsed>=*budget ? 0 : *budget-(uint32_t)elapsed;
    if (!e && vbe && (uint16_t)r->eax!=0x004F) e=-V86_EIO;
    if (!(g_boot.flags&CBI_F_SMBIOS_QEMU)) log_regs("fullscreen","out",r);
    int logged=display_fullscreen_checkpoint("bios-return",e);
    if (!e && !recovery) e=logged;
    return e;
}
int biosvm_video_fullscreen_begin(uint16_t mode, uint8_t info[256])
{
    bool standard=mode<=3 || mode==0x0D || mode==0x0E || mode==0x10 || mode==0x11 || mode==0x12 || mode==0x13;
    bool packed=dos_video_mode(mode)!=0;
    if ((!standard && !packed) || (packed && !info) || !g_current || g_current==vm_thread || !(read_eflags()&V86_IF)) return -EINVAL;
    int e=biosvm_video_prepare();
    if (e) return e;
    kmutex_lock(&request_mutex);
    if (fullscreen_video || video_claimed || family_disabled(BIOS_VIDEO)) { e=-EBUSY; goto done_fs; }
    if ((e=display_fullscreen_checkpoint("video-claim",0))) goto done_fs;
    if ((e=claim_video())) goto done_fs;
    fullscreen_aperture(true);
    uint32_t budget=2000;
    struct biosvm_regs r={.interrupt=0x10,.eax=0x4F04,.ecx=15};
    fullscreen_bytes=0;
    if ((e=fullscreen_call(&r,&budget,true,false))) goto revoke_fs;
    unsigned bytes=(uint16_t)r.ebx*64u;
    if (!bytes || bytes>sizeof(fullscreen_saved)) { e=-EOPNOTSUPP; goto revoke_fs; }
    memset(P2V(BIOSVM_SCRATCH+0x800),0,bytes);
    r=(struct biosvm_regs){.interrupt=0x10,.eax=0x4F04,.ecx=15,.edx=1,
        .es=BIOSVM_SCRATCH>>4,.ebx=0x800};
    if ((e=fullscreen_call(&r,&budget,true,false))) goto revoke_fs;
    memcpy(fullscreen_saved,P2V(BIOSVM_SCRATCH+0x800),bytes);
    fullscreen_bytes=bytes;
    /* The fixed synthetic inventory is selected by the display service;
     * guest registers/firmware pointers are never forwarded. */
    r=(struct biosvm_regs){.interrupt=0x10,.eax=packed ? 0x4F02 : mode,.ebx=packed ? mode|0x4000 : 0};
    e=fullscreen_call(&r,&budget,packed,false);
    if (!e) {
        r=(struct biosvm_regs){.interrupt=0x10,.eax=packed ? 0x4F03 : 0x0F00};
        e=fullscreen_call(&r,&budget,packed,false);
        if (!e && (packed ? (r.ebx&0x7FFF)!=(mode|0x4000) : (r.eax&0x7F)!=mode)) e=-V86_EIO;
    }
    if (!e && packed) {
        memset(P2V(BIOSVM_SCRATCH+0x800),0,256);
        r=(struct biosvm_regs){.interrupt=0x10,.eax=0x4F01,.ecx=mode,
            .es=BIOSVM_SCRATCH>>4,.edi=0x800};
        if (!(e=fullscreen_call(&r,&budget,true,false))) memcpy(info,P2V(BIOSVM_SCRATCH+0x800),256);
    }
    /* Even a failed mode set may have touched hardware: retain the saved
     * state and leases so the caller can execute the rollback transaction. */
    fullscreen_video=true;
revoke_fs:
    fullscreen_aperture(false);
    if (!fullscreen_video && !family_disabled(BIOS_VIDEO) && release_video()) e=-V86_EIO;
done_fs:
    kmutex_unlock(&request_mutex);
    return e;
}
int biosvm_video_fullscreen_end(uint16_t mode, uint8_t info[256])
{
    if (!info || !g_current || g_current==vm_thread || !(read_eflags()&V86_IF)) return -EINVAL;
    kmutex_lock(&request_mutex);
    int e=-V86_EIO;
    if (!fullscreen_video || !fullscreen_bytes || family_disabled(BIOS_VIDEO)) goto done_restore;
    fullscreen_aperture(true);
    uint32_t budget=2000;
    memcpy(P2V(BIOSVM_SCRATCH+0x800),fullscreen_saved,fullscreen_bytes);
    struct biosvm_regs r={.interrupt=0x10,.eax=0x4F04,.ecx=15,.edx=2,
        .es=BIOSVM_SCRATCH>>4,.ebx=0x800};
    if ((e=fullscreen_call(&r,&budget,true,true))) goto revoke_restore;
    r=(struct biosvm_regs){.interrupt=0x10,.eax=0x4F03};
    if ((e=fullscreen_call(&r,&budget,true,true))) goto revoke_restore;
    if ((r.ebx&0x7FFF)!=(mode|0x4000)) { e=-V86_EIO; goto revoke_restore; }
    memset(P2V(BIOSVM_SCRATCH+0x800),0,256);
    r=(struct biosvm_regs){.interrupt=0x10,.eax=0x4F01,.ecx=mode,
        .es=BIOSVM_SCRATCH>>4,.edi=0x800};
    if (!(e=fullscreen_call(&r,&budget,true,true))) memcpy(info,P2V(BIOSVM_SCRATCH+0x800),256);
revoke_restore:
    fullscreen_aperture(false);
    fullscreen_video=false; fullscreen_bytes=0;
    if (!family_disabled(BIOS_VIDEO) && release_video()) e=-V86_EIO;
done_restore:
    kmutex_unlock(&request_mutex);
    return e;
}
