/* Production shell tray, damage, popup and tooltip observations.
 * SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct desktop d;
static char texts[24][128];
static unsigned text_count;
void __real_du_text(struct du_canvas *, int, int, const char *, uint32_t);
void __wrap_du_text(struct du_canvas *c, int x, int y, const char *s, uint32_t colour)
{
    assert(text_count < 24);
    snprintf(texts[text_count++], sizeof(texts[0]), "%s", s);
    __real_du_text(c, x, y, s, colour);
}
static int drawn(const char *s)
{ for (unsigned i = 0; i < text_count; i++) if (!strcmp(texts[i], s)) return 1; return 0; }
static void render(void)
{
    for (unsigned i = 0; i < d.width * d.height; i++) d.pixels[i] = DU_FACE;
    text_count = 0; desk_shell_overlay(&d, (struct desk_box){0, 0, d.width, d.height});
}
static void click(int x, int y)
{
    d.mouse_x = x; d.mouse_y = y;
    assert(desk_shell_button(&d, CIUKI_BUTTON_LEFT, 1));
    desk_shell_button(&d, CIUKI_BUTTON_LEFT, 0);
}
static uint32_t icon_pixels[16 * 16];
static void check_icon(const char *name, int disabled)
{
    struct du_canvas c = {icon_pixels, 16, 16, {0, 0, 16, 16}};
    for (unsigned i = 0; i < 256; i++) icon_pixels[i] = DU_FACE;
    du_icon_blit(&c, 0, 0, desk_shell_icon(name, 16), 16, disabled ? DU_ICON_DISABLED : DU_ICON_NORMAL);
    int x = (int)d.width - SH_TRAY_W, y = (int)d.height - 24;
    for (unsigned row = 0; row < 16; row++)
        assert(!memcmp(&d.pixels[(y + row) * d.width + x], &icon_pixels[row * 16], 16 * 4));
}
static void hover(int x, int y)
{
    d.mouse_x = x; d.mouse_y = y; desk_shell_motion(&d);
    d.now_ns += 700000000ull; desk_shell_tick(&d);
    assert(d.sh.tooltip == 2); render();
}
int main(void)
{
    fake_reset(); desk_init(&d, &fake_ops, 640, 480); desk_shell_enable(&d);
    d.pixels = calloc(d.width * d.height, 4); assert(d.pixels);
    const int nx = (int)d.width - SH_TRAY_W + 8, ny = (int)d.height - 16;
    d.now_ns = 1; d.damage.count = 0; desk_shell_tick(&d);
    assert(d.sh.want_reload & 16); d.sh.want_reload = 0;
    d.now_ns = 2000000000ull; desk_shell_tick(&d); assert(!(d.sh.want_reload & 16));
    d.now_ns++; desk_shell_tick(&d); assert(d.sh.want_reload & 16);
    struct ciuki_net_state n = { .version = 1, .size = sizeof(n) };
    desk_shell_network_update(&d, &n); render(); check_icon("network-error", 1);
    hover(nx, ny); assert(drawn("No network adapter"));
    click(nx, ny); assert(d.sh.network_open && !d.sh.volume_open && !d.sh.tooltip);
    render(); assert(drawn("No network adapter"));
    assert(desk_shell_key(&d, DU_KEY_ESC, 1) && !d.sh.network_open);
    n.present = 1; n.link = CIUKI_NET_LINK_DOWN; n.state = CIUKI_NET_RUNNING;
    strcpy(n.driver, "e100"); strcpy(n.reason, "admission");
    const uint8_t mac[] = {0x00, 0xd0, 0x59, 0xb6, 0x43, 0x8f}; memcpy(n.mac, mac, 6);
    d.damage.count = 0; desk_shell_network_update(&d, &n);
    assert(d.damage.count == 1);
    struct desk_box tray = d.damage.boxes[0];
    assert(tray.x == 640 - SH_TRAY_W - 4 && tray.y == 452 && tray.w == SH_TRAY_W && tray.h == 25);
    render(); check_icon("network-offline", 0);
    hover(nx, ny); assert(drawn("Network: no cable"));
    click(nx, ny); render();
    assert(d.sh.network_open && drawn("Driver: e100") && drawn("MAC: 00:d0:59:b6:43:8f"));
    assert(drawn("Link: no cable") && drawn("State: running") && drawn("Reason: admission"));
    n.link = CIUKI_NET_LINK_UP; desk_shell_network_update(&d, &n); render();
    assert(drawn("Link: cable connected")); check_icon("network-wired", 0);
    click(nx, ny); assert(!d.sh.network_open);
    hover(nx, ny); assert(drawn("Network: cable connected (e100 00:d0:59:b6:43:8f)"));
    /* State changes erase the old tooltip as well as drawing the new width. */
    n.link = CIUKI_NET_LINK_DOWN; desk_shell_network_update(&d, &n); render();
    assert(drawn("Network: no cable"));
    d.sh.tooltip = 0; d.damage.count = 0;
    n.rx_frames++; n.tx_frames++; desk_shell_network_update(&d, &n); assert(!d.damage.count);
    desk_shell_network_update(&d, &n); assert(!d.damage.count);
    click(nx, ny); n.state = CIUKI_NET_QUARANTINED; strcpy(n.reason, "link_fault");
    d.damage.count = 0; desk_shell_network_update(&d, &n);
    assert(d.damage.count && d.damage.count<=DESK_DAMAGE_MAX);
    /* Preserve the exact union even where the popup shadow overlaps the
     * tray; bounding-box merging used to include clean corner pixels. */
    struct desk_box popup={360,332,282,122};
    for (int y=0;y<480;y++) for (int x=0;x<640;x++) {
        unsigned covered=0;
        for (unsigned i=0;i<d.damage.count;i++) covered+=(unsigned)desk_contains(d.damage.boxes[i],x,y);
        unsigned wanted=desk_contains(tray,x,y)||desk_contains(popup,x,y);
        assert(covered==wanted);
    }
    for (unsigned i = 0; i < d.damage.count; i++) {
        struct desk_box b = d.damage.boxes[i];
        assert(b.x >= 360 && b.y >= 332 && b.x + b.w <= 640 && b.y + b.h <= 478);
    }
    render(); assert(drawn("State: quarantined") && drawn("Reason: link_fault"));
    click(nx + 20, ny); assert(!d.sh.network_open && d.sh.volume_open);
    click(nx, ny); assert(d.sh.network_open && !d.sh.volume_open);
    click(10, 10); assert(!d.sh.network_open);
    free(d.pixels); d.pixels = 0; desk_render_release(&d);
    assert(!fake_live());
    puts("PASS network tray: three Tango icons/disabled rendering, exact tooltips, popup, carrier/quarantine, two-second poll and bounded damage");
    return 0;
}
