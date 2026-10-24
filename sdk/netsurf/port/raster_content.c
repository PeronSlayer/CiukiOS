/* Native image content handler using the project's pinned PNG/JPEG decoder.
 * Follows NetSurf 3.11's bmp.c lifecycle and bitmap format contract.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "utils/errors.h"
#include "utils/utils.h"
#include "netsurf/plotters.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"
#include "content/llcache.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"
#include "raster.h"
#include "image_memory.h"
struct raster_content { struct content base; void *bitmap; };
static nserror raster_create(const struct content_handler *handler,lwc_string *mime,
    const struct http_parameter *params,llcache_handle *cache,const char *charset,bool quirks,struct content **out)
{
    struct raster_content *r=calloc(1,sizeof(*r));if(!r)return NSERROR_NOMEM;
    nserror error=content__init(&r->base,handler,mime,params,cache,charset,quirks);
    if(error) { free(r);return error; }
    *out=&r->base;return NSERROR_OK;
}
static bool raster_convert(struct content *c)
{
    struct raster_content *r=(void *)c;size_t bytes;int w=0,h=0;
    const uint8_t *source=content__get_source_data(c,&bytes);
    unsigned char *rgba=ciuki_raster_decode(source,bytes,&w,&h);
    if(!rgba) { content_broadcast_error(c,NSERROR_BAD_CONTENT,NULL);return false; }
    r->bitmap=guit->bitmap->create(w,h,BITMAP_NONE);
    if(!r->bitmap) { ciuki_image_free(rgba);content_broadcast_error(c,NSERROR_NOMEM,NULL);return false; }
    uint8_t *out=guit->bitmap->get_buffer(r->bitmap);size_t stride=guit->bitmap->get_rowstride(r->bitmap);bool opaque=true;
    for(int y=0;y<h;y++) {
        memcpy(out+(size_t)y*stride,rgba+(size_t)y*w*4,(size_t)w*4);
        for(int x=0;x<w;x++)if(rgba[((size_t)y*w+x)*4+3]!=255)opaque=false;
    }
    ciuki_image_free(rgba);
    bitmap_format_to_client(r->bitmap,&(bitmap_fmt_t){.layout=BITMAP_LAYOUT_R8G8B8A8});
    guit->bitmap->set_opaque(r->bitmap,opaque);guit->bitmap->modified(r->bitmap);
    c->width=w;c->height=h;c->size+=(size_t)w*h*4;
    content_set_ready(c);content_set_done(c);content_set_status(c,"");return true;
}
static void raster_destroy(struct content *c)
{ struct raster_content *r=(void *)c;if(r->bitmap)guit->bitmap->destroy(r->bitmap); }
static bool raster_redraw(struct content *c,struct content_redraw_data *data,const struct rect *clip,const struct redraw_context *ctx)
{
    (void)clip;struct raster_content *r=(void *)c;
    bitmap_flags_t flags=(data->repeat_x?BITMAPF_REPEAT_X:0)|(data->repeat_y?BITMAPF_REPEAT_Y:0);
    return r->bitmap && ctx->plot->bitmap(ctx,r->bitmap,data->x,data->y,data->width,data->height,data->background_colour,flags)==NSERROR_OK;
}
static nserror raster_clone(const struct content *old,struct content **out)
{
    struct raster_content *r=calloc(1,sizeof(*r));if(!r)return NSERROR_NOMEM;
    nserror error=content__clone(old,&r->base);
    if(error) { content_destroy(&r->base);return error; }
    if((old->status==CONTENT_STATUS_READY || old->status==CONTENT_STATUS_DONE) && !raster_convert(&r->base)) {
        content_destroy(&r->base);return NSERROR_CLONE_FAILED;
    }
    *out=&r->base;return NSERROR_OK;
}
static void *raster_internal(const struct content *c,void *context)
{ (void)context;return ((const struct raster_content *)c)->bitmap; }
static content_type raster_type(void) { return CONTENT_IMAGE; }
static bool raster_opaque(struct content *c)
{ struct raster_content *r=(void *)c;return r->bitmap && guit->bitmap->get_opaque(r->bitmap); }
static const content_handler raster_handler={.create=raster_create,.data_complete=raster_convert,
    .destroy=raster_destroy,.redraw=raster_redraw,.clone=raster_clone,.get_internal=raster_internal,
    .type=raster_type,.is_opaque=raster_opaque,.no_share=false};
static const char *raster_types[]={"image/png","image/x-png","image/jpeg","image/jpg","image/pjpeg"};
CONTENT_FACTORY_REGISTER_TYPES(ciuki_raster,raster_types,raster_handler);
