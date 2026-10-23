/* Bounded NETWORK.INI codec shared with host tests. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/net_config.h>
#if defined(CIUKI_CONFIG_USER) || __STDC_HOSTED__
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#define ksnprintf snprintf
#else
#include <ciuki/kernel.h>
#endif
static int config_cmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
void net_config_default(struct ciuki_net_config *c)
{
    memset(c, 0, sizeof(*c)); c->enabled = c->dhcp = 1;
    memcpy(c->hostname, "ciukios", 8);
}
static uint32_t host_word(uint32_t a)
{
    const uint8_t *b = (const void *)&a;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
static bool unicast(uint32_t a)
{ unsigned first = host_word(a) >> 24; return first && first != 127 && first < 224; }
static bool name_valid(const char *s, unsigned cap, bool domain)
{
    unsigned label = 0;
    for (unsigned i = 0; i < cap; i++) {
        unsigned ch = (unsigned char)s[i];
        if (!ch) return !i || (label && s[i-1] != '-');
        if (ch == '.' && domain) { if (!label || s[i-1] == '-') return false; label = 0; continue; }
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-') || (!label && ch == '-') || ++label > 63) return false;
    }
    return false;
}
bool net_config_valid(const struct ciuki_net_config *c)
{
    if (c->enabled > 1 || c->dhcp > 1 ||
        !((!c->speed && !c->duplex) || ((c->speed == 10 || c->speed == 100) && (c->duplex == 1 || c->duplex == 2))) ||
        !name_valid(c->hostname, sizeof(c->hostname), false) || !name_valid(c->domain, sizeof(c->domain), true)) return false;
    for (unsigned i = 0; i < 3; i++) if ((c->dns[i] && !unicast(c->dns[i])) || (i && c->dns[i] && !c->dns[i-1])) return false;
    if (c->dhcp) return !c->address && !c->mask && !c->gateway;
    uint32_t ip = host_word(c->address), mask = host_word(c->mask), tail = ~mask;
    if (!unicast(c->address) || !mask || tail < 3 || (tail & (tail+1)) || !(ip & tail) || (ip & tail) == tail) return false;
    uint32_t gw = host_word(c->gateway);
    return !gw || (unicast(c->gateway) && gw != ip && (gw & mask) == (ip & mask) && (gw & tail) && (gw & tail) != tail);
}
static uint32_t hash(const char *s, size_t n)
{ uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u; return h; }
static bool number(const char *s, uint32_t *out)
{
    uint32_t v = 0; if (!*s) return false;
    while (*s) { unsigned d = (unsigned)(*s++ - '0'); if (d > 9 || v > (UINT32_MAX-d)/10) return false; v = v*10+d; }
    *out = v; return true;
}
static bool ip_parse(const char *s, uint32_t *out)
{
    uint8_t b[4];
    for (unsigned i = 0; i < 4; i++) {
        unsigned v = 0, n = 0;
        while (*s >= '0' && *s <= '9') { if (++n > 3) return false; v = v*10+(unsigned)(*s++-'0'); }
        if (!n || v > 255 || (i < 3 ? *s++ != '.' : *s != 0)) return false;
        b[i] = (uint8_t)v;
    }
    memcpy(out, b, 4); return true;
}
static const char *const keys[] = {"enabled", "dhcp", "speed", "duplex", "address", "mask", "gateway", "dns1", "dns2", "dns3", "hostname", "domain", "vendor", "device", "mac"};
static int hex_digit(char ch)
{ return ch >= '0' && ch <= '9' ? ch-'0' : ch >= 'a' && ch <= 'f' ? ch-'a'+10 : -1; }
int net_config_parse(const char *text, size_t bytes, struct ciuki_net_config out[2], struct net_config_identity *identities)
{
    struct ciuki_net_config parsed[2]; memset(parsed, 0, sizeof(parsed));
    struct net_config_identity ids[2] = {0};
    unsigned seen[2] = {0}, index = 2, sections = 0; bool version = false;
    if (!bytes || bytes >= NET_SETTINGS_BYTES) return -EINVAL;
    for (size_t at = 0; at < bytes;) {
        size_t start = at, n = 0; char line[192];
        while (at < bytes && text[at] != '\n') { if (!text[at] || n+1 == sizeof(line)) return -EINVAL; line[n++] = text[at++]; }
        if (at == bytes) return -EINVAL;
        at++; line[n] = 0;
        if (!version) { if (config_cmp(line, "version=1")) return -EINVAL; version = true; continue; }
        if (!config_cmp(line, "[en0]")) { if (sections & 1) return -EINVAL; sections |= 1; index = 0; continue; }
        if (!config_cmp(line, "[en1]")) { if (sections & 2) return -EINVAL; sections |= 2; index = 1; continue; }
        if (!strncmp(line, "checksum=", 9)) {
            uint32_t sum;
            if (at != bytes || !number(line+9, &sum) || sum != hash(text, start) || seen[0] != 32767 || seen[1] != 32767 ||
                !net_config_valid(&parsed[0]) || !net_config_valid(&parsed[1])) return -EINVAL;
            for (unsigned i = 0; i < 2; i++) {
                unsigned bits = 0; for (unsigned j = 0; j < 6; j++) bits |= ids[i].mac[j];
                if ((ids[i].vendor || ids[i].device || bits) && (!ids[i].vendor || !bits || (ids[i].mac[0] & 1))) return -EINVAL;
            }
            if (ids[0].vendor && !memcmp(&ids[0], &ids[1], sizeof(ids[0]))) return -EINVAL;
            if (identities) memcpy(identities, ids, sizeof(ids));
            memcpy(out, parsed, sizeof(parsed)); return 0;
        }
        if (index > 1) return -EINVAL;
        char *value = line; while (*value && *value != '=') value++;
        if (!*value) return -EINVAL; *value++ = 0;
        unsigned k = 0; while (k < 15 && config_cmp(keys[k], line)) k++;
        if (k == 15 || (seen[index] & (1u << k))) return -EINVAL;
        seen[index] |= 1u << k;
        struct ciuki_net_config *c = &parsed[index];
        if (k < 10) {
            uint32_t v; if (!(k < 4 ? number(value, &v) : ip_parse(value, &v))) return -EINVAL;
            if (k == 0) c->enabled=v; else if (k == 1) c->dhcp=v; else if (k == 2) c->speed=v; else if (k == 3) c->duplex=v;
            else if (k == 4) c->address=v; else if (k == 5) c->mask=v; else if (k == 6) c->gateway=v; else c->dns[k-7]=v;
        } else if (k < 12) {
            char *to = k == 10 ? c->hostname : c->domain; size_t cap = k == 10 ? sizeof(c->hostname) : sizeof(c->domain);
            if (strlen(value) >= cap) return -EINVAL; memcpy(to, value, strlen(value)+1);
        } else if (k < 14) {
            uint32_t v; if (!number(value, &v) || v > 65535) return -EINVAL;
            if (k == 12) ids[index].vendor = v; else ids[index].device = v;
        } else {
            if (strlen(value) != 17) return -EINVAL;
            for (unsigned i = 0; i < 6; i++) {
                int hi = hex_digit(value[i*3]), lo = hex_digit(value[i*3+1]);
                if (hi < 0 || lo < 0 || (i < 5 && value[i*3+2] != ':')) return -EINVAL;
                ids[index].mac[i] = (uint8_t)((unsigned)hi*16+(unsigned)lo);
            }
        }
    }
    return -EINVAL;
}
size_t net_config_format(const struct ciuki_net_config c[2], const struct net_config_identity *identities, char *out, size_t cap)
{
    if (cap < NET_SETTINGS_BYTES || !net_config_valid(&c[0]) || !net_config_valid(&c[1])) return 0;
    size_t at = (size_t)ksnprintf(out, cap, "version=1\n");
    for (unsigned i = 0; i < 2; i++) {
        at += (size_t)ksnprintf(out+at, cap-at, "[en%u]\nenabled=%u\ndhcp=%u\nspeed=%u\nduplex=%u\n", i, c[i].enabled, c[i].dhcp, c[i].speed, c[i].duplex);
        uint32_t values[6] = {c[i].address, c[i].mask, c[i].gateway, c[i].dns[0], c[i].dns[1], c[i].dns[2]};
        for (unsigned j = 0; j < 6; j++) { const uint8_t *b = (const void *)&values[j];
            at += (size_t)ksnprintf(out+at, cap-at, "%s=%u.%u.%u.%u\n", keys[j+4], b[0], b[1], b[2], b[3]); }
        at += (size_t)ksnprintf(out+at, cap-at, "hostname=%s\ndomain=%s\n", c[i].hostname, c[i].domain);
        struct net_config_identity zero = {0}; const struct net_config_identity *id = identities ? &identities[i] : &zero;
        at += (size_t)ksnprintf(out+at, cap-at, "vendor=%u\ndevice=%u\nmac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            id->vendor, id->device, id->mac[0], id->mac[1], id->mac[2], id->mac[3], id->mac[4], id->mac[5]);
    }
    uint32_t sum = hash(out, at);
    at += (size_t)ksnprintf(out+at, cap-at, "checksum=%u\n", sum);
    return at < cap ? at : 0;
}
