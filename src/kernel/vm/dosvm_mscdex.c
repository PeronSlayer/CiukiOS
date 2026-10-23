/* fx-10 MSCDEX 2.x model over a virtual audio disc (dosvm_mscdex.h).
 * Request layouts and the PLAY/STOP/RESUME pseudocode follow MSCDEX21.DOC;
 * the head position is derived from the clock at 75 sectors per second.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dosvm_mscdex.h"

#define CF 1u

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint32_t linear(uint16_t seg, uint16_t off) { return (uint32_t)seg * 16u + off; }

uint32_t mscdex_redbook(uint32_t lba)
{
    uint32_t a = lba + MSCDEX_PREGAP;
    return (a / 4500u) << 16 | (a / 75u % 60u) << 8 | a % 75u;
}
uint32_t mscdex_hsg(uint32_t rb)
{
    uint32_t min = rb >> 16 & 0xFF, sec = rb >> 8 & 0xFF, frame = rb & 0xFF;
    if (rb >> 24 || sec > 59 || frame > 74) return UINT32_MAX;
    uint32_t a = min * 4500u + sec * 75u + frame;
    return a < MSCDEX_PREGAP ? UINT32_MAX : a - MSCDEX_PREGAP;
}
unsigned mscdex_track_at(const struct mscdex_disc *d, uint32_t lba)
{
    if (!d->present) return 0;
    for (unsigned t = d->first; t && t <= d->last; t++)
        if (lba >= d->track[t].lba && lba - d->track[t].lba < d->track[t].sectors) return t;
    return 0;
}
int mscdex_disc_add(struct mscdex_disc *d, uint32_t sectors, bool data)
{
    unsigned n = d->present ? d->last + 1u : 1u;
    if (n > MSCDEX_TRACKS || !sectors || sectors > 100u * 60u * 75u) return -EINVAL;
    uint32_t lba = d->present ? d->leadout : 0;
    if (d->present && (d->track[d->last].control & 0x40)) lba += MSCDEX_PREGAP;
    d->track[n] = (struct mscdex_track){lba, sectors, data ? MSCDEX_CONTROL_DATA : MSCDEX_CONTROL_AUDIO};
    if (!d->present) d->first = 1;
    d->present = true; d->last = (uint8_t)n; d->leadout = lba + sectors;
    return 0;
}

int mscdex_wav_parse(const uint8_t *h, unsigned bytes, uint32_t file_bytes,
                     struct mscdex_wav *out, const char **reason)
{
    *reason = "header";
    if (bytes < 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -EINVAL;
    bool format = false;
    for (unsigned at = 12; at + 8 <= bytes;) {
        uint32_t size = get32(h + at + 4), body = at + 8;
        if (!memcmp(h + at, "fmt ", 4)) {
            if (size < 16 || body + 16 > bytes) return -EINVAL;
            *reason = "format";
            if (get16(h + body) != 1) return -EINVAL;                 /* PCM */
            *reason = "channels";
            if (get16(h + body + 2) != 2) return -EINVAL;
            *reason = "rate";
            if (get32(h + body + 4) != 44100u) return -EINVAL;
            *reason = "bits";
            if (get16(h + body + 14) != 16 || get16(h + body + 12) != 4) return -EINVAL;
            format = true; *reason = "header";
        } else if (!memcmp(h + at, "data", 4)) {
            if (!format) return -EINVAL;
            *reason = "empty";
            if (body >= file_bytes || !size) return -EINVAL;
            if (size > file_bytes - body) size = file_bytes - body;    /* truncated rip */
            size &= ~3u;
            if (!size) return -EINVAL;
            *out = (struct mscdex_wav){body, size};
            *reason = 0; return 0;
        }
        if (size > bytes) break;                                       /* data beyond the header window */
        at = body + size + (size & 1u);
    }
    return -EINVAL;
}

