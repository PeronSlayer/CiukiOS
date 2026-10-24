/* Frozen f5-12A/12B user-mode audio boundary. LGPL-2.1-or-later. */
#ifndef W32_AUDIO_H
#define W32_AUDIO_H
#include <w32/base.h>
#define WA_VERSION 1u
#define WA_QUANTUM 256u
#define WA_LEAD 512u
#define WA_SOURCES 64u
#define WA_CALLBACKS 512u
#define WA_HEADERS 64u
#define WA_TIMERS 16u
#define WA_NOTIFICATIONS 64u
#define WA_PRIVATE_LIMIT (4u * 1024u * 1024u)
#define WA_OK 0u
#define WA_ALLOCATED 4u
#define WA_INVALID 11u
#define WA_NOMEM 7u
#define WA_BUSY 33u
#define WA_PREPARED 2u
#define WA_QUEUED 16u
#define WA_DONE 1u
#define WA_BEGINLOOP 4u
#define WA_ENDLOOP 8u
#define WA_STOP 0xffffffffu
#define WA_GLOBAL 0x8000u
#define WA_STICKY 0x4000u
#define WA_CALLBACK_WINDOW 0x10000u
#define WA_CALLBACK_THREAD 0x20000u
#define WA_CALLBACK_FUNCTION 0x30000u
#define WA_CALLBACK_EVENT 0x50000u
#pragma pack(push, 1)
struct wa_format {
  uint16_t tag, channels;
  uint32_t rate, average;
  uint16_t align, bits, extra;
};
#pragma pack(pop)
struct wa_header {
  char *data;
  uint32_t bytes, recorded;
  uintptr_t user;
  uint32_t flags, loops;
  struct wa_header *next;
  uintptr_t reserved;
};
struct wa_callback {
  uintptr_t target, instance;
  uint32_t kind;
};
struct wa_event {
  uint64_t due;
  struct wa_callback callback;
  uintptr_t handle, p1, p2;
  uint32_t message;
  struct wa_header *header;
  uint32_t *completion_flags;
  uint32_t completion_clear, completion_set;
  uint32_t *completion_count;
};
struct wa_notify {
  uint32_t offset;
  uintptr_t event;
};
struct wa_samples {
  uint32_t refs, bytes;
  uint8_t data[];
};
struct wa_source {
  struct wa_samples *samples;
  struct wa_format format;
  uint32_t flags, playing, looping, paused, frame, fraction, frequency, route;
  uint32_t heard, heard_fraction, epoch, slot, active, wave;
  int32_t volume, pan;
  uint32_t gain[2];
  struct wa_notify notify[WA_NOTIFICATIONS];
  uint32_t notifications;
  struct wa_header *headers[WA_HEADERS];
  uint32_t header_count, header_at, header_frame;
  uint32_t loop_start, loop_count, break_loop;
  uint64_t rendered_samples, heard_samples, end_frame;
  struct wa_callback callback;
  uintptr_t handle;
};
/* Transport counter counts stereo frames accepted from this process, NOT
 * backend fetched/audible frames. open/write return negative errno on failure.
 * status returns monotonic consumed and stream generation. No device in tests.
 */
struct wa_transport {
  void *opaque;
  int (*open)(void *, uint32_t);
  int (*status)(void *, uint64_t *, uint32_t *);
  int (*write)(void *, const int16_t *, uint32_t);
  void (*close)(void *);
  void (*dispatch)(void *, const struct wa_event *);
};
struct wa_stamp {
  uint32_t frame, fraction, epoch;
  uint64_t samples;
};
struct wa_engine {
  struct wa_transport transport;
  uint32_t rate, opened, generation, focused, started;
  uint32_t line_gain[3][2];
  uint32_t volume[2], source_count, callback_count, callback_head, header_count;
  uint64_t accepted, consumed, base_consumed, rendered;
  uint32_t slot_epoch[WA_SOURCES];
  struct wa_source *sources[WA_SOURCES];
  struct wa_stamp ledger[WA_LEAD][WA_SOURCES];
  struct wa_event events[WA_CALLBACKS];
  int16_t pending[WA_QUANTUM * 2];
  uint32_t pending_frames, pending_at;
  void (*synth)(void *, int32_t *, uint32_t, uint32_t);
  void *synth_context;
};
void wa_init(struct wa_engine *, const struct wa_transport *);
int wa_valid_format(const struct wa_format *);
uint32_t wa_open(struct wa_engine *, uint32_t);
void wa_close(struct wa_engine *);
uint32_t wa_register(struct wa_engine *, struct wa_source *);
void wa_cancel_notifications(struct wa_engine *, struct wa_source *);
void wa_unregister(struct wa_engine *, struct wa_source *);
void wa_source_init(struct wa_source *, const struct wa_format *);
uint32_t wa_poll(struct wa_engine *);
uint32_t wa_pump(struct wa_engine *);
/* Offline production renderer: same function used by pump. Caller owns lock.
 * Never render >256 frames. Returns fewer when callback quota is exhausted. */
uint32_t wa_render(struct wa_engine *, int16_t *, uint32_t);
void wa_consumed(struct wa_engine *, uint64_t);
void wa_dispatch(
    struct wa_engine *); /* outside personality lock, max 64/pass */
uint32_t wa_event(struct wa_engine *, const struct wa_event *);
uint32_t wa_write(struct wa_engine *, struct wa_source *, struct wa_header *);
uint32_t wa_reset(struct wa_engine *, struct wa_source *);
void wa_stop(struct wa_engine *, struct wa_source *);
void wa_position(struct wa_source *, uint32_t);
uint32_t wa_write_cursor(const struct wa_engine *, const struct wa_source *);
uint32_t wa_db_gain(int32_t);
void wa_source_gain(struct wa_source *);
/* Process singleton lives in winmm; dsound imports these, never owns a mixer.
 */
struct wa_engine *W32API CiukiAudioAcquire(uint32_t, uint32_t *);
void W32API CiukiAudioLock(void);
void W32API CiukiAudioUnlock(void);
void W32API CiukiAudioFocus(uint32_t);
void *wa_alloc(uint32_t);
void wa_free(void *);
uint32_t wa_private_bytes(void);
void wa_wave_route(uintptr_t, unsigned);
void wa_input_tick(void);
int wa_capture_native(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#endif
