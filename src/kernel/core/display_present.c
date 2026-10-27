/* F5-08: revocable present capability; no framebuffer mapping or BIOS authority.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/display.h>
#include <ciuki/present_abi.h>
#include <ciuki/supervisor.h>
#include <ciuki/win32.h>
#include "../vm/vga_presenter.h"
extern bool w32_service_desktop_client(struct process *, uint32_t);

/* Lock order: lease -> framebuffer presenter. Mode transactions NEVER hold
 * either mutex across firmware. transfer excludes all new operations there.
 * fx-5 follow-up: the presenter lives in drivers/fbdev.c, so the lease is an
 * outer level rather than an address that only the link order puts below it. */
static struct kmutex lease_lock = KMUTEX_CLASS("display.lease", KMUTEX_LEVEL_OUTER);
static bool active, transfer;
static uint32_t holder, desktop_pid, token, fb_generation, scale;
static uint16_t saved_mode;
static struct ciuki_present_surface layout;
static uint32_t palette[256];
static struct ciuki_present_gamma gamma;
static uint64_t next_frame;
static bool frame_running;
static bool awaiting_first_frame;
static uint64_t first_frame_deadline;

bool display_present_active(void) { return active || transfer; }
static bool owner_live(uint32_t pid)
{
    struct process *p=proc_find(pid);
    return p && p->state==PROC_LIVE;
}
static bool valid(struct process *p, const struct ciuki_w32_display_args *a)
{
    return active && !transfer && p && p->state==PROC_LIVE && p->pid==holder &&
        a->token==token && a->generation==fb_generation && owner_live(desktop_pid) &&
        grants_display_owner(proc_find(desktop_pid)) &&
        fbdev_get()->present && fbdev_get()->generation==fb_generation;
}
static int revoke(uint32_t expected_token, uint32_t expected_generation, uint32_t expected_holder)
{
    kmutex_lock(&lease_lock);
    if (expected_token && (!active || token!=expected_token || fb_generation!=expected_generation || holder!=expected_holder)) {
        kmutex_unlock(&lease_lock); return -EACCES;
    }
    if (transfer) { kmutex_unlock(&lease_lock); return -EBUSY; }
    if (!active) { kmutex_unlock(&lease_lock); return 0; }
    active=false; transfer=true; /* invalid before a parked pin/copy can resume */
    uint32_t pid=desktop_pid;
    uint16_t mode=saved_mode;
    kmutex_unlock(&lease_lock);
    struct process *d=proc_find(pid);
    int e=d ? display_set_for_present(d,mode,true) : -ESRCH;
    int resumed=grants_display_suspend(false);
    if (!e) e=resumed;
    if (!e) display_present_returned();
    kmutex_lock(&lease_lock);
    holder=desktop_pid=0; transfer=false;
    kmutex_unlock(&lease_lock);
    klog("[display] present_lease=revoke token=%u result=%d",token,e);
    return e ? e : 1;
}
int display_present_revoke_kernel(void) { return revoke(0,0,0); }
void display_present_process_exit(struct process *p)
{
    kmutex_lock(&lease_lock);
    uint32_t key=token, generation=fb_generation, pid=holder;
    bool needed=p && active && (p->pid==holder || p->pid==desktop_pid);
    kmutex_unlock(&lease_lock);
    if (needed) (void)revoke(key,generation,pid);
}
void display_present_poll(void)
{
    kmutex_lock(&lease_lock);
    uint32_t key=token, generation=fb_generation, pid=holder;
    bool needed=active && (!owner_live(holder) || !owner_live(desktop_pid) ||
        !grants_display_owner(proc_find(desktop_pid)) ||
        !w32_service_desktop_client(proc_find(desktop_pid),holder) ||
        (awaiting_first_frame && deadline_passed(first_frame_deadline)));
    kmutex_unlock(&lease_lock);
    if (needed) (void)revoke(key,generation,pid);
}
extern int render3d_native_prepare(void) __attribute__((weak));
static int grant(struct process *d, struct ciuki_w32_display_args *a)
{
    struct process *p=proc_find(a->pid);
    if (!p || p->personality!=PROC_WIN32 || p->state!=PROC_LIVE) return -ESRCH;
    if (!cvp_surface_valid(&a->surface) || a->scale>3 || a->mode>UINT16_MAX) return -EINVAL;
    kmutex_lock(&lease_lock);
    if (active || transfer || frame_running || display_fullscreen_active()) {
        kmutex_unlock(&lease_lock); return -EBUSY;
    }
    /* Reserve a never-reused interval before entering a sleeping transaction. */
    uint32_t fresh=gen_alloc();
    if (!fresh) { kmutex_unlock(&lease_lock); return -EOVERFLOW; }
    transfer=true;
    saved_mode=fbdev_get()->mode_id;
    uint32_t before_generation=fbdev_get()->generation;
    kmutex_unlock(&lease_lock);
    int e=display_worker_start();
    bool suspended=false;
    if (!e) { e=grants_display_suspend(true); suspended=!e; }
    if (!e) fbdev_cursor_hide();
    if (!e) e=display_set_for_present(d,(uint16_t)a->mode,true);
    const struct fb_device *f=fbdev_get();
    unsigned chosen=cvp_surface_scale(&a->surface,f->width,f->height,a->scale);
    if (!e && (!chosen || (f->bpp!=16 && f->bpp!=32))) e=-EOPNOTSUPP;
    /* Retained target evidence is rebound before publishing the new lease.
     * This may wait for 2D activation/marker I/O outside all display locks;
     * it does not consume a later GL QUERY's unchanged 2 s wire deadline.
     * A clean 3D refusal retains the ordinary software-capable lease. */
    uint32_t prepared_generation=f->generation;
    if (!e && render3d_native_prepare) (void)render3d_native_prepare();
    if (!e && (!f->present || f->generation!=prepared_generation)) e=-EAGAIN;
    /* Transactions can yield; re-prove both process identities on publication. */
    if (!e && (!owner_live(a->pid) || !owner_live(d->pid))) e=-ESRCH;
    if (!e && !w32_service_desktop_client(d,a->pid)) e=-EPERM;
    if (e) {
        if (suspended && f->mode_id!=saved_mode) {
            int rollback=display_set_for_present(d,saved_mode,true);
            if (rollback) {
                klog("[display] present_lease=rollback mode=%u error=%d",saved_mode,rollback);
                e=rollback;
            }
        }
        if (suspended) (void)grants_display_suspend(false);
        if (f->present && f->generation!=before_generation) display_present_returned();
    }
    kmutex_lock(&lease_lock);
    if (!e) {
        holder=a->pid; desktop_pid=d->pid; token=fresh; fb_generation=f->generation;
        scale=chosen; layout=a->surface; next_frame=0;
        awaiting_first_frame=true; first_frame_deadline=deadline_after_ms(10000);
        for (unsigned i=0;i<256;++i) {
            palette[i]=0;
            gamma.red[i]=gamma.green[i]=gamma.blue[i]=(uint16_t)(i*257);
        }
        active=true;
        a->token=token; a->generation=fb_generation; a->scale=scale;
    }
    transfer=false;
    kmutex_unlock(&lease_lock);
    klog("[display] present_lease=grant pid=%u token=%u generation=%u scale=%u result=%d",
        a->pid,fresh,fb_generation,chosen,e);
    return e;
}
struct frame_operation { struct uaddr *memory; struct ua_pin pin; uint32_t page; bool pinned, running; };
static void frame_cleanup(void *arg)
{
    struct frame_operation *op=arg;
    if (op->pinned) ua_unpin(op->memory,&op->pin);
    if (op->page) pmm_free(op->page);
    if (op->running) {
        kmutex_lock(&lease_lock); frame_running=false; kmutex_unlock(&lease_lock);
    }
}
static int frame(struct proc_thread *t, struct ciuki_w32_display_args *a)
{
    struct process *p=t->process;
    kmutex_lock(&lease_lock);
    if (!valid(p,a)) { kmutex_unlock(&lease_lock); return -EACCES; }
    if (frame_running) { kmutex_unlock(&lease_lock); return -EBUSY; }
    struct ciuki_present_surface s=layout;
    struct fb_device f=*fbdev_get();
    unsigned n=scale;
    uint64_t deadline=next_frame;
    frame_running=true;
    kmutex_unlock(&lease_lock);
    struct frame_operation op={.memory=p->memory,.running=true};
    t->cleanup=frame_cleanup; t->operation=&op;
    int e=0;
    while (g_ticks*UINT64_C(1000000)<deadline) {
        task_sleep_ms(1);
        kmutex_lock(&lease_lock);
        bool ok=valid(p,a);
        kmutex_unlock(&lease_lock);
        if (!ok) { e=-EACCES; break; }
    }
    if (!e) {
        e=ua_pin(p->memory,&op.pin,a->address,s.size,false);
        op.pinned=!e;
    }
    if (!e && !(op.page=pmm_alloc())) e=-ENOMEM;
    uint8_t *input=op.page ? P2V(op.page) : 0, *out=input ? input+2048 : 0;
    cvp_format fmt={.pitch=f.pitch,.width=(uint16_t)f.width,.height=(uint16_t)f.height,
        .bytes=f.bpp/8,.red_size=f.red_size,.red_pos=f.red_pos,.green_size=f.green_size,
        .green_pos=f.green_pos,.blue_size=f.blue_size,.blue_pos=f.blue_pos};
    unsigned w=s.width*n, h=s.height*n, ox=(f.width-w)/2, oy=(f.height-h)/2;
    for (unsigned y=0;y<f.height && !e;++y) {
        for (unsigned x=0;x<f.width && !e;x+=256) {
            unsigned count=f.width-x;
            if (count>256) count=256;
            memset(out,0,count*fmt.bytes);
            unsigned l=x>ox ? x : ox, r=x+count<ox+w ? x+count : ox+w;
            unsigned first=0, source_count=0, bytes=cvp_surface_bytes(s.format);
            if (y>=oy && y<oy+h && l<r) {
                first=(l-ox)/n;
                source_count=(r-ox+n-1)/n-first;
                e=ua_read(p->memory,input,a->address+(y-oy)/n*s.pitch+first*bytes,source_count*bytes);
            }
            /* Pin and reads may yield. The final proof is under the same lock
             * as revocation and spans the complete bounded native write. */
            kmutex_lock(&lease_lock);
            if (!e && !valid(p,a)) e=-EACCES;
            if (!e && source_count) {
                struct ciuki_present_surface tile={source_count,1,source_count*bytes,source_count*bytes,s.format};
                if (cvp_surface_row(&tile,input,palette,&gamma,&fmt,n,(l-ox)%n,r-l,
                    out+(l-x)*fmt.bytes,2048-(l-x)*fmt.bytes)) e=-EINVAL;
            }
            if (!e) e=fbdev_present_native(a->generation,x,y,out,count);
            kmutex_unlock(&lease_lock);
            kpreempt_point();
        }
    }
    kmutex_lock(&lease_lock);
    if (!e && !valid(p,a)) e=-EACCES;
    if (!e) {
        next_frame=cvp_present_deadline(g_ticks*UINT64_C(1000000));
        awaiting_first_frame=false;
    }
    kmutex_unlock(&lease_lock);
    t->cleanup=0; t->operation=0; frame_cleanup(&op);
    return e;
}
/* Completed owned-copy presentation. A matching tiled receipt can go
 * directly to the authorized native scanout; conversion/gamma uses CPU. */
