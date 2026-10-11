/* SPDX-License-Identifier: MIT
 * Desktop wallpaper engine: the approved Ciuki portrait on the desktop
 * colour (default), procedural CiukiOS wallpapers rendered at the screen's
 * resolution, and image files (PNG/JPEG/BMP, image.c) placed centred,
 * stretched, tiled or fitted. Pure memory functions, integer-only. */
#ifndef CIUKI_DESKTOP_WALLPAPER_H
#define CIUKI_DESKTOP_WALLPAPER_H
#include <stddef.h>
#include <stdint.h>
#define DESK_IMAGE_MAX_SIDE 4096
#define DESK_IMAGE_MAX_PIXELS (16u * 1024u * 1024u)
#define DESK_IMAGE_FILE_MAX (24u * 1024u * 1024u)
struct desk_image { uint32_t *pixels, width, height; };   /* ARGB8888 */
struct desk_builtin_wallpaper { const char *id, *label; };
#define DESK_BUILTIN_WALLPAPERS 6
extern const struct desk_builtin_wallpaper desk_builtin_wallpapers[DESK_BUILTIN_WALLPAPERS];
int desk_image_decode(const uint8_t *data, size_t size, struct desk_image *out);
void desk_image_free(struct desk_image *);
/* Renders the wallpaper into out (width*height XRGB). spec is the
 * settings value ("ciuki", "none", "builtin:<id>", or a path whose decoded
 * image is passed in image). portrait is the 256x256 approved array or NULL.
 * Returns 0, or -1 when the request cannot be honoured (the plain desktop
 * colour has been drawn instead). */
int desk_wallpaper_render(uint32_t *out, uint32_t width, uint32_t height, uint32_t top, uint32_t bottom,
                          const char *spec, int mode, uint32_t colour,
                          const struct desk_image *image, const uint32_t *portrait);
/* Scales an ARGB image into a w*h ARGB box with the given mode, for previews. */
void desk_image_scale(const struct desk_image *src, uint32_t *dst, uint32_t w, uint32_t h, int mode, uint32_t colour);
void desk_builtin_paint(const char *id, uint32_t *out, uint32_t width, uint32_t height, uint32_t stride);
#endif
