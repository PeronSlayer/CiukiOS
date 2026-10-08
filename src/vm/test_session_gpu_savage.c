/* Compile the production backend against a bounded model of documented BCI.
 * This verifies protocol, bounds and ownership; it is not S3 hardware testing. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "session_gpu_savage.h"

#define MMIO_PHYS 0x10000000U
#define FB_PHYS 0x20000000U
#define AP_PHYS 0x30000000U
#define VRAM_BYTES (16U * 1024U * 1024U)
#define USABLE_BYTES (237U * 65536U)
static uint32_t registers[0x80000U / 4U], original_registers[0x80000U / 4U];
static uint8_t video[VRAM_BYTES], crtc[256], original_crtc[256];
static uint8_t cr_index, seq_index, sequencer[256];
static uint32_t pci_address, probe_physical, command[32], command_count;
static uint32_t present, bad_bar, memory_disabled, map_failure, busy;
static uint32_t suppress_fill, suppress_triangle, writes, reads, unmaps;
static uint32_t draws, copies, fills, max_video_offset, tested;
static uint32_t fifo_stuck_after_fill, busy_after_draw;
static uint32_t suppress_copy, other_pci, seq_locked, seq_unstable, seq61_reads;
static uint32_t cr_data_writes, seq_data_writes;
static uint32_t pci_class, bar_flags[3], physical_profile, mmio_override;

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
    if (bytes == 4U) value &= 0x00ffffffU;
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

static void execute(void)
{
    uint32_t head, x, y, width, height, i;
    if (!command_count) return;
    head = command[0];
    if ((head & 0xff000000U) == 0x96000000U) {
        uint32_t count = (head >> 16) & 0xffU, index = head & 0xffffU;
        assert(command_count == count + 1U);
        for (i = 0U; i < count; ++i)
            registers[(0x4850cU + (index + i) * 4U) / 4U] = command[1U + i];
    } else if (head == 0x4bf08c00U) {
        assert(command_count == 6U);
        x = command[4] & 0xffffU; y = command[4] >> 16;
        width = command[5] & 0xffffU; height = command[5] >> 16;
        registers[0x8170U / 4U] = command[1];
        registers[0x8174U / 4U] = command[2];
        if (!suppress_fill)
            for (i = 0U; i < width * height; ++i)
                pixel_set(command[1], command[2], x + i % width,
                            y + i / width, command[3]);
        ++fills;
    } else if (head == 0x4bcc0d40U) {
        uint32_t dx = command[6] & 0xffffU, dy = command[6] >> 16;
        assert(command_count == 8U);
        x = command[5] & 0xffffU; y = command[5] >> 16;
        width = command[7] & 0xffffU; height = command[7] >> 16;
        registers[0x8170U / 4U] = command[1];
        registers[0x8174U / 4U] = command[2];
        registers[0x8178U / 4U] = command[3];
        registers[0x817cU / 4U] = command[4];
        if (!suppress_copy)
            for (i = 0U; i < width * height; ++i)
                pixel_set(command[1], command[2], dx + i % width, dy + i / width,
                            pixel_get(command[3], command[4], x + i % width, y + i / width));
        ++copies;
    } else if (head == 0x800300faU) {
        uint32_t dest = registers[0x485dcU / 4U], descriptor, offset;
        uint32_t start = registers[0x485e0U / 4U], end = registers[0x485e4U / 4U];
        float vx[3], vy[3], area;
        assert(command_count == 13U);
        assert(registers[0x48584U / 4U] == 0x44000010U);
        assert(registers[0x485a8U / 4U] == 0U); /* texture disabled */
        assert(registers[0x485d4U / 4U] == 0U); /* Z disabled */
        offset = ((dest >> 8) & 0x3fffU) * 2048U;
        descriptor = 0x11000000U | ((dest & 0x80000000U) ? 0x00200000U : 0x00100000U);
        descriptor |= (dest & 0x7fU) * ((dest & 0x80000000U) ? 32U : 64U);
        for (i = 0U; i < 3U; ++i) {
            vx[i] = word_float(command[1U + i * 4U]);
            vy[i] = word_float(command[2U + i * 4U]);
        }
        area = edge(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2]);
        if (!suppress_triangle && area != 0.0f) {
            for (y = (start >> 12) & 0xfffU; y <= ((end >> 12) & 0xfffU); ++y)
                for (x = start & 0x7ffU; x <= (end & 0x7ffU); ++x) {
                    float a = edge(vx[1], vy[1], vx[2], vy[2], (float)x + .5f, (float)y + .5f) / area;
                    float b = edge(vx[2], vy[2], vx[0], vy[0], (float)x + .5f, (float)y + .5f) / area;
                    float c = 1.0f - a - b;
                    if (a >= 0.0f && b >= 0.0f && c >= 0.0f) {
                        uint32_t rgb = command[4] & 0xffffffU;
                        if (((descriptor >> 16) & 255U) == 16U)
                            rgb = ((rgb >> 8) & 0xf800U) | ((rgb >> 5) & 0x7e0U) | ((rgb >> 3) & 31U);
                        pixel_set(offset, descriptor, x, y, rgb);
                    }
                }
        }
        ++draws;
        if (busy_after_draw) busy = 1U;
    } else assert(head == 0xc0020000U || head == 0xc0030000U);
    command_count = 0U;
}

uint32_t cvdev_in(uint32_t port, uint32_t size)
{
    (void)size;
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
    case 0x18U: return (physical_profile ? 0xe4000000U : AP_PHYS) | bar_flags[2];
    default: return 0U;
    }
}

void cvdev_out(uint32_t port, uint32_t value, uint32_t size)
{
    (void)size;
    if (port == 0xcf8U) pci_address = value;
    else if (port == 0x3d4U) cr_index = (uint8_t)value;
    else if (port == 0x3c4U) seq_index = (uint8_t)value;
    else if (port == 0x3c5U) { sequencer[seq_index] = (uint8_t)value; ++seq_data_writes; }
    else { assert(port == 0x3d5U); crtc[cr_index] = (uint8_t)value; ++cr_data_writes; }
}

uint32_t cvgpu_map_mmio(uint32_t physical, uint32_t bytes)
{
    if (map_failure) return 0U;
    if (physical == mmio_base()) { assert(bytes == 0x80000U); return 1U; }
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
        memcpy(&value, video + probe_physical + offset, 4U); return value;
    }
    assert(linear == 1U && offset < 0x80000U);
    if (offset == 0x48c60U) {
        assert(crtc[0x40] & 1U); /* Firmware MMIO-off is enabled before reads. */
        execute();
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
    if (offset >= 0x10000U && offset < 0x10080U) {
        if (offset == 0x10000U) execute();
        assert((offset - 0x10000U) / 4U == command_count);
        command[command_count++] = value;
    } else registers[offset / 4U] = value;
}

static void reset(void)
{
    assert(cvsavage_release() == 0U);
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
    pci_class = 0x03000000U; memset(bar_flags, 0, sizeof(bar_flags));
    physical_profile = mmio_override = 0U;
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

int main(void)
{
    uint32_t before, value, i;
    cvsavage_triangle_packet p;
    assert(sizeof(cvsavage_status_info) == 128U && sizeof(p) == 64U);
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
    for (i = 0U; i < 3U; ++i) {
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
    reset(); busy = 1U; assert(bind(32U) == 0U && !cvsavage_owned()); assert(reads < 70000U); ++tested;
    assert(memcmp(crtc, original_crtc, sizeof(crtc)) == 0);
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
    assert(cvsavage_release() == 0U);
    printf("SuperSavage native BCI protocol: %u scenarios PASS\n", tested);
    return 0;
}
