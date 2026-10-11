/* Private DOS lifecycle, software virtual IF and checked guest memory.
 * Research/decisions: docs/design/f3-acceptance.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/input.h>
#include <ciuki/abi.h>
#include "dosvm_internal.h"

extern void v86_enter(const struct v86_frame *, uint32_t *);
extern void v86_leave(uint32_t) __attribute__((noreturn));
/* f4-05 DOS VM audio output (audio/vm_source.c): one mixer source per VM
 * while a backend is ready. Weak: host harnesses link the VM without F4. */
extern int audio_vm_attach(struct dosvm_key) __attribute__((weak));
extern void audio_vm_detach(struct dosvm_key) __attribute__((weak));
extern void audio_vm_kick(void) __attribute__((weak));
static struct dosvm slots[DOSVM_LIMIT];
static struct dosvm_key input_focus;

bool dosvm_admit(uint64_t free_bytes, uint64_t reserved, uint64_t resident,
                 uint32_t request, struct dosvm_admission *out)
{
    uint64_t outstanding = reserved >= resident ? reserved - resident : UINT64_MAX;
    bool ok = request >= DOSVM_LOW_CAP && request <= DOSVM_RESIDENT_CAP &&
              !(request & (PAGE_SIZE - 1)) && outstanding <= free_bytes &&
              (uint64_t)request + DOSVM_HEADROOM <= free_bytes - outstanding;
    if (out) *out = (struct dosvm_admission){free_bytes, reserved, resident,
        outstanding, request, DOSVM_HEADROOM, ok};
    return ok;
}

struct dosvm *dosvm_lookup(struct dosvm_key key)
{
    if (!key.id || key.id > DOSVM_LIMIT || !key.generation) return 0;
    struct dosvm *v = &slots[key.id - 1];
    return gen_matches(v->info.key.generation, key.generation) ? v : 0;
}
struct dosvm_key dosvm_key(const struct dosvm *v) { return v->info.key; }
bool dosvm_valid(struct dosvm_key key)
{
    struct dosvm *v = dosvm_lookup(key);
    return v && v->info.state < DOSVM_STOPPING;
}
void dosvm_info(const struct dosvm *v, struct dosvm_info *out) { *out = v->info; }
void dosvm_snapshot(struct dosvm_ledger *out)
{
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct dosvm *v = &slots[i];
        if (!v->info.key.generation || v->info.state == DOSVM_DEAD) continue;
        out->vms++;
        out->tasks += v->task != 0;
        /* Count owned backing once, including unmapped EMS-window backing. */
        if (v->as.pd_phys) out->pages += DOSVM_PAGES + v->memory.quota / PAGE_SIZE + v->as.tables + 1;
        out->pages += v->audio.q.page_count; /* f3-05 PCM/event queues */
        out->handles += v->base_open + v->overlay_open;
        out->devices += 4; /* authoritative PIC, PIT, text/input service and VGA adapter */
        out->reserved += v->info.commitment;
        out->resident += v->info.resident;
    }
}

bool dosvm_range(const struct dosvm *v, uint32_t linear, unsigned bytes, bool write)
{
    uint32_t end=VM_POOL_BASE+v->memory.quota;
    bool low=linear<DOSVM_LINEAR_END && bytes<=DOSVM_LINEAR_END-linear;
    bool pool=linear>=VM_POOL_BASE && linear<=end && bytes<=end-linear;
    if ((!low && !pool) || !v->as.pd_phys)
        return false;
    while (bytes) {
        unsigned n = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
        if (n > bytes) n = bytes;
        if (!(linear >= 0xA0000 && linear < 0xC0000) &&
            !as_range_ok(&v->as, linear, n, write)) return false;
        linear += n; bytes -= n;
    }
    return true;
}
static int transfer(struct dosvm *v, uint32_t linear, void *buffer, unsigned bytes, bool write)
{
    if (!dosvm_range(v, linear, bytes, write)) return -EFAULT;
    uint8_t *b = buffer;
    while (bytes) {
        unsigned n = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
        if (n > bytes) n = bytes;
        if (linear >= 0xA0000 && linear < 0xC0000) {
            /* Kernel reads peek (no latch load); guest cycles use the VGA bus. */
            for (unsigned i = 0; i < n; i++) {
                if (write) cvga_write_vram(&v->vga, linear + i, b[i]);
                else b[i] = cvga_peek_vram(&v->vga, linear + i);
            }
            b += n; linear += n; bytes -= n; continue;
        }
        uint32_t phys = as_lookup(&v->as, linear, 0);
        if (!phys) return -EFAULT;
        if (write) memcpy(P2V(phys), b, n);
        else memcpy(b, P2V(phys), n);
        b += n; linear += n; bytes -= n;
    }
    return 0;
}
int dosvm_read(struct dosvm *v, uint32_t linear, void *b, unsigned n) { return transfer(v, linear, b, n, false); }
int dosvm_write(struct dosvm *v, uint32_t linear, const void *b, unsigned n) { return transfer(v, linear, (void *)b, n, true); }
static int monitor_copy(void *arg, uint32_t linear, void *b, unsigned n, bool write)
{ return transfer(arg, linear, b, n, write); }
static void *monitor_memory(void *arg, uint32_t linear, unsigned n, bool write)
{
    struct dosvm *v = arg;
    /* The monitor only dereferences one-byte fetches itself. Larger ranges
     * use monitor_copy; this pointer is solely its validation token. */
    return dosvm_range(v, linear, n, write) ? P2V(as_lookup(&v->as, linear, 0)) : 0;
}
static uint8_t absent_in(void *arg, uint16_t port) { (void)arg; (void)port; return 0xFF; }
static void absent_out(void *arg, uint16_t port, uint8_t value) { (void)arg; (void)port; (void)value; }

