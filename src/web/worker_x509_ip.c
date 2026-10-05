#include "worker_x509_ip.h"

struct der_span {
    const unsigned char *data;
    size_t length;
};

struct der_cursor {
    const unsigned char *data;
    size_t length;
    size_t offset;
};

static int der_take(struct der_cursor *cursor, unsigned expected,
    struct der_span *span)
{
    size_t at, length, count, i;
    unsigned char first;
    if (cursor == 0 || span == 0 || cursor->offset >= cursor->length) return 0;
    at = cursor->offset;
    if (cursor->data[at++] != expected || at >= cursor->length) return 0;
    first = cursor->data[at++];
    if ((first & 0x80u) == 0) {
        length = first;
    } else {
        count = first & 0x7Fu;
        if (count == 0 || count > sizeof(size_t) || count > cursor->length - at ||
            cursor->data[at] == 0) return 0;
        length = 0;
        for (i = 0; i < count; ++i) {
            if (length > (((size_t)-1) >> 8)) return 0;
            length = (length << 8) | cursor->data[at++];
        }
        if (length < 128) return 0;
    }
    if (length > cursor->length - at) return 0;
    span->data = cursor->data + at;
    span->length = length;
    cursor->offset = at + length;
    return 1;
}

static int parse_san(const unsigned char *data, size_t length,
    const unsigned char address[4])
{
    struct der_cursor list;
    struct der_span names, name;
    int matched = 0, any = 0;
    list.data = data;
    list.length = length;
    list.offset = 0;
    if (!der_take(&list, 0x30, &names) || list.offset != list.length) return 0;
    list.data = names.data;
    list.length = names.length;
    list.offset = 0;
    while (list.offset < list.length) {
        size_t before = list.offset;
        unsigned tag = list.data[before];
        struct der_cursor one;
        one.data = list.data;
        one.length = list.length;
        one.offset = before;
        if (!der_take(&one, tag, &name) || one.offset <= before) return 0;
        list.offset = one.offset;
        any = 1;
        if (tag == 0x87u && name.length == 4 &&
            name.data[0] == address[0] && name.data[1] == address[1] &&
            name.data[2] == address[2] && name.data[3] == address[3]) matched = 1;
    }
    return any && matched;
}

static int parse_extensions(const unsigned char *data, size_t length,
    const unsigned char address[4])
{
    static const unsigned char san_oid[3] = {0x55, 0x1D, 0x11};
    struct der_cursor sequence, extensions;
    struct der_span seq, item, oid, critical, value;
    int found = 0, matched = 0;
    sequence.data = data;
    sequence.length = length;
    sequence.offset = 0;
    if (!der_take(&sequence, 0x30, &seq) || sequence.offset != sequence.length) return 0;
    extensions.data = seq.data;
    extensions.length = seq.length;
    extensions.offset = 0;
    while (extensions.offset < extensions.length) {
        struct der_cursor fields;
        if (!der_take(&extensions, 0x30, &item)) return 0;
        fields.data = item.data;
        fields.length = item.length;
        fields.offset = 0;
        if (!der_take(&fields, 0x06, &oid)) return 0;
        if (fields.offset < fields.length && fields.data[fields.offset] == 0x01) {
            if (!der_take(&fields, 0x01, &critical) || critical.length != 1 ||
                (critical.data[0] != 0 && critical.data[0] != 0xFFu)) return 0;
        }
        if (!der_take(&fields, 0x04, &value) || fields.offset != fields.length) return 0;
        if (oid.length == sizeof san_oid &&
            oid.data[0] == san_oid[0] && oid.data[1] == san_oid[1] &&
            oid.data[2] == san_oid[2]) {
            if (found) return 0;
            found = 1;
            matched = parse_san(value.data, value.length, address);
        }
    }
    return found && matched;
}

int wx509_has_ipv4_san(const unsigned char *certificate, size_t length,
    const unsigned char address[4])
{
    struct der_cursor outer, tbs;
    struct der_span cert, body, field, signature;
    if (certificate == 0 || address == 0 || length == 0) return 0;
    outer.data = certificate;
    outer.length = length;
    outer.offset = 0;
    if (!der_take(&outer, 0x30, &cert) || outer.offset != outer.length) return 0;
    outer.data = cert.data;
    outer.length = cert.length;
    outer.offset = 0;
    if (!der_take(&outer, 0x30, &body) ||
        !der_take(&outer, 0x30, &field) ||
        !der_take(&outer, 0x03, &signature) || outer.offset != outer.length ||
        signature.length == 0 || signature.data[0] > 7) return 0;
    tbs.data = body.data;
    tbs.length = body.length;
    tbs.offset = 0;
    if (tbs.offset < tbs.length && tbs.data[tbs.offset] == 0xA0 &&
        !der_take(&tbs, 0xA0, &field)) return 0;
    if (!der_take(&tbs, 0x02, &field) || !der_take(&tbs, 0x30, &field) ||
        !der_take(&tbs, 0x30, &field) || !der_take(&tbs, 0x30, &field) ||
        !der_take(&tbs, 0x30, &field) || !der_take(&tbs, 0x30, &field)) return 0;
    while (tbs.offset < tbs.length) {
        unsigned tag = tbs.data[tbs.offset];
        if (!der_take(&tbs, tag, &field)) return 0;
        if (tag == 0xA3u) {
            if (tbs.offset != tbs.length) return 0;
            return parse_extensions(field.data, field.length, address);
        }
        if (tag != 0x81u && tag != 0x82u) return 0;
    }
    return 0;
}
