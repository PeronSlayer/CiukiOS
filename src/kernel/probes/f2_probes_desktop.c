/* Interim ring-3 object-isolation controller; no simulated fault success.
 * Payload execution and progress require the lead's QEMU evidence.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/supervisor.h>
#include <ciuki/probe.h>
#include <ciuki/storage.h>

#ifdef CIUKI_DESKTOP_PAYLOAD_BIN
__asm__(".pushsection .rodata.desktop_payload,\"a\"\n"
        ".balign 4\n"
        "desktop_payload_start:\n"
        ".incbin \"" CIUKI_DESKTOP_PAYLOAD_BIN "\"\n"
        "desktop_payload_end:\n"
        ".popsection\n");
extern const uint8_t desktop_payload_start[], desktop_payload_end[];
#define DESKTOP_RESULT (CIUKI_IMAGE_BASE + CIUKI_PAGE_SIZE)
struct desktop_result {
    uint32_t mode, stage, errors, turns, ticks;
    int32_t endpoint, victim_endpoint;
    uint32_t release, surface, mapping;
    int32_t display, input;
    uint32_t unauthorized, ack;
};
_Static_assert(offsetof(struct desktop_result, ack) == 52, "private NASM controller layout");
static int desktop_image_read(void *cookie, uint32_t off, void *dst, uint32_t bytes)
{
    (void)cookie;
    uint32_t size = (uint32_t)(desktop_payload_end - desktop_payload_start);
    if (off > size || bytes > size - off) return -EIO;
    memcpy(dst, desktop_payload_start + off, bytes);
    return 0;
}
static int desktop_payload(struct process *parent, unsigned mode, int32_t endpoint, struct process **out)
{
    struct proc_strings *strings = proc_strings_new();
    if (!strings) return -ENOMEM;
    int err = proc_strings_add(strings, "standin", sizeof("standin"), false);
    struct ciuki_file file = { .bytes = (uint32_t)(desktop_payload_end - desktop_payload_start), .read = desktop_image_read };
    struct ciuki_spawn_fd inherit = { endpoint, 4 };
    if (!err) err = proc_spawn_file(parent, &file, strings, endpoint < 0 ? 0 : &inherit,
                                    endpoint < 0 ? 0 : 1, CIUKI_SPAWN_NEW_GROUP, 0, out);
    proc_strings_free(strings);
    if (err < 0) return err;
    struct desktop_result initial = { .mode = mode, .endpoint = endpoint < 0 ? -1 : 4,
        .victim_endpoint = -1, .display = -1, .input = -1 };
    ua_write((*out)->memory, DESKTOP_RESULT, &initial, sizeof(initial));
    return err;
}
static bool desktop_result_read(struct process *p, struct desktop_result *r)
{
    return p && p->state == PROC_LIVE && p->memory &&
        !ua_read(p->memory, r, DESKTOP_RESULT, sizeof(*r));
}
static void desktop_word(struct process *p, unsigned offset, uint32_t value)
{
    if (p && p->state == PROC_LIVE) ua_write(p->memory, DESKTOP_RESULT + offset, &value, sizeof(value));
}
static void desktop_activity_record(struct process *server, int32_t grant, const char *stage)
{
    struct desktop_activity a;
    desktop_activity_snapshot(&a);
    struct ciuki_display_info info;
    int err = grant_display_info(server, grant, &info);
    uint32_t digest = 0;
    if (!err) {
        const struct fb_device *d = fbdev_get();
        digest = fnv1a32(d->mapped, d->size, 2166136261u);
    }
    rec_emit("crash-isolation", "DATA", "case=display server=standin stage=%s presents=%llu input_events=%llu pixel_digest=%08x display_error=%d",
             stage, a.presents, a.input_events, digest, err);
}
static bool desktop_pause(struct process *survivor)
{
    desktop_word(survivor, offsetof(struct desktop_result, ack), 1);
    struct desktop_result r;
    uint64_t deadline = g_ticks + 1000;
    while (g_ticks < deadline && desktop_result_read(survivor, &r)) {
        if (r.stage == 2) return true;
        task_sleep_ms(1);
    }
    return false;
}

#endif

static void desktop_remove(struct process *parent, struct process *p)
{
    if (!p) return;
    uint32_t pid=p->pid;
    if (p->state != PROC_ZOMBIE) proc_stop(p, 0, 0);
    uint64_t deadline = g_ticks + 1000;
    while ((p=proc_find(pid)) && p->state != PROC_ZOMBIE && g_ticks < deadline) { proc_collect(); task_sleep_ms(1); }
    if (p && p->state == PROC_ZOMBIE) proc_reap(parent, p);
}
static bool desktop_ledgers_equal(const struct desktop_ledger *a, const struct desktop_ledger *b)
{
    return a->descriptions == b->descriptions && a->surfaces == b->surfaces &&
        a->pages == b->pages && a->channels == b->channels && a->messages == b->messages && a->grants == b->grants;
}

/* Native summaries enter through the existing call-3 hook below. Only the
 * controller-selected desktop and its registered children can update state.
 * The desktop explicitly publishes a dedicated anonymous control page. */