int dosvm_set_a20(struct dosvm *v, bool enabled)
{
    if (!v->as.pd_phys) return -EFAULT;
    for (unsigned i = 0; i < 16; i++) {
        int e = as_map(&v->as, 0x100000 + i * PAGE_SIZE,
                       v->frames[enabled ? 256 + i : i], PTE_U | PTE_W);
        if (e) return e; /* table already exists: no allocation here */
    }
    v->a20 = enabled;
    return 0;
}

static struct dosvm *owner(const struct task *t)
{
    for (unsigned i = 0; i < DOSVM_LIMIT; i++)
        if (slots[i].task == t && t) return &slots[i];
    return 0;
}
/* f3-04 scheduler hook: the incoming worker's LDT and watchpoints, or none. */
void dosvm_task_switch(const struct task *t)
{
    struct dosvm *v = owner(t);
    if (v && v->dpmi.active) {
        uint32_t address[4] = {0};
        for (unsigned i = 0; i < DPMI_WATCH; i++) address[i] = v->dpmi.watch[i].addr;
        arch_ldt_load(v->dpmi.ldt, DPMI_LDT_ENTRIES);
        arch_debug_load(address, v->dpmi.dr7);
    } else {
        arch_ldt_load(0, 0);
        arch_debug_load((const uint32_t[4]){0}, 0);
    }
}
bool dosvm_client(const struct task *t)
{
    struct dosvm *v = owner(t);
    return v && v->continuation && v->dpmi.active;
}
bool dosvm_dpmi_info(struct dosvm_key key, struct dpmi_info *out)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!out || !v || !v->info.key.generation || v->info.state == DOSVM_DEAD) return false;
    dpmi_info_fill(v, out);
    return true;
}
uint32_t dosvm_task_cr3(const struct task *t, uint32_t normal)
{
    struct dosvm *v = owner(t);
    return v && v->continuation ? v->as.pd_phys : normal;
}
uint32_t dosvm_task_esp0(const struct task *t, uint32_t normal)
{
    struct dosvm *v = owner(t);
    return v && v->continuation ? v->continuation : normal;
}
int dosvm_stop(struct dosvm_key key, enum dosvm_reason reason)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v) return -EINVAL;
    if (v->info.state >= DOSVM_STOPPING) return 0;
    v->info.reason = reason;
    v->info.state = DOSVM_STOPPING;
    v->mouse.mask = v->mouse.pending_mask = 0;
    v->mouse.active = false;
    if (input_focus.id == key.id && input_focus.generation == key.generation) input_focus = (struct dosvm_key){0};
    /* No new callback/port/input work can now validate this key. */
    kwait_wake_all(&v->wake);
    if (v->task && !v->started) task_kill(v->task, reason);
    return 0;
}

