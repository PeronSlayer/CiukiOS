/* Frame adapter shared by V86 INT, extender reflection and 0300h. Guest
 * binding and allocation are negotiated by f3-50; an absent binding chains.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dosvm_internal.h"
#include "dos_file_hook.h"
#include "dos_unified.h"
#include "dos_unified_ifs.h"
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
static void file_process_exit(void *arg,uint16_t psp) { dosvm_network_process_exit(arg,psp); }
void dosvm_file_ops_init(struct dosvm *v,struct dos_file_ops *ops) {
    *ops=(struct dos_file_ops){file_check,file_read,file_write,file_release,file_wall,dosvm_file_monitor_us,
        file_pause,file_critical,0,file_stopped,v,file_process_exit};
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
_Static_assert(DFV2_DIRECT>=DOS_HOOK_END,"DFV2 gates overlap the v1 ROM");
_Static_assert(DFV2_END<=0x2000,"DFV2 ROM exceeds protected F1 page");
_Static_assert(DFV2_DIRECT>DOSVM_MOUSE_RETURN+3 && DFV2_DIRECT>=DPMI_RM_END,"DFV2 gates overlap mouse/DPMI stubs");
_Static_assert(0xf0000+DFV2_DIRECT>DPMI_PM_BASE+DPMI_PM_LIMIT,"DFV2 gates overlap DPMI PM stubs");
/* v1 is written for every VM exactly as before; DFV2 only for the unified
 * profile (kernel=unified, schema value 2), into the unused 1C00-1C3F. */
void dosvm_file_rom_init(struct dosvm *v) {
    dos_file_hook_rom(P2V(v->frames[0xf1]));
    if (v->kernel==2) dos_unified_rom(P2V(v->frames[0xf1]),v->info.key.generation);
}
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
/* f3-56/f3-57 DFV2 (private, unified profile only). The state is allocated
 * at the first event-gate call and charged to the VM; an allocation failure
 * is a refused gate call, which CIUKIDF.SYS reports as a contained INIT
 * failure. The backend is the f3-57 live IFS (dos_unified_ifs.c): the final
 * bind mounts this VM's logical C: view through f3-47 (the same provider and
 * generation as BIOS INT 13h, which then serializes through it), maps the
 * read-only system D: and the policy-controlled E:, and converts their CDS
 * entries to IFS drives. */