#include "../../../apps/desktop/gate.h"
extern int supervisor_spawn_desktop_probe(struct process **out);
extern int probes_crash_server(const char *, unsigned, uint32_t, bool, bool);
static bool report_unsigned(const char *, const char *, uint32_t *);
static struct {
    bool active, invalid;
    const char *invalid_check;
    uint32_t reported_control, reported_survivor, reported_stage;
    uint32_t setup_step, setup_error;
    uint32_t server, survivor, victim, control, generation, cycles, replies;
    uint32_t survivor_stage, survivor_turns, survivor_snapshot, victim_stage, unauthorized, keys, motion, buttons;
} native_reports;

static void native_invalid(const char *check)
{
    if (!native_reports.invalid) native_reports.invalid_check=check;
    native_reports.invalid=true;
}

static void native_report(struct process *p, const char *line, unsigned length)
{
    if (!native_reports.active || !p) return;
    bool server = p->pid == native_reports.server;
    bool survivor = p->pid == native_reports.survivor;
    bool victim = p->pid == native_reports.victim;
    if (!server && !survivor && !victim) return;
    if (native_reports.invalid) return;
    if (!length || length > CIUKI_PROBE_REPORT_MAX) { native_invalid("length"); return; }
    if (server && !strncmp(line, "case=native-desktop ", 20)) {
        uint32_t control, generation, child, fault, cycle, replies, keys, motion, buttons;
        /* Preserve the first rejection, rather than overwriting it with a
         * subsequent report. Parsed identities remain visible on failure. */
#define NATIVE_REQUIRE(test, check) do { if (!(test)) { native_invalid(check); return; } } while (0)
#define NATIVE_FIELD(field) NATIVE_REQUIRE(report_unsigned(line,#field,&field), #field)
        NATIVE_FIELD(control); native_reports.reported_control=control;
        NATIVE_FIELD(generation);
        NATIVE_REQUIRE(report_unsigned(line,"survivor",&child),"survivor"); native_reports.reported_survivor=child;
        NATIVE_REQUIRE(report_unsigned(line,"victim",&fault),"victim");
        NATIVE_FIELD(cycle); NATIVE_FIELD(replies); NATIVE_FIELD(keys); NATIVE_FIELD(motion); NATIVE_FIELD(buttons);
        struct process *c=proc_find(child), *v=fault ? proc_find(fault) : 0;
        NATIVE_REQUIRE(c,"survivor_pid");
        NATIVE_REQUIRE(c->ppid==p->pid,"survivor_parent");
        NATIVE_REQUIRE(c->pgid!=p->pgid,"survivor_group");
        NATIVE_REQUIRE(!fault || (v && v->ppid==p->pid && v->pgid!=p->pgid && fault!=child),"victim_identity");
        NATIVE_REQUIRE(!(control & (CIUKI_PAGE_SIZE-1)),"control_align");
        NATIVE_REQUIRE(control>=CIUKI_MMAP_BASE,"control_arena");
        NATIVE_REQUIRE(p->memory && ua_range(p->memory,control,CIUKI_PAGE_SIZE,PROT_READ|PROT_WRITE),"control_range");
        NATIVE_REQUIRE(!native_reports.control || control==native_reports.control,"control_changed");
        NATIVE_REQUIRE(!native_reports.survivor || child==native_reports.survivor,"survivor_changed");
        NATIVE_REQUIRE(generation>=native_reports.generation,"generation_order");
        NATIVE_REQUIRE(cycle>=native_reports.cycles && cycle<=100,"cycle_order");
        NATIVE_REQUIRE(replies>=native_reports.replies,"replies_order");
        NATIVE_REQUIRE(keys>=native_reports.keys && motion>=native_reports.motion && buttons>=native_reports.buttons,"input_order");
        native_reports.control=control; native_reports.generation=generation;
        native_reports.survivor=child; native_reports.victim=fault;
        native_reports.cycles=cycle; native_reports.replies=replies;
        native_reports.keys=keys; native_reports.motion=motion; native_reports.buttons=buttons;
    } else if (!server && !strncmp(line,"case=native-demo ",17)) {
        uint32_t stage, turns, unauthorized, generation;
        NATIVE_FIELD(stage); native_reports.reported_stage=stage;
        NATIVE_FIELD(turns); NATIVE_FIELD(unauthorized); NATIVE_FIELD(generation);
        native_reports.unauthorized |= unauthorized;
        NATIVE_REQUIRE(!unauthorized,"unauthorized");
        NATIVE_REQUIRE(survivor ? stage==2 : stage==1 || stage==3,"stage");
        NATIVE_REQUIRE(!survivor || turns>=native_reports.survivor_turns,"turns_order");
        if (survivor) { native_reports.survivor_stage=stage; native_reports.survivor_turns=turns; native_reports.survivor_snapshot=generation; }
        else native_reports.victim_stage=stage;
    } else if (server && !strncmp(line,"case=native-setup ",18)) {
        uint32_t step,error; NATIVE_FIELD(step); NATIVE_FIELD(error);
        NATIVE_REQUIRE(step && step<ARRAY_SIZE(gate_setup_checks),"setup_step");
        if (!native_reports.setup_step) { native_reports.setup_step=step; native_reports.setup_error=error; }
    } else native_invalid("case");
#undef NATIVE_FIELD
#undef NATIVE_REQUIRE
}
static bool native_live(struct process *p)
{
    return p && p->state==PROC_LIVE && p->memory && !native_reports.invalid && !native_reports.setup_step;
}
static void native_reason(char reason[64], const char *fallback)
{
    struct process *server=proc_find(native_reports.server), *survivor=proc_find(native_reports.survivor);
    if (native_reports.invalid)
        ksnprintf(reason,64,"invalid_report:%s",native_reports.invalid_check);
    else if (native_reports.setup_step)
        ksnprintf(reason,64,"setup:%s:%u",gate_setup_checks[native_reports.setup_step],native_reports.setup_error);
    else if (native_reports.server && (!server || server->state!=PROC_LIVE))
        ksnprintf(reason,64,"server_exit:%d",server ? server->status : -1);
    else if (native_reports.survivor && (!survivor || survivor->state!=PROC_LIVE))
        ksnprintf(reason,64,"survivor_exit:%d",survivor ? survivor->status : -1);
    else ksnprintf(reason,64,"%s",fallback);
}
static void native_launch_record(uint64_t start, const char *failure)
{
    char reason[64]; native_reason(reason,failure ? failure : "ok");
    rec_emit("crash-isolation","DATA","case=launch server=desktop pid=%u survivor=%u control=%08x stage=%u ticks=%llu reason=%s",
        native_reports.server,native_reports.reported_survivor,native_reports.reported_control,
        native_reports.reported_stage,g_ticks-start,reason);
}
static void native_step_record(unsigned command, uint32_t generation, const char *failure)
{
    struct process *server=proc_find(native_reports.server), *survivor=proc_find(native_reports.survivor), *victim=proc_find(native_reports.victim);
    char reason[64];
    if (command) native_reason(reason,failure);
    else ksnprintf(reason,sizeof(reason),"%s",failure);
    rec_emit("crash-isolation","DATA","case=step server=desktop command=%u generation=%u reached=%u live=%u survivor=%u victim=%u reason=%s",
        command,generation,native_reports.generation,server && server->state==PROC_LIVE,
        survivor && survivor->state==PROC_LIVE,victim && victim->state==PROC_LIVE,reason);
}
static const char *native_command_failure;
static bool native_command(struct process *server, unsigned command, uint32_t *generation)
{
    native_command_failure="missing_control";
    if (!native_live(server) || !native_reports.control) return false;
    struct gate_control control={.command=command,.generation=++*generation};
    native_command_failure="control_write";
    if (ua_write(server->memory,native_reports.control,&control,sizeof(control))) return false;
    uint64_t deadline=g_ticks+3000;
    while (native_live(server) && (native_reports.generation<*generation || (command==GATE_SNAPSHOT && native_reports.survivor_snapshot<*generation)) && g_ticks<deadline) task_sleep_ms(1);
    native_command_failure=command==GATE_SNAPSHOT ? "snapshot_timeout" : "command_timeout";
    bool ready=native_live(server) && native_reports.generation==*generation && (command!=GATE_SNAPSHOT || native_reports.survivor_snapshot==*generation);
    if (native_reports.generation>*generation || (command==GATE_SNAPSHOT && native_reports.survivor_snapshot>*generation)) native_command_failure="generation_mismatch";
    return ready;
}
static uint32_t native_digest(void)
{
    const struct fb_device *d=fbdev_get();
    return d->present && d->mapped ? fnv1a32(d->mapped,d->size,2166136261u) : 0;
}
static bool native_payload_present(void)
{
    const struct ciuki_file_ops *ops=proc_get_file_ops();
    struct ciuki_file file={0};
    if (!ops || !ops->open || ops->open(proc_supervisor()->cwd,"/bin/desktop",&file)) return false;
    if (file.close) file.close(file.cookie);
    return true;
}
static bool native_proc_equal(const struct proc_ledger *a,const struct proc_ledger *b)
{
    return a->processes==b->processes && a->threads==b->threads && a->zombies==b->zombies &&
        a->handles==b->handles && a->extents==b->extents && a->backing==b->backing && a->tables==b->tables;
}
static int native_crash_isolation(void)
{
    const char *name="crash-isolation";
    struct desktop_ledger initial,final;
    struct proc_ledger before,after;
    desktop_snapshot(&initial); proc_snapshot(&before);
    struct process *owner=0,*server=0,*survivor=0;
    uint32_t deaths=supervisor_desktop_deaths(), generation=0, cycles=0;
    uint64_t launch_start=g_ticks;
    memset(&native_reports,0,sizeof(native_reports));
    bool pass=!proc_prepare(proc_supervisor(),&owner);
    if (!pass) { native_launch_record(launch_start,"controller_setup"); goto finished; }
    proc_publish(owner,0);
    int made=supervisor_spawn_desktop_probe(&server);
    if (made<0) { rec_emit(name,"ERROR","server=desktop reason=desktop_launch error=%d",made); native_launch_record(launch_start,"desktop_launch"); pass=false; goto cleanup; }
    server->ppid=owner->pid;
    native_reports.active=true; native_reports.server=server->pid;
    rec_emit(name,"DATA","case=payload server=desktop path=/bin/desktop clients=/bin/demo");
    uint32_t server_pid=server->pid,server_cr3=server->memory->as.pd_phys,server_pgid=server->pgid;
    uint64_t deadline=g_ticks+10000;
    while (native_live(server) && !native_reports.survivor_stage && g_ticks<deadline) task_sleep_ms(1);
    survivor=proc_find(native_reports.survivor);
    const char *launch_failure="survivor_timeout";
    pass=native_live(server) && native_live(survivor) && native_reports.survivor_stage==2;
    if (pass) { pass=native_command(server,GATE_SNAPSHOT,&generation); launch_failure=native_command_failure; }
    native_launch_record(launch_start,pass ? 0 : launch_failure);
    if (!pass) goto cleanup;
    uint32_t survivor_pid=survivor->pid,survivor_cr3=survivor->memory->as.pd_phys,survivor_pgid=survivor->pgid;
    pass=server_cr3!=survivor_cr3 && server_pgid!=survivor_pgid;
    if (!pass) native_step_record(GATE_SNAPSHOT,generation,"identity");
    rec_emit(name,"DATA","case=identity server=desktop server_pid=%u survivor_pid=%u server_pgid=%u survivor_pgid=%u",
        server_pid,survivor_pid,server_pgid,survivor_pgid);
    rec_emit(name,"DATA","case=spaces server=desktop server_cr3=%08x survivor_cr3=%08x",server_cr3,survivor_cr3);
    for (;pass && cycles<100;cycles++) {
        struct desktop_ledger baseline,restored;
        struct proc_ledger baseline_proc,restored_proc;
        desktop_snapshot(&baseline); proc_snapshot(&baseline_proc);
        native_reports.victim_stage=0;
        pass=native_command(server,GATE_SPAWN,&generation);
        if (!pass) { native_step_record(GATE_SPAWN,generation,native_command_failure); break; }
        struct process *victim=proc_find(native_reports.victim);
        pass=native_live(victim);
        if (!pass) {
            char reason[64]; ksnprintf(reason,sizeof(reason),"victim_exit:%d",victim ? victim->status : -1);
            native_step_record(GATE_SPAWN,generation,reason); break;
        }
        uint32_t victim_pid=victim->pid,victim_cr3=victim->memory->as.pd_phys,victim_pgid=victim->pgid;
        pass=victim_cr3!=server_cr3 && victim_cr3!=survivor_cr3 && victim_pgid!=server_pgid && victim_pgid!=survivor_pgid;
        deadline=g_ticks+3000;
        while (pass && native_live(victim) && native_reports.victim_stage!=1 && g_ticks<deadline) task_sleep_ms(1);
        if (!pass || native_reports.victim_stage!=1) {
            native_step_record(GATE_SPAWN,generation,pass ? "victim_timeout" : "victim_identity"); pass=false; break;
        }
        pass=native_command(server,GATE_RELEASE,&generation);
        if (!pass) { native_step_record(GATE_RELEASE,generation,native_command_failure); break; }
        deadline=g_ticks+3000;
        while (pass && victim->state!=PROC_ZOMBIE && g_ticks<deadline) { proc_collect(); task_sleep_ms(1); }
        int status=victim->state==PROC_ZOMBIE ? victim->status : -1;
        unsigned expected=cycles%5==1 ? SIGPIPE : SIGSEGV;
        pass=pass && native_reports.victim_stage==3 && status==(int)expected;
        rec_emit(name,"DATA","case=victim server=desktop cycle=%u pid=%u cr3=%08x pgid=%u kind=%s expected=%u status=%d",
            cycles+1,victim_pid,victim_cr3,victim_pgid,gate_faults[cycles%5],expected,status);
        if (!pass) { native_step_record(GATE_RELEASE,generation,native_reports.victim_stage!=3 ? "victim_stage" : status==-1 ? "victim_exit_timeout" : "victim_status"); break; }
        if (!native_command(server,GATE_REAP,&generation)) { native_step_record(GATE_REAP,generation,native_command_failure); pass=false; break; }
        uint32_t start_server=native_reports.replies,start_client=native_reports.survivor_turns;
        native_reports.survivor_stage=0;
        uint64_t start_tick=g_ticks;
        pass=native_command(server,GATE_RUN,&generation);
        if (!pass) { native_step_record(GATE_RUN,generation,native_command_failure); break; }
        deadline=start_tick+3000;
        while (pass && native_live(server) && native_live(survivor) && g_ticks<deadline &&
               (native_reports.survivor_stage!=2 || native_reports.survivor_turns-start_client<100 || g_ticks-start_tick<100)) task_sleep_ms(1);
        pass=native_live(server) && native_live(survivor) && native_reports.survivor_stage==2 && native_reports.survivor_turns-start_client>=100 && g_ticks-start_tick>=100;
        if (!pass) { native_step_record(GATE_RUN,generation,"progress_timeout"); break; }
        if (!native_command(server,GATE_SNAPSHOT,&generation)) { native_step_record(GATE_SNAPSHOT,generation,native_command_failure); pass=false; break; }
        desktop_snapshot(&restored); proc_snapshot(&restored_proc);
        pass=pass && native_live(server) && native_live(survivor) &&
            server->pid==server_pid && survivor->pid==survivor_pid &&
            server->memory->as.pd_phys==server_cr3 && survivor->memory->as.pd_phys==survivor_cr3 &&
            server->pgid==server_pgid && survivor->pgid==survivor_pgid &&
            native_reports.replies-start_server>=100 && native_reports.survivor_turns-start_client>=100 &&
            native_reports.survivor_stage==2 && g_ticks-start_tick>=100 &&
            !native_reports.unauthorized && desktop_ledgers_equal(&baseline,&restored) && native_proc_equal(&baseline_proc,&restored_proc) &&
            supervisor_desktop_deaths()==deaths;
        rec_emit(name,"DATA","case=progress server=desktop cycle=%u server_replies=%u replies=%u ticks=%llu unauthorized_access=%u desktop_restarts=%u",
            cycles+1,native_reports.replies-start_server,native_reports.survivor_turns-start_client,g_ticks-start_tick,
            native_reports.unauthorized,supervisor_desktop_deaths()-deaths);
        rec_emit(name,"DATA","case=restored server=desktop cycle=%u equal=%u processes_equal=%u",cycles+1,desktop_ledgers_equal(&baseline,&restored),native_proc_equal(&baseline_proc,&restored_proc));
        if (!pass) native_step_record(GATE_SNAPSHOT,generation,"restored_contract");
    }
    rec_emit(name,"DATA","case=cycles server=desktop cycles=%u desktop_restarts=%u",cycles,supervisor_desktop_deaths()-deaths);
    if (pass) {
        pass=native_command(server,GATE_INTERACT,&generation);
        if (!pass) { native_step_record(GATE_INTERACT,generation,native_command_failure); goto cleanup; }
        /* Let the desktop restore pixels after the command's report. */
        task_sleep_ms(20);
        struct desktop_activity a,b;
        desktop_activity_snapshot(&a); uint32_t digest=native_digest();
        rec_emit(name,"ARM","server=desktop action=post_fault_input presents=%llu input_events=%llu pixel_digest=%08x",a.presents,a.input_events,digest);
        uint64_t start=g_ticks; deadline=start+15000;
        do { task_sleep_ms(10); desktop_activity_snapshot(&b); }
        while (native_live(server) && g_ticks<deadline && (native_reports.keys<2 || native_reports.motion<1 || native_reports.buttons<2 || b.presents<=a.presents || g_ticks-start<500));
        uint32_t next_digest=native_digest();
        pass=pass && native_live(server) && native_live(survivor) && b.input_events>a.input_events && b.presents>a.presents &&
            next_digest!=digest && native_reports.keys>=2 && native_reports.motion>=1 && native_reports.buttons>=2 && server->pid==server_pid && survivor->pid==survivor_pid && supervisor_desktop_deaths()==deaths;
        rec_emit(name,"DATA","case=interaction server=desktop stage=before presents=%llu input_events=%llu pixel_digest=%08x",a.presents,a.input_events,digest);
        rec_emit(name,"DATA","case=interaction server=desktop stage=after presents=%llu input_events=%llu pixel_digest=%08x changed=%u keys=%u motion=%u buttons=%u",b.presents,b.input_events,next_digest,pass,native_reports.keys,native_reports.motion,native_reports.buttons);
        if (!pass) native_step_record(GATE_INTERACT,generation,"interaction_contract");
        /* Runner observes the frame while its owning desktop is still live.
         * Capture failure/deadline cannot be turned into guest qualification. */
        task_sleep_ms(3000);
    }
cleanup:
    native_reports.active=false;
    if (server) {
        /* Test children belong to the production desktop. Collect them before
         * the desktop, retaining no private result mappings after teardown. */
        for (unsigned i=0;i<2;i++) {
            struct process *child=proc_find(i ? native_reports.victim : native_reports.survivor);
            if (child) desktop_remove(server,child);
        }
        desktop_remove(owner,server);
    }
    if (owner) { proc_stop(owner,0,0); proc_collect(); }
finished:
    desktop_snapshot(&final); proc_snapshot(&after);
    bool objects=desktop_ledgers_equal(&initial,&final), processes=native_proc_equal(&before,&after);
    if (!objects || !processes) native_step_record(0,generation,"cleanup_ledger");
    pass=pass && objects && processes;
    rec_emit(name,"DATA","case=ledger server=desktop objects_equal=%u processes_equal=%u descriptions=%u surfaces=%u pages=%u channels=%u messages=%u grants=%u",
        objects,processes,final.descriptions,final.surfaces,final.pages,final.channels,final.messages,final.grants);
    rec_emit(name,"END",pass ? "server=desktop status=PASS" : "server=desktop status=FAIL reason=desktop_contract");
    return pass ? 0 : 1;
}

