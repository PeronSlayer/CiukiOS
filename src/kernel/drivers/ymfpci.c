// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) by Jaroslav Kysela <perex@perex.cz>
 * Adapted from Linux v6.12 sound/pci/ymfpci/{ymfpci.c,ymfpci.h,ymfpci_main.c}.
 * CiukiOS: fixed stereo PCM, private ownership/MMIO/DMA adapters, verified
 * firmware, elapsed deadlines, coherent cursors and stop/quarantine proof.
 * Firmware has its own Yamaha notice: third_party/yamaha/NOTICE.txt.
 * Research and limitations: docs/design/f1-119-yamaha-ds-xg.md. */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio_backend.h>
#include <ciuki/sha256.h>

#define YDSXG_DSPLENGTH 0x80u
#define YDSXG_CTRLLENGTH 0x3000u
#include "../../../third_party/yamaha/ds1_microcode.h"
_Static_assert(sizeof(DspInst) == YDSXG_DSPLENGTH, "DS-1 DSP size");
_Static_assert(sizeof(CntrlInst) == YDSXG_CTRLLENGTH, "DS-1 control size");
_Static_assert(sizeof(CntrlInst1E) == YDSXG_CTRLLENGTH, "DS-1E control size");

enum {
    YMF_GLOBAL = 0x08, YMF_TIMER = 0x10, YMF_SPDIF_OUT = 0x18,
    YMF_SPDIF_IN = 0x34, YMF_CODEC_DATA = 0x60, YMF_CODEC_CMD = 0x62,
    YMF_CODEC_VALUE = 0x64, YMF_CODEC_STATUS = 0x66, YMF_SECONDARY = 0x70,
    YMF_DAC_VOLUME = 0x84, YMF_STATUS = 0x100, YMF_BANK = 0x104,
    YMF_MODE = 0x108, YMF_CONFIG = 0x114, YMF_PLAY_SIZE = 0x140,
    YMF_REC_SIZE = 0x144, YMF_EFF_SIZE = 0x148, YMF_WORK_SIZE = 0x14c,
    YMF_REC_MAP = 0x150, YMF_EFF_MAP = 0x154, YMF_PLAY_BASE = 0x158,
    YMF_REC_BASE = 0x15c, YMF_EFF_BASE = 0x160, YMF_WORK_BASE = 0x164,
    YMF_DSP_RAM = 0x1000, YMF_CTRL_RAM = 0x4000,
    YMF_BANK_BYTES = 120, YMF_BANKS = 0x200,
    YMF_CAPTURE = 0x400, YMF_EFFECT = 0x500, YMF_WORK = 0x600,
    YMF_BANK_START = 18 * 4
};
#define YMF_BANK_IRQ 0x80000000u
#define YMF_FRAMES (AUDIO_HW_SLOTS * AUDIO_QUANTUM)

bool ymfpci_match(const struct pci_func *p, enum audio_backend_kind *kind)
{
    if (p->vendor != 0x1073 || p->class_code != 4 || p->subclass != 1) return false;
    switch (p->device) {
    case 0x0004: case 0x000a: case 0x000d: case 0x000c: case 0x0010: case 0x0012:
        *kind = AUDIO_YMFPCI; return true;
    default: return false;
    }
}
static bool ymf_ds1e(struct audio_dma_device *d)
{ return d->pci.device != 0x0004 && d->pci.device != 0x000a; }

int ymfpci_resources(struct audio_dma_device *d)
{
    /* Refuse I/O, 64-bit, prefetchable, unassigned, misaligned and wrapping
     * BARs rather than truncating any address or probing BAR size by writes. */
    uint32_t base = d->pci.bar[0];
    if ((d->pci.header_type & 0x7f) || !base || base == UINT32_MAX ||
        (base & 0x7fffu) || base > UINT32_MAX - 0x8000u) {
        d->reason = "invalid_bars"; return -ENODEV;
    }
    if (!(d->command & 2)) { d->reason = "firmware_state_unreadable"; return -ENODEV; }
    if (!audio_pci_routed(&d->io, &d->pci)) { d->reason = "routing_unqualified"; return -ENODEV; }
    d->mmio_phys = base; return 0;
}

