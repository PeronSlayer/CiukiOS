#ifndef CIUK_WEB_WORKER_JS_CONFIG_H
#define CIUK_WEB_WORKER_JS_CONFIG_H

/* Include before mquickjs_priv.h in every target translation unit, including
   mquickjs.c, so JSContext has the same custom-class layout everywhere. */
#define JS_CLASS_CIUK_ELEMENT (JS_CLASS_USER + 0)
#define JS_CLASS_COUNT (JS_CLASS_USER + 1)

#endif