int supervisor_spawn_standin(struct process *parent, struct process **out)
{
#ifdef CIUKI_DESKTOP_PAYLOAD_BIN
    int result = desktop_payload(parent, 0, -1, out);
    if (result < 0) return result;
    int32_t fds[2];
    int err = grants_install(*out, fds);
    if (err) {
        proc_stop(*out, 1, 0);
        if (parent != proc_supervisor()) desktop_remove(parent, *out);
        else proc_collect();
        *out = 0;
        return err;
    }
    desktop_word(*out, offsetof(struct desktop_result, display), (uint32_t)fds[0]);
    desktop_word(*out, offsetof(struct desktop_result, input), (uint32_t)fds[1]);
    return result;
#else
    (void)parent; (void)out;
    return -ENOENT;
#endif
}

int probe_f2_crash_isolation(void)
{
    const char *name = "crash-isolation";
    const struct fb_device *display=fbdev_get();
    bool payload=display->present && !(g_boot.flags & CBI_F_SAFE_MODE) && native_payload_present();
    int selected=probes_crash_server(g_boot.test_request,g_boot.test_request_len,g_boot.flags,display->present,payload);
    rec_emit(name, "BEGIN", "server=%s",selected>0 ? "desktop" : "standin");
    if (selected<0) { rec_emit(name,"END","status=FAIL reason=selector"); return 1; }
    if (selected) return native_crash_isolation();
#ifndef CIUKI_DESKTOP_PAYLOAD_BIN
    rec_emit(name, "ERROR", "server=standin status=not_run reason=missing_standin_payload");
    rec_emit(name, "END", "server=standin status=FAIL reason=not_run");
    return 1;
#else
    if (!ua_map_shared) {
        rec_emit(name, "ERROR", "server=standin status=not_run reason=missing_shared_mapping_hooks");
        rec_emit(name, "END", "server=standin status=FAIL reason=not_run");
        return 1;
    }
    uint8_t hash[32]; char hex[65];
    sha256(desktop_payload_start, (uint32_t)(desktop_payload_end - desktop_payload_start), hash);
    sha256_hex(hash, hex);
    rec_emit(name, "DATA", "case=payload server=standin sha256=%s", hex);
    struct process *parent = 0, *server = 0, *survivor = 0;
    struct desktop_ledger initial, final;
    desktop_snapshot(&initial);
    struct proc_ledger before, after;
    proc_snapshot(&before);
    bool pass = proc_prepare(proc_supervisor(), &parent) == 0;
    if (!pass) goto finished;
    proc_publish(parent, 0); /* kernel controller's child owner, no runnable user thread */
    int32_t pair[2];
    pass = !channel_pair(parent, pair);
    if (!pass) goto cleanup;
    parent->fds[pair[0]].flags = parent->fds[pair[1]].flags = 0;
    pass = desktop_payload(parent, 0, pair[0], &server) > 0;
    if (!pass) goto cleanup;
    int32_t grants[2];
    pass = !grants_install(server, grants);
    if (!pass) goto cleanup;
    desktop_word(server, offsetof(struct desktop_result, display), (uint32_t)grants[0]);
    desktop_word(server, offsetof(struct desktop_result, input), (uint32_t)grants[1]);
    pass = desktop_payload(parent, 1, pair[1], &survivor) > 0;
    desktop_close(parent, pair[0]); desktop_close(parent, pair[1]);
    if (!pass) goto cleanup;
    uint32_t server_pid = server->pid, survivor_pid = survivor->pid;
    uint32_t server_cr3 = server->memory->as.pd_phys, survivor_cr3 = survivor->memory->as.pd_phys;
    rec_emit(name, "DATA", "case=identity server=standin server_pid=%u survivor_pid=%u server_cr3=%08x survivor_cr3=%08x",
             server_pid, survivor_pid, server_cr3, survivor_cr3);
    struct desktop_result sr = { 0 }, cr = { 0 };
    uint64_t deadline = g_ticks + 2000;
    while (g_ticks < deadline && desktop_result_read(survivor, &cr) && cr.turns < 2) task_sleep_ms(1);
    pass = desktop_result_read(server, &sr) && cr.turns >= 2 && !cr.errors && !sr.errors && desktop_pause(survivor);
    desktop_activity_record(server, grants[0], "before");
    unsigned cycles = 0;
    for (; pass && cycles < 100; cycles++) {
        struct desktop_ledger baseline, restored;
        desktop_snapshot(&baseline);
        struct process *victim = 0;
        int32_t ends[2];
        if (channel_pair(parent, ends)) { pass = false; break; }
        parent->fds[ends[1]].flags = 0;
        int server_fd = desktop_fd_slot(server, 0);
        if (server_fd < 0) { pass = false; break; }
        /* Trusted controller moves one endpoint; client receives only the
         * other through production explicit spawn inheritance. */
        server->fds[server_fd] = parent->fds[ends[0]];
        parent->fds[ends[0]] = (struct proc_fd){ 0 };
        desktop_word(server, offsetof(struct desktop_result, ack), 0);
        desktop_word(server, offsetof(struct desktop_result, victim_endpoint), (uint32_t)server_fd);
        unsigned mode = 2 + cycles % 4;
        int made = desktop_payload(parent, mode, ends[1], &victim);
        desktop_close(parent, ends[1]);
        if (made < 0) { pass = false; desktop_close(server, server_fd); break; }
        uint32_t victim_pid = victim->pid, victim_cr3 = victim->memory->as.pd_phys;
        pass = victim_cr3 != server_cr3 && victim_cr3 != survivor_cr3 && victim->pgid != server->pgid && victim->pgid != survivor->pgid && server->pgid != survivor->pgid && server_cr3 != survivor_cr3;
        bool armed = false;
        struct desktop_result vr = { 0 };
        desktop_word(survivor, offsetof(struct desktop_result, ack), 0);
        deadline = g_ticks + 2000;
        while (g_ticks < deadline && victim->state != PROC_ZOMBIE) {
            if (!armed && desktop_result_read(server, &sr) && sr.ack)
                desktop_word(victim, offsetof(struct desktop_result, ack), 1);
            if (!armed && desktop_result_read(victim, &vr) && vr.stage == 3) {
                pass = pass && !vr.errors && !vr.unauthorized;
                armed = true;
                desktop_word(victim, offsetof(struct desktop_result, ack), 2);
            }
            task_sleep_ms(1);
        }
        int status = victim->state == PROC_ZOMBIE ? victim->status : -1;
        uint32_t expected = mode == 3 ? SIGPIPE : SIGSEGV;
        pass = pass && armed && status == (int)expected;
        /* Closing the receiver drops the pending message and its surface ref. */
        desktop_word(server, offsetof(struct desktop_result, victim_endpoint), UINT32_MAX);
        desktop_close(server, server_fd);
        desktop_remove(parent, victim);
        desktop_result_read(server, &sr); desktop_result_read(survivor, &cr);
        uint32_t start_server = sr.turns, start_client = cr.turns;
        uint64_t start_tick = g_ticks;
        deadline = g_ticks + 2500;
        do {
            task_sleep_ms(1);
            pass = pass && desktop_result_read(server, &sr) && desktop_result_read(survivor, &cr);
        } while (pass && g_ticks < deadline && (sr.turns - start_server < 100 || cr.turns - start_client < 100 || g_ticks - start_tick < 100));
        pass = pass && desktop_pause(survivor);
        desktop_snapshot(&restored);
        pass = pass && server->pid == server_pid && survivor->pid == survivor_pid &&
            server->memory->as.pd_phys == server_cr3 && survivor->memory->as.pd_phys == survivor_cr3 &&
            sr.turns - start_server >= 100 && cr.turns - start_client >= 100 && g_ticks - start_tick >= 100 &&
            !sr.errors && !cr.errors && !cr.unauthorized && desktop_ledgers_equal(&baseline, &restored);
        rec_emit(name, "DATA", "case=victim server=standin cycle=%u pid=%u cr3=%08x mode=%u expected=%u status=%d",
                 cycles + 1, victim_pid, victim_cr3, mode, expected, status);
        rec_emit(name, "DATA", "case=progress server=standin cycle=%u server_replies=%u replies=%u ticks=%llu unauthorized_access=%u desktop_restarts=0",
                 cycles + 1, sr.turns - start_server, cr.turns - start_client, g_ticks - start_tick, cr.unauthorized);
        rec_emit(name,"DATA","case=restored server=standin cycle=%u equal=%u",cycles+1,desktop_ledgers_equal(&baseline,&restored));
        /* Survivor is paused after consuming its reply: both snapshots have
         * no ordinary request in flight, so messages are compared exactly. */
    }
    rec_emit(name, "DATA", "case=cycles server=standin cycles=%u desktop_restarts=0", cycles);
    if (server->state == PROC_LIVE) desktop_activity_record(server, grants[0], "after");
cleanup:
    if (parent) {
        desktop_remove(parent, survivor);
        desktop_remove(parent, server);
        proc_stop(parent, 0, 0);
        proc_collect(); /* PID 1 reaps the kernel controller owner */
    }
finished:
    desktop_snapshot(&final); proc_snapshot(&after);
    pass = pass && desktop_ledgers_equal(&initial, &final) &&
        before.processes == after.processes && before.threads == after.threads &&
        before.handles == after.handles && before.extents == after.extents && before.backing == after.backing;
    rec_emit(name, "DATA", "case=ledger server=standin objects_equal=%u processes_equal=%u descriptions=%u surfaces=%u pages=%u channels=%u messages=%u grants=%u",
             desktop_ledgers_equal(&initial,&final),native_proc_equal(&before,&after),final.descriptions, final.surfaces, final.pages, final.channels, final.messages, final.grants);
    rec_emit(name, "END", pass ? "server=standin status=PASS" : "server=standin status=FAIL reason=desktop_contract");
    return pass ? 0 : 1;
#endif
}

