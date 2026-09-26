#!/usr/bin/env python3
"""Compile the actual ICH codec functions against a register model, not a T23.

This checks failure handling and init writes; it cannot validate analog output,
DMA timing, or the port I/O implementation on a real controller.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("CIUKIOS_VSBHDA_SOURCE_DIR", str(
    ROOT / "build/external/audio-compat/VSBHDA-75fa4bbfea70cbcc0c40d1212f04952ff8abbf16"
)))
code = (SOURCE / "src/hw/SC_ICH.C").read_text()
io = code[code.index("static unsigned int ich_codec_rdy("):
          code.index("/* called by ICH_adetect();")]
init = code[code.index("static unsigned int ich_ac97_init("):
            code.index("/*\n * called by ICH_setrate()")]
constants = "\n".join(re.findall(r"^#define ICH_.*$", code, re.M))
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "AC97.H"
#define dbgprintf(x) ((void)0)
struct intel_card_s {
    unsigned baseport_codec;
    unsigned char vra, codec_initialized;
    uint16_t codec_id1, codec_id2, codec_extid;
    uint32_t codec_errors;
};
struct aucards_mixerchan_s {
    struct { unsigned submixch_bits; } submixerchans[2];
};
static struct aucards_mixerchan_s mixer;
static struct aucards_mixerchan_s *aucards_ac97chan_mixerset[] = { &mixer };
static uint16_t regs[128];
static unsigned writes[128], status, semaphore_busy, read_failure;
static unsigned delay_calls, ready_after, last_power_write;
static unsigned mock_read32(void) { return status; }
static unsigned mock_read8(void) { return semaphore_busy; }
static void mock_write32(unsigned bits) { status &= ~bits; }
#define ich_read_32(card,reg) mock_read32()
#define ich_read_8(card,reg) mock_read8()
#define ich_write_32(card,reg,bits) mock_write32(bits)
static unsigned inpw(unsigned reg) {
    if (read_failure) { status |= ICH_GBL_ST_RCS; return 0xffff; }
    return regs[reg];
}
static void outpw(unsigned reg, unsigned data) {
    writes[reg]++;
    if (reg == AC97_POWER_CONTROL) {
        last_power_write = data;
        regs[reg] = (data & 0xff00) | (regs[reg] & 0x000f);
    } else regs[reg] = data;
}
static void pds_delay_10us(unsigned delay) {
    (void)delay;
    delay_calls++;
    if (ready_after && delay_calls >= ready_after)
        regs[AC97_POWER_CONTROL] |= 0x000e;
}
'''
checks = r'''
static void reset(struct intel_card_s *card) {
    memset(card,0,sizeof(*card));
    memset(regs,0,sizeof(regs));
    memset(writes,0,sizeof(writes));
    status = ICH_GBL_ST_PCR;
    semaphore_busy = read_failure = delay_calls = ready_after = 0;
    last_power_write = 0;
    regs[AC97_VENDOR_ID1] = 0x8384;
    regs[AC97_VENDOR_ID2] = 0x7600;
    regs[AC97_EXTENDED_ID] = AC97_EA_ID_VRA | AC97_EA_ID_SPDIF;
    regs[AC97_POWER_CONTROL] = 0x000f;
}
int main(void) {
    struct intel_card_s card;
    reset(&card);
    assert(ich_ac97_init(&card,44100));
    assert(card.vra && card.codec_initialized && !card.codec_errors);
    assert(regs[AC97_EXTENDED_STATUS] == (AC97_EA_VRA | AC97_EA_SPDIF));
    assert(regs[AC97_PCMOUT_VOL] == 0x0202 && !writes[0x5e]);
    puts("PASS standard codec: analog init, VRA, advertised S/PDIF");

    reset(&card);
    regs[AC97_POWER_CONTROL] = 0xff00;
    ready_after = 3;
    assert(ich_ac97_init(&card,48000));
    assert(last_power_write == 0x8100); /* EAPD + ADC policy preserved */
    assert(delay_calls == 3 && !card.vra);
    puts("PASS warm powered-down codec: wake and wait; preserve EAPD/ADC");

    reset(&card);
    regs[AC97_VENDOR_ID1] = 0x4352;
    regs[AC97_VENDOR_ID2] = 0x5931;
    regs[AC97_EXTENDED_ID] = AC97_EA_ID_VRA;
    regs[AC97_EXTENDED_STATUS] = AC97_EA_DRA | AC97_EA_SPDIF;
    assert(ich_ac97_init(&card,44100));
    assert(regs[0x5e] == 0x0080 && writes[0x5e] == 1);
    assert(regs[AC97_EXTENDED_STATUS] == AC97_EA_VRA);
    puts("PASS CS4299 ID-gated analog slots; no unsupported generic S/PDIF");

    reset(&card);
    status |= ICH_GBL_ST_RCS;
    assert(ich_codec_read(&card,AC97_VENDOR_ID1) == 0x8384);
    assert(!card.codec_errors && !(status & ICH_GBL_ST_RCS));
    puts("PASS stale codec timeout is acknowledged before the next read");

    reset(&card);
    read_failure = 1;
    assert(ich_codec_read(&card,AC97_VENDOR_ID1) == 0xffff);
    assert(card.codec_errors == 1 && !(status & ICH_GBL_ST_RCS));
    assert(!ich_ac97_init(&card,44100) && !card.codec_initialized);
    puts("PASS failed codec read cannot become successful initialization");

    reset(&card);
    semaphore_busy = 1;
    ich_codec_write(&card,AC97_PCMOUT_VOL,0);
    assert(!writes[AC97_PCMOUT_VOL] && card.codec_errors == 1);
    assert(delay_calls == ICH_DEFAULT_RETRY);
    puts("PASS busy semaphore: bounded timeout, no unsafe port write");

    reset(&card);
    status = 0;
    assert(ich_codec_read(&card,AC97_VENDOR_ID1) == 0xffff);
    assert(card.codec_errors == 1 && delay_calls == ICH_DEFAULT_RETRY);
    puts("PASS unavailable AC-link: bounded failure");

    reset(&card);
    regs[AC97_POWER_CONTROL] = 0;
    assert(!ich_ac97_init(&card,44100));
    assert(delay_calls == 100 && !card.codec_initialized);
    assert(!writes[AC97_PCMOUT_VOL]);
    puts("PASS unavailable analog path: bounded failure before unmuting");

    reset(&card);
    regs[AC97_VENDOR_ID1] = regs[AC97_VENDOR_ID2] = 0;
    assert(!ich_ac97_init(&card,44100) && !writes[AC97_POWER_CONTROL]);
    puts("PASS absent codec ID rejected");
    return 0;
}
'''
# Compile extracted production functions, not a reimplementation of their logic.
with tempfile.TemporaryDirectory(prefix="ciukios-ich-codec-") as work:
    executable = str(Path(work) / "codec-test")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-parameter", "-x", "c", "-",
                    "-I", str(SOURCE / "src/hw"), "-o", executable],
                   input=constants + "\n" + fixture + io + init + checks,
                   text=True, check=True)
    subprocess.run([executable], check=True)
print("Register-model checks only; this does not verify real-hardware audio.")
