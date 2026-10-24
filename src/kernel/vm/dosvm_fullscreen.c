/* Single physical VGA owner. Transfers run in thread context, guest access
 * is revoked before capture/restore. Windowed rendering remains in dosvm_vga.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/display.h>
#include <ciuki/input.h>
#include <ciuki/sync.h>
#include <ciuki/task.h>
#include <ciuki/vga_ports.h>
#include "dosvm_internal.h"
#include "vga_hardware.h"
static struct dosvm *fullscreen_owner;
static bool fullscreen_busy, fullscreen_return;
static bool fullscreen_background;
static unsigned desktop_pending,desktop_next;
static struct dosvm_key fullscreen_request;
static uint32_t fullscreen_ptes[32];
static void *fullscreen_mapping;
static uint64_t fullscreen_frame_deadline;
static bool fullscreen_frame_seen;
static bool fullscreen_linear,fullscreen_model_changed;
static uint64_t fullscreen_sample_tick;
static bool fullscreen_sample_frame;
static uint32_t fullscreen_palette;
static const struct task *fullscreen_service_task;
/* f3-61: what requested the pending return (enum dosvm_fullscreen_cause).
 * The record keeps reason=return-or-fault and names the cause. */
static uint8_t fullscreen_cause_next;
static const char *const fullscreen_causes[DOSVM_FS_CAUSES]={
    "none","hotkey","background","mode-change","present-error","stopping",
    "first-frame-timeout","guest-progress-timeout","owner-lost","request"};
static void return_cause(uint8_t cause) { if (!fullscreen_cause_next) fullscreen_cause_next=cause; }
/* fx-22 (T23 f441c5bd, e831e685; supersavage-fullscreen.md fx-22 amendment).
 * revoked: the owner VM's continuation has lost the adapter (park ended or
 * timed out, stop, VBE mode change); hold alone only asks it to park.
 * transfer_vm: a synthetic VGA BIOS transfer (model_begin..model_end) is in
 * flight and completes before the revoke, within FS_PARK_TRANSFER_MS.
 * fullscreen_dirty: guest aperture pages A0000h..BFFFFh (bit n = page n)
 * written since the last full synchronisation of model and adapter. */
static bool fullscreen_revoked;
static struct dosvm *transfer_vm;
static uint32_t fullscreen_dirty;
/* fx-22 policy: begin (sliced into tests/host/t23_fullscreen_return_test.c) */
#define FS_PARK_MS 250u
#define FS_PARK_TRANSFER_MS 750u
#define FS_CAPTURE_MS 750u
#define FS_PARK_RECHECK_MS 20u
/* Win95 DDK Adjust_Exec_Priority/Time_Critical_Boost: the switch runs above
 * every ordinary time slice (the desktop share and the focused-VM
 * preference start below SCHED_BOOST_CAP), below device service. */
struct fs_boost { struct task *t; enum task_prio prio; uint8_t configured; bool raised; };
static struct fs_boost boost_begin(void)
{
    struct fs_boost b={g_current,P_NORMAL,0,false};
    if (b.t && b.t->prio!=P_DEVICE && b.t->configured_priority<SCHED_BOOST_CAP) {
        b.prio=b.t->prio; b.configured=b.t->configured_priority; b.raised=true;
        task_set_base(b.t,b.prio,SCHED_BOOST_CAP);
    }
    return b;
}
static void boost_end(const struct fs_boost *b) { if (b->raised) task_set_base(b->t,b->prio,b->configured); }
/* fx-22 policy: end */
/* A VM may yield halfway through a synthetic VGA BIOS transfer. After a
 * forced return its C continuation may finish, but must never touch the
 * restored adapter. The display worker retains its independent access. */
