/* Single physical VGA owner. Transfers run in thread context, guest access
 * is revoked before capture/restore. Windowed rendering remains in dosvm_vga.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/display.h>
#include <ciuki/input.h>
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
static uint8_t fs_in(void *arg, uint16_t p) { (void)arg; return inb(p); }
static void fs_out(void *arg, uint16_t p, uint8_t v) { (void)arg; outb(p,v); }
static uint8_t fs_read(void *arg, unsigned a) { return ((volatile uint8_t *)arg)[a]; }
static void fs_write(void *arg, unsigned a, uint8_t v) { ((volatile uint8_t *)arg)[a]=v; }
static struct vga_hardware_bus hardware;
int dosvm_fullscreen_port(uint16_t port, unsigned width, bool write, uint32_t *value)
{
    uint32_t result=0;
    for (unsigned i=0;i<width;i++) {
        unsigned p=(unsigned)port+i; uint8_t byte=0xFF;
        if (p>=0x3B0 && p<0x3E0) {
            if (write) outb(p,(uint8_t)(*value>>(8*i)));
            else byte=inb(p);
        }
        result|=(uint32_t)byte<<(8*i);
    }
    if (!write) *value=result;
    return 0;
}
void dosvm_fullscreen_model_begin(struct dosvm *v)
{
    if (!v->fullscreen) return;
    vga_hardware_capture(&v->vga,&hardware);
    v->fullscreen_model=true;
}
void dosvm_fullscreen_model_end(struct dosvm *v)
{
    if (!v->fullscreen) return;
    vga_hardware_load(&v->vga,&hardware);
    v->fullscreen_model=false;
}
static bool same_key(struct dosvm_key a, struct dosvm_key b)
{ return a.id==b.id && a.generation==b.generation; }
static void entry_error(struct dosvm_key key,int error)
{
    uint32_t flags=irq_save();
    struct dosvm *v=dosvm_lookup(key);
    if (v) v->fullscreen_error=error;
    irq_restore(flags);
}
bool dosvm_fullscreen_active(struct dosvm_key key)
{ return fullscreen_owner && same_key(key,fullscreen_owner->info.key); }
void dosvm_fullscreen_switch(const struct task *t)
{
    arch_vga_io(fullscreen_owner && fullscreen_owner->fullscreen &&
        !fullscreen_owner->fullscreen_hold && fullscreen_owner->info.state<DOSVM_STOPPING &&
        fullscreen_owner->task==t);
}
static void unhold(struct dosvm *v)
{
    v->fullscreen_hold=false;
    dosvm_idle_wake(v);
}
void dosvm_fullscreen_checkpoint(struct dosvm *v)
{
    if (!v->fullscreen_hold && !v->fullscreen_suspended) return;
    v->fullscreen_parked=true;
    while ((v->fullscreen_hold || v->fullscreen_suspended) && v->info.state<DOSVM_STOPPING)
        task_sleep_ms(1);
    v->fullscreen_parked=false;
}
static int park(struct dosvm *v)
{
    v->fullscreen_hold=true;
    dosvm_idle_wake(v);
    uint32_t flags=irq_save(); dosvm_fullscreen_switch(g_current); irq_restore(flags);
    uint64_t until=deadline_after_ms(2000);
    while (v->started && v->task && task_alive(v->task) &&
           !v->fullscreen_parked) {
        if (deadline_passed(until)) { unhold(v); return -EBUSY; }
        task_sleep_ms(1);
    }
    return 0;
}
static int map_aperture(struct dosvm *v, bool grant)
{
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
int dosvm_fullscreen_enter(struct dosvm_key key)
{
    struct dosvm *v=dosvm_lookup(key);
    if (!v || !dosvm_valid(key)) return -ESRCH;
    if (dosvm_fullscreen_active(key)) return 0;
    if (fullscreen_busy || fullscreen_owner) return -EBUSY;
    if (!v->owner_pid || !v->window_input || !same_key(key,dosvm_focused_key())) return -EACCES;
    uint8_t mode=0;
    if (dosvm_read(v,0x449,&mode,1) || !cvga_find_mode(mode)) return -EOPNOTSUPP;
    fullscreen_busy=true;
    v->fullscreen_error=0;
    /* Persistence sleeps: pin the VM before the first record can yield. */
    v->fullscreen_hold=true;
    klog("[dosvm] fullscreen_request vm=%u generation=%u mode=%u",key.id,key.generation,mode);
    int e=display_fullscreen_checkpoint("vm-park",0);
    if (e) goto resume;
    e=park(v);
    if (e) goto done;
    if (!dosvm_valid(key)) { e=-ESRCH; goto resume; }
    if ((e=display_fullscreen_checkpoint("vga-save",0))) goto resume;
    dosvm_vga_sync(v,true);
    for (unsigned i=0;i<32;i++) as_lookup(&v->as,0xA0000+i*PAGE_SIZE,&fullscreen_ptes[i]);
    if (!fullscreen_mapping) fullscreen_mapping=vmm_map_mmio(0xA0000,0x20000,true);
    if (!fullscreen_mapping) { e=-ENOMEM; goto resume; }
    hardware=(struct vga_hardware_bus){fullscreen_mapping,fs_in,fs_out,fs_read,fs_write};
    if ((e=display_fullscreen_begin(mode))) goto resume;
    if ((e=display_fullscreen_checkpoint("vga-load",0))) goto rollback;
    vga_hardware_load(&v->vga,&hardware);
    unsigned bad=vga_hardware_verify(&v->vga,&hardware);
    if (bad) {
        klog("[dosvm] fullscreen_transport_fault vm=%u registers=%x",key.id,bad);
        display_fullscreen_checkpoint("vga-readback",-EIO);
        e=-EIO; goto rollback;
    }
    if (!dosvm_valid(key) || !same_key(key,dosvm_focused_key())) { e=-EINTR; goto rollback; }
    if ((e=display_fullscreen_checkpoint("guest-grant",0))) goto rollback;
    if ((e=map_aperture(v,true))) { map_aperture(v,false); goto rollback; }
    dosvm_kbd_release(v); dosvm_mouse_event(v,0,0,0);
    fullscreen_owner=v; v->fullscreen=true;
    fullscreen_background=false;
    v->fullscreen_progress=g_ticks; v->fullscreen_ip=UINT32_MAX;
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
    display_fullscreen_end();
