/* f3-53: actual software cadence/OPL producers, no physical device access.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#define CIUKI_CPU_H
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t f) { (void)f; }
#include "../../src/kernel/vm/dosvm_sb16.c"
#include "../../src/kernel/vm/dosvm_opl.c"
volatile uint64_t g_ticks;
struct ciuki_boot_info g_boot;
uint64_t g_tsc_per_ms;
uint64_t dosvm_perf_clock(void) { return 0; }
static struct dosvm title_vm;
#define REQUIRE(c) do { if (!(c)) return __LINE__; } while (0)
int main(void)
{
    struct dosvm *v=&title_vm;
    v->audio.sb.irq=7; v->audio.sb.irq_entry_pending=true;
    v->audio.clock=960; /* 20 ms at 48 kHz */
    dosvm_audio_irq_enter(v,5);
    REQUIRE(!v->audio.cadence_irq_entries);
    dosvm_audio_irq_enter(v,7);
    REQUIRE(v->audio.cadence_irq_entries==1 && v->audio.cadence_irq_latency_max_us==20000);
    dosvm_audio_irq_enter(v,7); REQUIRE(v->audio.cadence_irq_entries==1);
    v->audio.sb.causes=1;v->audio.sb.unserviced=2;v->audio.sb.x.waiting=false;
    sb_ack(v,1);
    REQUIRE(v->audio.cadence_late_acks==1);
    sb_ack(v,1); REQUIRE(v->audio.cadence_late_acks==1);
    dosvm_opl_init(v);
    dosvm_opl_io(v,0,true,0x20);
    REQUIRE(!v->audio.opl_writes);
    dosvm_opl_io(v,1,true,0x01);
    REQUIRE(v->audio.opl_writes==1);
    memset(&v->audio.sb.cadence,0,sizeof(v->audio.sb.cadence));
    dosvm_opl_init(v); sb_reset_port(v,1);sb_reset_port(v,0);
    REQUIRE(v->audio.opl_writes==1 && v->audio.cadence_irq_entries==1 &&
            v->audio.cadence_late_acks==1 && v->audio.cadence_irq_latency_max_us==20000);
    /* The amended title profile uses the existing configurable mixer. */
    v->audio.sb.mixer_index=0x80;sb_mixer_write(&v->audio.sb,2);
    REQUIRE(v->audio.sb.irq==5 && sb_mixer_read(&v->audio.sb)==2);
    v->audio.sb.mixer_index=0x81;sb_mixer_write(&v->audio.sb,0x22);
    REQUIRE(v->audio.sb.dma8==1 && v->audio.sb.dma16==5 && sb_mixer_read(&v->audio.sb)==0x22);
    return 0;
}