static bool wake_condition(void *arg)
{
    struct dosvm *v = arg;
    return v->info.state == DOSVM_STOPPING ||
           (v->monitor.vif && !v->monitor.shadow && !v->dpmi.shadow &&
            (v86_irq_pending(&v->monitor) >= 0 || (!v->frame_pm && v->mouse.pending_mask)));
}
void dosvm_input_poll(struct dosvm *v)
{
    if (v->owner_pid || v->window_input || !dosvm_valid(v->info.key) || input_focus.id != v->info.key.id ||
        !gen_matches(input_focus.generation, v->info.key.generation)) return;
    struct input_event event;
    unsigned budget = 32;
    while (budget-- && v->key_count < ARRAY_SIZE(v->keyboard) && input_read(&event)) {
        if (event.tick - v->input_since >= (1ull << 63)) continue;
        if (event.type == INPUT_TEXT && event.code && event.code < 128)
            dosvm_keyboard(v->info.key, event.code);
    }
}
int dosvm_wait(struct dosvm *v, bool keyboard, uint64_t deadline)
{
    v->info.state = DOSVM_BLOCKED;
    while (v->info.state != DOSVM_STOPPING) {
        if (keyboard) { dosvm_input_poll(v); if (dosvm_keyboard_refresh(v)) return -EFAULT; }
        if (keyboard && v->key_count) break;
        if (!keyboard && deadline_passed(deadline)) break;
        if (!v->info.dos_ready && deadline_passed(v->boot_deadline)) {
            dosvm_stop(v->info.key, DOSVM_TIMEOUT);
            break;
        }
        /* A bounded scheduler sleep also permits the existing input consumer
         * to enqueue text. No physical IRQ/IF ownership is transferred. */
        task_sleep_ms(1);
    }
    if (v->info.state == DOSVM_STOPPING) return -V86_EIO;
    v->info.state = v->info.dos_ready ? DOSVM_RUNNABLE : DOSVM_BOOTING;
    return 0;
}

/* Every trap of the worker's guest, V86 or (f3-04) a ring-3 DPMI client.
 * The hardware frame is copied into v->frame, the single working copy all
 * emulation, dispatch and mode switches use; it goes back to the stack when
 * the mode is unchanged, otherwise the worker re-enters with the new frame
 * (a PM trap_frame and a V86 frame differ in size on the same TSS stack). */
bool dosvm_trap(struct trap_frame *tf)
{
    struct dosvm *v = owner(g_current);
    if (!v || !v->continuation) return false;
    bool pm = !(tf->eflags & V86_VM);
    /* CR2/DR6 belong to this trap only while interrupts stay off. */
    uint32_t cr2 = tf->vector == 14 ? read_cr2() : 0;
    uint32_t dr6 = tf->vector == 1 && v->dpmi.dr7 ? arch_debug_take() : 0;
    unsigned size = pm ? sizeof(struct trap_frame) : sizeof(struct v86_frame);
    struct v86_frame *f = &v->frame;
    memcpy(f, tf, size);
    v->frame_pm = pm;
    sti();
    int e = 0;
    if (v->info.state != DOSVM_STOPPING) {
        if (pm) e = dpmi_pm_trap(v, f, cr2, dr6);
        else if (tf->vector == 13) e = v86_emulate(&v->monitor, f);
        else if (tf->vector == 1 && (v->monitor.shadow || v->monitor.track_boundaries)) {
            if (dr6 & 15) {
                dpmi_watch_hits(v, dr6);
                for (unsigned i = 0; i < DPMI_WATCH; i++)
                    if ((dr6 & (1u << i)) && v->dpmi.watch[i].type == 0) f->tf.eflags |= 1u << 16;
            }
            e = v86_debug_step(&v->monitor, f);
        }
        else if (tf->vector == 14 && cr2 >= 0xA0000 && cr2 < 0xC0000)
            e = dosvm_vga_fault(v, f);
        else if (tf->vector == 7) fpu_handle_nm();
        else if (tf->vector == 3 || tf->vector == 4) e = v86_reflect(&v->monitor, f, tf->vector);
        else if (tf->vector < 0x20 || tf->vector >= 0x30) e = -EFAULT;
    }
    if (e < 0) {
        v->info.fault_vector = tf->vector;
        v->info.fault_eip = pm ? dpmi_fault_linear(v, (const struct v86_frame *)tf) : (tf->cs << 4) + tf->eip;
        v->info.fault_address = cr2;
        v->info.fault_error = tf->err;
        dosvm_stop(v->info.key, DOSVM_FAULT);
    }
    if (!v->info.dos_ready && deadline_passed(v->boot_deadline))
        dosvm_stop(v->info.key, DOSVM_TIMEOUT);
    while (v->info.state != DOSVM_STOPPING) {
        if (!v->info.dos_ready && deadline_passed(v->boot_deadline)) {
            dosvm_stop(v->info.key, DOSVM_TIMEOUT);
            break;
        }
        if (v->dpmi.active) {
            e = 0;
            if (!v->frame_pm) {
                uint32_t flags = irq_save();
                e = dosvm_mouse_dispatch(v, f);
                irq_restore(flags);
            }
            if (!e) e = dpmi_deliver(v, f);
        } else {
            uint32_t flags = irq_save();
            e = dosvm_mouse_dispatch(v, f);
            if (!e) e = v86_irq_deliver(&v->monitor, f);
            irq_restore(flags);
        }
        if (e < 0) {
            v->info.fault_vector = tf->vector;
            v->info.fault_eip = dpmi_fault_linear(v, f);
            dosvm_stop(v->info.key, DOSVM_FAULT);
            break;
        }
        if (v->monitor.state != V86_HALTED) break;
        v->info.state = DOSVM_BLOCKED;
        kwait_wait_until(&v->wake, wake_condition, v, deadline_after_ms(10));
    }
    /* A protected-mode frame must let the kernel IRET succeed. */
    if (v->info.state != DOSVM_STOPPING && v->frame_pm && !dpmi_frame_check(v, f)) {
        v->info.fault_vector = 13;
        v->info.fault_eip = dpmi_fault_linear(v, f);
        v->dpmi.stats.faults++;
        dosvm_stop(v->info.key, DOSVM_FAULT);
    }
    if (v->info.state == DOSVM_STOPPING) {
        cli();
        v86_leave(v->continuation);
    }
    if (v->info.state == DOSVM_BLOCKED)
        v->info.state = v->info.dos_ready ? DOSVM_RUNNABLE : DOSVM_BOOTING;
    if (g_need_resched) schedule();
    if (!v->frame_pm) v86_prepare_return(&v->monitor, f);
    cli();
    if (v->frame_pm != pm) v86_leave(v->continuation);
    memcpy(tf, f, size);
    return true;
}

