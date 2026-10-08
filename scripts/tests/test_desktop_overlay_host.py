"""Execute the desktop's production event/paint code on clipped host pixels."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DESKTOP = ROOT / 'src/apps/desktop.c'


def c_function(source, name):
    match = re.search(r'(?m)^(?:static\s+)?(?:int|void)\s+' + re.escape(name)
                      + r'\s*\([^)]*\)\s*\{', source)
    if not match:
        raise AssertionError(f'cannot find production function {name}')
    brace = source.index('{', match.start())
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():pos + 1]
    raise AssertionError(f'unclosed production function {name}')


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 80
#define H 64
struct host { int window, x, y; } host;
#define HOST host
struct popup { int open, x, y, w, h; } pop;
struct dialog { int win, open; } dlg, pdlg;
struct webmodule { int segment; } wallpaper_module = {1};
struct { int open, drag; } vol;
struct { int sel; } icons[2];
static int nicons = 2, dragging, drag_dx, drag_dy;
static int box_state, box_x0, box_x1, box_y0, box_y1;
static int seen_changes, top_seen_fs, ui_dirty, m_start[15];
static char app_title[64];
static unsigned char surface[H][W], expected[H][W];
static int clip_top, clip_bottom, wallpaper_calls, icon_calls;
static void ui_rect(int x, int y, int w, int h, int colour)
{
    int xx, yy;
    for (yy=y; yy<y+h; ++yy) for (xx=x; xx<x+w; ++xx)
        if (xx>=0 && xx<W && yy>=clip_top && yy<clip_bottom)
            surface[yy][xx] = (unsigned char)colour;
}
static void top_paint(void) { ui_rect(0, 0, W, 5, 4); }
static void draw_icon(int i, int x, int y, int ghost)
{
    ++icon_calls;
    ui_rect(72 + x, 10 + i * 16 + y, 6, 10, ghost ? 9 : 15);
}
static void vol_draw(void) { ui_rect(12, 12, 22, 8, 7); }
static void popup_draw(struct popup *p)
{
    if (p->open) ui_rect(p->x, p->y, p->w, p->h, 7);
}
static int webmodule_call(struct webmodule *m, int ev, int arg)
{
    assert(m == &wallpaper_module && ev == EV_PAINT && arg == 0);
    ++wallpaper_calls;
    ui_rect(0, 0, W, H, 3);
    return 0;
}
static int dialog_mine(struct dialog *d) { return d->win && d->win == HOST.window; }
static void dialog_draw(struct dialog *d) { (void)d; ui_rect(25, 18, 30, 20, 7); }
static int dialog_pre(struct dialog *d, int w, int *ev, int *a)
{ (void)d; (void)w; (void)ev; (void)a; return -1; }
static void dialog_sync(struct dialog *d) { (void)d; }
static void dialog_hover(struct dialog *d, int x, int y) { (void)d; (void)x; (void)y; }
static int dialog_mouse(struct dialog *d, int a, int x, int y)
{ (void)d; (void)a; (void)x; (void)y; return 0; }
static void dialog_result(int r) { (void)r; }
static int props_event(int ev, int a, int b, int c)
{ (void)ev; (void)a; (void)b; (void)c; return 0; }
static void vol_close(void) { vol.open = 0; }
static void vol_open(void) { vol.open = 1; }
static void menu_close(void) { pop.open = 0; }
static void menu_open(int *items, int n, int x, int y, const char *label)
{ (void)items; (void)n; (void)x; (void)y; (void)label; pop.open = 1; }
static void str_copy(char *to, const char *from) { strcpy(to, from); }
static int fs_changes(void) { return 0; }
static void reload(void) {}
static void top_load_ram_total(void) {}
static void top_find_mixer(void) {}
static void top_sample(void) {}
static int on_mouse(int a, int x, int y) { (void)a; (void)x; (void)y; return 0; }
static int on_key(int a) { (void)a; return 0; }
static int wallpaper_poll(void) { return 0; }
static int poll(void) { return 0; }
static void wallpaper_close(void) {}
'''

CASES = r'''
static void compose_client(void)
{
    int x, y;
    /* A static DOS client with distinct rows; no animation can repair it. */
    for (y=8; y<58; ++y) for (x=6; x<70; ++x)
        if (y >= clip_top && y < clip_bottom)
            surface[y][x] = (unsigned char)(16 + (x/4 + y/4) % 64);
}
static void fresh_band(int top, int bottom)
{
    memset(surface, 0xa5, sizeof surface);
    clip_top=top; clip_bottom=bottom;
    wallpaper_calls=icon_calls=0; vol.open=pop.open=0;
    HOST.window=WIN_DESKTOP;
    assert(app_event(EV_PAINT, 0, 0, 0)==0);
    assert(wallpaper_calls==1 && icon_calls==2);
    compose_client();
}
static void compare_outside(int x, int y, int w, int h)
{
    int xx, yy;
    for (yy=0; yy<H; ++yy) for (xx=0; xx<W; ++xx)
        if (!(xx>=x && xx<x+w && yy>=y && yy<y+h))
            if (surface[yy][xx]!=expected[yy][xx]) {
                fprintf(stderr,"compare_outside: overwritten pixel %d,%d\n",xx,yy);
                exit(2);
            }
}
int main(void)
{
    int top, bottom;
    for (top=0; top<H; top+=8) {
        bottom=top+8;
        fresh_band(top,bottom);
        memcpy(expected,surface,sizeof surface);
        HOST.window=WIN_OVERLAY; vol.open=1;
        assert(app_event(EV_PAINT,0,0,0)==0);
        compare_outside(12,12,22,8);
        assert(wallpaper_calls==1 && icon_calls==2);
        /* Closing it recompiles this band from background and client source. */
        vol.open=0; HOST.window=WIN_DESKTOP;
        app_event(EV_PAINT,0,0,0); compose_client();
        assert(!memcmp(surface,expected,sizeof surface));

        fresh_band(top,bottom);
        ui_rect(30,25,32,28,7); /* Another native window above the DOS client. */
        memcpy(expected,surface,sizeof surface);
        HOST.window=WIN_OVERLAY; pop.open=1;
        pop.x=4; pop.y=5; pop.w=26; pop.h=36;
        app_event(EV_PAINT,0,0,0);
        assert(wallpaper_calls==1 && icon_calls==2);
        compare_outside(pop.x,pop.y,pop.w,pop.h);
    }
    fresh_band(0,H);
    memcpy(expected,surface,sizeof surface);
    HOST.window=WIN_OVERLAY; app_event(EV_PAINT,0,0,0);
    assert(!memcmp(surface,expected,sizeof surface) && wallpaper_calls==1);
    HOST.window=22; dlg.win=22; app_event(EV_PAINT,0,0,0);
    assert(wallpaper_calls==1); compare_outside(25,18,30,20);
    dlg.win=0; wallpaper_module.segment=0; HOST.window=WIN_DESKTOP;
    app_event(EV_PAINT,0,0,0); assert(wallpaper_calls==1);
    puts("PASS eight clipped bands: Volume/menu retain DOS and native windows; exposure, empty overlay, dialog, absent wallpaper");
    return 0;
}
'''


def build_harness(source):
    header = (ROOT / 'src/apps/app.h').read_text()
    constants = '\n'.join(line.split('/*', 1)[0] for line in header.splitlines()
                          if re.match(r'#define (?:EV_\w+|WIN_DESKTOP|WIN_OVERLAY|TITLE_H|MOUSE_HOVER|C_TITLE)\b', line))
    return (constants + '\n' + HARNESS + '\n'
            + c_function(source, 'paint') + '\n'
            + c_function(source, 'app_event') + '\n' + CASES)


class DesktopOverlayHostTests(unittest.TestCase):
    def run_harness(self, source, expected_success=True):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('host C compiler not available')
        with tempfile.TemporaryDirectory(prefix='desktop-overlay-') as tmp:
            src, exe = Path(tmp) / 'overlay.c', Path(tmp) / 'overlay'
            src.write_text(build_harness(source))
            subprocess.run([compiler, '-std=c99', '-Wall', '-Wextra', '-Werror',
                            str(src), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            if expected_success:
                self.assertEqual(result.returncode, 0, result.stderr)
            else:
                self.assertNotEqual(result.returncode, 0,
                                    'regression harness did not detect wallpaper over client pixels')
                self.assertIn('compare_outside', result.stderr,
                              'original defect must fail the pixel-retention assertion')

    def test_production_layers_preserve_idle_window_pixels(self):
        self.run_harness(DESKTOP.read_text())

    def test_original_overlay_wallpaper_defect_is_detected(self):
        source = DESKTOP.read_text()
        guarded = 'if(HOST.window==WIN_DESKTOP&&wallpaper_module.segment)'
        self.assertIn(guarded, source)
        self.run_harness(source.replace(guarded, 'if(wallpaper_module.segment)', 1), False)


if __name__ == '__main__':
    unittest.main()