static bool fs_access(void) {
    return g_current==fullscreen_service_task ||
        (fullscreen_owner && fullscreen_owner->task==g_current &&
         fullscreen_owner->fullscreen && !fullscreen_revoked);
}
static uint8_t fs_in(void *arg, uint16_t p) { (void)arg; return fs_access() ? inb(p) : 0xFF; }
static void fs_out(void *arg, uint16_t p, uint8_t v) { (void)arg; if (fs_access()) outb(p,v); }
static uint8_t fs_read(void *arg, unsigned a) { return fs_access() ? ((volatile uint8_t *)arg)[a] : 0xFF; }
static void fs_write(void *arg, unsigned a, uint8_t v) { if (fs_access()) ((volatile uint8_t *)arg)[a]=v; }
static struct vga_hardware_bus hardware;
int dosvm_fullscreen_port(uint16_t port, unsigned width, bool write, uint32_t *value)
{
    uint32_t result=0;
    for (unsigned i=0;i<width;i++) {
        unsigned p=(unsigned)port+i; uint8_t byte=0xFF;
        if (ciuki_vga_port(p)) {
            if (write) outb(p,(uint8_t)(*value>>(8*i)));
            else byte=inb(p);
        }
        result|=(uint32_t)byte<<(8*i);
    }
    if (!write) *value=result;
    return 0;
}
/* Granted aperture PTEs of the owner: their D bits join fullscreen_dirty;
 * clear=true also clears them (the caller's CR3 is the VM's, or the VM's
 * next CR3 load flushes its TLB). */
static uint32_t collect_dirty(struct dosvm *v, bool clear)
{
    if (fullscreen_owner!=v || !v->fullscreen || fullscreen_linear || !v->as.pd_phys) return fullscreen_dirty;
    uint32_t *pd=P2V(v->as.pd_phys);
    if (!(pd[0]&PTE_P)) return fullscreen_dirty;
    volatile uint32_t *pt=P2V(pd[0]&~0xFFFu);
    for (unsigned i=0;i<32;i++) if (pt[0xA0+i]&0x40u) {
        fullscreen_dirty|=1u<<i;
        if (clear) { pt[0xA0+i]&=~0x40u; if (read_cr3()==v->as.pd_phys) invlpg(0xA0000+i*PAGE_SIZE); }
    }
    return fullscreen_dirty;
}
/* Guest page g holds plane offsets (g*4096)&FFFFh for the A0000h maps and
 * ((g-8)*4096)&FFFFh for the B8000h map (GC06 may change while the guest
 * owns the ports); chain-4 and odd/even keep the offset's page. */
