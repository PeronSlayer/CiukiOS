/*
 * Host unit test for the HDPMI direct VGA mapping state machine.
 *
 * Build manually with the host C compiler and function-section garbage
 * collection, e.g.:
 *   cc -std=c99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
 *      src/vm/test_hdpmi_video_adapter.c src/vm/virtual_vga.c \
 *      -Wl,--gc-sections -o /tmp/test-hdpmi
 * This includes the adapter implementation so private mapping transitions
 * are exercised without HDPMI, DOS, firmware, or a framebuffer device.
 */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "session_video.h"
#include "hdpmi_video_adapter.c"

static uint32_t aperture_ptes[16], shadow_ptes[16], video_pages[1 + 4 * 16];
static unsigned flushes;
static uint32_t shared_storage[(CVVID_BLOCK_BYTES + 3) / 4];

uint32_t *cvdpmi_aperture_ptes(void) { return aperture_ptes; }
uint32_t *cvdpmi_shadow_ptes(void) { return shadow_ptes; }
uint32_t *cvdpmi_video_pages(void) { return video_pages; }
void cvdpmi_flush_tlb(void) { ++flushes; }
uint32_t cvdpmi_selector_base(uint32_t selector) { (void)selector; return 0; }
uint32_t cvdpmi_selector_dbit(uint32_t selector) { (void)selector; return 0; }

static cvvid_shared *shared(void)
{
    return (cvvid_shared *)shared_storage;
}

static cvga_state *vga(void)
{
    return (cvga_state *)((uint8_t *)shared_storage + CVVID_MODEL_OFFSET);
}

static void write_seq(uint8_t index, uint8_t value)
{
    cvdpmi_video_port_write(shared_storage, 0x3c4, index);
    cvdpmi_video_port_write(shared_storage, 0x3c5, value);
}

static void test_mapping_and_harvest(void)
{
    unsigned i;
    memset(shared_storage, 0, sizeof(shared_storage));
    memset(aperture_ptes, 0, sizeof(aperture_ptes));
    memset(shadow_ptes, 0, sizeof(shadow_ptes));
    memset(video_pages, 0, sizeof(video_pages));
    flushes = 0;
    shared()->magic = CVVID_SHARED_MAGIC;
    shared()->version = 0x0100;
    shared()->bytes = CVVID_BLOCK_BYTES;
    vga()->seq[4] = 6; vga()->misc = 3; vga()->enable = 1;
    vga()->gc[6] = 4; vga()->gc[8] = 0xff;
    for (i = 0; i < 16; ++i) {
        shadow_ptes[i] = 0x00100007u + i * 4096u;
        for (unsigned plane = 0; plane < 4; ++plane)
            video_pages[1 + plane * 16 + i] = 0x00200000u + (plane * 16 + i) * 4096u;
    }

    write_seq(2, 1);                    /* map plane 0 writable */
    assert((shared()->pm_direct & 7) == 1);
    assert(flushes == 1);
    assert(aperture_ptes[0] == (video_pages[1] | 7));

    aperture_ptes[0] |= 0x40;           /* emulate a guest store */
    write_seq(2, 1);                    /* same mapping must preserve D */
    assert(flushes == 1);
    assert(aperture_ptes[0] & 0x40);
    assert(vga()->dirty[0][0] == 0);

    write_seq(2, 2);                    /* switch: harvest plane 0 then remap */
    assert((shared()->pm_direct & 7) == 2);
    assert(flushes == 2);
    assert(!(aperture_ptes[0] & 0x40));
    assert(vga()->dirty[0][0] == 0xff);
    assert(aperture_ptes[0] == (video_pages[17] | 7));

    cvdpmi_video_tick(shared_storage);   /* no dirty pages: no invalidation */
    assert(flushes == 2);
    aperture_ptes[3] |= 0x40;
    cvdpmi_video_tick(shared_storage);
    assert(flushes == 3);
    assert(vga()->dirty[1][3 * 64] == 0xff);
    assert(!(aperture_ptes[3] & 0x40));
    cvdpmi_video_tick(shared_storage);
    assert(flushes == 3);
}

static void test_frame_start_edges(void)
{
    vga()->crtc_index = 0x0d;
    cvdpmi_video_port_write(shared_storage, 0x3d5, vga()->crtc[0x0d]);
    assert(!(shared()->pm_direct & 0x20));
    assert(flushes == 3);

    aperture_ptes[4] |= 0x40;
    cvdpmi_video_port_write(shared_storage, 0x3d5, (uint8_t)(vga()->crtc[0x0d] + 1));
    assert(shared()->pm_direct & 0x20);
    assert(flushes == 4);
    assert(vga()->dirty[1][4 * 64] == 0xff);
    assert(!(aperture_ptes[4] & 0x40));
}

int main(void)
{
    test_mapping_and_harvest();
    test_frame_start_edges();
    return 0;
}
