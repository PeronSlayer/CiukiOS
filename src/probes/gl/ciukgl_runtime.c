#include <stdlib.h>
#include <GL/gl.h>
#include "zbuffer.h"
#include "ciukgl.h"

struct ciukgl_context {
    ZBuffer *buffer;
    int width, height;
};

ciukgl_context *ciukgl_create(int width, int height)
{
    ciukgl_context *context;
    if (width < 16 || height < 16 || width > 2048 || height > 2048) return 0;
    context = malloc(sizeof *context);
    if (!context) return 0;
    context->buffer = ZB_open(width, height, ZB_MODE_5R6G5B, 0, 0, 0, 0);
    if (!context->buffer) { free(context); return 0; }
    context->width = width;
    context->height = height;
    glInit(context->buffer);
    glViewport(0, 0, width, height);
    return context;
}

const unsigned short *ciukgl_pixels(const ciukgl_context *context)
{
    return context ? context->buffer->pbuf : 0;
}

void ciukgl_destroy(ciukgl_context *context)
{
    if (!context) return;
    glClose();
    ZB_close(context->buffer);
    free(context);
}
