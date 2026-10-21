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

static struct d53_plan title_plan;
static uint8_t title_wire[D53_PLAN_BYTES];
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
            if (!error && entry.size!=sizeof(title_wire)) error=-EINVAL;
            if (!error) error=vfs_read(&title_table,fd,title_wire,sizeof(title_wire),&got);
            int closed=vfs_close(&title_table,fd);
            if (!error && (closed || got!=sizeof(title_wire))) error=-EIO;
        }
        vfs_table_destroy(&title_table);
    }
    if (!error && (!d53_parse(title_wire,sizeof(title_wire),&title_plan) || strncmp(id,title_plan.title,24))) error=-EINVAL;
    if (error) {
        rec_emit("title","END","status=FAIL reason=title_fixture_or_plan error=%d",error);
        return 1;
    }
    uint8_t plan_hash[32]; char plan_hex[65];
    sha256(title_wire,sizeof(title_wire),plan_hash); sha256_hex(plan_hash,plan_hex);
    rec_emit("title","DATA","group=dos_title title=%s case=plan sha256=%s",id,plan_hex);
    struct dosvm_ledger before,after;
    dosvm_snapshot(&before);
    struct dosvm *v=0;
    struct dosvm_config cfg={.probe="title",.base=base,.dpmi=true};
    uint64_t started=g_ticks, launched=0, playing=0;
    error=dosvm_create(&cfg,&v);
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
                    rec_emit("title","DATA","group=dos_title title=%s case=%s ms=%u matched=1 digest=%s",id,title_checks[checkpoint],(uint32_t)(g_ticks-started),digest);
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
