/* Ring-3 F5 mechanism probes; uses f5-02's unchanged nested-SEH image.
 * The builtin bootstrap is test-only and never installed as system ntdll.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/w32_objects.h>
#include <ciuki/sha256.h>
#include <ciuki/signal.h>
__asm__(".pushsection .rodata.w32_fixture,\"a\"\n.balign 4\n"
        ".global w32_fixture_start,w32_fixture_end,w32_loader_start,w32_loader_end\n"
        "w32_fixture_start:\n.incbin \"" CIUKI_W32_EXE_BIN "\"\n"
        "w32_fixture_end:\nw32_loader_start:\n.incbin \"" CIUKI_W32_LOADER_BIN "\"\n"
        "w32_loader_end:\n.popsection\n");
extern const uint8_t w32_fixture_start[], w32_fixture_end[], w32_loader_start[], w32_loader_end[];
extern const uint8_t f2_payload_start[], f2_payload_end[];
struct core_result { uint32_t magic, done, errors, faults, memory, fs, sse, stack, threads, sse_fault, tcg_fallback; };
struct probe_image { const uint8_t *data; uint32_t bytes; };
static int core_read(void *cookie, uint32_t off, void *dst, uint32_t bytes)
{
    const struct probe_image *im = cookie;
    if (off > im->bytes || bytes > im->bytes - off) return -EIO;
    memcpy(dst, im->data + off, bytes); return 0;
}
static struct probe_image exe_image, loader_image, native_image;
static int core_open(void *cwd, const char *path, struct ciuki_file *out)
{
    (void)cwd;
    if (strncmp(path,"/System/Win32/ntdll.dll",sizeof("/System/Win32/ntdll.dll"))) return -ENOENT;
    *out = (struct ciuki_file){ .cookie=&loader_image, .bytes=loader_image.bytes, .read=core_read };
    return 0;
}
static int core_spawn(struct process *parent, struct probe_image *im, const char *mode, struct process **out)
{
    struct proc_strings *s = proc_strings_new();
    if (!s) return -ENOMEM;
    int err = proc_strings_add(s,"fixture",8,false);
    if (!err) err = proc_strings_add(s,mode,(uint32_t)strlen(mode)+1,false);
    struct ciuki_file f = { .cookie=im, .bytes=im->bytes, .read=core_read };
    if (!err) err = proc_spawn_file(parent,&f,s,0,0,0,0,out);
    proc_strings_free(s); return err;
}
static bool objects_evidence(const char *name)
{
    struct w32_account before,after;w32_snapshot(&before);
    struct process *p=0;struct proc_thread *t=0;
    bool pass=!proc_prepare(proc_supervisor(),&p);
    if(pass){p->personality=PROC_WIN32;p->memory->win32=true;pass=!w32_process_init(p);}
    if(pass)pass=!proc_thread_prepare(p,0x400000,0,0,0,CIUKI_THREAD_DETACHED,0,true,&t);
    uint32_t handles[2]={0};
    if(pass){
        p->state=PROC_LIVE;
        for(unsigned i=0;i<2;i++){
            struct ciuki_w32_object_args a={.action=W32_OBJ_CREATE,.type=W32_OBJ_EVENT,.value=i,.maximum=0};
            w32_object_call(t,&a);handles[i]=a.result;pass=pass&&!a.error;
        }
        pass=pass&&w32_wait_try(t,handles,2,true,0)==W32_WAIT_PENDING;
        struct ciuki_w32_object_args a={.action=W32_OBJ_SET,.handle=handles[0]};w32_object_call(t,&a);
        pass=pass&&!a.error&&w32_wait_try(t,handles,2,true,0)==0;
        pass=pass&&w32_wait_try(t,handles,2,false,0)==W32_WAIT_PENDING;
        struct ciuki_w32_section_args section={.kind=W32_SEC_PAGEFILE,.bytes=65536,.protect=W32_PAGE_READWRITE};
        w32_section_call(t,&section);pass=pass&&!section.error;
        struct ciuki_w32_view_args view={.action=W32_VIEW_MAP,.section=section.result,.bytes=4096,.access=W32_FILE_MAP_COPY};
        w32_view_call(t,&view);pass=pass&&!view.error;
        uint32_t value=0xfeed1234,read=1;
        if(pass)pass=!ua_read(p->memory,&read,view.address,4)&&!read&&!ua_write(p->memory,view.address,&value,4)&&!ua_read(p->memory,&read,view.address,4)&&read==value;
    }
    if(p)proc_discard(p);
    w32_snapshot(&after);bool clean=!memcmp(&before,&after,sizeof(before));
    rec_emit(name,"DATA","case=objects errors=%u atomic=%u cow=%u",pass?0:1,pass,pass);
    rec_emit(name,"DATA","case=teardown clean=%u",clean);
    return pass&&clean&&ledger_reconcile();
}
static int objects_probe(void)
{
    rec_emit("w32-objects","BEGIN",0);bool pass=objects_evidence("w32-objects");
    rec_emit("w32-objects","END",pass?"status=PASS":"status=FAIL reason=win32_objects");return pass?0:1;
}
static int run_core(const char *name, const char *mode)
{
    rec_emit(name,"BEGIN",0);
    struct proc_ledger before, after; proc_snapshot(&before);
    exe_image=(struct probe_image){w32_fixture_start,(uint32_t)(w32_fixture_end-w32_fixture_start)};
    loader_image=(struct probe_image){w32_loader_start,(uint32_t)(w32_loader_end-w32_loader_start)};
    native_image=(struct probe_image){f2_payload_start,(uint32_t)(f2_payload_end-f2_payload_start)};
    uint8_t digest[32];char hash[65];sha256(exe_image.data,exe_image.bytes,digest);sha256_hex(digest,hash);
    rec_emit(name,"DATA","case=fixture sha256=%s bytes=%u",hash,exe_image.bytes);
    struct process *parent=0,*p=0;bool pass=core_spawn(proc_supervisor(),&native_image,"p",&parent)>0;
    if (pass) {
        const struct ciuki_file_ops ops={core_open}, *old=proc_get_file_ops();
        proc_set_file_ops(&ops);
        pass=core_spawn(parent,&exe_image,mode,&p)>0;
        proc_set_file_ops(old);
    }
    struct core_result r={0};uint64_t deadline=g_ticks+15000;
    uint32_t pid=p?p->pid:0;
    while (pass && g_ticks<deadline && p->state==PROC_LIVE) {
        if (!ua_read(p->memory,&r,CIUKI_W32_CORE_BASE+CIUKI_W32_CORE_RESULT_RVA,sizeof(r)) && r.done) break;
        task_sleep_ms(1);
    }
    pass=pass && r.magic==0x57333243 && r.done && !r.errors && r.fs &&
         r.faults>=(mode[0]=='e'?5u:mode[0]=='m'?3u:7u) && (mode[0]=='e'||(r.memory&&r.stack)) &&
         r.sse==(uint32_t)(g_cpu_fxsr&&g_cpu_sse) && r.threads==r.sse;
    rec_emit(name,"DATA","case=personality pid=%u fs=%08x errors=%u sse=%u threads=%u",pid,r.fs,r.errors,r.sse,r.threads);
    rec_emit(name,"DATA","case=exceptions faults=%u done=%u errors=%u sse_fault=%u tcg_fallback=%u hardware_required_vector=19",r.faults,r.done,r.errors,r.sse_fault,r.tcg_fallback);
    rec_emit(name,"DATA","case=memory checked=%u errors=%u stack=%u",r.memory,r.errors,r.stack);
    /* While the Win32 process is still live, a native ELF must still get #UD
     * for SSE: CR4.OSFXSR follows the personality of the dispatched task. */
    struct process *native=0; uint32_t native_vector=0; int32_t native_status=-1;
    if (parent && core_spawn(parent,&native_image,"x",&native)>0) {
        deadline=g_ticks+5000;
        while(native->state!=PROC_ZOMBIE&&g_ticks<deadline)task_sleep_ms(1);
        if(native->state==PROC_ZOMBIE){native_vector=native->fault_vector;native_status=native->status;proc_reap(parent,native);}
        else {proc_stop(native,99,0);task_sleep_ms(2);if(native->state==PROC_ZOMBIE)proc_reap(parent,native);}
    }
    bool native_ud=native_vector==6&&native_status==SIGILL;
    pass=pass&&native_ud;
    rec_emit(name,"DATA","case=native_sse vector=%u status=%d ud=%u",native_vector,native_status,native_ud);
    if(p){
        if(p->state==PROC_LIVE){uint32_t one=1;ua_write(p->memory,CIUKI_W32_CORE_BASE+CIUKI_W32_CORE_RELEASE_RVA,&one,4);}
        deadline=g_ticks+1000;
        while(p->state!=PROC_ZOMBIE&&g_ticks<deadline)task_sleep_ms(1);
        if(p->state!=PROC_ZOMBIE){pass=false;proc_stop(p,99,0);task_sleep_ms(2);}
        if(p->state==PROC_ZOMBIE){pass=pass&&p->raw_exit==0;proc_reap(parent,p);}
    }
    if(parent){proc_stop(parent,0,0);task_sleep_ms(2);proc_collect();}
    proc_snapshot(&after);
    bool clean=before.processes==after.processes&&before.threads==after.threads&&before.backing==after.backing&&before.extents==after.extents&&before.tables==after.tables;
    pass=pass&&clean;
    rec_emit(name,"DATA","case=teardown clean=%u",clean);
    if(mode[0]=='a')pass=objects_evidence(name)&&pass;
    rec_emit(name,"END",pass?"status=PASS":"status=FAIL reason=win32_core");
    return pass?0:1;
}
static int core_probe(void){return run_core("f5-core","all");}
static int exception_probe(void){return run_core("w32-exceptions","exceptions");}
static int memory_probe(void){return run_core("w32-memory","memory");}
CIUKI_F5_PROBE("f5-core",core_probe);
CIUKI_F5_PROBE("w32-exceptions",exception_probe);
CIUKI_F5_PROBE("w32-memory",memory_probe);