struct unified_runtime {
    struct dos_unified unified;
    struct dfv2_ifs ifs;
    struct dos_file_volume volume,removable;
    struct blkdev provider,removable_provider;
    struct vfs_table native;
    bool volume_open,removable_open,native_ready,active_logged;
};
static void unified_unmount(void *arg,struct dfv2_ifs *s) {
    struct dosvm *v=arg; struct unified_runtime *r=(struct unified_runtime *)v->unified; (void)s;
    if (v->file_volume==&r->volume) v->file_volume=0;
    if (r->native_ready) { vfs_table_destroy(&r->native); r->native_ready=false; }
    if (r->removable_open) { (void)dos_file_volume_close(&r->removable); r->removable_open=false; }
    if (r->volume_open) {
        int e=dos_file_volume_close(&r->volume);
        if (e) {
            if (!v->info.cleanup_error) v->info.cleanup_error=e;
            (void)dos_file_volume_discard(&r->volume);
        }
        r->volume_open=false;
    }
}
static int unified_mount(void *arg,struct dfv2_ifs *s) {
    struct dosvm *v=arg; struct unified_runtime *r=(struct unified_runtime *)v->unified;
    struct storage *storage=storage_get();
    if (!storage || !storage->ready || v->file_volume) return -FS_EIO;
    dosvm_disk_provider_init(v,&r->provider);
    int e=dos_file_volume_open(&r->volume,&storage->cache,&r->provider,v->overlay_open);
    if (!e) { r->volume_open=true; e=dfv2_ifs_map(s,2,&r->volume.table,2,!v->overlay_open); }
    if (!e && vfs_drive_present(&storage->vfs,2)) {
        e=vfs_table_init(&storage->vfs,&r->native,2);
        if (!e) { r->native_ready=true; e=dfv2_ifs_map(s,3,&r->native,2,true); }
    }
    if (!e && v->conformance_media.mode) {
        r->removable_provider=(struct blkdev){.read=removable_read,.capacity=D48_VOLUME_SECTORS,
            .sector_size=512,.media_generation=v->conformance_media.generation,.logical=true,.ctx=&v->conformance_media};
        e=dos_file_volume_open(&r->removable,&storage->cache,&r->removable_provider,false);
        if (!e) { r->removable_open=true; e=dfv2_ifs_map(s,4,&r->removable.table,2,true); }
    } else if (!e && r->native_ready) {
        for (unsigned i=3;i<26;i++)
            if (vfs_drive_present(&storage->vfs,i) && (storage->vfs.drive_flags[i]&VFS_DRIVE_REMOVABLE)) {
                e=dfv2_ifs_map(s,4,&r->native,i,true); break;   /* policy: not writable */
            }
    }
    if (e) { unified_unmount(v,s); return e; }
    v->file_volume=&r->volume;
    return 0;
}
static int unified_create(struct dosvm *v) {
    unsigned bytes=(sizeof(struct unified_runtime)+PAGE_SIZE-1)&~(PAGE_SIZE-1);
    if (v->info.resident+bytes>v->info.commitment) return -FS_ENOMEM;
    uint32_t phys=pmm_alloc_contig(bytes/PAGE_SIZE); if (!phys) return -FS_ENOMEM;
    mm_claim(phys,bytes/PAGE_SIZE,MM_VM_PRIVATE);
    struct unified_runtime *r=P2V(phys); memset(r,0,bytes);
    struct dos_file_ops ops; dosvm_file_ops_init(v,&ops);
    struct dfv2_ifs_host host={unified_mount,unified_unmount,v};
    struct dfv2_backend backend;
    dfv2_ifs_init(&r->ifs,&r->unified,&ops,&host,v->info.key.id,v->info.key.generation);
    dfv2_ifs_backend(&r->ifs,&backend);
    dos_unified_init(&r->unified,v->info.key.generation,&ops,&backend);
    v->unified=&r->unified; v->unified_phys=phys; v->unified_bytes=bytes; v->info.resident+=bytes;
    return 0;
}
static void unified_release(struct dosvm *v) {
    if (!v->unified) return;
    dos_unified_destroy(v->unified);       /* closes descriptions, then unmounts */
    for (unsigned i=0;i<v->unified_bytes/PAGE_SIZE;i++) mm_free(v->unified_phys+i*PAGE_SIZE,MM_VM_PRIVATE);
    v->info.resident-=v->unified_bytes; v->unified=0; v->unified_phys=v->unified_bytes=0;
}
static int unified_interrupt(struct dosvm *v,struct v86_frame *f,uint8_t vector) {
    struct dos_file_frame r=unpack(f);
    if (vector==0x2f) {
        if (!dos_unified_discover(&r)) return -ENOSYS;
        pack(f,&r); return 0;
    }
    if (vector!=0xfe || f->tf.cs!=DFV2_ROM_SEG) return -ENOSYS;
    uint32_t ip=f->tf.eip;
    if (ip==DFV2_CRITICAL+2) {
        /* Monitor-only PENDING ends here exactly once; anything else stops
         * the VM (contained, no host panic). */
        if (!v->unified || dos_unified_critical(v->unified,&r,f->tf.eax&255)!=DFV2_RESUME) {
            dosvm_stop(v->info.key,DOSVM_FAULT); return 0;
        }
        *f=v->unified_saved; v->monitor.vif=v->unified_vif; return 0;
    }
    if (ip!=DFV2_DIRECT+2 && ip!=DFV2_LOCAL+2 && ip!=DFV2_EVENT+2) return -ENOSYS;
    if (!v->unified && (ip!=DFV2_EVENT+2 || unified_create(v))) return 0;
    enum dfv2_outcome outcome=dos_unified_gate(v->unified,&r);
    if (v->unified->bind_refusal && !v->unified_logged) {
        /* One line per VM: the final bind is one-shot (DOS=LOW is required).
         * f3-57 Amendment 1: a refused activation stops the VM here, at the
         * lifecycle gate, unless CIUKIDF.SYS was loaded with /LOCAL. */
        struct unified_runtime *r=(struct unified_runtime *)v->unified;
        char token[64];
        v->unified_logged=true;
        if (dos_unified_activation_stop(v->unified,r->ifs.why,token,sizeof(token))) {
            klog("[dosvm] unified_activation_refused reason=%s",token);
            dosvm_stop(v->info.key,DOSVM_FAULT); return 0;
        }
        klog("[dosvm] DFV2 bind refused vm=%u generation=%u reason=%s ifs=%s",
             v->info.key.id,v->info.key.generation,v->unified->bind_refusal,r->ifs.why ? r->ifs.why : "-");
    }
    {
        struct unified_runtime *r=(struct unified_runtime *)v->unified;
        if (r->ifs.active && !r->active_logged) {
            klog("[dosvm] DFV2 IFS active vm=%u generation=%u drives=%x",
                 v->info.key.id,v->info.key.generation,r->ifs.owned);
            r->active_logged=true;
        }
    }
    if (outcome!=DFV2_SUSPEND) return 0;
    /* INT 24h callback on the driver stack; IRET returns to the C2 gate. */
    uint16_t ss,sp,vector24[2];
    if (!dos_unified_callback_stack(v->unified,&ss,&sp) || dosvm_read(v,0x24*4,vector24,4)) {
        dosvm_stop(v->info.key,DOSVM_FAULT); return 0;
    }
    uint16_t frame[3]={DFV2_CRITICAL,DFV2_ROM_SEG,
        (uint16_t)((f->tf.eflags&~V86_IF)|(v->monitor.vif ? V86_IF : 0))};
    sp=(uint16_t)(sp-6);
    if (dosvm_write(v,vm_ptr(ss,sp),frame,6)) { dosvm_stop(v->info.key,DOSVM_FAULT); return 0; }
    v->unified_saved=*f; v->unified_vif=v->monitor.vif;
    const struct dfv2_critical *c=&v->unified->critical;
    vm_low(&f->tf.eax,c->ax); vm_low(&f->tf.edi,c->di); vm_low(&f->tf.ebp,c->bp); vm_low(&f->tf.esi,c->si);
    vm_low(&f->tf.user_ss,ss); vm_low(&f->tf.user_esp,sp);
    f->tf.cs=vector24[1]; f->tf.eip=vector24[0]; f->tf.eflags&=~V86_TF;
    v->monitor.vif=false;
    return 0;
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
    if (v->kernel==2) {
        int unified=unified_interrupt(v,f,vector);
        if (unified!=-ENOSYS) return unified;
    }
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
    unified_release(v);
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
