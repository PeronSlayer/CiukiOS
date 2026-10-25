/* Production VFS budget, VM producers, formatter and boot-log slot queue.
 * Stall the drain for five seconds; no device or guest image is needed.
 * SPDX-License-Identifier: GPL-2.0-only */
#define F186_BUDGET
#include "dosvm_perf_irq_test.c"
#include <ciuki/arch.h>
#include "../../src/kernel/fs/vfs.h"

static void cli(void) { cpu_flags=0; }
static void hlt(void) { abort(); }
static uint32_t read_cr2(void) { return 0; }
static unsigned hold_rows,wait_rows,summary_rows,serial_rows;
static unsigned held_observed,waited_observed;
static unsigned serial_bytes;
static bool reenter;
static struct vfs budget_vfs;
static char summaries[1024];
uint32_t fs_host_clock_ms(void) { return (uint32_t)g_ticks; }
void serial_write(const char *s,size_t n) {
    serial_rows++; serial_bytes+=(unsigned)n;
    if (!strncmp(s,"[storage] vfs_hold ",19)) hold_rows++;
    if (!strncmp(s,"[storage] vfs_wait ",19)) wait_rows++;
    if (!strncmp(s,"[storage] vfs_contention ",25)) {
        CHECK(strlen(summaries)+n<sizeof(summaries));
        strncat(summaries,s,n); summary_rows++;
    }
    /* Logger reentry cannot retain a diagnostic counter or namespace lock. */
    if (reenter) { reenter=false; vfs_lock(&budget_vfs,0); vfs_unlock(&budget_vfs); }
}
void console_write(const char *s,size_t n) { (void)s;(void)n; }
void vfs_hold_observer(char d,uint32_t ms,const struct vfs_hold_info *h,uint32_t w) {
    CHECK((d=='C' || d=='D') && ms>=50 && !w && h->task.name[0]); held_observed++;
}
void vfs_wait_observer(char d,uint32_t ms,char holder,const char *reason,
                       const struct fs_task_identity *task,const struct vfs_hold_info *h) {
    CHECK((d=='C' || d=='D') && ms>=100 && holder=='C' && !strcmp(reason,"volume"));
    CHECK(task->id==29 && h->task.id==17); waited_observed++;
}
#include "../../src/kernel/core/output.c"
#include "../../src/kernel/core/bootlog.c"

