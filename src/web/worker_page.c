/* Small bounded HTML/script bridge for the isolated 32-bit web worker.
   This intentionally implements a documented DOM subset, not an HTML5 DOM. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "worker_page.h"
#include "worker_js.h"

#define WPAGE_MAX_BYTES (1024UL * 1024UL)
#define WPAGE_SCRIPT_MAX 16
#define WPAGE_INLINE_MAX (64UL * 1024UL)
#define WPAGE_SCRIPT_TOTAL (256UL * 1024UL)
#define WPAGE_ID_MAX 255
#define WPAGE_TEXT_MAX 1024
#define WPAGE_EXTERNAL_MAX 128
#define WPAGE_TLV_BYTES (24UL * 1024UL)

struct page_script {
    uint32_t source_offset, source_bytes;
    uint16_t external_index, tag_ordinal;
    uint8_t external;
};
struct page_element {
    char id[WPAGE_ID_MAX + 1];
    char tag[16];
    uint16_t id_bytes;
    uint32_t content_start, content_end;
};
struct page_resource_entry {
    char path[128];
    uint8_t set;
};

static char *page_bytes;
static uint32_t page_length, page_capacity;
static char *script_bytes;
static uint32_t script_length;
static struct page_script scripts[WPAGE_SCRIPT_MAX];
static uint32_t script_count, external_count;
static uint32_t document_write_cursor;
static struct page_resource_entry resources[WPAGE_SCRIPT_MAX];
static struct page_element elements[WJS_ELEMENT_SNAPSHOT_COUNT];
static uint32_t element_count;
static uint8_t tlv[WPAGE_TLV_BYTES];

static int web_path(const char *path)
{
#ifdef WPAGE_HOST_TEST
    return path && path[0] != 0;
#else
    return path && strncmp(path, "C:\\NET\\", 7) == 0;
#endif
}

static int ascii_equal(char a, char b)
{
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static int starts_ci(const char *s, uint32_t n, uint32_t at,
                     const char *word)
{
    uint32_t i;
    for (i = 0; word[i]; ++i) {
        if (at + i >= n || !ascii_equal(s[at + i], word[i]))
            return 0;
    }
    return 1;
}

static uint32_t find_ci(const char *s, uint32_t n, uint32_t from,
                        const char *word)
{
    uint32_t i;
    for (i = from; i < n; ++i)
        if (starts_ci(s, n, i, word))
            return i;
    return n;
}

static int read_file(const char *path, char **out, uint32_t *out_bytes,
                     uint32_t limit)
{
    FILE *f;
    long end;
    char *p;
    f = fopen(path, "rb");
    if (!f)
        return WPAGE_E_IO;
    if (fseek(f, 0, SEEK_END) || (end = ftell(f)) < 0 ||
        (unsigned long)end > limit || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return WPAGE_E_LIMIT;
    }
    p = (char *)malloc((size_t)end + 1);
    if (!p) {
        fclose(f);
        return WPAGE_E_LIMIT;
    }
    if (fread(p, 1, (size_t)end, f) != (size_t)end) {
        fclose(f);
        free(p);
        return WPAGE_E_IO;
    }
    fclose(f);
    p[end] = 0;
    *out = p;
    *out_bytes = (uint32_t)end;
    return WPAGE_OK;
}

static int read_file_into(const char *path, char *buffer, uint32_t capacity,
                          uint32_t *out_bytes)
{
    FILE *f;
    long end;
    f = fopen(path, "rb");
    if (!f) return WPAGE_E_IO;
    if (fseek(f, 0, SEEK_END) || (end = ftell(f)) < 0 ||
        (unsigned long)end > capacity || fseek(f, 0, SEEK_SET)) {
        fclose(f); return WPAGE_E_LIMIT;
    }
    if (fread(buffer, 1, (size_t)end, f) != (size_t)end) {
        fclose(f); return WPAGE_E_IO;
    }
    fclose(f);
    *out_bytes = (uint32_t)end;
    buffer[*out_bytes] = 0;
    return WPAGE_OK;
}

static int append_inline(uint32_t start, uint32_t bytes,
                         struct page_script *record)
{
    if (bytes > WPAGE_INLINE_MAX || script_length > WPAGE_SCRIPT_TOTAL - bytes)
        return WPAGE_E_LIMIT;
    record->source_offset = script_length;
    record->source_bytes = bytes;
    record->external = 0;
    record->external_index = 0xffffU;
    if (bytes)
        memcpy(script_bytes + script_length, page_bytes + start, bytes);
    script_length += bytes;
    return WPAGE_OK;
}

static int get_attr(const char *s, uint32_t n, uint32_t start,
                    uint32_t end, const char *name, char *out,
                    uint32_t cap, uint32_t *out_bytes)
{
    uint32_t p = start, k, value_start, value_end;
    *out_bytes = 0;
    while (p < end) {
        while (p < end && (isspace((unsigned char)s[p]) || s[p] == '/')) ++p;
        if (p >= end || s[p] == '>') break;
        k = p;
        while (p < end && (isalnum((unsigned char)s[p]) || s[p] == '-' || s[p] == '_' || s[p] == ':')) ++p;
        if (p == k) { ++p; continue; }
        value_start = value_end = p;
        while (p < end && isspace((unsigned char)s[p])) ++p;
        if (p < end && s[p] == '=') {
            ++p;
            while (p < end && isspace((unsigned char)s[p])) ++p;
            if (p < end && (s[p] == '\'' || s[p] == '"')) {
                char quote = s[p++];
                value_start = p;
                while (p < end && s[p] != quote) ++p;
                value_end = p;
                if (p < end) ++p;
            } else {
                value_start = p;
                while (p < end && !isspace((unsigned char)s[p]) && s[p] != '>') ++p;
                value_end = p;
            }
        }
        /* Attribute name length is retained separately because p now points
           after its value. */
        {
            uint32_t name_len = 0, q;
            while (name[name_len]) ++name_len;
            q = k;
            while (q < end && (isalnum((unsigned char)s[q]) || s[q] == '-' || s[q] == '_' || s[q] == ':')) ++q;
            if (q - k == name_len && starts_ci(s, n, k, name)) {
                uint32_t vlen = value_end - value_start;
                if (vlen >= cap) return -1;
                memcpy(out, s + value_start, vlen);
                out[vlen] = 0;
                *out_bytes = vlen;
                return 1;
            }
        }
    }
    return 0;
}

