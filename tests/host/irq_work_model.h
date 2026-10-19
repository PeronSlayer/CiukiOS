/* Instrument executed production paths, not host wall time. Conservative
 * replay model: 40 ns/basic block, 4 ns/scalar byte, 100 ns/division,
 * 1 us/device register operation. Physical qualification remains separate.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define MODEL_HOOK __attribute__((no_instrument_function,no_sanitize("coverage")))
struct model_target { void *fn; const char *name; unsigned limit, calls; uint64_t maximum, count10, count1000;
    uint64_t catchup_total, catchup_max; unsigned catchup_calls; };
static struct model_target model_targets[24];
static unsigned model_target_count, model_depth, model_phase;
MODEL_HOOK void irq_model_phase(unsigned) __attribute__((weak));
MODEL_HOOK void irq_model_phase(unsigned count) { model_phase=count; }
static struct { unsigned target; uint64_t blocks, bytes, divides, ports; } model_frames[24];
static void *model_io[16];
static unsigned model_io_count;
MODEL_HOOK void __sanitizer_cov_trace_pc_guard_init(uint32_t *a,uint32_t *b)
{ for(;a!=b;a++)*a=1; }
MODEL_HOOK void __sanitizer_cov_trace_pc_guard(uint32_t *g)
{ (void)g;for(unsigned i=0;i<model_depth;i++)model_frames[i].blocks++; }
#define MODEL_ACCESS(n) \
 MODEL_HOOK void __sanitizer_cov_load##n(void *p) { (void)p;for(unsigned i=0;i<model_depth;i++)model_frames[i].bytes+=n; } \
 MODEL_HOOK void __sanitizer_cov_store##n(void *p) { (void)p;for(unsigned i=0;i<model_depth;i++)model_frames[i].bytes+=n; }
MODEL_ACCESS(1) MODEL_ACCESS(2) MODEL_ACCESS(4) MODEL_ACCESS(8) MODEL_ACCESS(16)
MODEL_HOOK void __sanitizer_cov_trace_div4(uint32_t n)
{ (void)n;for(unsigned i=0;i<model_depth;i++)model_frames[i].divides++; }
MODEL_HOOK void __sanitizer_cov_trace_div8(uint64_t n)
{ (void)n;for(unsigned i=0;i<model_depth;i++)model_frames[i].divides++; }
MODEL_HOOK void __cyg_profile_func_enter(void *fn,void *caller)
{
    (void)caller;
    for(unsigned i=0;i<model_io_count;i++)if(fn==model_io[i])
        for(unsigned j=0;j<model_depth;j++)model_frames[j].ports++;
    for(unsigned i=0;i<model_target_count;i++)if(fn==model_targets[i].fn) {
        if(model_depth==24)abort();
        model_frames[model_depth].target=i;
        model_frames[model_depth].blocks=model_frames[model_depth].bytes=0;
        model_frames[model_depth].divides=model_frames[model_depth].ports=0;
        model_depth++;break;
    }
}
MODEL_HOOK void __cyg_profile_func_exit(void *fn,void *caller)
{
    (void)caller;
    if(!model_depth)return;
    unsigned n=model_depth-1, id=model_frames[n].target;
    if(fn!=model_targets[id].fn)return;
    uint64_t ns=model_frames[n].blocks*40+model_frames[n].bytes*4+
        model_frames[n].divides*100+model_frames[n].ports*1000;
    if(ns>model_targets[id].maximum)model_targets[id].maximum=ns;
    if(model_phase==10 && ns>model_targets[id].count10)model_targets[id].count10=ns;
    if(model_phase==1000 && ns>model_targets[id].count1000)model_targets[id].count1000=ns;
    if(model_phase==40) {
        model_targets[id].catchup_total+=ns; model_targets[id].catchup_calls++;
        if(ns>model_targets[id].catchup_max)model_targets[id].catchup_max=ns;
    }
    model_targets[id].calls++;model_depth--;
}
MODEL_HOOK static void model_target(void *fn,const char *name,unsigned limit)
{ model_targets[model_target_count++]=(struct model_target){.fn=fn,.name=name,.limit=limit}; }
MODEL_HOOK static void model_port(void *fn) { model_io[model_io_count++]=fn; }
MODEL_HOOK static int model_report(void)
{
    int failed=0;
    for(unsigned i=0;i<model_target_count;i++) {
        struct model_target *t=&model_targets[i];
        unsigned us=(unsigned)((t->maximum+999)/1000);
        printf("f0-46 IRQ model handler=%s samples=%u max_us=%u budget_us=%u %s\n",t->name,t->calls,us,t->limit,t->calls && us<=t->limit?"PASS":"FAIL");
        if(t->count10 || t->count1000)printf("f0-46 timer cost handler=%s count10_max_us=%llu count1000_max_us=%llu\n",t->name,(unsigned long long)((t->count10+999)/1000),(unsigned long long)((t->count1000+999)/1000));
        if(t->catchup_calls)printf("f0-46 VM catchup handler=%s turns=%u max_us=%llu total_us=%llu\n",t->name,t->catchup_calls,
            (unsigned long long)((t->catchup_max+999)/1000),(unsigned long long)((t->catchup_total+999)/1000));
        if(!t->calls || us>t->limit)failed=1;
    }
    return failed;
}
