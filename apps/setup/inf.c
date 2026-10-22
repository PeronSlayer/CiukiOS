/* SPDX-License-Identifier: MIT
 * SETUP.INF schema version 1 (docs/design/cd-setup-plan.md §8, D10): a
 * strict parser and the unattended rules. Syntax follows the Windows INF
 * conventions Setup users know: [Section] headers, key=value lines, ';'
 * comments (outside double quotes), case-insensitive section and key names
 * and yes/no values. Anything the schema does not define stops Setup with a
 * status id; nothing is guessed. */
#include "setup.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *const setup_inf_key_names[SETUP_KEY_COUNT] = {
    "Version", "AcceptLicences", "EraseDisk", "Disk", "Reinstall", "Components", "UserName",
    "ComputerName", "Keyboard", "TimeZone", "Clock", "Network", "IPX", "Restart"
};
const char *const setup_component_names[SETUP_COMP_COUNT] = {
    "system", "dos", "windows", "netsurf", "games", "devtools", "soundfont", "sources"
};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int same(const char *a, size_t n, const char *b)
{
    size_t m = strlen(b);
    if (n != m) return 0;
    for (size_t i = 0; i < n; i++) if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}
static void set_status(struct setup_inf_status *st, const char *reason, const char *key, size_t key_length, unsigned line)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
    snprintf(st->reason, sizeof(st->reason), "%s", reason);
    if (key_length > sizeof(st->key) - 1) key_length = sizeof(st->key) - 1;
    if (key) memcpy(st->key, key, key_length);
    st->line = line;
}
static int stop(struct setup_inf_status *st, const char *reason, const char *key, unsigned line)
{ set_status(st, reason, key, key ? strlen(key) : 0, line); return -1; }

void setup_answers_defaults(struct setup_answers *a)
{
    memset(a, 0, sizeof(*a));
    a->disk = SETUP_DISK_ATA0;
    a->components = SETUP_COMP_SYSTEM | SETUP_COMP_DOS | SETUP_COMP_WINDOWS | SETUP_COMP_NETSURF |
                    SETUP_COMP_GAMES | SETUP_COMP_SOUNDFONT;
    snprintf(a->keyboard, sizeof(a->keyboard), "us");
    a->clock = SETUP_CLOCK_LOCAL;
    a->network = SETUP_NET_DHCP;
    a->ipx = 1; a->lobby = 1;
    a->restart = SETUP_RESTART_EJECT;
}

/* UTF-8 sequence for U+0020..U+007E and U+00A0..U+00FF (the face's range). */
static int latin1_chars(const char *s, size_t *count)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; n++) {
        if (*p >= 0x20 && *p < 0x7f) { p++; continue; }
        if ((p[0] == 0xc2 && p[1] >= 0xa0 && p[1] <= 0xbf) || (p[0] == 0xc3 && p[1] >= 0x80 && p[1] <= 0xbf)) { p += 2; continue; }
        return -1;
    }
    *count = n;
    return 0;
}

int setup_valid_user(const char *s)
{
    size_t n;
    if (latin1_chars(s, &n) || n < 1 || n > SETUP_USER_MAX) return 0;
    return s[0] != ' ' && s[strlen(s) - 1] != ' ';
}

/* RFC 1123 host-name label characters within the 15-character NetBIOS limit:
 * a name both the TCP/IP lobby and IPX/NetBIOS games accept. */
int setup_valid_computer(const char *s)
{
    size_t n = strlen(s), digits = 0;
    if (n < 1 || n > SETUP_COMPUTER_MAX || s[0] == '-' || s[n - 1] == '-') return 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c >= '0' && c <= '9') digits++;
        else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-')) return 0;
    }
    return digits != n;
}

int setup_parse_ipv4(const char *s, uint8_t out[4])
{
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return -1;
        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s++ - '0'); if (++digits > 3 || v > 255) return -1; }
        if (digits > 1 && s[-(int)digits] == '0') return -1;     /* no leading zeros */
        out[i] = (uint8_t)v;
        if (i < 3 && *s++ != '.') return -1;
    }
    return *s ? -1 : 0;
}