static uint16_t dirty_planes(uint32_t pages)
{
    uint16_t planes=0;
    for (unsigned g=0;g<32;g++) if (pages&(1u<<g)) planes|=(uint16_t)((1u<<(g&15))|(1u<<((g+8)&15)));
    return planes;
}
void dosvm_fullscreen_model_begin(struct dosvm *v)
{
    if (!v->fullscreen) return;
    transfer_vm=v;
    /* fx-22: the adapter differs from the model only where the guest wrote. */
    uint32_t flags=irq_save(); uint32_t pages=collect_dirty(v,false); irq_restore(flags);
    (void)vga_hardware_capture_pages(&v->vga,&hardware,dirty_planes(pages),0,0);
    v->fullscreen_model=true;
}
void dosvm_fullscreen_model_end(struct dosvm *v)
{
    if (!v->fullscreen) { if (transfer_vm==v) transfer_vm=0; return; }
    vga_hardware_load(&v->vga,&hardware);
    if (!fullscreen_revoked) {
        /* Model and adapter agree again: restart the dirty record. */
        uint32_t flags=irq_save(); (void)collect_dirty(v,true); fullscreen_dirty=0; irq_restore(flags);
    }
    v->fullscreen_model=false;
    if (transfer_vm==v) transfer_vm=0;
}
bool dosvm_fullscreen_aperture(struct dosvm *v, uint32_t linear, unsigned bytes, bool write)
{
    if (fullscreen_owner!=v || !v->fullscreen || v->vbe_mode || v->fullscreen_model || !fs_access()) return false;
    if (write && bytes && linear>=0xA0000 && linear<0xC0000) {
        uint32_t first=(linear-0xA0000)>>12, last=(linear+bytes-1-0xA0000)>>12;
        for (uint32_t g=first;g<=last && g<32;g++) fullscreen_dirty|=1u<<g;
    }
    return true;
}
bool dosvm_fullscreen_pinned(const struct dosvm *v)
{
    return v->fullscreen_hold || (fullscreen_busy && (fullscreen_owner==v || transfer_vm==v));
}
bool dosvm_fullscreen_service(void) { return fullscreen_busy && fullscreen_service_task==g_current; }
static bool same_key(struct dosvm_key a, struct dosvm_key b)
{ return a.id==b.id && a.generation==b.generation; }
static void entry_error(struct dosvm_key key,int error)
{
    uint32_t flags=irq_save();
    struct dosvm *v=dosvm_lookup(key);
    if (v) { v->fullscreen_error=error; dosvm_present_notify(v); }
    irq_restore(flags);
}
static int entry_windowed(struct dosvm_key key,int reason)
{
    klog("[dosvm] fullscreen_refused vm=%u generation=%u error=%d continuation=windowed",
        key.id,key.generation,reason);
    uint32_t flags=irq_save();
    struct dosvm *v=dosvm_lookup(key);
    if (v) { v->fullscreen_refusals++; v->fullscreen_refusal=reason; }
    irq_restore(flags);
    entry_error(key,0);
    return DISPLAY_FULLSCREEN_WINDOWED;
}
bool dosvm_fullscreen_active(struct dosvm_key key)
{ return fullscreen_owner && same_key(key,fullscreen_owner->info.key); }
void dosvm_fullscreen_switch(const struct task *t)
{
    arch_vga_io(fullscreen_owner && fullscreen_owner->fullscreen &&
        !fullscreen_linear &&
        !fullscreen_owner->fullscreen_hold && fullscreen_owner->info.state<DOSVM_STOPPING &&
        fullscreen_owner->task==t);
}
static void unhold(struct dosvm *v)
{
    dosvm_client_priority(v);
    if (v->task) task_input_withdraw(v->task);
    v->fullscreen_hold=false;
    dosvm_present_notify(v);
    dosvm_idle_wake(v);
}
static bool park_released(void *arg)
{
    const struct dosvm *v=arg;
    return (!v->fullscreen_hold && !v->fullscreen_suspended && !v->clipboard_mark) || v->info.state>=DOSVM_STOPPING;
}
void dosvm_fullscreen_checkpoint(struct dosvm *v)
{
    if (!v->fullscreen_hold && !v->fullscreen_suspended && !v->clipboard_mark) return;
    v->fullscreen_parked=true;
    /* fx-22: a suspended VM is not scheduled (VMM Suspend_VM; VPICD
     * simulates no interrupt into it). The former 1 ms sleep loop kept the
     * focused VM runnable, with its pending virtual IRQ urgent, every tick:
     * each wake preempted the switch transaction at its next safe point
     * (e831e685: a 226 ms capture took 2230 ms). Unhold, stop and resume
     * wake this queue; the bound covers a resume path without a wake. */
    while (!park_released(v))
        (void)kwait_wait_until(&v->wake,park_released,v,deadline_after_ms(FS_PARK_RECHECK_MS));
    v->fullscreen_parked=false;
}
static int park(struct dosvm *v,unsigned timeout)
{
    v->fullscreen_hold=true;
    dosvm_idle_wake(v);
    uint32_t flags=irq_save(); dosvm_fullscreen_switch(g_current); irq_restore(flags);
    uint64_t began=g_ticks;
    int e=0;
    while (v->started && v->task && task_alive(v->task) &&
           !v->fullscreen_parked) {
        /* fx-22: never tear the VM's own synthetic VGA BIOS transfer (a mode
         * set when Quake quits): it keeps the adapter until it ends, bounded. */
        unsigned limit=transfer_vm==v && timeout<FS_PARK_TRANSFER_MS ? FS_PARK_TRANSFER_MS : timeout;
        if (g_ticks-began>=limit) { e=-EBUSY; break; }
        task_sleep_ms(1);
    }
    if (fullscreen_owner==v) fullscreen_revoked=true;
    return e;
}
static int map_aperture(struct dosvm *v, bool grant)
{
    if (fullscreen_linear) return 0; /* VBE keeps its private RAM mapping */
    uint32_t *pd=P2V(v->as.pd_phys);
    if (!(pd[0]&PTE_P)) return -EFAULT;
    uint32_t *pt=P2V(pd[0]&~0xFFFu);
    for (unsigned i=0;i<32;i++) {
        uint32_t a=0xA0000+i*PAGE_SIZE;
        /* All 32 PTEs already exist. as_map intentionally accepts RAM flags
         * only; physical VGA must retain UC cache attributes and ownership
         * of these borrowed frames must never reach as_destroy. */
        pt[a>>12]=grant ? a|PTE_P|PTE_U|PTE_W|PTE_PCD|PTE_PWT : fullscreen_ptes[i];
        if (read_cr3()==v->as.pd_phys) invlpg(a);
    }
    return 0;
}
void dosvm_fullscreen_mode_changed(struct dosvm *v)
{
    if (fullscreen_owner!=v) return;
    /* A guest worker cannot synchronously park itself. Finish the synthetic
     * operation, then park at its normal checkpoint for the display worker. */
    arch_vga_io(false); map_aperture(v,false);
    fullscreen_model_changed=true; fullscreen_return=true; return_cause(DOSVM_FS_MODE_CHANGE);
    fullscreen_revoked=true; if (transfer_vm==v) transfer_vm=0;
    v->fullscreen_hold=true;
    display_fullscreen_return_request();
}
static void linear_palette(struct dosvm *v)
{
    if (fullscreen_palette==v->vga.display_changes) return;
    outb(0x3C6,v->vga.dac_mask);outb(0x3C8,0);
    /* Native packed setup uses X.Org's eight-bit CLUT; guests retain the
     * six-bit VGA DAC advertised by the synthetic VBE interface. */
    for (unsigned i=0;i<256;i++) for (unsigned k=0;k<3;k++) {
        unsigned c=v->vga.dac[i][k];outb(0x3C9,(c<<2)|(c>>4));
    }
    fullscreen_palette=v->vga.display_changes;
}
int dosvm_fullscreen_enter(struct dosvm_key key)
{
    struct dosvm *v=dosvm_lookup(key);
    if (!v || !dosvm_valid(key)) return -ESRCH;
    if (dosvm_fullscreen_active(key)) return 0;
    if (fullscreen_busy || fullscreen_owner) return -EBUSY;
    if (!v->owner_pid || !v->window_input || !same_key(key,dosvm_focused_key())) return -EACCES;
    struct fs_boost boost={0};
    uint8_t bios_mode=0;
    if (dosvm_read(v,0x449,&bios_mode,1)) return entry_windowed(key,-EOPNOTSUPP);
    uint16_t mode=v->vbe_mode ? v->vbe_mode : bios_mode;
    if (v->vbe_mode ? !display_fullscreen_supported(mode) : !cvga_find_mode(mode))
        return entry_windowed(key,-EOPNOTSUPP);
    fullscreen_busy=true;
    fullscreen_service_task=g_current;
    boost=boost_begin();
    fullscreen_linear=v->vbe_mode!=0; fullscreen_model_changed=false;
    v->fullscreen_error=0; v->fullscreen_cause=DOSVM_FS_NONE; fullscreen_cause_next=0;
    /* Persistence sleeps: pin the VM before the first record can yield. */
    v->fullscreen_hold=true;
    klog("[dosvm] fullscreen_request vm=%u generation=%u mode=%u",key.id,key.generation,mode);
    int e=display_fullscreen_checkpoint("vm-park",0);
    bool recovery_failed=false;
    if (e) goto resume;
    e=park(v,2000);
    if (e) goto resume;
    if (!dosvm_valid(key)) { e=-ESRCH; goto resume; }
    if ((e=display_fullscreen_checkpoint("vga-save",0))) goto resume;
    dosvm_vga_sync(v,true);
    for (unsigned i=0;i<32;i++) as_lookup(&v->as,0xA0000+i*PAGE_SIZE,&fullscreen_ptes[i]);
    if (!fullscreen_linear && !fullscreen_mapping) fullscreen_mapping=vmm_map_mmio(0xA0000,0x20000,true);
    if (!fullscreen_linear && !fullscreen_mapping) { e=-ENOMEM; goto resume; }
    hardware=(struct vga_hardware_bus){fullscreen_mapping,fs_in,fs_out,fs_read,fs_write};
    if ((e=display_fullscreen_begin(mode))) {
        recovery_failed=e<0;
        goto resume;
    }
    if ((e=display_fullscreen_checkpoint("vga-load",0))) goto rollback;
    unsigned bad=0;
    if (fullscreen_linear) {
        fullscreen_palette=~v->vga.display_changes; linear_palette(v);
        e=dosvm_vbe_flush(v,true);
        if (e<0) goto rollback;
        e=0;
    } else {
        vga_hardware_load(&v->vga,&hardware);
        bad=vga_hardware_verify(&v->vga,&hardware);
    }
    if (bad) {
        klog("[dosvm] fullscreen_transport_fault vm=%u registers=%x",key.id,bad);
        display_fullscreen_checkpoint("vga-readback",-EIO);
        e=-EIO; goto rollback;
    }
    if (!dosvm_valid(key) || !same_key(key,dosvm_focused_key())) { e=-EINTR; goto rollback; }
    if ((e=display_fullscreen_checkpoint("guest-grant",0))) goto rollback;
    if ((e=map_aperture(v,true))) { map_aperture(v,false); goto rollback; }
    dosvm_kbd_release(v); dosvm_mouse_event(v,0,0,0);
    fullscreen_dirty=0; fullscreen_revoked=false; /* the load above is a full synchronisation */
    fullscreen_owner=v; v->fullscreen=true;
    fullscreen_background=false;
    v->fullscreen_progress=g_ticks; v->fullscreen_ip=UINT32_MAX;
    v->fullscreen_seconds=v->fullscreen_frame_seconds=0;
    fullscreen_sample_tick=g_ticks; fullscreen_sample_frame=false;
    fullscreen_frame_seen=mode<=3; /* a static text prompt need not store again */
    /* Arm before resuming: the native worker observes hardware PTE D bits,
     * independent of guest instruction progress and idle hints. */
    e=display_fullscreen_checkpoint("watchdog-armed",0);
    if (e) {
        fullscreen_owner=0; v->fullscreen=false; map_aperture(v,false);
        goto rollback;
    }
    fullscreen_frame_deadline=deadline_after_ms(5000);
    klog("[dosvm] fullscreen=1 vm=%u generation=%u mode=%u",key.id,key.generation,mode);
    goto resume;
rollback:
    {
        int restored=display_fullscreen_end();
        recovery_failed=restored!=0;
        if (restored) e=restored;
    }
resume:
    unhold(v);
    fullscreen_busy=false;
    boost_end(&boost);
    if (e && !recovery_failed) return entry_windowed(key,e);
    entry_error(key,e);
    return e;
}
static bool capture_expired(void *arg) { return deadline_passed(*(const uint64_t *)arg); }
static unsigned page_count(uint16_t pages) { unsigned n=0; for (;pages;pages&=(uint16_t)(pages-1)) n++; return n; }
int dosvm_fullscreen_leave(struct dosvm_key key, bool background)
{
    if (!dosvm_fullscreen_active(key)) return 0;
    if (fullscreen_busy) return -EBUSY;
    fullscreen_busy=true;
    fullscreen_service_task=g_current;
    struct fs_boost boost=boost_begin();
    uint64_t began=g_ticks;
    struct dosvm *v=fullscreen_owner;
    v->fullscreen_cause=fullscreen_cause_next ? fullscreen_cause_next : DOSVM_FS_REQUEST;
    fullscreen_cause_next=0;
    display_fullscreen_return_request();
    display_fullscreen_checkpoint("guest-revoke",0);
    /* Leave at least 1750 ms of the 2 s return budget for capture/native
     * recovery. A blocked device owner cannot be killed or forcibly unlocked.
     * Revoke its physical lease and suspend it at its next VM checkpoint. */
    bool transfer=transfer_vm==v;
    int parked=park(v,FS_PARK_MS);
    uint64_t park_ms=g_ticks-began;
    if (parked) klog("[dosvm] fullscreen_quiesced vm=%u generation=%u park_ms=%llu transfer=%u",
        key.id,key.generation,park_ms,(unsigned)transfer);
    uint32_t flags=irq_save();
    arch_vga_io(false);
    uint32_t dirty=collect_dirty(v,false);
    v->fullscreen=false;
    int e=map_aperture(v,false);
    irq_restore(flags);
    display_fullscreen_checkpoint("vga-capture",0);
    uint64_t capture_began=g_ticks;
    uint16_t wanted=0,captured=0;
    const char *capture="skipped";
    if (!parked && !fullscreen_linear && !fullscreen_model_changed) {
        /* fx-22: only what the guest wrote since the last full transfer,
         * page by page, within FS_CAPTURE_MS. */
        uint64_t until=deadline_after_ms(FS_CAPTURE_MS);
        wanted=dirty_planes(dirty);
        captured=vga_hardware_capture_pages(&v->vga,&hardware,wanted,capture_expired,&until);
        capture=captured==wanted ? "complete" : "partial";
    }
    bool capture_lost=captured!=wanted;
    uint64_t capture_ms=g_ticks-capture_began;
    display_fullscreen_checkpoint("vga-captured",capture_lost ? -ETIMEDOUT : 0);
    if (transfer_vm==v) transfer_vm=0;
    fullscreen_dirty=0;
    v->fullscreen_model=false;
    v->video_presented=false; v->video_lut_valid=false; v->video_read_window=false;
    dosvm_kbd_release(v); dosvm_mouse_event(v,0,0,0); v->pointer_known=false;
    uint64_t restore_began=g_ticks;
    int restored=display_fullscreen_end();
    uint64_t restore_ms=g_ticks-restore_began;
    int revoked=e;
    if (!e) e=restored;
    /* A guest state that could not be saved within its bound leaves the VM
     * suspended in its window (f1-80 park policy): the desktop returns. */
    if (e || parked || capture_lost) v->fullscreen_error=e ? e : -ETIMEDOUT;
    fullscreen_owner=0; fullscreen_return=false;
    desktop_next=0; desktop_pending=background && fullscreen_background ? 5 : 1;
    v->fullscreen_suspended=(parked || capture_lost || (background && v->fullscreen_pause)) && dosvm_valid(key);
    unhold(v);
    klog("[dosvm] fullscreen_return vm=%u generation=%u park_ms=%llu park=%s transfer=%u capture=%s capture_pages=%u/%u capture_ms=%llu restore_ms=%llu total_ms=%llu result=%d",
        key.id,key.generation,park_ms,parked ? "timeout" : "parked",(unsigned)transfer,capture,
        page_count(captured),page_count(wanted),capture_ms,restore_ms,
        g_ticks-began,e);
    klog("[dosvm] fullscreen=0 vm=%u generation=%u result=%d suspended=%u revoke_result=%d restore_result=%d cause=%s",
        key.id,key.generation,e,v->fullscreen_suspended,revoked,restored,
        fullscreen_causes[v->fullscreen_cause<DOSVM_FS_CAUSES ? v->fullscreen_cause : 0]);
    fullscreen_busy=false;
    boost_end(&boost);
    return e;
}
void dosvm_fullscreen_stopping(struct dosvm *v)
{
    if (fullscreen_owner==v) {
        display_fullscreen_return_request();
        uint32_t flags=irq_save();
        arch_vga_io(false);
        (void)collect_dirty(v,false); /* the PTEs below no longer show guest writes */
        map_aperture(v,false);
        fullscreen_revoked=true;
        fullscreen_return=true; return_cause(DOSVM_FS_STOPPING);
        irq_restore(flags);
    }
    v->fullscreen_suspended=false;
}
void dosvm_fullscreen_progress(struct dosvm *v, uint32_t cs, uint32_t ip)
{
    if (fullscreen_owner!=v) return;
    if (v->fullscreen_ip!=ip || v->fullscreen_cs!=cs) {
        v->fullscreen_ip=ip; v->fullscreen_cs=cs; v->fullscreen_progress=g_ticks;
    }
}
void dosvm_fullscreen_poll(void)
{
    uint32_t flags=irq_save();
    if (fullscreen_busy) { irq_restore(flags); return; }
    struct dosvm *v=fullscreen_owner;
    if (v) fullscreen_busy=true; /* checkpoint waits must pin the owner */
    irq_restore(flags);
    if (v) {
        if (fullscreen_linear && !fullscreen_return) {
            linear_palette(v);
            int frame=dosvm_vbe_flush(v,false);
            if (frame<0) { v->fullscreen_error=frame; fullscreen_return=true; return_cause(DOSVM_FS_PRESENT_ERROR); }
            if (frame>0) fullscreen_sample_frame=true;
            if (frame>0 && !fullscreen_frame_seen) {
                fullscreen_frame_seen=true;
                klog("[dosvm] fullscreen_first_frame vm=%u generation=%u tick=%llu source=linear-write",
                    v->info.key.id,v->info.key.generation,g_ticks);
                display_fullscreen_checkpoint("first-guest-frame",0);
            }
        }
        if (!fullscreen_linear && !fullscreen_frame_seen && v->as.pd_phys) {
            uint32_t *pd=P2V(v->as.pd_phys);
            if (pd[0]&PTE_P) {
                volatile uint32_t *pt=P2V(pd[0]&~0xFFFu);
                for (unsigned i=0xA0;i<0xC0;i++)
                    if (pt[i]&0x40u) fullscreen_frame_seen=true;
            }
            if (fullscreen_frame_seen) {
                klog("[dosvm] fullscreen_first_frame vm=%u generation=%u tick=%llu source=aperture-write",
                    v->info.key.id,v->info.key.generation,g_ticks);
                display_fullscreen_checkpoint("first-guest-frame",0);
            }
        }
        /* f3-61: once a first frame was seen, one sample per second of the
         * aperture dirty bits (cleared here; the VM's CR3 reload flushes its
         * TLB). Frame evidence for probes, never a watchdog input. */
        if (fullscreen_frame_seen && !fullscreen_return && g_ticks-fullscreen_sample_tick>=1000) {
            fullscreen_sample_tick=g_ticks;
            if (!fullscreen_linear && v->as.pd_phys) {
                uint32_t flags=irq_save();
                uint32_t *pd=P2V(v->as.pd_phys);
                if (fullscreen_owner==v && v->fullscreen && (pd[0]&PTE_P)) {
                    volatile uint32_t *pt=P2V(pd[0]&~0xFFFu);
                    for (unsigned i=0xA0;i<0xC0;i++) if (pt[i]&0x40u) {
                        fullscreen_sample_frame=true; fullscreen_dirty|=1u<<(i-0xA0); pt[i]&=~0x40u;
                        if (read_cr3()==v->as.pd_phys) invlpg(i<<12);
                    }
                }
                irq_restore(flags);
            }
            v->fullscreen_seconds++;
            if (fullscreen_sample_frame) v->fullscreen_frame_seconds++;
            fullscreen_sample_frame=false;
        }
        bool idle=v->fullscreen_suspended || v->idle.sleeping ||
            (v->idle.seen && g_ticks-v->idle.last_tick<1000) ||
            (v->monitor.state==V86_HALTED && v->monitor.vif);
        bool frame_timeout=!fullscreen_frame_seen && deadline_passed(fullscreen_frame_deadline);
        bool progress_timeout=!idle && g_ticks-v->fullscreen_progress>=15000;
        struct process *p=proc_find(v->owner_pid);
        struct process *desktop=proc_find(grants_display_pid());
        if (fullscreen_return || !dosvm_valid(v->info.key) || !p || p->state>=PROC_STOPPING ||
            !desktop || desktop->state>=PROC_STOPPING || !same_key(v->info.key,dosvm_focused_key()) ||
            !v->task || !task_alive(v->task) || frame_timeout || progress_timeout) {
            const char *reason=fullscreen_return ? "return-or-fault" : frame_timeout ? "first-frame-timeout" :
                progress_timeout ? "guest-progress-timeout" : "owner-lost";
            if (!fullscreen_return)
                return_cause(frame_timeout ? DOSVM_FS_FIRST_FRAME_TIMEOUT : progress_timeout ? DOSVM_FS_PROGRESS_TIMEOUT : DOSVM_FS_OWNER_LOST);
            uint8_t cause=fullscreen_cause_next<DOSVM_FS_CAUSES ? fullscreen_cause_next : DOSVM_FS_NONE;
            display_fullscreen_return_request();
            /* f3-61: the VM keeps running in its window; only cause=stopping
             * is a VM that stopped by itself (vector!=0: a guest fault). */
            klog("[dosvm] fullscreen_watchdog vm=%u generation=%u reason=%s cause=%s stop_reason=%u vector=%u",
                 v->info.key.id,v->info.key.generation,reason,fullscreen_causes[cause],
                 cause==DOSVM_FS_STOPPING ? v->info.reason : 0,cause==DOSVM_FS_STOPPING ? v->info.fault_vector : 0);
            display_fullscreen_checkpoint("watchdog-return",0);
            if (frame_timeout || progress_timeout) v->fullscreen_error=-ETIMEDOUT;
            struct dosvm_key key=v->info.key;
            fullscreen_busy=false;
            dosvm_fullscreen_leave(key,fullscreen_background);
        } else fullscreen_busy=false;
    } else if (fullscreen_request.id) {
        struct dosvm_key key=fullscreen_request;
        fullscreen_request=(struct dosvm_key){0};
        struct dosvm *v=dosvm_lookup(key);
        if (v && same_key(key,dosvm_focused_key())) v->fullscreen_suspended=false;
        int e=dosvm_fullscreen_enter(key);
        if (e==DISPLAY_FULLSCREEN_WINDOWED) e=0;
        entry_error(key,e);
        if (e) klog("[dosvm] fullscreen_refused vm=%u generation=%u error=%d",key.id,key.generation,e);
    }
}
/* Merged PS/2/USB/firmware events, before the desktop queue. This code only
 * changes bounded VM device state and queues requests; never switches modes. */
