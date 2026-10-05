#ifndef CIUK_WEBMODULE_H
#define CIUK_WEBMODULE_H
#include "app.h"
struct webmodule {u16 segment,entry;};
int webmodule_load(struct webmodule *module,const char *path);
int webmodule_call(struct webmodule *module,int event,void *request);
void webmodule_free(struct webmodule *module);
#endif