static uint32_t ip32(const uint8_t a[4]) { return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3]; }
int setup_static_valid(const uint8_t ip[4], unsigned prefix, const uint8_t gateway[4], const uint8_t dns[4])
{
    if (prefix < 8 || prefix > 30) return 0;
    uint32_t mask = prefix ? 0xffffffffu << (32 - prefix) : 0, a = ip32(ip);
    if (ip[0] == 0 || ip[0] == 127 || ip[0] >= 224) return 0;
    if ((a & ~mask) == 0 || (a & ~mask) == ~mask) return 0;   /* network or broadcast address */
    uint32_t g = ip32(gateway), d = ip32(dns);
    if (g && ((g & mask) != (a & mask) || g == a || (g & ~mask) == 0 || (g & ~mask) == ~mask)) return 0;
    if (d && (dns[0] == 0 || dns[0] == 127 || dns[0] >= 224)) return 0;
    return 1;
}

int setup_components_valid(uint32_t c)
{
    if (!(c & SETUP_COMP_SYSTEM) || (c & ~SETUP_COMP_ALL)) return 0;
    for (unsigned i = 0; i < SETUP_COMP_COUNT; i++)
        if ((c & st_components[i].bit) && (c & st_components[i].requires) != st_components[i].requires) return 0;
    return 1;
}

static int yes_no(const char *v, size_t n, uint8_t *out)
{
    if (same(v, n, "yes")) { *out = 1; return 0; }
    if (same(v, n, "no")) { *out = 0; return 0; }
    return -1;
}

static int copy_value(char *out, size_t cap, const char *v, size_t n)
{
    if (n >= cap) return -1;
    memcpy(out, v, n); out[n] = 0;
    return 0;
}

/* model="<IDENTIFY model>" serial="<serial>" (either or both, any order). */
static int disk_selector(struct setup_answers *a, const char *v, size_t n)
{
    if (same(v, n, "ata0")) { a->disk = SETUP_DISK_ATA0; return 0; }
    if (same(v, n, "auto")) { a->disk = SETUP_DISK_AUTO; return 0; }
    const char *p = v, *end = v + n;
    int model = 0, serial = 0;
    while (p < end) {
        while (p < end && *p == ' ') p++;
        if (p == end) break;
        const char *name = p;
        while (p < end && *p != '=' && *p != ' ') p++;
        size_t name_length = (size_t)(p - name);
        if (p >= end || *p != '=' || p + 1 >= end || p[1] != '"') return -1;
        p += 2;
        const char *text = p;
        while (p < end && *p != '"') p++;
        if (p >= end) return -1;
        size_t text_length = (size_t)(p - text);
        p++;
        if (p < end && *p != ' ') return -1;
        while (text_length && text[text_length - 1] == ' ') text_length--;
        while (text_length && *text == ' ') { text++; text_length--; }
        if (!text_length) return -1;
        if (same(name, name_length, "model") && !model) {
            if (copy_value(a->disk_model, sizeof(a->disk_model), text, text_length)) return -1;
            model = 1;
        } else if (same(name, name_length, "serial") && !serial) {
            if (copy_value(a->disk_serial, sizeof(a->disk_serial), text, text_length)) return -1;
            serial = 1;
        } else return -1;
    }
    if (!model && !serial) return -1;
    a->disk = SETUP_DISK_MATCH;
    return 0;
}

static int components(struct setup_answers *a, const char *v, size_t n, int *dependency)
{
    uint32_t set = SETUP_COMP_SYSTEM, seen = 0;
    const char *p = v, *end = v + n;
    while (n && p <= end) {                /* an empty value installs the system only */
        const char *item = p;
        while (p < end && *p != ',') p++;
        size_t length = (size_t)(p - item);
        while (length && (*item == ' ' || *item == '\t')) { item++; length--; }
        while (length && (item[length - 1] == ' ' || item[length - 1] == '\t')) length--;
        unsigned k = 0;
        while (length && k < SETUP_COMP_COUNT && !same(item, length, setup_component_names[k])) k++;
        if (!length || k == SETUP_COMP_COUNT || (seen & (1u << k))) return -1;
        seen |= 1u << k; set |= 1u << k;
        if (p == end) break;
        p++;                               /* past ','; a trailing comma leaves an empty item */
    }
    *dependency = !setup_components_valid(set);
    a->components = set;
    return 0;
}