bool dosvm_fullscreen_input(uint16_t type, uint16_t code, int32_t value)
{
    static bool left_alt,right_alt,left_ctrl,right_ctrl;
    static uint16_t swallowed;
    if (type==INPUT_RESYNC) {
        left_alt=right_alt=left_ctrl=right_ctrl=false; swallowed=0;
        if (fullscreen_owner) {
            dosvm_kbd_release(fullscreen_owner); dosvm_mouse_event(fullscreen_owner,0,0,0);
        }
        return false;
    }
    if (type==INPUT_KEY) {
        if (code==0x38) left_alt=value!=0;
        if (code==0x138) right_alt=value!=0;
        if (code==0x1D) left_ctrl=value!=0;
        if (code==0x11D) right_ctrl=value!=0;
        if (swallowed==code) { if (!value) swallowed=0; return true; }
        bool alt=left_alt||right_alt, ctrl=left_ctrl||right_ctrl;
        if (ctrl && alt && (code==0x153 || code==0x53)) return false; /* system path */
        if (value==1 && alt && !ctrl && (code==0x1C || code==0x11C || (code==0x0F && fullscreen_owner))) {
            if (fullscreen_busy) { swallowed=code; return true; }
            struct dosvm_key key=dosvm_focused_key();
            if (fullscreen_owner) {
                display_fullscreen_return_request();
                fullscreen_return=true; fullscreen_background=code==0x0F;
                return_cause(code==0x0F ? DOSVM_FS_BACKGROUND : DOSVM_FS_HOTKEY);
            }
            else {
                struct dosvm *v=dosvm_lookup(key);
                if (!v || !v->owner_pid || !v->window_input) return false;
                fullscreen_request=key;
            }
            swallowed=code; return true;
        }
    }
    struct dosvm *v=fullscreen_owner;
    if (!v) return false;
    if (v->fullscreen_hold || fullscreen_return || !dosvm_valid(v->info.key)) return true;
    if (type==INPUT_KEY) dosvm_kbd_event(v,code,value);
    else if (type==INPUT_REL && code!=INPUT_WHEEL)
        dosvm_mouse_event(v,code==INPUT_X ? value : 0,code==INPUT_Y ? value : 0,v->mouse.buttons);
    else if (type==INPUT_BTN && code<=INPUT_MIDDLE)
        dosvm_mouse_event(v,0,0,value ? v->mouse.buttons|(1u<<code) : v->mouse.buttons&~(1u<<code));
    dosvm_idle_wake(v);
    return !(type==INPUT_KEY && (code==0x38 || code==0x138 || code==0x1D || code==0x11D));
}
bool dosvm_fullscreen_desktop_event(struct ciuki_input_event *out)
{
    if (desktop_next>=desktop_pending) return false;
    unsigned n=desktop_next++;
    *out=(struct ciuki_input_event){.source=INPUT_NATIVE,.monotonic_ns=g_ticks*1000000};
    if (!n) out->type=CIUKI_INPUT_RESYNC;
    else {
        out->type=CIUKI_INPUT_KEY;
        out->code=(n==1 || n==4) ? 0x38 : 0x0F;
        out->value=n<=2;
    }
    return true;
}
