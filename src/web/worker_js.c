/* Bounded JavaScript worker bindings for MicroQuickJS. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "worker_js.h"
#include "worker_js_config.h"
#include "mquickjs.h"

static JSValue wjs_print(JSContext *, JSValue *, int, JSValue *);
static JSValue js_date_constructor(JSContext *, JSValue *, int, JSValue *);
static JSValue js_date_now(JSContext *, JSValue *, int, JSValue *);
static JSValue js_performance_now(JSContext *, JSValue *, int, JSValue *);
static JSValue wjs_element_constructor(JSContext *, JSValue *, int, JSValue *);
static JSValue wjs_element_get_text(JSContext *, JSValue *, int, JSValue *);
static JSValue wjs_element_set_text(JSContext *, JSValue *, int, JSValue *);
static JSValue wjs_element_get_html(JSContext *, JSValue *, int, JSValue *);
static JSValue wjs_element_set_html(JSContext *, JSValue *, int, JSValue *);
static JSValue wjs_document_get_element_by_id(JSContext *, JSValue *, int, JSValue *);
static JSValue wjs_document_write(JSContext *, JSValue *, int, JSValue *);
static JSValue wjs_document_alert(JSContext *, JSValue *, int, JSValue *);

#include "worker_js_stdlib.h"

#define WJS_ARENA_BYTES (256UL * 1024UL)
#define WJS_STDLIB_ALIGN 512UL
#define WJS_MAX_SOURCE_BYTES (64UL * 1024UL)

/* DJGPP COFF only supports modest section alignment. mQuickJS needs its
   generated atom table aligned to ATOM_ALIGN for masked hash lookups, so make
   a runtime-aligned copy while preserving the table's relative offsets. */
static union {
    uint32_t align;
    uint8_t bytes[sizeof(js_stdlib_table) + WJS_STDLIB_ALIGN - 1];
} wjs_stdlib_storage;
static JSCFunctionDef wjs_cfunc_runtime[
    sizeof(js_c_function_table) / sizeof(js_c_function_table[0])];
static JSSTDLibraryDef wjs_stdlib_runtime;
static int wjs_stdlib_state;

static int wjs_rebase_rom_pointer(JSWord value, uintptr_t old_base,
                                  uintptr_t old_end, uintptr_t new_base,
                                  JSWord *relocated)
{
    uintptr_t pointer;
    if (!JS_IsPtr(value))
        return 0;
    pointer = (uintptr_t)value - 1;
    if (pointer < old_base || pointer >= old_end)
        return 0;
    *relocated = (JSWord)(new_base + pointer - old_base + 1);
    return 1;
}

#ifdef WJS_ROM_TEST
int wjs_test_rebase_rom(JSWord *words, uint32_t word_count,
                        uintptr_t old_base, uintptr_t old_end,
                        uintptr_t new_base, const uint32_t *slots,
                        uint32_t slot_count)
{
    uint32_t i;
    JSWord relocated;
    for (i = 0; i < slot_count; ++i) {
        if (slots[i] >= word_count ||
            !wjs_rebase_rom_pointer(words[slots[i]], old_base, old_end,
                                    new_base, &relocated))
            return 0;
        words[slots[i]] = relocated;
    }
    return 1;
}
#endif

static const JSSTDLibraryDef *wjs_get_stdlib(void)
{
    uintptr_t base, old_base, old_end;
    JSWord value, relocated;
    size_t i, words, slot;
    if (wjs_stdlib_state < 0)
        return NULL;
    if (!wjs_stdlib_state) {
        base = ((uintptr_t)wjs_stdlib_storage.bytes + WJS_STDLIB_ALIGN - 1) &
               ~(uintptr_t)(WJS_STDLIB_ALIGN - 1);
        memcpy((void *)base, js_stdlib_table, sizeof(js_stdlib_table));
        old_base = (uintptr_t)js_stdlib_table;
        old_end = old_base + sizeof(js_stdlib_table);
        words = sizeof(js_stdlib_table) / sizeof(JSWord);
        for (i = 0; i < js_stdlib_reloc_count; ++i) {
            slot = js_stdlib_reloc_slots[i];
            if (slot >= words) {
                wjs_stdlib_state = -1;
                return NULL;
            }
            value = ((JSWord *)base)[slot];
            if (!wjs_rebase_rom_pointer(value, old_base, old_end, base,
                                        &relocated)) {
                wjs_stdlib_state = -1;
                return NULL;
            }
            ((JSWord *)base)[slot] = relocated;
        }
        memcpy(wjs_cfunc_runtime, js_c_function_table,
               sizeof(wjs_cfunc_runtime));
        for (i = 0; i < js_c_function_table_count; ++i) {
            value = wjs_cfunc_runtime[i].name;
            /* Single-character atom names are immediate JS values, not ROM
               pointers; preserve those and rebase only pointer-tagged names. */
            if (JS_IsPtr(value)) {
                if (!wjs_rebase_rom_pointer(value, old_base, old_end, base,
                                            &relocated)) {
                    wjs_stdlib_state = -1;
                    return NULL;
                }
                wjs_cfunc_runtime[i].name = (JSValue)relocated;
            }
        }
        wjs_stdlib_runtime = js_stdlib;
        wjs_stdlib_runtime.stdlib_table = (const JSWord *)base;
        wjs_stdlib_runtime.c_function_table = wjs_cfunc_runtime;
        wjs_stdlib_state = 1;
    }
    return &wjs_stdlib_runtime;
}

