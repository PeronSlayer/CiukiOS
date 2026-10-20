#define _POSIX_C_SOURCE 200809L
/* Host CPU model only, excludes ATA PIO/device latency. LGPL-2.1-or-later. */
#include "../../../../sdk/win32/audio/audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static struct wa_engine engine;
static struct wa_source source;
static uint8_t sectors[176400], reservoir[176400];
static volatile uint32_t checksum;
void *wa_alloc(uint32_t n) { return calloc(1,n); }
void wa_free(void *p) { free(p); }
static uint64_t cpu(void) {
  struct timespec t; assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));
  return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
int main(void) {
  for (unsigned i=0;i<sizeof(sectors);i++) sectors[i]=(uint8_t)(i*13u);
  struct wa_transport transport={0}; wa_init(&engine,&transport);
  const struct wa_format format={1,2,44100,176400,4,16,0};
  wa_source_init(&source,&format);
  struct wa_samples *samples=malloc(sizeof(*samples)+sizeof(sectors)); assert(samples);
  samples->refs=1; samples->bytes=sizeof(sectors); memcpy(samples->data,sectors,sizeof(sectors));
  source.samples=samples; source.playing=source.looping=1; assert(!wa_register(&engine,&source));
  uint64_t begin=cpu();
  for (unsigned second=0;second<600;second++) { memcpy(reservoir,sectors,sizeof(sectors)); checksum+=reservoir[second%sizeof(reservoir)]; }
  uint64_t copied=cpu()-begin; int16_t out[512]; begin=cpu();
  for (unsigned second=0;second<60;second++) for (uint32_t frame=0;frame<44100;) {
    uint32_t n=44100-frame; if (n>256) n=256;
    assert(wa_render(&engine,out,n)==n); checksum+=(uint16_t)out[0]; frame+=n;
  }
  uint64_t mixed=cpu()-begin;
  printf("CD_COST copy_ns_per_audio_second=%llu mix_ns_per_audio_second=%llu total_host_cpu_percent=%.6f estimate_percent=0.18 checksum=%u\n",
         (unsigned long long)(copied/600),(unsigned long long)(mixed/60),
         ((double)copied/600+(double)mixed/60)/10000000.0,checksum);
  free(samples); return 0;
}