CIUKI_F5_PROBE("w32-objects",objects_probe);

/* Production loader vertical slice. The complete 137-image structural corpus
 * is a host-model gate; synthetic callback bytes are never executed here. */
static int fixtures_probe(void)
{
    const char *name="w32-fixtures";
    rec_emit(name,"BEGIN",0);
    struct proc_ledger before,after;proc_snapshot(&before);
    const struct ciuki_file_ops *ops=proc_get_file_ops();
    const char *files[]={"seh-chain.exe","seh-nested.exe","compiled-tls.exe","consumer.exe","packed-code.exe"};
    const uint32_t expected[]={0,0,0,18,42};unsigned errors=0,completed=0;
    struct process *parent=proc_current();if(!parent)parent=proc_supervisor();
    for(unsigned i=0;i<ARRAY_SIZE(files);i++){
        char path[128];ksnprintf(path,sizeof(path),"/System/Win32/Fixtures/%s",files[i]);
        struct ciuki_file file={0};struct proc_strings *strings=proc_strings_new();struct process *child=0;
        int err=(!ops||!strings)?-ENOENT:ops->open(parent->cwd,path,&file);
        if(!err)err=proc_strings_add(strings,path,(uint32_t)strlen(path)+1,false);
        if(!err)err=proc_spawn_file(parent,&file,strings,0,0,0,0,&child);
        if(file.close)file.close(file.cookie);proc_strings_free(strings);
        uint64_t deadline=g_ticks+10000;
        if(err<0){errors++;continue;}
        while(child->state!=PROC_ZOMBIE&&g_ticks<deadline)task_sleep_ms(1);
        if(child->state!=PROC_ZOMBIE){errors++;proc_stop(child,99,0);task_sleep_ms(2);}
        if(child->state==PROC_ZOMBIE){if((uint32_t)child->raw_exit!=expected[i])errors++;completed++;proc_reap(parent,child);}
    }
    proc_snapshot(&after);
    bool clean=before.processes==after.processes&&before.threads==after.threads&&before.backing==after.backing&&before.extents==after.extents&&before.tables==after.tables;
    rec_emit(name,"DATA","case=fixtures completed=%u errors=%u",completed,errors);
    rec_emit(name,"DATA","case=teardown clean=%u",clean);
    bool pass=completed==5&&!errors&&clean;
    rec_emit(name,"END",pass?"status=PASS":"status=FAIL reason=win32_fixtures");return pass?0:1;
}
CIUKI_F5_PROBE("w32-fixtures",fixtures_probe);