static int time_zone(struct setup_answers *a, const char *v, size_t n)
{
    if (n != 6 || (v[0] != '+' && v[0] != '-') || v[3] != ':') return -1;
    for (int i = 1; i < 6; i++) if (i != 3 && (v[i] < '0' || v[i] > '9')) return -1;
    int hours = (v[1] - '0') * 10 + (v[2] - '0'), minutes = (v[4] - '0') * 10 + (v[5] - '0');
    if (minutes != 0 && minutes != 30 && minutes != 45) return -1;
    int total = (hours * 60 + minutes) * (v[0] == '-' ? -1 : 1);
    if (total < SETUP_TZ_MIN || total > SETUP_TZ_MAX) return -1;
    a->tz_minutes = (int16_t)total;
    return 0;
}

/* dhcp | none | static:<ip>/<prefix>,<gateway>,<dns> (gateway and DNS may be
 * empty: a crossover cable or a switch without a router). */
static int network(struct setup_answers *a, const char *v, size_t n)
{
    if (same(v, n, "dhcp")) { a->network = SETUP_NET_DHCP; return 0; }
    if (same(v, n, "none")) { a->network = SETUP_NET_NONE; return 0; }
    if (n < 8 || !same(v, 7, "static:") || n > 80) return -1;
    char text[81]; memcpy(text, v + 7, n - 7); text[n - 7] = 0;
    char *slash = strchr(text, '/'), *c1 = slash ? strchr(slash, ',') : NULL, *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
    if (!slash || !c1 || !c2 || strchr(c2 + 1, ',')) return -1;
    *slash = 0; *c1 = 0; *c2 = 0;
    const char *prefix = slash + 1;
    if (!prefix[0] || strlen(prefix) > 2 || prefix[0] < '0' || prefix[0] > '9' || (prefix[1] && (prefix[1] < '0' || prefix[1] > '9'))) return -1;
    uint8_t ip[4], gateway[4] = {0}, dns[4] = {0};
    unsigned bits = (unsigned)(prefix[0] - '0') * (prefix[1] ? 10u : 1u) + (prefix[1] ? (unsigned)(prefix[1] - '0') : 0u);
    if (setup_parse_ipv4(text, ip) || (c1[1] && setup_parse_ipv4(c1 + 1, gateway)) || (c2[1] && setup_parse_ipv4(c2 + 1, dns))) return -1;
    if (!setup_static_valid(ip, bits, gateway, dns)) return -1;
    a->network = SETUP_NET_STATIC;
    memcpy(a->ip, ip, 4); memcpy(a->gateway, gateway, 4); memcpy(a->dns, dns, 4); a->prefix = (uint8_t)bits;
    return 0;
}