static int interrupt_service(void *arg, struct v86_frame *f, uint8_t vector)
{
    struct dosvm *v = arg;
    if (vector == 0xFD) return dosvm_mouse_return(v, f);
    if (vector != 0xFE) return dpmi_v86_int(v, f, vector);
    int stub = dpmi_v86_stub(v, f);
    if (stub != -ENOSYS) return stub;
    if (f->tf.cs == VM_XMS_SEG && f->tf.eip == 4) return dosvm_xms(v, f);
    if (f->tf.cs == VM_XMS_SEG && f->tf.eip == 18) return dosvm_ems_return(v, f);
    /* Only synthetic ROM stubs may enter the private firmware service.
     * Original INT already pushed its own FLAGS/CS/IP; no second INT frame. */
    uint8_t service;
    if (f->tf.cs == 0xF100 && f->tf.eip == 2) service = 0x67; /* EMS stub page */
    else if (f->tf.cs != 0xF000 || (f->tf.eip & 15) != 2 || f->tf.eip >= 0x1000) return -V86_EPERM;
    else service = (uint8_t)(f->tf.eip >> 4);
    uint32_t off = (uint16_t)f->tf.user_esp;
    if (off > 0xFFFA || f->tf.user_ss > 0xFFFF) return -EFAULT;
    uint32_t linear = (f->tf.user_ss << 4) + off + 4;
    uint16_t saved;
    if (dosvm_read(v, linear, &saved, 2)) return -EFAULT;
    f->tf.eflags = (f->tf.eflags & ~(V86_CF | V86_ZF)) | (saved & (V86_CF | V86_ZF));
    int e = dosvm_bios(v, f, service);
    if (e) return e;
    saved = (saved & ~(V86_CF | V86_ZF)) | (f->tf.eflags & (V86_CF | V86_ZF));
    return dosvm_write(v, linear, &saved, 2);
}

