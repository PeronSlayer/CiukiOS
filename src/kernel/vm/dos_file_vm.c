/* Frame adapter shared by V86 INT, extender reflection and 0300h. Guest
 * binding and allocation are negotiated by f3-50; an absent binding chains.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dosvm_internal.h"
#include "dos_file.h"
#include "dos_file_volume.h"
#include <ciuki/probe.h>
#include <ciuki/timing.h>
static bool file_check(void *arg,uint32_t linear,size_t bytes,bool write) {
    /* Conventional/HMA frames are retained for the VM's entire lifetime;
     * destroy waits for its worker before releasing those existing pins. */
    return bytes<=UINT32_MAX && dosvm_range(arg,linear,(unsigned)bytes,write);
}
static void file_read(void *arg,uint32_t linear,void *buffer,size_t bytes) {
    struct dosvm *v=arg;
    if (dosvm_read(v,linear,buffer,(unsigned)bytes)) dosvm_stop(v->info.key,DOSVM_FAULT);
}
static void file_write(void *arg,uint32_t linear,const void *buffer,size_t bytes) {
    struct dosvm *v=arg;
    if (dosvm_write(v,linear,buffer,(unsigned)bytes)) dosvm_stop(v->info.key,DOSVM_FAULT);
}
static void file_release(void *arg) { (void)arg; }
static uint64_t file_wall(void *arg) {
    (void)arg; uint64_t cycles=dosvm_perf_clock();
    return g_tsc_per_ms ? (cycles/g_tsc_per_ms)*1000+(cycles%g_tsc_per_ms)*1000/g_tsc_per_ms : g_ticks*1000;
}
static void file_pause(void *arg) { (void)arg; task_sleep_ms(1); }
static bool file_critical(void *arg,uint64_t token,const struct dos_file_frame *frame,uint8_t flags,uint8_t cause) {
    struct dosvm *v=arg; (void)token; (void)frame; (void)flags; (void)cause;
    return v->info.state!=DOSVM_DEAD && v->info.state!=DOSVM_STOPPING;
}
static bool file_stopped(void *arg) {
    struct dosvm *v=arg; return v->info.state==DOSVM_STOPPING || v->info.state==DOSVM_DEAD;
}
void dosvm_file_ops_init(struct dosvm *v,struct dos_file_ops *ops) {
    *ops=(struct dos_file_ops){file_check,file_read,file_write,file_release,file_wall,dosvm_file_monitor_us,
        file_pause,file_critical,0,file_stopped,v};
}
static struct dos_file_frame unpack(const struct v86_frame *f) {
    return (struct dos_file_frame){(uint16_t)f->tf.eax,(uint16_t)f->tf.ebx,(uint16_t)f->tf.ecx,(uint16_t)f->tf.edx,
        (uint16_t)f->ds,(uint16_t)f->es,(uint16_t)f->tf.esi,(uint16_t)f->tf.edi,(uint16_t)f->tf.ebp,
        (uint16_t)f->tf.eflags,(uint16_t)f->tf.cs,(uint16_t)f->tf.eip,(uint16_t)f->tf.user_ss,(uint16_t)f->tf.user_esp};
}
static void pack(struct v86_frame *f,const struct dos_file_frame *r) {
    vm_low(&f->tf.eax,r->ax); vm_low(&f->tf.ebx,r->bx); vm_low(&f->tf.ecx,r->cx); vm_low(&f->tf.edx,r->dx);
    vm_low(&f->ds,r->ds); vm_low(&f->es,r->es); vm_low(&f->tf.esi,r->si); vm_low(&f->tf.edi,r->di); vm_low(&f->tf.ebp,r->bp);
    f->tf.eflags=(f->tf.eflags&~V86_CF)|(r->flags&V86_CF);
}
int dosvm_file_interrupt(struct dosvm *v,struct v86_frame *f,uint8_t vector,unsigned origin) {
    if (!v->files) return -ENOSYS;
    if (vector==0xfe && v->files->critical && f->tf.cs==v->files->binding.critical_return.seg &&
        f->tf.eip==(uint32_t)v->files->binding.critical_return.off+2 &&
        f->tf.user_ss==v->files->callback.ss && (uint16_t)f->tf.user_esp==(uint16_t)(v->files->callback.sp+6)) {
        unsigned reply=f->tf.eax&255; struct dos_file_frame result;
        uint16_t owning=v->files->owning;
        enum dos_file_result completed=dos_file_complete(v->files,v->files->token,reply,&result);
        *f=v->file_saved;
        if (reply==2) {
            struct dos_file_far p=v->files->binding.selected_psp;
            if (dosvm_write(v,((uint32_t)p.seg<<4)+p.off,&owning,2)) return -EFAULT;
            vm_low(&f->tf.eax,0x4c00); return v86_reflect(&v->monitor,f,0x21);
        }
        if (completed!=DOS_FILE_DONE) return -FS_EIO;
        pack(f,&result); v->monitor.vif=(result.flags&V86_IF)!=0; return 0;
    }
    if (vector!=0x21 && vector!=0x2f) return -ENOSYS;
    if (!v->files->sequence && (g_boot.flags&CBI_F_DIAG_VERBOSE)) v->files->trace_enabled=true;
    if (!v->files->critical) v->file_saved=*f;
    struct dos_file_frame r=unpack(f); v->files->a20=v->a20;
    r.flags=(uint16_t)((r.flags&~V86_IF)|(v->monitor.vif ? V86_IF : 0));
    enum dos_file_result result=dos_file_dispatch(v->files,&r,(enum dos_file_origin)origin,vector);
    if (result==DOS_FILE_CHAIN) return -ENOSYS;
    if (result==DOS_FILE_PENDING) {
        if (file_stopped(v)) return 0;
        if (!v->files->critical) return -FS_EIO;
        pack(f,&v->files->callback);
        f->tf.cs=v->files->callback.cs; f->tf.eip=v->files->callback.ip;
        vm_low(&f->tf.user_ss,v->files->callback.ss); vm_low(&f->tf.user_esp,v->files->callback.sp);
        f->tf.eflags=(f->tf.eflags&~(V86_TF|V86_CF))|V86_VM|V86_IF|2;
        v->monitor.vif=false; return 0;
    }
    pack(f,&r); return 0;
}
int dosvm_file_reflect(struct dosvm *v,struct v86_frame *f,uint8_t vector,unsigned origin,uint16_t image) {
    if (!v->files || (vector!=0x21 && vector!=0x2f)) return -ENOSYS;
    struct v86_frame returned=*f;
    returned.tf.cs=0xf000; returned.tf.eip=DPMI_RM_RETURN;
    vm_low(&returned.tf.user_esp,(uint16_t)returned.tf.user_esp+6);
    returned.tf.eflags=(returned.tf.eflags&~0x0cd5u)|(image&0x0cd5u)|V86_VM|V86_IF|2;
    v->monitor.vif=(image&V86_IF)!=0;
    int e=dosvm_file_interrupt(v,&returned,vector,origin);
    if (!e) *f=returned;
    else v->monitor.vif=false;
    return e;
}
void dosvm_file_return(struct dosvm *v,const struct v86_frame *f) {
    if (!v->files) return;
    struct dos_file_frame r=unpack(f);
    r.flags=(uint16_t)((r.flags&~V86_IF)|(v->monitor.vif ? V86_IF : 0));
    dos_file_observe_return(v->files,&r);
}
void dosvm_file_reflected_return(struct dosvm *v,const struct v86_frame *f,bool vif) {
    if (!v->files) return;
    struct dos_file_frame r=unpack(f); r.ip=(uint16_t)(r.ip-2);
    r.flags=(uint16_t)((r.flags&~V86_IF)|(vif ? V86_IF : 0));
    dos_file_observe_return(v->files,&r);
}
int dosvm_file_disk(struct dosvm *v,uint32_t lba,void *sector,bool write) {
    return v->file_volume ? dos_file_volume_raw(v->file_volume,lba,sector,write,0) : -ENOSYS;
}
int dosvm_file_destroy(struct dosvm *v) {
    if (v->files) dos_file_cancel(v->files);
    if (v->file_volume) {
        int e=dos_file_volume_close(v->file_volume); if (e) return e;
        v->file_volume=0;
    }
    return 0;
}
void dosvm_file_deadline(struct dosvm *v) {
    if (v->files && v->files->critical && v->files->ops.wall(v->files->ops.arg)>=v->files->deadline)
        dosvm_stop(v->info.key,DOSVM_TIMEOUT);
}