/* Call 3 remains bounded ASCII and is framed by the existing supervisor.
 * Observe the trusted payload's three summaries as well as its exit status. */
static struct { uint32_t pid, stages, checks, failures; bool invalid; } libc_reports;
static bool report_unsigned(const char *line, const char *field, uint32_t *value)
{
    size_t length = strlen(field);
    for (const char *p = line; *p; p++) {
        if ((p == line || p[-1] == ' ') && !strncmp(p, field, length) && p[length] == '=') {
            p += length + 1;
            if (*p < '0' || *p > '9') return false;
            uint32_t n = 0;
            do {
                unsigned digit = (unsigned)(*p++ - '0');
                if (n > (UINT32_MAX - digit) / 10) return false;
                n = n * 10 + digit;
            } while (*p >= '0' && *p <= '9');
            if (*p && *p != ' ') return false;
            *value = n; return true;
        }
    }
    return false;
}
void probe_f2_libc_report(struct task *task, const char *line, uint32_t length)
{
    struct proc_thread *t = proc_thread_for(task);
    if (t) native_report(t->process,line,length);
    if (!libc_reports.pid || !t || t->process->pid != libc_reports.pid) return;
    if (!length || length > CIUKI_PROBE_REPORT_MAX) { libc_reports.invalid = true; return; }
    const char *cases[] = { "case=libc-smoke ", "case=atexit ", "case=destructor " };
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        if (strncmp(line, cases[i], strlen(cases[i]))) continue;
        uint32_t failures, checks, order;
        if (libc_reports.stages != i || !report_unsigned(line, "failures", &failures) ||
            !report_unsigned(line, "checks", &checks) || !report_unsigned(line, "order", &order) ||
            !checks || checks < libc_reports.checks || order != i + 2) {
            libc_reports.invalid = true; return;
        }
        libc_reports.stages++; libc_reports.checks = checks; libc_reports.failures |= failures;
        return;
    }
    uint32_t observed;
    if (report_unsigned(line, "observed", &observed) && !observed)
        libc_reports.failures |= 1; /* a failed CHECK reports expected=1 observed=0 */
}

