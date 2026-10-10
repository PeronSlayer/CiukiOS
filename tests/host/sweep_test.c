/* Production parser/table and reset sequence tests, without privileged I/O. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/reboot.h>
#include <ciuki/process.h>
#include "selector.h"

static unsigned calls, begins, ends;
extern bool probes_operator_mode;
extern bool probes_operator_wait(bool (*)(void *), uint64_t (*)(uint32_t),
                                void (*)(uint32_t), void (*)(bool), void *);
static uint64_t operator_clock, event_at;
static unsigned announcements, redraws, events;
static uint64_t operator_now(uint32_t ms) { return operator_clock + ms; }
static void operator_sleep(uint32_t ms) { operator_clock += ms; }
static bool operator_event(void *arg)
{
    assert(arg == &events);
    if (operator_clock != event_at) return false;
    events++; return true;
}
static void operator_prompt(bool announce) { if (announce) announcements++; else redraws++; }
static void operator_tests(void)
{
    assert(!probes_operator_mode);
    const unsigned arrival[] = { 0, 10, 59990, 60000, 60010 };
    for (unsigned wrap = 0; wrap < 2; wrap++) {
        for (unsigned i = 0; i < sizeof(arrival)/sizeof(*arrival); i++) {
            operator_clock = wrap ? UINT64_MAX - 100 : 0;
            uint64_t start = operator_clock;
            event_at = start + arrival[i]; announcements = redraws = events = 0;
            bool normal = probes_operator_wait(operator_event, operator_now, operator_sleep, operator_prompt, &events);
            assert(normal == (arrival[i] < 60000));
            assert(announcements == 1 && events == (unsigned)normal);
            assert(operator_clock - start == (normal ? arrival[i] : 60000));
            assert(redraws <= 599);
        }
    }
}
static int pass(void) { calls++; return 0; }
static int fail(void) { calls++; return 1; }
static void panic_probe(void) { calls++; }
static void app_begin(const struct probe_selection *p) { assert(p->phase == 2); begins++; }
static void app_end(void) { ends++; }
static void selection_tests(void)
{
    struct probe_selection s; struct sweep_cursor c;
    const char *good[] = { "f0:sweep run=12345678", "all:sweep run=abcdef12",
        "f1:sweep run=12345678 step=16 state=00041000", "f2:sweep run=12345678 step=0" };
    for (unsigned i=0;i<sizeof(good)/sizeof(*good);i++) {
        assert(sweep_parse(good[i], strlen(good[i]), &c));
        assert(probes_parse_selector(good[i], strlen(good[i]), 0, &s));
        assert(!strcmp(s.probe, "sweep") && !strcmp(s.run, c.run));
    }
    const char *bad[] = { "f0:sweep run=12345678 platform=e500", "f1:sweep run=12345678 safe=1",
        "f2:sweep run=12345678 server=desktop", "all:all run=12345678", "f1:sweep run=12345678 step=64",
        "f1:sweep run=12345678 step=", "f1:sweep run=12345678 step=000", "f1:sweep run=12345678 state=00000000",
        "f1:sweep run=12345678 step=1 state=ffffffff", "f1:fat-write run=12345678 step=1", "f0:sweep run=1234567g" };
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) assert(!probes_parse_selector(bad[i], strlen(bad[i]), 0, &s));
    assert(!sweep_parse(good[0], 65, &c));
    assert(probes_parse_selector("f0:all run=12345678", 19, 0, &s));
}
static void order_tests(void)
{
    const struct probe_def f0[] = {{"boot",pass},{"fpu",fail}};
    const struct probe_def f1[] = {{"registry",pass},{"input",fail},{"fat-write",pass},
        {"cache",pass},{"mount-crash",pass},{"safe",pass},{"bootlog",pass}};
    const struct ciuki_f2_probe f2[] = {{"elf-load",pass},{"fd-table",fail},{"app-gate",pass}};
    const struct probe_tables t = {f0,f1,2,7,f2,3};
    const struct probe_hooks h = {app_begin,app_end,panic_probe};
    const char *expected[] = {"registry","input","cache","safe","fat-write","fat-write",
        "mount-crash","mount-crash","bootlog","bootlog"};
    struct sweep_step step; struct sweep_cursor cursor = {.run="12345678"};
    assert(sweep_count(0,&t)==3 && sweep_count(1,&t)==10 && sweep_count(2,&t)==3 && sweep_count(3,&t)==16);
    for (unsigned i=0;i<10;i++) {
        assert(sweep_at(1,i,&t,&step)); assert(!strcmp(step.name,expected[i]));
        assert(step.boot == (i>=4 ? (i-4)%2 : 0));
        sweep_dispatch(&step,&cursor,&t,&h);
    }
    assert(calls==10); // failure at input did not skip following steps
    assert(sweep_at(0,2,&t,&step) && step.panic && !strcmp(step.name,"panic"));
    assert(sweep_at(3,3,&t,&step) && !strcmp(step.name,"registry"));
    for(unsigned i=0;i<3;i++) { assert(sweep_at(2,i,&t,&step)); sweep_dispatch(&step,&cursor,&t,&h); }
    assert(begins==3 && ends==3 && calls==13);
    assert(!sweep_at(3,16,&t,&step));
}
static unsigned stage, delays, polls; static bool stuck;
static void disable(void *c) { (void)c; assert(stage==0); stage=1; }
static uint8_t status(void *c) { (void)c; assert(stage==1); polls++; return stuck ? 2 : 0; }
static void pulse(void *c) { (void)c; assert(stage==1); stage=2; }
static void delay(void *c) { (void)c; assert(stage==1 || stage==2); delays++; }
static void triple(void *c) { (void)c; assert(stage==(stuck ? 1u : 2u)); assert(delays>=100); stage=3; }
int main(void)
{
    selection_tests(); order_tests(); operator_tests();
    const struct reboot_ops ops = {disable,status,pulse,delay,triple};
    reboot_sequence(&ops,0); assert(stage==3 && polls==1 && delays==100);
    stuck=true; stage=delays=polls=0; reboot_sequence(&ops,0);
    assert(stage==3 && polls==10000 && delays==10100);
    puts("PASS sweep: selectors, stable tables, continue after failure, F2 framing, bounded 8042/triple order");
}