static const uint16_t attenuation[32] = { /* permille of 10^(-k/10), k = 31 - level */
    1000, 794, 631, 501, 398, 316, 251, 200, 158, 126, 100, 79, 63, 50, 40, 32,
    25, 20, 16, 13, 10, 8, 6, 5, 4, 3, 3, 2, 2, 1, 1, 1};
unsigned mscdex_gain(uint8_t volume, uint8_t cd_level, uint8_t master_level)
{
    unsigned g = volume * 100u / 255u;
    g = (g * attenuation[31u - (cd_level & 31u)] + 500u) / 1000u;
    return (g * attenuation[31u - (master_level & 31u)] + 500u) / 1000u;
}

static void event(struct mscdex *m, unsigned kind, uint32_t a, uint32_t b, uint32_t c)
{
    if (m->ops.event) m->ops.event(m->ops.ctx, kind, a, b, c);
}
static void backend(struct mscdex *m, bool play, uint32_t lba, uint32_t sectors)
{
    if (m->ops.audio) m->ops.audio(m->ops.ctx, play, lba, sectors);
}
void mscdex_init(struct mscdex *m, const struct mscdex_ops *ops, uint8_t drive)
{
    /* The disc (TOC) is published separately and kept. */
    m->ops = *ops; m->drive = drive;
    m->playing = m->paused = m->door_open = m->locked = m->changed = m->installed = false;
    m->start = m->end = m->play_from = m->play_ms = m->head = 0;
    m->requests = m->plays = m->stops = m->resumes = m->completions = m->refused = 0;
    for (unsigned i = 0; i < 4; i++) { m->channel[i][0] = (uint8_t)i; m->channel[i][1] = 0xFF; }
}
static bool ready(const struct mscdex *m) { return m->disc.present && !m->door_open; }
void mscdex_media_changed(struct mscdex *m, bool changed)
{
    m->playing = m->paused = false; m->start = m->end = m->head = 0;
    if (changed) m->changed = true;
}
uint32_t mscdex_position(struct mscdex *m)
{
    if (m->paused) return m->start;
    if (!m->playing) return m->head;
    uint32_t elapsed = m->ops.now_ms(m->ops.ctx) - m->play_ms;
    if (elapsed > 0x0FFFFFFFu / MSCDEX_SECTORS_PER_SECOND) elapsed = 0x0FFFFFFFu / MSCDEX_SECTORS_PER_SECOND;
    uint32_t sectors = elapsed * MSCDEX_SECTORS_PER_SECOND / 1000u;
    if (sectors < m->end - m->play_from) return m->play_from + sectors;
    /* PLAY/RESUME COMPLETED: playing, paused and the locations are cleared. */
    m->head = m->end; m->playing = m->paused = false; m->start = m->end = 0;
    m->completions++; event(m, MSCDEX_EV_COMPLETE, m->head, 0, 0);
    return m->head;
}
static void stop_reset(struct mscdex *m)
{
    if (m->playing) { m->head = mscdex_position(m); if (m->playing) backend(m, false, 0, 0); }
    m->playing = m->paused = false; m->start = m->end = 0;
}
static void volume(struct mscdex *m)
{
    if (!m->ops.volume) return;
    /* Outputs 0/1 are left/right; an input above 1 counts as volume 0. */
    m->ops.volume(m->ops.ctx, m->channel[0][0] < 2 ? m->channel[0][1] : 0,
                  m->channel[1][0] < 2 ? m->channel[1][1] : 0);
}

