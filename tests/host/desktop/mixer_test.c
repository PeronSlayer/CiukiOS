/* Volume Control host tests: level mapping, fake mixer, window input.
 * SPDX-License-Identifier: MIT */
#include "mixer.h"
#include "acc_test.h"
#include <errno.h>
static struct ciuki_mixer_master device = { .version = 1, .size = 32, .left = 50, .right = 50, .save_state = CIUKI_MIXER_DEFAULTS };
static unsigned sets; static uint32_t last_l, last_r, last_mute; static int fail_get;
static int get(void *ctx, struct ciuki_mixer_master *m) { (void)ctx; if (fail_get) return -ENOENT; *m = device; return 0; }
static int set(void *ctx, uint32_t l, uint32_t r, uint32_t mute) { (void)ctx; sets++; last_l = l; last_r = r; last_mute = mute; return 0; }
static struct ciuki_mixer_line lines[2][6]; static unsigned line_sets;
static int get_line(void *ctx,struct ciuki_mixer_line *l) {
    (void)ctx; *l=lines[l->recording][l->line]; return 0;
}
static int set_line(void *ctx,const struct ciuki_mixer_line *l) {
    (void)ctx; line_sets++; lines[l->recording][l->line]=*l;
    lines[l->recording][l->line].available=1;
    if (l->recording && l->selected) lines[1][9-l->line].selected=0;
    return 0;
}
static void desktop_applies(struct mixer_app *a)
{
    device.left = last_l; device.right = last_r; device.mute = last_mute; device.save_state = CIUKI_MIXER_SAVED;
    mixer_result(a, 0);
}
int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : NULL;
    uint32_t l, r; int v, b;
    mixer_split(80, 0, &l, &r); assert(l == 80 && r == 80);
    mixer_split(80, 50, &l, &r); assert(l == 40 && r == 80);
    mixer_split(80, -100, &l, &r); assert(l == 80 && r == 0);
    mixer_split(150, 0, &l, &r); assert(l == 100);
    mixer_join(40, 80, &v, &b); assert(v == 80 && b == 50);
    mixer_join(80, 0, &v, &b); assert(v == 80 && b == -100);
    mixer_join(0, 0, &v, &b); assert(v == 0 && b == 0);
    for (int vol = 0; vol <= 100; vol += 5) for (int bal = -100; bal <= 100; bal += 25) {
        mixer_split(vol, bal, &l, &r); mixer_join(l, r, &v, &b);
        assert(v == vol && (vol < 50 || abs(b - bal) <= 2));   /* integer channel levels: coarse balance at low volume */
    }
    static const struct mixer_backend backend = { get, set, NULL, get_line, set_line };
    static uint32_t pixels[MIXER_W * MIXER_H];
    static struct du_ui ui; static struct mixer_app app;
    du_init(&ui, pixels, MIXER_W, MIXER_H);
    for (unsigned rec=0;rec<2;rec++) for (unsigned i=1;i<=5;i++)
        lines[rec][i]=(struct ciuki_mixer_line){.version=1,.size=sizeof(struct ciuki_mixer_line),.line=i,.recording=rec,
            .left=100,.right=100,.available=!rec || i>=4,.selected=rec && i==5};
    mixer_attach(&app, &ui, &backend);
    assert(app.available && app.volume == 50 && app.balance == 0 && strstr(app.status, "Default"));
    du_render(&ui);
    at_save(folder, "mixer-default", &ui);
    /* Keyboard: PgUp twice (+20), Right twice (balance +10); applied once after the pause. */
    at_tap(&ui, DU_KEY_PGUP); at_tap(&ui, DU_KEY_PGUP); at_tap(&ui, DU_KEY_RIGHT); at_tap(&ui, DU_KEY_RIGHT);
    assert(app.volume == 70 && app.balance == 10 && app.pending && !sets);
    du_tick(&ui, ui.now + 200000000ull);
    assert(sets == 1 && last_l == 63 && last_r == 70 && !last_mute && app.waiting);
    desktop_applies(&app);
    assert(!app.waiting && strstr(app.status, "Saved") && app.volume == 70 && app.balance == 10);
    /* Mute applies at once. */
    at_click_widget(&ui, MX_MUTE);
    assert(sets == 2 && last_mute == 1);
    desktop_applies(&app);
    assert(app.mute && !strcmp(du_get(&ui, MX_MUTE)->text, "[x] &Mute"));
    /* Each real source has its own volume/balance/mute; recording uses an
     * exclusive Select control, with absent destinations disabled. */
    struct du_widget *wave=du_get(&ui,MX_LINE_BASE+10);
    wave->value=65;
    struct du_event change={.type=DU_EV_CHANGE,.id=wave->id}; mixer_event(&ui,&change,&app);
    du_tick(&ui,ui.now+200000000ull);
    assert(line_sets==1 && lines[0][1].left==35 && app.line_volume[1]==35);
    at_click_widget(&ui,MX_LINE_BASE+12); assert(line_sets==2 && lines[0][1].mute);
    at_click_widget(&ui,MX_MODE); assert(app.recording && du_get(&ui,MX_VOLUME)->disabled);
    assert(du_get(&ui,MX_LINE_BASE+10)->disabled && !du_get(&ui,MX_LINE_BASE+40)->disabled);
    at_click_widget(&ui,MX_LINE_BASE+42);
    assert(lines[1][4].selected && !lines[1][5].selected && app.lines[4].selected);
    at_click_widget(&ui,MX_MODE); assert(!app.recording && !du_get(&ui,MX_VOLUME)->disabled);
    /* Clicking the slider's track pages the volume down. */
    struct du_widget *s = du_get(&ui, MX_VOLUME);
    at_click(&ui, s->box.x + s->box.w / 2, s->box.y + s->box.h - 24);
    assert(app.volume < 70 && app.pending);
    du_tick(&ui, ui.now + 200000000ull);
    assert(sets == 3);
    mixer_result(&app, -EPERM);
    assert(strstr(app.status, "access denied"));
    du_render(&ui);
    at_save(folder, "mixer-muted", &ui);
    /* No device. */
    fail_get = 1; mixer_refresh(&app);
    assert(!app.available && du_get(&ui, MX_VOLUME)->disabled && strstr(app.status, "unavailable"));
    puts("mixer: PASS");
    return 0;
}