static int value(struct setup_answers *a, enum setup_key k, const char *v, size_t n, int *dependency)
{
    uint8_t flag;
    switch (k) {
    case SETUP_KEY_VERSION: return n == 1 && v[0] == '1' ? 0 : -1;
    case SETUP_KEY_ACCEPT_LICENCES: if (yes_no(v, n, &flag) || !flag) return -1; a->accept_licences = 1; return 0;
    case SETUP_KEY_ERASE_DISK: if (yes_no(v, n, &flag)) return -1; a->erase_disk = flag; return 0;
    case SETUP_KEY_DISK: return disk_selector(a, v, n);
    case SETUP_KEY_REINSTALL: if (yes_no(v, n, &flag)) return -1; a->reinstall = flag; return 0;
    case SETUP_KEY_COMPONENTS: return components(a, v, n, dependency);
    case SETUP_KEY_USER_NAME:
        if (copy_value(a->user_name, sizeof(a->user_name), v, n) || !setup_valid_user(a->user_name)) return -1;
        return 0;
    case SETUP_KEY_COMPUTER_NAME:
        if (copy_value(a->computer_name, sizeof(a->computer_name), v, n) || !setup_valid_computer(a->computer_name)) return -1;
        for (char *p = a->computer_name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        return 0;
    case SETUP_KEY_KEYBOARD:
        for (unsigned i = 0; i < st_keyboard_count; i++)
            if (same(v, n, st_keyboards[i].id)) { snprintf(a->keyboard, sizeof(a->keyboard), "%s", st_keyboards[i].id); return 0; }
        return -1;
    case SETUP_KEY_TIME_ZONE: return time_zone(a, v, n);
    case SETUP_KEY_CLOCK:
        if (same(v, n, "local")) a->clock = SETUP_CLOCK_LOCAL;
        else if (same(v, n, "utc")) a->clock = SETUP_CLOCK_UTC;
        else return -1;
        return 0;
    case SETUP_KEY_NETWORK: return network(a, v, n);
    case SETUP_KEY_IPX: if (yes_no(v, n, &flag)) return -1; a->ipx = flag; return 0;
    case SETUP_KEY_RESTART:
        if (same(v, n, "eject")) a->restart = SETUP_RESTART_EJECT;
        else if (same(v, n, "wait")) a->restart = SETUP_RESTART_WAIT;
        else if (same(v, n, "force")) a->restart = SETUP_RESTART_FORCE;
        else return -1;
        return 0;
    case SETUP_KEY_COUNT: break;
    }
    return -1;
}

struct line { const char *s; size_t n; unsigned number; };
static int next_line(const char **p, const char *end, struct line *out, unsigned *number)
{
    if (*p >= end) return 0;
    const char *s = *p, *e = memchr(s, '\n', (size_t)(end - s));
    if (!e) e = end;
    *p = e < end ? e + 1 : end;
    size_t n = (size_t)(e - s);
    if (n && s[n - 1] == '\r') n--;
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    out->s = s; out->n = n; out->number = ++*number;
    return 1;
}
/* Value up to an unquoted ';', trimmed. */
static size_t strip_comment(const char *v, size_t n)
{
    int quoted = 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] == '"') quoted = !quoted;
        else if (v[i] == ';' && !quoted) { n = i; break; }
    }
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
    return n;
}

