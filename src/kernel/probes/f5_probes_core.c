/* Ring-3 F5 mechanism probes; uses f5-02's unchanged nested-SEH image.
 * The builtin bootstrap is test-only and never installed as system ntdll.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/win32.h>
#include <ciuki/sha256.h>
#include <ciuki/signal.h>
__asm__(".pushsection .rodata.w32_fixture,\"a\"\n.balign 4\n"
        ".global w32_fixture_start,w32_fixture_end,w32_loader_start,w32_loader_end\n"
        "w32_fixture_start:\n.incbin \"" CIUKI_W32_EXE_BIN "\"\n"
        "w32_fixture_end:\nw32_loader_start:\n.incbin \"" CIUKI_W32_LOADER_BIN "\"\n"
        "w32_loader_end:\n.popsection\n");
extern const uint8_t w32_fixture_start[], w32_fixture_end[], w32_loader_start[], w32_loader_end[];
extern const uint8_t f2_payload_start[], f2_payload_end[];
struct core_result { uint32_t magic, done, errors, faults, memory, fs, sse, stack, threads; };
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
    rec_emit(name,"DATA","case=exceptions faults=%u done=%u errors=%u",r.faults,r.done,r.errors);
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
    rec_emit(name,"END",pass?"status=PASS":"status=FAIL reason=win32_core");
    return pass?0:1;
}
static int core_probe(void){return run_core("f5-core","all");}
static int exception_probe(void){return run_core("w32-exceptions","exceptions");}
static int memory_probe(void){return run_core("w32-memory","memory");}
CIUKI_F5_PROBE("f5-core",core_probe);
CIUKI_F5_PROBE("w32-exceptions",exception_probe);
CIUKI_F5_PROBE("w32-memory",memory_probe);
