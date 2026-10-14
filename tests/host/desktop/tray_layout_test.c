/* Production tray geometry, Tango pixels and redraw transitions. SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct desktop d;
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
    int cw = du_text_width(text), count = media ? 3 : 2;
    struct desk_box t = desk_shell_tray_box(&d);
    assert(t.w == 12 + cw + count * 20 && t.x + t.w == (int)d.width - 4);
    assert(t.y == (int)d.height - 28 && t.h == 25);
    int right = t.x + t.w - 6 - cw;
    for (int i = 0; i < count; i++) {
        struct desk_box b = desk_shell_tray_icon_box(&d, (enum sh_tray_icon)i);
        assert(b.x + b.w + 4 == right && b.w == 16 && b.h == 16 && b.y == t.y + 4);
        right = b.x;
    }
    assert(right == t.x + 6);
    if (!media) assert(!desk_shell_tray_icon_box(&d, SH_TRAY_MEDIA).w);
    desk_composite(&d, (struct desk_box){0, 0, d.width, d.height});
    assert(clock_x == t.x + t.w - 6 - cw && clock_y == t.y + (t.h - DU_GLYPH_H) / 2);
    for (int i = 0; i < DESK_CLIENTS; i++) {
        struct desk_box b = desk_task_button(&d, i);
        assert(b.w > 0 && b.x + b.w <= t.x - 8);
    }
}
static void icon(enum sh_tray_icon which, const char *name, int disabled)
{
    uint32_t pixels[256];
    for (int i = 0; i < 256; i++) pixels[i] = desk_theme->taskbar;
    struct du_canvas c = {pixels, 16, 16, {0, 0, 16, 16}};
    const uint32_t *source = du_icon_argb(name, 16); assert(source);
    assert(desk_shell_icon(name, 16) == source);
    du_icon_blit(&c, 0, 0, source, 16, disabled ? DU_ICON_DISABLED : DU_ICON_NORMAL);
    struct desk_box b = desk_shell_tray_icon_box(&d, which);
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
        if (popup < 3) click((enum sh_tray_icon)popup);
        else d.sh.tooltip = 2;
        desk_composite(&d, (struct desk_box){0, 0, d.width, d.height});
        d.damage.count = 0;
        uint64_t previous = d.realtime_second;
        int old_width = desk_shell_tray_box(&d).w;
        d.realtime_second = 10 * 3600;
        assert(desk_shell_tray_box(&d).w != old_width);
        desk_shell_clock_damage(&d, previous); redraw();
        old_width = desk_shell_tray_box(&d).w;
        d.sh.cfg.clock_seconds = 1;
        desk_shell_tray_damage(&d, old_width); redraw();
        old_width = desk_shell_tray_box(&d).w;
        d.sh.cfg.clock_seconds = 0;
        desk_shell_tray_damage(&d, old_width); redraw();
    }
    d.sh.volume_open = d.sh.network_open = d.sh.media_open = d.sh.tooltip = 0;
    d.realtime_second = 12 * 3600; desk_composite(&d, (struct desk_box){0, 0, d.width, d.height});
    d.damage.count = 0; d.realtime_second = 13 * 3600;
    desk_shell_clock_damage(&d, 12 * 3600); redraw();
    /* Removing media while its popup is open also erases the old popup. */
    click(SH_TRAY_MEDIA); redraw();
    s.removable_mask = 0; desk_shell_media_update(&d, &s); assert(!d.sh.media_open); redraw();
}
int main(void)
{
    const unsigned widths[] = {320, 640, 1024};
    for (int theme = 0; theme < desk_theme_count(); theme++) for (unsigned size = 0; size < 3; size++) {
        fake_reset(); desk_init(&d, &fake_ops, widths[size], size ? 768 : 240);
        d.icon = store; desk_shell_enable(&d);
        snprintf(d.sh.cfg.theme, sizeof(d.sh.cfg.theme), "%s", desk_theme_at(theme)->name);
        desk_shell_apply(&d);
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
        d.sh.cfg.clock_seconds = 0; transitions();
        free(d.pixels); d.pixels = NULL; desk_render_release(&d); assert(!fake_live());
    }
    puts("PASS tray layout: all themes, three widths, four clock formats, shared Tango baseline/disabled pixels and exact incremental redraws");
    return 0;
}
