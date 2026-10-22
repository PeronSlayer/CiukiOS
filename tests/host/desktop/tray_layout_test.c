/* Production tray geometry, Tango pixels and redraw transitions. SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct desktop d;
static uint32_t portrait[256*256];
static char clock_label[24];
static int clock_x, clock_y;
void __real_du_text(struct du_canvas *, int, int, const char *, uint32_t);
void __wrap_du_text(struct du_canvas *c, int x, int y, const char *s, uint32_t colour)
{
    if (!strcmp(s, clock_label)) { clock_x = x; clock_y = y; }
    __real_du_text(c, x, y, s, colour);
}
static const uint32_t *store(const char *name, unsigned size)
{ return du_icon_argb(name, size); }
static void geometry(const char *text, int media)
{
    snprintf(clock_label, sizeof(clock_label), "%s", text);
    int cw = desk_shell_clock_width_at(&d,d.realtime_second), count = media ? 4 : 3;
    struct desk_box t = desk_shell_tray_box(&d);
    assert(t.w == 12 + cw + count * 20 && t.x + t.w == (int)d.width - 4);
    assert(t.y == 4 && t.h == DESK_BAR-7);
    int right = t.x + t.w - 6 - cw;
    const enum sh_tray_icon order[]={SH_TRAY_NOTIFICATION,SH_TRAY_VOLUME,SH_TRAY_NETWORK,SH_TRAY_MEDIA};
    for (int i = 0; i < count; i++) {
        struct desk_box b = desk_shell_tray_icon_box(&d, order[i]);
        assert(b.x + b.w + 4 == right && b.w == 16 && b.h == 16 && b.y == t.y + 4);
        right = b.x;
    }
    assert(right == t.x + 6);
    if (!media) assert(!desk_shell_tray_icon_box(&d, SH_TRAY_MEDIA).w);
    desk_composite(&d, (struct desk_box){0, 0, d.width, d.height});
    assert(clock_x == t.x + t.w - 6 - cw + (cw-du_text_width(text))/2 && clock_y == t.y);
    for (int i = 0; i < DESK_CLIENTS; i++) {
        struct desk_box b = desk_task_button(&d, i);
        assert(b.w > 0 && b.x + b.w <= (int)d.width-DESK_TASK_X && b.y >= (int)d.height-DESK_BAR);
    }
}
static void icon(enum sh_tray_icon which, const char *name, int disabled)
{
    uint32_t pixels[256], background[1024*DESK_BAR];
    struct desk_box b = desk_shell_tray_icon_box(&d, which);
    assert(d.width<=1024);
    struct desk_paint p={background,(int)d.width,DESK_BAR,0,0,(int)d.width,DESK_BAR};
    desk_paint_bar(&p,0,0,d.width,DESK_BAR,desk_theme->topbar,d.lfb_bytes_per_pixel==2);
    for (int y=0; y<16; y++)
        memcpy(pixels+y*16,background+(b.y+y)*d.width+b.x,16*4);
    struct du_canvas c = {pixels, 16, 16, {0, 0, 16, 16}};
    const uint32_t *source = du_icon_argb(name, 16); assert(source);
    assert(desk_shell_icon(name, 16) == source);
    du_icon_blit(&c, 0, 0, source, 16, disabled ? DU_ICON_DISABLED : DU_ICON_NORMAL);
    for (int y = 0; y < 16; y++)
        assert(!memcmp(d.pixels + (b.y + y) * d.width + b.x, pixels + y * 16, 16 * 4));
}
static void redraw(void)
{
    for (unsigned i = 0; i < d.damage.count; i++) desk_composite(&d, d.damage.boxes[i]);
    size_t bytes = (size_t)d.width * d.height * 4;
    uint32_t *incremental = malloc(bytes); assert(incremental);
    memcpy(incremental, d.pixels, bytes);
    desk_composite(&d, (struct desk_box){0, 0, d.width, d.height});
    if (memcmp(incremental,d.pixels,bytes)) for (unsigned i=0;i<d.width*d.height;i++) {
        if (incremental[i]==d.pixels[i]) continue;
        fprintf(stderr,"tray mismatch %s %ux%u at %u,%u actual=%06x expected=%06x time=%llu seconds=%d popup=%d/%d/%d tooltip=%d notice=%d slide=%d hot=%d menu=%d\n",
            desk_theme->name,d.width,d.height,i%d.width,i/d.width,incremental[i],d.pixels[i],
            (unsigned long long)d.realtime_second,d.sh.cfg.clock_seconds,d.sh.volume_open,d.sh.network_open,
            d.sh.media_open,d.sh.tooltip,d.sh.notification.active,d.sh.notification.slide,d.sh.start_hot,d.menu_open);
        for (unsigned k=0;k<d.damage.count;k++) fprintf(stderr,"damage %d,%d %dx%d\n",
            d.damage.boxes[k].x,d.damage.boxes[k].y,d.damage.boxes[k].w,d.damage.boxes[k].h);
        break;
    }
    assert(!memcmp(incremental, d.pixels, bytes));
    free(incremental); d.damage.count = 0;
}
static void click(enum sh_tray_icon which)
{
    struct desk_box b = desk_shell_tray_icon_box(&d, which);
    d.mouse_x = b.x + 8; d.mouse_y = b.y + 8;
    assert(desk_shell_button(&d, CIUKI_BUTTON_LEFT, 1));
    desk_shell_button(&d, CIUKI_BUTTON_LEFT, 0);
}
static void transitions(void)
{
    /* An EC observation updates the existing volume UI, including an open
     * slider and the mute icon, with exact incremental/full redraw parity. */
    d.sh.mixer_state=2;d.sh.volume_open=1;
    desk_composite(&d,(struct desk_box){0,0,d.width,d.height});d.damage.count=0;
    d.sh.volume=21;d.sh.muted=1;desk_shell_volume_damage(&d);assert(d.damage.count);redraw();
    d.sh.volume=57;d.sh.muted=0;desk_shell_volume_damage(&d);assert(d.damage.count);redraw();
    d.sh.volume_open=0;desk_composite(&d,(struct desk_box){0,0,d.width,d.height});d.damage.count=0;
    struct ciuki_media_state s = {.version = 1, .size = sizeof(s), .removable_mask = 1u << 4};
    desk_shell_media_update(&d, &s); redraw();
    s.removable_mask = 0; desk_shell_media_update(&d, &s); redraw();
    s.removable_mask = 1u << 4; desk_shell_media_update(&d, &s); redraw();
    d.sh.cfg.clock_24h = 0;
    for (int popup = 0; popup < 4; popup++) {
        d.realtime_second = 9 * 3600 + 59 * 60;
        d.sh.volume_open = d.sh.network_open = d.sh.media_open = d.sh.tooltip = 0;
        if (popup < 3) click(popup == 2 ? SH_TRAY_MEDIA : (enum sh_tray_icon)popup);
        else d.sh.tooltip = 2;
        desk_composite(&d, (struct desk_box){0, 0, d.width, d.height});
        d.damage.count = 0;
        uint64_t previous = d.realtime_second;
        int old_width = desk_shell_tray_box(&d).w;
        d.realtime_second = 10 * 3600;
        assert(desk_shell_tray_box(&d).w >= old_width);
        desk_shell_clock_damage(&d, previous);
        for(unsigned k=0;k<d.damage.count;k++)
            assert(d.damage.boxes[k].w<(int)d.width); /* tray rollover never the whole bar */
        redraw();
        old_width = desk_shell_tray_box(&d).w;
        d.sh.cfg.clock_seconds = 1;
        desk_shell_tray_damage(&d, old_width); redraw();
        old_width = desk_shell_tray_box(&d).w;
        d.sh.cfg.clock_seconds = 0;
        desk_shell_tray_damage(&d, old_width); redraw();
    }
    d.sh.volume_open = d.sh.network_open = d.sh.media_open = d.sh.tooltip = 0;
    d.sh.cfg.clock_seconds=0;d.realtime_second=12*3600+2;d.damage.count=0;
    desk_shell_clock_damage(&d,12*3600+1);assert(!d.damage.count);
    d.realtime_second = 12 * 3600; desk_composite(&d, (struct desk_box){0, 0, d.width, d.height});
    d.damage.count = 0; d.realtime_second = 13 * 3600;
    desk_shell_clock_damage(&d, 12 * 3600); redraw();
    /* Removing media while its popup is open also erases the old popup. */
    click(SH_TRAY_MEDIA); redraw();
    s.removable_mask = 0; desk_shell_media_update(&d, &s); assert(!d.sh.media_open); redraw();
}
static void shell_layout(void)
{
    size_t bytes=(size_t)d.width*DESK_BAR*4;
    uint32_t *top=malloc(bytes); assert(top);
    desk_composite(&d,(struct desk_box){0,0,d.width,d.height});
    memcpy(top,d.pixels,bytes);
    /* Focus, captions and notices must never add text to the free top bar. */
    d.focus=0; strcpy(d.clients[0].title,"Caption belongs to the window");
    strcpy(d.notice,"No top-bar notice");
    desk_composite(&d,(struct desk_box){0,0,d.width,DESK_BAR});
    assert(!memcmp(top,d.pixels,bytes)); d.focus=-1; d.notice[0]=0;
    /* The direct fixture title edit also changes its bottom task label. */
    desk_composite(&d,(struct desk_box){0,0,d.width,d.height});
    struct desk_box orb=desk_start_button(&d);
    assert(orb.w==38 && orb.h==38 && orb.y+orb.h-DESK_BAR==6);
    d.mouse_x=orb.x+orb.w/2; d.mouse_y=orb.y+orb.h/2;
    d.damage.count=0; desk_shell_motion(&d); assert(d.sh.start_hot); redraw();
    assert(memcmp(top,d.pixels,bytes)); memcpy(top,d.pixels,bytes);
    assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1) && d.menu_open); redraw();
    assert(memcmp(top,d.pixels,bytes));
    assert(desk_menu_box(&d).x==orb.x);
    desk_menu_close(&d); redraw();
    d.mouse_x=0; d.mouse_y=DESK_BAR+1; desk_shell_motion(&d); redraw();
    desk_shell_notify(&d,"Notification","Anchored below the top bar",DU_DLG_ICON_INFO,SH_NOTIFY_DETAILS);
    struct desk_box notice=desk_shell_notification_bounds(&d);
    assert(notice.y>=DESK_BAR && notice.y+notice.h<=(int)d.height-DESK_BAR);
    redraw(); assert(desk_shell_key(&d,DU_KEY_ESC,1)); redraw();
    free(top);
}
int main(void)
{
    FILE *f=fopen(DU_ICON_PORTRAIT_PATH,"rb"); assert(f);
    assert(fread(portrait,4,256*256,f)==256*256 && !fclose(f));
    const unsigned widths[] = {320, 640, 800, 1024}, heights[]={240,480,600,768};
    for (int theme = 0; theme < desk_theme_count(); theme++) for (unsigned size = 0; size < 4; size++) for (int rgb565=0; rgb565<2; rgb565++) {
        fake_reset(); desk_init(&d, &fake_ops, widths[size], heights[size]);
        d.hw_cursor=1; /* Scene parity; cursor movement is covered by the presenter replay. */
        d.portrait=portrait;
        d.icon = store; desk_shell_enable(&d);
        snprintf(d.sh.cfg.theme, sizeof(d.sh.cfg.theme), "%s", desk_theme_at(theme)->name);
        desk_shell_apply(&d);
        d.lfb_bytes_per_pixel=rgb565 ? 2 : 4;
        d.pixels = calloc((size_t)d.width * d.height, 4); assert(d.pixels);
        for (int i = 0; i < DESK_CLIENTS; i++) { d.clients[i].window = 1; d.clients[i].minimized = 1; }
        d.realtime_second = 12 * 3600; d.sh.cfg.clock_24h = 1; d.sh.cfg.clock_seconds = 0;
        geometry("12:00", 0);
        printf("TRAY_GEOMETRY %s %u width=%d clock=%d\n", desk_theme->name, d.width,
               desk_shell_tray_box(&d).w, du_text_width("12:00"));
        icon(SH_TRAY_VOLUME, "audio-volume-high", 0); icon(SH_TRAY_NETWORK, "network-error", 1);
        d.sh.media.removable_mask = 1u << 4; d.sh.mixer_state = -1; d.sh.muted = 1;
        geometry("12:00", 1);
        icon(SH_TRAY_VOLUME, "audio-volume-muted", 1); icon(SH_TRAY_MEDIA, "drive-removable-media", 0);
        d.sh.network.present = 1; d.sh.network.link = CIUKI_NET_LINK_DOWN;
        d.sh.cfg.clock_seconds = 1; geometry("12:00:00", 1); icon(SH_TRAY_NETWORK, "network-offline", 0);
        d.sh.cfg.clock_24h = 0; d.sh.cfg.clock_seconds = 0; geometry("12:00 PM", 1);
        d.sh.network.link = CIUKI_NET_LINK_UP; d.sh.mixer_state = 2;
        d.sh.cfg.clock_seconds = 1; geometry("12:00:00 PM", 1);
        icon(SH_TRAY_NETWORK, "network-wired", 0); icon(SH_TRAY_VOLUME, "audio-volume-muted", 0);
        d.sh.cfg.clock_seconds = 0; transitions(); shell_layout();
        free(d.pixels); d.pixels = NULL; desk_render_release(&d); assert(!fake_live());
    }
    puts("PASS tray layout: both themes, four widths, four clock formats, top-bar status, empty caption area, orb states, bottom tasks and exact incremental redraws");
    return 0;
}
