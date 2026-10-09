/* Compile the production backend against a bounded model of documented BCI.
 * This verifies protocol, bounds and ownership; it is not S3 hardware testing. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "session_gpu_savage.h"

extern void cvsavage_test_reset_discovery(void);

#define MMIO_PHYS 0x10000000U
#define FB_PHYS 0x20000000U
#define AP_PHYS 0x30000000U
#define VRAM_BYTES (16U * 1024U * 1024U)
#define USABLE_BYTES (237U * 65536U)
static uint32_t registers[0x80000U / 4U], original_registers[0x80000U / 4U];
static uint8_t video[VRAM_BYTES], crtc[256], original_crtc[256];
static uint8_t cr_index, seq_index, sequencer[256];
static uint32_t pci_address, probe_physical, command[32], command_count;
static uint32_t pci_reads, pci_writes;
static uint32_t present, bad_bar, memory_disabled, map_failure, busy;
static uint32_t suppress_fill, suppress_triangle, writes, reads, unmaps;
static uint32_t draws, copies, fills, max_video_offset, tested;
static uint32_t fifo_stuck_after_fill, busy_after_draw;
static uint32_t suppress_copy, other_pci, seq_locked, seq_unstable, seq61_reads;
static uint32_t cr_data_writes, seq_data_writes;
static uint32_t pci_class, bar_flags[3], physical_profile, mmio_override;
static uint32_t aperture_unassigned, ignore_cr_index;
static uint32_t vga_reads, vga_writes, map_attempts;
static uint32_t state3d_reads, state3d_writes, mix16_reads, mix16_writes;
static uint32_t setup_fault, busy_on_setup_fault;
static uint32_t reject_bci_gbd, bci_setup_busy, bci_setup_fifo_busy;
static uint32_t bci_restore_busy, bci_restore_fifo_busy, restore_status_reads;
static uint32_t ignore_final_mmio_gbd, final_gbd_mmio_writes;
static uint32_t bci_setup_e1_count, bci_restore_e1_count;
static uint32_t retrace_timeout, retrace_reads, retrace_phase, retrace_active;
static uint32_t invalid_2d_read32, rejected_gbd16_offset, rejected_cr66_enable;
static uint32_t engine_disabled_commands, cr66_bit_changes, cr66_enable_writes;
static uint32_t cr66_disable_writes;
static uint32_t engine16_reads, engine16_writes;
static uint32_t fail_gbd_half_once, gbd_programmed, gbd_half_failed;
static uint32_t busy_after_cr66_enable, retrace_timeout_after_cr66_enable;
static uint32_t ignore_cr50_depth, pbd_readback_fault, pixel_readback_fault;
static uint32_t xrgb_high_byte;
static uint32_t descriptor_seed_count, bypass_descriptor_contract, hypothesis_prior_colour;
static uint32_t corrupt_foreground_colour;
static uint32_t misc16_reads, misc16_writes, mmio32_engine_writes;
static uint32_t reject_misc_restore;
static uint32_t posted_mmio, posted_count, posted_completions, posted_busy;
static uint32_t misc_reserved_xor, misc_restore_stalls;
static uint32_t dword_restore_stalls, control_restore_stalls;
static uint32_t rsf_read_unstable, rsf_access_changes, packed_fills, packed_copies;
static uint32_t bci_only_corrupt, packed_corrupt, packed_busy, packed_command;
static uint32_t final_rsf_stalls;
static uint32_t mapped_misc_reads;
static uint32_t invalid_packed_commands;
static struct { uint32_t offset, value, word; } posted[64];
static void execute_packed(uint32_t value);

/* MMIO writes may be accepted before the engine register bank reflects them.
 * Complete the ordered queue only through a bounded engine-status poll.
 * This timing fixture does not claim to emulate undocumented 8C2E reads. */
static void mmio_commit(uint32_t offset, uint32_t value, uint32_t word)
{
    uint32_t shift = (offset & 2U) * 8U;
    if (!word && (offset == 0x8120U || offset == 0x8124U) &&
        (corrupt_foreground_colour || packed_corrupt) && value == 0x00123456U)
        value = 0x56565656U;
    if (offset == 0x8118U) {
        packed_command = value;
    } else if (offset == 0x8144U) {
        /* Reserved bits6/10 are not writable firmware payload. */
        registers[offset / 4U] = (registers[offset / 4U] & ~0x0bbfU) | (value & 0x0bbfU);
    } else if (word) {
        registers[offset / 4U] = (registers[offset / 4U] & ~(0xffffU << shift)) |
                                 ((value & 0xffffU) << shift);
    } else registers[offset / 4U] = value;
}

static void mmio_store(uint32_t offset, uint32_t value, uint32_t word)
{
    if (!posted_mmio) { mmio_commit(offset, value, word); return; }
    assert(posted_count < sizeof(posted) / sizeof(posted[0]));
    posted[posted_count].offset = offset;
    posted[posted_count].value = value;
    posted[posted_count++].word = word;
    if (offset == 0x8144U && misc16_writes > 1U && misc_restore_stalls)
        posted_busy = misc_restore_stalls;
    if (offset == 0x8144U && misc16_writes == 3U && final_rsf_stalls)
        posted_busy = final_rsf_stalls;
    if (offset == 0x8128U && dword_restore_stalls) posted_busy = dword_restore_stalls;
    if (offset == 0x48c18U && control_restore_stalls &&
        value == original_registers[offset / 4U]) posted_busy = control_restore_stalls;
}

static uint32_t mmio_complete(void)
{
    uint32_t i;
    if (posted_busy) return 1U;
    for (i = 0U; i < posted_count; ++i)
        mmio_commit(posted[i].offset, posted[i].value, posted[i].word);
    if (posted_count) ++posted_completions;
    posted_count = 0U;
    if (packed_command) {
        uint32_t cmd = packed_command; packed_command = 0U;
        execute_packed(cmd);
    }
    return 0U;
}

static uint32_t mmio_base(void)
{
    return mmio_override ? mmio_override : physical_profile ? 0xc0100000U : MMIO_PHYS;
}

static uint32_t framebuffer_base(void)
{
    return physical_profile ? 0xe8000000U : FB_PHYS;
}

static uint32_t pixel_address(uint32_t base, uint32_t descriptor,
                               uint32_t x, uint32_t y)
{
    uint32_t bytes = ((descriptor >> 16) & 0xffU) / 8U;
    uint32_t pitch = (descriptor & 0xffffU) * bytes, offset;
    assert(bytes == 2U || bytes == 4U);
    if (descriptor & 0x01000000U) {
        uint32_t tile_width = 128U / bytes;
        offset = ((y / 16U) * (pitch / 128U) + x / tile_width) * 2048U +
                 (y % 16U) * 128U + (x % tile_width) * bytes;
    } else offset = y * pitch + x * bytes;
    assert(base + offset < USABLE_BYTES);
    if (base + offset + bytes > max_video_offset)
        max_video_offset = base + offset + bytes;
    return base + offset;
}

static uint32_t pixel_get(uint32_t base, uint32_t descriptor,
                           uint32_t x, uint32_t y)
{
    uint32_t value = 0U, bytes = ((descriptor >> 16) & 0xffU) / 8U;
    memcpy(&value, video + pixel_address(base, descriptor, x, y), bytes);
    return value;
}

static void pixel_set(uint32_t base, uint32_t descriptor, uint32_t x,
                        uint32_t y, uint32_t value)
{
    uint32_t bytes = ((descriptor >> 16) & 0xffU) / 8U;
    /* Keep the explicit replication experiment's raw DWORD; ordinary model
     * writes keep their existing zero-X convention. Neither byte is proof. */
    if (bytes == 4U && !hypothesis_prior_colour && !corrupt_foreground_colour)
        value &= 0x00ffffffU;
    memcpy(video + pixel_address(base, descriptor, x, y), &value, bytes);
}

static float word_float(uint32_t value)
{
    float number;
    memcpy(&number, &value, 4U);
    return number;
}

static float edge(float ax, float ay, float bx, float by, float x, float y)
{
    return (x - ax) * (by - ay) - (y - ay) * (bx - ax);
}

/* Decode the ROP truth table independently of the production command word.
 * Its bit index is [pattern, source, destination], as in the upstream ROP
 * table: CC copies source, F0 copies pattern, AA preserves destination. */
static uint32_t ternary_rop(uint32_t rop, uint32_t pattern,
                             uint32_t source, uint32_t dest)
{
    uint32_t bit, index, result = 0U;
    for (bit = 0U; bit < 32U; ++bit) {
        index = (((pattern >> bit) & 1U) << 2) |
                (((source >> bit) & 1U) << 1) | ((dest >> bit) & 1U);
        result |= ((rop >> index) & 1U) << bit;
    }
    return result;
}

/* Independent packed-MMIO decoder: operation/source/destination are decoded
 * from command fields, not compared with production's command constants. */
static void execute_packed(uint32_t cmd)
{
    uint32_t op = (cmd >> 13) & 7U, source = (cmd >> 16) & 3U;
    uint32_t dest = (cmd >> 18) & 3U, i, x, y, dx, dy, width, height;
    uint32_t bases[3], descriptors[3], colour;
    /* Vendor MM8118 bit0 is mandatory. Invalid commands complete without
     * drawing; this independent contract must expose a retained sentinel. */
    if (!(cmd & 1U)) { ++invalid_packed_commands; return; }
    assert((cmd & 0x00b0U) == 0x00b0U && (crtc[0x66] & 1U));
    assert((registers[0x8144U / 4U] & 0x0bbfU) == 0x0a00U);
    for (i = 0U; i < 3U; ++i) {
        bases[i] = registers[(0x8168U + i * 8U) / 4U];
        descriptors[i] = registers[(0x816cU + i * 8U) / 4U];
    }
    assert(dest < 3U && source < 3U);
    x = registers[0x8100U / 4U] >> 16; y = registers[0x8100U / 4U] & 65535U;
    dx = registers[0x8108U / 4U] >> 16; dy = registers[0x8108U / 4U] & 65535U;
    width = (registers[0x8148U / 4U] >> 16) + 1U;
    height = (registers[0x8148U / 4U] & 65535U) + 1U;
    assert(width * height <= 65536U);
    if (op == 2U) {
        assert(registers[0x8134U / 4U] == 0x00270027U);
        colour = registers[0x8124U / 4U];
        assert(registers[0x8120U / 4U] == colour);
        if (corrupt_foreground_colour || packed_corrupt)
            colour = (colour & 255U) * 0x01010101U;
        for (i = 0U; i < width * height; ++i)
            if (!suppress_fill)
                pixel_set(bases[dest], descriptors[dest], x + i % width,
                          y + i / width, colour);
        ++packed_fills;
    } else {
        assert(op == 6U && registers[0x8134U / 4U] == 0x00670067U);
        for (i = 0U; i < width * height; ++i)
            if (!suppress_copy)
                pixel_set(bases[dest], descriptors[dest], dx + i % width,
                          dy + i / width,
                          pixel_get(bases[source], descriptors[source],
                                    x + i % width, y + i / width));
        ++packed_copies;
    }
    if (packed_busy) busy = 1U;
}

