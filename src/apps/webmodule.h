#ifndef CIUK_WEBMODULE_H
#define CIUK_WEBMODULE_H
#include "app.h"
enum webmodule_error {
    WEBMODULE_OK=0,
    WEBMODULE_OPEN,
    WEBMODULE_HEADER_READ,
    WEBMODULE_HEADER,
    WEBMODULE_SEEK,
    WEBMODULE_SIZE,
    WEBMODULE_ALLOC,
    WEBMODULE_REWIND,
    WEBMODULE_IMAGE_READ,
    WEBMODULE_ENTRY
};
struct webmodule {
    u16 segment,entry;
    u16 error,dos_error,largest_block;
};
int webmodule_load(struct webmodule *module,const char *path);
int webmodule_call(struct webmodule *module,int event,void *request);
void webmodule_free(struct webmodule *module);
const char *webmodule_error_text(u16 error);
#endif
