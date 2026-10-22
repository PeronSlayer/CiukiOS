/* Runtime f3-51 controller. Results remain unchanged guest hex records.
 * Fresh VM/COW per case makes all initial files/handles identical. No fixture
 * or runtime evidence is synthesized. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/storage.h>
#include <ciuki/bootlog.h>
#include "selector.h"
#include "../vm/dosvm_internal.h"
#include "../vm/dos_file.h"

static struct vfs_table plan_table;
static char plan[4096];
static volatile bool log_stop;
static void d51_log_writer(void *arg)
{
    (void)arg;
    while (!log_stop) { (void)bootlog_drain();task_sleep_ms(10); }
    task_exit(0);
}
struct d51_case { char tag[7]; unsigned results; bool bad,ready,shell,pass; };

static void d51_observe(struct dosvm *v,const char *line,unsigned n,void *arg)
{
    (void)v;
    struct d51_case *state=arg;
    if (n==12 && !memcmp(line,"DOSKEY READY",12)) { state->ready=true; return; }
    if (n==9 && !memcmp(line,"D48 SHELL",9)) {
        if (state->results!=1 || state->shell) state->bad=true;
        state->shell=true; return;
    }
    if (n<3 || memcmp(line,"D48",3)) return;
    if (n!=16 || memcmp(line,state->tag,6) || line[6]!=' ' || state->results++) {
        state->bad=true; return;
    }
    if (!memcmp(line+7,"PASS 0000",9)) state->pass=true;
    else if (memcmp(line+7,"FAIL ",5)) state->bad=true;
}
static int d51_type(struct dosvm *v,const char *text)
{
    uint64_t until=deadline_after_ms(10000);
    for (unsigned i=0;;i++) {
        uint16_t key=text[i] ? (uint8_t)text[i] : 0x1c0d;
        while (!dosvm_keyboard(dosvm_key(v),key)) {
            if (deadline_passed(until) || !dosvm_valid(dosvm_key(v))) return -V86_ETIMEDOUT;
            task_sleep_ms(2);
        }
        if (!text[i]) return 0;
    }
}
static int d51_wait(struct dosvm *v,struct d51_case *state,unsigned phase,bool keys)
{
    uint64_t until=deadline_after_ms(30000);
    bool injected=false;
    for (;;) {
        struct dosvm_info info; dosvm_info(v,&info);
        if (state->bad || info.dropped_records || info.state>=DOSVM_STOPPING) return -V86_EIO;
        if (phase==0 ? info.dos_ready : phase==1 ? state->results==1 : state->shell) return 0;
        if (keys && state->ready && !injected) {
            if (!dosvm_keyboard(dosvm_key(v),0x2d58) || !dosvm_keyboard(dosvm_key(v),0x1559)) return -V86_EIO;
            injected=true;
        }
        if (deadline_passed(until)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
}
static bool d51_same(const struct dosvm_ledger *a,const struct dosvm_ledger *b)
{
    return a->vms==b->vms && a->tasks==b->tasks && a->pages==b->pages &&
        a->handles==b->handles && a->devices==b->devices &&
        a->reserved==b->reserved && a->resident==b->resident;
}
static void d51_line(const char *line)
{
    serial_write(line,strlen(line)); /* whole bounded controller record */
}
static void d51_log_pause(void *arg) { (void)arg;task_sleep_ms(1); }
static int d51_run(void)
{
    struct probe_selection selected;
    if (!probes_parse_selector(g_boot.test_request,g_boot.test_request_len,g_boot.flags,&selected)) return 1;
    /* All historical runtime selectors map to unified. The pristine
     * reference is confined to the host conformance harness. */
    const char *base="C:\\SYSTEM\\DOS\\UNIFIED.IMG";
    const char *sidecar="C:\\SYSTEM\\DOS\\UNIFIED.SHA";
    const char *guest="ciukidos";
    const char *name="dos-conformance";
    rec_emit(name,"BEGIN","guest=%s adapter=1 kernel=unified",guest);
    char message[160];
    ksnprintf(message,sizeof(message),"D48 BEGIN run=%s guest=%s\n",selected.run,guest); d51_line(message);
    struct storage *storage=storage_get();
    /* Selected probes start read-only and without the normal storage writer.
     * Open only the canonical overlay's existing write gate; log draining is
     * owned/joined by this controller, never by the VM or an IRQ callback. */
    int error=!storage || !storage->ready ? -V86_EIO : storage_enable_write(storage,2);
    if (!error) error=vfs_table_init(&storage->vfs,&plan_table,2);
    int amount=0;
    if (!error) {
        {
            struct fat_entry entry;
            error=vfs_stat(&plan_table,base,&entry);
            if (!error && (entry.attr&0x10)) error=-EINVAL;
            if (!error) error=vfs_stat(&plan_table,sidecar,&entry);
            if (error) {
                vfs_table_destroy(&plan_table);
                rec_emit(name,"END","status=FAIL reason=unified_absent_or_corrupt error=%d",error);
                return 1;
            }
        }
        int fd=vfs_open(&plan_table,"C:\\SYSTEM\\DOS\\D48PLAN.TXT",VFS_READ,VFS_DENY_WRITE,0);
        if (fd<0) error=fd;
        else {
            size_t got=0;
            error=vfs_read(&plan_table,fd,plan,sizeof(plan),&got);
            amount=(int)got;
            int closed=vfs_close(&plan_table,fd);
            if (error || amount<=0 || amount>=(int)sizeof(plan) || closed) error=-V86_EIO;
        }
        vfs_table_destroy(&plan_table);
    }
    if (error) { rec_emit(name,"END","status=FAIL reason=missing_plan error=%d",error); return 1; }
    error=bootlog_activate(&storage->vfs,true,storage->sequence);
    struct task *writer=0;
    if (!error) {
        log_stop=false;writer=task_create_kernel("d48-log",d51_log_writer,0,P_DEVICE);
        if (!writer) error=-ENOMEM;
        else task_start(writer);
    }
    if (error) { rec_emit(name,"END","status=FAIL reason=log_writer error=%d",error);return 1; }
    unsigned count=0,at=0;
    struct dosvm_ledger baseline,after; dosvm_snapshot(&baseline);
    while (at<(unsigned)amount && !error) {
        unsigned end=at; while (end<(unsigned)amount && plan[end]!='\n') end++;
        if (end==(unsigned)amount || end-at<8 || end-at>24) { error=-EINVAL; break; }
        plan[end]=0;
        static struct d51_case state;
        memset(&state,0,sizeof(state));
        ksnprintf(state.tag,sizeof(state.tag),"D48%03u",count+1);
        if (memcmp(plan+at,state.tag,6) || plan[at+6]!=' ') { error=-EINVAL; break; }
        const char *fixture=plan+at+7;
        bool keys=!strncmp(fixture,"keys-XY",8);
        unsigned fault=!strncmp(fixture,"fault-once",11) ? 1 : !strncmp(fixture,"fault-always",13) ? 2 : 0;
        if (strncmp(fixture,"basic",6) && strncmp(fixture,"share",6) && !keys && !fault) { error=-EINVAL;break; }
        struct dosvm *v=0;
        const struct dosvm_config config={.probe=name,.dpmi=true,.kernel=2,
            .disable_file_bridge=true,.base=base,
            .record_observer=d51_observe,.record_arg=&state};
        error=dosvm_create(&config,&v);
        if (!error) d48_reset(&v->conformance_media,fault,dosvm_key(v).generation);
        if (!error) error=dosvm_boot(v);
        if (!error) error=d51_wait(v,&state,0,false);
        if (!error && v->files) error=-V86_EIO;
        if (!error) {
            char command[64];
            ksnprintf(command,sizeof(command),"CD \\D48TEST\\%s",state.tag);
            error=d51_type(v,command);
            if (!error) { task_sleep_ms(400); ksnprintf(command,sizeof(command),"\\D48\\%s.COM",state.tag); error=d51_type(v,command); }
        }
        if (!error) error=d51_wait(v,&state,1,keys);
        if (!error) { task_sleep_ms(100); error=d51_type(v,"\\D48\\D48DONE.COM"); }
        if (!error) error=d51_wait(v,&state,2,false);
        if (!error && (!state.pass || state.results!=1)) error=-V86_EIO;
        if (v) {
            if (fault && (v->conformance_media.bad || v->conformance_media.opens!=1 ||
                v->conformance_media.failures!=D48_BIOS_RETRIES)) error=-V86_EIO;
            if (fault) rec_emit(name,"DATA","group=d48_fault case=%s opens=%u failures=%u generation=%u",
                state.tag,v->conformance_media.opens,v->conformance_media.failures,v->conformance_media.generation);
            if (v->files && (v->files->dropped || v->files->cleanup_status)) error=-V86_EIO;
            struct dosvm_info info; dosvm_info(v,&info);
            if (info.dropped_records || info.cleanup_error) error=-V86_EIO;
            int stopped=dosvm_stop(dosvm_key(v),DOSVM_EXIT);
            int destroyed=dosvm_destroy(dosvm_key(v),false);
            if (stopped || destroyed) error=error ? error : -V86_EIO;
        }
        dosvm_snapshot(&after);
        if (!d51_same(&baseline,&after)) error=-V86_EIO;
        if (!error) count++;
        else rec_emit(name,"ERROR","case=%s status=FAIL reason=case_or_teardown error=%d",state.tag,error);
        at=end+1;
        task_sleep_ms(1000); /* ordinary guest record budget is unchanged */
    }
    if (!count) error=-V86_EIO;
    if (!error) {
        struct bootlog_stats log; bootlog_snapshot(&log);
        if (!log.active || log.error)
            error=-V86_EIO;
        if (!error && rec_sequence_ledger) rec_sequence_ledger(name);
        else error=-V86_EIO;
        if (!error) error=bootlog_sync(0,d51_log_pause,0);
    }
    log_stop=true;
    uint64_t stopped=deadline_after_ms(6000);
    while (task_alive(writer) && !deadline_passed(stopped)) task_sleep_ms(1);
    if (task_alive(writer)) error=-V86_ETIMEDOUT;
    else task_reap(writer);
    if (!error) {
        ksnprintf(message,sizeof(message),"D48 CLEAN run=%s guest=%s handles=0 searches=0 locks=0 vms=0 dropped=0\n",selected.run,guest);
        d51_line(message);
        ksnprintf(message,sizeof(message),"D48 END run=%s guest=%s count=%u\n",selected.run,guest,count); d51_line(message);
    }
    rec_emit(name,"END","status=%s count=%u error=%d",error ? "FAIL" : "PASS",count,error);
    return error ? 1 : 0;
}
CIUKI_F3_PROBE("dos-conformance",d51_run);
