/* Scripted /dev/media model. No register protocol is modelled here: the
 * production kernel packet CDBs are checked separately by ata_test.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "mci_host.h"
#include <ciuki/abi.h>
#ifndef MCI_TEST_CD
int mci_cd_host_open(void) { return -ENODEV; }
void mci_cd_host_close(int fd) { (void)fd; }
int mci_cd_host_ctl(int fd, uint32_t op, void *p, uint32_t n, void *b) {
  (void)fd; (void)op; (void)p; (void)n; (void)b; return -ENODEV;
}
uint32_t mci_cd_host_force(uint32_t slot) { (void)slot; return 0; }
void mci_cd_host_log(uint32_t slot, uint32_t analog, const char *reason) {
  (void)slot; (void)analog; (void)reason;
}
#else
#include "cd_model.h"
struct cd_model cd_model;
static void *readers[32];
static uint8_t *disc;
static uint32_t disc_bytes;
int mci_cd_reader_step(void *);
int mci_test_lock_depth(void);
void cd_reader_add(void *p) {
  for (unsigned i = 0; i < 32; i++) if (!readers[i]) { readers[i] = p; return; }
  assert(0);
}
void cd_tick(void) {
  if (mci_test_lock_depth()) return;
  for (unsigned i = 0; i < 32; i++) if (readers[i] && !mci_cd_reader_step(readers[i])) readers[i] = 0;
}
void cd_load(const char *name) {
  assert(!cd_model.handles);
  free(disc); disc = 0;
  memset(&cd_model, 0, sizeof(cd_model));
  char path[200]; snprintf(path, sizeof(path), "%s.toc", name);
  int f = hf_open(path); assert(f >= 0);
  assert(hf_read(f, &cd_model.toc, sizeof(cd_model.toc)) == sizeof(cd_model.toc)); hf_close(f);
  snprintf(path, sizeof(path), "%s.raw", name); f = hf_open(path); assert(f >= 0);
  disc_bytes = (uint32_t)hf_size(f); disc = malloc(disc_bytes); assert(disc);
  assert(hf_read(f, disc, disc_bytes) == disc_bytes); hf_close(f);
  cd_model.generation = 1; cd_model.present = 1; cd_model.drive = 3;
  cd_model.caps = CIUKI_CD_CAP_REPORTED | CIUKI_CD_CAP_CDDA_READ | CIUKI_CD_CAP_CDDA_ACCURATE | CIUKI_CD_CAP_AUDIO_PLAY | CIUKI_CD_CAP_EJECT;
}
void cd_destroy(void) {
  cd_tick();
  for (unsigned i = 0; i < 32; i++) assert(!readers[i]);
  assert(!cd_model.handles); free(disc); disc = 0;
}
int mci_cd_host_open(void) { cd_model.handles++; return 80; }
void mci_cd_host_close(int fd) { assert(fd == 80 && cd_model.handles); cd_model.handles--; }
uint32_t mci_cd_host_force(uint32_t slot) { assert(slot == 1); return cd_model.force; }
void mci_cd_host_log(uint32_t slot, uint32_t analog, const char *reason) {
  out("CD slot=%u path=%s reason=%s\n", slot, analog ? "analog" : "digital", reason);
  cd_model.analog = analog;
}
int mci_cd_host_ctl(int fd, uint32_t op, void *data, uint32_t bytes, void *buffer) {
  assert(fd == 80); uint32_t *h = data;
  assert(h[0] == 1 && h[1] == bytes && h[2] < 4);
  if (op == MEDIA_CD_STATE) {
    struct ciuki_media_cd_state *s = data;
    assert(bytes == sizeof(*s));
    s->present = h[2] == 1; s->generation = cd_model.generation;
    s->state = cd_model.present ? CIUKI_CD_READY : CIUKI_CD_NO_MEDIUM;
    s->drive = cd_model.drive; return 0;
  }
  assert(h[2] == 1);
  if (op == MEDIA_CD_INFO) {
    struct ciuki_media_cd_info *p = data; assert(bytes == sizeof(*p));
    p->capabilities = cd_model.caps; p->generation = cd_model.generation; p->max_read_sectors = 27; return 0;
  }
  if (op == MEDIA_CD_TRAY) {
    struct ciuki_media_cd_tray *p = data; assert(bytes == sizeof(*p));
    cd_model.tray++; cd_model.generation++;
    cd_model.present = p->action == CIUKI_CD_TRAY_CLOSE; return 0;
  }
  if (!cd_model.present) return -ENXIO;
  if (h[3] && h[3] != cd_model.generation) return -EAGAIN;
  if (op == MEDIA_CD_TOC) {
    struct ciuki_media_cd_toc *p = data; assert(bytes == sizeof(*p));
    *p = cd_model.toc; p->version = 1; p->size = sizeof(*p); p->slot = 1; p->generation = cd_model.generation;
    return 0;
  }
  if (op == MEDIA_CD_READ_AUDIO) {
    assert(!mci_test_lock_depth()); /* blocking reads cannot own MCI guard */
    struct ciuki_media_cd_read *r = data; assert(bytes == sizeof(*r));
    assert(r->sectors && r->sectors <= 27 && buffer);
    for (uint32_t i = 0; i < cd_model.toc.count; i++) {
      uint32_t end = i + 1 < cd_model.toc.count ? cd_model.toc.tracks[i+1].start_lba : cd_model.toc.leadout_lba;
      assert(!(r->lba < end && r->lba + r->sectors > cd_model.toc.tracks[i].start_lba && (cd_model.toc.tracks[i].control & 4)));
    }
    cd_model.reads++;
    if (cd_model.during_read) { void (*f)(void) = cd_model.during_read; cd_model.during_read = 0; f(); }
    if (cd_model.read_error) return cd_model.read_error;
    if (!cd_model.present) return -ENXIO;
    if (r->generation != cd_model.generation) return -EAGAIN;
    assert((r->lba + r->sectors) * 2352 <= disc_bytes);
    memcpy(buffer, disc + r->lba * 2352, r->sectors * 2352); r->done = r->sectors * 2352;
    return 0;
  }
  if (op == MEDIA_CD_PLAY) {
    struct ciuki_media_cd_play *p = data; assert(bytes == sizeof(*p));
    cd_model.plays++; cd_model.absolute = cd_model.play_from = p->start_lba; cd_model.play_to = p->end_lba;
    cd_model.audio_status = CIUKI_CD_AUDIO_PLAYING; return cd_model.play_error;
  }
  if (op == MEDIA_CD_PAUSE) {
    struct ciuki_media_cd_pause *p = data; assert(bytes == sizeof(*p)); cd_model.actions++;
    cd_model.audio_status = p->action == CIUKI_CD_ACTION_STOP ? CIUKI_CD_AUDIO_COMPLETED :
                            p->action == CIUKI_CD_ACTION_PAUSE ? CIUKI_CD_AUDIO_PAUSED : CIUKI_CD_AUDIO_PLAYING;
    return 0;
  }
  if (op == MEDIA_CD_POSITION) {
    struct ciuki_media_cd_position *p = data; assert(bytes == sizeof(*p)); cd_model.positions++;
    p->audio_status = cd_model.audio_status; p->absolute_lba = cd_model.absolute; return 0;
  }
  assert(0); return -EINVAL;
}
#endif