/* Returns the device error code, 0 on success. */
static unsigned ioctl_input(struct mscdex *m, uint32_t at, uint16_t count)
{
    uint8_t b[11] = {0};
    if (!count || m->ops.read(m->ops.ctx, at, b, 1)) return MSCDEX_ERR_GENERAL;
    unsigned code = b[0], length;
    switch (code) {
    case 1: {                                            /* location of head */
        if (m->ops.read(m->ops.ctx, at + 1, b + 1, 1)) return MSCDEX_ERR_GENERAL;
        if (b[1] > 1) return MSCDEX_ERR_GENERAL;
        uint32_t lba = mscdex_position(m);
        put32(b + 2, b[1] ? mscdex_redbook(lba) : lba); length = 6; break;
    }
    case 4:                                              /* audio channel info */
        for (unsigned i = 0; i < 4; i++) { b[1 + i * 2] = m->channel[i][0]; b[2 + i * 2] = m->channel[i][1]; }
        length = 9; break;
    case 6: {                                            /* device status */
        uint32_t s = MSCDEX_DEV_RAW | MSCDEX_DEV_AUDIO | MSCDEX_DEV_CHANNELS | MSCDEX_DEV_REDBOOK;
        (void)mscdex_position(m);
        if (m->door_open) s |= MSCDEX_DEV_DOOR_OPEN;
        if (!m->locked) s |= MSCDEX_DEV_UNLOCKED;
        if (m->playing) s |= MSCDEX_DEV_PLAYING;
        if (!ready(m)) s |= MSCDEX_DEV_NO_DISC;
        put32(b + 1, s); length = 5; break;
    }
    case 7:                                              /* sector size */
        if (m->ops.read(m->ops.ctx, at + 1, b + 1, 1)) return MSCDEX_ERR_GENERAL;
        if (b[1] > 1) return MSCDEX_ERR_GENERAL;
        put16(b + 2, b[1] ? MSCDEX_SECTOR_BYTES : 2048u); length = 4; break;
    case 8:                                              /* volume size */
        if (!ready(m)) return MSCDEX_ERR_NOT_READY;
        put32(b + 1, m->disc.leadout + MSCDEX_PREGAP); length = 5; break;
    case 9:                                              /* media changed */
        b[1] = m->changed ? 0xFF : 1; m->changed = false; length = 2; break;
    case 10:                                             /* audio disk info */
        if (!ready(m)) return MSCDEX_ERR_NOT_READY;
        b[1] = m->disc.first; b[2] = m->disc.last; put32(b + 3, mscdex_redbook(m->disc.leadout));
        length = 7; break;
    case 11: {                                           /* audio track info */
        if (!ready(m)) return MSCDEX_ERR_NOT_READY;
        if (m->ops.read(m->ops.ctx, at + 1, b + 1, 1)) return MSCDEX_ERR_GENERAL;
        unsigned t = b[1];
        if (t < m->disc.first || t > m->disc.last) return MSCDEX_ERR_SECTOR_NOT_FOUND;
        put32(b + 2, mscdex_redbook(m->disc.track[t].lba)); b[6] = m->disc.track[t].control;
        length = 7; break;
    }
    case 12: {                                           /* audio Q-channel info */
        if (!ready(m)) return MSCDEX_ERR_NOT_READY;
        uint32_t lba = mscdex_position(m);
        unsigned t = mscdex_track_at(&m->disc, lba);
        if (!t) t = m->disc.last;
        uint32_t rel = lba >= m->disc.track[t].lba ? lba - m->disc.track[t].lba : 0;
        uint32_t abs = mscdex_redbook(lba);
        b[1] = m->disc.track[t].control; b[2] = (uint8_t)t; b[3] = 1;
        b[4] = (uint8_t)(rel / 4500u); b[5] = (uint8_t)(rel / 75u % 60u); b[6] = (uint8_t)(rel % 75u); b[7] = 0;
        b[8] = (uint8_t)(abs >> 16); b[9] = (uint8_t)(abs >> 8); b[10] = (uint8_t)abs;
        length = 11; break;
    }
    case 15:                                             /* audio status info */
        (void)mscdex_position(m);
        put16(b + 1, m->paused ? 1 : 0);
        put32(b + 3, mscdex_redbook(m->start)); put32(b + 7, mscdex_redbook(m->end));
        length = 11; break;
    default:
        return MSCDEX_ERR_UNKNOWN_COMMAND;
    }
    if (length > count) length = count;
    return m->ops.write(m->ops.ctx, at, b, length) ? MSCDEX_ERR_GENERAL : 0;
}
static unsigned ioctl_output(struct mscdex *m, uint32_t at, uint16_t count)
{
    uint8_t b[9] = {0};
    if (!count || m->ops.read(m->ops.ctx, at, b, count < sizeof(b) ? count : sizeof(b))) return MSCDEX_ERR_GENERAL;
    switch (b[0]) {
    case 0:                                              /* eject */
        stop_reset(m); m->door_open = true; m->locked = false; m->changed = true; return 0;
    case 1:                                              /* lock/unlock door */
        if (count < 2) return MSCDEX_ERR_GENERAL;
        m->locked = b[1] != 0; return 0;
    case 2:                                              /* reset drive */
        stop_reset(m); return 0;
    case 3:                                              /* audio channel control */
        if (count < 9) return MSCDEX_ERR_GENERAL;
        for (unsigned i = 0; i < 4; i++) { m->channel[i][0] = b[1 + i * 2] & 3; m->channel[i][1] = b[2 + i * 2]; }
        volume(m); return 0;
    case 5:                                              /* close tray */
        if (m->door_open) { m->door_open = false; m->changed = true; }
        return 0;
    default:
        return MSCDEX_ERR_UNKNOWN_COMMAND;
    }
}
static unsigned play(struct mscdex *m, const uint8_t *h)
{
    if (!ready(m)) return MSCDEX_ERR_NOT_READY;
    uint32_t start = get32(h + 14), count = get32(h + 18);
    if (h[13] > 1) return MSCDEX_ERR_GENERAL;
    if (h[13] == 1) start = mscdex_hsg(start);
    if (start == UINT32_MAX || start >= m->disc.leadout) return MSCDEX_ERR_SECTOR_NOT_FOUND;
    unsigned t = mscdex_track_at(&m->disc, start);
    if (!t || (m->disc.track[t].control & 0x40)) return MSCDEX_ERR_GENERAL;  /* data or pregap */
    if (count > m->disc.leadout - start) count = m->disc.leadout - start;
    stop_reset(m);
    if (!count) { m->head = start; return 0; }
    m->playing = true; m->paused = false;
    m->start = start; m->end = start + count;
    m->play_from = start; m->play_ms = m->ops.now_ms(m->ops.ctx);
    m->plays++;
    backend(m, true, start, count);
    event(m, MSCDEX_EV_PLAY, t, start, count);
    return 0;
}
static unsigned stop(struct mscdex *m)
{
    if (m->playing) {
        uint32_t at = mscdex_position(m);
        if (m->playing) {
            m->start = at; m->playing = false; m->paused = true; m->head = at;
            m->stops++;
            backend(m, false, 0, 0);
            event(m, MSCDEX_EV_STOP, at, 1, 0);
            return 0;
        }
    }
    if (m->paused) event(m, MSCDEX_EV_STOP, m->start, 0, 0);
    m->playing = m->paused = false; m->start = m->end = 0;
    return 0;
}
static unsigned resume(struct mscdex *m)
{
    if (!m->paused) return MSCDEX_ERR_GENERAL;
    if (!ready(m)) return MSCDEX_ERR_NOT_READY;
    m->playing = true; m->paused = false;
    m->play_from = m->start; m->play_ms = m->ops.now_ms(m->ops.ctx);
    m->resumes++;
    backend(m, true, m->start, m->end - m->start);
    event(m, MSCDEX_EV_RESUME, m->start, m->end - m->start, 0);
    return 0;
}
static void request(struct mscdex *m, struct mscdex_regs *r)
{
    uint32_t at = linear(r->es, r->bx);
    uint8_t h[22] = {0};
    if (m->ops.read(m->ops.ctx, at, h, 13)) { r->flags |= CF; r->ax = 0x000F; return; }
    unsigned command = h[2], code = 0, error;
    m->requests++;
    if (h[1]) error = MSCDEX_ERR_UNKNOWN_UNIT;
    else switch (command) {
    case MSCDEX_CMD_IOCTL_INPUT: case MSCDEX_CMD_IOCTL_OUTPUT: {
        if (m->ops.read(m->ops.ctx, at + 13, h + 13, 7)) { error = MSCDEX_ERR_GENERAL; break; }
        uint32_t control = linear(get16(h + 16), get16(h + 14));
        uint8_t first = 0;
        (void)m->ops.read(m->ops.ctx, control, &first, 1);
        code = first;
        error = command == MSCDEX_CMD_IOCTL_INPUT ? ioctl_input(m, control, get16(h + 18))
                                                  : ioctl_output(m, control, get16(h + 18));
        break;
    }
    case MSCDEX_CMD_PLAY:
        error = m->ops.read(m->ops.ctx, at + 13, h + 13, 9) ? MSCDEX_ERR_GENERAL : play(m, h);
        break;
    case MSCDEX_CMD_STOP: error = stop(m); break;
    case MSCDEX_CMD_RESUME: error = resume(m); break;
    case MSCDEX_CMD_INPUT_FLUSH: case MSCDEX_CMD_OUTPUT_FLUSH:
    case MSCDEX_CMD_OPEN: case MSCDEX_CMD_CLOSE: case MSCDEX_CMD_SEEK:
        error = 0; break;
    case MSCDEX_CMD_READ_LONG: case MSCDEX_CMD_READ_PREFETCH:
        /* An audio-only disc has no data sectors to read. */
        error = ready(m) ? MSCDEX_ERR_SECTOR_NOT_FOUND : MSCDEX_ERR_NOT_READY; break;
    default:
        error = MSCDEX_ERR_UNKNOWN_COMMAND; break;
    }
    (void)mscdex_position(m);
    uint16_t status = MSCDEX_ST_DONE | (m->playing ? MSCDEX_ST_BUSY : 0);
    if (error) {
        status |= MSCDEX_ST_ERROR | error;
        m->refused++; event(m, MSCDEX_EV_REFUSED, command, code, error);
    }
    uint8_t word[2]; put16(word, status);
    if (m->ops.write(m->ops.ctx, at + 3, word, 2)) { r->flags |= CF; r->ax = 0x000F; return; }
    r->flags &= ~CF;
}

