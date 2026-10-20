/* CD audio over the versioned F4 /dev/media boundary. MMC-3 packet details
 * remain in the kernel. See docs/validation/f5-12b2-cd-audio.md.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "audio.h"
#include "mci.h"
#include <ciuki/abi.h>
#include <w32/kernel32.h>
extern HANDLE W32API CreateThread(SECURITY_ATTRIBUTES *, DWORD, DWORD(W32API *)(void *), void *, DWORD, DWORD *);
extern BOOL W32API SetThreadPriority(HANDLE, int);
uint32_t W32API waveOutOpen(uintptr_t *, uint32_t, const struct wa_format *, uintptr_t, uintptr_t, uint32_t);
uint32_t W32API waveOutClose(uintptr_t);
uint32_t W32API waveOutPrepareHeader(uintptr_t, struct wa_header *, uint32_t);
uint32_t W32API waveOutUnprepareHeader(uintptr_t, struct wa_header *, uint32_t);
uint32_t W32API waveOutWrite(uintptr_t, struct wa_header *, uint32_t);
uint32_t W32API waveOutReset(uintptr_t);
uint32_t W32API waveOutPause(uintptr_t);
uint32_t W32API waveOutRestart(uintptr_t);
uint32_t W32API waveOutGetPosition(uintptr_t, void *, uint32_t);
#define CD_BUFFERS 4u
#define CD_BYTES (CIUKI_CD_READ_MAX_SECTORS * CIUKI_CD_SECTOR_BYTES)
/* Four owned headers form the reservoir: 108 sectors / 1.44 s, 254016 B.
 * An underrun leaves the mixer silent until the next header arrives, holds
 * the audible position, and never skips unread samples or repeats a read. */
