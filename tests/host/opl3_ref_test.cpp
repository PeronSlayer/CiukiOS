/* f4-05: the kernel's C OPL3 port (src/kernel/audio/opl3.c) against the
 * pinned upstream DBOPL C++ (third_party/vsbhda, DBOPL.CPP/H unmodified,
 * extracted by scripts/test/host_kernel_tests.sh). Both run Chip(true) and
 * Setup(48000); random register scripts over OPL2 and OPL3 modes, every
 * waveform, four-operator pairs, rhythm, vibrato/tremolo, feedback, KSL/KSR
 * and A/B routing are written to both, with generation chunks of random
 * length between writes. Samples must agree exactly: VSBHDA accumulates in
 * int16_t (wrapping), the port in int32_t, so the port's samples are compared
 * modulo 2^16, which addition preserves.
 * SPDX-License-Identifier: GPL-2.0-or-later (links DBOPL) */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "DBOPL.H"
extern "C" {
#include <ciuki/opl3.h>
}

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static DBOPL::Chip *ref;
static opl3_chip port;
static int16_t ref_out[4 * 512];
static int32_t port_out[2 * 512];
static unsigned long long frames, sounding;

static void poke(unsigned reg, uint8_t value) { ref->WriteReg(reg, value); opl3_write(&port, reg, value); }
static void generate(unsigned n)
{
    std::memset(ref_out, 0x5A, sizeof(ref_out));
    bool stereo = ref->opl3Active != 0;
    CHECK(!stereo == !port.opl3_active);
    ref->Generate(ref_out, n);
    opl3_generate(&port, port_out, n);
    for (unsigned i = 0; i < n; i++) {
        int16_t l = stereo ? ref_out[i * 2] : ref_out[i], r = stereo ? ref_out[i * 2 + 1] : ref_out[i];
        if ((int16_t)port_out[i * 2] != l || (int16_t)port_out[i * 2 + 1] != r) {
            std::fprintf(stderr, "frame %llu: port %d,%d ref %d,%d (opl3=%d)\n", frames + i,
                         port_out[i * 2], port_out[i * 2 + 1], l, r, stereo);
            CHECK(false);
        }
        if (l || r) sounding++;
        checks++;
    }
    frames += n;
}
static unsigned random_reg(bool opl3)
{
    static const unsigned bases[][2] = { {0x20, 0x35}, {0x40, 0x55}, {0x60, 0x75}, {0x80, 0x95}, {0xE0, 0xF5},
                                         {0xA0, 0xA8}, {0xB0, 0xB8}, {0xC0, 0xC8} };
    unsigned pick = rnd() % 100;
    if (pick < 3) return 0xBD;
    if (pick < 4) return 0x08;
    if (pick < 5) return 0x01;
    if (opl3 && pick < 7) return 0x104;
    const unsigned *range = bases[rnd() % 8];
    unsigned reg = range[0] + rnd() % (range[1] - range[0] + 1);
    return (opl3 && (rnd() & 1)) ? 0x100 + reg : reg;
}
static uint8_t random_value(unsigned reg)
{
    uint8_t v = (uint8_t)rnd();
    unsigned low = reg & 0xff;
    if (low >= 0x40 && low <= 0x55) v &= 0xC0 | (rnd() & 1 ? 0x0F : 0x3F); /* audible levels more often */
    if (low >= 0x60 && low <= 0x75 && (rnd() & 1)) v |= 0xA0;                /* fast attacks more often */
    if (low >= 0xB0 && low <= 0xB8 && (rnd() % 3 == 0)) v |= 0x20;           /* key on */
    return v;
}
static void script(bool opl3, unsigned writes)
{
    poke(0x105, opl3 ? 1 : 0);
    poke(0x01, 0x20);
    for (unsigned i = 0; i < writes; i++) {
        unsigned reg = random_reg(opl3);
        poke(reg, random_value(reg));
        if (rnd() % 4 == 0) generate(1 + rnd() % 300);
        if (rnd() % 997 == 0) poke(0x105, (uint8_t)(rnd() & 1)); /* mode switches mid-stream */
    }
    generate(512);
}
/* A plain 440 Hz sine (AM pair, modulator at TL 63): the f4:vm-audio note. */
static void note(void)
{
    static const uint8_t regs[][2] = { {0x20, 0x21}, {0x23, 0x21}, {0x40, 0x3F}, {0x43, 0x00}, {0x60, 0xF0}, {0x63, 0xF0},
                                       {0x80, 0x0F}, {0x83, 0x0F}, {0xC0, 0x01}, {0xA0, 0x44}, {0xB0, 0x32} };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) poke(regs[i][0], regs[i][1]);
    for (unsigned i = 0; i < 40; i++) generate(480);
    poke(0xB0, 0x12);
    generate(480);
}
int main(void)
{
    for (unsigned round = 0; round < 6; round++) {
        ref = new DBOPL::Chip(true);
        ref->Setup(OPL3_RATE);
        opl3_init(&port);
        generate(64);
        if (round == 0) note();
        else script(round % 2 == 0, 6000);
        delete ref;
    }
    CHECK(sounding > frames / 4);
    std::printf("OPL3 C port vs pinned DBOPL C++: PASS (%llu frames, %llu sounding, %u checks)\n", frames, sounding, checks);
    return 0;
}
