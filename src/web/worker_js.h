#ifndef CIUK_WEB_WORKER_JS_H
#define CIUK_WEB_WORKER_JS_H

#include <stdint.h>

#define WJS_OK 0
#define WJS_E_ARGUMENT 1
#define WJS_E_MEMORY 2
#define WJS_E_SCRIPT 3
#define WJS_E_LIMIT 4
#define WJS_E_CANCELLED 5

#define WJS_TLV_SET_TEXT 1
#define WJS_TLV_SET_HTML 2
#define WJS_TLV_WRITE 3
#define WJS_TLV_ALERT 4

/* Output record: little-endian u16 operation, u16 id bytes, u16 text bytes,
   then the two byte strings. Records are concatenated without padding. */
#define WJS_TLV_HEADER_BYTES 6
#define WJS_MAX_ID_BYTES 255
#define WJS_MAX_TEXT_BYTES 8192
#define WJS_ELEMENT_SNAPSHOT_COUNT 32
#define WJS_ELEMENT_TEXT_BYTES 1024
#define WJS_INTERRUPT_BUDGET 256

void wjs_reset(void);
void wjs_set_cancel_flag(volatile const uint32_t *flag, uint32_t expected);
int wjs_set_element(const char *id, uint32_t id_bytes,
                    const char *text, uint32_t text_bytes);
int wjs_eval(const char *source, uint32_t source_bytes,
             uint8_t *output, uint32_t output_capacity,
             uint32_t *output_bytes);
const char *wjs_last_error(void);

#endif