static union {
    uint32_t align;
    uint8_t bytes[WJS_ARENA_BYTES];
} wjs_arena;
/* mQuickJS requires input[input_len] to be NUL. Mailbox/file spans are not
   guaranteed to have a terminator at that boundary, so give it an owned copy. */
static char wjs_source[WJS_MAX_SOURCE_BYTES + 1];
static JSContext *wjs_ctx;
static uint8_t *wjs_output;
static uint32_t wjs_output_capacity;
static uint32_t wjs_output_length;
static uint32_t wjs_interrupts_left;
static uint32_t wjs_output_failed;
static char wjs_error_text[128];
static volatile const uint32_t *wjs_cancel_flag;
static uint32_t wjs_cancel_expected;
struct wjs_element_snapshot {
    uint16_t id_bytes;
    uint16_t text_bytes;
    uint8_t used;
    char id[WJS_MAX_ID_BYTES];
    char text[WJS_ELEMENT_TEXT_BYTES];
};
static struct wjs_element_snapshot wjs_elements[WJS_ELEMENT_SNAPSHOT_COUNT];
static struct wjs_element_snapshot wjs_elements_saved[WJS_ELEMENT_SNAPSHOT_COUNT];

static int64_t wjs_now_ms(void)
{
    time_t now = time(NULL);
    if (now < (time_t)0)
        return 0;
    return (int64_t)now * 1000;
}