/* DMA control banks are little endian, shared with the DSP, never C bitfields.
 * Aligned dword accesses must remain atomic while the controller writes them. */
static uint32_t ymf_load(const volatile uint32_t *p) { return *p; } /* i686 LE */
static void ymf_store(volatile uint32_t *p, uint32_t value) { *p = value; }
static volatile uint32_t *ymf_bank(struct audio_dma_device *d, unsigned voice, unsigned bank)
{ return (volatile uint32_t *)((uint8_t *)d->desc + YMF_BANKS + (voice * 2 + bank) * YMF_BANK_BYTES); }

static int ymf_codec_ready(struct audio_dma_device *d)
{
    uint64_t end = audio_hw_deadline(d, 100);
    do {
        uint32_t value = audio_hw_read(d, YMF_CODEC_STATUS, 2);
        if (value == 0xffff) return -EIO;
        if (!(value & 0x8000)) return 0;
        d->io.sleep(d->io.cookie, 1);
    } while ((int64_t)(d->io.now(d->io.cookie) - end) < 0);
    d->reason = "codec_timeout"; return -ETIMEDOUT;
}
static int ymf_codec_read(struct audio_dma_device *d, uint16_t reg, uint16_t *value)
{
    int e = ymf_codec_ready(d);
    if (e) return e;
    audio_hw_write(d, YMF_CODEC_CMD, 0x8000 | reg, 2);
    e = ymf_codec_ready(d);
    if (e) return e;
    if (d->pci.device == 0x0010 && d->pci.revision < 2)
        for (unsigned i = 0; i < 600; i++) (void)audio_hw_read(d, YMF_CODEC_VALUE, 2);
    *value = audio_hw_read(d, YMF_CODEC_VALUE, 2);
    return *value == 0xffff ? -EIO : 0;
}
int ymfpci_codec_write(struct audio_dma_device *d, uint16_t reg, uint16_t value)
{
    int e = ymf_codec_ready(d);
    if (e) return e;
    audio_hw_write(d, YMF_CODEC_DATA, (uint32_t)reg << 16 | value, 4);
    uint16_t got;
    e = ymf_codec_read(d, reg, &got); /* flush posted write before next command */
    unsigned mask = reg == 0x26 ? 0xff00 : reg == 2 || reg == 0x18 ? 0x9f1f : 0xffff;
    if (!e && ((got ^ value) & mask)) { d->reason = "codec_readback"; e = -EIO; }
    return e;
}
static int ymf_mute(struct audio_dma_device *d, bool mute)
{
    /* The common software master applies linear gain once. Digital DAC and
     * codec paths stay at unity except for an explicit global mute. */
    audio_hw_write(d, YMF_DAC_VOLUME, mute ? 0 : 0x3fff3fff, 4);
    if (audio_hw_read(d, YMF_DAC_VOLUME, 4) != (mute ? 0u : 0x3fff3fffu)) return -EIO;
    return audio_dma_codec_mute(d, mute, ymfpci_codec_write);
}
static int ymf_firmware_check(const uint32_t *words, size_t bytes, const char *expected)
{
    uint8_t digest[32]; char hex[65];
    sha256(words, bytes, digest); sha256_hex(digest, hex);
    return strncmp(hex, expected, sizeof(hex)) ? -EIO : 0;
}
static int ymf_upload(struct audio_dma_device *d, unsigned ram, const uint32_t *words, unsigned bytes)
{
    for (unsigned i = 0; i < bytes / 4; i++) {
        audio_hw_write(d, ram + i * 4, words[i], 4);
        if (audio_hw_read(d, ram + i * 4, 4) != words[i]) {
            d->reason = "microcode_readback"; return -EIO;
        }
        /* Bound work without holding IF off for the whole upload. */
        if ((i & 255) == 255) {
            d->io.sleep(d->io.cookie, 1);
            if ((int64_t)(d->io.now(d->io.cookie) - d->activation_deadline) >= 0) return -ETIMEDOUT;
        }
    }
    return 0;
}
static int ymf_bases(struct audio_dma_device *d, bool enable)
{
    static const unsigned offsets[] = {YMF_PLAY_BASE, YMF_REC_BASE, YMF_EFF_BASE, YMF_WORK_BASE, YMF_WORK_SIZE};
    const uint32_t values[] = {d->desc_phys, d->desc_phys + YMF_CAPTURE,
        d->desc_phys + YMF_EFFECT, d->desc_phys + YMF_WORK, 0x400 / 4};
    for (unsigned i = 0; i < 5; i++) {
        uint32_t value = enable ? values[i] : 0;
        audio_hw_write(d, offsets[i], value, 4);
        if (audio_hw_read(d, offsets[i], 4) != value) { d->reason = "control_base_readback"; return -EIO; }
    }
    return 0;
}
static void ymf_inhibit(struct audio_dma_device *d)
{
    audio_hw_write(d, YMF_DAC_VOLUME, 0, 4);
    audio_hw_write(d, YMF_MODE, 0, 4); /* immediate, nonblocking IRQ inhibition */
    audio_hw_write(d, YMF_STATUS, YMF_BANK_IRQ, 4); /* deassert our shared INTx cause */
    (void)audio_hw_read(d, YMF_MODE, 4);
}
static int ymf_halt(struct audio_dma_device *d)
{
    d->inhibited = true; /* IRQs must not rearm during the sleeping stop proof. */
    ymf_inhibit(d);
    uint64_t end = audio_hw_deadline(d, 500);
    bool idle = false;
    do {
        uint32_t mode = audio_hw_read(d, YMF_MODE, 4), status = audio_hw_read(d, YMF_STATUS, 4);
        if (mode == UINT32_MAX || status == UINT32_MAX) break;
        if (!(mode & 3) && !(status & 2)) { idle = true; break; }
        d->io.sleep(d->io.cookie, 1);
    } while ((int64_t)(d->io.now(d->io.cookie) - end) < 0);
    if (!idle) goto unsafe;
    audio_hw_write(d, YMF_CONFIG, 0, 4);
    if (audio_hw_read(d, YMF_CONFIG, 4) != 0 || (audio_hw_read(d, YMF_STATUS, 4) & 2)) goto unsafe;
    /* Controller-idle proof plus the PCI DMA gate, never a timeout alone. */
    audio_pci_write(&d->io, &d->pci, 4, (d->command | 2) & ~4u, 2);
    if ((audio_pci_read(&d->io, &d->pci, 4) & 0xffff) != ((d->command | 2u) & ~4u)) goto unsafe;
    uint32_t positions[4];
    for (unsigned i = 0; i < 4; i++) positions[i] = ymf_load(ymf_bank(d, i / 2, i % 2) + 18);
    uint32_t bank = audio_hw_read(d, YMF_BANK, 4);
    d->io.sleep(d->io.cookie, 1);
    if (bank > 1 || audio_hw_read(d, YMF_BANK, 4) != bank || (audio_hw_read(d, YMF_STATUS, 4) & 2)) goto unsafe;
    for (unsigned i = 0; i < 4; i++)
        if (positions[i] != ymf_load(ymf_bank(d, i / 2, i % 2) + 18)) goto unsafe;
    audio_hw_write(d, YMF_STATUS, YMF_BANK_IRQ, 4);
    if (ymf_bases(d, false)) goto unsafe;
    /* No controller ownership remains; CPU can now discard control state. */
    memset(d->desc, 0, PAGE_SIZE);
    d->state.running = false; return 0;
unsafe:
    d->reason = "dma_stop_unproved"; return -EIO;
}
static void ymf_init_banks(struct audio_dma_device *d)
{
    memset(d->desc, 0, PAGE_SIZE);
    /* Table counts all 64 voices; unused pointers remain zero. Only our first
     * stereo pair has banks and is published. Capture/effects stay disabled. */
    volatile uint32_t *table = (volatile uint32_t *)d->desc;
    ymf_store(table, 64);
    for (unsigned voice = 0; voice < 2; voice++) {
        for (unsigned bank = 0; bank < 2; bank++) {
            volatile uint32_t *b = ymf_bank(d, voice, bank);
            ymf_store(b + 0, 0x00010000 | voice); /* interleaved S16LE, L/R */
            ymf_store(b + 2, d->phys);
            ymf_store(b + 4, YMF_FRAMES); /* loop end, stereo frames */
            ymf_store(b + 6, 0x10000000); /* delta_end: 48 kHz */
            ymf_store(b + 7, 0x40000000); /* lpfK_end */
            ymf_store(b + 8, 0x40000000); /* eg_gain_end */
            ymf_store(b + 9 + voice, 0x40000000);
            ymf_store(b + 14, 0x40000000); /* lpfQ */
            ymf_store(b + 20, 0x10000000); /* delta */
            ymf_store(b + 21, 0x40000000); /* lpfK */
            ymf_store(b + 22, 0x40000000); /* eg_gain */
            ymf_store(b + 23 + voice, 0x40000000);
        }
        ymf_store(table + voice + 1, d->desc_phys + YMF_BANKS + voice * 2 * YMF_BANK_BYTES);
    }
    __asm__ volatile("" ::: "memory");
}
static int ymf_start(struct audio_dma_device *d, bool proof)
{
    if (d->state.quarantine || d->inhibited) return -EIO;
    ymf_init_banks(d);
    int e = ymf_bases(d, true);
    if (e) return e;
    audio_hw_write(d, YMF_CONFIG, 1, 4);
    if (audio_hw_read(d, YMF_CONFIG, 4) != 1) return -EIO;
    d->io.sleep(d->io.cookie, 1); /* donor requires >=100 us after DSP enable */
    audio_hw_write(d, YMF_STATUS, YMF_BANK_IRQ, 4);
    audio_hw_write(d, YMF_DAC_VOLUME, proof || audio.master.mute ? 0 : 0x3fff3fff, 4);
    audio_pci_write(&d->io, &d->pci, 4, d->command | 6u, 2);
    if ((audio_pci_read(&d->io, &d->pci, 4) & 0xffff) != (d->command | 6u)) return -EIO;
    audio_hw_write(d, YMF_MODE, 3, 4);
    return (audio_hw_read(d, YMF_MODE, 4) & 3) == 3 ? 0 : -EIO;
}
static int ymf_cursor(struct audio_dma_device *d, unsigned *frame)
{
    uint32_t status = audio_hw_read(d, YMF_STATUS, 4);
    uint32_t select = audio_hw_read(d, YMF_BANK, 4);
    if (status == UINT32_MAX || select > 1 || !(audio_hw_read(d, YMF_MODE, 4) & 1)) return -EIO;
    uint32_t left = ymf_load(ymf_bank(d, 0, select) + 18);
    uint32_t right = ymf_load(ymf_bank(d, 1, select) + 18);
    __asm__ volatile("" ::: "memory");
    if (select != audio_hw_read(d, YMF_BANK, 4) ||
        left != ymf_load(ymf_bank(d, 0, select) + 18) ||
        right != ymf_load(ymf_bank(d, 1, select) + 18)) return -EAGAIN;
    if (left >= YMF_FRAMES || right >= YMF_FRAMES) return -EIO;
    if (left != right) return -EAGAIN;
    *frame = left; return 0;
}
static bool ymf_ack(struct audio_dma_device *d)
{
    uint32_t status = audio_hw_read(d, YMF_STATUS, 4);
    if (status == UINT32_MAX || !(status & YMF_BANK_IRQ)) return false;
    audio_hw_write(d, YMF_STATUS, YMF_BANK_IRQ, 4);
    if (d->state.running && !d->inhibited && !d->state.quarantine)
        audio_hw_write(d, YMF_MODE, 3, 4);
    return true;
}
static int ymf_prepare(struct audio_dma_device *d)
{
    /* Refuse another function's live service before any controller write. */
    uint32_t legacy = audio_pci_read(&d->io, &d->pci, 0x40);
    uint32_t power = audio_pci_read(&d->io, &d->pci, 0x54);
    uint32_t mode = audio_hw_read(d, YMF_MODE, 4), status = audio_hw_read(d, YMF_STATUS, 4);
    if (legacy == UINT32_MAX || power == UINT32_MAX || mode == UINT32_MAX || status == UINT32_MAX) {
        d->reason = "firmware_state_unreadable"; return -EIO;
    }
    if ((power & 3) || (!(legacy & 0x8000) && (legacy & 0x1f)) || (mode & 3) || (status & 2) ||
        audio_hw_read(d, YMF_REC_MAP, 4) || audio_hw_read(d, YMF_EFF_MAP, 4) ||
        audio_hw_read(d, YMF_TIMER, 2) || audio_hw_read(d, 0x04, 2) ||
        audio_hw_read(d, 0x52, 2) || (audio_hw_read(d, YMF_SECONDARY, 2) & 0x0330)) {
        d->reason = "firmware_function_active"; return -EBUSY;
    }
    const uint32_t *control = ymf_ds1e(d) ? CntrlInst1E : CntrlInst;
    int e = ymf_firmware_check(DspInst, sizeof(DspInst), "79ed987978fc99ba999c537f401584ca952af9e4d77367f7b1da8b034f06162b");
    if (!e) e = ymf_firmware_check(control, YDSXG_CTRLLENGTH, ymf_ds1e(d) ?
        "399f62b9539ff4aba84e934752c80b57826ad16b9b606424f1aa986f42afe9e2" :
        "cd6dcf547cd70882e42b901dbc9940c5db9edcf078124092e2aec4d9320bf526");
    if (e) { d->reason = "microcode_hash"; return e; }
    d->saved_legacy = legacy & 0xffff;
    d->ymf_config48 = audio_pci_read(&d->io, &d->pci, 0x48);
    d->ymf_config4c = audio_pci_read(&d->io, &d->pci, 0x4c);
    if (d->ymf_config48 == UINT32_MAX || d->ymf_config4c == UINT32_MAX) return -EIO;
    d->touched = true;
    audio_pci_write(&d->io, &d->pci, 4, (d->command | 2) & ~4u, 2);
    if ((audio_pci_read(&d->io, &d->pci, 4) & 0xffff) != ((d->command | 2u) & ~4u)) return -EIO;
    d->legacy_changed = true;
    audio_pci_write(&d->io, &d->pci, 0x40, (legacy | 0x8000) & ~0x1fu, 2);
    if ((audio_pci_read(&d->io, &d->pci, 0x40) & 0xffff) != ((legacy | 0x8000) & 0xffe0)) return -EIO;
    /* All native/legacy/secondary users were accounted for. AC-link reset
     * follows the donor; power is restored only after a proved shutdown. */
    d->ymf_link_changed = true;
    uint8_t ctrl = d->ymf_config48;
    audio_pci_write(&d->io, &d->pci, 0x48, ctrl & ~3u, 1);
    audio_pci_write(&d->io, &d->pci, 0x48, ctrl | 3u, 1);
    d->io.sleep(d->io.cookie, 1);
    audio_pci_write(&d->io, &d->pci, 0x48, ctrl & ~3u, 1);
    audio_pci_write(&d->io, &d->pci, 0x4a, 0, 2);
    audio_pci_write(&d->io, &d->pci, 0x4e, 0, 2);
    if ((audio_pci_read(&d->io, &d->pci, 0x48) & 0xffff0003) ||
        (audio_pci_read(&d->io, &d->pci, 0x4c) >> 16)) return -EIO;
    audio_hw_write(d, YMF_DAC_VOLUME, 0, 4);
    audio_hw_write(d, YMF_CONFIG, 0, 4);
    e = audio_hw_wait(d, YMF_STATUS, 2, 0, 500);
    if (!e && audio_hw_read(d, YMF_CONFIG, 4)) e = -EIO;
    if (e) return e;
    audio_hw_write(d, YMF_MODE, 0x10000, 4);
    audio_hw_write(d, YMF_MODE, 0, 4);
    audio_hw_write(d, YMF_REC_MAP, 0, 4); audio_hw_write(d, YMF_EFF_MAP, 0, 4);
    e = ymf_bases(d, false);
    audio_hw_write(d, YMF_GLOBAL, audio_hw_read(d, YMF_GLOBAL, 2) & ~7u, 2);
    audio_hw_write(d, YMF_TIMER, 0, 2);
    audio_hw_write(d, YMF_SPDIF_OUT, 0, 2); audio_hw_write(d, YMF_SPDIF_IN, 0, 2);
    for (unsigned reg = 0x80; reg < 0xc0; reg += 4) audio_hw_write(d, reg, 0, 4);
    if (!e) e = ymf_upload(d, YMF_DSP_RAM, DspInst, YDSXG_DSPLENGTH);
    if (!e) e = ymf_upload(d, YMF_CTRL_RAM, control, YDSXG_CTRLLENGTH);
    if (e) return e;
    audio_hw_write(d, YMF_CONFIG, 1, 4);
    d->io.sleep(d->io.cookie, 1);
    if (audio_hw_read(d, YMF_CONFIG, 4) != 1 || audio_hw_read(d, YMF_PLAY_SIZE, 4) != 30 ||
        audio_hw_read(d, YMF_REC_SIZE, 4) != 4 || audio_hw_read(d, YMF_EFF_SIZE, 4) != 4) {
        d->reason = "unsupported_control_bank_size"; return -EIO;
    }
    e = audio_dma_codec_prepare(d, ymf_codec_read, ymfpci_codec_write);
    if (!e) e = ymf_mute(d, audio.master.mute);
    if (!e) d->state.rate = AUDIO_RATE;
    return e;
}
int ymfpci_release(struct audio_dma_device *d)
{
    if (d->state.quarantine) return -EIO;
    if (d->ymf_link_changed) {
        audio_pci_write(&d->io, &d->pci, 0x4a, d->ymf_config48 >> 16, 2);
        audio_pci_write(&d->io, &d->pci, 0x4e, d->ymf_config4c >> 16, 2);
        audio_pci_write(&d->io, &d->pci, 0x48, d->ymf_config48 & 255, 1);
        if (audio_pci_read(&d->io, &d->pci, 0x48) != d->ymf_config48 ||
            audio_pci_read(&d->io, &d->pci, 0x4c) != d->ymf_config4c) return -EIO;
        d->ymf_link_changed = false;
    }
    if (d->legacy_changed) {
        audio_pci_write(&d->io, &d->pci, 0x40, d->saved_legacy, 2);
        if ((audio_pci_read(&d->io, &d->pci, 0x40) & 0xffff) != d->saved_legacy) return -EIO;
        d->legacy_changed = false;
    }
    return 0;
}
const struct audio_dma_ops ymfpci_ops = {
    ymf_prepare, ymf_start, ymf_halt, ymf_cursor, ymf_mute, ymf_ack, ymf_inhibit
};
