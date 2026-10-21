/* Frame adapter shared by V86 INT, extender reflection and 0300h. Guest
 * binding and allocation are negotiated by f3-50; an absent binding chains.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dosvm_internal.h"
#include "dos_file_hook.h"
#include "dos_file_volume.h"
#include "dosvm_network.h"
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
_Static_assert(DOS_HOOK_DISPATCH>=DPMI_RM_END,"DOS gates overlap DPMI real stubs");
_Static_assert(0xf0000+DOS_HOOK_DISPATCH>DPMI_PM_BASE+DPMI_PM_LIMIT,"DOS gates overlap DPMI PM stubs");
_Static_assert(DOS_HOOK_DISPATCH>DOSVM_MOUSE_RETURN+3,"DOS gates overlap mouse callback");
_Static_assert(DOS_HOOK_END<=0x2000,"DOS ROM exceeds protected F1 page");
void dosvm_file_rom_init(struct dosvm *v) { dos_file_hook_rom(P2V(v->frames[0xf1])); }
struct file_runtime {
    struct dos_file files;
    struct dos_file_volume volume;
    struct blkdev provider;
    struct dos_file_volume *removable;
    struct blkdev removable_provider;
    struct vfs_table native;
    bool native_ready;
};
static int removable_read(struct blkdev *dev,uint64_t lba,uint32_t count,void *data) {
    int e=blkdev_range(dev,lba,count);
    for (unsigned i=0;!e && i<count;i++)
        e=d48_sector(dev->ctx,D48_E_START+(uint32_t)lba+i,(uint8_t *)data+i*512,false);
    return e;
}
static int file_bind(struct dosvm *v,struct dos_file_frame *f,uint16_t caller_cs) {
    if (v->disable_file_bridge) return -FS_EACCES;
    if (v->files) return -FS_EINVAL;
    unsigned extra=v->conformance_media.mode ? sizeof(struct dos_file_volume) : 0;
    unsigned bytes=(sizeof(struct file_runtime)+extra+PAGE_SIZE-1)&~(PAGE_SIZE-1);
    if (v->info.resident+bytes>v->info.commitment) return -FS_ENOMEM;
    uint32_t phys=pmm_alloc_contig(bytes/PAGE_SIZE); if (!phys) return -FS_ENOMEM;
    mm_claim(phys,bytes/PAGE_SIZE,MM_VM_PRIVATE);
    struct file_runtime *runtime=P2V(phys); memset(runtime,0,bytes);
    if (extra) runtime->removable=(struct dos_file_volume *)(runtime+1);
    struct dos_file_ops ops; dosvm_file_ops_init(v,&ops);
    dos_file_init(&runtime->files,v->info.key.id,v->info.key.generation,&ops);
    dosvm_disk_provider_init(v,&runtime->provider);
    struct storage *storage=storage_get();
    int e=!storage || !storage->ready ? -FS_EIO :
        dos_file_volume_open(&runtime->volume,&storage->cache,&runtime->provider,v->overlay_open);
    if (!e) e=dos_file_map(&runtime->files,2,&runtime->volume.table,2,!v->overlay_open);
    if (!e && vfs_drive_present(&storage->vfs,2)) {
        e=vfs_table_init(&storage->vfs,&runtime->native,2);
        if (!e) { runtime->native_ready=true; e=dos_file_native_views(&runtime->files,&runtime->native,false); }
    }
    if (!e && v->conformance_media.mode) {
        runtime->removable_provider=(struct blkdev){.read=removable_read,.capacity=D48_VOLUME_SECTORS,
            .sector_size=512,.media_generation=v->conformance_media.generation,.logical=true,.ctx=&v->conformance_media};
        e=dos_file_volume_open(runtime->removable,&storage->cache,&runtime->removable_provider,false);
        if (!e) e=dos_file_map(&runtime->files,4,&runtime->removable->table,2,true);
    }
    if (!e) e=dos_file_hook_bind(&runtime->files,f,caller_cs);
    if (e) {
        dos_file_cancel(&runtime->files);
        if (runtime->native_ready) vfs_table_destroy(&runtime->native);
        if (runtime->removable) (void)dos_file_volume_close(runtime->removable);
        (void)dos_file_volume_close(&runtime->volume);
        for (unsigned i=0;i<bytes/PAGE_SIZE;i++) mm_free(phys+i*PAGE_SIZE,MM_VM_PRIVATE);
        return e;
    }
    v->files=&runtime->files; v->file_volume=&runtime->volume;
    v->file_phys=phys; v->file_bytes=bytes; v->info.resident+=bytes; return 0;
}
static int file_finish(struct dosvm *v,struct v86_frame *f,const struct dos_file_frame *r) {
    if (f->tf.cs==DOS_HOOK_ROM_SEG && f->tf.eip==DOS_HOOK_DISPATCH+2) {
        uint16_t off=(uint16_t)f->tf.user_esp;
        if (off>65526) return -EFAULT;
        uint16_t flags=r->flags;
        if (dosvm_write(v,vm_ptr(f->tf.user_ss,off+8),&flags,2)) return -EFAULT;
        uint8_t done=DOS_FILE_DONE; struct dos_file_far result=v->files->personality.result;
        if (dosvm_write(v,vm_ptr(result.seg,result.off),&done,1)) return -EFAULT;
    }
    pack(f,r); return 0;
}
int dosvm_file_interrupt(struct dosvm *v,struct v86_frame *f,uint8_t vector,unsigned origin) {
    if (v->conformance_media.mode && vector==0x21 && (f->tf.eax&0xff00)==0x3d00) {
        static const char path[]="E:\\D48FAULT.DAT";
        char input[sizeof(path)]; struct dos_file_frame r=unpack(f);
        bool matched=(uint16_t)f->tf.edx<=65536-sizeof(path) &&
            !dosvm_read(v,vm_ptr(f->ds,(uint16_t)f->tf.edx),input,sizeof(input)) && !memcmp(input,path,sizeof(path));
        d48_open_enter(&v->conformance_media,&r,matched);
    }
    if (v->disable_file_bridge) return -ENOSYS;
    if (vector==0x2f) {
        struct dos_file_frame r=unpack(f);
        uint8_t magic[8];
        bool ciuki=v->files || (!dosvm_read(v,vm_ptr(f->tf.cs,2),magic,8) && !memcmp(magic,"CIUKIDOS",8));
        if (ciuki && dos_file_hook_discover(&r)) { pack(f,&r); return 0; }
    }
    if (vector==0xfe && f->tf.cs==DOS_HOOK_ROM_SEG && f->tf.eip==DOS_HOOK_EVENT+2) {
        uint16_t ret[2]; unsigned sp=(uint16_t)f->tf.user_esp;
        if (sp>65532 || dosvm_read(v,vm_ptr(f->tf.user_ss,sp),ret,4)) return -EFAULT;
        struct dos_file_frame r=unpack(f);
        int e=r.ax==0 ? file_bind(v,&r,ret[1]) :
            v->files ? dos_file_hook_event(v->files,&r,ret[1]) : -FS_EINVAL;
        if (!e && (r.ax==DOS_FILE_PROCESS_EXIT || r.ax==DOS_FILE_EXEC_ROLLBACK))
            dosvm_network_process_exit(v,r.bx);
        struct dos_file_error error; dos_file_error_from_fs(&error,e,false);
        r.ax=e ? error.code : 0; r.flags=(uint16_t)((r.flags&~1u)|(e ? 1 : 0)); pack(f,&r); return 0;
    }
    if (!v->files) return -ENOSYS;
    if (vector==0xfe && v->files->critical && f->tf.cs==v->files->binding.critical_return.seg &&
        f->tf.eip==(uint32_t)v->files->binding.critical_return.off+2 &&
        f->tf.user_ss==v->files->callback.ss && (uint16_t)f->tf.user_esp==(uint16_t)(v->files->callback.sp+6)) {
        unsigned reply=f->tf.eax&255; struct dos_file_frame result;
        uint16_t owning=v->files->owning;
        enum dos_file_result completed=dos_file_complete(v->files,v->files->token,reply,&result);
        dosvm_file_dump(v);
        *f=v->file_saved;
        if (reply==2) {
            struct dos_file_far p=v->files->binding.selected_psp;
            if (dosvm_write(v,((uint32_t)p.seg<<4)+p.off,&owning,2)) return -EFAULT;
            vm_low(&f->tf.eax,0x4c00); return v86_reflect(&v->monitor,f,0x21);
        }
        if (completed!=DOS_FILE_DONE) return -FS_EIO;
        int e=file_finish(v,f,&result); v->monitor.vif=(result.flags&V86_IF)!=0; return e;
    }
    bool hook=vector==0xfe && f->tf.cs==DOS_HOOK_ROM_SEG && f->tf.eip==DOS_HOOK_DISPATCH+2 && v->files->personality.live;
    if (v->files->personality.live && !hook) {
        /* File calls are observed once at the hook; the real kernel,
         * console/EXEC and all multiplex calls retain their own traces. */
        if (vector!=0x21 && vector!=0x2f) return -ENOSYS;
        if (vector==0x21 && dos_file_serves(vector,(uint16_t)f->tf.eax)) return -ENOSYS;
    } else if (!hook && vector!=0x21 && vector!=0x2f) return -ENOSYS;
    if (!v->files->sequence && ((g_boot.flags&CBI_F_DIAG_VERBOSE) ||
        (v->probe && !strncmp(v->probe,"dos-conformance",sizeof("dos-conformance"))))) v->files->trace_enabled=true;
    if (!v->files->critical) v->file_saved=*f;
    struct dos_file_frame r=unpack(f); v->files->a20=v->a20;
    r.flags=(uint16_t)((r.flags&~V86_IF)|(v->monitor.vif ? V86_IF : 0));
    if (hook) {
        unsigned sp=(uint16_t)f->tf.user_esp; uint16_t stack[5];
        if (sp>65526 || dosvm_read(v,vm_ptr(f->tf.user_ss,sp),stack,sizeof(stack))) return -EFAULT;
        if (stack[1]!=v->files->personality.code_seg) return -V86_EPERM;
        r.flags=stack[4]; r.cs=stack[3]; r.ip=stack[2];
    }
    enum dos_file_result result=dos_file_dispatch(v->files,&r,hook ? DOS_FILE_HOOK : (enum dos_file_origin)origin,hook ? 0x21 : vector);
    dosvm_file_dump(v); /* drain before the next guest boundary, including chains */
    if (result==DOS_FILE_CHAIN) {
        if (!hook) return -ENOSYS;
        uint8_t chain=0; struct dos_file_far pointer=v->files->personality.result;
        return dosvm_write(v,vm_ptr(pointer.seg,pointer.off),&chain,1);
    }
    if (result==DOS_FILE_PENDING) {
        if (file_stopped(v)) return 0;
        if (!v->files->critical) return -FS_EIO;
        pack(f,&v->files->callback);
        f->tf.cs=v->files->callback.cs; f->tf.eip=v->files->callback.ip;
        vm_low(&f->tf.user_ss,v->files->callback.ss); vm_low(&f->tf.user_esp,v->files->callback.sp);
        f->tf.eflags=(f->tf.eflags&~(V86_TF|V86_CF))|V86_VM|V86_IF|2;
        v->monitor.vif=false; return 0;
    }
    return file_finish(v,f,&r);
}
int dosvm_file_reflect(struct dosvm *v,struct v86_frame *f,uint8_t vector,unsigned origin,uint16_t image) {
    if (!v->files || v->files->personality.live || (vector!=0x21 && vector!=0x2f)) return -ENOSYS;
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
    struct dos_file_frame r=unpack(f);
    d48_open_return(&v->conformance_media,&r,v->disable_file_bridge);
    if (!v->files) return;
    r.flags=(uint16_t)((r.flags&~V86_IF)|(v->monitor.vif ? V86_IF : 0));
    dos_file_observe_return(v->files,&r);
    dosvm_file_dump(v);
}
void dosvm_file_reflected_return(struct dosvm *v,const struct v86_frame *f,bool vif) {
    if (!v->files) return;
    struct dos_file_frame r=unpack(f); r.ip=(uint16_t)(r.ip-2);
    r.flags=(uint16_t)((r.flags&~V86_IF)|(vif ? V86_IF : 0));
    dos_file_observe_return(v->files,&r);
    dosvm_file_dump(v);
}
int dosvm_file_disk(struct dosvm *v,uint32_t lba,void *sector,bool write) {
    return v->file_volume ? dos_file_volume_raw(v->file_volume,lba,sector,write,0) : -ENOSYS;
}
int dosvm_file_destroy(struct dosvm *v) {
    if (v->files) {
        dos_file_cancel(v->files);
        dosvm_file_dump(v);
        unsigned pending=0;
        for (unsigned i=0;i<DOS_FILE_OBSERVERS;i++) if (v->files->observer[i].used) pending++;
        dosvm_file_trace_close(v,pending);
        v->info.cleanup_error=v->files->cleanup_status;
    }
    if (v->file_volume) {
        int e=dos_file_volume_close(v->file_volume);
        if (e) {
            if (!v->info.cleanup_error) v->info.cleanup_error=e;
            /* The worker is joined. A failed private image cannot pin the
             * VM forever; report its durability loss while retiring its
             * quiescent mount. Other clients/claims still force retention. */
            if (dos_file_volume_discard(v->file_volume)) return e;
        }
        v->file_volume=0;
    }
    if (v->file_phys) {
        struct file_runtime *runtime=(struct file_runtime *)v->files;
        int e=runtime->removable ? dos_file_volume_close(runtime->removable) : 0;
        if (e) return e;
        if (runtime->native_ready) vfs_table_destroy(&runtime->native);
        for (unsigned i=0;i<v->file_bytes/PAGE_SIZE;i++) mm_free(v->file_phys+i*PAGE_SIZE,MM_VM_PRIVATE);
        v->info.resident-=v->file_bytes; v->file_phys=v->file_bytes=0; v->files=0;
    }
    return 0;
}
void dosvm_file_deadline(struct dosvm *v) {
    if (v->files && v->files->critical && v->files->ops.wall(v->files->ops.arg)>=v->files->deadline)
        dosvm_stop(v->info.key,DOSVM_TIMEOUT);
}