static JSValue wjs_print(JSContext *ctx, JSValue *this_val,
                         int argc, JSValue *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

static JSValue js_date_constructor(JSContext *ctx, JSValue *this_val,
                                   int argc, JSValue *argv)
{
    double value;
    (void)this_val;
    argc &= ~FRAME_CF_CTOR;
    if (argc == 0)
        value = (double)wjs_now_ms();
    else if (argc == 1) {
        if (JS_ToNumber(ctx, &value, argv[0]))
            return JS_EXCEPTION;
    } else {
        return JS_ThrowTypeError(ctx, "unsupported Date argument");
    }
    return JS_NewDate(ctx, value);
}

static JSValue js_date_now(JSContext *ctx, JSValue *this_val,
                           int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt64(ctx, wjs_now_ms());
}

static JSValue js_performance_now(JSContext *ctx, JSValue *this_val,
                                  int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    /* DJGPP's wall clock is coarse; expose milliseconds without claiming
       sub-millisecond monotonic precision. */
    return JS_NewInt64(ctx, wjs_now_ms());
}

static int wjs_find_element(const char *id, uint32_t id_bytes, int create)
{
    uint32_t i, free_slot = WJS_ELEMENT_SNAPSHOT_COUNT;
    for (i = 0; i < WJS_ELEMENT_SNAPSHOT_COUNT; ++i) {
        if (wjs_elements[i].used) {
            if (wjs_elements[i].id_bytes == id_bytes &&
                memcmp(wjs_elements[i].id, id, id_bytes) == 0)
                return (int)i;
        } else if (free_slot == WJS_ELEMENT_SNAPSHOT_COUNT) {
            free_slot = i;
        }
    }
    if (!create || free_slot == WJS_ELEMENT_SNAPSHOT_COUNT)
        return -1;
    wjs_elements[free_slot].used = 1;
    wjs_elements[free_slot].id_bytes = (uint16_t)id_bytes;
    memcpy(wjs_elements[free_slot].id, id, id_bytes);
    return (int)free_slot;
}

static int wjs_interrupted(JSContext *ctx, void *opaque)
{
    (void)ctx;
    (void)opaque;
    if (wjs_cancel_flag && *wjs_cancel_flag != wjs_cancel_expected)
        return 1;
    if (wjs_interrupts_left == 0)
        return 1;
    --wjs_interrupts_left;
    return 0;
}

static int wjs_emit(uint16_t operation, const char *id, uint32_t id_bytes,
                    const char *text, uint32_t text_bytes)
{
    uint32_t need;
    uint8_t *p;
    if (id_bytes > WJS_MAX_ID_BYTES || text_bytes > WJS_MAX_TEXT_BYTES ||
        id_bytes > 0xffffUL || text_bytes > 0xffffUL) {
        wjs_output_failed = 1;
        return -1;
    }
    need = WJS_TLV_HEADER_BYTES + id_bytes + text_bytes;
    if (need > wjs_output_capacity - wjs_output_length) {
        wjs_output_failed = 1;
        return -1;
    }
    p = wjs_output + wjs_output_length;
    p[0] = (uint8_t)operation;
    p[1] = (uint8_t)(operation >> 8);
    p[2] = (uint8_t)id_bytes;
    p[3] = (uint8_t)(id_bytes >> 8);
    p[4] = (uint8_t)text_bytes;
    p[5] = (uint8_t)(text_bytes >> 8);
    if (id_bytes)
        memcpy(p + WJS_TLV_HEADER_BYTES, id, id_bytes);
    if (text_bytes)
        memcpy(p + WJS_TLV_HEADER_BYTES + id_bytes, text, text_bytes);
    wjs_output_length += need;
    return 0;
}

static int wjs_value_text(JSContext *ctx, JSValue value,
                          const char **text, size_t *length,
                          JSCStringBuf *buffer)
{
    const char *p = JS_ToCStringLen(ctx, length, value, buffer);
    if (!p)
        return -1;
    if (*length > WJS_MAX_TEXT_BYTES)
        return -1;
    *text = p;
    return 0;
}

static int wjs_element_id(JSContext *ctx, JSValue object,
                          const char **id, size_t *length,
                          JSCStringBuf *buffer)
{
    JSValue value = JS_GetPropertyStr(ctx, object, "id");
    if (JS_IsException(value))
        return -1;
    return wjs_value_text(ctx, value, id, length, buffer);
}

static JSValue wjs_element_constructor(JSContext *ctx, JSValue *this_val,
                                       int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "Element cannot be constructed");
}

static JSValue wjs_document_get_element_by_id(JSContext *ctx,
                                              JSValue *this_val,
                                              int argc, JSValue *argv)
{
    JSValue result;
    JSGCRef element_ref, id_ref;
    JSValue *element_root, *id_root;
    const char *id_text;
    size_t id_bytes;
    char id_copy[WJS_MAX_ID_BYTES];
    int slot;
    JSCStringBuf buffer;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getElementById requires an id");
    if (wjs_value_text(ctx, argv[0], &id_text, &id_bytes, &buffer) < 0 ||
        id_bytes == 0 || id_bytes > WJS_MAX_ID_BYTES)
        return JS_ThrowRangeError(ctx, "element id exceeds worker limit");
    memcpy(id_copy, id_text, id_bytes);
    slot = wjs_find_element(id_copy, (uint32_t)id_bytes, 0);
    if (slot < 0)
        return JS_NULL;
    element_root = JS_PushGCRef(ctx, &element_ref);
    *element_root = JS_NewObjectClassUser(ctx, JS_CLASS_CIUK_ELEMENT, 0);
    if (JS_IsException(*element_root)) {
        result = JS_PopGCRef(ctx, &element_ref);
        return result;
    }
    id_root = JS_PushGCRef(ctx, &id_ref);
    *id_root = JS_NewStringLen(ctx, id_copy, id_bytes);
    if (JS_IsException(*id_root)) {
        (void)JS_PopGCRef(ctx, &id_ref);
        result = JS_PopGCRef(ctx, &element_ref);
        return result;
    }
    if (JS_IsException(JS_SetPropertyStr(ctx, *element_root, "id", *id_root))) {
        (void)JS_PopGCRef(ctx, &id_ref);
        (void)JS_PopGCRef(ctx, &element_ref);
        return JS_EXCEPTION;
    }
    (void)JS_PopGCRef(ctx, &id_ref);
    return JS_PopGCRef(ctx, &element_ref);
}