static struct dosvm guests[4];
static void contention(char drive,unsigned n,unsigned duration) {
    for (unsigned i=0;i<n;i++) {
        vfs_lock(&budget_vfs,1u<<(drive-'A'));
        budget_vfs.holds[drive-'A'].started=(uint32_t)g_ticks-duration;
        budget_vfs.diagnostic_pending=(struct vfs_pending_waits){.count=1,.records={{
            .drive=drive,.holder='C',.reason="volume",.ms=duration,
            .task={.name="files",.id=29},.owner={.task={.name="log_writer",.id=17},.op="write"}}}};
        vfs_unlock(&budget_vfs);
    }
}
int main(void) {
    reset(); g_ticks=0; g_boot.flags=CBI_F_DIAG_VERBOSE;
    bootlog_reset(); vfs_init(&budget_vfs);
    struct vfs_request r={.task={.name="log_writer",.id=17},.operation="write"};
    struct fs_request_scope scope; fs_request_enter(&scope,&r);
    for (unsigned vm_id=0;vm_id<4;vm_id++) {
        seed(true,0); guests[vm_id]=vm;
        guests[vm_id].perf.serial.tick=0;
        guests[vm_id].info.key.id=35+vm_id; guests[vm_id].info.key.generation=7;
        memcpy(guests[vm_id].perf.serial.ports[0],guests[vm_id].perf.interval[0].ports,
               sizeof(guests[vm_id].perf.serial.ports[0]));
    }
    perf_current=0; g_ticks=2000;
    for (unsigned i=0;i<4;i++) dosvm_perf_report(&guests[i]);
    reenter=true; contention('C',12,150);
    CHECK(!reenter && hold_rows==8 && wait_rows==8 && held_observed==8 && waited_observed==8);
    contention('C',1,1415); /* suppressed maxima must remain observable */
    g_ticks=3999; contention('C',1,200);
    CHECK(hold_rows==8 && wait_rows==8 && !summary_rows);
    g_ticks=4000;
    for (unsigned i=0;i<4;i++) memcpy(guests[i].perf.serial.ports[1],guests[i].perf.interval[0].ports,
                                    sizeof(guests[i].perf.serial.ports[1]));
    for (unsigned i=0;i<4;i++) dosvm_perf_report(&guests[i]);
    contention('C',10,200);
    CHECK(hold_rows==16 && wait_rows==16 && summary_rows==1);
    CHECK(strstr(summaries,"drive=C hold_suppressed=6 wait_suppressed=6 hold_max_ms=1415 wait_max_ms=1415\n"));
    g_ticks=5000;
    for (unsigned i=0;i<4;i++) { dosvm_perf_report(&guests[i]); CHECK(guests[i].perf.sample_count==1); }
    struct bootlog_stats s; bootlog_snapshot(&s);
    CHECK(!s.storage_calls && !s.dropped_protected && !s.dropped_verbose);
    CHECK(record_count<=BOOTLOG_RECORDS && used<=BOOTLOG_RAM && !s.boot_queued);
    CHECK(serial_rows==73 && hold_rows==16 && wait_rows==16); /* 4*5*2 + 32 + summary */
    /* 45 disk rows per VM bank: t23-dosperf added vm_perf_presenter (was 44). */
    CHECK(record_count==253 && record_count==serial_rows+4*45);
    unsigned verbose=0; for (unsigned i=head;i!=NONE;i=records[i].next) verbose+=records[i].verbose;
    CHECK(verbose==16); /* waits never promote to protected disk slots */
    printf("PASS f1-86 stalled drain 5s: four VMs, slots=%u/%u bytes=%u/%u UART=%u B protected=%u verbose=%u drops=0 disk_families=180 serial_rows=%u\n",
           record_count,BOOTLOG_RECORDS,used,BOOTLOG_RAM,serial_bytes,record_count-verbose,verbose,serial_rows);
    /* Per-drive independence and next-boundary summary, no duplicate replay. */
    contention('D',1,175); CHECK(hold_rows==17 && wait_rows==17);
    g_ticks=6000; vfs_lock(&budget_vfs,0); vfs_unlock(&budget_vfs);
    CHECK(summary_rows==2 && strstr(summaries,"drive=C hold_suppressed=2 wait_suppressed=2 hold_max_ms=200 wait_max_ms=200\n"));
    vfs_lock(&budget_vfs,0); vfs_unlock(&budget_vfs); CHECK(summary_rows==2);
    fs_request_leave(&scope); vfs_destroy(&budget_vfs);
    /* An ordinary boot has no verbose waits on disk or serial. */
    bootlog_reset(); vfs_init(&budget_vfs); g_boot.flags=0;
    unsigned old_wait_rows=wait_rows; contention('C',1,150);
    bootlog_snapshot(&s); CHECK(wait_rows==old_wait_rows && record_count==1 && !records[head].verbose);
    vfs_destroy(&budget_vfs);
    bootlog_reset(); vfs_init(&budget_vfs); g_boot.flags=CBI_F_DIAG_VERBOSE;
    g_ticks=UINT32_MAX-999u;
    unsigned old_holds=hold_rows,old_waits=wait_rows,old_summaries=summary_rows;
    contention('C',9,150); g_ticks+=1999; contention('C',1,150);
    CHECK(hold_rows==old_holds+8 && wait_rows==old_waits+8 && summary_rows==old_summaries);
    g_ticks++; contention('C',1,150);
    CHECK(hold_rows==old_holds+9 && wait_rows==old_waits+9 && summary_rows==old_summaries+1);
    vfs_destroy(&budget_vfs);
    puts("PASS f1-86 contention: 8 holds + 8 waits/drive/2s, rollover maxima and suppression, drive isolation, unlocked reentry, verbose-only waits");
    puts("PASS f1-86 contention budget: exact 1999/2000ms rollover across uint32 clock wrap");
    return 0;
}