static void execute(void)
{
    uint32_t head, x, y, width, height, i, cursor;
    if (!command_count) return;
    cursor = 0U;
    while (cursor < command_count) {
        const uint32_t *words = command + cursor;
        head = command[cursor];
        if ((head & 0xff000000U) == 0x96000000U) {
            uint32_t count = (head >> 16) & 0xffU, index = head & 0xffffU;
            assert(cursor + count + 1U <= command_count);
            /* BCI register indices E0/E1 alias the GBD low/high MMIO pair.
             * This command path is available before a valid GBD is installed. */
            if (count == 1U && (index == 0xe0U || index == 0xe1U)) {
                uint32_t offset = index == 0xe0U ? 0x8168U : 0x816cU;
                uint32_t value = command[cursor + 1U];
                uint32_t reject = reject_bci_gbd ||
                    (setup_fault == 2U && index == 0xe0U && value == 0U) ||
                    (setup_fault == 3U && index == 0xe1U && value == 0x10200401U);
                if (!(crtc[0x66] & 1U)) {
                    ++engine_disabled_commands;
                } else if (!reject) registers[offset / 4U] = value;
                if (index == 0xe1U && (value & 0x10000000U)) {
                    gbd_programmed = 1U;
                }
                if (index == 0xe1U && value == 0x10200401U) {
                    ++bci_setup_e1_count;
                    if (bci_setup_busy && bci_setup_e1_count >= 2U) busy = 1U;
                }
                if (index == 0xe1U && value == original_registers[0x816cU / 4U]) {
                    ++bci_restore_e1_count;
                    if (bci_restore_busy && bci_restore_e1_count >= 2U) busy = 1U;
                }
            } else {
                assert(count != 0U);
                if (!(crtc[0x66] & 1U)) ++engine_disabled_commands;
                else for (i = 0U; i < count; ++i)
                    registers[(0x4850cU + (index + i) * 4U) / 4U] = command[cursor + 1U + i];
            }
            cursor += count + 1U;
            continue;
        }
        if ((head & 0xff00ffffU) == 0x4b008c00U) {
        uint32_t foreground = words[3];
        /* Independently check the full upstream initialization contract;
         * ordinary pixel decoding must not hide an omitted initial format. */
        if (!fills && !bypass_descriptor_contract) assert(descriptor_seed_count == 4U);
        if (hypothesis_prior_colour) {
            uint32_t prior_bpp = (registers[0x8174U / 4U] >> 16) & 255U;
            /* Explicit hypothesis model, not a specified silicon rule:
             * accepting colour with an inherited8-bit descriptor explains
             * the physical lowbyte replication while NEW later reads32. */
            if (prior_bpp == 8U) foreground = (foreground & 255U) * 0x01010101U;
            else if (prior_bpp == 16U) foreground = (foreground & 65535U) * 0x00010001U;
        }
        if (corrupt_foreground_colour || bci_only_corrupt)
            foreground = (foreground & 255U) * 0x01010101U;
        assert(crtc[0x66] & 1U);
        assert((crtc[0x50] & 0xf1U) ==
               (cvsavage_status.bpp == 32U ? 0xf1U : 0xd1U));
        assert(!(crtc[0x31] & 1U));
        assert(registers[0x8168U / 4U] == 0U);
        assert((registers[0x816cU / 4U] & 0x10200001U) ==
               (cvsavage_status.bpp == 32U ? 0x10200001U : 0x10000001U));
        assert(cursor + 6U <= command_count);
        x = words[4] & 0xffffU; y = words[4] >> 16;
        width = words[5] & 0xffffU; height = words[5] >> 16;
        registers[0x8170U / 4U] = words[1];
        registers[0x8174U / 4U] = words[2];
        registers[0x8124U / 4U] = foreground;
        if (!suppress_fill)
            for (i = 0U; i < width * height; ++i) {
                uint32_t dest = pixel_get(words[1], words[2],
                                          x + i % width, y + i / width);
                /* No pattern is submitted by SRC_SOLID / PAT_NONE. A
                 * distinct poison value exposes use of the wrong operand
                 * without assuming an undocumented hardware reset value. */
                pixel_set(words[1], words[2], x + i % width,
                            y + i / width,
                            ternary_rop((head >> 16) & 0xffU,
                                        0x00c39a5aU, foreground, dest));
            }
        ++fills;
        cursor += 6U;
        } else if (head == 0x4bcc0d40U) {
        assert(crtc[0x66] & 1U);
        assert((crtc[0x50] & 0xf1U) ==
               (cvsavage_status.bpp == 32U ? 0xf1U : 0xd1U));
        assert(!(crtc[0x31] & 1U));
        assert(registers[0x8168U / 4U] == 0U);
        assert((registers[0x816cU / 4U] & 0x10200001U) ==
               (cvsavage_status.bpp == 32U ? 0x10200001U : 0x10000001U));
        uint32_t dx = words[6] & 0xffffU, dy = words[6] >> 16;
        assert(cursor + 8U <= command_count);
        x = words[5] & 0xffffU; y = words[5] >> 16;
        width = words[7] & 0xffffU; height = words[7] >> 16;
        registers[0x8170U / 4U] = words[1];
        registers[0x8174U / 4U] = words[2];
        registers[0x8178U / 4U] = words[3];
        registers[0x817cU / 4U] = words[4];
        if (!suppress_copy)
            for (i = 0U; i < width * height; ++i)
                pixel_set(words[1], words[2], dx + i % width, dy + i / width,
                            pixel_get(words[3], words[4], x + i % width, y + i / width));
        ++copies;
        cursor += 8U;
        } else if (head == 0x800300faU) {
        assert(crtc[0x66] & 1U);
        assert((crtc[0x50] & 0xf1U) ==
               (cvsavage_status.bpp == 32U ? 0xf1U : 0xd1U));
        assert(!(crtc[0x31] & 1U));
        assert(registers[0x8168U / 4U] == 0U);
        assert((registers[0x816cU / 4U] & 0x10200001U) ==
               (cvsavage_status.bpp == 32U ? 0x10200001U : 0x10000001U));
        uint32_t dest = registers[0x485dcU / 4U], descriptor, offset;
        uint32_t start = registers[0x485e0U / 4U], end = registers[0x485e4U / 4U];
        float vx[3], vy[3], area;
        assert(cursor + 13U <= command_count);
        assert(start & (1U << 11));
        assert(registers[0x48584U / 4U] == 0x44000010U);
        assert(registers[0x485a8U / 4U] == 0U); /* texture disabled */
        assert(registers[0x485d4U / 4U] == 0U); /* Z disabled */
        offset = ((dest >> 8) & 0x3fffU) * 2048U;
        descriptor = 0x11000000U | ((dest & 0x80000000U) ? 0x00200000U : 0x00100000U);
        descriptor |= (dest & 0x7fU) * ((dest & 0x80000000U) ? 32U : 64U);
        for (i = 0U; i < 3U; ++i) {
            vx[i] = word_float(words[1U + i * 4U]);
            vy[i] = word_float(words[2U + i * 4U]);
        }
        area = edge(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2]);
        if (!suppress_triangle && area != 0.0f) {
            for (y = (start >> 12) & 0xfffU; y <= ((end >> 12) & 0xfffU); ++y)
                for (x = start & 0x7ffU; x <= (end & 0x7ffU); ++x) {
                    float a = edge(vx[1], vy[1], vx[2], vy[2], (float)x + .5f, (float)y + .5f) / area;
                    float b = edge(vx[2], vy[2], vx[0], vy[0], (float)x + .5f, (float)y + .5f) / area;
                    float c = 1.0f - a - b;
                    if (a >= 0.0f && b >= 0.0f && c >= 0.0f) {
                        uint32_t rgb = words[4] & 0xffffffU;
                        if (((descriptor >> 16) & 255U) == 16U)
                            rgb = ((rgb >> 8) & 0xf800U) | ((rgb >> 5) & 0x7e0U) | ((rgb >> 3) & 31U);
                        pixel_set(offset, descriptor, x, y, rgb);
                    }
                }
        }
        ++draws;
        if (busy_after_draw) busy = 1U;
        cursor += 13U;
        } else {
            assert(head == 0xc0020000U || head == 0xc0030000U);
            ++cursor;
        }
    }
    command_count = 0U;
}

uint32_t cvdev_in(uint32_t port, uint32_t size)
{
    (void)size;
    if (port == 0xcf8U || port == 0xcfcU) ++pci_reads;
    if (port == 0x3daU) {
        ++retrace_reads;
        if (retrace_timeout) { retrace_active = 0U; return 0U; }
        retrace_phase ^= 1U;
        retrace_active = retrace_phase;
        return retrace_active ? 8U : 0U;
    }
    if (port == 0x3c3U || port == 0x3ccU || port == 0x3d4U ||
        port == 0x3d5U || port == 0x3c4U || port == 0x3c5U) ++vga_reads;
    if (port == 0x3d4U) return cr_index;
    if (port == 0x3d5U) return crtc[cr_index];
    if (port == 0x3c4U) return seq_index;
    if (port == 0x3c5U) {
        if (seq_locked) return 255U;
        if (seq_index == 0x61U && seq_unstable && seq61_reads++ > 0U)
            return sequencer[seq_index] ^ 1U;
        return sequencer[seq_index];
    }
    if (port == 0x3c3U || port == 0x3ccU) return 1U;
    if (port == 0xcf8U) return pci_address;
    assert(port == 0xcfcU);
    if (!present || (pci_address & 0x00ffff00U) != 0x00010000U) return 0xffffffffU;
    switch (pci_address & 0xfcU) {
    case 0U: return other_pci ? 0x8a225333U : 0x8c2e5333U;
    case 4U: return memory_disabled ? 1U : 3U;
    case 8U: return pci_class;
    case 0x0cU: return 0U;
    case 0x10U: return mmio_base() | bar_flags[0];
    case 0x14U: return (framebuffer_base() + (bad_bar ? 0x100000U : 0U)) | bar_flags[1];
    case 0x18U: return (aperture_unassigned ? 0U :
                       physical_profile ? 0xe4000000U : AP_PHYS) | bar_flags[2];
    default: return 0U;
    }
}

void cvdev_out(uint32_t port, uint32_t value, uint32_t size)
{
    (void)size;
    if (port == 0xcf8U || port == 0xcfcU) ++pci_writes;
    if (port == 0x3daU) return;
    if (port == 0x3c3U || port == 0x3ccU || port == 0x3d4U ||
        port == 0x3d5U || port == 0x3c4U || port == 0x3c5U) ++vga_writes;
    if (port == 0xcf8U) pci_address = value;
    else if (port == 0x3d4U) cr_index = (uint8_t)value;
    else if (port == 0x3c4U) seq_index = (uint8_t)value;
    else if (port == 0x3c5U) { sequencer[seq_index] = (uint8_t)value; ++seq_data_writes; }
    else {
        assert(port == 0x3d5U);
        if (cr_index == 0x66U && (((uint8_t)value ^ crtc[0x66]) & 1U)) {
            assert(retrace_active || (sequencer[0x01U] & 0x20U));
            ++cr66_bit_changes;
            if ((uint8_t)value & 1U) ++cr66_enable_writes;
            else ++cr66_disable_writes;
        }
        if (cr_index == 0x66U && ((uint8_t)value & 1U) && rejected_cr66_enable)
            return;
        if (cr_index != ignore_cr_index) {
            if (cr_index == 0x50U && ignore_cr50_depth)
                crtc[cr_index] = (uint8_t)((value & ~0x30U) | (crtc[cr_index] & 0x30U));
            else crtc[cr_index] = (uint8_t)value;
        }
        if (cr_index == 0x66U && (crtc[0x66] & 1U) &&
            ((uint8_t)value & 1U)) {
            if (busy_after_cr66_enable) busy = 1U;
            if (retrace_timeout_after_cr66_enable) retrace_timeout = 1U;
        }
        ++cr_data_writes;
    }
}

uint32_t cvgpu_map_mmio(uint32_t physical, uint32_t bytes)
{
    ++map_attempts;
    if (map_failure) return 0U;
    if (physical == mmio_base()) {
        assert(bytes == 0x80000U); mapped_misc_reads = 0U; return 1U;
    }
    assert(physical >= framebuffer_base() && physical + bytes <= framebuffer_base() + USABLE_BYTES);
    probe_physical = physical - framebuffer_base();
    return 2U;
}

void cvgpu_unmap_mmio(uint32_t linear)
{
    assert(linear == 1U || linear == 2U); ++unmaps;
}

uint32_t cvsavage_test_read(uint32_t linear, uint32_t offset)
{
    uint32_t value;
    ++reads;
    if (linear == 2U) {
        memcpy(&value, video + probe_physical + offset, 4U);
        if (fills == 1U && !copies && !cvsavage_status.selftests && offset < 16U) {
            if (pixel_readback_fault == offset / 4U + 1U &&
                value == (cvsavage_status.bpp == 32U ? 0x00123456U : 0x5aa55aa5U)) value ^= 1U;
            if (xrgb_high_byte && cvsavage_status.bpp == 32U) value |= 0xff000000U;
        }
        return value;
    }
    assert(linear == 1U && offset < 0x80000U);
    assert(offset != 0x8134U); /* These are separate 16-bit registers. */
    if (offset >= 0x8100U && offset <= 0x817cU) {
        ++invalid_2d_read32;
        return registers[offset / 4U] & 0xffffU;
    }
    if (offset >= 0x48500U && offset < 0x48600U) ++state3d_reads;
    if (offset == 0x48c60U) {
        assert(crtc[0x40] & 1U); /* Firmware MMIO-off is enabled before reads. */
        if (mmio_complete()) return 0x00200020U;
        execute();
        if (bci_restore_fifo_busy && ++restore_status_reads == 2U) busy = 1U;
        if (fifo_stuck_after_fill && fills >= fifo_stuck_after_fill)
            return 0x00e00020U;
        return busy ? 0x00200020U : 0x00e00000U;
    }
    return registers[offset / 4U];
}

