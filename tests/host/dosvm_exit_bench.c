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
    destroy(v,true); empty(); free(ram);
    return 0;
}
