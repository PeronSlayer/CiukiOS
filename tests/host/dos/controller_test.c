/* H51 controller observation/input/deadline with production routines.
 * Host fakes supply time and VM scheduling only; no guest result is evidence.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <string.h>
#include <ciuki/probe.h>
#undef CIUKI_F3_PROBE
#define CIUKI_F3_PROBE(id,func)
static int d51_run(void) __attribute__((unused));
#include "../../../src/kernel/probes/f3_probes_dos_conformance.c"
static uint64_t ticks;
static unsigned injected,sleeps;
static bool stopped;
static struct d51_case *active;
static struct dosvm *fake_vm=(struct dosvm *)1;
uint64_t deadline_after_ms(uint32_t ms) { return ticks+ms; }
bool deadline_passed(uint64_t at) { return ticks>=at; }
struct dosvm_key dosvm_key(const struct dosvm *v)
{ assert(v==fake_vm); return (struct dosvm_key){1,7}; }
void dosvm_info(const struct dosvm *v,struct dosvm_info *info)
{ assert(v==fake_vm); memset(info,0,sizeof(*info));info->dos_ready=true;info->state=stopped ? DOSVM_STOPPING : DOSVM_RUNNABLE; }
bool dosvm_keyboard(struct dosvm_key key,uint16_t value)
{
    assert(key.id==1 && key.generation==7 && active->ready);
    assert(value==(injected ? 0x1559 : 0x2d58));injected++;
    if (injected==2) d51_observe(fake_vm,"D48001 PASS 0000",16,active);
    return true;
}
void task_sleep_ms(uint32_t ms)
{
    ticks+=ms;sleeps++;
    if (sleeps==2 && active) d51_observe(fake_vm,"DOSKEY READY",12,active);
}
int main(void)
{
    struct d51_case state={.tag="D48001"};active=&state;
    assert(!d51_wait(fake_vm,&state,0,false));
    assert(!d51_wait(fake_vm,&state,1,true));
    assert(injected==2 && sleeps>=2 && state.results==1 && state.pass && !state.bad);
    d51_observe(fake_vm,"D48 SHELL",9,&state);assert(state.shell && !state.bad);
    assert(!d51_wait(fake_vm,&state,2,false));
    d51_observe(fake_vm,"D48001 PASS 0000",16,&state);assert(state.bad);
    memset(&state,0,sizeof(state));memcpy(state.tag,"D48001",7);
    d51_observe(fake_vm,"D48002 PASS 0000",16,&state);assert(state.bad);
    memset(&state,0,sizeof(state));memcpy(state.tag,"D48001",7);
    d51_observe(fake_vm,"D48 SHELL",9,&state);assert(state.bad);
    memset(&state,0,sizeof(state));memcpy(state.tag,"D48001",7);
    active=0;uint64_t before=ticks;
    assert(d51_wait(fake_vm,&state,1,false)==-V86_ETIMEDOUT && ticks-before==30000);
    stopped=true;assert(d51_wait(fake_vm,&state,1,false)==-V86_EIO);
}
