/* SPDX-License-Identifier: MIT */
#include "sounds.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
const char *const sound_keys[SOUND_EVENTS] = {
    "startup", "shutdown", "menu_open", "menu_select", "window_open", "window_close",
    "minimise", "restore", "error", "information", "question", "end_task", "volume", "empty_recycle"
};
const char *const sound_labels[SOUND_EVENTS] = {
    "Startup", "Shutdown", "Menu open", "Menu select", "Window open", "Window close",
    "Minimise", "Restore", "Error", "Information", "Question", "End Task", "Volume", "Empty recycle"
};
void sound_defaults(struct sound_scheme *s)
{
    memset(s, 0, sizeof(*s)); s->enabled = 1;
    for (unsigned i = 0; i < SOUND_EVENTS; i++) snprintf(s->file[i], SOUND_NAME_MAX, "%s.wav", sound_keys[i]);
}
static int name_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n) return 1; /* empty assignment disables one event */
    if (n >= SOUND_NAME_MAX || n < 5 || strcmp(s + n - 4, ".wav") || strstr(s, "..")) return 0;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
        (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-' || s[i] == '.')) return 0;
    return 1;
}
int sound_parse(struct sound_scheme *s, const char *text, size_t n)
{
    if (n > SOUND_TEXT_MAX || memchr(text, 0, n)) return -EINVAL;
    struct sound_scheme next = *s;
    for (size_t at = 0; at < n;) {
        size_t end = at; while (end < n && text[end] != '\n') end++;
        size_t a = at, b = end; at = end + 1;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
        while (b > a && (text[b-1] == ' ' || text[b-1] == '\t' || text[b-1] == '\r')) b--;
        if (a == b || text[a] == '#') continue;
        size_t eq = a; while (eq < b && text[eq] != '=') eq++;
        if (eq == b) return -EINVAL;
        size_t key_end = eq; while (key_end > a && (text[key_end-1] == ' ' || text[key_end-1] == '\t')) key_end--;
        size_t v = eq + 1; while (v < b && (text[v] == ' ' || text[v] == '\t')) v++;
        int id = -1;
        for (unsigned k = 0; k < SOUND_EVENTS; k++) if (key_end-a == strlen(sound_keys[k]) && !memcmp(text+a, sound_keys[k], key_end-a)) id = (int)k;
        if (key_end-a == 7 && !memcmp(text+a, "enabled", 7)) {
            if (b-v != 1 || (text[v] != '0' && text[v] != '1')) return -EINVAL;
            next.enabled = text[v] == '1';
        } else if (id >= 0) {
            if (b-v >= SOUND_NAME_MAX) return -EINVAL;
            char value[SOUND_NAME_MAX]; memcpy(value, text+v, b-v); value[b-v] = 0;
            if (!name_ok(value)) return -EINVAL;
            memcpy(next.file[id], value, b-v+1);
        } /* unknown keys reserved for future schemes */
    }
    *s = next; return 0;
}
size_t sound_format(const struct sound_scheme *s, char *out, size_t cap)
{
    size_t n = 0;
    int written = snprintf(out, cap, "enabled=%d\n", !!s->enabled);
    if (written < 0 || (size_t)written >= cap) return 0;
    n = (size_t)written;
    for (unsigned i = 0; i < SOUND_EVENTS; i++) {
        if (!name_ok(s->file[i])) return 0;
        written = snprintf(out+n, cap-n, "%s=%s\n", sound_keys[i], s->file[i]);
        if (written < 0 || (size_t)written >= cap-n) return 0;
        n += (size_t)written;
    }
    return n;
}
int sound_path(const struct sound_scheme *s, unsigned e, char *out, size_t cap)
{
    if (e >= SOUND_EVENTS || !name_ok(s->file[e])) return -EINVAL;
    if (!s->file[e][0]) return -ENOENT;
    int n = snprintf(out, cap, SOUND_DIR "/%s", s->file[e]);
    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}
