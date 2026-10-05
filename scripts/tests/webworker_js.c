/* Host functional harness for the production worker_js adapter and mQuickJS.
 * Generate worker_js_stdlib.h/mquickjs_atom.h with -m64 for a 64-bit host,
 * then compile this file with worker_js.c and the four vendored engine C files.
 * This file is intentionally not a stub: it links the production interpreter.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mquickjs.h"
#include "worker_js.h"
#include "worker_abi.h"

#ifdef WJS_ROM_TEST
int wjs_test_rebase_rom(JSWord *words, uint32_t word_count,
                        uintptr_t old_base, uintptr_t old_end,
                        uintptr_t new_base, const uint32_t *slots,
                        uint32_t slot_count);
#endif

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "webworker_js: FAIL line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int next_record(const uint8_t *output, uint32_t output_bytes,
                       uint32_t *offset, uint16_t *operation,
                       const uint8_t **id, uint16_t *id_bytes,
                       const uint8_t **text, uint16_t *text_bytes)
{
    uint32_t remaining;
    if (*offset > output_bytes)
        return 0;
    remaining = output_bytes - *offset;
    if (remaining < WJS_TLV_HEADER_BYTES)
        return 0;
    *operation = read_le16(output + *offset);
    *id_bytes = read_le16(output + *offset + 2);
    *text_bytes = read_le16(output + *offset + 4);
    if ((uint32_t)WJS_TLV_HEADER_BYTES + *id_bytes + *text_bytes > remaining)
        return 0;
    *id = output + *offset + WJS_TLV_HEADER_BYTES;
    *text = *id + *id_bytes;
    *offset += WJS_TLV_HEADER_BYTES + *id_bytes + *text_bytes;
    return 1;
}

int main(void)
{
    static const char script[] =
        "var n=0;"
        "function add(a,b){return a+b;}"
        "for(var i=0;i<5;i=i+1){n=add(n,i);}"
        "if(n===10){document.write('ready');}"
        "var title=document.getElementById('title');"
        "title.textContent=title.textContent+':'+n;"
        "title.innerHTML='<b>done</b>';"
        "alert('ok');";
    static const char readback[] =
        "if(document.getElementById('title').textContent!=='<b>done</b>')"
        "throw new Error('readback mismatch');";
    static const char bounded_source[] =
        "document.write('bounded');throw new Error('outside span');";
    static const char partial_error[] =
        "document.write('partial');throw new Error('expected');";
    static const char infinite_loop[] = "while(true){}";
    static const char overflow_write[] = "document.write('too long');";
#ifdef WJS_ROM_TEST
    static JSWord synthetic_rom[2];
    static const uint32_t reloc_slots[] = { 0 };
#endif
    uint8_t output[CWW_DATA_BYTES];
    uint8_t guarded[24];
    uint32_t output_bytes, offset;
    uint16_t operation, id_bytes, text_bytes;
    const uint8_t *id, *text;
    volatile uint32_t cancel_word = 1;
    char too_large[WJS_ELEMENT_TEXT_BYTES + 1];

#ifdef WJS_ROM_TEST
    /* Reproduce the DOS low-address table collision: the packed ASCII
       bytes "Id" look pointer-tagged and lie within the synthetic ROM span,
       but only generated relocation slots may be changed. */
    synthetic_rom[0] = (JSWord)(0x59f0UL + 0x20UL + 1UL);
    synthetic_rom[1] = (JSWord)0x6449UL;
    CHECK(wjs_test_rebase_rom(synthetic_rom, 2, 0x59f0UL, 0x7f5cUL,
                              0x200000UL, reloc_slots, 1));
    CHECK(synthetic_rom[0] == (JSWord)(0x200000UL + 0x20UL + 1UL));
    CHECK(synthetic_rom[1] == (JSWord)0x6449UL);