static void worker(void *arg)
{
    struct dosvm *v = arg;
    write_cr3(v->as.pd_phys);
    /* f3-04: a mode switch leaves the trap stack and re-enters here with
     * the new frame; only a stopping VM ends the loop. */
    for (;;) {
        v86_enter(&v->frame, &v->continuation);
        if (v->info.state == DOSVM_STOPPING) break;
    }
    v->continuation = 0;
    if (v->dpmi.active) {
        arch_ldt_load(0, 0);
        arch_debug_load((const uint32_t[4]){0}, 0);
    }
    write_cr3(vmm_kernel_pd());
    tss_set_kernel_stack((uint32_t)v->task->kstack + KSTACK_SIZE);
    uint8_t code[8] = {0};
    if (v->info.reason == DOSVM_FAULT) dosvm_read(v, v->info.fault_eip, code, sizeof(code));
    rec_emit(dosvm_probe(v), "DATA", "group=vm_stop vm=%u generation=%u reason=%u vector=%u eip=%08x address=%08x error=%x code=%02x%02x%02x%02x%02x%02x%02x%02x",
             v->info.key.id, v->info.key.generation, v->info.reason,
             v->info.fault_vector, v->info.fault_eip, v->info.fault_address, v->info.fault_error,
             code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
    if (v->info.reason != DOSVM_EXIT) {
        /* Guest registers and stack top of the last frame: the INT frame
         * and return addresses name the DOS path without a build-time trace. */
        const struct v86_frame *s = &v->frame;
        /* A DPMI frame shows selectors and the protected-mode CS:EIP. */
        rec_emit(dosvm_probe(v), "DATA", "group=vm_fault pm=%u ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x ds=%04x es=%04x ss=%04x sp=%04x cs=%04x ip=%08x",
                 v->frame_pm, s->tf.eax & 0xFFFF, s->tf.ebx & 0xFFFF, s->tf.ecx & 0xFFFF, s->tf.edx & 0xFFFF,
                 s->tf.esi & 0xFFFF, s->tf.edi & 0xFFFF, s->tf.ebp & 0xFFFF,
                 (v->frame_pm ? s->tf.ds : s->ds) & 0xFFFF, (v->frame_pm ? s->tf.es : s->es) & 0xFFFF,
                 s->tf.user_ss & 0xFFFF, s->tf.user_esp & 0xFFFF, s->tf.cs & 0xFFFF, s->tf.eip);
        uint32_t top = ((s->tf.user_ss & 0xFFFF) << 4) + (s->tf.user_esp & 0xFFFF);
        struct dpmi_seg stack;
        if (v->frame_pm && dpmi_seg(v, (uint16_t)s->tf.user_ss, &stack))
            top = stack.base + (stack.big ? s->tf.user_esp : (s->tf.user_esp & 0xFFFF));
        for (unsigned part = 0; part < 3; part++) {
            uint16_t w[16] = {0};
            dosvm_read(v, top + part * sizeof(w), w, sizeof(w));
            rec_emit(dosvm_probe(v), "DATA", "group=vm_stack part=%u words=%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x",
                     part, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
        }
    }
    task_exit(v->info.reason);
}

int dosvm_destroy(struct dosvm_key key, bool forced)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v) return -EINVAL;
    if (v->info.state == DOSVM_DEAD) return v->info.cleanup_error;
    if (v->task == g_current) return -V86_EPERM;
    dosvm_stop(key, forced ? DOSVM_TERMINATE : DOSVM_EXIT);
    uint64_t until = deadline_after_ms(6000);
    while (v->task && task_alive(v->task)) {
        if (deadline_passed(until)) return -16; /* retain resources until quiescent */
        task_sleep_ms(1);
    }
    if (v->task) { task_reap(v->task); v->task = 0; }
    v->info.data_loss = v->info.dirty && (forced || v->info.reason != DOSVM_EXIT);
    int e = dosvm_disk_close(v, forced || v->info.reason != DOSVM_EXIT);
    if (e && v->info.dirty) v->info.data_loss = true;
    if (audio_vm_detach) audio_vm_detach(key); /* f4-05: revoke the mixer source first */
    dosvm_audio_close(v);
    if (v->as.pd_phys) {
        /* f3-06: window PTEs may name the model's plane pages (kernel memory);
         * restore the VM's own frames before the address space frees them. */
        dosvm_vga_sync(v, true);
        dpmi_close(v);
        dosvm_memory_close(v);
        as_destroy(&v->as);
    }
    memset(v->frames, 0, sizeof(v->frames));
    memset(v->ports, 0, sizeof(v->ports));
    v->key_count = v->line_length = 0;
    v->info.commitment = v->info.resident = 0;
    v->info.cleanup_error = e;
    /* A failed unlink retains the generation/table for retry, never reuses
     * a slot while its private scratch file still exists. */
    v->info.state = v->overlay_created ? DOSVM_STOPPING : DOSVM_DEAD;
    rec_emit(dosvm_probe(v), "DATA", "group=vm_cleanup vm=%u generation=%u data_loss=%u error=%d",
             key.id, key.generation, v->info.data_loss, e);
    return e;
}