static JSValue wjs_document_write(JSContext *ctx, JSValue *this_val,
                                  int argc, JSValue *argv)
{
    const char *text;
    size_t text_bytes;
    JSCStringBuf buffer;
    (void)this_val;
    if (argc < 1)
        return JS_UNDEFINED;
    if (wjs_value_text(ctx, argv[0], &text, &text_bytes, &buffer) < 0)
        return JS_ThrowRangeError(ctx, "document.write exceeds worker limit");
    if (wjs_emit(WJS_TLV_WRITE, NULL, 0, text, (uint32_t)text_bytes) < 0)
        return JS_ThrowRangeError(ctx, "DOM operation output is full");
    return JS_UNDEFINED;
}

static JSValue wjs_document_alert(JSContext *ctx, JSValue *this_val,
                                  int argc, JSValue *argv)
{
    const char *text;
    size_t text_bytes;
    JSCStringBuf buffer;
    (void)this_val;
    if (argc < 1)
        return JS_UNDEFINED;
    if (wjs_value_text(ctx, argv[0], &text, &text_bytes, &buffer) < 0)
        return JS_ThrowRangeError(ctx, "alert text exceeds worker limit");
    if (wjs_emit(WJS_TLV_ALERT, NULL, 0, text, (uint32_t)text_bytes) < 0)
        return JS_ThrowRangeError(ctx, "DOM operation output is full");
    return JS_UNDEFINED;
}

static JSValue wjs_element_get_text(JSContext *ctx, JSValue *this_val,
                                    int argc, JSValue *argv)
{
    const char *id;
    size_t id_bytes;
    JSCStringBuf id_buffer;
    int slot;
    (void)argc;
    (void)argv;
    if (wjs_element_id(ctx, *this_val, &id, &id_bytes, &id_buffer) < 0)
        return JS_ThrowTypeError(ctx, "invalid DOM element proxy");
    slot = wjs_find_element(id, (uint32_t)id_bytes, 0);
    if (slot < 0)
        return JS_NewStringLen(ctx, "", 0);
    return JS_NewStringLen(ctx, wjs_elements[slot].text,
                           wjs_elements[slot].text_bytes);
}

static JSValue wjs_element_set_text(JSContext *ctx, JSValue *this_val,
                                    int argc, JSValue *argv)
{
    const char *id, *text;
    size_t id_bytes, text_bytes;
    char id_copy[WJS_MAX_ID_BYTES];
    JSCStringBuf id_buffer, text_buffer;
    int slot;
    if (argc < 1)
        return JS_UNDEFINED;
    if (wjs_element_id(ctx, *this_val, &id, &id_bytes, &id_buffer) < 0 ||
        id_bytes > WJS_MAX_ID_BYTES)
        return JS_ThrowRangeError(ctx, "DOM element id exceeds worker limit");
    memcpy(id_copy, id, id_bytes);
    slot = wjs_find_element(id_copy, (uint32_t)id_bytes, 0);
    if (slot < 0)
        return JS_ThrowTypeError(ctx, "DOM element no longer exists");
    if (wjs_value_text(ctx, argv[0], &text, &text_bytes, &text_buffer) < 0)
        return JS_ThrowRangeError(ctx, "DOM text exceeds worker limit");
    if (text_bytes > WJS_ELEMENT_TEXT_BYTES)
        return JS_ThrowRangeError(ctx, "element text exceeds worker snapshot limit");
    if (wjs_emit(WJS_TLV_SET_TEXT, id_copy, (uint32_t)id_bytes,
                 text, (uint32_t)text_bytes) < 0)
        return JS_ThrowRangeError(ctx, "DOM operation output is full");
    memcpy(wjs_elements[slot].text, text, text_bytes);
    wjs_elements[slot].text_bytes = (uint16_t)text_bytes;
    return JS_UNDEFINED;
}

static JSValue wjs_element_get_html(JSContext *ctx, JSValue *this_val,
                                    int argc, JSValue *argv)
{
    return wjs_element_get_text(ctx, this_val, argc, argv);
}