int setup_inf_parse(const char *text, size_t length, struct setup_answers *a, struct setup_inf_status *st)
{
    setup_answers_defaults(a);
    a->unattended = 1;
    if (length > SETUP_INF_MAX_BYTES) return stop(st, SETUP_ST_TOO_LARGE, "SETUP.INF", 0);
    if (length >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf) { text += 3; length -= 3; }
    if (memchr(text, 0, length)) return stop(st, SETUP_ST_SYNTAX, "line1", 1);
    const char *end = text + length;
    /* Pass 1: the version decides how the rest is read. */
    const char *p = text; struct line l; unsigned number = 0; int in_setup = 0;
    while (next_line(&p, end, &l, &number)) {
        if (!l.n || l.s[0] == ';') continue;
        if (l.s[0] == '[') { in_setup = l.n > 2 && l.s[l.n - 1] == ']' && same(l.s + 1, l.n - 2, "Setup"); continue; }
        const char *eq = memchr(l.s, '=', l.n);
        if (!in_setup || !eq) continue;
        size_t kn = (size_t)(eq - l.s);
        while (kn && (l.s[kn - 1] == ' ' || l.s[kn - 1] == '\t')) kn--;
        if (!same(l.s, kn, "Version")) continue;
        const char *v = eq + 1; size_t vn = l.n - (size_t)(v - l.s);
        while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
        vn = strip_comment(v, vn);
        if (!(vn == 1 && v[0] == '1')) return stop(st, SETUP_ST_VERSION, "Version", l.number);
    }
    /* Pass 2: strict reading in file order. */
    p = text; number = 0; in_setup = 0;
    int setup_seen = 0, dependency = 0;
    unsigned dependency_line = 0;
    while (next_line(&p, end, &l, &number)) {
        char where[16]; snprintf(where, sizeof(where), "line%u", l.number);
        if (!l.n || l.s[0] == ';') continue;
        if (l.s[0] == '[') {
            if (l.n < 3 || l.s[l.n - 1] != ']') return stop(st, SETUP_ST_SYNTAX, where, l.number);
            const char *name = l.s + 1; size_t nn = l.n - 2;
            while (nn && *name == ' ') { name++; nn--; }
            while (nn && name[nn - 1] == ' ') nn--;
            if (!same(name, nn, "Setup")) { set_status(st, SETUP_ST_SECTION, name, nn, l.number); return -1; }
            if (setup_seen) return stop(st, SETUP_ST_DUPLICATE, "[Setup]", l.number);
            setup_seen = in_setup = 1;
            continue;
        }
        const char *eq = memchr(l.s, '=', l.n);
        if (!eq || eq == l.s || !in_setup) return stop(st, SETUP_ST_SYNTAX, where, l.number);
        size_t kn = (size_t)(eq - l.s);
        while (kn && (l.s[kn - 1] == ' ' || l.s[kn - 1] == '\t')) kn--;
        unsigned k = 0;
        while (k < SETUP_KEY_COUNT && !same(l.s, kn, setup_inf_key_names[k])) k++;
        if (k == SETUP_KEY_COUNT) { set_status(st, SETUP_ST_UNKNOWN_KEY, l.s, kn, l.number); return -1; }
        if (a->present & (1u << k)) return stop(st, SETUP_ST_DUPLICATE, setup_inf_key_names[k], l.number);
        const char *v = eq + 1; size_t vn = l.n - (size_t)(v - l.s);
        while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
        vn = strip_comment(v, vn);
        int needs_dependency = 0;
        if (value(a, (enum setup_key)k, v, vn, &needs_dependency))
            return stop(st, SETUP_ST_VALUE, setup_inf_key_names[k], l.number);
        if (needs_dependency) { dependency = 1; dependency_line = l.number; }
        a->present |= 1u << k;
    }
    if (!setup_seen) return stop(st, SETUP_ST_SECTION, "", 0);
    if (!(a->present & (1u << SETUP_KEY_VERSION))) return stop(st, SETUP_ST_MISSING, "Version", 0);
    if (dependency) return stop(st, SETUP_ST_DEPENDENCY, "Components", dependency_line);
    a->lobby = (a->components & SETUP_COMP_WINDOWS) != 0;
    return 0;
}

int setup_inf_complete(const struct setup_answers *a, struct setup_inf_status *st)
{
    static const enum setup_key order[] = {
        SETUP_KEY_ACCEPT_LICENCES, SETUP_KEY_DISK, SETUP_KEY_COMPONENTS, SETUP_KEY_USER_NAME,
        SETUP_KEY_COMPUTER_NAME, SETUP_KEY_KEYBOARD, SETUP_KEY_TIME_ZONE, SETUP_KEY_NETWORK, SETUP_KEY_IPX
    };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        if (!(a->present & (1u << order[i]))) return stop(st, SETUP_ST_MISSING, setup_inf_key_names[order[i]], 0);
    return 0;
}

static int identity_equal(const char *want, const char *have)
{
    while (*have == ' ') have++;
    size_t n = strlen(have);
    while (n && have[n - 1] == ' ') n--;
    return strlen(want) == n && !memcmp(want, have, n);
}