struct cd {
  struct mci_device *device;
  const struct mci_host *host;
  int fd;
  uint32_t slot, generation, capabilities, forced, analog;
  struct ciuki_media_cd_toc toc;
  uint32_t format, mode, position, from, to, next, epoch, closing, refs;
  uint32_t poll_at, pending, present, batch;
  HANDLE reader;
  uintptr_t output;
  struct wa_header headers[CD_BUFFERS];
  uint8_t *buffers[CD_BUFFERS];
};
#define CD_SLOTS 4u /* F4 STORAGE_DISKS; slots are independent of drive letters */
static uint32_t owners[CD_SLOTS];
#ifdef W32_HOST_MODEL
int mci_cd_host_open(void);
void mci_cd_host_close(int);
int mci_cd_host_ctl(int, uint32_t, void *, uint32_t, void *);
uint32_t mci_cd_host_force(uint32_t);
void mci_cd_host_log(uint32_t, uint32_t, const char *);
static int media_open(void) { return mci_cd_host_open(); }
static void media_close(int fd) { mci_cd_host_close(fd); }
#else
static int native(uint32_t n, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
  int r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory", "cc");
  return r;
}
static int media_open(void) { return native(CIUKI_SYS_OPEN, (uintptr_t)"/dev/media", O_RDONLY | O_CLOEXEC, 0, 0); }
static void media_close(int fd) { native(CIUKI_SYS_CLOSE, fd, 0, 0, 0); }
#endif
static int ctl(struct cd *c, uint32_t op, void *p, uint32_t n, void *buffer) {
  uint32_t *h = p;
  h[0] = 1; h[1] = n; h[2] = c->slot;
#ifdef W32_HOST_MODEL
  return mci_cd_host_ctl(c->fd, op, p, n, buffer);
#else
  (void)buffer;
  return native(CIUKI_SYS_DEVCTL, c->fd, op, (uintptr_t)p, n);
#endif
}
static uint32_t map_error(int e) {
  if (e >= 0) return 0;
  switch (-e) {
  case EINVAL: return MCIERR_OUTOFRANGE;
  case ENODEV: return MCIERR_DEVICE_NOT_INSTALLED;
  case ENXIO: case EAGAIN: return MCIERR_DEVICE_NOT_READY;
  case ENOMEM: return MCIERR_OUT_OF_MEMORY;
  case EBUSY: return MCIERR_DEVICE_LOCKED;
  case EOPNOTSUPP: return MCIERR_UNSUPPORTED_FUNCTION;
  default: return MCIERR_HARDWARE;
  }
}
static void log_path(struct cd *c, const char *reason) {
#ifdef W32_HOST_MODEL
  mci_cd_host_log(c->slot, c->analog, reason);
#else
  /* A launch record uses the same native diagnostic sink as the desktop. */
  char slot[] = {(char)('0' + c->slot), 0};
  const char *path = c->analog ? "analog" : "digital";
  char out[128]; uint32_t n = 0;
  const char *parts[] = {"[winmm] cdaudio slot=", slot, " path=", path, " reason=", reason};
  for (unsigned i = 0; i < 6; i++) for (const char *s = parts[i]; *s && n + 1 < sizeof(out); s++) out[n++] = *s;
  out[n] = 0;
  native(CIUKI_SYS_DEBUG_WRITE, (uintptr_t)out, n, 0, 0);
#endif
}
static uint32_t force_path(uint32_t slot) {
#ifdef W32_HOST_MODEL
  return mci_cd_host_force(slot);
#else
  char key[] = "CD0"; key[2] += slot;
  /* system.ini [CiukiCDAudio]: CD0..CD3 = 0 auto, 1 digital, 2 analog. */
  uint32_t v = GetPrivateProfileIntA("CiukiCDAudio", key, 0, "system.ini");
  return v <= 2 ? v : 0;
#endif
}
static uint32_t track_index(struct cd *c, uint32_t position) {
  uint32_t i = 0;
  while (i + 1 < c->toc.count && position >= c->toc.tracks[i + 1].start_lba) i++;
  return i;
}
static int audio_range(struct cd *c, uint32_t from, uint32_t to) {
  if (from > to || from < c->toc.tracks[0].start_lba || to > c->toc.leadout_lba) return 0;
  for (uint32_t i = 0; i < c->toc.count; i++) {
    uint32_t end = i + 1 < c->toc.count ? c->toc.tracks[i + 1].start_lba : c->toc.leadout_lba;
    if (from < end && to > c->toc.tracks[i].start_lba && (c->toc.tracks[i].control & 4)) return 0;
  }
  return 1;
}
/* MCI MSF is absolute disc time, TMSF is track-relative. Track lengths
 * always return MSF when TMSF is selected (Quake's dwReturn << 8 idiom). */
