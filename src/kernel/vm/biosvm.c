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
#include <ciuki/biosvm.h>

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
static bool initialized, quarantined, synthetic;
static bool request_pending, request_done;
static unsigned request_test;
static struct biosvm_regs request_regs;
static uint32_t request_ms;
static int request_result;
static void (*input_service)(void);
static void (*input_observer)(uint8_t status, uint8_t byte, bool keyboard_irq);
static uint8_t controller_status;
static bool controller_status_valid;
static struct biosvm_mapping mappings[BIOSVM_MAP_COUNT];
static struct { int handle; gen_t generation; } leases[4];
static unsigned lease_count;
static uint16_t mouse_lost;

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
    for (unsigned i = 0; i < lease_count; i++) {
        int rc = registry_quarantine(leases[i].handle, leases[i].generation);
        klog("[biosvm] step=quarantine handle=%d generation=%u result=%d",
             leases[i].handle, leases[i].generation, rc);
    }
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
    if (!initialized || quarantined || synthetic)
        return;
    pending_irqs |= 1u << (tf->vector - 0x20);
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
        rc = v86_irq_deliver(&firmware, f);
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
    if (ebda < 0x80000 || ebda >= 0xA0000 || conventional_kib > 640 ||
        (uint32_t)conventional_kib * 1024 > ebda) {
        klog("[biosvm] step=ebda_base error=%d segment=%04x conventional_kib=%u", -EFAULT, ebda_seg, conventional_kib);
        return -EFAULT;
    }
    uint32_t end = ebda + *(const uint8_t *)P2V(ebda) * 1024u;
    if (end <= ebda || end > 0xA0000) {
        klog("[biosvm] step=ebda_size error=%d start=%08x end=%08x", -EFAULT, ebda, end);
        return -EFAULT;
    }
    int rc = as_create(&vm_as);
    if (rc)
        return init_error("address_space", rc);
    low_pt = pmm_alloc();
    if (!low_pt) {
        pmm_free(vm_as.pd_phys);
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
    mappings[3] = (struct biosvm_mapping){PAGE_ALIGN_DOWN(ebda), PAGE_ALIGN_UP(end), rw};
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

static int run_vm(struct biosvm_regs *regs, uint32_t ms, unsigned test)
{
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
        static const uint8_t denied[] = {0xB0, 0, 0xE6, 0x80, 0xCF};
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
        rc = v86_irq_deliver(&firmware, &frame);
        if (!rc) {
            firmware.state = V86_DONE;
            return 0;
        }
    }
    if (rc >= 0) {
        uint32_t old_cr3 = read_cr3();
        write_cr3(vm_as.pd_phys);
        v86_enter(&frame, &continuation);
        cli();
        continuation = 0;
        write_cr3(old_cr3);
        tss_set_kernel_stack((uint32_t)(uintptr_t)vm_thread->kstack + KSTACK_SIZE);
        sti();
        rc = firmware.result;
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
    }
    if (firmware.state == V86_DISABLED && !synthetic)
        quarantine();
    return rc;
}

static bool worker_ready(void *arg)
{
    (void)arg;
    return request_pending || (!quarantined && pending_irqs);
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
        if (!quarantined && !synthetic && pending_irqs) {
            struct biosvm_regs r = {0};
            run_vm(&r, 100, 0);
            input_irq = true;
        }
        input_irq |= firmware.stats.reflected_irqs != reflected;
        if (!quarantined && !synthetic && input_service && (input_irq || deadline_passed(poll))) {
            input_service();
            poll = deadline_after_ms(10);
        }
        if (deadline_passed(poll))
            poll = deadline_after_ms(10);
        kwait_wait_until(&wake, worker_ready, 0, quarantined ? deadline_after_ms(1000) : poll);
    }
}

int biosvm_init(void)
{
    if (quarantined)
        return init_error("already_quarantined", -V86_EIO);
    if (initialized)
        return 0;
    if (!g_current || !(read_eflags() & V86_IF))
        return init_error("thread_context", -V86_EPERM);
    if (g_boot.input_policy != CBI_INPUT_FIRMWARE)
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
    rc = claim_input();
    if (rc)
        goto failed;
    kmutex_init(&request_mutex);
    kwait_init(&wake);
    kwait_init(&completion);
    uint32_t f = irq_save();
    initialized = true;
    irq_set_handler(1, irq_input);
    irq_set_handler(12, irq_input);
    task_start(vm_thread);
    pic_unmask(1);
    pic_unmask(12);
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
        pmm_free(low_pt);
    if (vm_as.pd_phys)
        pmm_free(vm_as.pd_phys);
    low_pt = vm_as.pd_phys = 0;
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
    if (!initialized || quarantined || firmware.state == V86_DISABLED)
        return reject_call("call_disabled", regs, -V86_EIO);
    if (!regs || !g_current || !(read_eflags() & V86_IF) || !deadline_ms)
        return reject_call("call_context", regs, -EINVAL);
    uint8_t ah = regs->eax >> 8;
    bool keyboard = regs->interrupt == 0x16 && (ah == 0x10 || ah == 0x11);
    bool mouse = regs->interrupt == 0x15 && ah == 0xC2 && (regs->eax & 0xFF) <= 7;
    if ((!keyboard && !mouse) || deadline_ms > (mouse ? 500u : 100u))
        return reject_call("call_policy", regs, -V86_EPERM);
    if (g_current != vm_thread)
        kmutex_lock(&request_mutex);
    int rc;
    if (quarantined || firmware.state == V86_DISABLED) {
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
    return quarantined || firmware.state == V86_DISABLED ? BIOSVM_DISABLED_BACKEND :
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