#endif

    wjs_reset();
    CHECK(wjs_set_element("title", 5, "Base", 4) == WJS_OK);
    CHECK(wjs_eval(script, (uint32_t)(sizeof(script) - 1), output,
                   sizeof(output), &output_bytes) == WJS_OK);
    offset = 0;
    CHECK(next_record(output, output_bytes, &offset, &operation,
                      &id, &id_bytes, &text, &text_bytes));
    CHECK(operation == WJS_TLV_WRITE && id_bytes == 0 &&
          text_bytes == 5 && memcmp(text, "ready", 5) == 0);
    CHECK(next_record(output, output_bytes, &offset, &operation,
                      &id, &id_bytes, &text, &text_bytes));
    CHECK(operation == WJS_TLV_SET_TEXT && id_bytes == 5 &&
          memcmp(id, "title", 5) == 0 && text_bytes == 7 &&
          memcmp(text, "Base:10", 7) == 0);
    CHECK(next_record(output, output_bytes, &offset, &operation,
                      &id, &id_bytes, &text, &text_bytes));
    CHECK(operation == WJS_TLV_SET_HTML && id_bytes == 5 &&
          memcmp(id, "title", 5) == 0 && text_bytes == 11 &&
          memcmp(text, "<b>done</b>", 11) == 0);
    CHECK(next_record(output, output_bytes, &offset, &operation,
                      &id, &id_bytes, &text, &text_bytes));
    CHECK(operation == WJS_TLV_ALERT && id_bytes == 0 &&
          text_bytes == 2 && memcmp(text, "ok", 2) == 0);
    CHECK(offset == output_bytes);

    CHECK(wjs_eval(readback, (uint32_t)(sizeof(readback) - 1), output,
                   sizeof(output), &output_bytes) == WJS_OK);
    CHECK(output_bytes == 0);

    /* The supplied span stops before a throwing statement that follows it in
       memory. The production adapter must NUL-terminate its own source copy. */
    CHECK(wjs_eval(bounded_source,
                   (uint32_t)(sizeof("document.write('bounded');") - 1),
                   output, sizeof(output), &output_bytes) == WJS_OK);
    offset = 0;
    CHECK(next_record(output, output_bytes, &offset, &operation,
                      &id, &id_bytes, &text, &text_bytes));
    CHECK(operation == WJS_TLV_WRITE && text_bytes == 7 &&
          memcmp(text, "bounded", 7) == 0 && offset == output_bytes);

    CHECK(wjs_eval(partial_error, (uint32_t)(sizeof(partial_error) - 1),
                   output, sizeof(output), &output_bytes) == WJS_E_SCRIPT);
    CHECK(output_bytes > 0); /* caller must discard output on every error */
    CHECK(strstr(wjs_last_error(), "expected") != NULL);

    memset(guarded, 0xA5, sizeof(guarded));
    CHECK(wjs_eval(overflow_write, (uint32_t)(sizeof(overflow_write) - 1),
                   guarded, 8, &output_bytes) == WJS_E_LIMIT);
    CHECK(output_bytes == 0);
    CHECK(guarded[8] == 0xA5 && guarded[23] == 0xA5);

    wjs_set_cancel_flag(&cancel_word, 1);
    cancel_word = 2;
    CHECK(wjs_eval(infinite_loop, (uint32_t)(sizeof(infinite_loop) - 1),
                   output, sizeof(output), &output_bytes) == WJS_E_CANCELLED);
    wjs_set_cancel_flag(NULL, 0);
    CHECK(wjs_eval(infinite_loop, (uint32_t)(sizeof(infinite_loop) - 1),
                   output, sizeof(output), &output_bytes) == WJS_E_LIMIT);

    memset(too_large, 'x', sizeof(too_large));
    CHECK(wjs_set_element("large", 5, too_large, sizeof(too_large)) ==
          WJS_E_ARGUMENT);

    puts("[WEBWORKER_JS] PASS");
    return 0;
}