struct render3d_service;
extern int render3d_scanout(struct render3d_service *,uint32_t,const struct ciuki_w32_render3d_args *,
                           const struct fb_device *) __attribute__((weak));
int display_present_render3d(void *service, uint32_t pid,
    const struct ciuki_w32_render3d_args *r, const void *pixels, uint32_t bytes)
{
    struct ciuki_w32_display_args a={.token=r->token,.generation=r->generation};
    struct process *p=proc_find(pid);
    /* NULL pixels: the frame is GPU-resident; only the direct native path
     * may run, -EOPNOTSUPP asks the caller for the read-back CPU copy. */
    uint32_t page=pixels ? pmm_alloc() : 0;
    if (pixels && !page) return -ENOMEM;
    kmutex_lock(&lease_lock);
    int e=!valid(p,&a) ? -EACCES : frame_running ? -EBUSY : 0;
    struct ciuki_present_surface s=layout;
    struct fb_device f=*fbdev_get();
    unsigned n=scale;
    if (!e && (s.format!=CIUKI_PRESENT_XRGB8888 || s.width!=r->width ||
        s.height!=r->height || s.pitch!=r->pitch || s.size!=bytes)) e=-EOPNOTSUPP;
    bool direct=n==1 && s.width<=f.width && s.height<=f.height && service && render3d_scanout;
    if (!e && !pixels && !direct) e=-EOPNOTSUPP;
    if (!e) frame_running=true;
    kmutex_unlock(&lease_lock);
    if (e) { if (page) pmm_free(page); return e; }
    if (direct) {
        /* A passing presenter/drawing conflict (2D activation, a cursor or
         * console write) is retried briefly outside the lease lock: one busy
         * moment must not fail SwapBuffers, which Quake III treats as fatal. */
        for (unsigned attempt=0;attempt<20;attempt++) {
            if (attempt) task_sleep_ms(1);
            kmutex_lock(&lease_lock);
            bool identity=true;
            for (unsigned i=0;i<256 && identity;i++)
                if (gamma.red[i]!=i*257u || gamma.green[i]!=i*257u || gamma.blue[i]!=i*257u) identity=false;
            e=!valid(proc_find(pid),&a) ? -EACCES : !identity ? -EOPNOTSUPP : render3d_scanout(service,pid,r,&f);
            kmutex_unlock(&lease_lock);
            if (e!=-EBUSY) break;
        }
        if (e!=-EOPNOTSUPP || !pixels) goto complete;
        e=0;
    }
    uint8_t *out=P2V(page);
    cvp_format fmt={.pitch=f.pitch,.width=(uint16_t)f.width,.height=(uint16_t)f.height,
        .bytes=f.bpp/8,.red_size=f.red_size,.red_pos=f.red_pos,.green_size=f.green_size,
        .green_pos=f.green_pos,.blue_size=f.blue_size,.blue_pos=f.blue_pos};
    unsigned w=s.width*n,h=s.height*n,ox=(f.width-w)/2,oy=(f.height-h)/2;
    for (unsigned y=0;y<f.height && !e;y++) for (unsigned x=0;x<f.width && !e;x+=256) {
        unsigned count=f.width-x;
        if (count>256) count=256;
        memset(out,0,count*fmt.bytes);
        unsigned l=x>ox ? x : ox,rgt=x+count<ox+w ? x+count : ox+w;
        kmutex_lock(&lease_lock);
        if (!valid(proc_find(pid),&a)) e=-EACCES;
        if (!e && y>=oy && y<oy+h && l<rgt) {
            unsigned first=(l-ox)/n,source_count=(rgt-ox+n-1)/n-first;
            struct ciuki_present_surface tile={source_count,1,source_count*4,source_count*4,s.format};
            const uint8_t *input=(const uint8_t *)pixels+(y-oy)/n*s.pitch+first*4;
            if (cvp_surface_row(&tile,input,palette,&gamma,&fmt,n,(l-ox)%n,rgt-l,
                out+(l-x)*fmt.bytes,PAGE_SIZE-(l-x)*fmt.bytes)) e=-EINVAL;
        }
        if (!e) e=fbdev_present_native(a.generation,x,y,out,count);
        kmutex_unlock(&lease_lock);
        kpreempt_point();
    }
complete:
    kmutex_lock(&lease_lock);
    if (!e && !valid(proc_find(pid),&a)) e=-EACCES;
    if (!e) {
        next_frame=cvp_present_deadline(g_ticks*UINT64_C(1000000));
        awaiting_first_frame=false;
    }
    frame_running=false;
    kmutex_unlock(&lease_lock);
    if (page) pmm_free(page);
    return e;
}
uint32_t w32_display_call(struct proc_thread *t, struct ciuki_w32_display_args *a)
{
    struct process *p=t->process;
    int e=0;
    if (a->action==W32_DISPLAY_GRANT || a->action==W32_DISPLAY_REVOKE) {
        if (p->pid!=supervisor_desktop_pid() || !grants_display_owner(p)) e=-EPERM;
        else if (a->action==W32_DISPLAY_GRANT && !w32_service_desktop_client(p,a->pid)) e=-EPERM;
        else if (a->action==W32_DISPLAY_GRANT) e=grant(p,a);
        else {
            e=a->token ? revoke(a->token,a->generation,a->pid) : -EACCES;
            if (e>0) e=0;
        }
    } else if (a->action==W32_DISPLAY_FRAME) e=frame(t,a);
    else if (a->action==W32_DISPLAY_PALETTE || a->action==W32_DISPLAY_GAMMA) {
        /* One page is larger than either LUT; copy user bytes outside the lock. */
        unsigned bytes=a->action==W32_DISPLAY_GAMMA ? sizeof(gamma) : a->count*4;
        if (a->action==W32_DISPLAY_PALETTE && (!a->count || a->first>=256 || a->count>256-a->first)) e=-EINVAL;
        kmutex_lock(&lease_lock);
        if (!e && !valid(p,a)) e=-EACCES;
        kmutex_unlock(&lease_lock);
        struct frame_operation op={.memory=p->memory};
        t->cleanup=frame_cleanup; t->operation=&op;
        if (!e) {
            e=ua_pin(p->memory,&op.pin,a->address,bytes,false);
            op.pinned=!e;
        }
        if (!e && !(op.page=pmm_alloc())) e=-ENOMEM;
        if (!e) e=copy_from_user(P2V(op.page),a->address,bytes);
        kmutex_lock(&lease_lock);
        if (!e && (!valid(p,a) || frame_running)) e=frame_running ? -EBUSY : -EACCES;
        if (!e) {
            if (a->action==W32_DISPLAY_GAMMA) memcpy(&gamma,P2V(op.page),sizeof(gamma));
            else cvp_palette_update(palette,a->first,a->count,P2V(op.page));
        }
        kmutex_unlock(&lease_lock);
        t->cleanup=0; t->operation=0; frame_cleanup(&op);
    } else e=-EINVAL;
    a->error=e<0 ? (uint32_t)-e : 0; a->result=!e;
    return 0;
}
