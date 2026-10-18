/* Slow UC transport under the F0-20 scheduler/cpustat replay. Negative
 * control suppresses safe points with the production preemption hold.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main latency_fixture_main
#include "kernel_latency_test.c"
#undef main
#include "../../src/kernel/vm/vga_hardware.h"
static cvga_state image;
static unsigned bytes,peer_runs;
static bool pumping;
static uint8_t input(void *arg,uint16_t p) { (void)arg;(void)p;return 0; }
static void output(void *arg,uint16_t p,uint8_t b) { (void)arg;(void)p;(void)b; }
static void access_byte(void)
{
    /* 2 us per byte: 131 ms per plane, matching the observed ~133/160 ms
     * callbacks without claiming a hardware timing measurement. */
    cycles+=2;bytes++;
    if (!(bytes%500)) {
        cycles-=1000;run_tick(0xc011aaaa);
    }
}
static uint8_t read_byte(void *arg,unsigned a) { (void)arg;(void)a;access_byte();return 0x5a; }
static void write_byte(void *arg,unsigned a,uint8_t b) { (void)arg;(void)a;(void)b;access_byte(); }
static void dispatch_peer(void)
{
    if (pumping) return;
    pumping=true;
    while (g_current!=&hog) {
        CHECK(g_current==&normal);peer_runs++;run_tick(0x4444);
        task_sleep_ms(1);
    }
    pumping=false;
}
static unsigned replay_transfer(bool cooperative,bool writing)
{
    reset();hog.prio=P_NORMAL;hog.user=normal.user=false;
    bytes=peer_runs=0;ui.state=T_BLOCKED;
    rq_push(&hog);rq_push(&normal);schedule();CHECK(g_current==&hog);
    latency_switch_hook=dispatch_peer;
    struct vga_hardware_bus bus={0,input,output,read_byte,write_byte};
    if (!cooperative) kpreempt_hold();
    if (writing) vga_hardware_load(&image,&bus);
    else vga_hardware_capture(&image,&bus);
    if (!cooperative) kpreempt_release();
    cpu_point();latency_switch_hook=0;
    unsigned maximum=ms(stretch_max);
    CHECK(bytes>=4*CVGA_PLANE_SIZE);
    CHECK(hog.max_stretch<=(cooperative ? 4u : 600u)*per_ms());
    CHECK(normal.max_stretch<=4*per_ms());
    if (cooperative) CHECK(peer_runs>=40); else CHECK(!peer_runs);
    printf("F1-80 VGA %s cooperative=%u bytes=%u stretch_max_ms=%u peer_runs=%u PASS\n",
           writing ? "load" : "capture",cooperative,bytes,maximum,peer_runs);
    return maximum;
}
int main(void)
{
    for (unsigned writing=0;writing<2;writing++) {
        unsigned before=replay_transfer(false,writing),after=replay_transfer(true,writing);
        CHECK(before>=500 && after<=4);
    }
    return 0;
}