int dosvm_create(const struct dosvm_config *config, struct dosvm **out)
{
    if (!out) return -EINVAL;
    *out = 0;
    uint32_t commitment = config && config->commitment ? config->commitment : DOSVM_RESIDENT_CAP;
    struct dosvm_ledger ledger;
    struct dosvm_admission admission;
    dosvm_snapshot(&ledger);
    bool admit = dosvm_admit((uint64_t)pmm_free_count() * PAGE_SIZE,
                             ledger.reserved, ledger.resident, commitment, &admission);
    rec_emit(config && config->probe ? config->probe : "vm-boot", "DATA", "group=admission free=%llu reserved=%llu resident=%llu outstanding=%llu request=%u headroom=%u admitted=%u",
             admission.free_bytes, admission.reserved_bytes, admission.resident_bytes,
             admission.outstanding_bytes, commitment, DOSVM_HEADROOM, admit);
    if (!admit) return -ENOMEM;
    struct dosvm *v = 0;
    unsigned slot;
    for (slot = 0; slot < DOSVM_LIMIT; slot++)
        if (!slots[slot].info.key.generation || slots[slot].info.state == DOSVM_DEAD) { v = &slots[slot]; break; }
    if (!v) return -ENOSPC;
    uint32_t generation = gen_alloc();
    if (!generation) return -ENOSPC;
    memset(v, 0, sizeof(*v));
    v->info.key = (struct dosvm_key){slot + 1, generation};
    v->info.state = DOSVM_CREATING;
    v->info.commitment = commitment;
    v->info.resident = sizeof(*v);
    v->base = v->overlay = -1;
    v->a20 = true;
    v->probe = config ? config->probe : 0;
    v->dpmi_enabled = config && config->dpmi;
    kwait_init(&v->wake);
    kwait_init(&v->disk_wait); v->disk_busy = false;
    int e = as_create(&v->as);
    if (e) goto fail;
    for (unsigned i = 0; i < DOSVM_PAGES; i++) {
        uint32_t phys = pmm_alloc_zone(ZONE_NORMAL);
        if (!phys) { e = -ENOMEM; goto fail; }
        memset(P2V(phys), 0, PAGE_SIZE);
        e = as_map(&v->as, i * PAGE_SIZE, phys, dosvm_low_flags(i));
        if (e) { pmm_free(phys); goto fail; }
        v->frames[i] = phys;
        v->info.resident += PAGE_SIZE;
    }
    v->info.resident += (v->as.tables + 1) * PAGE_SIZE;
    const struct v86_ops ops = {monitor_memory, absent_in, absent_out, v};
    v86_init(&v->monitor, &ops);
    v->monitor.copy = monitor_copy;
    v->monitor.io = dosvm_io;
    v->monitor.interrupt = interrupt_service;
    v->monitor.track_boundaries = true;
    v->monitor.pic[0].imr = 0xF8; /* timer and cascade, no physical input IRQ */
    v->monitor.pic[1].imr = 0xFF;
    dosvm_pit_init(v); dosvm_kbd_init(v); dosvm_mouse_init(v);
    if ((e = dosvm_port_register(v->info.key, 0x40, 0x43, dosvm_pit_port, 0)) ||
        (e = dosvm_port_register(v->info.key, 0x61, 0x61, dosvm_pit_port, 0)) ||
        (e = dosvm_port_register(v->info.key, 0x60, 0x60, dosvm_kbd_port, 0)) ||
        (e = dosvm_port_register(v->info.key, 0x64, 0x64, dosvm_kbd_port, 0)) ||
        (e = dosvm_audio_init(v))) goto fail; /* f3-05 SB16, OPL3 and 8237 */
    e = dosvm_bios_init(v);
    if (e) goto fail;
    e = dosvm_memory_init(v, config);
    if (e) goto fail;
    e = dosvm_vga_init(v);
    if (e) goto fail;
    if ((e = dpmi_rom_init(v))) goto fail;
    e = dosvm_disk_open(v, config ? config->base : 0);
    if (e) goto fail;
    v->task = task_create_kernel("dosvm", worker, v, P_NORMAL);
    if (!v->task) { e = -ENOMEM; goto fail; }
    v->info.resident += KSTACK_SIZE + PAGE_SIZE + sizeof(struct task);
    if (v->info.resident - v->memory.quota > DOSVM_LOW_CAP ||
        v->info.resident > commitment) { e = -ENOMEM; goto fail; }
    /* f4-05: the VM's SB16/OPL output joins the mixer when a backend is
     * ready; without one f3-05 keeps discarding with its counter. */
    if (audio_vm_attach) audio_vm_attach(v->info.key);
    *out = v;
    return 0;
fail:
    dosvm_destroy(v->info.key, true);
    return e;
}

