/* SPDX-License-Identifier: MIT
 * Image Viewer: shows PNG, JPEG, BMP, GIF and raw square XRGB images (the
 * desktop's portrait and wallpaper caches), with zoom, fit, scrolling and the
 * previous/next image of the folder, as the 0.8 viewer. Decoding: stb_image
 * (third_party/stb). Scaling and drawing are integer-only. */
#ifndef CIUKI_ACC_VIEWER_H
#define CIUKI_ACC_VIEWER_H
#include "acc.h"
#define VIEWER_MAX_SIDE 8192
#define VIEWER_MAX_PIXELS (4096u * 4096u)
#define VIEWER_MAX_FILE (32u << 20)
#define VIEWER_PATH 512
#define VIEWER_FOLDER 256
struct viewer_image { uint32_t *pixels; int width, height; char format[8]; };
/* Decodes a file's bytes; 0 or a negative errno-like code (-EINVAL unsupported, -EFBIG too large, -ENOMEM). */
int viewer_decode(const uint8_t *data, size_t size, const char *name, struct viewer_image *);
void viewer_free(struct viewer_image *);
int viewer_is_image_name(const char *name);
/* Zoom in percent; 0 means best fit. */
int viewer_zoom_step(int zoom, int direction);
/* Scales (nearest, integer) the visible part of an image into a canvas rectangle. */
void viewer_blit(struct du_canvas *, struct du_rect area, const struct viewer_image *, int zoom_percent, int scroll_x, int scroll_y);
int viewer_fit_percent(const struct viewer_image *, int area_w, int area_h);

struct viewer_fs {
    /* Reads a whole file (malloc'd); 0 or -errno. */
    int (*read)(void *ctx, const char *path, uint8_t **data, size_t *size, size_t limit);
    du_lister list; void *ctx;
};
#define VIEWER_W 560
#define VIEWER_H 420
struct viewer_load;
struct viewer_app {
    struct viewer_load *loading; int asynchronous;
    struct du_ui *ui; const struct viewer_fs *fs;
    void (*quit)(void *host); void *host;
    int (*set_wallpaper)(void *, const char *);
    struct viewer_image image; int loaded; char error[160];
    char path[VIEWER_PATH], folder[VIEWER_PATH];
    char names[VIEWER_FOLDER][96]; unsigned count; int index;
    int zoom;                 /* percent, 0 = best fit */
    int scroll_x, scroll_y;   /* image pixels at the top-left of the view */
    struct du_file_entry *entries;
    struct du_menu menus[3]; struct du_menu_item file_items[6], view_items[6], help_items[1];
};
enum { V_M_OPEN = 1, V_M_PREV, V_M_NEXT, V_M_EXIT, V_M_IN, V_M_OUT, V_M_ACTUAL, V_M_FIT, V_M_ABOUT, V_M_WALLPAPER,
       V_STATUS = 100, V_PREV, V_NEXT, V_IN, V_OUT, V_FIT, V_ACTUAL, V_D_OPEN = 200, V_D_INFO };
int viewer_attach(struct viewer_app *, struct du_ui *, const struct viewer_fs *);
void viewer_wallpaper_callback(struct viewer_app *, int (*)(void *, const char *));
void viewer_detach(struct viewer_app *);
void viewer_poll(struct viewer_app *);
int viewer_open(struct viewer_app *, const char *path);
void viewer_event(struct du_ui *, const struct du_event *, void *);
void viewer_paint(struct du_ui *, void *);
void viewer_layout(struct viewer_app *);
struct du_rect viewer_area(const struct viewer_app *);
const struct viewer_fs *viewer_native_fs(void);
#endif