void cvsavage_test_write(uint32_t linear, uint32_t offset, uint32_t value)
{
    ++writes;
    if (linear == 2U) { memcpy(video + probe_physical + offset, &value, 4U); return; }
    assert(linear == 1U && offset < 0x80000U);
    assert(offset != 0x8134U);
    /* This source contract is independent of the pixel/BCI colour model:
     * the documented 32B control must enable every direct DWORD 2D write. */
    if (offset >= 0x8100U && offset < 0x8180U) {
        assert(offset != 0x8144U && offset != 0x8146U);
        assert(registers[0x8144U / 4U] & 0x0200U);
        ++mmio32_engine_writes;
    }
    /* The T23 prefix-15 capture failed on the first shadow-pointer write
     * or its replay. This backend must leave that firmware address intact. */
    assert(offset != 0x48c0cU);
    if (offset >= 0x48500U && offset < 0x48600U) ++state3d_writes;
    if (offset == 0x48c18U && (value & 0x0eU) == 8U && bci_setup_fifo_busy)
        busy = 1U;
    if (offset >= 0x10000U && offset < 0x10080U) {
        if (offset == 0x10000U) execute();
        assert((offset - 0x10000U) / 4U == command_count);
        command[command_count++] = value;
    } else {
        /* Hardware which does not latch a setup write must be rejected with
         * its exact predicate/value, and still restore the original state. */
        if (offset == 0x816cU && value == 0x10200401U) ++final_gbd_mmio_writes;
        if ((setup_fault == 1U && offset == 0x48c18U && value == 0x308U) ||
            (setup_fault == 6U && offset == 0x8128U && value == 0xffffffffU) ||
            (setup_fault == 7U && offset == 0x48c18U && value == 0x300U)) {
            if (busy_on_setup_fault) busy = 1U;
            return;
        }
        if (setup_fault >= 9U && setup_fault <= 12U && !fills &&
            offset == 0x8170U + (setup_fault - 9U) * 4U &&
            value == ((setup_fault & 1U) ? 0U : 0x10200400U)) return;
        if (descriptor_seed_count < 4U &&
            offset == 0x8170U + descriptor_seed_count * 4U &&
            value == ((descriptor_seed_count & 1U) ?
                       0x10000000U | (cvsavage_status.bpp << 16) |
                       (cvsavage_status.pitch / (cvsavage_status.bpp / 8U)) : 0U))
            ++descriptor_seed_count;
        if (!(ignore_final_mmio_gbd && offset == 0x816cU && value == 0x10200401U))
            mmio_store(offset, value, 0U);
    }
}

uint16_t cvsavage_test_read16(uint32_t linear, uint32_t offset)
{
    assert(linear == 1U && (offset == 0x8134U || offset == 0x8136U ||
           (offset >= 0x8100U && offset <= 0x817eU)));
    assert(offset != 0x8146U); /* a different, write-only read selector */
    if (offset == 0x8144U) {
        ++misc16_reads;
        {
            uint16_t value = (uint16_t)((registers[offset / 4U] & 0x0fffU) ^
                                       (misc16_writes ? misc_reserved_xor : 0U));
            if (rsf_read_unstable && ++mapped_misc_reads > 1U)
                value = (uint16_t)((value & 0x0fefU) | 0xe000U);
            return value;
        }
    }
    if (offset == rejected_gbd16_offset) return 0U;
    if (offset != 0x8134U && offset != 0x8136U) {
        ++engine16_reads;
        if (rsf_access_changes) registers[0x8144U / 4U] |= 0x10U;
        if (fills == 1U && !copies &&
            ((pbd_readback_fault == 1U && offset == 0x8170U) ||
             (pbd_readback_fault == 2U && offset == 0x8174U)))
            return (uint16_t)(registers[offset / 4U] ^ 1U);
        if (fail_gbd_half_once && gbd_programmed && !gbd_half_failed &&
            offset == 0x816eU) {
            gbd_half_failed = 1U;
            return 0U;
        }
        return (uint16_t)(registers[offset / 4U] >> ((offset & 2U) * 8U));
    }
    ++mix16_reads;
    return (uint16_t)(registers[0x8134U / 4U] >> ((offset & 2U) * 8U));
}

void cvsavage_test_write16(uint32_t linear, uint32_t offset, uint16_t value)
{
    assert(linear == 1U && (offset == 0x8134U || offset == 0x8136U ||
           (offset >= 0x8100U && offset <= 0x817eU)));
    if (offset == 0x8144U) {
        ++misc16_writes;
        assert((value & 0xf000U) == 0xe000U);
        if (setup_fault == 13U && (value & 0x0200U)) return;
        if (reject_misc_restore &&
            (value & 0x0bbfU) == (original_registers[offset / 4U] & 0x0bbfU)) return;
        assert(!(value & 0x0440U));
        mmio_store(offset, value, 1U);
        return;
    }
    if (offset == rejected_gbd16_offset) return;
    if (offset != 0x8134U && offset != 0x8136U) {
        ++engine16_writes;
        mmio_store(offset, value, 1U);
        return;
    }
    ++mix16_writes;
    mmio_store(offset, value, 1U);
}

static void reset(void)
{
    assert(cvsavage_release() == 0U);
    cvsavage_test_reset_discovery();
    memset((void *)&cvsavage_status, 0, sizeof(cvsavage_status));
    memset(registers, 0, sizeof(registers));
    registers[0x48c18U / 4U] = 0x305U;
    registers[0x816cU / 4U] = 0x00200400U;
    memset(crtc, 0, sizeof(crtc)); crtc[0x36] = 6U;
    crtc[0x6b] = 2U;
    crtc[0x31] = 0x21U; crtc[0x40] = 0x40U; crtc[0x38] = 0x13U; crtc[0x39] = 0x17U;
    memcpy(original_registers, registers, sizeof(registers));
    memcpy(original_crtc, crtc, sizeof(crtc));
    memset(video, 0x6a, sizeof(video));
    present = 1U; bad_bar = memory_disabled = map_failure = busy = 0U;
    suppress_fill = suppress_triangle = writes = reads = unmaps = 0U;
    fifo_stuck_after_fill = busy_after_draw = 0U;
    suppress_copy = other_pci = seq_locked = seq_unstable = seq61_reads = 0U;
    cr_data_writes = seq_data_writes = 0U;
    vga_reads = vga_writes = map_attempts = 0U;
    pci_reads = pci_writes = 0U;
    state3d_reads = state3d_writes = mix16_reads = mix16_writes = 0U;
    setup_fault = busy_on_setup_fault = 0U;
    reject_bci_gbd = bci_setup_busy = bci_setup_fifo_busy = 0U;
    bci_restore_busy = bci_restore_fifo_busy = restore_status_reads = 0U;
    bci_setup_e1_count = bci_restore_e1_count = 0U;
    ignore_final_mmio_gbd = final_gbd_mmio_writes = 0U;
    retrace_timeout = retrace_reads = retrace_phase = retrace_active = 0U;
    invalid_2d_read32 = rejected_gbd16_offset = rejected_cr66_enable = 0U;
    engine_disabled_commands = cr66_bit_changes = cr66_enable_writes = 0U;
    cr66_disable_writes = engine16_reads = engine16_writes = 0U;
    fail_gbd_half_once = gbd_programmed = gbd_half_failed = 0U;
    busy_after_cr66_enable = retrace_timeout_after_cr66_enable = 0U;
    ignore_cr50_depth = pbd_readback_fault = pixel_readback_fault = xrgb_high_byte = 0U;
    descriptor_seed_count = bypass_descriptor_contract = hypothesis_prior_colour = 0U;
    corrupt_foreground_colour = 0U;
    misc16_reads = misc16_writes = mmio32_engine_writes = reject_misc_restore = 0U;
    posted_mmio = posted_count = posted_completions = posted_busy = 0U;
    misc_reserved_xor = misc_restore_stalls = 0U;
    dword_restore_stalls = control_restore_stalls = 0U;
    rsf_read_unstable = rsf_access_changes = packed_fills = packed_copies = 0U;
    bci_only_corrupt = packed_corrupt = packed_busy = packed_command = 0U;
    final_rsf_stalls = 0U;
    mapped_misc_reads = 0U;
    invalid_packed_commands = 0U;
    pci_class = 0x03000000U; memset(bar_flags, 0, sizeof(bar_flags));
    physical_profile = mmio_override = aperture_unassigned = 0U; ignore_cr_index = 256U;
    memset(sequencer, 0, sizeof(sequencer));
    sequencer[0x61] = 127U; sequencer[0x69] = 255U; sequencer[0x6e] = 0x20U;
    cr_index = 0x3fU; seq_index = 0x55U;
    draws = copies = fills = max_video_offset = command_count = 0U;
}

static uint32_t bind(uint32_t bpp)
{
    return cvsavage_bind(FB_PHYS, 640U * 480U * (bpp / 8U),
                          640U, 480U, 640U * (bpp / 8U), bpp, USABLE_BYTES);
}

static cvsavage_triangle_packet triangle(void)
{
    cvsavage_triangle_packet p = {
        CVSAVAGE_TRIANGLE_MAGIC, 64U, 0U, 0U,
        {{0x41200000U,0x41200000U,0x3f000000U,0xffff0000U},
         {0x42c80000U,0x41200000U,0x3f000000U,0xffff0000U},
         {0x41200000U,0x42c80000U,0x3f000000U,0xffff0000U}}
    };
    return p;
}

static void posted_prefixes(uint32_t misc)
{
    uint32_t depth, round, prefix, before, result;
    for (depth = 16U; depth <= 32U; depth += 16U) {
        reset(); physical_profile = aperture_unassigned = posted_mmio = 1U;
        bar_flags[1] = bar_flags[2] = 8U;
        registers[0x8144U / 4U] = 0x72590000U | misc;
        registers[0x48c18U / 4U] = 0x30bU;
        crtc[0x50U] = depth == 32U ? 0x30U : 0x10U;
        memcpy(original_registers, registers, sizeof(registers));
        memcpy(original_crtc, crtc, sizeof(crtc));
        /* Repeat exactly as the real qualifier: no reset between prefixes,
         * so incomplete restoration contaminates the next attempt. */
        for (round = 0U; round < 2U; ++round)
            for (prefix = 1U; prefix <= CVSAVAGE_SETUP_CHECKS; ++prefix) {
                before = cvsavage_status.command_words; descriptor_seed_count = 0U;
                result = cvsavage_bind_step(0xe8000000U, 3145728U, 1024U, 768U,
                    depth == 32U ? 4096U : 2048U, depth, USABLE_BYTES, 0x100U + prefix);
                if (result != 1U) fprintf(stderr,
                    "posted depth=%u initial_misc=%03x round=%u prefix=%u result=%u stage=%08x status=%08x pending=%u\n",
                    depth, misc, round, prefix, result, cvsavage_status.error_stage,
                    cvsavage_status.last_status, posted_count);
                assert(result == 1U);
                assert(!posted_count && !posted_busy && !cvsavage_owned() && !cvsavage_status.caps);
                assert(cvsavage_status.command_words - before == (prefix >= 19U ? 16U : 0U));
                assert(!memcmp(registers, original_registers, sizeof(registers)));
                assert(!memcmp(crtc, original_crtc, sizeof(crtc)));
                assert(!state3d_reads && !state3d_writes && !fills && !copies && !draws);
                ++tested;
            }
        assert(posted_completions > 0U);
    }
}

