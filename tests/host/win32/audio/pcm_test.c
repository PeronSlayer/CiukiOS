/* Authored arithmetic oracle over production renderer and COM implementation.
 */
#include "../../../../sdk/win32/audio/audio.h"
#include "../../../../sdk/win32/audio/dsound.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct wa_engine engine;
static uint64_t consumed;
static uint32_t generation = 7;
static int refuse, partial;
static int16_t captured[32768];
static uint32_t capture_frames;
static uintptr_t events[1024];
static unsigned event_count;
static uint32_t allocated;
void *wa_alloc(uint32_t n) {
  uint32_t *p = calloc(1, n + 16);
  assert(p);
  *p = n;
  allocated += n;
  return p + 4;
}
void wa_free(void *p) {
  if (p) {
    uint32_t *q = (uint32_t *)p - 4;
    allocated -= *q;
    free(q);
  }
}
uint32_t wa_private_bytes(void) { return allocated + sizeof(engine); }
void W32API CiukiAudioLock(void) {}
void W32API CiukiAudioUnlock(void) {}
struct wa_engine *W32API CiukiAudioAcquire(uint32_t r, uint32_t *error) {
  *error = wa_open(&engine, r);
  return *error ? 0 : &engine;
}
struct wa_engine *wa_process_engine(void) { return &engine; }
static int open_mock(void *p, uint32_t rate) {
  (void)p;
  (void)rate;
  return refuse ? -12 : 0;
}
static int status_mock(void *p, uint64_t *n, uint32_t *g) {
  (void)p;
  *n = consumed;
  *g = generation;
  return 0;
}
static int write_mock(void *p, const int16_t *v, uint32_t n) {
  (void)p;
  if (partial && (uint32_t)partial < n)
    n = (uint32_t)partial;
  assert(capture_frames + n <= 16384);
  memcpy(captured + capture_frames * 2, v, n * 4);
  capture_frames += n;
  return (int)n;
}
static void close_mock(void *p) { (void)p; }
static void dispatch_mock(void *p, const struct wa_event *v) {
  (void)p;
  assert(event_count < 1024);
  events[event_count++] = v->message == 0x3bd ? v->p1 : v->callback.target;
}
static void reset_engine(unsigned rate) {
  assert(engine.source_count == 0);
  struct wa_transport t = {0,          open_mock,  status_mock,
                           write_mock, close_mock, dispatch_mock};
  wa_init(&engine, &t);
  consumed = 0;
  capture_frames = event_count = 0;
  partial = refuse = 0;
  assert(wa_open(&engine, rate) == 0);
}
static struct wa_format format(unsigned bits, unsigned channels,
                               unsigned rate) {
  struct wa_format f = {
      1,    channels, rate, rate * channels * bits / 8, channels * bits / 8,
      bits, 0};
  return f;
}
static int oracle_sample(const uint8_t *p, unsigned bits) {
  return bits == 8 ? ((int)*p - 128) * 256 : (int16_t)(p[0] | p[1] << 8);
}
static void exact_pcm(void) {
  unsigned rates[] = {11025, 22050, 44100, 48000},
           outputs[] = {22050, 44100, 48000};
  for (unsigned oi = 0; oi < 3; oi++)
    for (unsigned ri = 0; ri < 4; ri++)
      for (unsigned bits = 8; bits <= 16; bits += 8)
        for (unsigned channels = 1; channels <= 2; channels++) {
          reset_engine(outputs[oi]);
          struct wa_format f = format(bits, channels, rates[ri]);
          struct wa_source s;
          wa_source_init(&s, &f);
          struct wa_samples *m = wa_alloc(sizeof(*m) + 997 * f.align);
          m->refs = 1;
          m->bytes = 997 * f.align;
          for (unsigned i = 0; i < m->bytes; i++)
            m->data[i] = (uint8_t)(i * 97 + 13);
          s.samples = m;
          s.looping = s.playing = 1;
          s.flags = WA_GLOBAL;
          assert(!wa_register(&engine, &s));
          uint64_t phase = 0;
          unsigned produced = 0;
          int16_t out[512];
          while (produced < 2048) {
            unsigned batch = produced % 3 == 0   ? 1
                             : produced % 3 == 1 ? 17
                                                 : 256;
            if (batch > 2048 - produced)
              batch = 2048 - produced;
            if (produced > 400 && produced < 800) {
              s.volume = -600;
              s.pan = 3000;
              s.frequency = 33075;
            } else {
              s.volume = 0;
              s.pan = -1000;
              s.frequency = f.rate;
            }
            wa_source_gain(&s);
            unsigned n = wa_render(&engine, out, batch);
            assert(n == batch);
            for (unsigned i = 0; i < n; i++) {
              uint32_t frame = (uint32_t)(phase / engine.rate) % 997,
                       rem = (uint32_t)(phase % engine.rate),
                       next = (frame + 1) % 997;
              for (unsigned c = 0; c < 2; c++) {
                unsigned channel = channels == 1 ? 0 : c;
                int a = oracle_sample(
                        m->data + frame * f.align + channel * bits / 8, bits),
                    b = oracle_sample(
                        m->data + next * f.align + channel * bits / 8, bits);
                int64_t v = a + (int64_t)(b - a) * rem / engine.rate;
                v = v * s.gain[c] / 32768;
                assert(out[i * 2 + c] == v);
              }
              phase += s.frequency;
            }
            engine.accepted += n;
            wa_consumed(&engine, engine.accepted);
            assert(s.heard == (phase / engine.rate) % 997);
            assert(s.heard_fraction == phase % engine.rate);
            produced += n;
          }
          wa_unregister(&engine, &s);
          wa_free(m);
        }
}
static void transport_and_epochs(void) {
  for (unsigned r = 22050; r <= 48000; r = r == 22050   ? 44100
                                           : r == 44100 ? 48000
                                                        : 48001) {
    reset_engine(r);
    struct wa_source s;
    struct wa_format f = format(16, 2, r);
    wa_source_init(&s, &f);
    struct wa_samples *m = wa_alloc(sizeof(*m) + 4096);
    m->bytes = 4096;
    m->refs = 1;
    for (unsigned i = 0; i < 4096; i++)
      m->data[i] = (uint8_t)i;
    s.samples = m;
    s.playing = s.looping = 1;
    wa_register(&engine, &s);
    partial = 37;
    for (unsigned i = 0; i < 40; i++) {
      assert(!wa_pump(&engine));
      assert(engine.accepted - engine.consumed <= 512);
    }
    assert(engine.accepted == 512);
    assert(!s.heard);
    consumed = 193;
    assert(!wa_pump(&engine));
    assert(s.heard == 193);
    assert(wa_write_cursor(&engine, &s) == ((193 + 512) % 1024) * 4);
    wa_position(&s, 80);
    consumed = 400;
    assert(!wa_pump(&engine));
    assert(s.heard == 20);
    wa_unregister(&engine, &s);
    wa_free(m);
  }
}
static void notifications_and_wave(void) {
  reset_engine(44100);
  struct wa_format f = format(16, 1, 44100);
  struct wa_source s;
  wa_source_init(&s, &f);
  struct wa_samples *m = wa_alloc(sizeof(*m) + 16);
  m->bytes = 16;
  m->refs = 1;
  s.samples = m;
  s.playing = 1;
  s.notifications = 3;
  s.notify[0] = (struct wa_notify){4, 41};
  s.notify[1] = (struct wa_notify){12, 42};
  s.notify[2] = (struct wa_notify){WA_STOP, 43};
  wa_register(&engine, &s);
  wa_pump(&engine);
  assert(!event_count);
  consumed = 8;
  wa_pump(&engine);
  wa_dispatch(&engine);
  assert(event_count == 3 && events[0] == 41 && events[1] == 42 &&
         events[2] == 43);
  wa_unregister(&engine, &s);
  wa_free(m);
  reset_engine(44100);
  struct wa_source w;
  wa_source_init(&w, &f);
  w.wave = 1;
  w.handle = 123;
  wa_register(&engine, &w);
  int16_t a[3] = {1, 2, 3}, b[2] = {4, 5};
  struct wa_header h[2] = {
      {.data = (void *)a, .bytes = 6, .flags = WA_PREPARED},
      {.data = (void *)b, .bytes = 4, .flags = WA_PREPARED}};
  assert(!wa_write(&engine, &w, h));
  assert(!wa_write(&engine, &w, h + 1));
  wa_pump(&engine);
  assert(!(h[0].flags & WA_DONE));
  consumed = 3;
  wa_pump(&engine);
  wa_dispatch(&engine);
  assert(event_count == 1 && events[0] == (uintptr_t)h);
  assert(h[0].flags & WA_DONE);
  assert(h[1].flags & WA_QUEUED);
  assert(!wa_reset(&engine, &w));
  wa_dispatch(&engine);
  assert(event_count == 2 && events[1] == (uintptr_t)(h + 1));
  assert(h[1].flags & WA_DONE);
  assert(!w.heard_samples && !engine.header_count);
  wa_unregister(&engine, &w);
  reset_engine(44100);
  wa_source_init(&w, &f);
  w.wave = 1;
  w.handle = 124;
  wa_register(&engine, &w);
  struct wa_header quota[65];
  for (unsigned i = 0; i < 65; i++) {
    quota[i] =
        (struct wa_header){.data = (void *)a, .bytes = 6, .flags = WA_PREPARED};
    assert(wa_write(&engine, &w, &quota[i]) == (i == 64 ? 4 : 0));
  }
  assert(!wa_reset(&engine, &w));
  wa_dispatch(&engine);
  assert(!engine.header_count);
  wa_unregister(&engine, &w);
}
static void com_buffers(void) {
  reset_engine(44100);
  struct com *d, *b, *copy;
  assert(!DirectSoundCreate(0, &d, 0));
  struct wa_format f = format(16, 1, 22050);
  struct desc desc = {sizeof(desc), 0x80 | 0x40 | 0x20 | 0x100, 20, 0, &f, {0}};
  assert(!create_buffer(d, &desc, &b, 0));
  void *p, *q;
  uint32_t n, m;
  assert(!lock_buffer(b, 18, 6, &p, &n, &q, &m, 0) && n == 2 && m == 4);
  memset(p, 17, n);
  memset(q, 23, m);
  assert(!unlock_buffer(b, p, n, q, m));
  assert(!duplicate(d, b, &copy));
  assert(buf(copy)->source.samples == buf(b)->source.samples);
  assert(!set_volume(b, -600));
  assert(!set_pan(b, 10000));
  assert(!set_frequency(b, 11025));
  assert(!play(b, 0, 0, 1));
  wa_pump(&engine);
  assert(!stop(b));
  assert(!restore(b));
  assert(!release(copy));
  assert(!release(b));
  struct desc primary = {sizeof(primary), 1, 0, 0, 0, {0}};
  assert(!create_buffer(d, &primary, &b, 0));
  assert(lock_buffer(b, 0, 4, &p, &n, &q, &m, 0) == DSERR_INVALIDCALL);
  assert(!cooperative(d, 0, 4));
  assert(!lock_buffer(b, 0, 4, &p, &n, &q, &m, 0));
  assert(!unlock_buffer(b, p, n, q, m));
  release(b);
  desc.flags = 0x10 | 0x80 | 0x20;
  assert(!create_buffer(d, &desc, &b, 0));
  struct buffer *spatial = buf(b);
  assert(!s_set_pos(&spatial->spatial, 2, 0, 0, 1));
  assert(spatial->source.gain[1] == 32768);
  assert(!commit(&dev(d)->listener));
  assert(spatial->source.gain[0] == 0 && spatial->source.gain[1] == 16384);
  assert(!s_set_mode(&spatial->spatial, 2, 0));
  assert(spatial->source.gain[0] == 32768 && spatial->source.gain[1] == 32768);
  release(b);
  release(d);
  assert(!allocated);
}
static void gain_and_saturation(void){
 for(int db=0;db>-10000;db--)assert(wa_db_gain(db)==(uint32_t)llround(32768*pow(10,db/2000.0)));
 assert(!wa_db_gain(-10000));reset_engine(44100);
 struct wa_format f=format(16,1,44100);struct wa_source a,b;wa_source_init(&a,&f);wa_source_init(&b,&f);
 struct wa_samples *m=wa_alloc(sizeof(*m)+2);m->refs=2;m->bytes=2;w32_put16(m->data,32767);
 a.samples=b.samples=m;a.playing=b.playing=a.looping=b.looping=1;
 a.flags=WA_GLOBAL;wa_register(&engine,&a);wa_register(&engine,&b);int16_t out[2];
 engine.volume[0]=engine.volume[1]=32767;assert(wa_render(&engine,out,1)==1);assert(out[0]==32766&&out[1]==32766);
 engine.accepted++;wa_consumed(&engine,engine.accepted);engine.focused=0;engine.volume[0]=engine.volume[1]=65535;
 assert(wa_render(&engine,out,1)==1&&out[0]==32767&&out[1]==32767);
 engine.accepted++;wa_consumed(&engine,engine.accepted);engine.focused=1;
 assert(wa_render(&engine,out,1)==1&&out[0]==32767&&out[1]==32767);
 wa_unregister(&engine,&a);wa_unregister(&engine,&b);wa_free(m);
}
int main(void) {
  gain_and_saturation();exact_pcm();
  transport_and_epochs();
  notifications_and_wave();
  com_buffers();
  puts("PASS PCM exact: "
       "formats/rates/split/gain/pan/frequency/cursors/notifications/wave/"
       "reset/quotas/COM/duplicates/primary");
  return 0;
}
