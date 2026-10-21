/* f3-53: selected private DOS title using the vm-window/workload services.
 * No guest marker can manufacture a checkpoint or PASS.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/storage.h>
#include <ciuki/sha256.h>
#include <ciuki/audio_vm.h>
#include <ciuki/dpmi.h>
#include <ciuki/abi.h>
#include "selector.h"
#include "dos_title_model.h"
#include "../vm/dosvm_internal.h"
#include "../vm/dos_file_volume.h"

static struct d53_plan title_plan;
static uint8_t title_wire[D53_V2_BYTES];
static unsigned title_wire_size;
static struct vfs_table title_table;
static const char *const title_checks[]={"boot","menu","gameplay"};

static bool title_prompt(struct dosvm *v)
{
    const uint16_t *text=dosvm_text(v,0);
    for (unsigned i=0;i<1997;i++) if ((uint8_t)text[i]=='C' && (uint8_t)text[i+1]==':' &&
        (uint8_t)text[i+2]=='\\') {
        for (unsigned j=i+3;j<(i/80+1)*80;j++) if ((uint8_t)text[j]=='>') return true;
    }
    return false;
}
static int title_type(struct dosvm *v,const char *s)
{
    uint64_t until=deadline_after_ms(10000);
    for (unsigned i=0;;i++) {
        while (!dosvm_keyboard(dosvm_key(v),s[i] ? (uint8_t)s[i] : 0x1c0d)) {
            if (deadline_passed(until) || !dosvm_valid(dosvm_key(v))) return -V86_ETIMEDOUT;
            task_sleep_ms(1);
        }
        if (!s[i]) return 0;
    }
}
static bool title_alive(struct dosvm *v)
{
    struct dosvm_info info;
    dosvm_info(v,&info);
    return info.state<DOSVM_STOPPING && !info.dropped_records && !info.verdict_done;
}
static int title_keys(struct dosvm *v,unsigned phase,uint64_t since,unsigned *next)
{
    while (*next<title_plan.count) {
        const struct d53_input *e=&title_plan.input[*next];
        if (e->phase!=phase || e->at>g_ticks-since) break;
        struct ciuki_input_event key={.type=CIUKI_INPUT_KEY,.code=(int32_t)(e->edge&127),.value=(e->edge&256)!=0};
        if (!dosvm_input_event(dosvm_key(v),&key)) return -EIO;
        (*next)++;
    }
    return 0;
}
static void title_audio(struct dosvm *v,struct d53_audio *out)
{
    struct audio_vm_diagnostic d={0};
    audio_vm_diagnostic(dosvm_key(v),&d);
    struct dosvm_audio_info a={0};
    dosvm_audio_info(dosvm_key(v),&a);
    uint32_t flags=irq_save();
    *out=(struct d53_audio){.underruns=d.starved,.pcm_frames=a.frames,
        .opl_writes=v->audio.opl_writes,.blocks=a.boundaries,
        .late_acks=v->audio.cadence_late_acks,.irq_entries=v->audio.cadence_irq_entries,
        .irq_latency_max_us=v->audio.cadence_irq_latency_max_us,.irq=a.irq,
        .attached=d.attached,.ready=d.ready && !d.error};
    irq_restore(flags);
}
static bool title_same(const struct dosvm_ledger *a,const struct dosvm_ledger *b)
{
    return a->vms==b->vms && a->tasks==b->tasks && a->pages==b->pages &&
        a->handles==b->handles && a->devices==b->devices && a->reserved==b->reserved &&
        a->resident==b->resident && a->committed==b->committed && a->swapped==b->swapped &&
        a->pinned==b->pinned && a->cache==b->cache && a->overlay_rows==b->overlay_rows;
}
/* Bounded enumeration of the actual private installed tree, including hidden
 * files. Sort relative 8.3 paths; hash names, LE32 sizes and content digests.
 * Use the existing VFS/mount rather than a second FAT reader. */