static uint32_t packed_msf(uint32_t frames) {
  return frames / 4500 | ((frames / 75 % 60) << 8) | ((frames % 75) << 16);
}
static uint32_t from_lba(struct cd *c, uint32_t lba, int length, uint32_t *kind) {
  if (c->format == MCI_FORMAT_MILLISECONDS) return (uint32_t)((uint64_t)(lba + (length ? 0 : 150)) * 1000 / 75);
  *kind = MCI_COLONIZED3_RETURN;
  if (c->format == MCI_FORMAT_TMSF && !length) {
    uint32_t i = track_index(c, lba);
    *kind = MCI_COLONIZED4_RETURN;
    return c->toc.tracks[i].number | (packed_msf(lba - c->toc.tracks[i].start_lba) << 8);
  }
  return packed_msf(lba + (length ? 0 : 150));
}
static uint32_t to_lba(struct cd *c, uint32_t value, uint32_t *lba) {
  uint64_t frames;
  if (c->format == MCI_FORMAT_MILLISECONDS) frames = ((uint64_t)value * 75 + 999) / 1000;
  else {
    uint32_t base = 0;
    if (c->format == MCI_FORMAT_TMSF) {
      uint32_t track = value & 255;
      if (track < c->toc.first || track > c->toc.last) return MCIERR_OUTOFRANGE;
      base = c->toc.tracks[track - c->toc.first].start_lba;
      value >>= 8;
    } else if (value >> 24) return MCIERR_OUTOFRANGE;
    uint32_t m = value & 255, s = value >> 8 & 255, f = value >> 16 & 255;
    if (s >= 60 || f >= 75 || m > 99) return MCIERR_OUTOFRANGE;
    frames = (m * 60 + s) * 75 + f;
    if (c->format == MCI_FORMAT_TMSF) {
      uint32_t i = track_index(c, base);
      uint32_t end = i + 1 < c->toc.count ? c->toc.tracks[i + 1].start_lba : c->toc.leadout_lba;
      if (frames > end - base) return MCIERR_OUTOFRANGE;
      *lba = base + (uint32_t)frames; return 0;
    }
  }
  if (frames < 150 || frames - 150 > c->toc.leadout_lba) return MCIERR_OUTOFRANGE;
  *lba = (uint32_t)frames - 150; return 0;
}
static uint32_t current(struct cd *c) {
  if (c->output) {
    struct { uint32_t type, value[2]; } t = {2, {0, 0}};
    if (!waveOutGetPosition(c->output, &t, sizeof(t))) {
      uint32_t lba = c->from + t.value[0] / 588;
      c->position = lba > c->to ? c->to : lba;
    }
  }
  return c->position;
}
static int release_output(struct cd *c) {
  if (!c->output) return 0;
  if (waveOutReset(c->output)) return -EIO;
  for (unsigned i = 0; i < CD_BUFFERS; i++) {
    if ((c->headers[i].flags & WA_PREPARED) && waveOutUnprepareHeader(c->output, &c->headers[i], sizeof(c->headers[i]))) return -EIO;
    w32_fill(&c->headers[i], 0, sizeof(c->headers[i]));
  }
  if (waveOutClose(c->output)) return -EIO;
  c->output = 0; return 0;
}
static int analog_action(struct cd *c, uint32_t action) {
  struct ciuki_media_cd_pause r = {.generation = c->generation, .action = action};
  return ctl(c, MEDIA_CD_PAUSE, &r, sizeof(r), 0);
}
static int halt(struct cd *c) {
  current(c); c->epoch++;
  int e = 0;
  if (c->analog && c->mode != MCI_MODE_STOP) e = analog_action(c, CIUKI_CD_ACTION_STOP);
  int output_error = release_output(c); c->mode = MCI_MODE_STOP;
  if (c->device && c->device->busy) c->host->abort(c->device);
  return e ? e : output_error;
}
static int refresh(struct cd *c) {
  struct ciuki_media_cd_state s = {0};
  int e = ctl(c, MEDIA_CD_STATE, &s, sizeof(s), 0);
  c->present = !e && s.present && s.state == CIUKI_CD_READY;
  if (!c->present) return e ? e : -ENXIO;
  if (!c->toc.count || s.generation != c->generation) {
    if (c->mode != MCI_MODE_STOP) return -EAGAIN;
    struct ciuki_media_cd_toc t = {.generation = s.generation};
    e = ctl(c, MEDIA_CD_TOC, &t, sizeof(t), 0); if (e) return e;
    if (!t.count || t.count > 99 || t.first < 1 || t.last > 99 || t.count != t.last - t.first + 1) return -EIO;
    for (uint32_t i = 0; i < t.count; i++)
      if (t.tracks[i].number != t.first + i || t.tracks[i].start_lba >= t.leadout_lba ||
          (i && t.tracks[i].start_lba <= t.tracks[i-1].start_lba)) return -EIO;
    c->toc = t; c->generation = t.generation; c->position = t.tracks[0].start_lba;
  }
  return 0;
}
static void unref(struct cd *c) {
  if (--c->refs) return;
  media_close(c->fd);
  for (unsigned i = 0; i < CD_BUFFERS; i++) c->host->free(c->buffers[i]);
  c->host->free(c);
}
static void failure(struct cd *c) {
  current(c); release_output(c); c->mode = MCI_MODE_STOP; c->epoch++;
  c->toc.count = 0; c->present = 0;
  c->host->complete(c->device, MCI_NOTIFY_FAILURE);
}
static int start_analog(struct cd *c, uint32_t from, const char *reason) {
  if (!(c->capabilities & CIUKI_CD_CAP_AUDIO_PLAY)) return -EOPNOTSUPP;
  struct ciuki_media_cd_play r = {.generation = c->generation, .start_lba = from, .end_lba = c->to};
  int e = ctl(c, MEDIA_CD_PLAY, &r, sizeof(r), 0);
  if (!e) { c->analog = 1; c->position = from; log_path(c, reason); }
  return e;
}
/* Exactly one reader owns an in-flight buffer. IO runs outside the MCI lock.
 * Stop/seek/new play increment epoch; a late packet is discarded. Close
 * detaches the device, retaining the reader's reference until IO returns. */