int mscdex_interrupt(struct mscdex *m, struct mscdex_regs *r)
{
    if ((r->ax & 0xFF00) != 0x1500) return -ENOSYS;
    bool ours = r->cx == m->drive;
    if (!m->installed) { m->installed = true; event(m, MSCDEX_EV_INSTALL, m->drive, ready(m), 0); }
    switch (r->ax & 0xFF) {
    case 0x00: r->bx = 1; r->cx = m->drive; return 0;
    case 0x0B: r->bx = 0xADAD; r->ax = ours ? 1 : 0; return 0;
    case 0x0C: r->bx = MSCDEX_VERSION; return 0;
    case 0x0D: {
        uint8_t letter = m->drive;
        if (m->ops.write(m->ops.ctx, linear(r->es, r->bx), &letter, 1)) r->flags |= CF; else r->flags &= ~CF;
        return 0;
    }
    case 0x02: case 0x03: case 0x04: {                    /* no VTOC file names on an audio disc */
        if (!ours) { r->flags |= CF; r->ax = 0x000F; return 0; }
        uint8_t nul = 0;
        if (m->ops.write(m->ops.ctx, linear(r->es, r->bx), &nul, 1)) { r->flags |= CF; r->ax = 0x000F; }
        else r->flags &= ~CF;
        return 0;
    }
    case 0x05: case 0x08: case 0x09:                      /* VTOC and sector I/O: no data session */
        r->flags |= CF; r->ax = ours ? 0x0015 : 0x000F; return 0;
    case 0x06: case 0x07: return 0;                       /* debugging: non-functional in production */
    case 0x10:
        if (!ours) { r->flags |= CF; r->ax = 0x000F; return 0; }
        request(m, r); return 0;
    default:
        /* 1501h (device header list) is not offered: there is no guest-resident driver. */
        return -ENOSYS;
    }
}