int setup_inf_disk_rules(const struct setup_answers *a, const struct setup_inf_disk d[4],
                         const char *release, struct setup_inf_status *st)
{
    unsigned count = 0;
    for (unsigned i = 0; i < 4; i++) count += d[i].present != 0;
    switch (a->disk) {
    case SETUP_DISK_UNSET: return stop(st, SETUP_ST_MISSING, "Disk", 0);
    case SETUP_DISK_AUTO:
        if (count > 1) return stop(st, SETUP_ST_AMBIGUOUS, "Disk", 0);
        if (!d[0].present) return stop(st, SETUP_ST_NOT_FOUND, "Disk", 0);
        break;
    case SETUP_DISK_ATA0:
        if (!d[0].present) return stop(st, SETUP_ST_NOT_FOUND, "Disk", 0);
        break;
    case SETUP_DISK_MATCH:
        if (!d[0].present || (a->disk_model[0] && !identity_equal(a->disk_model, d[0].model)) ||
            (a->disk_serial[0] && !identity_equal(a->disk_serial, d[0].serial)))
            return stop(st, SETUP_ST_NOT_FOUND, "Disk", 0);
        break;
    }
    if (d[0].installed && release && release[0] && !strcmp(d[0].release, release) && !a->reinstall)
        return stop(st, SETUP_ST_INSTALLED, "Reinstall", 0);
    if (d[0].occupied && !a->erase_disk) return stop(st, SETUP_ST_CONSENT, "EraseDisk", 0);
    return 0;
}

int setup_status_line(char *out, size_t cap, const struct setup_inf_status *st)
{
    return snprintf(out, cap, "[setup] status=stopped reason=%s key=%s", st->reason[0] ? st->reason : "unknown",
                    st->key[0] ? st->key : "-");
}

/* Search order (plan §8): D:\SETUP.INF on the CD, then \SETUP.INF on the
 * first FAT USB volume in letter order. */
int st_answers_load(struct st_app *a)
{
    char path[48] = "";
    size_t length = 0;
    int r = a->h.read_file(a->h.ctx, "D:\\SETUP.INF", a->file, ST_FILE_BYTES, &length);
    if (!r) snprintf(path, sizeof(path), "D:\\SETUP.INF");
    else if (r == -ENOENT || r == -ENODEV || r == -ENXIO) {
        int usb = st_usb_volume(a);
        if (usb < 0) return 0;
        snprintf(path, sizeof(path), "%c:\\SETUP.INF", a->volumes[usb].letter);
        r = a->h.read_file(a->h.ctx, path, a->file, ST_FILE_BYTES, &length);
        if (r == -ENOENT) return 0;
    }
    a->answers_found = 1; a->unattended = 1;
    st_log(a, "answers path=%s result=%d bytes=%zu", path, r, length);
    if (r == -EFBIG || (!r && length > SETUP_INF_MAX_BYTES)) {
        setup_answers_defaults(&a->answers); a->answers.unattended = 1;
        snprintf(a->answers.source, sizeof(a->answers.source), "%s", path);
        a->status_valid = 1;
        return stop(&a->status, SETUP_ST_TOO_LARGE, "SETUP.INF", 0);
    }
    if (r) {
        setup_answers_defaults(&a->answers); a->answers.unattended = 1;
        snprintf(a->answers.source, sizeof(a->answers.source), "%s", path);
        a->status_valid = 1;
        return stop(&a->status, SETUP_ST_READ, "SETUP.INF", 0);
    }
    int bad = setup_inf_parse(a->file, length, &a->answers, &a->status);
    snprintf(a->answers.source, sizeof(a->answers.source), "%s", path);
    if (bad || setup_inf_complete(&a->answers, &a->status)) { a->status_valid = 1; return -1; }
    struct setup_inf_disk disks[4];
    memset(disks, 0, sizeof(disks));
    for (int i = 0; i < a->disk_count; i++) {
        const struct st_disk *d = &a->disks[i];
        unsigned slot = (unsigned)d->slot * 2u + d->unit;
        if (!d->present || slot > 3) continue;
        disks[slot].present = 1;
        disks[slot].occupied = d->part_count > 0 || d->install != ST_INSTALL_NONE;
        disks[slot].installed = d->install == ST_INSTALL_DONE;
        snprintf(disks[slot].model, sizeof(disks[slot].model), "%s", d->model);
        snprintf(disks[slot].serial, sizeof(disks[slot].serial), "%s", d->serial);
        snprintf(disks[slot].release, sizeof(disks[slot].release), "%s", d->release);
    }
    if (setup_inf_disk_rules(&a->answers, disks, a->release, &a->status)) { a->status_valid = 1; return -1; }
    return 1;
}
