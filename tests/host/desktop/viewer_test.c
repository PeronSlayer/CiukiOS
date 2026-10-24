/* Image Viewer host tests: decoders (PNG, JPEG, BMP, GIF via stb_image; raw
 * XRGB), integer scaling, folder navigation, zoom and the window.
 * SPDX-License-Identifier: MIT */
#include "viewer.h"
#include "../../../apps/libdeskui/recent.h"
#include "acc_test.h"
#include "wallpaper.h"
#include <time.h>
#include <errno.h>
int setenv(const char *, const char *, int);
static const unsigned char jpeg16x8[] = {
0xff,0xd8,0xff,0xe0,0x00,0x10,0x4a,0x46,0x49,0x46,0x00,0x01,0x01,0x00,0x00,0x01,0x00,0x01,0x00,0x00,0xff,0xdb,0x00,0x43,0x00,0x02,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x01,0x01,0x02,0x02,0x02,0x02,0x02,0x04,0x03,0x02,0x02,0x02,0x02,0x05,0x04,0x04,0x03,0x04,0x06,0x05,0x06,0x06,0x06,0x05,0x06,0x06,0x06,0x07,0x09,0x08,0x06,0x07,0x09,0x07,0x06,0x06,0x08,0x0b,0x08,0x09,0x0a,0x0a,0x0a,0x0a,0x0a,0x06,0x08,0x0b,0x0c,0x0b,0x0a,0x0c,0x09,0x0a,0x0a,0x0a,0xff,0xdb,0x00,0x43,0x01,0x02,0x02,0x02,0x02,0x02,0x02,0x05,0x03,0x03,0x05,0x0a,0x07,0x06,0x07,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0x0a,0xff,0xc0,0x00,0x11,0x08,0x00,0x08,0x00,0x10,0x03,0x01,0x22,0x00,0x02,0x11,0x01,0x03,0x11,0x01,0xff,0xc4,0x00,0x1f,0x00,0x00,0x01,0x05,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0xff,0xc4,0x00,0xb5,0x10,0x00,0x02,0x01,0x03,0x03,0x02,0x04,0x03,0x05,0x05,0x04,0x04,0x00,0x00,0x01,0x7d,0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xff,0xc4,0x00,0x1f,0x01,0x00,0x03,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0xff,0xc4,0x00,0xb5,0x11,0x00,0x02,0x01,0x02,0x04,0x04,0x03,0x04,0x07,0x05,0x04,0x04,0x00,0x01,0x02,0x77,0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xff,0xda,0x00,0x0c,0x03,0x01,0x00,0x02,0x11,0x03,0x11,0x00,0x3f,0x00,0xf8,0xbe,0xbe,0x43,0xa2,0x8a,0xfe,0xa8,0xfd,0x99,0x7f,0xf3,0x55,0xff,0x00,0xdc,0x8f,0xfe,0xee,0x1f,0xda,0x9f,0xb4,0x33,0xfe,0x69,0x9f,0xfb,0x9d,0xff,0x00,0xdd,0x43,0xff,0xd9
};
/* 4x2 24-bit BMP: red, green, blue, white / black x4 (bottom-up rows, padded to 4 bytes). */
static size_t make_bmp(uint8_t *b)
{
    memset(b, 0, 94);
    b[0] = 'B'; b[1] = 'M'; b[2] = 94; b[10] = 54; b[14] = 40; b[18] = 4; b[22] = 2; b[26] = 1; b[28] = 24; b[34] = 40;
    uint8_t *p = b + 54;                 /* bottom row first: black */
    p += 12;
    static const uint8_t top[12] = { 0,0,255, 0,255,0, 255,0,0, 255,255,255 };
    memcpy(p, top, 12);
    return 54 + 24;
}
static unsigned print_requests,setup_requests;
static void host_print(void *arg,int setup) { (void)arg;if(setup)setup_requests++;else print_requests++; }
static void write_file(const char *folder, const char *name, const void *data, size_t n)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s", folder, name);
    FILE *f = fopen(path, "wb"); assert(f && fwrite(data, 1, n, f) == n); fclose(f);
}
static int calls, last_mode; static char last_path[1100];
static int record(void *host, const char *file, int mode) { (void)host; calls++; last_mode = mode; snprintf(last_path, sizeof(last_path), "%s", file); return 0; }
int main(int argc, char **argv)
{
    assert(argc >= 3);   /* argv[1]: scratch folder; argv[2]: a PNG wallpaper; argv[3]: optional render folder */
    const char *scratch = argv[1], *png = argv[2], *folder = argc > 3 ? argv[3] : NULL;
    struct viewer_image im;
    assert(viewer_is_image_name("a.PNG") && viewer_is_image_name("b.jpeg") && viewer_is_image_name("c.xrgb") && !viewer_is_image_name("d.txt") && !viewer_is_image_name(".png"));
    assert(!viewer_decode(jpeg16x8, sizeof(jpeg16x8), "t.jpg", &im));
    assert(im.width == 16 && im.height == 8 && !strcmp(im.format, "JPEG"));
    uint32_t left = im.pixels[3 * 16 + 2], right = im.pixels[3 * 16 + 13];
    assert(((left >> 16) & 0xff) > 200 && (left & 0xff) < 60);       /* red half */
    assert((right & 0xff) > 200 && ((right >> 16) & 0xff) < 60);     /* blue half */
    viewer_free(&im);
    uint8_t bmp[128]; size_t bn = make_bmp(bmp);
    assert(!viewer_decode(bmp, bn, "t.bmp", &im) && im.width == 4 && im.height == 2 && !strcmp(im.format, "BMP"));
    assert(im.pixels[0] == 0xff0000u && im.pixels[1] == 0x00ff00u && im.pixels[2] == 0x0000ffu && im.pixels[3] == 0xffffffu && im.pixels[4] == 0);
    viewer_free(&im);
    static const uint8_t gif[] = { 'G','I','F','8','9','a',2,0,1,0,0x80,0,0, 0,0,0, 255,255,255,
                                   0x2c,0,0,0,0,2,0,1,0,0, 2,2,0x44,0x01,0, 0x3b };
    assert(!viewer_decode(gif, sizeof(gif), "t.gif", &im) && im.width == 2 && im.height == 1 && !strcmp(im.format, "GIF"));
    viewer_free(&im);
    uint32_t raw[16]; for (int i = 0; i < 16; i++) raw[i] = 0xff000000u | (uint32_t)i;
    assert(!viewer_decode((const uint8_t *)raw, sizeof(raw), "p.xrgb", &im) && im.width == 4 && im.pixels[5] == 5u);
    viewer_free(&im);
    assert(viewer_decode((const uint8_t *)raw, 12, "p.xrgb", &im) == -EINVAL);
    assert(viewer_decode((const uint8_t *)"hello world", 11, "x.png", &im) == -EINVAL);
    assert(viewer_zoom_step(100, 1) == 150 && viewer_zoom_step(100, -1) == 67 && viewer_zoom_step(800, 1) == 800 && viewer_zoom_step(10, -1) == 10);
    struct viewer_image big = { .width = 1000, .height = 500 };
    assert(viewer_fit_percent(&big, 500, 500) == 50 && viewer_fit_percent(&big, 2000, 2000) == 100);

    /* A folder with three images and a text file; HOME for the recent list. */
    write_file(scratch, "a-tiny.bmp", bmp, bn);
    write_file(scratch, "b-photo.jpg", jpeg16x8, sizeof(jpeg16x8));
    write_file(scratch, "notes.txt", "not an image", 12);
    FILE *src = fopen(png, "rb"); assert(src);
    static uint8_t pngdata[4 << 20]; size_t pn = fread(pngdata, 1, sizeof(pngdata), src); fclose(src); assert(pn > 0);
    write_file(scratch, "c-wallpaper.png", pngdata, pn);
    write_file(scratch, "d-broken.png", "broken", 6);
    setenv("HOME", scratch, 1);

    static uint32_t pixels[VIEWER_W * VIEWER_H];
    static struct du_ui ui; static struct viewer_app app;
    du_init(&ui, pixels, VIEWER_W, VIEWER_H);
    viewer_attach(&app, &ui, viewer_native_fs());
    app.printing=host_print;
    struct du_event printing={.type=DU_EV_ACTIVATE,.id=V_M_PRINT};viewer_event(&ui,&printing,&app);assert(!print_requests);
    printing.id=V_M_PAGESETUP;viewer_event(&ui,&printing,&app);assert(setup_requests==1);
    du_render(&ui);
    at_save(folder, "viewer-empty", &ui);
    char path[1024]; snprintf(path, sizeof(path), "%s/c-wallpaper.png", scratch);
    assert(!viewer_open(&app, path));
    assert(app.loaded && app.count == 4 && app.index == 2 && !strcmp(app.image.format, "PNG"));
    at_chord(&ui,DU_KEY_LCTRL,0x19);assert(print_requests==1 && app.loaded);
    struct du_recent recent; assert(!du_recent_load(&recent) && recent.count == 1 && !strcmp(recent.paths[0], path));
    du_render(&ui);
    at_save(folder, "viewer-wallpaper-fit", &ui);
    at_tap(&ui, DU_KEY_PGDN); assert(app.index == 3 && !app.loaded && strstr(app.error, "not a supported image"));
    at_tap(&ui, DU_KEY_PGDN); assert(app.index == 0 && app.loaded && app.image.width == 4);   /* wraps */
    at_click_widget(&ui, V_PREV); assert(app.index == 3);
    at_click_widget(&ui, V_PREV); assert(app.index == 2 && app.loaded);
    at_click_widget(&ui, V_ACTUAL); assert(app.zoom == 100);
    int big_image = app.image.width > viewer_area(&app).w || app.image.height > viewer_area(&app).h;
    if (big_image) { at_tap(&ui, DU_KEY_DOWN); at_tap(&ui, DU_KEY_RIGHT); assert(app.index == 2 && (app.scroll_x > 0 || app.scroll_y > 0)); }
    at_tap(&ui, 0x0d);   /* '=' zooms in */
    assert(app.zoom == 150);
    at_tap(&ui, 0x21);   /* 'f' fits */
    assert(app.zoom == 0);
    snprintf(path, sizeof(path), "%s/b-photo.jpg", scratch);
    assert(!viewer_open(&app, path) && app.index == 1);
    at_click_widget(&ui, V_ACTUAL); at_click_widget(&ui, V_IN); at_click_widget(&ui, V_IN); at_click_widget(&ui, V_IN);
    assert(app.zoom == 300);
    du_render(&ui);
    at_save(folder, "viewer-jpeg-300", &ui);
    assert(!du_recent_load(&recent) && recent.count == 3);
    uint32_t *large = calloc(760 * 520 + 1, 4); assert(large);
    large[760 * 520] = 0xdeadbeef;
    uint32_t *image_pixels = app.image.pixels;
    du_resize(&ui, large, 760, 520);
    assert(app.image.pixels == image_pixels && app.zoom == 300 && app.index == 1);
    assert(du_get(&ui, V_STATUS)->box.y == 500 && du_get(&ui, V_STATUS)->box.h == 20);
    du_render(&ui); assert(large[760 * 520] == 0xdeadbeef);
    du_resize(&ui, pixels, VIEWER_W, VIEWER_H); free(large);
    snprintf(path, sizeof(path), "%s/missing.png", scratch);
    assert(viewer_open(&app, path) == -ENOENT && strstr(app.error, "not found"));
    /* fx-19 WM_DROPFILES: the first dropped picture is shown. */
    {
        char drop[1100]; snprintf(drop,sizeof(drop),"%s/viewer-drop.data",scratch);
        FILE *f=fopen(drop,"wb"); assert(f && fprintf(f,"X\nF%s/c-wallpaper.png\nF%s/b-photo.jpg\n",scratch,scratch)>0 && !fclose(f));
        assert(!du_drop_source(drop));
        struct du_event dropped={.type=DU_EV_DROPFILES,.x=10,.y=10}; viewer_event(&ui,&dropped,&app);
        assert(app.loaded && app.index==2 && !strcmp(app.image.format,"PNG"));
        remove(drop); puts("PASS fx-19 viewer drop: first dropped picture shown");
    }
    app.asynchronous=1; snprintf(path,sizeof(path),"%s/b-photo.jpg",scratch);
    assert(!viewer_open(&app,path)); assert(viewer_open(&app,path)==-EBUSY);
    for(unsigned i=0;i<2000 && app.loading;i++) { viewer_poll(&app); if(app.loading) nanosleep(&(struct timespec){.tv_nsec=1000000},NULL); }
    assert(!app.loading && app.loaded && app.index==1 && !strcmp(app.image.format,"JPEG"));
    du_render(&ui);
    assert(!viewer_open(&app,path)); /* close safely joins an outstanding decode */
    viewer_detach(&app);
    /* G27: rotation of the picture (the view), keys, status, and the menu. */
    {
        snprintf(path, sizeof(path), "%s/a-tiny.bmp", scratch); app.asynchronous = 0;
        assert(!viewer_open(&app, path) && app.loaded && app.rotation == 0 && app.image.width == 4 && app.image.height == 2);
        uint32_t original[8]; memcpy(original, app.image.pixels, sizeof(original));
        /* Clockwise: the top-left pixel goes to the top-right corner; 4 x 2 becomes 2 x 4. */
        assert(!viewer_rotate(&app.image, 1) && app.image.width == 2 && app.image.height == 4);
        assert(app.image.pixels[0 * 2 + 1] == original[0] && app.image.pixels[0 * 2 + 0] == original[4] && app.image.pixels[3 * 2 + 1] == original[3] && app.image.pixels[3 * 2 + 0] == original[7]);
        assert(!viewer_rotate(&app.image, -1) && app.image.width == 4 && memcmp(app.image.pixels, original, sizeof(original)) == 0);
        assert(!viewer_rotate(&app.image, 2) && app.image.pixels[0] == original[7] && app.image.pixels[7] == original[0]);
        assert(!viewer_rotate(&app.image, 2) && !memcmp(app.image.pixels, original, sizeof(original)));
        for (int i = 0; i < 4; i++) assert(!viewer_rotate(&app.image, 1));
        assert(!memcmp(app.image.pixels, original, sizeof(original)) && !viewer_rotate(&app.image, 0) && !viewer_rotate(&app.image, 4) && !viewer_rotate(&app.image, 8));
        assert(!memcmp(app.image.pixels, original, sizeof(original)) && app.image.width == 4);
        struct viewer_image empty = {0}; assert(!viewer_rotate(&empty, 1));
        viewer_open(&app, path);                                                   /* a fresh load of the file as stored */
        at_tap(&ui, 0x13); assert(app.rotation == 1 && app.image.width == 2 && strstr(du_get(&ui, V_STATUS)->text, "rotated right"));
        at_tap(&ui, 0x13); assert(app.rotation == 2 && app.image.width == 4 && strstr(du_get(&ui, V_STATUS)->text, "rotated 180"));
        at_tap(&ui, 0x26); assert(app.rotation == 1);
        at_tap(&ui, 0x26); assert(app.rotation == 0 && app.image.width == 4 && !strstr(du_get(&ui, V_STATUS)->text, "rotated"));
        viewer_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = V_M_RIGHT}, &app); assert(app.rotation == 1);
        viewer_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = V_M_TURN}, &app); assert(app.rotation == 3);
        viewer_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = V_M_LEFT}, &app); assert(app.rotation == 2);
        assert(!(app.view_items[6].flags & DU_ITEM_DISABLED));
        du_render(&ui);
        at_save(folder, "viewer-rotated", &ui);
        char saved[1024];snprintf(saved,sizeof(saved),"%s/rotation.bmp",scratch);
        assert(!viewer_save_bmp(&app.image,saved));
        uint8_t *encoded=NULL;size_t length=0;
        assert(!viewer_native_fs()->read(NULL,saved,&encoded,&length,VIEWER_MAX_FILE));
        struct viewer_image back;assert(!viewer_decode(encoded,length,saved,&back));free(encoded);
        assert(back.width==app.image.width && back.height==app.image.height && !memcmp(back.pixels,app.image.pixels,(size_t)back.width*back.height*4));
        viewer_free(&back);
        assert(viewer_save_bmp(&app.image,"/no-such-g27b-folder/picture.bmp")<0);
        assert(viewer_save_bmp(NULL,saved)==-EINVAL);
        struct viewer_image over_limit={.pixels=raw,.width=8192,.height=2048};
        assert(viewer_save_bmp(&over_limit,saved)==-EFBIG);
        viewer_event(&ui,&(struct du_event){.type=DU_EV_ACTIVATE,.id=V_M_SAVE_AS},&app);
        assert(du_dialog_id(&ui)==V_D_SAVE);du_dialog_close(&ui);
        snprintf(path, sizeof(path), "%s/b-photo.jpg", scratch); viewer_open(&app, path); assert(app.rotation == 0);   /* a new picture starts upright */
    }
    /* Set As Wallpaper: the file as stored, for the formats the desktop decodes. */
    {
        app.wallpaper = NULL; du_render(&ui); viewer_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = V_M_WALL_TILE}, &app); assert(!calls);   /* no hook: nothing happens */
        app.wallpaper = record;
        snprintf(path, sizeof(path), "%s/b-photo.jpg", scratch); viewer_open(&app, path);
        du_render(&ui); assert(!(app.file_items[6].flags & DU_ITEM_DISABLED));
        viewer_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = V_M_WALL_TILE}, &app);
        assert(calls == 1 && last_mode == 1 && !strcmp(last_path, path) && ui.dialog.kind && strstr(ui.dialog.text, "tiled")); ui.dialog.kind = 0;
        viewer_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = V_M_WALL_STRETCH}, &app); assert(calls == 2 && last_mode == 2); ui.dialog.kind = 0;
        viewer_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = V_M_WALL_CENTER}, &app); assert(calls == 3 && last_mode == 0); ui.dialog.kind = 0;
        at_tap(&ui, 0x13);
        viewer_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = V_M_WALL_CENTER}, &app);
        assert(calls == 4 && strstr(ui.dialog.text, "rotation is not saved")); ui.dialog.kind = 0;   /* the view only: said so */
        assert(!acc_wallpaper_supported("x.gif") && !acc_wallpaper_supported("x.xrgb") && acc_wallpaper_supported("x.JPG") && acc_wallpaper_supported("a.bmp") && acc_wallpaper_supported("a.png") && !acc_wallpaper_supported(NULL));
        write_file(scratch, "g.gif", gif, sizeof(gif)); snprintf(path, sizeof(path), "%s/g.gif", scratch); viewer_open(&app, path);
        viewer_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = V_M_WALL_TILE}, &app);
        assert(calls == 4 && ui.dialog.kind && strstr(ui.dialog.text, "BMP, PNG and JPEG")); ui.dialog.kind = 0;   /* GIF is refused with the reason */
        app.wallpaper = NULL;
    }
    /* The settings update keeps every other key, and refuses what the desktop cannot use. */
    {
        char cfg[1100]; snprintf(cfg, sizeof(cfg), "%s/desktop-test.cfg", scratch);
        static const char original_cfg[] = "# kept\ntheme=Ciuki Blue\nwallpaper=none\nwallpaper_mode=center\nclock_24h=1\nfuture_key=stays\n";
        write_file(scratch, "desktop-test.cfg", original_cfg, strlen(original_cfg));
        assert(!acc_wallpaper_write(cfg, "/Documents/Pictures/dog.png", ACC_WALL_TILE));
        FILE *f = fopen(cfg, "rb"); assert(f); char text[2048]; size_t n = fread(text, 1, sizeof(text) - 1, f); text[n] = 0; fclose(f);
        assert(strstr(text, "wallpaper=/Documents/Pictures/dog.png") && strstr(text, "wallpaper_mode=tile") && strstr(text, "theme=Ciuki Blue") && strstr(text, "clock_24h=1") && strstr(text, "future_key=stays"));
        assert(!acc_wallpaper_write(cfg, "/x/y.JPG", ACC_WALL_STRETCH));
        f = fopen(cfg, "rb"); n = fread(text, 1, sizeof(text) - 1, f); text[n] = 0; fclose(f);
        assert(strstr(text, "wallpaper_mode=stretch") && strstr(text, "wallpaper=/x/y.JPG") && !strstr(text, "dog.png"));
        assert(acc_wallpaper_write(cfg, "relative.png", ACC_WALL_TILE) == -ENAMETOOLONG && acc_wallpaper_write(cfg, "/x/a.gif", ACC_WALL_TILE) == -EINVAL);
        assert(acc_wallpaper_write(cfg, "/x/a.png", 99) == -EINVAL);
        char longp[300]; memset(longp, 'a', sizeof(longp)); longp[0] = '/'; strcpy(longp + 270, ".png");   /* longer than CFG_PATH_MAX (G20: 256) */
        assert(acc_wallpaper_write(cfg, longp, ACC_WALL_TILE) == -ENAMETOOLONG);
        char missing[1100]; snprintf(missing, sizeof(missing), "%s/new.cfg", scratch);
        assert(!acc_wallpaper_write(missing, "/a/b.bmp", ACC_WALL_CENTER));        /* no settings file yet: defaults, then the wallpaper */
        f = fopen(missing, "rb"); n = fread(text, 1, sizeof(text) - 1, f); text[n] = 0; fclose(f); assert(strstr(text, "wallpaper=/a/b.bmp"));
        snprintf(missing, sizeof(missing), "%s/no-such-folder/x.cfg", scratch); assert(acc_wallpaper_write(missing, "/a/b.bmp", ACC_WALL_CENTER) < 0);
    }
    puts("viewer: PASS");
    return 0;
}