static int parse_tag(const char *s, uint32_t n, uint32_t open,
                    uint32_t *tag_end, uint32_t *name_start,
                    uint32_t *name_bytes, int *closing, int *self_closing)
{
    uint32_t p = open + 1;
    char quote = 0;
    if (open >= n || s[open] != '<') return 0;
    *closing = 0;
    if (p < n && s[p] == '/') { *closing = 1; ++p; }
    while (p < n && isspace((unsigned char)s[p])) ++p;
    *name_start = p;
    while (p < n && (isalnum((unsigned char)s[p]) || s[p] == '-')) ++p;
    *name_bytes = p - *name_start;
    if (*name_bytes == 0) return 0;
    *tag_end = p;
    while (*tag_end < n) {
        char c = s[(*tag_end)++];
        if (quote) { if (c == quote) quote = 0; }
        else if (c == '\'' || c == '"') quote = c;
        else if (c == '>') break;
    }
    if (*tag_end == 0 || s[*tag_end - 1] != '>') return 0;
    p = *tag_end - 1;
    while (p > open && isspace((unsigned char)s[p - 1])) --p;
    *self_closing = p > open && s[p - 1] == '/';
    return 1;
}

static int raw_text_tag(const char *name, uint32_t bytes);
static uint32_t find_raw_text_end(const char *s, uint32_t n, uint32_t from,
                                  const char *name, uint32_t name_bytes);