resume:
    unhold(v);
done:
    fullscreen_busy=false;
    entry_error(key,e);
    return e;
}
int dosvm_fullscreen_leave(struct dosvm_key key, bool background)
{
    if (!dosvm_fullscreen_active(key)) return 0;
    if (fullscreen_busy) return -EBUSY;
    fullscreen_busy=true;
    struct dosvm *v=fullscreen_owner;
    display_fullscreen_return_request();
    display_fullscreen_checkpoint("guest-revoke",0);
    int e=park(v);
    if (e) { fullscreen_busy=false; return e; }
    uint32_t flags=irq_save();
    arch_vga_io(false); v->fullscreen=false;
    e=map_aperture(v,false);
    irq_restore(flags);
    display_fullscreen_checkpoint("vga-capture",0);
    vga_hardware_capture(&v->vga,&hardware);
    v->video_presented=false; v->video_lut_valid=false; v->video_read_window=false;
    dosvm_kbd_release(v); dosvm_mouse_event(v,0,0,0); v->pointer_known=false;
    int restored=display_fullscreen_end();
    int revoked=e;
    if (!e) e=restored;
    if (e) v->fullscreen_error=e;
    fullscreen_owner=0; fullscreen_return=false;
    desktop_next=0; desktop_pending=background && fullscreen_background ? 5 : 1;
    v->fullscreen_suspended=background && v->fullscreen_pause && dosvm_valid(key);
    unhold(v);
    klog("[dosvm] fullscreen=0 vm=%u generation=%u result=%d suspended=%u revoke_result=%d restore_result=%d",
        key.id,key.generation,e,v->fullscreen_suspended,revoked,restored);
    fullscreen_busy=false;
    return e;
}
void dosvm_fullscreen_stopping(struct dosvm *v)
{
    if (fullscreen_owner==v) {
        display_fullscreen_return_request();
        uint32_t flags=irq_save();
        arch_vga_io(false); map_aperture(v,false);
        fullscreen_return=true;
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
        if (!fullscreen_frame_seen && v->as.pd_phys) {
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
            display_fullscreen_return_request();
            klog("[dosvm] fullscreen_watchdog vm=%u generation=%u reason=%s",v->info.key.id,v->info.key.generation,reason);
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