static void rsf_prefixes(void)
{
    uint32_t depth, misc, round, prefix, result;
    for (depth = 16U; depth <= 32U; depth += 16U)
        for (misc = 0U; misc <= 0x210U; misc += misc == 0x10U ? 0x1f0U : 0x10U) {
            reset(); physical_profile = aperture_unassigned = posted_mmio = 1U;
            rsf_read_unstable = rsf_access_changes = 1U;
            bar_flags[1] = bar_flags[2] = 8U;
            registers[0x8144U / 4U] = 0x72590000U | misc;
            memcpy(original_registers, registers, sizeof(registers));
            /* This adversarial selector fixture represents no asserted
             * undocumented8C2E rule: RSF readback need not be stable. */
            for (round = 0U; round < 2U; ++round)
                for (prefix = 1U; prefix <= CVSAVAGE_SETUP_CHECKS; ++prefix) {
                    descriptor_seed_count = 0U;
                    result = cvsavage_bind_step(0xe8000000U, 3145728U, 1024U, 768U,
                        depth == 32U ? 4096U : 2048U, depth, USABLE_BYTES, 0x100U + prefix);
                    if (result != 1U) fprintf(stderr, "RSF depth=%u misc=%03x prefix=%u result=%u stage=%08x\n",
                                              depth, misc, prefix, result, cvsavage_status.error_stage);
                    assert(result == 1U && !cvsavage_owned() && !posted_count);
                    if (memcmp(registers, original_registers, sizeof(registers)))
                        fprintf(stderr, "RSF state depth=%u initial=%03x round=%u prefix=%u actual=%08x expected=%08x\n",
                                depth, misc, round, prefix, registers[0x8144U / 4U], original_registers[0x8144U / 4U]);
                    assert(!memcmp(registers, original_registers, sizeof(registers)));
                    assert(!memcmp(crtc, original_crtc, sizeof(crtc)));
                    assert(!fills && !copies && !draws && !packed_fills && !packed_copies);
                    ++tested;
                }
        }
}