static uint32_t matching_close(const char *s, uint32_t n, uint32_t from,
                               const char *name, uint32_t name_bytes)
{
    uint32_t p = from, end, ns, nb, depth = 1;
    int closing, self_closing;
    while (p < n) {
        uint32_t open = find_ci(s, n, p, "<");
        if (open >= n) break;
        if (starts_ci(s, n, open, "<!--")) {
            uint32_t comment_end = find_ci(s, n, open + 4, "-->");
            p = comment_end < n ? comment_end + 3 : n;
            continue;
        }
        if (!parse_tag(s, n, open, &end, &ns, &nb, &closing, &self_closing)) {
            p = open + 1;
            continue;
        }
        if (!closing && !self_closing && raw_text_tag(s + ns, nb)) {
            p = find_raw_text_end(s, n, end, s + ns, nb);
            continue;
        }
        if (nb == name_bytes && starts_ci(s, n, ns, name)) {
            if (closing) {
                if (--depth == 0) return open;
            } else if (!self_closing) {
                ++depth;
            }
        }
        p = end;
    }
    return n;
}

static int supported_script_type(const char *s, uint32_t n,
                                uint32_t start, uint32_t end)
{
    char type[40];
    uint32_t bytes;
    int found = get_attr(s, n, start, end, "type", type, sizeof(type), &bytes);
    if (found <= 0 || bytes == 0) return found >= 0;
    return (bytes == strlen("text/javascript") && starts_ci(type, bytes, 0, "text/javascript")) ||
           (bytes == strlen("application/javascript") && starts_ci(type, bytes, 0, "application/javascript")) ||
           (bytes == strlen("application/ecmascript") && starts_ci(type, bytes, 0, "application/ecmascript")) ||
           (bytes == strlen("text/ecmascript") && starts_ci(type, bytes, 0, "text/ecmascript"));
}

static int raw_text_tag(const char *name, uint32_t bytes)
{
    return (bytes == 5 && starts_ci(name, bytes, 0, "style")) ||
           (bytes == 8 && starts_ci(name, bytes, 0, "textarea")) ||
           (bytes == 5 && starts_ci(name, bytes, 0, "title")) ||
           (bytes == 3 && starts_ci(name, bytes, 0, "xmp")) ||
           (bytes == 6 && starts_ci(name, bytes, 0, "iframe")) ||
           (bytes == 7 && starts_ci(name, bytes, 0, "noembed")) ||
           (bytes == 8 && starts_ci(name, bytes, 0, "noframes"));
}

static uint32_t find_raw_text_end(const char *s, uint32_t n, uint32_t from,
                                  const char *name, uint32_t name_bytes)
{
    uint32_t p = from, close, after_name, end, i;
    while ((close = find_ci(s, n, p, "</")) < n) {
        after_name = close + 2;
        for (i = 0; i < name_bytes && after_name + i < n; ++i)
            if (!ascii_equal(s[after_name + i], name[i])) break;
        if (i == name_bytes &&
            (after_name + name_bytes == n ||
             isspace((unsigned char)s[after_name + name_bytes]) ||
             s[after_name + name_bytes] == '>')) {
            end = find_ci(s, n, after_name + name_bytes, ">");
            return end < n ? end + 1 : n;
        }
        p = close + 2;
    }
    return n;
}