static struct { char path[101]; uint32_t size; bool dir; } title_tree[128];
static uint8_t title_buffer[4096];
static __attribute__((noinline)) int title_tree_hash(struct dosvm *v,const char *target,char hex[65],unsigned *files,uint64_t until)
{
    if (!v->file_volume) return -EIO;
    struct vfs_table *table=&v->file_volume->table;
    memset(title_tree,0,sizeof(title_tree)); title_tree[0].dir=true;
    unsigned count=1; *files=0;
    for (unsigned i=0;i<count;i++) if (title_tree[i].dir) {
        char path[128];
        ksnprintf(path,sizeof(path),"C:\\%s%s%s\\*",target,title_tree[i].path[0] ? "\\" : "",title_tree[i].path);
        struct vfs_find find={0}; struct fat_entry entry;
        int e=vfs_find_first(table,path,0x37,&find,&entry);
        while (!e) {
            if (deadline_passed(until)) return -V86_ETIMEDOUT;
            if (strncmp(entry.name,".",sizeof(entry.name)) && strncmp(entry.name,"..",sizeof(entry.name)) && !(entry.attr&FAT_ATTR_VOLUME)) {
                if (count==ARRAY_SIZE(title_tree)) return -EINVAL;
                unsigned n=strlen(entry.name);
                if (!n || n>12 || strlen(title_tree[i].path)+n+1>100) return -EINVAL;
                ksnprintf(title_tree[count].path,sizeof(title_tree[count].path),"%s%s%s",title_tree[i].path,title_tree[i].path[0] ? "/" : "",entry.name);
                title_tree[count].dir=(entry.attr&FAT_ATTR_DIR)!=0;
                title_tree[count++].size=entry.size;
            }
            e=vfs_find_next(table,&find,&entry);
        }
        if (e!=-FS_ENOENT) return e;
    }
    struct sha256_ctx tree; sha256_init(&tree);
    uint32_t total=0;
    /* Lexicographic selection avoids a second path array/large stack frame. */
    char last[101]={0};
    for (;;) {
        unsigned best=count;
        for (unsigned i=1;i<count;i++) if (!title_tree[i].dir && strncmp(title_tree[i].path,last,101)>0 &&
            (best==count || strncmp(title_tree[i].path,title_tree[best].path,101)<0)) best=i;
        if (best==count) break;
        const char *relative=title_tree[best].path;
        char path[128]; ksnprintf(path,sizeof(path),"C:\\%s\\%s",target,relative);
        for (unsigned j=0;path[j];j++) if (path[j]=='/') path[j]='\\';
        uint32_t size=title_tree[best].size;
        if (size>96u*1024u*1024u-total) return -EINVAL;
        total+=size;
        int fd=vfs_open(table,path,VFS_READ,VFS_DENY_WRITE,0);
        if (fd<0) return fd;
        struct sha256_ctx content; sha256_init(&content);
        int error=0;
        for (uint32_t at=0;!error && at<size;) {
            if (deadline_passed(until)) { error=-V86_ETIMEDOUT; break; }
            size_t got=0; unsigned n=size-at<sizeof(title_buffer) ? size-at : sizeof(title_buffer);
            error=vfs_read(table,fd,title_buffer,n,&got);
            if (!error && got!=n) error=-EIO;
            if (!error) { sha256_update(&content,title_buffer,n); at+=n; }
        }
        int closed=vfs_close(table,fd);
        if (error || closed) return error ? error : closed;
        uint8_t digest[32],le[4]={size,size>>8,size>>16,size>>24};
        sha256_final(&content,digest);
        sha256_update(&tree,relative,strlen(relative)+1); sha256_update(&tree,le,4); sha256_update(&tree,digest,32);
        memcpy(last,relative,strlen(relative)+1); (*files)++;
    }
    if (!*files) return -EINVAL;
    uint8_t digest[32]; sha256_final(&tree,digest); sha256_hex(digest,hex);
    return 0;
}
static __attribute__((noinline)) int title_install(struct dosvm *v,const char *id)
{
    for (unsigned i=0;i<title_plan.stages;i++) {
        const struct d53_stage *s=&title_plan.stage[i];
        dosvm_expect(v,"RUNREC");
        uint64_t started=g_ticks, until=deadline_after_ms(s->timeout);
        int error=title_type(v,s->command);
        unsigned next=0;
        struct dosvm_info info={0};
        while (!error) {
            dosvm_info(v,&info);
            if (info.state>=DOSVM_STOPPING || info.dropped_records) { error=-EIO; break; }
            if (deadline_passed(until)) { error=-V86_ETIMEDOUT; break; }
            while (next<s->count && s->input[next].at<=g_ticks-started) {
                struct ciuki_input_event key={.type=CIUKI_INPUT_KEY,.code=(int32_t)(s->input[next].edge&127),.value=(s->input[next].edge&256)!=0};
                if (!dosvm_input_event(dosvm_key(v),&key)) { error=-EIO; break; }
                next++;
            }
            if (info.verdict_done && (!info.verdict_pass || (next==s->count && title_prompt(v)))) break;
            if (!error) task_sleep_ms(10);
        }
        if (!error && (!info.verdict_done || !info.verdict_pass || next!=s->count)) error=-EIO;
        char tree[65]={0}; unsigned files=0;
        if (!error) error=title_tree_hash(v,s->target,tree,&files,until);
        if (!error && s->tree[0] && strncmp(s->tree,tree,65)) error=-EIO;
        rec_emit("title","DATA","group=dos_title title=%s case=install stage=%u ms=%u delivered=%u rc=%u files=%u tree_sha256=%s",id,i,(uint32_t)(g_ticks-started),next,error ? 1u : 0u,files,tree[0] ? tree : "unavailable");
        if (error) return error;
    }
    return 0;
}
static int title_run(void)
{
    struct probe_selection selected;
    if (!probes_parse_selector(g_boot.test_request,g_boot.test_request_len,g_boot.flags,&selected)) return 1;
    const char *id=selected.title;
    rec_emit("title","BEGIN","title=%s version=1",id);
    char base[24]={0}, path[24]={0};
    int error=dosvm_workload_base(base);
    struct storage *s=storage_get();
    if (!error && (!s || !s->ready)) error=-EIO;
    if (!error) error=storage_enable_write(s,2);
    if (!error) error=vfs_table_init(&s->vfs,&title_table,2);
    if (!error) {
        ksnprintf(path,sizeof(path),"%c:\\DTITLE.PLN",base[0]);
        int fd=vfs_open(&title_table,path,VFS_READ,VFS_DENY_WRITE,0);
        size_t got=0;
        if (fd<0) error=fd;
        else {
            struct fat_entry entry;
            error=vfs_stat(&title_table,path,&entry);
            if (!error && entry.size!=D53_PLAN_BYTES && entry.size!=D53_V2_BYTES) error=-EINVAL;
            if (!error) { title_wire_size=entry.size; error=vfs_read(&title_table,fd,title_wire,title_wire_size,&got); }
            int closed=vfs_close(&title_table,fd);
            if (!error && (closed || got!=title_wire_size)) error=-EIO;
        }
        vfs_table_destroy(&title_table);
    }
    if (!error && (!d53_parse(title_wire,title_wire_size,&title_plan) || strncmp(id,title_plan.title,24))) error=-EINVAL;
    if (error) {
        rec_emit("title","END","status=FAIL reason=title_fixture_or_plan error=%d",error);
        return 1;
    }
    uint8_t plan_hash[32]; char plan_hex[65];
    sha256(title_wire,title_wire_size,plan_hash); sha256_hex(plan_hash,plan_hex);
    rec_emit("title","DATA","group=dos_title title=%s case=plan sha256=%s",id,plan_hex);
    if (title_plan.extended) rec_emit("title","DATA","group=dos_title title=%s case=mode calibration=%u",id,title_plan.calibration);
    struct dosvm_ledger before,after;
    dosvm_snapshot(&before);
    struct dosvm *v=0;
    struct dosvm_config cfg={.probe="title",.base=base,.dpmi=true};
    uint64_t started=g_ticks, launched=0, playing=0;
    error=dosvm_create(&cfg,&v);
    if (!error) {
        /* Per-battery wiring: Creative mixer 80h selects IRQ, 81h DMA 1/5. */
        uint32_t port=0x80, value=title_plan.irq==5 ? 2u : 4u;
        error=dosvm_audio_port(v,0x224,1,true,&port,0);
        if (!error) error=dosvm_audio_port(v,0x225,1,true,&value,0);
        port=0x81; value=0x22;
        if (!error) error=dosvm_audio_port(v,0x224,1,true,&port,0);
        if (!error) error=dosvm_audio_port(v,0x225,1,true,&value,0);
    }
    if (!error) error=dosvm_boot(v);
    uint64_t until=deadline_after_ms(30000);
    while (!error) {
        struct dosvm_info info; dosvm_info(v,&info);
        if (info.state>=DOSVM_STOPPING) { error=-EIO; break; }
        if (info.dos_ready && title_prompt(v)) break;
        if (deadline_passed(until)) { error=-V86_ETIMEDOUT; break; }
        task_sleep_ms(10);
    }
    if (!error && !dosvm_input_focus(dosvm_key(v),true)) error=-EIO;
    if (!error) {
        error=title_type(v,title_plan.irq==5 ? "SET BLASTER=A220 I5 D1 H5 T6" : "SET BLASTER=A220 I7 D1 H5 T6");
        if (!error) task_sleep_ms(100);
    }
    if (!error) error=title_install(v,id);
    if (!error) {
        dosvm_expect(v,"RUNREC");
        launched=g_ticks;
        error=title_type(v,title_plan.command);
        struct dosvm_key key=dosvm_key(v);
        rec_emit("title","DATA","group=dos_title title=%s case=start vm=%u generation=%u launch_ms=%u",id,key.id,key.generation,(uint32_t)(launched-started));
    }
    unsigned next=0, checkpoint=0, changes=0;
    struct dosvm_frame frame={0}; char digest[65]={0}; uint8_t previous[32]={0};
    struct d53_audio audio={0};
    while (!error && checkpoint<3) {
        if (!title_alive(v)) { error=-EIO; break; }
        error=title_keys(v,0,launched,&next);
        uint64_t ms=g_ticks-launched;
        const struct d53_checkpoint *cp=&title_plan.cp[checkpoint];
        if (!error && (ms>cp->deadline || g_ticks-started>60000)) error=-V86_ETIMEDOUT;
        if (!error && ms>=cp->at) {
            int rendered=dosvm_frame_digest(v,0,&frame);
            if (rendered && rendered!=-EAGAIN) error=rendered;
            if (!rendered) {
                sha256_hex(frame.digest,digest);
                if (d53_match(cp,digest,frame.text ? dosvm_text(v,0) : 0)) {
                    rec_emit("title","DATA","group=dos_title title=%s case=%s ms=%u matched=%u digest=%s",id,title_checks[checkpoint],(uint32_t)(g_ticks-started),!title_plan.calibration,digest);
                    checkpoint++;
                }
            }
        }
        if (!error) task_sleep_ms(20);
    }
    if (!error && next<title_plan.count && !title_plan.input[next].phase) error=-EINVAL;
    if (!error) { playing=g_ticks; memcpy(previous,frame.digest,32); }
    while (!error && g_ticks-playing<30000) {
        if (!title_alive(v)) { error=-EIO; break; }
        error=title_keys(v,1,playing,&next);
        int rendered=error ? error : dosvm_frame_digest(v,0,&frame);
        if (rendered && rendered!=-EAGAIN) error=rendered;
        if (!rendered && memcmp(previous,frame.digest,32)) { changes++;memcpy(previous,frame.digest,32); }
        title_audio(v,&audio);
        if (!d53_audio_ok(&title_plan,&audio) && (audio.underruns || audio.irq_latency_max_us>20000)) error=-EIO;
        if (!error) task_sleep_ms(20);
    }
    if (!error) error=title_keys(v,1,playing,&next);
    if (!error && next<title_plan.count && title_plan.input[next].phase==1) error=-EINVAL;
    if (!error) {
        title_audio(v,&audio);
        rec_emit("title","DATA","group=dos_title title=%s case=play duration_ms=%u faults=0 frame_changes=%u",id,(uint32_t)(g_ticks-playing),changes);
        rec_emit("title","DATA","group=dos_title title=%s case=audio attached=%u ready=%u irq=%u underruns=%llu",id,audio.attached,audio.ready,audio.irq,(unsigned long long)audio.underruns);
        rec_emit("title","DATA","group=dos_title title=%s case=pcm pcm_frames=%llu blocks=%u irq_entries=%u",id,(unsigned long long)audio.pcm_frames,audio.blocks,audio.irq_entries);
        rec_emit("title","DATA","group=dos_title title=%s case=cadence late_acks=%u irq_latency_max_us=%u opl_writes=%llu",id,audio.late_acks,audio.irq_latency_max_us,(unsigned long long)audio.opl_writes);
        if (!d53_audio_ok(&title_plan,&audio) || changes<2) error=-EIO;
    }
    uint64_t exiting=g_ticks;
    while (!error && next<title_plan.count) {
        /* The final key-down may return to RUNREC before its key-up. The
         * child verdict is allowed here, but never a stopped/faulted VM. */
        struct dosvm_info info; dosvm_info(v,&info);
        if (info.state>=DOSVM_STOPPING || info.dropped_records) { error=-EIO; break; }
        error=title_keys(v,2,exiting,&next);
        if (!error) task_sleep_ms(10);
    }
    if (!error) {
        rec_emit("title","DATA","group=dos_title title=%s case=input delivered=%u expected=%u",id,next,title_plan.count);
        until=deadline_after_ms(30000);
        struct dosvm_info info={0};
        for (;;) {
            dosvm_info(v,&info);
            if (info.state>=DOSVM_STOPPING || info.dropped_records) { error=-EIO; break; }
            if (info.verdict_done && title_prompt(v)) break;
            if (deadline_passed(until)) { error=-V86_ETIMEDOUT; break; }
            task_sleep_ms(10);
        }
        struct dpmi_info dpmi={0};
        dosvm_dpmi_info(dosvm_key(v),&dpmi);
        bool clean=!error && info.verdict_done && info.verdict_pass && !dpmi.active && !dpmi.stats.faults;
        rec_emit("title","DATA","group=dos_title title=%s case=exit clean=%u rc=%u faults=%u",id,clean,info.verdict_pass ? 0u : 1u,dpmi.stats.faults);
        if (!clean) error=-EIO;
    }
    if (v) {
        if (!error) error=title_type(v,"EXIT");
        until=deadline_after_ms(5000);
        while (!error) {
            struct dosvm_info info; dosvm_info(v,&info);
            if (info.state>=DOSVM_STOPPING) break;
            if (deadline_passed(until)) { error=-V86_ETIMEDOUT; break; }
            task_sleep_ms(10);
        }
        int destroyed=dosvm_destroy(dosvm_key(v),error!=0);
        if (!error) error=destroyed;
    }
    dosvm_snapshot(&after);
    bool restored=title_same(&before,&after);
    rec_emit("title","DATA","group=dos_title title=%s case=cleanup restored=%u",id,restored);
    if (!restored) error=-EIO;
    rec_emit("title","END","status=%s error=%d",error ? "FAIL" : "PASS",error);
    return !!error;
}
CIUKI_F3_PROBE("title",title_run);
