#ifndef CIUKIOS_CIUKGL_H
#define CIUKIOS_CIUKGL_H

/* One software OpenGL context per DOS/4GW process in the first release.
 * The returned pixels are RGB565, width*height, top row first. */
typedef struct ciukgl_context ciukgl_context;
ciukgl_context *ciukgl_create(int width, int height);
const unsigned short *ciukgl_pixels(const ciukgl_context *context);
void ciukgl_destroy(ciukgl_context *context);

#endif