/* Build a bounded ordered script list and a small ID/text snapshot. */
static int parse_page(int do_scripts, int do_elements)
{
    uint32_t p = 0, tag_end, name_start, name_end, close, attr_len, text_len;
    uint32_t all_scripts = 0;
    char name[16], attr[WPAGE_EXTERNAL_MAX + 1], id[WPAGE_ID_MAX + 1];
    int rc, tag_closing, tag_self_closing;
    if (do_scripts) {
        script_count = external_count = script_length = 0;
        memset(scripts, 0, sizeof(scripts));
    }
    if (do_elements) {
        element_count = 0;
        memset(elements, 0, sizeof(elements));
    }
    while (p < page_length) {
        if (page_bytes[p] != '<') { ++p; continue; }
        if (starts_ci(page_bytes, page_length, p, "<!--")) {
            close = find_ci(page_bytes, page_length, p + 4, "-->");
            p = close < page_length ? close + 3 : page_length;
            continue;
        }
        tag_end = p + 1;
        {
            char quote = 0;
            while (tag_end < page_length) {
                char c = page_bytes[tag_end++];
                if (quote) { if (c == quote) quote = 0; }
                else if (c == '\'' || c == '"') quote = c;
                else if (c == '>') break;
            }
        }
        if (tag_end >= page_length || page_bytes[tag_end - 1] != '>') break;
        name_start = p + 1;
        tag_closing = name_start < tag_end && page_bytes[name_start] == '/';
        while (name_start < tag_end && (isspace((unsigned char)page_bytes[name_start]) || page_bytes[name_start] == '/')) ++name_start;
        name_end = name_start;
        while (name_end < tag_end && (isalnum((unsigned char)page_bytes[name_end]) || page_bytes[name_end] == '-')) ++name_end;
        if (name_end == name_start || name_end - name_start >= sizeof(name)) { p = tag_end; continue; }
        memcpy(name, page_bytes + name_start, name_end - name_start);
        name[name_end - name_start] = 0;
        tag_self_closing = 0;
        {
            uint32_t q = tag_end - 1;
            while (q > p && isspace((unsigned char)page_bytes[q - 1])) --q;
            tag_self_closing = q > p && page_bytes[q - 1] == '/';
        }
        if (!tag_closing && starts_ci(page_bytes, page_length, name_start, "script") &&
            (name_end == name_start + 6 || isspace((unsigned char)page_bytes[name_end]) || page_bytes[name_end] == '>')) {
            uint32_t content = tag_end;
            uint32_t script_ordinal;
            close = find_ci(page_bytes, page_length, content, "</script");
            if (close >= page_length) { p = tag_end; continue; }
            if (!do_scripts) {
                p = find_ci(page_bytes, page_length, close, ">") + 1;
                continue;
            }
            if (!supported_script_type(page_bytes, page_length, name_end,
                                       tag_end - 1)) {
                p = find_ci(page_bytes, page_length, close, ">") + 1;
                continue;
            }
            script_ordinal = all_scripts++;
            if (script_count >= WPAGE_SCRIPT_MAX) return WPAGE_E_LIMIT;
            rc = get_attr(page_bytes, page_length, name_end, tag_end - 1,
                          "src", attr, sizeof(attr), &attr_len);
            if (rc < 0) return WPAGE_E_LIMIT;
            if (rc > 0 && attr_len) {
                if (attr_len > WPAGE_EXTERNAL_MAX) return WPAGE_E_LIMIT;
                scripts[script_count].external = 1;
                scripts[script_count].tag_ordinal = (uint16_t)script_ordinal;
                scripts[script_count].external_index = (uint16_t)external_count;
                scripts[script_count].source_bytes = attr_len;
                scripts[script_count].source_offset = 0;
                ++external_count;
                /* Store URL in the record's otherwise unused inline span. */
                if (script_length > WPAGE_SCRIPT_TOTAL - attr_len) return WPAGE_E_LIMIT;
                memcpy(script_bytes + script_length, attr, attr_len);
                scripts[script_count].source_offset = script_length;
                script_length += attr_len;
            } else {
                rc = append_inline(content, close - content, &scripts[script_count]);
                if (rc) return rc;
                scripts[script_count].tag_ordinal = (uint16_t)script_ordinal;
            }
            ++script_count;
            p = find_ci(page_bytes, page_length, close, ">") + 1;
            continue;
        }
        /* These HTML elements contain text, not nested markup. Skipping them
           prevents CSS/title text that resembles <script> or id tags from
           being executed or exposed through the worker DOM snapshot. */
        if (!tag_closing && !tag_self_closing &&
            raw_text_tag(name, name_end - name_start)) {
            p = find_raw_text_end(page_bytes, page_length, tag_end, name,
                                  name_end - name_start);
            continue;
        }
        /* Seed only simple, non-self-closing identified elements. */
        if (do_elements && !tag_closing && !tag_self_closing &&
            element_count < WJS_ELEMENT_SNAPSHOT_COUNT) {
            rc = get_attr(page_bytes, page_length, name_end, tag_end - 1,
                          "id", id, sizeof(id), &text_len);
            if (rc < 0) return WPAGE_E_LIMIT;
            if (rc > 0 && text_len && text_len <= WPAGE_ID_MAX) {
                struct page_element *e = &elements[element_count];
                if (name_end - name_start > 14) { p = tag_end; continue; }
                strcpy(e->tag, name);
                memcpy(e->id, id, text_len + 1);
                e->id_bytes = (uint16_t)text_len;
                e->content_start = tag_end;
                uint32_t end_tag = matching_close(page_bytes, page_length,
                                                  tag_end, name,
                                                  name_end - name_start);
                e->content_end = end_tag;
                if (end_tag < page_length) ++element_count;
            }
        }
        p = tag_end;
    }
    return WPAGE_OK;
}

