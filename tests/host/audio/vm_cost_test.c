/* F4-28: isolated production render stages. Link-time GC discards the VM
 * control path; no exported test API enters the kernel. SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include "../vm_audio_shim.h"
#include "../../../src/kernel/audio/vm_source.c"
struct audio_state audio;
volatile uint64_t g_ticks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); exit(1); } } while (0)
/* The isolated PCM window is already fetched; reaching a queue is a failure. */
int dosvm_audio_fetch(struct dosvm_key key, uint32_t token, struct dosvm_audio_block *block, int16_t *out, unsigned frames)
{ (void)key; (void)token; (void)block; (void)out; (void)frames; CHECK(0); return -EAGAIN; }
static uint64_t cpu_ns(void)
{
    struct timespec t; CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t));
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static struct vm_slot slot;
static int64_t output[AUDIO_QUANTUM][2];
static volatile uint32_t sink;
static void arithmetic_oracles(void)
{
    const int16_t ends[] = {-32768, 32767};
    for (unsigned a = 0; a < 2; a++) for (unsigned b = 0; b < 2; b++)
        for (unsigned phase = 0; phase < VM_HZ; phase++)
            CHECK(pcm_interpolate(ends[a], ends[b], phase) ==
                  ends[a] + ((int64_t)ends[b] - ends[a]) * phase / VM_HZ);
    const int32_t samples[] = {-(int32_t)VM_RAW_BOUND, -65536, -32768, -1, 0, 1, 32767, 65534, VM_RAW_BOUND};
    /* Every CT1745/source combination, including non-unity +18 dB and
     * negative fractions: Q16 floors; interpolation truncates toward zero. */
    for (unsigned master = 0; master < 32; master++) for (unsigned voice = 0; voice < 32; voice++)
        for (unsigned out = 0; out < 4; out++) for (unsigned source = 0; source <= 100; source++) {
            int64_t gain = (((int64_t)level_gain(master) * level_gain(voice) >> 16) * output_gain[out] >> 16) * source / 100;
            CHECK(combined_gain(master, voice, out, source) == gain);
            for (unsigned i = 0; i < ARRAY_SIZE(samples); i++)
                CHECK(gained_sample(samples[i], (uint32_t)gain) == ((int64_t)samples[i] * gain >> 16));
        }
    uint32_t seed = 0x243f6a88;
    const unsigned divisors[] = {1, 8000, 11025, 22050, 44100, 45000, 48000, 65535, 65536};
    for (unsigned n = 0; n < 10000; n++) {
        seed = seed * 1664525u + 1013904223u; uint64_t value = (uint64_t)seed << 32;
        seed = seed * 1664525u + 1013904223u; value |= seed;
        if (n == 0) value = UINT64_MAX;
        for (unsigned d = 0; d < ARRAY_SIZE(divisors); d++) {
            uint32_t remainder;
            CHECK(timeline_div(value, divisors[d], &remainder) == value / divisors[d]);
            CHECK(remainder == value % divisors[d]);
        }
        /* Conservative 4-VM amplified bound with extreme cancellation. */
        const uint32_t bound = VM_MIX_BOUND * DOSVM_LIMIT;
        int32_t a = (int32_t)(seed % (bound * 2u + 1u)) - (int32_t)bound;
        seed = seed * 1664525u + 1013904223u;
        int32_t b = (int32_t)(seed % (bound * 2u + 1u)) - (int32_t)bound;
        unsigned phase = seed % 44100;
        CHECK(mix_interpolate(a, b, phase, 44100) == a + ((int64_t)b - a) * phase / 44100);
    }
    for (unsigned r = 0; r < ARRAY_SIZE(divisors); r++) {
        pcm_step(&slot, divisors[r]); slot.frac = 47999; slot.idx = 0;
        unsigned phase = slot.frac; uint64_t idx = 0;
        for (unsigned k = 0; k < 2000; k++) {
            phase += divisors[r]; idx += phase / VM_HZ; phase %= VM_HZ;
            slot.frac += slot.remainder; bool carry = slot.frac >= VM_HZ;
            if (carry) slot.frac -= VM_HZ;
            slot.idx += slot.advance[carry];
            CHECK(slot.frac == phase && slot.idx == idx);
        }
    }
    puts("VM arithmetic: all CT1745/source gains, signed rounding, S16 extrema, amplified cancellation, full-width timeline and step oracles PASS");
}
static void note(struct opl3_chip *chip)
{
    static const uint8_t regs[][2] = {{0x20,0x21},{0x23,0x21},{0x40,0x3f},{0x43,0},
        {0x60,0xf0},{0x63,0xf0},{0x80,0xf},{0x83,0xf},{0xc0,1},{0xa0,0x44},{0xb0,0x32}};
    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) opl3_write(chip, regs[i][0], regs[i][1]);
}
int main(void)
{
    arithmetic_oracles();
    audio.master.left = 50; audio.master.right = 37;
    slot.gain[0] = 75; slot.gain[1] = 63;
    struct dosvm_audio_info info = {.master={28,28}, .voice={25,25}, .fm={20,20}, .output_gain={1,1}};
    for (unsigned i = 0; i < VM_WINDOW; i++) {
        slot.win[i][0] = (int16_t)(i * 1009); slot.win[i][1] = (int16_t)(i * 3011);
    }
    opl3_init(&slot.chip); note(&slot.chip);
    const char *names[] = {"pcm_11025", "pcm_22050", "fm_block", "fm_scalar", "gain_taps_levels", "saturation_tap"};
    for (unsigned stage = 0; stage < ARRAY_SIZE(names); stage++) {
        uint64_t cost = 0;
        for (unsigned iteration = 0; iteration < 2000; iteration++) {
            slot.stream = true; slot.idx = slot.win_first = 0; slot.win_count = VM_WINDOW;
            pcm_step(&slot, stage ? 22050 : 11025); slot.frac = iteration % VM_HZ;
            for (unsigned k = 0; k < AUDIO_QUANTUM; k++) {
                output[k][0] = (int32_t)(k * 317) - 40000;
                output[k][1] = 40000 - (int32_t)(k * 503);
            }
            uint64_t begin = cpu_ns();
            if (stage < 2) pcm_render(&slot, 0, &info, pcm_buf, AUDIO_QUANTUM);
            else if (stage == 2) opl3_generate(&slot.chip, opl_buf, AUDIO_QUANTUM);
            else if (stage == 3) for (unsigned k = 0; k < AUDIO_QUANTUM; k++) opl3_generate(&slot.chip, opl_buf + k * 2, 1);
            else if (stage == 4) CHECK(mix_samples(&slot, &info, output, AUDIO_QUANTUM));
            else for (unsigned k = 0; k < AUDIO_QUANTUM; k++) tap_frame(&ring, output[k][0], output[k][1]);
            cost += cpu_ns() - begin;
            sink ^= (uint32_t)pcm_buf[iteration % (AUDIO_QUANTUM * 2)] ^ slot.stats.opl.digest ^ ring.digest;
        }
        CHECK(cost / 2000 < 100000);
        printf("VM cost stage=%s mean_ns=%llu PASS\n", names[stage], (unsigned long long)(cost / 2000));
    }
    return 0;
}