static JSValue wjs_element_set_html(JSContext *ctx, JSValue *this_val,
                                    int argc, JSValue *argv)
{
    const char *id, *text;
    size_t id_bytes, text_bytes;
    char id_copy[WJS_MAX_ID_BYTES];
    JSCStringBuf id_buffer, text_buffer;
    int slot;
    if (argc < 1)
        return JS_UNDEFINED;
    if (wjs_element_id(ctx, *this_val, &id, &id_bytes, &id_buffer) < 0 ||
        id_bytes > WJS_MAX_ID_BYTES)
        return JS_ThrowRangeError(ctx, "DOM element id exceeds worker limit");
    memcpy(id_copy, id, id_bytes);
    slot = wjs_find_element(id_copy, (uint32_t)id_bytes, 0);
    if (slot < 0)
        return JS_ThrowTypeError(ctx, "DOM element no longer exists");
    if (wjs_value_text(ctx, argv[0], &text, &text_bytes, &text_buffer) < 0)
        return JS_ThrowRangeError(ctx, "DOM markup exceeds worker limit");
    if (text_bytes > WJS_ELEMENT_TEXT_BYTES)
        return JS_ThrowRangeError(ctx, "element markup exceeds worker snapshot limit");
    if (wjs_emit(WJS_TLV_SET_HTML, id_copy, (uint32_t)id_bytes,
                 text, (uint32_t)text_bytes) < 0)
        return JS_ThrowRangeError(ctx, "DOM operation output is full");
    memcpy(wjs_elements[slot].text, text, text_bytes);
    wjs_elements[slot].text_bytes = (uint16_t)text_bytes;
    return JS_UNDEFINED;
}

void wjs_reset(void)
{
    const JSSTDLibraryDef *stdlib;
    if (wjs_ctx)
        JS_FreeContext(wjs_ctx);
    memset(wjs_elements, 0, sizeof(wjs_elements));
    memset(wjs_arena.bytes, 0, sizeof(wjs_arena.bytes));
    stdlib = wjs_get_stdlib();
    wjs_ctx = stdlib ? JS_NewContext(wjs_arena.bytes, sizeof(wjs_arena.bytes),
                                    stdlib) : NULL;
    if (wjs_ctx)
        JS_SetInterruptHandler(wjs_ctx, wjs_interrupted);
}

int wjs_set_element(const char *id, uint32_t id_bytes,
                    const char *text, uint32_t text_bytes)
{
    int slot;
    if (!id || !text || id_bytes == 0 || id_bytes > WJS_MAX_ID_BYTES ||
        text_bytes > WJS_ELEMENT_TEXT_BYTES)
        return WJS_E_ARGUMENT;
    slot = wjs_find_element(id, id_bytes, 1);
    if (slot < 0)
        return WJS_E_LIMIT;
    if (text_bytes)
        memcpy(wjs_elements[slot].text, text, text_bytes);
    wjs_elements[slot].text_bytes = (uint16_t)text_bytes;
    return WJS_OK;
}

void wjs_set_cancel_flag(volatile const uint32_t *flag, uint32_t expected)
{
    wjs_cancel_flag = flag;
    wjs_cancel_expected = expected;
}

const char *wjs_last_error(void)
{
    return wjs_error_text;
}

static char wjs_type_code(JSValue value)
{
    if (JS_IsException(value)) return 'E';
    if (JS_IsUndefined(value)) return 'U';
    if (JS_IsNull(value)) return 'N';
    if (JS_IsFunction(wjs_ctx, value)) return 'F';
    if (JS_IsPtr(value)) return 'O';
    return 'V';
}

static JSValue wjs_probe_property(JSValue object, const char *name)
{
    JSValue value = JS_GetPropertyStr(wjs_ctx, object, name);
    if (JS_IsException(value)) {
        (void)JS_GetException(wjs_ctx);
        return JS_UNDEFINED;
    }
    return value;
}

static void wjs_append_dom_types(void)
{
    JSValue global, document, get_by_id = JS_UNDEFINED, write = JS_UNDEFINED;
    char document_type, get_type = '-', write_type = '-';
    size_t at;
    static const char prefix[] = "not a function [document=";
    static const char middle[] = " getElementById=";
    static const char suffix[] = " write=";
    if (!wjs_ctx || strcmp(wjs_error_text, "not a function") != 0)
        return;
    global = JS_GetGlobalObject(wjs_ctx);
    if (!JS_IsPtr(global))
        return;
    document = wjs_probe_property(global, "document");
    document_type = wjs_type_code(document);
    if (JS_IsPtr(document)) {
        get_by_id = wjs_probe_property(document, "getElementById");
        write = wjs_probe_property(document, "write");
        get_type = wjs_type_code(get_by_id);
        write_type = wjs_type_code(write);
    }
    at = 0;
#define WJS_APPEND_LITERAL(s) do { \
        size_t wjs_n = sizeof(s) - 1; \
        if (at + wjs_n < sizeof(wjs_error_text)) { \
            memcpy(wjs_error_text + at, s, wjs_n); at += wjs_n; \
        } \
    } while (0)
    WJS_APPEND_LITERAL(prefix);
    if (at + 1 < sizeof(wjs_error_text)) wjs_error_text[at++] = document_type;
    WJS_APPEND_LITERAL(middle);
    if (at + 1 < sizeof(wjs_error_text)) wjs_error_text[at++] = get_type;
    WJS_APPEND_LITERAL(suffix);
    if (at + 1 < sizeof(wjs_error_text)) wjs_error_text[at++] = write_type;
    if (at + 1 < sizeof(wjs_error_text)) {
        wjs_error_text[at++] = ']';
        wjs_error_text[at] = 0;
    }
