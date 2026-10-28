/* f3-33: aggregate thread CPU cost on production exits, with fake trap
 * transport. This cannot measure the CPU's ring transition or hardware IRET.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main dosvm_tests_main
#include "dosvm_test.c"
#undef main
#include <time.h>
static uint64_t exit_ns(void)
{
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t));
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
/* -DT23_HEAD_BASELINE: the same cases against a347ca76b sources. */
#ifdef T23_HEAD_BASELINE
#define T23_UNPROVABLE(v) 0u
#else
#define T23_UNPROVABLE(v) ((v)->dpmi.cli.unprovable)
#endif
static void exit_result(const char *name, uint64_t start, unsigned n)
{ printf("exit_bench case=%s iterations=%u avg_ns=%llu\n", name, n, (unsigned long long)((exit_ns()-start)/n)); }
static void __attribute__((noinline)) exit_copy(struct v86_frame *to, const struct v86_frame *from)
{ memcpy(to,from,sizeof(*to)); }
static struct dosvm *exit_vm(bool pm)
{
    struct dosvm *v = pm ? dpmi_vm(true) : create();
    if (pm) {
        v86_start(); enter(v, true);
        client_desc(v, 900, 0, UINT32_MAX, 0xFA, 0x40);
        client_desc(v, 901, 0, UINT32_MAX, 0xF2, 0x40);
        memset(&cur, 0, sizeof(cur));
        cur.tf.cs=DPMI_SEL(900); cur.tf.user_ss=cur.tf.ds=cur.tf.es=DPMI_SEL(901);
        cur.tf.user_esp=0x28000; cur.tf.eflags=V86_IF|2;
    } else {
        CHECK(!dosvm_boot(v)); v86_start();
        cur.tf.cs=0x3000;
    }
    v->info.dos_ready=true;
    v->monitor.pic[0].imr=v->monitor.pic[1].imr=0xFF;
    v->monitor.state=V86_RUNNING;
    return v;
}
static void exit_vga_bench(bool pm)
{
    const unsigned n=20000;
    struct dosvm *v=exit_vm(pm); fast_planar(v);
    v->monitor.vif=false; v->dpmi.if_armed=false; v->dpmi.return_steps=0;
    unsigned address=0x30000;
    const uint8_t byte[]={0xEE,0x90}, word16[]={0xEF,0x90}, word32[]={0x66,0xEF,0x90};
    for (unsigned word=0;word<2;word++) {
        CHECK(!dosvm_write(v,address,word ? (pm ? word32 : word16) : byte,word && pm ? 3 : 2));
        uint64_t start=exit_ns();
        for (unsigned i=0;i<n;i++) {
            cur.tf.eip=pm ? address : 0; cur.tf.edx=word ? 0x3C4 : 0x3C5;
            cur.tf.eax=word ? ((1u<<(i&3))<<8)|2 : 1u<<(i&3);
            step(v,13,0); CHECK(!left);
        }
        exit_result(pm ? (word ? "pm_vga_word_exit" : "pm_vga_byte_exit") :
                    (word ? "rm_vga_word_exit" : "rm_vga_byte_exit"),start,n);
    }
    if (pm) {
        /* fx-27: the steady state of a settled client (fx-16: no CLI seed
         * demand), where map-mask OUTs may take the lean path. */
        bool settled = v->dpmi.cli_settled; uint64_t miss = v->dpmi.cli_miss_tick, ticks = g_ticks;
        v->dpmi.cli_settled = true; v->dpmi.cli_miss_tick = 0; if (g_ticks < 5000) g_ticks = 5000;
        CHECK(!dosvm_write(v,address,byte,sizeof(byte)));
        uint64_t start=exit_ns();
        for (unsigned i=0;i<n;i++) {
            cur.tf.eip=address; cur.tf.edx=0x3C5; cur.tf.eax=1u<<(i&3);
            step(v,13,0); CHECK(!left);
        }
        exit_result("pm_vga_byte_exit_settled",start,n);
#ifndef FX27_HEAD_BASELINE
        /* fx-27: the same settled exits with a cache-cold monitor, as on
         * the T23 where the game's working set evicts the monitor between
         * exits: 4 MiB are walked (one load per 64-byte line) before each
         * exit and timed separately. "ordinary" refuses the lean path with
         * the IPX-binding precondition only. Thread CPU time per exit. */
        static uint8_t polluter[4u << 20];
        /* fx-doomwin: mode 2, the same select retired at trap entry
         * (dosvm_trap_fast, as trap_dispatch offers it first). */
        static const char *const hot[] = {"pm_vga_byte_exit_settled_hot_lean",
            "pm_vga_byte_exit_settled_hot_ordinary", "pm_vga_byte_exit_settled_hot_fast"};
        static const char *const cold_name[] = {"pm_vga_byte_exit_settled_cold_lean",
            "pm_vga_byte_exit_settled_cold_ordinary", "pm_vga_byte_exit_settled_cold_fast"};
        for (unsigned mode = 0; mode < 3; mode++) {
            v->network_attached = mode == 1;
            uint64_t t0 = __builtin_ia32_rdtsc();
            for (unsigned i = 0; i < n; i++) {
                cur.tf.eip=address; cur.tf.edx=0x3C5; cur.tf.eax=1u<<(i&3);
                if (mode == 2) CHECK(step_entry(v)); else step(v,13,0);
                CHECK(!left);
            }
            printf("exit_bench case=%s iterations=%u avg_tsc_cycles=%llu\n", hot[mode], n,
                   (unsigned long long)((__builtin_ia32_rdtsc() - t0) / n));
        }
        for (unsigned mode = 0; mode < 3; mode++) {
            v->network_attached = mode == 1;
            uint64_t total = 0; volatile uint8_t sink = 0;
            const unsigned cold = 2000;
            for (unsigned i = 0; i < cold; i++) {
                for (unsigned at = 0; at < sizeof(polluter); at += 64) sink += polluter[at] + (uint8_t)i;
                cur.tf.eip=address; cur.tf.edx=0x3C5; cur.tf.eax=1u<<(i&3);
                uint64_t t0 = __builtin_ia32_rdtsc();   /* no system call inside the interval */
                bool fast = mode == 2 ? step_entry(v) : (step(v,13,0), false);
                total += __builtin_ia32_rdtsc() - t0;
                CHECK(!left && fast == (mode == 2));
            }
            (void)sink;
            printf("exit_bench case=%s iterations=%u avg_tsc_cycles=%llu\n", cold_name[mode], cold,
                   (unsigned long long)(total / cold));
        }
        v->network_attached = false;
#endif
        v->dpmi.cli_settled = settled; v->dpmi.cli_miss_tick = miss; g_ticks = ticks;
    }
    CHECK(!dosvm_write(v,address,byte,sizeof(byte)));
    tsc_clock_hook=exit_ns; g_tsc_per_ms=1000000;
    uint64_t clocked=exit_ns();
    for (unsigned i=0;i<n;i++) {
        cur.tf.eip=pm ? address : 0; cur.tf.edx=0x3C5; cur.tf.eax=1u<<(i&3);
        step(v,13,0); CHECK(!left);
    }
    exit_result(pm ? "pm_vga_byte_exit_clocked" : "rm_vga_byte_exit_clocked",clocked,n);
    tsc_clock_hook=0; g_tsc_per_ms=0;
    port_out(v,0x3CE,1,4);
    for (unsigned word=0;word<2;word++) {
        CHECK(!dosvm_write(v,address,word ? (pm ? word32 : word16) : byte,word && pm ? 3 : 2));
        uint64_t begin=exit_ns();
        for (unsigned i=0;i<n;i++) {
            cur.tf.eip=pm ? address : 0; cur.tf.edx=word ? 0x3CE : 0x3CF;
            cur.tf.eax=word ? ((i&3)<<8)|4 : i&3;
            step(v,13,0); CHECK(!left);
        }
        exit_result(pm ? (word ? "pm_gc_word_exit" : "pm_gc_byte_exit") :
                    (word ? "rm_gc_word_exit" : "rm_gc_byte_exit"),begin,n);
    }
    port_out(v,0x3C4,2,0x0102);
    /* Component measurements share active CR3 and the production PTE batch.
     * No host clock syscall inside these loops; they measure C/host fakes. */
    cpu_cr3=v->as.pd_phys;
    uint64_t start=exit_ns();
    for (unsigned i=0;i<n;i++) { v->video_writable=true; video_exchange(v,(i&3)+1); }
    exit_result(pm ? "pm_vga_pte_batch_only" : "rm_vga_pte_batch_only",start,n);
    start=exit_ns();
    for (unsigned i=0;i<n;i++) { uint32_t value=1u<<(i&3); CHECK(!dosvm_io(v,0x3C5,1,true,&value)); }
    exit_result(pm ? "pm_vga_dispatch_remap" : "rm_vga_dispatch_remap",start,n);
    CHECK(!dosvm_write(v,address,byte,sizeof(byte)));
    start=exit_ns();
    for (unsigned i=0;i<n;i++) {
        struct v86_frame f=cur; f.tf.eip=pm ? address : 0; f.tf.edx=0x3C5; f.tf.eax=1u<<(i&3);
        if (pm) {
            struct dpmi_seg cs; CHECK(dpmi_seg(v,f.tf.cs,&cs));
            CHECK(!emulate(v,&f));
        } else CHECK(!v86_emulate(&v->monitor,&f));
    }
    exit_result(pm ? "pm_vga_decode_dispatch_remap" : "rm_vga_decode_dispatch_remap",start,n);
    /* INC DX sequence has one native MOV AL and two OUTs at the boundary.
     * The control binary executes the unbatched second cycle explicitly. */
    const uint8_t sequence[]={0xEE,0xB0,8,0x42,0xEE,0x90};
    CHECK(!dosvm_write(v,address,sequence,sizeof(sequence)));
    start=exit_ns(); unsigned exits=0;
    for (unsigned i=0;i<n;i++) {
        unsigned ip=pm ? address : 0;
        cur.tf.eip=ip; cur.tf.edx=0x3C4; cur.tf.eax=2;
        step(v,13,0); exits++;
        if (cur.tf.eip==ip+1) {
            cur.tf.eax=8; cur.tf.edx=0x3C5; cur.tf.eip=ip+4;
            step(v,13,0); exits++;
        }
        CHECK(!left && cur.tf.eip==ip+5);
    }
    exit_result(pm ? "pm_vga_index_inc_data_sequence" : "rm_vga_index_inc_data_sequence",start,n);
    printf("exit_bench vga_sequence=%s exits_per_sequence=%.2f\n",pm ? "pm" : "rm",(double)exits/n);
    cpu_cr3=0; destroy(v,true); empty();
}
int main(void)
{
    const unsigned n=20000;
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram);
    struct dosvm *v=exit_vm(false);
    struct v86_frame copied;
    uint64_t copy_start=exit_ns();
    for (unsigned i=0;i<n;i++) { cur.tf.eax=i; exit_copy(&copied,&cur); exit_copy(&cur,&copied); }
    CHECK(cur.tf.eax==n-1);
    exit_result("frame_save_restore_only",copy_start,n);
    uint8_t code[]={0xE6,0x43,0xE4,0x40,0x88,0xC4,0xE4,0x40,0x90};
    CHECK(!dosvm_write(v,0x30000,code,sizeof(code)));
    uint64_t start=exit_ns();
    for (unsigned i=0;i<n;i++) {
        cur.tf.eip=0; cur.tf.eax=0;
        step(v,13,0);
        if (cur.tf.eip==2) step(v,13,0);
        if (cur.tf.eip==4) { cur.tf.eax=(cur.tf.eax&~0xFF00u)|((cur.tf.eax&255)<<8); cur.tf.eip+=2; }
        if (cur.tf.eip==6) step(v,13,0);
        CHECK(cur.tf.eip==8 && !left);
    }
    exit_result("rm_pit_latch_two_reads",start,n);
    uint8_t pic[]={0xE6,0x20,0x90}; CHECK(!dosvm_write(v,0x30000,pic,sizeof(pic)));
    start=exit_ns();
    for (unsigned i=0;i<n;i++) { cur.tf.eip=0; cur.tf.eax=0x20; step(v,13,0); CHECK(!left); }
    exit_result("rm_pic_eoi_exit",start,n);
    start=exit_ns();
    for (unsigned i=0;i<n;i++) { uint32_t value=0x20; CHECK(!v86_io(&v->monitor,0x20,1,true,&value)); }
    exit_result("pic_dispatch_only",start,n);
    uint8_t intr[]={0xCD,0x1C,0x90}, iret=0xCF; uint16_t vec[]={0,0x50};
    CHECK(!dosvm_write(v,0x30000,intr,sizeof(intr)) && !dosvm_write(v,0x500,&iret,1) && !dosvm_write(v,0x70,vec,4));
    start=exit_ns();
    for (unsigned i=0;i<n;i++) { cur.tf.cs=0x3000; cur.tf.eip=0; step(v,13,0); step(v,13,0); CHECK(cur.tf.cs==0x3000 && cur.tf.eip==2 && !left); }
    exit_result("rm_int_iret_pair",start,n);
    start=exit_ns();
    for (unsigned i=0;i<n;i++) { struct v86_frame f=cur; CHECK(!v86_reflect(&v->monitor,&f,0x1C)); }
    exit_result("rm_reflection_only",start,n);
    start=exit_ns();
    for (unsigned i=0;i<n;i++) { struct v86_frame f=cur; v86_prepare_return(&v->monitor,&f); CHECK(f.tf.cs==cur.tf.cs); }
    exit_result("rm_resume_only",start,n);
    destroy(v,true); empty();
    v=exit_vm(true);
    uint8_t walk[515]; memset(walk,0x90,sizeof(walk)); walk[512]=0x9C; walk[513]=0x58; walk[514]=0xFA;
    CHECK(!dosvm_write(v,0x30000,walk,sizeof(walk)));
    struct dpmi_seg cs; CHECK(dpmi_seg(v,cur.tf.cs,&cs));
    v->dpmi.decode_valid=true; v->dpmi.decode_cs=cur.tf.cs; v->dpmi.decode_eip=0x30000;
    cur.tf.eip=0x30202;
    start=exit_ns();
    for (unsigned i=0;i<n/10;i++) {
        uint8_t pop=0; unsigned width=0;
        CHECK(flags_preceding(v,&cur,&cs,&pop,&width,false) && pop==0x58 && width==4);
    }
    exit_result("pm_flags_decode_only",start,n/10);
    for (unsigned armed=0;armed<2;armed++) {
        start=exit_ns();
        for (unsigned i=0;i<n/10;i++) {
            v->monitor.vif=true; v->dpmi.if_armed=false; v->dpmi.decode_valid=true;
            v->dpmi.decode_cs=cur.tf.cs; v->dpmi.decode_eip=0x30000;
            cur.tf.eip=0x30202; cur.tf.eax=armed ? V86_IF|2 : 0;
            step(v,13,0); CHECK(!left && v->dpmi.if_armed==!!armed);
        }
        exit_result(armed ? "pm_flags_proved_save_exit" : "pm_flags_no_image_exit",start,n/10);
    }
    destroy(v,true); empty();
    /* t23-dosperf: PM CLI/STI of a settled client (no seed demand), lean
     * against ordinary (the IPX-binding precondition only), TSC cycles per
     * exit; then Warcraft II's event-loop CLI (flags saved to a local:
     * PUSHFW; POP AX; MOV EDX,1; MOV [EBP-4],AX; MOV [EBP-0Ch],EDX; CLI),
     * a candidate that no proof can arm. */
    v=exit_vm(true);
    v->dpmi.cli_settled=true; v->dpmi.cli_miss_tick=0; if (g_ticks<5000) g_ticks=5000;
    static const uint8_t flags_code[]={0xFA,0xFB,0x90,
        0x66,0x9C,0x66,0x58,0xBA,0x01,0x00,0x00,0x00,0x66,0x89,0x45,0xFC,0x89,0x55,0xF4,0xFA,0x90};
    CHECK(!dosvm_write(v,0x30000,flags_code,sizeof(flags_code)));
    for (unsigned mode=0;mode<2;mode++) {
        v->network_attached=mode==1;
        uint64_t t0=__builtin_ia32_rdtsc();
        for (unsigned i=0;i<n;i++) {
            v->monitor.vif=true; cur.tf.eip=0x30000; step(v,13,0); CHECK(!left && !v->monitor.vif);
            step(v,13,0); CHECK(!left && v->monitor.vif && cur.tf.eip==0x30002);
        }
        printf("exit_bench case=%s iterations=%u avg_tsc_cycles=%llu\n", mode ? "pm_cli_sti_settled_ordinary" :
               "pm_cli_sti_settled_lean", 2*n, (unsigned long long)((__builtin_ia32_rdtsc()-t0)/(2*n)));
    }
    v->network_attached=false;
    unsigned missed=v->dpmi.cli.missed;
    uint64_t t0=__builtin_ia32_rdtsc();
    for (unsigned i=0;i<n;i++) {
        v->monitor.vif=true; cur.tf.eip=0x30013;
        cur.tf.eax=(cur.tf.eax&0xFFFF0000u)|(cur.tf.eflags&0x0CD5u)|V86_IF|2u;
        step(v,13,0); CHECK(!left && !v->monitor.vif && !v->dpmi.if_armed);
    }
    printf("exit_bench case=pm_cli_war2_event_loop iterations=%u avg_tsc_cycles=%llu missed=%u unprovable=%u\n", n,
           (unsigned long long)((__builtin_ia32_rdtsc()-t0)/n), v->dpmi.cli.missed-missed, T23_UNPROVABLE(v));
#ifndef T23_HEAD_BASELINE
    CHECK(v->dpmi.cli.missed==missed);
#endif
    destroy(v,true); empty();
    exit_vga_bench(false); exit_vga_bench(true);
    free(ram);
    return 0;
}
