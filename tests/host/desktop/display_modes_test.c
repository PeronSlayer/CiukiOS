/* f1-23: production compositor relayout and cfg round trip. */
#include "fake.h"
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static struct desktop d;
static int mode_set(struct desktop *desktop,uint16_t mode)
{ assert(desktop==&d); desktop->display_mode=desktop->display_pending=mode; return 0; }
int main(void)
{
    fake_reset(); desk_init(&d, &fake_ops, 1024, 768); desk_shell_enable(&d);
    struct desk_client *c = &d.clients[0]; fake_channel(8); c->channel = 8;
    c->window = c->hello = c->resizable = 1; c->version = DESK_SHELL_VERSION; c->minor = 1;
    c->min_width = c->min_height = 100; c->max_width = 1600; c->max_height = 1200;
    c->step_width = c->step_height = 1; c->frame = (struct desk_box){400,300,800,600};
    c->restore = c->frame; d.mouse_x = 1000; d.mouse_y = 750;
    strcpy(d.sh.cfg.wallpaper,"none");
    d.pixels = calloc(640*480,4); assert(d.pixels);
    desk_display_relayout(&d, 640, 480);
    assert(d.width == 640 && d.height == 480 && c->frame.x >= 0 && c->frame.y >= DESK_BAR);
    assert(c->frame.x + c->frame.w <= 640 && c->frame.y + c->frame.h <= 480 - DESK_BAR);
    assert(d.mouse_x == 639 && d.mouse_y == 479 && !d.sh.wall_valid);
    assert(d.damage.count == 1 && d.damage.boxes[0].w == 640 && d.damage.boxes[0].h == 480);
    desk_flush(&d,0);
    assert(fake_sent >= 1 && fake_messages[fake_sent-1].data[1] == DESK_CONFIGURE);
    unsigned old_sent = fake_sent;
    assert(!desk_shell_wallpaper_update(&d) && d.sh.wall_w == 640 && d.sh.wall_h == 480);
    assert(d.sh.icons[0].box.y >= DESK_BAR);
    free(d.pixels); d.pixels = calloc(1024*768,4); assert(d.pixels);
    struct desk_box restore = c->frame;
    /* Preserve a nonzero old origin: relayout must fit the new size at the
     * maximized origin before applying the available-space limits. */
    c->zoomed = 1; c->restore = restore;
    assert(c->zoomed && !memcmp(&c->restore,&restore,sizeof(restore)));
    assert(!desk_resize_hit(c,c->frame.x,c->frame.y));
    desk_geometry_begin(&d,0,1);
    assert(!d.keyboard_geometry && d.drag == -1);
    desk_display_relayout(&d, 1024, 768);
    assert(c->frame.x == 0 && c->frame.y == DESK_BAR && c->frame.w == 1024 && c->frame.h == 768-2*DESK_BAR);
    desk_flush(&d,0);
    assert(fake_sent > old_sent && !d.sh.wall_valid);
    assert(!desk_shell_wallpaper_update(&d) && d.sh.wall_w == 1024 && d.sh.wall_h == 768);
    for (int i = 0; i < d.sh.icon_count; i++) assert(d.sh.icons[i].box.y + d.sh.icons[i].box.h <= 768 - DESK_BAR);
    /* A depth-only change still configures the client; a smaller mode refits
     * the maximized frame without replacing its saved normal geometry. */
    uint32_t serial = c->resize_serial;
    old_sent = fake_sent;
    desk_display_relayout(&d,1024,768);
    assert(c->zoomed && c->resize_serial != serial);
    assert(c->frame.x == 0 && c->frame.y == DESK_BAR && c->frame.w == 1024 && c->frame.h == 768-2*DESK_BAR);
    desk_flush(&d,0);
    assert(fake_sent > old_sent && fake_messages[fake_sent-1].data[1] == DESK_CONFIGURE);
    assert(desk_get32(fake_messages[fake_sent-1].data+12) == c->resize_serial);
    free(d.pixels); d.pixels = calloc(640*480,4); assert(d.pixels);
    desk_display_relayout(&d,640,480);
    assert(c->zoomed && c->frame.x == 0 && c->frame.y == DESK_BAR && c->frame.w == 640 && c->frame.h == 480-2*DESK_BAR);
    assert(!memcmp(&c->restore,&restore,sizeof(restore)));
    desk_flush(&d,0);
    desk_zoom(&d,0);
    assert(!c->zoomed && !memcmp(&c->frame,&restore,sizeof(restore)));
    desk_flush(&d,0);
    puts("PASS display maximized: grow/shrink refit, depth-only CONFIGURE, saved restore geometry, mouse/keyboard resize blocked");
    puts("PASS display compositor: 1024x768 -> 640x480 -> 1024x768, clamped windows/pointer, CONFIGURE, bars/icons/wallpaper");
    d.sh.cfg.display_mode = 0x118; d.display_mode = 0x118; d.display_pending = 0x111;
    d.damage.count = 0;
    assert(!desk_display_frame_commit(&d,0) && d.sh.cfg.display_mode == 0x118); /* allocation failed: old mode */
    d.display_mode = 0x111; d.damage.count = 1;
    assert(!desk_display_frame_commit(&d,0) && d.sh.cfg.display_mode == 0x118);
    d.damage.count = 0;
    assert(!desk_display_frame_commit(&d,-1) && d.sh.cfg.display_mode == 0x118);
    assert(desk_display_frame_commit(&d,0) && !d.display_pending && d.sh.cfg.display_mode == 0x111);
    assert(!desk_display_frame_commit(&d,0));
    d.display_reply_mode=0x118; d.display_reply_preview=1; d.display_mode=d.display_pending=0x118;
    assert(desk_display_frame_commit(&d,0)==DESK_DISPLAY_PREVIEW_FRAME && d.sh.cfg.display_mode==0x111);
    assert(!d.display_pending); d.display_reply_mode=0; d.display_reply_preview=0;
    d.display_mode=d.display_pending=0x118;
    assert(desk_display_frame_commit(&d,0)==DESK_DISPLAY_PERSIST_FRAME && d.sh.cfg.display_mode==0x118);
    puts("PASS display persistence: failed allocation/partial/failed frame retains saved id; successful confirmed frame commits once");
    struct ciuki_settings s, after; cfg_defaults(&s); s.display_mode = 0x118;
    char text[CFG_TEXT_MAX]; size_t n = cfg_format(&s,text,sizeof(text));
    assert(n && strstr(text,"display_mode=280\n")); cfg_defaults(&after);
    assert(!cfg_parse(&after,text,n) && after.display_mode == 0x118);
    cfg_defaults(&after); assert(!after.display_mode);
    assert(cfg_parse(&after,"display_mode=65536\n",19) == 1 && !after.display_mode);
    assert(cfg_parse(&after,"display_mode=-1\n",16) == 1 && !after.display_mode);
    const char valid[] = "display_mode=65535\n"; assert(!cfg_parse(&after,valid,sizeof(valid)-1) && after.display_mode == 65535);
    puts("PASS desktop.cfg: display_mode decimal round trip, loader default, malformed/range rejection");
    struct ciuki_message m; struct desk_packet p;
    desk_message(&m,DESK_SETTINGS,24); m.data[0] = DESK_SHELL_VERSION;
    desk_put32(m.data+8,DESK_SETTINGS_DISPLAY); desk_put32(m.data+12,0x111);
    assert(desk_parse(&m,1,NULL,&p) && p.values[0] == 0x111);
    desk_put32(m.data+16,1); assert(!desk_parse(&m,1,NULL,&p));
    desk_put32(m.data+16,0); desk_put32(m.data+12,65536); assert(!desk_parse(&m,1,NULL,&p));
    desk_put32(m.data+8,DESK_SETTINGS_DISPLAY_PREVIEW);desk_put32(m.data+12,0x111);
    assert(desk_parse(&m,1,NULL,&p) && p.request==DESK_SETTINGS_DISPLAY_PREVIEW);
    desk_put32(m.data+16,1);assert(!desk_parse(&m,1,NULL,&p));
    puts("PASS display IPC: Control Panel settings relay, bounded uint16 id and reserved values");
    /* Native request admission, completed frame classification and reply. */
    c->program=DESK_APP_CONTROL; c->pid=7; c->tx_count=0; d.display_set=mode_set;
    desk_message(&m,DESK_SETTINGS,24);m.data[0]=DESK_SHELL_VERSION;m.data[2]=(uint8_t)c->minor;m.sender_pid=7;
    desk_put32(m.data+4,42);desk_put32(m.data+8,DESK_SETTINGS_DISPLAY_PREVIEW);desk_put32(m.data+12,0x111);
    desk_receive(&d,0,&m);assert(d.display_reply_preview && d.display_reply_mode==0x111 && !c->tx_count);
    assert(desk_display_frame_commit(&d,0)==DESK_DISPLAY_PREVIEW_FRAME && d.sh.cfg.display_mode==0x118);
    desk_display_reply(&d,0);assert(!d.display_reply_preview && !d.display_reply_mode && c->tx_count==1);
    assert(desk_parse(&c->tx[c->tx_head],0,NULL,&p) && p.serial==42 && !p.result);c->tx_count=0;
    desk_put32(m.data+4,43);desk_put32(m.data+8,DESK_SETTINGS_DISPLAY);desk_put32(m.data+12,0x118);
    desk_receive(&d,0,&m);assert(!d.display_reply_preview && d.display_reply_mode==0x118);
    assert(desk_display_frame_commit(&d,0)==DESK_DISPLAY_PERSIST_FRAME);desk_display_reply(&d,0);c->tx_count=0;
    c->program=0;desk_put32(m.data+8,DESK_SETTINGS_DISPLAY_PREVIEW);
    desk_receive(&d,0,&m);assert(c->tx_count==1 && !d.display_pending);
    assert(desk_parse(&c->tx[c->tx_head],0,NULL,&p) && p.result==-EPERM);
    puts("PASS display preview ownership: admitted Control Panel request, deferred reply, no automatic preference save, legacy durable frame, ordinary-client refusal");
    desk_drop_client(&d,0); free(d.pixels); free(d.sh.wall);
    assert(!fake_live()); return 0;
}