static int reader_step(struct cd *c) {
  c->host->lock();
  if (c->closing) {
    if (release_output(c)) { c->host->unlock(); return 1; }
    const struct mci_host *h = c->host; unref(c); h->unlock(); return 0;
  }
  struct mci_device *d = c->device;
  if (c->mode == MCI_MODE_STOP) { (void)release_output(c); c->host->unlock(); return 1; }
  uint32_t epoch = c->epoch;
  if (GetTickCount() - c->poll_at >= 100) {
    c->poll_at = GetTickCount();
    c->host->unlock();
    struct ciuki_media_cd_state s = {0};
    int e = ctl(c, MEDIA_CD_STATE, &s, sizeof(s), 0);
    c->host->lock();
    if (c->closing || c->epoch != epoch) { c->host->unlock(); return 1; }
    if (e || s.generation != c->generation || s.state != CIUKI_CD_READY) {
      failure(c); c->host->unlock(); return 1;
    }
  }
  if (c->mode == MCI_MODE_PAUSE) { c->host->unlock(); return 1; }
  if (c->analog) {
    struct ciuki_media_cd_position p = {.generation = c->generation};
    c->host->unlock();
    int e = ctl(c, MEDIA_CD_POSITION, &p, sizeof(p), 0);
    c->host->lock();
    if (!c->closing && c->epoch == epoch) {
      if (e || p.audio_status == CIUKI_CD_AUDIO_ERROR) failure(c);
      else if (p.audio_status == CIUKI_CD_AUDIO_COMPLETED) {
        c->position = c->to; c->mode = MCI_MODE_STOP;
        c->host->complete(d, MCI_NOTIFY_SUCCESSFUL);
      } else if (p.audio_status == CIUKI_CD_AUDIO_PLAYING || p.audio_status == CIUKI_CD_AUDIO_PAUSED)
        c->position = p.absolute_lba;
    }
    c->host->unlock(); return 1;
  }
  unsigned outstanding = 0, slot = CD_BUFFERS;
  for (unsigned i = 0; i < CD_BUFFERS; i++) {
    struct wa_header *h = &c->headers[i];
    uint32_t flags = __atomic_load_n(&h->flags, __ATOMIC_ACQUIRE);
    if ((flags & WA_PREPARED) && !(flags & WA_QUEUED)) {
      waveOutUnprepareHeader(c->output, h, sizeof(*h)); h->flags = 0; flags = 0;
    }
    if (flags & WA_QUEUED) outstanding++;
    else if (slot == CD_BUFFERS) slot = i;
  }
  if (!outstanding && c->next >= c->to) {
    release_output(c); c->position = c->to; c->mode = MCI_MODE_STOP;
    c->host->complete(d, MCI_NOTIFY_SUCCESSFUL);
    c->host->unlock(); return 1;
  }
  if (slot == CD_BUFFERS || c->next >= c->to) { c->host->unlock(); return 1; }
  uint32_t sectors = c->to - c->next;
  if (sectors > c->batch) sectors = c->batch;
  /* Keep packets inside one track; some drives stop at audio transitions. */
  uint32_t ti = track_index(c, c->next);
  uint32_t end = ti + 1 < c->toc.count ? c->toc.tracks[ti + 1].start_lba : c->toc.leadout_lba;
  if (sectors > end - c->next) sectors = end - c->next;
  struct ciuki_media_cd_read r = {.generation = c->generation, .lba = c->next, .sectors = sectors,
                                 .buffer = (uint32_t)(uintptr_t)c->buffers[slot]};
  c->pending = 1; c->host->unlock();
  int e = ctl(c, MEDIA_CD_READ_AUDIO, &r, sizeof(r), c->buffers[slot]);
  c->host->lock(); c->pending = 0;
  if (c->closing || c->epoch != epoch) { c->host->unlock(); return 1; }
  if (!e && r.done != sectors * CIUKI_CD_SECTOR_BYTES) e = -EIO;
  if (e) {
    if (e == -ENOMEM && sectors > 1) {
      /* Allocation pressure is not evidence that the drive is incapable. */
      c->batch = sectors / 2;
    } else {
      uint32_t at = current(c);
      if (release_output(c) || c->forced == 1 || (e != -EIO && e != -EOPNOTSUPP) || start_analog(c, at, "digital-read-failed")) failure(c);
    }
    c->host->unlock(); return 1;
  }
  struct wa_header *h = &c->headers[slot];
  h->data = (char *)c->buffers[slot]; h->bytes = r.done;
  if (waveOutPrepareHeader(c->output, h, sizeof(*h)) || waveOutWrite(c->output, h, sizeof(*h))) failure(c);
  else c->next += sectors;
  c->host->unlock(); return 1;
}
#ifdef W32_HOST_MODEL
/* Deterministic host scheduler calls the same step with mock blocking IO. */
int mci_cd_reader_step(void *p) {
  return reader_step(p);
}
#endif
static DWORD W32API reader_main(void *p) {
  struct cd *c = p;
  while (reader_step(c)) Sleep(2);
  return 0;
}
static uint32_t cd_open(struct mci_device *d, uint32_t flags, struct mci_open *p) {
  (void)flags; (void)p;
  int fd = media_open(); if (fd < 0) return map_error(fd);
  struct cd *c = d->host->alloc(sizeof(*c));
  if (!c) { media_close(fd); return MCIERR_OUT_OF_MEMORY; }
  w32_fill(c, 0, sizeof(*c)); c->host = d->host; c->device = d; c->fd = fd; c->refs = 1;
  c->format = MCI_FORMAT_MSF; c->mode = MCI_MODE_STOP; c->batch = CIUKI_CD_READ_MAX_SECTORS;
  uint32_t error = MCIERR_DEVICE_NOT_INSTALLED;
  for (c->slot = 0; c->slot < CD_SLOTS; c->slot++) {
    struct ciuki_media_cd_state s = {0};
    if (ctl(c, MEDIA_CD_STATE, &s, sizeof(s), 0) || !s.present) continue;
    if (d->element[0]) {
      /* Private shell selector for audio-only discs without a drive letter. */
      if (d->element[0]=='@' && d->element[1]>='0' && (unsigned)(d->element[1]-'0')<CD_SLOTS && !d->element[2]) {
        if (c->slot != (uint32_t)(d->element[1]-'0')) continue;
      } else {
      unsigned ch = (uint8_t)d->element[0]; if (ch >= 'a' && ch <= 'z') ch -= 32;
      if (!d->element[1] || d->element[1] != ':' || d->element[2] || s.drive != ch - 'A') continue;
      }
    }
    if (owners[c->slot]) { error = MCIERR_DEVICE_OPEN; break; }
    struct ciuki_media_cd_info info = {0};
    int e = ctl(c, MEDIA_CD_INFO, &info, sizeof(info), 0);
    if (e) { error = map_error(e); break; }
    c->capabilities = info.capabilities; c->generation = s.generation;
    c->forced = force_path(c->slot);
    c->analog = c->forced == 2 || (!c->forced && (info.capabilities & CIUKI_CD_CAP_REPORTED) && !(info.capabilities & CIUKI_CD_CAP_CDDA_READ));
    /* Opening a drive with an empty tray is valid. */
    if (s.state == CIUKI_CD_READY && (e = refresh(c))) { error = map_error(e); break; }
    owners[c->slot] = d->id; d->data = c; return 0;
  }
  unref(c); return error;
}
static uint32_t start(struct cd *c, uint32_t from, uint32_t to) {
  c->from = c->position = c->next = from; c->to = to; c->epoch++;
  if (from == to) return 0;
  /* Establish cleanup ownership before opening a mixer source. Even a
   * quota-refused waveOut close keeps buffers alive until the reader can
   * finish cleanup after callbacks drain. */
  if (!c->reader) {
    c->refs++;
    c->reader = CreateThread(0, 65536, reader_main, c, 0, 0);
    if (!c->reader) { c->refs--; return MCIERR_OUT_OF_MEMORY; }
    SetThreadPriority(c->reader, 1);
  }
  if (c->analog) {
    int e = start_analog(c, from, c->forced == 2 ? "forced" : "no-cdda-capability");
    if (e) return map_error(e);
  } else {
    for (unsigned i = 0; i < CD_BUFFERS; i++) if (!c->buffers[i]) {
      c->buffers[i] = c->host->alloc(CD_BYTES);
      if (!c->buffers[i]) return MCIERR_OUT_OF_MEMORY;
    }
    const struct wa_format pcm = {1, 2, 44100, 176400, 4, 16, 0};
    if (waveOutOpen(&c->output, 0, &pcm, 0, 0, 0)) {
      (void)release_output(c); return MCIERR_WAVE_OUTPUTSUNSUITABLE;
    }
    log_path(c, c->forced == 1 ? "forced" : "primary");
  }
  c->mode = MCI_MODE_PLAY; c->host->begin(c->device); return MCI_ASYNC;
}
static uint32_t boolean(int value) { return MCI_MAKERESOURCE(value ? 1 : 0, value ? MCI_TRUE : MCI_FALSE); }
static uint32_t cd_command(struct mci_device *d, uint32_t msg, uint32_t flags, void *parms) {
  struct cd *c = d->data; uint32_t error;
  if (msg == MCI_SET && (flags & (MCI_SET_DOOR_OPEN | MCI_SET_DOOR_CLOSED))) {
    if ((flags & MCI_SET_DOOR_OPEN) && (flags & MCI_SET_DOOR_CLOSED)) return MCIERR_FLAGS_NOT_COMPATIBLE;
    if ((flags & MCI_SET_DOOR_OPEN) && c->mode != MCI_MODE_STOP) return MCIERR_DEVICE_LOCKED;
    struct ciuki_media_cd_tray r = {.action = flags & MCI_SET_DOOR_OPEN ? CIUKI_CD_TRAY_OPEN : CIUKI_CD_TRAY_CLOSE};
    int e = ctl(c, MEDIA_CD_TRAY, &r, sizeof(r), 0);
    if (!e) { c->toc.count = 0; c->present = 0; }
    return map_error(e);
  }
  if (msg == MCI_PLAY || msg == MCI_SEEK) {
    int e = refresh(c); if (e) return map_error(e);
  }
  switch (msg) {
  case MCI_PLAY: {
    struct mci_play *p = parms;
    if ((flags & (MCI_FROM | MCI_TO)) && !p) return MCIERR_NULL_PARAMETER_BLOCK;
    uint32_t from = current(c), to = c->toc.leadout_lba;
    if ((flags & MCI_FROM) && (error = to_lba(c, p->from, &from))) return error;
    if ((flags & MCI_TO) && (error = to_lba(c, p->to, &to))) return error;
    if (!audio_range(c, from, to)) return MCIERR_OUTOFRANGE;
    if (c->mode != MCI_MODE_STOP && !(flags & MCI_FROM) && to == c->to) {
      if (c->mode == MCI_MODE_PAUSE) {
        int e = c->analog ? analog_action(c, CIUKI_CD_ACTION_RESUME) : (int)waveOutRestart(c->output);
        if (e) return e < 0 ? map_error(e) : MCIERR_HARDWARE;
      }
      c->mode = MCI_MODE_PLAY; return MCI_ASYNC;
    }
    int e = halt(c); return e ? map_error(e) : start(c, from, to);
  }
  case MCI_STOP: return map_error(halt(c));
  case MCI_PAUSE: case MCI_RESUME: {
    int pause = msg == MCI_PAUSE;
    if ((pause && c->mode == MCI_MODE_PLAY) || (!pause && c->mode == MCI_MODE_PAUSE)) {
      int e = c->analog ? analog_action(c, pause ? CIUKI_CD_ACTION_PAUSE : CIUKI_CD_ACTION_RESUME) :
                         (int)(pause ? waveOutPause(c->output) : waveOutRestart(c->output));
      if (e) return e < 0 ? map_error(e) : MCIERR_HARDWARE;
      c->mode = pause ? MCI_MODE_PAUSE : MCI_MODE_PLAY;
    }
    return 0;
  }
  case MCI_SEEK: {
    unsigned n = !!(flags & MCI_TO) + !!(flags & MCI_SEEK_TO_START) + !!(flags & MCI_SEEK_TO_END);
    if (n != 1) return n ? MCIERR_FLAGS_NOT_COMPATIBLE : MCIERR_MISSING_PARAMETER;
    uint32_t to = flags & MCI_SEEK_TO_END ? c->toc.leadout_lba : c->toc.tracks[0].start_lba;
    if (flags & MCI_TO) {
      if (!parms) return MCIERR_NULL_PARAMETER_BLOCK;
      if ((error = to_lba(c, ((struct mci_seek *)parms)->to, &to))) return error;
    }
    int e = halt(c); c->position = to; return map_error(e);
  }
  case MCI_SET: {
    struct mci_set *p = parms;
    if (flags & ~ (MCI_SET_TIME_FORMAT | MCI_NOTIFY | MCI_WAIT)) return MCIERR_UNSUPPORTED_FUNCTION;
    if (flags & MCI_SET_TIME_FORMAT) {
      if (!p) return MCIERR_NULL_PARAMETER_BLOCK;
      if (p->time_format != MCI_FORMAT_MSF && p->time_format != MCI_FORMAT_TMSF && p->time_format != MCI_FORMAT_MILLISECONDS) return MCIERR_BAD_TIME_FORMAT;
      c->format = p->time_format;
    }
    return 0;
  }
  case MCI_STATUS: {
    struct mci_status *p = parms;
    if (!p) return MCIERR_NULL_PARAMETER_BLOCK;
    if (!(flags & MCI_STATUS_ITEM)) return MCIERR_MISSING_PARAMETER;
    int e = refresh(c);
    if (e && c->mode != MCI_MODE_STOP) failure(c);
    uint32_t kind = 0, i = 0;
    if (flags & MCI_TRACK) {
      if (e) return map_error(e);
      if (p->track < c->toc.first || p->track > c->toc.last) return MCIERR_OUTOFRANGE;
      i = p->track - c->toc.first;
    }
    switch (p->item) {
    case MCI_STATUS_MODE: {
      uint32_t mode = e ? MCI_MODE_NOT_READY : c->mode;
      p->value = MCI_MAKERESOURCE(mode, mode); return MCI_RESOURCE_RETURNED;
    }
    case MCI_STATUS_READY: case MCI_STATUS_MEDIA_PRESENT:
      p->value = boolean(!e); return MCI_RESOURCE_RETURNED;
    case MCI_STATUS_TIME_FORMAT:
      p->value = MCI_MAKERESOURCE(c->format, MCI_FORMAT_RETURN_BASE + c->format); return MCI_RESOURCE_RETURNED;
    default: if (e) return map_error(e);
    }
    switch (p->item) {
    case MCI_STATUS_LENGTH: {
      uint32_t n = c->toc.leadout_lba - c->toc.tracks[0].start_lba;
      if (flags & MCI_TRACK) n = (i + 1 < c->toc.count ? c->toc.tracks[i+1].start_lba : c->toc.leadout_lba) - c->toc.tracks[i].start_lba;
      p->value = from_lba(c, n, 1, &kind); break;
    }
    case MCI_STATUS_POSITION:
      p->value = from_lba(c, flags & MCI_TRACK ? c->toc.tracks[i].start_lba :
                              flags & MCI_STATUS_START ? c->toc.tracks[0].start_lba : current(c), 0, &kind); break;
    case MCI_STATUS_NUMBER_OF_TRACKS: p->value = c->toc.count; break;
    case MCI_STATUS_CURRENT_TRACK: p->value = c->toc.tracks[track_index(c, current(c))].number; break;
    case MCI_CDA_STATUS_TYPE_TRACK:
      if (!(flags & MCI_TRACK)) return MCIERR_MISSING_PARAMETER;
      p->value = c->toc.tracks[i].control & 4 ? MCI_CDA_TRACK_OTHER : MCI_CDA_TRACK_AUDIO;
      p->value = MCI_MAKERESOURCE(p->value, p->value); return MCI_RESOURCE_RETURNED;
    default: return MCIERR_UNSUPPORTED_FUNCTION;
    }
    return kind;
  }
  case MCI_GETDEVCAPS: {
    struct mci_caps *p = parms;
    if (!p) return MCIERR_NULL_PARAMETER_BLOCK;
    if (!(flags & MCI_GETDEVCAPS_ITEM)) return MCIERR_MISSING_PARAMETER;
    if (p->item == MCI_GETDEVCAPS_DEVICE_TYPE) { p->value = MCI_MAKERESOURCE(MCI_DEVTYPE_CD_AUDIO, MCI_DEVTYPE_CD_AUDIO); return MCI_RESOURCE_RETURNED; }
    switch (p->item) {
    case MCI_GETDEVCAPS_HAS_AUDIO: case MCI_GETDEVCAPS_CAN_PLAY: p->value = boolean(1); break;
    case MCI_GETDEVCAPS_CAN_EJECT: p->value = boolean(c->capabilities & CIUKI_CD_CAP_EJECT); break;
    case MCI_GETDEVCAPS_CAN_RECORD: case MCI_GETDEVCAPS_HAS_VIDEO: case MCI_GETDEVCAPS_USES_FILES:
    case MCI_GETDEVCAPS_COMPOUND_DEVICE: case MCI_GETDEVCAPS_CAN_SAVE: p->value = boolean(0); break;
    default: return MCIERR_UNSUPPORTED_FUNCTION;
    }
    return MCI_RESOURCE_RETURNED;
  }
  case MCI_INFO: {
    struct mci_info *p = parms;
    if (!p || !p->text) return MCIERR_NULL_PARAMETER_BLOCK;
    if (!(flags & MCI_INFO_PRODUCT)) return MCIERR_NO_IDENTITY;
    const char *s = "CiukiOS CD Audio";
    if (p->size <= w32_strlen(s)) return MCIERR_PARAM_OVERFLOW;
    w32_copy((void *)p->text, s, w32_strlen(s) + 1); return 0;
  }
  default: return MCIERR_UNSUPPORTED_FUNCTION;
  }
}
static void cd_close(struct mci_device *d) {
  struct cd *c = d->data;
  halt(c); c->closing = 1; c->device = 0;
  owners[c->slot] = 0;
  if (c->reader) CloseHandle(c->reader);
  unref(c); d->data = 0;
}
const struct mci_driver *mci_cd_driver(void) {
  static const struct mci_driver driver = {1, "cdaudio", MCI_DEVTYPE_CD_AUDIO, cd_open, cd_command, cd_close, 0};
  return &driver;
}
