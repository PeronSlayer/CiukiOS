/* Compile the unchanged production glue with only its OS calls redirected.
 * SPDX-License-Identifier: MIT */
#include "replay_base.h"
#include "resize_transport.h"
#include "../../../apps/libdeskui/deskui.h"
#include <string.h>
unsigned rt_renders, rt_compared_rows;
int rt_render_dirty;
struct du_rect rt_render_bounds;
static void measured_render(struct du_ui *u)
{
    rt_renders++; rt_render_dirty=u->dirty;
    rt_render_bounds=u->dirty==3 ? u->invalid : u->dirty==2 ? u->dialog.box :
        (struct du_rect){0,0,u->canvas.width,u->canvas.height};
    du_render(u);
}
static int measured_compare(const void *a,const void *b,size_t bytes)
{ rt_compared_rows++; return memcmp(a,b,bytes); }
#define close rt_close
#define munmap rt_unmap
#define du_render measured_render
#define memcmp measured_compare
#include "../../../apps/libdeskui/client.c"