int wpage_scan(const char *path, uint8_t *output, uint32_t capacity,
               uint32_t *output_bytes)
{
    uint32_t i, used = 2, bytes;
    int rc;
    if (output_bytes) *output_bytes = 0;
    if (!web_path(path) || !output || !output_bytes || capacity < 2 ||
        strlen(path) > 127)
        return WPAGE_E_ARGUMENT;
    free(page_bytes); free(script_bytes);
    page_bytes = script_bytes = NULL;
    page_length = page_capacity = 0;
    rc = read_file(path, &page_bytes, &page_length, WPAGE_MAX_BYTES);
    if (rc) return rc;
    page_capacity = page_length + 1;
    script_bytes = (char *)malloc(WPAGE_SCRIPT_TOTAL);
    if (!script_bytes) return WPAGE_E_LIMIT;
    memset(resources, 0, sizeof(resources));
    rc = parse_page(1, 1);
    if (rc) return rc;
    for (i = 0; i < script_count; ++i) if (scripts[i].external) {
        bytes = scripts[i].source_bytes;
        if (used > capacity || capacity - used < 4 + bytes) return WPAGE_E_LIMIT;
        output[used++] = (uint8_t)scripts[i].external_index;
        output[used++] = (uint8_t)(scripts[i].external_index >> 8);
        output[used++] = (uint8_t)bytes;
        output[used++] = (uint8_t)(bytes >> 8);
        memcpy(output + used, script_bytes + scripts[i].source_offset, bytes);
        used += bytes;
    }
    output[0] = (uint8_t)external_count;
    output[1] = (uint8_t)(external_count >> 8);
    *output_bytes = used;
    return WPAGE_OK;
}

int wpage_resource(uint32_t external_index, const char *path)
{
    uint32_t n;
    if (external_index >= external_count || !web_path(path)) return WPAGE_E_ARGUMENT;
    n = (uint32_t)strlen(path);
    if (!n || n >= sizeof(resources[0].path)) return WPAGE_E_LIMIT;
    memcpy(resources[external_index].path, path, n + 1);
    resources[external_index].set = 1;
    return WPAGE_OK;
}

static int replace_range(uint32_t start, uint32_t end,
                         const char *replacement, uint32_t bytes)
{
    uint32_t old, new_length, needed;
    char *grown;
    if (start > end || end > page_length) return WPAGE_E_FORMAT;
    old = end - start;
    if (bytes > WPAGE_MAX_BYTES - (page_length - old)) return WPAGE_E_LIMIT;
    new_length = page_length - old + bytes;
    needed = new_length + 1;
    if (needed > page_capacity) {
        grown = (char *)realloc(page_bytes, needed);
        if (!grown) return WPAGE_E_LIMIT;
        page_bytes = grown;
        page_capacity = needed;
    }
    memmove(page_bytes + start + bytes, page_bytes + end, page_length - end);
    if (bytes) memcpy(page_bytes + start, replacement, bytes);
    if (end <= document_write_cursor)
        document_write_cursor = document_write_cursor - old + bytes;
    else if (start < document_write_cursor)
        document_write_cursor = start + bytes;
    page_length = new_length;
    page_bytes[page_length] = 0;
    return WPAGE_OK;
}