int probe_f2_libc_smoke(void)
{
    const char *name = "libc-smoke";
    rec_emit(name, "BEGIN", 0);
    /* Test selectors start read-only. Temporary-file assertions require the
     * existing F1 write qualification, as does the fd-table controller. */
    int setup = storage_enable_write(storage_get(), 2);
    rec_emit(name, "DATA", "case=write-gate expected=0 observed=%d", setup);
    if (setup) { rec_emit(name, "END", "status=FAIL reason=write_gate"); return 1; }
    const char *argv[] = { "libc_smoke", 0 };
    struct process *app = 0;
    int result = supervisor_spawn("/bin/libc_smoke", argv, 0, false, &app);
    if (result < 0) {
        rec_emit(name, "ERROR", "status=not_run reason=%s error=%d",
                 result == -ENOENT ? "missing_payload" : "application_setup", result);
        rec_emit(name, "END", "status=FAIL reason=not_run");
        return 1;
    }
    uint32_t pid = app->pid;
    /* Retain exit status using a private child owner until the controller
     * consumes it; orphans still go through PID 1 on controller teardown. */
    struct process *owner = 0;
    int err = proc_prepare(proc_supervisor(), &owner);
    if (err) { proc_stop(app, 1, 0); proc_collect(); rec_emit(name, "END", "status=FAIL reason=controller_memory"); return 1; }
    proc_publish(owner, 0);
    app->ppid = owner->pid;
    memset(&libc_reports, 0, sizeof(libc_reports)); libc_reports.pid = pid;
    supervisor_observe(name, pid);
    rec_emit(name, "DATA", "case=launch pid=%u pgid=%u abi_version=%u", pid, app->pgid, CIUKI_ABI_VERSION);
    uint64_t deadline = g_ticks + 180000;
    while (app->state != PROC_ZOMBIE && g_ticks < deadline) task_sleep_ms(1);
    bool pass = app->state == PROC_ZOMBIE && app->status == 0 && libc_reports.stages == 3 &&
        !libc_reports.failures && !libc_reports.invalid;
    rec_emit(name, "DATA", "group=libc-smoke checks=%u failures=%u callback_order=%u report_stages=%u invalid_reports=%u",
             libc_reports.checks, libc_reports.failures, libc_reports.stages ? libc_reports.stages + 1 : 0,
             libc_reports.stages, libc_reports.invalid);
    libc_reports.pid = 0;
    rec_emit(name, "DATA", "case=wait pid=%u status=%d timeout=%u", pid, app->state == PROC_ZOMBIE ? app->status : -1, g_ticks >= deadline);
    supervisor_observe_end();
    if (app->state != PROC_ZOMBIE) {
        proc_stop(app, 1, 0);
        uint64_t cleanup_deadline = g_ticks + 1000;
        while (app->state != PROC_ZOMBIE && g_ticks < cleanup_deadline) { task_sleep_ms(1); proc_collect(); }
    }
    if (app->state == PROC_ZOMBIE) proc_reap(owner, app);
    proc_stop(owner, 0, 0); proc_collect();
    rec_emit(name, "END", pass ? "status=PASS" : "status=FAIL reason=application_status");
    return pass ? 0 : 1;
}
CIUKI_F2_PROBE("crash-isolation", probe_f2_crash_isolation);
CIUKI_F2_PROBE("libc-smoke", probe_f2_libc_smoke);
