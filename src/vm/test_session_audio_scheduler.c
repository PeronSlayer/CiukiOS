#define _GNU_SOURCE
/* Exercise the production transport and peripheral model with an Intel
 * descriptor cursor, a delayed guest IRQ handler, and two guest DMA halves. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "session_devices.c"

static uint8_t guest_dma[128];
static uint32_t fake_clock = 10000, fake_civ, fake_lvi, fake_status, irq_requests, dma_reads;
static uint8_t *audio_memory;
static struct cvdev_instance test_instance;

uint32_t cvdev_in(uint32_t port, uint32_t width)
{
    (void)width;
    if (port == 0x1814) return fake_civ;
    if (port == 0x1815) return fake_lvi;
    if (port == 0x1816) return fake_status;
    if (port == 0x181b) return 1;
    return 0;
}
void cvdev_out(uint32_t port, uint32_t value, uint32_t width)
{
    (void)width;
    if (port == 0x1815) fake_lvi = value;
    if (port == 0x1816) fake_status &= ~value;
    if (port == 0x181b && value == 1) fake_status &= ~1u;
}
int cvdev_raise_irq(uint32_t vm, uint32_t irq, uint32_t audio)
{
    (void)vm; assert(irq == 7 && audio); ++irq_requests; return 1;
}
void cvdev_route_line(uint32_t irq) { (void)irq; }
uint8_t *cvdev_guest_linear(uint32_t vm, uint32_t physical)
{
    (void)vm;
    if (physical < 0x1000 || physical >= 0x1080) return 0;
    ++dma_reads;
    return guest_dma + physical - 0x1000;
}
uint32_t cvdev_pm_state(void) { return 0; }
void cvdev_fpu_save(void *area) { (void)area; }
void cvdev_fpu_restore(void *area) { (void)area; }
void cvvid_rdtsc(uint32_t *lo, uint32_t *hi)
{
    fake_clock += 10; *lo = fake_clock; *hi = 0;
}
void *cvop_create(unsigned rate) { (void)rate; return 0; }
void cvop_write(void *chip, unsigned reg, unsigned value) { (void)chip;(void)reg;(void)value; }
void cvop_generate(void *chip, short *out, unsigned frames) { (void)chip;(void)out;(void)frames; }

static void guest_write(unsigned port, unsigned value)
{
    assert(cvgp_io_write(&I->devices, 1, (uint16_t)port, 1, 0, value) == CVGP_IO_OK);
}
static void setup(unsigned rate)
{
    cvgp_backend backend;
    unsigned i;
    memset(&test_instance, 0, sizeof test_instance);
    I = A = &test_instance;
    I->active = I->generation = 1;
    I->tsc_per_us = 1;
    I->caps = CVGP_CAP_DMA_SB;
    memset(&backend, 0, sizeof backend);
    backend.memory_read8 = memory_read8;
    backend.opaque = I;
    cvgp_init(&I->devices);
    assert(cvgp_begin(&I->devices, 1, CVGP_CAP_DMA_SB, CVGP_CAP_DMA_SB, &backend) == CVGP_OK);
    I->devices.pic[0].auto_eoi = 1;
    guest_write(0x0c, 0); guest_write(2, 0); guest_write(2, 0x10);
    guest_write(3, 127); guest_write(3, 0); guest_write(0x83, 0);
    guest_write(0x0b, 0x59); guest_write(0x0a, 1);
    guest_write(0x22c, 0x41); guest_write(0x22c, 44100 >> 8); guest_write(0x22c, 44100 & 255);
    guest_write(0x22c, 0xc6); guest_write(0x22c, 0); guest_write(0x22c, 63); guest_write(0x22c, 0);
    memset(guest_dma, 0x81, 64); memset(guest_dma + 64, 0x82, 64);
    memset(audio_memory, 0, CVDEV_AUDIO_BUFFERS * 4096u);
    for (i = 0; i != CVDEV_AUDIO_BUFFERS; ++i)
        buffer_linear[i] = (uint32_t)(uintptr_t)(audio_memory + i * 4096u);
    audio_running = 1; audio_rate = rate; nabm = 0x1800;
    next_fill = render_offset = 0;
    fake_civ = 25; fake_lvi = 31; fake_status = 0;
    irq_requests = dma_reads = 0;
    I->audio_service_lo = fake_clock;
    cvaudio_stream_reset(&resampler);
}
static void service(void)
{
    audio_service();
    pump();
}
static void acknowledge(unsigned block)
{
    uint32_t ignored;
    assert(cvgp_io_read(&I->devices, 1, 0x22e, 1, 0, &ignored) == CVGP_IO_OK);
    /* Simulated complete guest interrupt handler updates the consumed half. */
    memset(guest_dma + (block & 1u) * 64u, (int)(0x83u + block), 64);
}
static int16_t output_sample(unsigned frame)
{
    unsigned descriptor = frame / CVDEV_AUDIO_FRAMES;
    return ((int16_t *)(audio_memory + descriptor * 4096u))[(frame % CVDEV_AUDIO_FRAMES) * 2u];
}
int main(void)
{
    unsigned i, last_reads, codec;
    const unsigned fallback_rates[] = {8000, 48000};
    int16_t old_pcm[2048 * 2];
    cvdev_audio_report report;
    audio_memory = mmap(0, CVDEV_AUDIO_BUFFERS * 4096u, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(audio_memory != MAP_FAILED && (uintptr_t)audio_memory < 0xffffffffu);
    assert(sizeof(cvdev_report) == 128 && sizeof(cvdev_audio_report) == 128);

    setup(44100);
    assert(cvgp_render_audio(&I->devices, 1, old_pcm, 2048, 44100) == CVGP_OK);
    pump();
    assert(dma_reads == 2048 && irq_requests == 1);
    for (i = 0; i != 2048; ++i) assert(old_pcm[i * 2u] == (i % 128u < 64u ? 256 : 512));
    puts("negative control: 32 DMA blocks collapse into 1 IRQ and repeated stale PCM");

    setup(44100);
    for (i = 0; i != 32; ++i) {
        service();
        assert(irq_requests == i + 1u && dma_reads == (i + 1u) * 64u);
        last_reads = dma_reads;
        service();                     /* guest has not acknowledged */
        assert(dma_reads == last_reads);
        acknowledge(i);
        service();                     /* return past the acknowledgement IN */
        assert(dma_reads == last_reads && I->audio_waiting == 2);
        if (((next_fill - fake_civ) & 31u) == CVDEV_AUDIO_LEAD)
            fake_civ = (fake_civ + 1u) & 31u;
    }
    assert(I->stats.buffers_rendered == 8 && render_offset == 0);
    for (i = 0; i != 2048; ++i) assert(output_sample(i) == (int16_t)((i / 64u + 1u) * 256u));
    cvdev_report_audio(&report);
    assert(report.magic == 0x54415643u && report.bytes == 128 && report.generation == 1);
    assert(report.sb_rate == 44100 && report.sb_block_units == 64 && report.sb_boundaries == 32);
    assert(report.source_frames == 2048 && report.output_frames == 2048 && !report.late_acks);
    assert(report.max_render_cycles && report.total_render_lo && report.max_gap_us);
    puts("production transport: 32 IRQs, fresh PCM, partial descriptors, acknowledgement yields");

    setup(44100);
    guest_write(0x22c, 0x41); guest_write(0x22c, 11025 >> 8); guest_write(0x22c, 11025 & 255);
    for (i = 0; i != 8; ++i) {
        service();
        assert(dma_reads == (i + 1u) * 64u && irq_requests == i + 1u);
        assert(I->audio_timing.output_frames == (i + 1u) * 256u && render_offset == 0);
        acknowledge(i);
        service();
        fake_civ = (fake_civ + 1u) & 31u;
    }
    puts("11.025-kHz SB source: programmed sample rate and 256-frame output periods preserved");

    setup(44100);
    service(); assert(I->audio_waiting == 1 && dma_reads == 64);
    fake_clock += 2000;                /* longer than its programmed 64/44100 period */
    service();
    assert(I->audio_timing.late_acks == 1 && dma_reads == 128);
    assert(I->audio_waiting == 1 && render_offset == 128);
    puts("masked guest IRQ: bounded wait preserves continuing DMA and counts late acknowledgement");

    fake_status = 0x10;
    service();
    assert(I->audio_timing.fifo_errors == 1 && !(fake_status & 0x10u));
    service(); assert(I->audio_timing.fifo_errors == 1);
    fake_status = 1;
    service();
    assert(I->audio_timing.dma_halts == 1 && I->stats.underruns == 1 && !(fake_status & 1u));
    puts("ICH status: FIFO errors and DMA halts counted separately before status clearing");

    /* A fallback codec's interpolation lookahead must also stop at the
     * source boundary. Ack/yield completes each handler before more reads. */
    for (codec = 0; codec != sizeof fallback_rates / sizeof fallback_rates[0]; ++codec) {
        setup(fallback_rates[codec]);
        for (i = 0; i != 16; ++i) {
            while (irq_requests != i + 1u) {
                if (((next_fill - fake_civ) & 31u) == CVDEV_AUDIO_LEAD)
                    fake_civ = (fake_civ + 1u) & 31u;
                service();
            }
            assert(dma_reads == (i + 1u) * 64u && irq_requests == i + 1u);
            last_reads = dma_reads;
            service(); assert(dma_reads == last_reads);
            acknowledge(i);
            service(); assert(dma_reads == last_reads);
            if (((next_fill - fake_civ) & 31u) == CVDEV_AUDIO_LEAD)
                fake_civ = (fake_civ + 1u) & 31u;
        }
        assert(I->audio_timing.source_frames == 1024 && !I->audio_timing.late_acks);
        printf("%u-Hz codec fallback: interpolation prefetch respects all 16 DMA boundaries\n",
               fallback_rates[codec]);
    }
    assert(munmap(audio_memory, CVDEV_AUDIO_BUFFERS * 4096u) == 0);
    puts("session audio scheduler: PASS");
    return 0;
}