static int find_element(const char *id, uint32_t id_bytes,
                        struct page_element *result)
{
    uint32_t i;
    for (i = 0; i < element_count; ++i)
        if (elements[i].id_bytes == id_bytes && !memcmp(elements[i].id, id, id_bytes)) {
            *result = elements[i]; return 1;
        }
    return 0;
}

static uint32_t script_close_offset(uint32_t wanted)
{
    uint32_t p = 0, index = 0, open, end, close, name_start, name_bytes;
    int closing, self_closing;
    while ((open = find_ci(page_bytes, page_length, p, "<")) < page_length) {
        if (starts_ci(page_bytes, page_length, open, "<!--")) {
            uint32_t comment_end = find_ci(page_bytes, page_length, open + 4, "-->");
            p = comment_end < page_length ? comment_end + 3 : page_length;
            continue;
        }
        if (!parse_tag(page_bytes, page_length, open, &end, &name_start,
                       &name_bytes, &closing, &self_closing)) {
            p = open + 1;
            continue;
        }
        if (!closing && !self_closing &&
            raw_text_tag(page_bytes + name_start, name_bytes)) {
            p = find_raw_text_end(page_bytes, page_length, end,
                                  page_bytes + name_start, name_bytes);
            continue;
        }
        if (closing || name_bytes != 6 ||
            !starts_ci(page_bytes, page_length, name_start, "script")) {
            p = end;
            continue;
        }
        close = find_ci(page_bytes, page_length, end + 1, "</script");
        if (close >= page_length) return page_length;
        if (!supported_script_type(page_bytes, page_length, name_start + name_bytes,
                                   end - 1)) {
            p = find_ci(page_bytes, page_length, close, ">") + 1;
            continue;
        }
        if (index++ == wanted) {
            uint32_t close_end = find_ci(page_bytes, page_length, close, ">");
            return close_end < page_length ? close_end + 1 : page_length;
        }
        p = find_ci(page_bytes, page_length, close, ">") + 1;
    }
    return page_length;
}

static int apply_tlvs(uint32_t bytes)
{
    uint32_t p = 0, id_bytes, text_bytes, op, i, out;
    char escaped[WJS_MAX_TEXT_BYTES * 6 + 1];
    struct page_element e;
    while (p < bytes) {
        if (bytes - p < WJS_TLV_HEADER_BYTES) return WPAGE_E_FORMAT;
        op = tlv[p] | ((uint32_t)tlv[p+1] << 8);
        id_bytes = tlv[p+2] | ((uint32_t)tlv[p+3] << 8);
        text_bytes = tlv[p+4] | ((uint32_t)tlv[p+5] << 8);
        p += WJS_TLV_HEADER_BYTES;
        if (id_bytes > bytes - p || text_bytes > bytes - p - id_bytes) return WPAGE_E_FORMAT;
        if (op == WJS_TLV_WRITE) {
            uint32_t pos = document_write_cursor;
            if (pos > page_length) pos = page_length;
            if (replace_range(pos, pos, (const char *)tlv + p + id_bytes, text_bytes)) return WPAGE_E_LIMIT;
            document_write_cursor = pos + text_bytes;
        } else if (op == WJS_TLV_SET_TEXT || op == WJS_TLV_SET_HTML) {
            if (!find_element((const char *)tlv + p, id_bytes, &e)) return WPAGE_E_FORMAT;
            out = 0;
            if (op == WJS_TLV_SET_TEXT) {
                for (i = 0; i < text_bytes; ++i) {
                    char c = (char)tlv[p + id_bytes + i];
                    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') {
                        const char *entity = c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;" : "&#39;";
                        uint32_t n = (uint32_t)strlen(entity);
                        memcpy(escaped + out, entity, n); out += n;
                    } else escaped[out++] = c;
                }
                escaped[out] = 0;
                if (replace_range(e.content_start, e.content_end, escaped, out)) return WPAGE_E_LIMIT;
            } else if (replace_range(e.content_start, e.content_end,
                                    (const char *)tlv + p + id_bytes, text_bytes)) return WPAGE_E_LIMIT;
            if (parse_page(0, 1)) return WPAGE_E_LIMIT;
        }
        p += id_bytes + text_bytes;
    }
    return WPAGE_OK;
}

