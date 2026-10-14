/* SPDX-License-Identifier: GPL-2.0-only
 * Production kernel XRGB upload loop, using the existing framebuffer fixture.
 * The destination is ordinary host RAM, never a claim about PCI/LFB timing. */
#define main framebuffer_reference_main
#include "../fbdev_test.c"
#undef main
void gfx_host_lfb_row(uint32_t *dst,const uint32_t *src,unsigned width)
{
    present_xrgb_row((uint8_t *)dst,(const uint8_t *)src,width);
    row_written();
}