int dosvm_boot(struct dosvm *v)
{
    if (v->info.state != DOSVM_CREATING || !v->task) return -EINVAL;
    int e = dosvm_disk_read(v, 0, v->sector);
    if (e || v->sector[510] != 0x55 || v->sector[511] != 0xAA) return -V86_EIO;
    if ((e = dosvm_write(v, 0x7C00, v->sector, 512))) return e;
    memset(&v->frame, 0, sizeof(v->frame));
    v->frame.tf.eip = 0x7C00;
    v->frame.tf.edx = 0x80;
    v->frame.tf.user_esp = 0x7C00;
    v->frame.tf.eflags = V86_VM | V86_IF | 2;
    v->monitor.state = V86_RUNNING;
    v->monitor.vif = true;
    v->boot_deadline = deadline_after_ms(30000);
    v->ticks = g_ticks;
    v->input_since = g_ticks;
    v->info.state = DOSVM_BOOTING;
    v->started = true;
    if (!dosvm_valid(input_focus)) input_focus = v->info.key;
    v86_prepare_return(&v->monitor, &v->frame);
    task_start(v->task);
    return 0;
}

bool dosvm_irq_raise(struct dosvm_key key, unsigned irq)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key) || irq >= 16) return false;
    uint32_t flags = irq_save();
    v86_irq_raise(&v->monitor, irq);
    irq_restore(flags);
    kwait_wake_all(&v->wake);
    return true;
}
int dosvm_keyboard_refresh(struct dosvm *v)
{
    uint16_t pos[2];
    if (dosvm_read(v, 0x41A, pos, sizeof(pos))) return -EFAULT;
    if ((pos[0] & 1) || (pos[1] & 1) || pos[0] < 0x1E || pos[0] >= 0x3E || pos[1] < 0x1E || pos[1] >= 0x3E) return -EFAULT;
    v->key_head = (pos[0] - 0x1E) / 2;
    v->key_count = ((pos[1] - pos[0] + 32) % 32) / 2;
    return dosvm_read(v, 0x41E, v->keyboard, 32);
}
bool dosvm_keyboard(struct dosvm_key key, uint16_t bios_key)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key)) return false;
    uint32_t flags = irq_save();
    if (dosvm_keyboard_refresh(v) || v->key_count == 15) { irq_restore(flags); return false; }
    unsigned tail = (v->key_head + v->key_count) % 16;
    uint16_t next = 0x1E + ((tail + 1) % 16) * 2;
    bool ok = !dosvm_write(v, 0x41E + tail * 2, &bios_key, 2) && !dosvm_write(v, 0x41C, &next, 2);
    if (ok) { v->keyboard[tail] = bios_key; v->key_count++; kwait_wake_all(&v->wake); }
    irq_restore(flags); return ok;
}
void dosvm_tick(void)
{
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct dosvm *v = &slots[i];
        if (!v->started || !dosvm_valid(v->info.key)) continue;
        v->monitor.ticks = g_ticks;
        uint64_t elapsed = g_ticks - v->ticks;
        v->ticks = g_ticks;
        v->clock_ms += elapsed;
        dosvm_pit_advance(v, elapsed);
        dosvm_audio_advance(v, elapsed);
        if (audio_vm_kick && (v->audio.q.seg_count || v->audio.q.event_count)) audio_vm_kick();
    }
}
const uint16_t *dosvm_text(const struct dosvm *v, unsigned *cursor)
{
    dosvm_read((struct dosvm *)v, 0xB8000, (void *)v->text, sizeof(v->text));
    dosvm_mouse_text((struct dosvm *)v);
    uint16_t pos = 0;
    dosvm_read((struct dosvm *)v, 0x450, &pos, 2);
    if (cursor) *cursor = v->mouse.visible >= 0 && v->mouse.text_type == 1 && v->mouse.x < 640 && v->mouse.y < 200 ?
        (unsigned)(v->mouse.y / 8) * 80 + v->mouse.x / 8 : (unsigned)(pos >> 8) * 80 + (pos & 255);
    return v->text;
}
int dosvm_port_register(struct dosvm_key key, uint16_t first, uint16_t last,
                        dosvm_port_handler handler, void *arg)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key) || first > last || !handler) return -EINVAL;
    /* These fixed ranges belong to the core. */
    static const uint16_t reserved[][2] = {{0x20,0x21},
        {0x70,0x71},{0x92,0x92},{0xA0,0xA1},{DOSVM_RECORD_PORT,DOSVM_CONTROL_PORT},
        {DOSVM_FAULT_PORT,DOSVM_FAULT_PORT}};
    for (unsigned i = 0; i < ARRAY_SIZE(reserved); i++)
        if (first <= reserved[i][1] && last >= reserved[i][0]) return -16;
    unsigned slot = DOSVM_PORTS;
    for (unsigned i = 0; i < DOSVM_PORTS; i++) {
        if (!v->ports[i].handler) { slot = i; continue; }
        if (first <= v->ports[i].last && last >= v->ports[i].first) return -16;
    }
    if (slot == DOSVM_PORTS) return -ENOSPC;
    v->ports[slot].first = first; v->ports[slot].last = last;
    v->ports[slot].arg = arg; v->ports[slot].handler = handler;
    return 0;
}
int dosvm_port_unregister(struct dosvm_key key, uint16_t first)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key)) return -EINVAL;
    for (unsigned i = 0; i < DOSVM_PORTS; i++) if (v->ports[i].handler && v->ports[i].first == first) {
        memset(&v->ports[i], 0, sizeof(v->ports[i])); return 0;
    }
    return -EINVAL;
}