#undef WJS_APPEND_LITERAL
}

static void wjs_capture_error(JSValue exception)
{
    JSValue value;
    const char *text;
    size_t bytes = 0, copy_bytes;
    JSCStringBuf buffer;
    wjs_error_text[0] = 0;
    if (!wjs_ctx)
        return;
    if (JS_IsException(exception))
        exception = JS_GetException(wjs_ctx);
    if (JS_IsException(exception))
        return;
    value = JS_IsError(wjs_ctx, exception) ?
            JS_GetPropertyStr(wjs_ctx, exception, "message") : exception;
    if (JS_IsException(value)) {
        (void)JS_GetException(wjs_ctx);
        value = exception;
    }
    text = JS_ToCStringLen(wjs_ctx, &bytes, value, &buffer);
    if (!text || !bytes) {
        memcpy(wjs_error_text, "JavaScript exception", 21);
        return;
    }
    copy_bytes = bytes < sizeof(wjs_error_text) - 1 ?
                 bytes : sizeof(wjs_error_text) - 1;
    memcpy(wjs_error_text, text, copy_bytes);
    wjs_error_text[copy_bytes] = 0;
    wjs_append_dom_types();
}

int wjs_eval(const char *source, uint32_t source_bytes,
             uint8_t *output, uint32_t output_capacity,
             uint32_t *output_bytes)
{
    JSValue result;
    const JSSTDLibraryDef *stdlib;
    if (output_bytes)
        *output_bytes = 0;
    if (!source || !output || !output_bytes || output_capacity < WJS_TLV_HEADER_BYTES)
        return WJS_E_ARGUMENT;
    if (source_bytes > WJS_MAX_SOURCE_BYTES)
        return WJS_E_LIMIT;
    if (!wjs_ctx) {
        stdlib = wjs_get_stdlib();
        if (!stdlib)
            return WJS_E_MEMORY;
        memset(wjs_arena.bytes, 0, sizeof(wjs_arena.bytes));
        wjs_ctx = JS_NewContext(wjs_arena.bytes, sizeof(wjs_arena.bytes),
                                stdlib);
        if (wjs_ctx)
            JS_SetInterruptHandler(wjs_ctx, wjs_interrupted);
    }
    if (!wjs_ctx)
        return WJS_E_MEMORY;
    if (source_bytes)
        memcpy(wjs_source, source, source_bytes);
    wjs_source[source_bytes] = 0;
    wjs_output = output;
    wjs_output_capacity = output_capacity;
    wjs_output_length = 0;
    wjs_output_failed = 0;
    wjs_error_text[0] = 0;
    wjs_interrupts_left = WJS_INTERRUPT_BUDGET;
    memcpy(wjs_elements_saved, wjs_elements, sizeof(wjs_elements));
    result = JS_Eval(wjs_ctx, wjs_source, source_bytes, "web-script.js",
                     JS_EVAL_STRIP_COL);
    wjs_output = NULL;
    wjs_output_capacity = 0;
    *output_bytes = wjs_output_length;
    if (wjs_output_failed) {
        memcpy(wjs_elements, wjs_elements_saved, sizeof(wjs_elements));
        return WJS_E_LIMIT;
    }
    if (wjs_cancel_flag && *wjs_cancel_flag != wjs_cancel_expected) {
        memcpy(wjs_elements, wjs_elements_saved, sizeof(wjs_elements));
        return WJS_E_CANCELLED;
    }
    if (JS_IsException(result)) {
        wjs_capture_error(JS_GetException(wjs_ctx));
        memcpy(wjs_elements, wjs_elements_saved, sizeof(wjs_elements));
        return (wjs_interrupts_left == 0) ? WJS_E_LIMIT : WJS_E_SCRIPT;
    }
    return WJS_OK;
}