int wpage_run(const char *output_path, char *error, uint32_t error_capacity)
{
    uint32_t i, out_bytes;
    unsigned long started, now;
    int rc;
    char external[64UL * 1024UL + 1];
    if (!page_bytes || !script_bytes || !web_path(output_path) || strlen(output_path) > 127)
        return WPAGE_E_ARGUMENT;
    if (error && error_capacity) error[0] = 0;
    if (parse_page(1, 1)) return WPAGE_E_FORMAT;
    started = (unsigned long)clock();
    wjs_reset();
    for (i = 0; i < element_count; ++i) {
        char text[WPAGE_TEXT_MAX + 1];
        uint32_t a = elements[i].content_start, b = elements[i].content_end, n = 0;
        while (a < b && n < WPAGE_TEXT_MAX) {
            if (page_bytes[a] == '<') { while (a < b && page_bytes[a++] != '>') { } }
            else text[n++] = page_bytes[a++];
        }
        text[n] = 0;
        if (wjs_set_element(elements[i].id, elements[i].id_bytes, text, n)) return WPAGE_E_LIMIT;
    }
    for (i = 0; i < script_count; ++i) {
        const char *source;
        uint32_t source_bytes;
        if (scripts[i].external) {
            uint32_t idx = scripts[i].external_index;
            if (idx >= WPAGE_SCRIPT_MAX || !resources[idx].set) {
                if (error && error_capacity) snprintf(error, error_capacity, "External script was not downloaded");
                break;
            }
            rc = read_file_into(resources[idx].path, external,
                                WPAGE_INLINE_MAX, &source_bytes);
            if (rc) {
                if (error && error_capacity) snprintf(error, error_capacity, "External script could not be read");
                break;
            }
            source = external;
        } else {
            source = script_bytes + scripts[i].source_offset;
            source_bytes = scripts[i].source_bytes;
        }
        document_write_cursor = script_close_offset(scripts[i].tag_ordinal);
        rc = wjs_eval(source, source_bytes, tlv, sizeof(tlv), &out_bytes);
        if (rc != WJS_OK) {
            const char *detail = wjs_last_error();
            if (error && error_capacity) {
                if (detail && detail[0])
                    snprintf(error, error_capacity,
                             "Script %lu failed (code %d): %.80s",
                             (unsigned long)(i + 1), rc, detail);
                else
                    snprintf(error, error_capacity,
                             "Script %lu failed (code %d)",
                             (unsigned long)(i + 1), rc);
            }
            break; /* Failed script emits no changes; keep earlier page work. */
        }
        rc = apply_tlvs(out_bytes);
        if (rc) return rc;
        now = (unsigned long)clock();
        if (now - started > 30UL * CLOCKS_PER_SEC) {
            if (error && error_capacity) snprintf(error, error_capacity, "JavaScript page budget reached");
            break;
        }
    }
    {
        FILE *f = fopen(output_path, "wb");
        if (!f) return WPAGE_E_IO;
        rc = fwrite(page_bytes, 1, page_length, f) == page_length ? WPAGE_OK : WPAGE_E_IO;
        if (fclose(f) != 0) rc = WPAGE_E_IO;
        return rc;
    }
}