/* Trusted window transport hook. No native syscall or VM-id authority is
 * granted here; f3-02 supplies the owner-bound adapter. */
bool dosvm_input_focus(struct dosvm_key key, bool focused)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key)) return false;
    uint32_t flags = irq_save();
    struct dosvm *previous = dosvm_lookup(input_focus);
    if (focused && previous && previous != v && dosvm_valid(input_focus)) {
        dosvm_kbd_release(previous); dosvm_mouse_event(previous, 0, 0, 0);
        previous->pointer_known = false;
    }
    v->window_input = true;
    if (focused) { input_focus = key; v->pointer_known = false; }
    else if (input_focus.id == key.id && input_focus.generation == key.generation) {
        dosvm_kbd_release(v); dosvm_mouse_event(v, 0, 0, 0);
        v->pointer_known = false; input_focus = (struct dosvm_key){0};
    }
    irq_restore(flags);
    return true;
}
bool dosvm_input_event(struct dosvm_key key, const struct ciuki_input_event *e)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!e || !v || !dosvm_valid(key) || !v->window_input ||
        input_focus.id != key.id || input_focus.generation != key.generation) return false;
    uint32_t flags = irq_save(); bool ok = true;
    switch (e->type) {
    case CIUKI_INPUT_KEY: ok = dosvm_kbd_event(v, e->code, e->value); break;
    case CIUKI_INPUT_TEXT: break; /* KEY already supplies the guest keyboard */
    case CIUKI_INPUT_MOTION:
        if (v->pointer_known) {
            int64_t dx = (int64_t)e->value - v->input_x, dy = (int64_t)e->value2 - v->input_y;
            if (dx < INT32_MIN || dx > INT32_MAX || dy < INT32_MIN || dy > INT32_MAX) { ok = false; break; }
            dosvm_mouse_event(v, dx, dy, v->mouse.buttons);
        }
        v->input_x = e->value; v->input_y = e->value2; v->pointer_known = true; break;
    case CIUKI_INPUT_BUTTON:
        if (e->code < 1 || e->code > 3 || (e->value != 0 && e->value != 1)) { ok = false; break; }
        dosvm_mouse_event(v, 0, 0, e->value ? v->mouse.buttons | (1u << (e->code - 1)) :
                         v->mouse.buttons & ~(1u << (e->code - 1))); break;
    case CIUKI_INPUT_RESYNC:
        dosvm_kbd_release(v); dosvm_mouse_event(v, 0, 0, 0); v->pointer_known = false; break;
    default: ok = false;
    }
    if (ok) kwait_wake_all(&v->wake);
    irq_restore(flags);
    return ok;
}
bool dosvm_device_info(struct dosvm_key key, struct dosvm_device_info *out)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!out || !v || !dosvm_valid(key)) return false;
    uint32_t flags = irq_save(); memset(out, 0, sizeof(*out));
    out->host_ticks = v->clock_ms; out->pit_clocks = v->pit.clocks; out->pit_edges = v->pit.edges;
    out->pit_dropped_ticks = v->pit.dropped_ticks; out->key_overflows = v->kbd.overflows;
    out->queued_bytes = v->kbd.count; out->buttons = v->mouse.buttons;
    for (unsigned i = 0; i < sizeof(v->kbd.held); i++)
        for (unsigned bit = 0; bit < 8; bit++) out->held_keys += !!(v->kbd.held[i] & (1u << bit));
    for (unsigned i = 0; i < 2; i++) {
        out->irr[i] = v->monitor.pic[i].irr; out->isr[i] = v->monitor.pic[i].isr; out->imr[i] = v->monitor.pic[i].imr;
        out->pic_base[i] = v->monitor.pic[i].base;
    }
    out->last_vector = v->monitor.stats.last_vector;
    for (unsigned i = 0; i < 16; i++) { out->delivered[i] = v->monitor.stats.delivered[i];
    }
    irq_restore(flags); return true;
}