static unsigned u16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p+2) << 16; }
int sound_wav(const void *data, size_t n, struct sound_clip *out)
{
    const uint8_t *p = data;
    *out = (struct sound_clip){0};
    if (n < 12 || n > SOUND_FILE_MAX || memcmp(p, "RIFF", 4) || memcmp(p+8, "WAVE", 4) || u32(p+4) != n-8) return -EINVAL;
    int fmt = 0; struct sound_clip clip = {0};
    for (size_t at = 12; at < n;) {
        if (n-at < 8) return -EINVAL;
        uint32_t len = u32(p+at+4); const uint8_t *body = p+at+8;
        if (len > n-at-8 || (len & 1u) > n-at-8-len) return -EINVAL;
        if (!memcmp(p+at, "fmt ", 4)) {
            if (fmt || len < 16 || u16(body) != 1 || u16(body+2) != 1 || u32(body+4) != SOUND_RATE ||
                u32(body+8) != SOUND_RATE*2 || u16(body+12) != 2 || u16(body+14) != 16) return -EINVAL;
            fmt = 1;
        } else if (!memcmp(p+at, "data", 4)) {
            if (clip.pcm || !len || len % 2 || len > SOUND_RATE*4) return -EINVAL;
            clip = (struct sound_clip){body, len/2};
        }
        at += 8u + (size_t)len + (len & 1u);
    }
    if (!fmt || !clip.pcm) return -EINVAL;
    *out = clip; return 0;
}
void sound_reset(struct sound_player *p) { memset(p, 0, sizeof(*p)); }
int sound_play(struct sound_player *p, struct sound_clip clip)
{
    if (!clip.pcm || !clip.frames || clip.frames > SOUND_RATE*2) return -EINVAL;
    if (p->error) return p->error;
    for (unsigned i = 0; i < 2; i++) if (!p->voice[i].clip.pcm) {
        p->voice[i] = (struct sound_voice){clip, 0}; return 0;
    }
    return -EBUSY; /* drop a third event; never create an unbounded backlog */
}
int sound_active(const struct sound_player *p)
{ return p->count > p->offset || p->voice[0].clip.pcm || p->voice[1].clip.pcm; }
int sound_pump(struct sound_player *p, int (*sink)(void *, const int16_t *, unsigned), void *ctx)
{
    if (p->error) return p->error;
    if (p->count == p->offset) {
        p->count = p->offset = 0;
        while (p->count < 256 && (p->voice[0].clip.pcm || p->voice[1].clip.pcm)) {
            int sum = 0;
            for (unsigned k = 0; k < 2; k++) {
                struct sound_voice *v = &p->voice[k];
                if (!v->clip.pcm) continue;
                unsigned sample = u16(v->clip.pcm + v->position++ * 2);
                sum += sample >= 32768 ? (int)sample - 65536 : (int)sample;
                if (v->position == v->clip.frames) v->clip.pcm = NULL;
            }
            p->pending[p->count++] = (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
        }
    }
    if (p->count == p->offset) return 0;
    int r = sink(ctx, p->pending + p->offset, p->count-p->offset);
    if (r == -EAGAIN || r == -EINTR) return 0;
    if (r < 0 || (unsigned)r > p->count-p->offset) { p->error = r < 0 ? r : -EIO; return p->error; }
    p->offset += (unsigned)r;
    return r;
}
void sound_tone(int16_t *pcm)
{
    static const int16_t sine[32] = {0,1598,3135,4551,5793,6811,7568,8035,8192,8035,7568,6811,5793,4551,3135,1598,
        0,-1598,-3135,-4551,-5793,-6811,-7568,-8035,-8192,-8035,-7568,-6811,-5793,-4551,-3135,-1598};
    unsigned phase = 0;
    for (unsigned i = 0; i < SOUND_RATE; i++) {
        unsigned slot = phase / SOUND_RATE, fraction = phase % SOUND_RATE;
        int sample = sine[slot] + (sine[(slot+1)%32]-sine[slot])*(int)fraction/(int)SOUND_RATE;
        unsigned edge = i < SOUND_RATE-1-i ? i : SOUND_RATE-1-i;
        if (edge < 110) sample = sample*(int)edge/110;
        pcm[i] = (int16_t)sample;
        phase = (phase + 32000) % (SOUND_RATE*32);
    }
}
