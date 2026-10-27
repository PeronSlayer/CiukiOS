/* Heap adapter for transport fixtures without a kernel heap. GPL-2.0-only */
#include <ciuki/kheap_limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <ciuki/sync.h>
void *kzalloc(size_t bytes) __attribute__((weak));
void *(kzalloc)(size_t bytes) { if (bytes > KHEAP_MAX_REQUEST) return 0; return calloc(1,bytes); }
void kfree(void *p) __attribute__((weak));
void kfree(void *p) { free(p); }
unsigned usb_count(void) __attribute__((weak));
unsigned usb_count(void) { return 0; }
struct usb_host;
const struct usb_host *usb_get(unsigned i) __attribute__((weak));
const struct usb_host *usb_get(unsigned i) { (void)i; return NULL; }
void input_hid_event(uint16_t,uint16_t,int32_t,uint32_t,gen_t,uint64_t) __attribute__((weak));
void input_hid_event(uint16_t t,uint16_t c,int32_t v,uint32_t d,gen_t g,uint64_t tick)
{ (void)t;(void)c;(void)v;(void)d;(void)g;(void)tick; abort(); }