static void packed_paths(void)
{
    uint32_t depth, posting, before, i, expected;
    for (depth = 16U; depth <= 32U; depth += 16U)
        for (posting = 0U; posting <= 1U; ++posting) {
            reset(); posted_mmio = posting; bci_only_corrupt = 1U;
            assert(bind(depth) == 1U && cvsavage_status.caps ==
                   (CVSAVAGE_CAP_FILL | CVSAVAGE_CAP_COPY | CVSAVAGE_CAP_PACKED2D));
            assert(packed_fills == 1U && packed_copies == 1U &&
                   cvsavage_status.selftests == 1U && cvsavage_status.copy_selftests == 1U);
            assert(!cvsavage_status.triangle_selftests && cvsavage_status.triangle_probe_failures == 1U);
            before = cvsavage_status.command_words;
            assert(cvsavage_fill(0U, 8U, 0x1234U, depth / 8U) == 0U);
            expected = depth == 32U ? 0x1234U : 0x12341234U;
            for (i = 0U; i < 8U * (depth / 8U); i += 4U) {
                uint32_t observed; memcpy(&observed, video + i, 4U); assert(observed == expected);
            }
            assert(cvsavage_page_copy(0U, depth * 80U, 8U * (depth / 8U), 1U,
                                       depth * 80U) == 0U);
            assert(!memcmp(video, video + depth * 80U, 8U * (depth / 8U)));
            assert(cvsavage_status.command_words == before && packed_fills == 2U && packed_copies == 2U);
            before = writes;
            assert(cvsavage_fill(3U, 1U, 0U, depth / 8U) == 1U && writes == before);
            assert(cvsavage_page_copy(0U, 8U, 16U, 1U, depth * 80U) == 1U && writes == before);
            assert(cvsavage_release() == 0U && !cvsavage_owned() && !posted_count);
            assert(!memcmp(registers, original_registers, sizeof(registers)));
            assert(!memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
        }
    reset(); bci_only_corrupt = packed_corrupt = 1U;
    assert(bind(32U) == 0U && !cvsavage_owned() && !cvsavage_status.selftests &&
           !cvsavage_status.caps && cvsavage_status.error_stage == 0xf1f10004U);
    assert(!memcmp(registers, original_registers, sizeof(registers))); ++tested;
    reset(); bci_only_corrupt = suppress_copy = 1U;
    assert(cvsavage_bind_step(FB_PHYS, 1228800U, 640U, 480U, 2560U, 32U,
                              USABLE_BYTES, 3U) == 0U);
    assert(!cvsavage_owned() && !cvsavage_status.copy_selftests &&
           (cvsavage_status.error_stage & 0xf70000ffU) == 0xf5000004U);
    assert(!memcmp(registers, original_registers, sizeof(registers))); ++tested;
    reset(); bci_only_corrupt = packed_busy = 1U;
    assert(bind(32U) == 2U && cvsavage_owned() && !unmaps && !cvsavage_status.caps);
    assert(packed_fills == 1U && !packed_copies);
    packed_busy = busy = 0U;
    assert(cvsavage_release() == 0U && !cvsavage_owned()); ++tested;
    reset(); bci_only_corrupt = 1U;
    assert(cvsavage_bind_step(FB_PHYS, 1228800U, 640U, 480U, 2560U, 32U,
                              USABLE_BYTES, 4U) == 0U);
    assert(!cvsavage_owned() && !cvsavage_status.ready &&
           cvsavage_status.error_stage == 9U && cvsavage_status.caps == CVSAVAGE_CAP_PACKED2D);
    before = draws;
    assert(cvsavage_bind_step(FB_PHYS, 1228800U, 640U, 480U, 2560U, 32U,
                              USABLE_BYTES, 5U) == 1U);
    assert(cvsavage_owned() && cvsavage_status.ready && cvsavage_status.caps ==
           (CVSAVAGE_CAP_FILL | CVSAVAGE_CAP_COPY | CVSAVAGE_CAP_PACKED2D));
    assert(draws == before && !cvsavage_status.triangle_selftests &&
           cvsavage_status.selftests == 2U && cvsavage_status.copy_selftests == 2U);
    assert(cvsavage_release() == 0U && !memcmp(registers, original_registers, sizeof(registers))); ++tested;
    reset();
    assert(cvsavage_bind_step(FB_PHYS, 1228800U, 640U, 480U, 2560U, 32U,
                              USABLE_BYTES, 5U) == 0U);
    assert(!cvsavage_owned() && !cvsavage_status.ready && !cvsavage_status.caps && !draws); ++tested;
}

static void command_valid_bit(void)
{
    uint32_t depth, posting, result;
    for (depth = 32U; depth >= 16U; depth -= 16U)
        for (posting = 0U; posting <= 1U; ++posting) {
            reset(); physical_profile = aperture_unassigned = bci_only_corrupt = 1U;
            posted_mmio = posting; bar_flags[1] = bar_flags[2] = 8U;
            crtc[0x50U] = depth == 32U ? 0x30U : 0x10U;
            memcpy(original_crtc, crtc, sizeof(crtc));
            result = cvsavage_bind_step(0xe8000000U, 3145728U, 1024U, 768U,
                depth == 32U ? 4096U : 2048U, depth, USABLE_BYTES, 3U);
            if (result != 1U) fprintf(stderr,
                "MM8118 depth=%u posted=%u result=%u stage=%08x pixel=%08x expected=%08x foreground=%08x invalid_commands=%u\n",
                depth, posting, result, cvsavage_status.error_stage,
                cvsavage_status.triangle_probe_pixel, cvsavage_status.triangle_probe_expected,
                cvsavage_status.last_status, invalid_packed_commands);
            assert(result == 1U && !cvsavage_owned() && !posted_count);
            assert(cvsavage_status.selftests == 1U && cvsavage_status.copy_selftests == 1U &&
                   packed_fills == 1U && packed_copies == 1U && !invalid_packed_commands);
            assert(!memcmp(registers, original_registers, sizeof(registers)));
            assert(!memcmp(crtc, original_crtc, sizeof(crtc)));
            assert(!cvsavage_status.idle_timeouts && !cvsavage_status.fifo_timeouts);
            ++tested;
        }
}

int main(int argc, char **argv)
{
    uint32_t before, value, i, j, depth;
    cvsavage_triangle_packet p;
    assert(sizeof(cvsavage_status_info) == 128U && sizeof(p) == 64U);
    if (argc == 2 && strcmp(argv[1], "--rsf-selector") == 0) {
        rsf_prefixes(); puts("RSF access-selector protocol PASS"); return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--rsf-rollback") == 0) {
        uint32_t result;
        reset(); posted_mmio = rsf_read_unstable = 1U;
        registers[0x8144U / 4U] = 0x10U;
        memcpy(original_registers, registers, sizeof(registers));
        result = cvsavage_bind_step(FB_PHYS, 1228800U, 640U, 480U, 2560U, 32U,
                                    USABLE_BYTES, 0x109U);
        if (result != 1U) fprintf(stderr, "prefix9 result=%u stage=%08x actual=%08x original_stage=%08x\n",
                                  result, cvsavage_status.error_stage, cvsavage_status.last_status,
                                  cvsavage_status.triangle_probe_pixel);
        assert(result == 1U && !cvsavage_owned() && !posted_count);
        assert(!memcmp(registers, original_registers, sizeof(registers)));
        puts("R15 RSF rollback predicate negative control PASS"); return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--packed-path") == 0) {
        packed_paths(); puts("Independent packed-MMIO fill/copy proofs PASS"); return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--command-valid-bit") == 0) {
        command_valid_bit(); puts("MM8118 mandatory-bit contract PASS"); return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--posted-enable") == 0) {
        posted_prefixes(0U); puts("Sequential posted-MMIO prefixes, initial32B=0 PASS"); return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--posted-control") == 0) {
        posted_prefixes(0x200U); puts("Sequential posted-MMIO prefixes, initial32B=1 PASS"); return 0;
    }
    /* This mode is run against the saved R10e production source. It must
     * reproduce an idle stage-4 mismatch, rather than passing the old fill
     * through a decoder which implicitly treats every ROP as SRCCOPY. */
    if (argc == 2 && strcmp(argv[1], "--reproduce-old-fill") == 0) {
        reset(); physical_profile = 1U; aperture_unassigned = 1U;
        /* Isolate the previous ROP defect from the newly checked format
         * contract: this earlier source needs firmware to set its depth. */
        crtc[0x50U] = original_crtc[0x50U] = 0x30U;
        registers[0x8144U / 4U] = original_registers[0x8144U / 4U] = 0x200U;
        bypass_descriptor_contract = 1U;
        assert(cvsavage_bind_step(0xe8000000U, 3145728U, 1024U, 768U,
                                  4096U, 32U, USABLE_BYTES, 3U) == 0U);
        assert(cvsavage_status.error_stage == 4U &&
               cvsavage_status.command_words == 23U &&
               cvsavage_status.idle_timeouts == 0U &&
               cvsavage_status.fifo_timeouts == 0U &&
               cvsavage_status.selftests == 0U &&
               !cvsavage_owned() && cvsavage_status.caps == 0U);
        assert(video[6291456U] == 0x6aU && video[6291471U] == 0x6aU);
        puts("R10e source reproduced: stage 4, 23 words, zero timeouts, private VRAM restored");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--model-old-descriptor") == 0) {
        reset(); physical_profile = aperture_unassigned = 1U;
        crtc[0x50U] = original_crtc[0x50U] = 0x30U;
        registers[0x8144U / 4U] = 0x200U;
        registers[0x8174U / 4U] = registers[0x817cU / 4U] = 0x10080400U;
        memcpy(original_registers, registers, sizeof(registers));
        bypass_descriptor_contract = hypothesis_prior_colour = 1U;
        assert(cvsavage_bind_step(0xe8000000U, 3145728U, 1024U, 768U,
                                  4096U, 32U, USABLE_BYTES, 3U) == 0U);
        assert(cvsavage_status.error_stage == 0xa3f13004U &&
               cvsavage_status.triangle_probe_pixel == 0x56565656U &&
               cvsavage_status.triangle_probe_expected == 0x00123456U &&
               cvsavage_status.command_words == 23U &&
               !cvsavage_status.idle_timeouts && !cvsavage_status.fifo_timeouts &&
               !cvsavage_owned() && !cvsavage_status.caps);
        for (i = 0U; i < 4096U; ++i) assert(video[probe_physical + i] == 0x6aU);
        puts("Hypothesis only: prior8-bit colour parsing reproduces56565656 and restored private VRAM");
        return 0;
    }
    assert(argc == 1);
    assert(ternary_rop(0xccU, 0x89abcdefU, 0x12345678U, 0x5aa5c33cU) == 0x12345678U); ++tested;
    assert(ternary_rop(0xf0U, 0x89abcdefU, 0x12345678U, 0x5aa5c33cU) == 0x89abcdefU); ++tested;
    assert(ternary_rop(0xaaU, 0x89abcdefU, 0x12345678U, 0x5aa5c33cU) == 0x5aa5c33cU); ++tested;
    assert(ternary_rop(0x66U, 0x89abcdefU, 0x12345678U, 0x5aa5c33cU) == (0x12345678U ^ 0x5aa5c33cU)); ++tested;
    /* Match the physical R10e phase-3 input and prove both operations. */
    reset(); physical_profile = aperture_unassigned = 1U;
    assert(cvsavage_bind_step(0xe8000000U, 3145728U, 1024U, 768U,
                              4096U, 32U, USABLE_BYTES, 3U) == 1U);
    assert(cvsavage_status.error_stage == 0U &&
           cvsavage_status.command_words == 32U &&
           cvsavage_status.selftests == 1U && cvsavage_status.copy_selftests == 1U &&
           !cvsavage_status.idle_timeouts && !cvsavage_status.fifo_timeouts &&
           !cvsavage_owned() && !cvsavage_status.caps);
    for (i = 0U; i < 4096U; ++i) assert(video[6291456U + i] == 0x6aU);
    assert(!memcmp(crtc, original_crtc, sizeof(crtc)) &&
           !memcmp(registers, original_registers, sizeof(registers))); ++tested;
    /* Execution depth is an explicit CR50 contract in the full upstream
     * mode initialization, separate from the inline bitmap descriptor.
     * Exercise every inherited depth, retaining unrelated bits and exactly
     * restoring firmware state after the private phase-3 proof. */
    for (depth = 16U; depth <= 32U; depth += 16U)
        for (j = 0U; j < 4U; ++j) {
            reset(); physical_profile = aperture_unassigned = 1U;
            crtc[0x50U] = original_crtc[0x50U] = (uint8_t)(0x0eU | (j << 4));
            assert(cvsavage_bind_step(0xe8000000U, 1024U * 768U * (depth / 8U),
                                      1024U, 768U, 1024U * (depth / 8U),
                                      depth, USABLE_BYTES, 3U) == 1U);
            assert(cvsavage_status.command_words == 32U &&
                   cvsavage_status.selftests == 1U && cvsavage_status.copy_selftests == 1U);
            assert(!memcmp(crtc, original_crtc, sizeof(crtc)) &&
                   !memcmp(registers, original_registers, sizeof(registers)));
            for (i = 0U; i < 4096U; ++i) assert(video[probe_physical + i] == 0x6aU);
            ++tested;
        }
    for (depth = 16U; depth <= 32U; depth += 16U) {
        reset(); ignore_cr50_depth = 1U;
        assert(bind(depth) == 0U && !cvsavage_owned());
        assert(cvsavage_status.error_stage == 0x00040003U &&
               cvsavage_status.last_status == 0xc1U && !fills && !copies && !draws);
        assert(!memcmp(crtc, original_crtc, sizeof(crtc)) &&
               !memcmp(registers, original_registers, sizeof(registers))); ++tested;
        for (j = 1U; j <= 2U; ++j) {
            reset(); physical_profile = aperture_unassigned = 1U;
            pbd_readback_fault = j;
            assert(cvsavage_bind_step(0xe8000000U, 1024U * 768U * (depth / 8U),
                                      1024U, 768U, 1024U * (depth / 8U),
                                      depth, USABLE_BYTES, 3U) == 1U);
            assert(!cvsavage_status.error_stage && cvsavage_status.selftests == 1U &&
                   cvsavage_status.copy_selftests == 1U && !cvsavage_owned());
            assert(!memcmp(crtc, original_crtc, sizeof(crtc)) &&
                   !memcmp(registers, original_registers, sizeof(registers))); ++tested;
        }
        /* Each distinct failed readback must survive rollback. Faults model
         * absent/corrupt readback, not the undiagnosed physical T23 cause. */
        for (j = 1U; j <= 6U; ++j) {
            uint32_t expected, active_cr50 = depth == 32U ? 0xfdU : 0xddU;
            reset(); physical_profile = aperture_unassigned = 1U;
            crtc[0x50U] = original_crtc[0x50U] = 0x1cU;
            if (j <= 2U) { pbd_readback_fault = j; suppress_fill = 1U; }
            else pixel_readback_fault = j - 2U;
            assert(cvsavage_bind_step(0xe8000000U, 1024U * 768U * (depth / 8U),
                                      1024U, 768U, 1024U * (depth / 8U),
                                      depth, USABLE_BYTES, 3U) == 0U);
            expected = depth == 32U ? 0x00123456U : 0x5aa55aa5U;
            assert(cvsavage_status.error_stage ==
                   (CVSAVAGE_PACKED_DIAG_TAG | ((j <= 2U ? 1U : j - 2U) << 24) |
                    (active_cr50 << 16) | 0x1c04U));
            assert(cvsavage_status.last_status == (depth == 32U ? 0x123456U : 0x5aa5U) &&
                   cvsavage_status.triangle_probe_pixel == (j <= 2U ? 0xa55aa55aU : expected ^ 1U) &&
                   cvsavage_status.triangle_probe_expected == expected);
            assert(!cvsavage_owned() && !cvsavage_status.caps && !cvsavage_status.selftests &&
                   cvsavage_status.command_words == 23U &&
                   !cvsavage_status.idle_timeouts && !cvsavage_status.fifo_timeouts);
            assert(!memcmp(crtc, original_crtc, sizeof(crtc)) &&
                   !memcmp(registers, original_registers, sizeof(registers)));
            for (i = 0U; i < 4096U; ++i) assert(video[probe_physical + i] == 0x6aU);
            ++tested;
        }
    }
    reset(); physical_profile = aperture_unassigned = xrgb_high_byte = 1U;
    assert(cvsavage_bind_step(0xe8000000U, 3145728U, 1024U, 768U,
                              4096U, 32U, USABLE_BYTES, 3U) == 1U);
    assert(cvsavage_status.selftests == 1U && cvsavage_status.copy_selftests == 1U);
    for (i = 0U; i < 4096U; ++i) assert(video[probe_physical + i] == 0x6aU);
    ++tested;
    for (depth = 16U; depth <= 32U; depth += 16U)
        for (j = 0U; j < 4U; ++j) {
            static const uint32_t inherited_bpp[] = {0U, 8U, 16U, 32U};
            reset(); physical_profile = aperture_unassigned = 1U;
            registers[0x8170U / 4U] = 0x115500U;
            registers[0x8174U / 4U] = 0x10000400U | (inherited_bpp[j] << 16);
            registers[0x8178U / 4U] = 0x226600U;
            registers[0x817cU / 4U] = 0x10000400U | (inherited_bpp[(j + 1U) % 4U] << 16);
            registers[0x8124U / 4U] = 0xdeca0012U;
            memcpy(original_registers, registers, sizeof(registers));
            hypothesis_prior_colour = 1U;
            assert(cvsavage_bind_step(0xe8000000U, 1024U * 768U * (depth / 8U),
                                      1024U, 768U, 1024U * (depth / 8U),
                                      depth, USABLE_BYTES, 3U) == 1U);
            assert(descriptor_seed_count == 4U && cvsavage_status.selftests == 1U &&
                   cvsavage_status.copy_selftests == 1U && cvsavage_status.command_words == 32U);
            assert(!memcmp(registers, original_registers, sizeof(registers)) &&
                   !memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
        }
    for (j = 9U; j <= 12U; ++j) {
        reset(); setup_fault = j;
        registers[0x8170U / 4U] = registers[0x8178U / 4U] = 0x115500U;
        registers[0x8174U / 4U] = registers[0x817cU / 4U] = 0x10080400U;
        memcpy(original_registers, registers, sizeof(registers));
        assert(cvsavage_bind_step(FB_PHYS, 3145728U, 1024U, 768U,
                                  4096U, 32U, USABLE_BYTES, 2U) == 0U);
        assert(cvsavage_status.error_stage == ((j << 16) | 3U) &&
               cvsavage_status.last_status == ((j & 1U) ? 0x115500U : 0x10080400U));
        assert(!fills && !copies && !draws && !cvsavage_owned());
        assert(!memcmp(registers, original_registers, sizeof(registers)) &&
               !memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    }
    reset(); physical_profile = aperture_unassigned = corrupt_foreground_colour = 1U;
    crtc[0x50U] = original_crtc[0x50U] = 0x30U;
    registers[0x8124U / 4U] = 0xdeca0012U;
    memcpy(original_registers, registers, sizeof(registers));
    assert(cvsavage_bind_step(0xe8000000U, 3145728U, 1024U, 768U,
                              4096U, 32U, USABLE_BYTES, 3U) == 0U);
    assert(cvsavage_status.error_stage == 0xf1f13004U &&
           cvsavage_status.triangle_probe_pixel == 0x56565656U &&
           cvsavage_status.triangle_probe_expected == 0x00123456U &&
           cvsavage_status.last_status == 0x56565656U);
    assert(!memcmp(registers, original_registers, sizeof(registers)) &&
           !memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    reset(); physical_profile = aperture_unassigned = corrupt_foreground_colour = 1U;
    crtc[0x50U] = original_crtc[0x50U] = 0x30U;
    registers[0x8144U / 4U] = original_registers[0x8144U / 4U] = 0x0200U;
    assert(cvsavage_bind_step(0xe8000000U, 3145728U, 1024U, 768U,
                              4096U, 32U, USABLE_BYTES, 3U) == 0U);
    assert(cvsavage_status.error_stage == 0xf9f13004U &&
           cvsavage_status.last_status == 0x56565656U &&
           cvsavage_status.triangle_probe_pixel == 0x56565656U &&
           cvsavage_status.triangle_probe_expected == 0x00123456U);
    assert(!memcmp(registers, original_registers, sizeof(registers)) &&
           !memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    reset(); registers[0x8124U / 4U] = 0xdeca0012U;
    memcpy(original_registers, registers, sizeof(registers));
    assert(bind(32U) == 1U && cvsavage_status.caps == 7U);
    assert(cvsavage_release() == 0U &&
           !memcmp(registers, original_registers, sizeof(registers))); ++tested;
    /* Borrow differing firmware misc flags at both depths without changing
     * comparison/clipping/base controls or the adjacent read-selector word. */
    for (depth = 16U; depth <= 32U; depth += 16U)
        for (j = 0U; j < 6U; ++j) {
            static const uint32_t flags[] = {0U, 0x015fU, 0x0dffU, 0x0bffU, 0x0200U, 0x0fffU};
            reset(); registers[0x8144U / 4U] = 0x72590000U | flags[j];
            registers[0x8124U / 4U] = 0xdeca0012U;
            memcpy(original_registers, registers, sizeof(registers));
            assert(bind(depth) == 1U && cvsavage_status.caps == 7U);
            assert(registers[0x8144U / 4U] == ((original_registers[0x8144U / 4U] | 0x200U) & ~0x10U));
            assert(misc16_reads >= 2U && misc16_writes == 1U && mmio32_engine_writes > 0U);
            assert(cvsavage_release() == 0U && misc16_writes == 3U &&
                   !memcmp(registers, original_registers, sizeof(registers))); ++tested;
        }
    reset(); setup_fault = 13U;
    registers[0x8144U / 4U] = original_registers[0x8144U / 4U] = 0x400U;
    assert(bind(32U) == 0U && !cvsavage_owned());
    assert(cvsavage_status.error_stage == 0x000d0003U && cvsavage_status.last_status == 0x400U);
    assert(!mmio32_engine_writes && !cvsavage_status.command_words && !fills && !copies && !draws);
    assert(!memcmp(registers, original_registers, sizeof(registers)) &&
           !memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    reset(); assert(bind(32U) == 1U); reject_misc_restore = 1U;
    assert(cvsavage_release() == 1U && cvsavage_owned() && !unmaps &&
           cvsavage_status.error_stage == 0xe3000008U && cvsavage_status.last_status == 0x200U);
    before = mmio32_engine_writes; value = cvsavage_status.command_words;
    reject_misc_restore = 0U;
    assert(cvsavage_release() == 0U && !cvsavage_owned() &&
           mmio32_engine_writes == before && cvsavage_status.command_words == value &&
           !memcmp(registers, original_registers, sizeof(registers)) &&
           !memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    /* Reserved bits can read differently without invalidating restoration;
     * every documented bit and the adjacent selector word remains intact. */
    for (depth = 16U; depth <= 32U; depth += 16U) {
        reset(); registers[0x8144U / 4U] = 0x72590440U;
        memcpy(original_registers, registers, sizeof(registers)); misc_reserved_xor = 0x440U;
        assert(bind(depth) == 1U && cvsavage_status.caps == 7U);
        assert(cvsavage_release() == 0U && !cvsavage_owned() &&
               !memcmp(registers, original_registers, sizeof(registers))); ++tested;
    }
    posted_prefixes(0U); posted_prefixes(0x200U);
    rsf_prefixes(); packed_paths(); command_valid_bit();
    for (i = 1U; i <= 4U; ++i) {
        uint32_t cause = i == 4U ? 0xc3f13004U : 0x00070003U;
        reset(); posted_mmio = 1U; assert(bind(32U) == 1U);
        cvsavage_status.error_stage = cause; cvsavage_status.last_status = 0x30bU;
        if (i == 1U) dword_restore_stalls = 1U;
        if (i == 2U) misc_restore_stalls = 1U;
        if (i == 3U) reject_misc_restore = 1U;
        if (i == 4U) control_restore_stalls = 1U;
        assert(cvsavage_release() == 1U && cvsavage_owned() && !unmaps && !cvsavage_status.caps);
        assert(cvsavage_status.error_stage == (0xe0000008U | i << 24));
        assert(cvsavage_status.triangle_probe_pixel == cause &&
               cvsavage_status.triangle_probe_expected == 0x30bU);
        if (i == 3U) {
            assert(cvsavage_status.last_status == 0x200U);
            /* Repeated readback rejection retains the original cause. */
            assert(cvsavage_release() == 1U && cvsavage_status.triangle_probe_pixel == cause &&
                   cvsavage_status.triangle_probe_expected == 0x30bU);
        } else assert(cvsavage_status.last_status == 0x00200020U && cvsavage_status.idle_timeouts);
        before = mmio32_engine_writes; value = cvsavage_status.command_words;
        posted_busy = misc_restore_stalls = dword_restore_stalls = control_restore_stalls = 0U;
        reject_misc_restore = 0U;
        assert(cvsavage_release() == 0U && !cvsavage_owned() && !posted_count &&
               before == mmio32_engine_writes && value == cvsavage_status.command_words);
        assert(!memcmp(registers, original_registers, sizeof(registers)) &&
               !memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    }
    reset(); posted_mmio = 1U; assert(bind(32U) == 1U); final_rsf_stalls = 1U;
    assert(cvsavage_release() == 1U && cvsavage_owned() && !unmaps &&
           cvsavage_status.error_stage == 0xe5000008U && posted_count);
    final_rsf_stalls = posted_busy = 0U;
    assert(cvsavage_release() == 0U && !cvsavage_owned() && !posted_count &&
           !memcmp(registers, original_registers, sizeof(registers))); ++tested;
    reset(); present = 0U; assert(bind(32U) == 0U && writes == 0U);
    assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_MISSING));
    assert(cvsavage_status.last_status == FB_PHYS && cvsavage_status.width == 640U);
    assert(cvsavage_status.height == 480U && cvsavage_status.pitch == 2560U &&
           cvsavage_status.bpp == 32U && cr_data_writes == 0U); ++tested;
    reset(); bad_bar = 1U; assert(bind(32U) == 0U && writes == 0U);
    assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_PHYSICAL));
    assert(cvsavage_status.last_status == FB_PHYS &&
           cvsavage_status.framebuffer_physical == FB_PHYS + 0x100000U); ++tested;
    reset(); memory_disabled = 1U; assert(bind(32U) == 0U && writes == 0U);
    assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_DECODE));
    assert(cvsavage_status.last_status == 1U); ++tested;
    reset(); map_failure = 1U; assert(bind(32U) == 0U && !cvsavage_owned()); ++tested;
    reset(); assert(cvsavage_bind(FB_PHYS, 1228800U, 640U, 480U, 2560U, 32U, 0U) == 0U); ++tested;
    reset(); assert(cvsavage_bind(FB_PHYS, 1228800U, 640U, 480U, 2560U, 32U, VRAM_BYTES + 1U) == 0U); ++tested;
    reset(); assert(bind(24U) == 0U && writes == 0U);
    assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_FORMAT));
    assert(cvsavage_status.bpp == 24U && cvsavage_status.last_status == 921600U); ++tested;
    reset(); assert(cvsavage_bind(FB_PHYS, 1228800U, 0U, 480U, 2560U, 32U,
                                 USABLE_BYTES) == 0U);
    assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_GEOMETRY));
    assert(cvsavage_status.last_status == 1228800U && writes == 0U); ++tested;
    reset(); assert(cvsavage_bind(FB_PHYS, 1228800U, 640U, 480U, 2559U, 32U,
                                 USABLE_BYTES) == 0U);
    assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_PITCH));
    assert(cvsavage_status.pitch == 2559U && writes == 0U); ++tested;
    reset(); assert(cvsavage_bind(FB_PHYS, 1228799U, 640U, 480U, 2560U, 32U,
                                 USABLE_BYTES) == 0U);
    assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_EXTENT));
    assert(cvsavage_status.last_status == 1228799U && writes == 0U); ++tested;
    reset(); assert(cvsavage_bind(0xfffff000U, 1228800U, 640U, 480U, 2560U, 32U,
                                 USABLE_BYTES) == 0U);
    assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_EXTENT) &&
           writes == 0U && cr_data_writes == 0U); ++tested;
    reset(); pci_class = 0x03800005U; assert(bind(32U) == 0U);
    assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_CLASS));
    assert(cvsavage_status.last_status == pci_class && writes == 0U); ++tested;
    for (i = 0U; i < 2U; ++i) {
        reset(); bar_flags[i] = 4U; assert(bind(32U) == 0U);
        assert(cvsavage_status.error_stage == (1U | (CVSAVAGE_PREFLIGHT_BAR0 << i)));
        assert((cvsavage_status.last_status & 7U) == 4U && writes == 0U &&
               cr_data_writes == 0U && seq_data_writes == 0U); ++tested;
    }
    reset(); mmio_override = 0xfffffff0U; assert(bind(32U) == 0U);
    assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_MMIO_SPAN));
    assert(cvsavage_status.last_status == 0xfffffff0U && writes == 0U); ++tested;
    reset(); physical_profile = 1U; bar_flags[1] = bar_flags[2] = 8U;
    assert(cvsavage_bind(0xe8000000U, 3145728U, 1024U, 768U, 4096U,
                         32U, USABLE_BYTES) == 1U);
    assert(cvsavage_status.caps == 7U && cvsavage_status.width == 1024U &&
           cvsavage_status.height == 768U && cvsavage_status.pitch == 4096U);
    assert(cvsavage_status.framebuffer_physical == 0xe8000000U &&
           cvsavage_status.mmio_physical == 0xc0100000U &&
           cvsavage_status.aperture_physical == 0xe4000000U);
    assert(cvsavage_status.scratch_offset == 3145728U &&
           max_video_offset < USABLE_BYTES && cvsavage_status.error_stage == 0U); ++tested;
    /* Strict BAR2 preflight: absent, I/O, 64-bit and all-ones resources are
     * rejected before VGA state, MMIO mapping or BCI access. */
    for (i = 0U; i < 4U; ++i) {
        reset(); aperture_unassigned = 1U;
        bar_flags[2] = i == 0U ? 0U : i == 1U ? 1U : i == 2U ? 4U : 0xffffffffU;
        assert(bind(32U) == 0U);
        assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_BAR2));
        assert(cvsavage_status.last_status == bar_flags[2] && writes == 0U && reads == 0U &&
               map_attempts == 0U && vga_reads == 0U && vga_writes == 0U && cr_data_writes == 0U &&
               seq_data_writes == 0U && !cvsavage_owned() &&
               cvsavage_status.caps == 0U); ++tested;
    }
    reset(); aperture_unassigned = 1U; bar_flags[2] = 0x000f0008U;
    assert(bind(32U) == 0U &&
           cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_BAR2) &&
           cvsavage_status.last_status == 0x000f0008U && writes == 0U && reads == 0U &&
           map_attempts == 0U && vga_reads == 0U && vga_writes == 0U && cr_data_writes == 0U &&
           seq_data_writes == 0U && !cvsavage_owned()); ++tested;
    /* Captured T23 diskseq61 tuple: C0200000 / E8000008 / 00000008.
     * Preserve the raw failure and prove rejection precedes every VGA/MMIO
     * or engine access. */
    reset(); physical_profile = aperture_unassigned = 1U;
    mmio_override = 0xc0200000U; bar_flags[1] = bar_flags[2] = 8U;
    assert(cvsavage_bind(0xe8000000U, 3145728U, 1024U, 768U, 4096U,
                         32U, USABLE_BYTES) == 0U);
    assert(cvsavage_status.error_stage == (1U | CVSAVAGE_PREFLIGHT_BAR2) &&
           cvsavage_status.last_status == 8U &&
           cvsavage_status.mmio_physical == 0xc0200000U &&
           cvsavage_status.framebuffer_physical == 0xe8000000U &&
           writes == 0U && reads == 0U && map_attempts == 0U && vga_reads == 0U && vga_writes == 0U &&
           cr_data_writes == 0U && seq_data_writes == 0U && !cvsavage_owned() &&
           cvsavage_status.caps == 0U); ++tested;
    /* Post-boot proof accepts precisely the unassigned 32-bit tiled BAR.
     * Each early phase restores all register and VRAM bytes, no screen writes. */
    for (i = 1U; i <= 4U; ++i) {
        uint32_t j;
        reset(); physical_profile = aperture_unassigned = 1U;
        mmio_override = 0xc0200000U; bar_flags[1] = bar_flags[2] = 8U;
        assert(cvsavage_bind_step(0xe8000000U, 3145728U, 1024U, 768U,
                                  4096U, 32U, USABLE_BYTES, i) == 1U);
        assert(cvsavage_status.aperture_physical == 0U);
        if (i < 4U) {
            assert(!cvsavage_owned() && !cvsavage_status.caps);
            assert(!memcmp(registers, original_registers, sizeof(registers)));
            assert(!memcmp(crtc, original_crtc, sizeof(crtc)));
        } else assert(cvsavage_status.caps == 7U && cvsavage_owned());
        for (j = 0U; j < (i < 4U ? VRAM_BYTES : 3145728U); ++j)
            assert(video[j] == 0x6aU);
        if (i == 1U) assert(!cvsavage_status.command_words);
        if (i == 2U) assert(cvsavage_status.command_words == 16U);
        if (i < 4U) assert(!state3d_reads && !state3d_writes);
        if (i == 2U) assert(map_attempts == 1U && mix16_reads == 2U && mix16_writes == 4U);
        ++tested;
    }
    /* Each durable prefix starts from the real diskseq63 PCI tuple and
     * returns exactly to firmware state without touching BAR1 or 3D state. */
    for (i = 1U; i <= CVSAVAGE_SETUP_CHECKS; ++i) {
        uint32_t j;
        reset(); physical_profile = aperture_unassigned = 1U;
        bar_flags[1] = bar_flags[2] = 8U;
        for (j = 0U; j < sizeof(registers) / sizeof(registers[0]); ++j)
            registers[j] = j * 0x12345U;
        registers[0x8144U / 4U] &= 0xffff0fffU; /* only lower 12 bits are state */
        memcpy(original_registers, registers, sizeof(registers));
        assert(cvsavage_bind_step(0xe8000000U, 3145728U, 1024U, 768U,
                                  4096U, 32U, USABLE_BYTES, 0x100U + i) == 1U);
        assert(!cvsavage_owned() && !cvsavage_status.caps);
        assert(cvsavage_status.command_words == (i >= 19U ? 16U : 0U));
        assert(!state3d_reads && !state3d_writes && map_attempts == 1U);
        assert(!memcmp(registers, original_registers, sizeof(registers)));
        assert(!memcmp(crtc, original_crtc, sizeof(crtc)));
        for (j = 0U; j < VRAM_BYTES; ++j) assert(video[j] == 0x6aU);
        assert(!fills && !copies && !draws);
        ++tested;
    }
    /* Firmware may have an enabled nonzero shadow pointer. Disable updates
     * through CONTROL only; never rewrite that pointer, including release
     * after commands or a failed private self-test. */
    for (i = 0U; i < 3U; ++i) {
        reset();
        registers[0x48c0cU / 4U] = 0x34567001U;
        registers[0x48c18U / 4U] = 0x30bU;
        memcpy(original_registers, registers, sizeof(registers));
        if (i == 2U) suppress_fill = 1U;
        assert(bind(i ? 32U : 16U) == (i == 2U ? 0U : 1U));
        assert(registers[0x48c0cU / 4U] == 0x34567001U);
        if (i < 2U) assert((registers[0x48c18U / 4U] & 0x0eU) == 8U);
        assert(cvsavage_release() == 0U);
        assert(!memcmp(registers, original_registers, sizeof(registers)));
        assert(!memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    }
    reset(); aperture_unassigned = 1U; bar_flags[2] = 4U;
    assert(cvsavage_bind_step(FB_PHYS, 1228800U, 640U, 480U, 2560U,
                              32U, USABLE_BYTES, 1U) == 0U);
    assert(!writes && !reads && !vga_writes && !map_attempts); ++tested;
    reset(); aperture_unassigned = 1U; bar_flags[2] = 8U; suppress_copy = 1U;
    assert(cvsavage_bind_step(FB_PHYS, 1228800U, 640U, 480U, 2560U,
                              32U, USABLE_BYTES, 3U) == 0U);
    assert(!cvsavage_owned() && !cvsavage_status.caps); ++tested;
    reset(); aperture_unassigned = 1U; bar_flags[2] = 8U; suppress_triangle = 1U;
    assert(cvsavage_bind_step(FB_PHYS, 1228800U, 640U, 480U, 2560U,
                              32U, USABLE_BYTES, 4U) == 0U);
    assert(!cvsavage_owned() && !cvsavage_status.caps); ++tested;
    for (i = 1U; i <= 7U; ++i) {
        static const uint32_t observed[] = {
            0U, 0x300U, 0x2220U, 0x10000001U, 0U, 0x21U, 0x11223344U, 0x305U
        };
        reset(); setup_fault = i;
        if (i == 4U || i == 5U) ignore_cr_index = i == 4U ? 0x50U : 0x31U;
        if (i == 2U) registers[0x8168U / 4U] = 0x2220U;
        if (i == 6U) registers[0x8128U / 4U] = 0x11223344U;
        memcpy(original_registers, registers, sizeof(registers));
        assert(cvsavage_bind_step(FB_PHYS, 3145728U, 1024U, 768U, 4096U,
                                  32U, USABLE_BYTES, i == 7U ? 0x10fU : 0x115U) == 0U);
        assert(!cvsavage_owned());
        assert(cvsavage_status.error_stage == ((i << 16) | 3U));
        assert(cvsavage_status.last_status == observed[i]);
        assert(cvsavage_status.command_words ==
               ((i >= 2U && i <= 6U) ? 16U : 0U));
        assert(!memcmp(registers, original_registers, sizeof(registers)));
        assert(!memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    }
    /* A rejected BCI descriptor is caught by MM816C readback. Since no
     * descriptor was installed, BCI rollback must retain ownership until it
     * can restore the temporary MM816C value and verify firmware state. */
    reset(); reject_bci_gbd = 1U;
    assert(bind(32U) == 2U && cvsavage_owned());
    assert(cvsavage_status.command_words == 16U);
    assert(!draws && !fills && !copies);
    reject_bci_gbd = 0U;
    assert(cvsavage_release() == 0U && !cvsavage_owned());
    assert(!memcmp(registers, original_registers, sizeof(registers)));
    assert(!memcmp(crtc, original_crtc, sizeof(crtc)));
    for (i = 0U; i < VRAM_BYTES; ++i) assert(video[i] == 0x6aU);
    ++tested;
    /* A missing final direct MMIO GBD write is harmless when BCI accepted
     * the descriptor; phase 2 must still restore exact state without draws. */
    reset(); ignore_final_mmio_gbd = 1U;
    assert(cvsavage_bind_step(FB_PHYS, 3145728U, 1024U, 768U, 4096U,
                              32U, USABLE_BYTES, 2U) == 1U);
    assert(!cvsavage_owned() && cvsavage_status.command_words == 16U);
    assert(final_gbd_mmio_writes == 0U && !draws && !fills && !copies);
    assert(!memcmp(registers, original_registers, sizeof(registers)));
    assert(!memcmp(crtc, original_crtc, sizeof(crtc)));
    for (i = 0U; i < VRAM_BYTES; ++i) assert(video[i] == 0x6aU);
    ++tested;
    /* A full setup FIFO now fails the required pre-command completion poll;
     * no BCI packet is submitted, and ownership remains until it drains. */
    reset(); bci_setup_fifo_busy = 1U;
    assert(cvsavage_bind_step(FB_PHYS, 3145728U, 1024U, 768U, 4096U,
                              32U, USABLE_BYTES, 0x115U) == 2U);
    assert(cvsavage_owned() && cvsavage_status.idle_timeouts == 2U &&
           !cvsavage_status.fifo_timeouts && !cvsavage_status.command_words && !draws && !fills);
    bci_setup_fifo_busy = busy = 0U;
    assert(cvsavage_release() == 0U && !cvsavage_owned());
    assert(!memcmp(registers, original_registers, sizeof(registers)));
    assert(!memcmp(crtc, original_crtc, sizeof(crtc)));
    for (i = 0U; i < VRAM_BYTES; ++i) assert(video[i] == 0x6aU);
    ++tested;
    reset(); bci_setup_busy = 1U;
    assert(cvsavage_bind_step(FB_PHYS, 3145728U, 1024U, 768U, 4096U,
                              32U, USABLE_BYTES, 0x115U) == 2U);
    assert(cvsavage_owned() && cvsavage_status.idle_timeouts >= 1U && !draws && !fills);
    bci_setup_busy = busy = 0U;
    assert(cvsavage_release() == 0U && !cvsavage_owned());
    assert(!memcmp(registers, original_registers, sizeof(registers)));
    assert(!memcmp(crtc, original_crtc, sizeof(crtc)));
    for (i = 0U; i < VRAM_BYTES; ++i) assert(video[i] == 0x6aU);
    ++tested;
    reset(); assert(bind(32U) == 1U); before = cvsavage_status.command_words;
    bci_restore_fifo_busy = 1U; restore_status_reads = 0U;
    assert(cvsavage_release() == 1U && cvsavage_owned());
    assert(cvsavage_status.fifo_timeouts == 1U && cvsavage_status.command_words == before);
    bci_restore_fifo_busy = busy = 0U;
    assert(cvsavage_release() == 0U && !cvsavage_owned());
    assert(!memcmp(registers, original_registers, sizeof(registers)));
    assert(!memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    /* A restore-idle failure must leave BCI enabled and ownership held; a
     * retry after the engine recovers completes the exact register restore. */
    reset(); assert(bind(32U) == 1U);
    bci_restore_busy = 1U; bci_restore_e1_count = 0U;
    assert(cvsavage_release() == 1U && cvsavage_owned());
    assert(cvsavage_status.idle_timeouts == 1U);
    bci_restore_busy = busy = 0U;
    assert(cvsavage_release() == 0U && !cvsavage_owned());
    assert(!memcmp(registers, original_registers, sizeof(registers)));
    assert(!memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    /* CR66_0 is independent of CR40 MMIO decode. Start with it disabled,
     * require a blanked enable for 2D use, then restore the firmware byte. */
    reset(); assert(!(original_crtc[0x66] & 1U));
    assert(bind(32U) == 1U && cvsavage_owned());
    assert((crtc[0x66] & 1U) && cr66_enable_writes == 1U &&
           !engine_disabled_commands && !invalid_2d_read32);
    assert(cvsavage_release() == 0U && !cvsavage_owned());
    assert(!memcmp(crtc, original_crtc, sizeof(crtc)) &&
           cr66_disable_writes == 1U && !engine_disabled_commands);
    assert(!memcmp(registers, original_registers, sizeof(registers))); ++tested;
    /* An already enabled firmware engine is borrowed without changing CR66. */
    reset(); crtc[0x66] = 0xa1U; original_crtc[0x66] = crtc[0x66];
    assert(bind(32U) == 1U && cvsavage_owned());
    assert(cr66_bit_changes == 0U && crtc[0x66] == 0xa1U);
    assert(cvsavage_release() == 0U && !cvsavage_owned());
    assert(crtc[0x66] == original_crtc[0x66] && cr66_bit_changes == 0U);
    assert(!engine_disabled_commands && !invalid_2d_read32); ++tested;
    /* A bounded no-retrace failure occurs before changing CR66 or submitting
     * commands; ownership is released because the original bit stayed off. */
    reset(); retrace_timeout = 1U;
    assert(bind(32U) == 0U && !cvsavage_owned());
    assert(cvsavage_status.error_stage == 12U && !(crtc[0x66] & 1U));
    assert(cr66_enable_writes == 0U && !cvsavage_status.command_words &&
           !engine_disabled_commands && !draws && !fills && !copies);
    assert(!memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    /* If retrace stalls while returning CR66, release keeps ownership and can
     * retry safely after the display begins producing retrace again. */
    reset(); assert(bind(32U) == 1U && cvsavage_owned());
    retrace_timeout = 1U;
    assert(cvsavage_release() == 1U && cvsavage_owned());
    assert(cvsavage_status.error_stage == 13U && (crtc[0x66] & 1U));
    retrace_timeout = 0U;
    assert(cvsavage_release() == 0U && !cvsavage_owned());
    assert(!memcmp(crtc, original_crtc, sizeof(crtc)) &&
           !memcmp(registers, original_registers, sizeof(registers)));
    assert(!engine_disabled_commands && !invalid_2d_read32); ++tested;
    /* A CR66 write that does not latch is a setup failure, not permission to
     * submit BCI work. Rollback recognizes the unchanged original state. */
    reset(); rejected_cr66_enable = 1U;
    assert(bind(32U) == 0U && !cvsavage_owned());
    assert(cvsavage_status.error_stage == 12U && cr66_enable_writes == 1U &&
           !engine_disabled_commands && !cvsavage_status.command_words);
    assert(!memcmp(crtc, original_crtc, sizeof(crtc)) &&
           !memcmp(registers, original_registers, sizeof(registers))); ++tested;
    /* Engine enable succeeds, but the first post-enable idle poll and the
     * CR66 rollback retrace both fail. With no saved GPU state, public ops
     * must report retained ownership (2), never permit CPU fallback (1). */
    reset(); busy_after_cr66_enable = retrace_timeout_after_cr66_enable = 1U;
    assert(bind(32U) == 2U && cvsavage_owned() && !cvsavage_status.ready);
    assert(cvsavage_status.owns_engine && cvsavage_status.command_words == 0U &&
           cvsavage_status.error_stage == 13U && (crtc[0x66] & 1U));
    before = writes; value = reads;
    p = triangle();
    assert(cvsavage_fill(0U, 1U, 0x123456U, 4U) == 2U);
    assert(cvsavage_page_copy(0U, 1024U, 16U, 1U, 2560U) == 2U);
    assert(cvsavage_triangle(&p) == 2U);
    assert(writes == before && reads == value && !draws && !fills && !copies &&
           !cvsavage_status.command_words);
    busy_after_cr66_enable = retrace_timeout_after_cr66_enable = 0U;
    busy = retrace_timeout = 0U;
    assert(cvsavage_release() == 0U && !cvsavage_owned());
    assert(!memcmp(crtc, original_crtc, sizeof(crtc)) &&
           !memcmp(registers, original_registers, sizeof(registers)));
    assert(!engine_disabled_commands && !invalid_2d_read32); ++tested;
    /* A transient failure of the upper GBD half is detected; restoring the
     * saved descriptor then succeeds with the recovered read path. */
    reset(); fail_gbd_half_once = 1U;
    assert(bind(32U) == 0U && !cvsavage_owned());
    assert(gbd_half_failed && cvsavage_status.error_stage == ((3U << 16) | 3U) &&
           cvsavage_status.last_status == 0x00000281U && !draws && !fills && !copies &&
           !invalid_2d_read32);
    assert(!memcmp(crtc, original_crtc, sizeof(crtc)) &&
           !memcmp(registers, original_registers, sizeof(registers)));
    assert(!engine_disabled_commands); ++tested;
    reset(); busy = 1U; assert(bind(32U) == 0U && !cvsavage_owned()); assert(reads < 70000U); ++tested;
    assert(memcmp(crtc, original_crtc, sizeof(crtc)) == 0);
    reset(); setup_fault = 1U; busy_on_setup_fault = 1U;
    assert(cvsavage_bind_step(FB_PHYS, 3145728U, 1024U, 768U, 4096U,
                              32U, USABLE_BYTES, 0x115U) == 2U);
    assert(cvsavage_owned() && cvsavage_status.error_stage == 8U);
    assert(cvsavage_status.last_status == 0x00200020U && cvsavage_status.idle_timeouts == 2U);
    assert(!cvsavage_status.command_words && !unmaps);
    busy_on_setup_fault = busy = 0U;
    assert(cvsavage_release() == 0U);
    assert(!memcmp(registers, original_registers, sizeof(registers)));
    assert(!memcmp(crtc, original_crtc, sizeof(crtc))); ++tested;
    reset(); suppress_fill = 1U; assert(bind(32U) == 0U && !cvsavage_owned());
    assert(memcmp(registers, original_registers, sizeof(registers)) == 0); ++tested;
    reset(); suppress_triangle = 1U; assert(bind(32U) == 1U);
    assert(cvsavage_status.caps == (CVSAVAGE_CAP_FILL | CVSAVAGE_CAP_COPY));
    assert(cvsavage_status.triangle_probe_failures == 1U); ++tested;
    reset(); busy_after_draw = 1U; assert(bind(32U) == 2U && cvsavage_owned());
    assert(cvsavage_status.ready == 0U && cvsavage_status.caps == 0U && unmaps == 0U);
    busy_after_draw = busy = 0U; assert(cvsavage_release() == 0U); ++tested;
    reset(); suppress_copy = 1U; assert(bind(32U) == 1U);
    assert(cvsavage_status.caps == CVSAVAGE_CAP_FILL && cvsavage_status.copy_selftests == 0U);
    before = writes; assert(cvsavage_page_copy(0U, 1024U, 16U, 1U, 2560U) == 1U);
    assert(before == writes && cvsavage_status.triangle_selftests == 0U); ++tested;
    reset(); assert(bind(32U) == 1U); assert(cvsavage_status.caps == 7U);
    assert(cvsavage_status.selftests == 1U && cvsavage_status.triangle_selftests == 1U);
    assert(max_video_offset < USABLE_BYTES - 65536U);
    for (i = 0U; i < 640U * 480U * 4U; ++i) assert(video[i] == 0x6aU);
    ++tested;
    before = writes;
    assert(cvsavage_fill(3U, 1U, 0U, 4U) == 1U);
    assert(cvsavage_fill(2556U, 2U, 0U, 4U) == 1U);
    assert(cvsavage_fill(0U, 0U, 0U, 4U) == 1U);
    assert(cvsavage_fill(0U, 4097U, 0U, 4U) == 1U);
    assert(writes == before); ++tested;
    assert(cvsavage_fill(2560U + 8U, 4U, 0x00123456U, 4U) == 0U);
    memcpy(&value, video + 2568U, 4U); assert(value == 0x00123456U);
    assert(video[2567] == 0x6aU && video[2584] == 0x6aU);
    assert(cvsavage_status.fills == 1U); ++tested;
    p = triangle(); assert(cvsavage_triangle(&p) == 0U);
    memcpy(&value, video + 20U * 2560U + 20U * 4U, 4U); assert(value == 0x00ff0000U);
    memcpy(&value, video + 90U * 2560U + 90U * 4U, 4U); assert(value == 0x006a6a6aU);
    assert(cvsavage_status.triangles == 1U && cvsavage_status.blits == 2U); ++tested;
    before = writes; p.vertex[0].x = 0x7fc00000U;
    assert(cvsavage_triangle(&p) == 1U && writes == before); ++tested;
    p = triangle(); p.vertex[0].z = 0xbf000000U;
    assert(cvsavage_triangle(&p) == 1U && writes == before); ++tested;
    p = triangle(); p.vertex[1].x = 0x441f0000U; p.vertex[2].y = 0x43ef0000U;
    assert(cvsavage_triangle(&p) == 1U && writes == before); ++tested;
    p = triangle(); p.flags = 1U; assert(cvsavage_triangle(&p) == 1U && writes == before); ++tested;
    busy = 1U; before = reads; assert(cvsavage_fill(0U, 1U, 0U, 4U) == 2U);
    assert(reads - before == 65536U && cvsavage_status.caps == 0U && cvsavage_owned());
    assert(cvsavage_release() == 1U && cvsavage_owned() && unmaps == 0U); ++tested;
    busy = 0U; assert(cvsavage_release() == 0U && !cvsavage_owned());
    assert(memcmp(registers, original_registers, sizeof(registers)) == 0);
    assert(memcmp(crtc, original_crtc, sizeof(crtc)) == 0 && unmaps == 2U); ++tested;
    reset(); assert(bind(16U) == 1U && cvsavage_status.caps == 7U);
    assert(cvsavage_fill(1280U + 4U, 4U, 0x5aa5U, 2U) == 0U);
    memcpy(&value, video + 1284U, 4U); assert(value == 0x5aa55aa5U);
    assert(cvsavage_triangle(&(cvsavage_triangle_packet){0}) == 1U);
    assert(cvsavage_sync() == 0U && cvsavage_status.error_stage == 0U); ++tested;
    reset(); assert(bind(32U) == 1U); fifo_stuck_after_fill = fills + 1U;
    before = reads; assert(cvsavage_fill(0U, 1U, 0U, 4U) == 2U);
    assert(cvsavage_status.fifo_timeouts == 1U && reads - before < 65540U);
    assert(cvsavage_owned() && cvsavage_status.caps == 0U);
    fifo_stuck_after_fill = 0U; assert(cvsavage_release() == 0U); ++tested;
    reset(); assert(cvsavage_bind(FB_PHYS, 1228800U, 640U, 480U, 2560U, 32U,
                                 1228800U + 65536U + 4096U) == 1U);
    assert(cvsavage_status.caps == (CVSAVAGE_CAP_FILL | CVSAVAGE_CAP_COPY)); ++tested;
    reset(); assert(cvsavage_bind(FB_PHYS, 2457600U, 640U, 480U, 2560U,
                                  32U, USABLE_BYTES) == 1U);
    assert(cvsavage_status.copy_selftests == 1U);
    for (i = 0U; i < 3U; ++i) memset(video + 10U * 2560U + 4U + i * 2560U, 0x33, 32U);
    assert(cvsavage_page_copy(25604U, 1228800U + 25604U, 32U, 3U, 2560U) == 0U);
    for (i = 0U; i < 3U; ++i) {
        memcpy(&value, video + 1228800U + 25604U + i * 2560U, 4U);
        assert(value == 0x00333333U);
    }
    assert(cvsavage_status.blits == 1U);
    assert(video[1228800U + 25603U] == 0x6aU);
    assert(video[1228800U + 25636U] == 0x6aU); ++tested;
    before = writes;
    assert(cvsavage_page_copy(25604U, 25608U, 32U, 3U, 2560U) == 1U);
    assert(cvsavage_page_copy(25604U, 1228800U + 25608U, 32U, 3U, 2560U) == 1U);
    assert(cvsavage_page_copy(25604U, 1228800U + 25604U, 32U, 3U, 2564U) == 1U);
    assert(cvsavage_page_copy(25604U, 1228800U + 25604U, 260U, 64U, 2560U) == 1U);
    assert(cvsavage_page_copy(25604U, 2457596U, 32U, 3U, 2560U) == 1U);
    assert(cvsavage_page_copy(479U * 2560U, 1228800U, 16U, 2U, 2560U) == 1U);
    assert(writes == before); ++tested;
    busy = 1U; assert(cvsavage_page_copy(25604U, 1228800U + 25604U, 32U, 3U, 2560U) == 2U);
    assert(writes == before && cvsavage_owned()); busy = 0U; ++tested;
    reset(); before = pci_address;
    assert(cvsavage_panel_probe() == 1U);
    assert(cvsavage_status.panel_width == 1024U && cvsavage_status.panel_height == 768U &&
           cvsavage_status.panel_flags == 1U && cvsavage_status.ready == 0U);
    assert(cr_data_writes == 0U && seq_data_writes == 0U && writes == 0U &&
           cr_index == 0x3fU && seq_index == 0x55U && pci_address == before); ++tested;
    sequencer[0x61] = 174U; sequencer[0x69] = 25U; sequencer[0x6e] = 0x40U;
    assert(cvsavage_panel_probe() == 1U);
    assert(cvsavage_status.panel_width == 1400U && cvsavage_status.panel_height == 1050U);
    assert(cr_data_writes == 0U && seq_data_writes == 0U); ++tested;
    crtc[0x6b] = 1U; assert(cvsavage_panel_probe() == 0U);
    assert(cvsavage_status.panel_flags == 0U && cvsavage_status.panel_width == 0U); ++tested;
    reset(); seq_locked = 1U; assert(cvsavage_panel_probe() == 0U); ++tested;
    reset(); seq_unstable = 1U; assert(cvsavage_panel_probe() == 0U);
    assert(seq_index == 0x55U && cr_index == 0x3fU && cr_data_writes == 0U); ++tested;
    reset(); other_pci = 1U; assert(cvsavage_panel_probe() == 0U);
    assert(cr_data_writes == 0U && seq_data_writes == 0U && writes == 0U); ++tested;
    reset(); memset(sequencer, 0, sizeof(sequencer)); assert(cvsavage_panel_probe() == 0U); ++tested;
    /* Empty PCI topology is enumerated once. Status polling thereafter is
     * constant work without PCI/VGA/MMIO I/O, even after release/detect. */
    reset(); present = 0U; pci_address = 0x80001234U;
    assert(cvsavage_panel_probe() == 0U);
    assert(pci_reads == 8193U && pci_writes == 8193U);
    assert(pci_address == 0x80001234U);
    before = pci_reads; value = pci_writes;
    assert(cvsavage_release() == 0U);
    for (i = 0U; i < 100U; ++i) {
        cvsavage_status.panel_flags = 1U;
        cvsavage_status.panel_width = 1024U; cvsavage_status.panel_height = 768U;
        assert(cvsavage_panel_probe() == 0U);
        assert(cvsavage_status.panel_flags == 0U &&
               cvsavage_status.panel_width == 0U && cvsavage_status.panel_height == 0U);
    }
    assert(pci_reads == before && pci_writes == value && pci_address == 0x80001234U);
    assert(vga_reads == 0U && vga_writes == 0U && map_attempts == 0U &&
           reads == 0U && writes == 0U && cvsavage_status.ready == 0U); ++tested;
    /* Unsupported adapters also retain their negative result. */
    reset(); other_pci = 1U; assert(cvsavage_panel_probe() == 0U);
    before = pci_reads; value = pci_writes;
    assert(before > 8192U);
    for (i = 0U; i < 10U; ++i) assert(cvsavage_panel_probe() == 0U);
    assert(pci_reads == before && pci_writes == value &&
           vga_reads == 0U && vga_writes == 0U && map_attempts == 0U); ++tested;
    /* A known device is checked on every query, with fresh LCD dimensions. */
    reset(); assert(cvsavage_panel_probe() == 1U);
    before = pci_reads; value = pci_writes;
    sequencer[0x61] = 174U; sequencer[0x69] = 25U; sequencer[0x6e] = 0x40U;
    assert(cvsavage_panel_probe() == 1U);
    assert(pci_reads - before == 5U && pci_writes - value == 5U);
    assert(cvsavage_status.panel_width == 1400U && cvsavage_status.panel_height == 1050U);
    assert(cr_data_writes == 0U && seq_data_writes == 0U && map_attempts == 0U); ++tested;
    /* Lost positive identity causes one new scan, then becomes negative. */
    present = 0U; before = pci_reads;
    assert(cvsavage_panel_probe() == 0U && pci_reads - before == 8194U);
    before = pci_reads; value = pci_writes;
    assert(cvsavage_status.panel_flags == 0U && cvsavage_status.panel_width == 0U);
    for (i = 0U; i < 10U; ++i) assert(cvsavage_panel_probe() == 0U);
    assert(pci_reads == before && pci_writes == value); ++tested;
    /* Register unavailability is not device absence: retry after mode setup. */
    reset(); seq_locked = 1U; assert(cvsavage_panel_probe() == 0U);
    seq_locked = 0U; before = pci_reads;
    assert(cvsavage_panel_probe() == 1U && pci_reads - before == 5U);
    assert(cvsavage_status.panel_width == 1024U && cvsavage_status.panel_height == 768U); ++tested;
    assert(cvsavage_release() == 0U);
    printf("SuperSavage native BCI protocol: %u scenarios PASS\n", tested);
    return 0;
}
