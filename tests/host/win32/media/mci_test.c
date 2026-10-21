/* f5-12B MCI host evidence: production mci.c/mcistr.c/mciwave.c/mciseq.c,
 * waveOut (wave.c), midiStream (midi.c), the mixer (mix.c) and ACM
 * (codecs/acm.c, adpcm.c). Only kernel32 files, critical sections, time and
 * the device transport are modelled (mci_host.c); time advances only in
 * Sleep(). Output: a deterministic trace (commands, results, notifications
 * with simulated time, MIDI messages with mixer frames, PCM digests and
 * reference-decode oracles). SPDX-License-Identifier: LGPL-2.1-or-later */
#include "mci_host.h"
/* ---- helpers ---- */
static uint32_t cmd(const char *s) {
  char result[256];
  memset(result, 0x5a, sizeof(result));
  out("> %s\n", s[0] ? s : "(empty)");
  uint32_t r = mciSendStringA(s, result, sizeof(result), 0x1234);
  out("= %u '%s'\n", r, r ? "" : result);
  return r;
}
static uint32_t expect(const char *s, uint32_t code, const char *want) {
  char result[256];
  out("> %s\n", s[0] ? s : "(empty)");
  uint32_t r = mciSendStringA(s, result, sizeof(result), 0x1234);
  out("= %u '%s'\n", r, r ? "" : result);
  if (r != code || (want && !r && strcmp(result, want))) {
    fprintf(stderr, "FAIL %s: %u '%s' (want %u '%s')\n", s, r,
            r ? "" : result, code, want ? want : "");
    exit(1);
  }
  return r;
}
static void wait_ms(uint32_t ms) {
  out("... %u ms\n", ms);
  for (uint32_t i = 0; i < ms; i += 5)
    Sleep(5);
}
static void pcm_mark(const char *what) {
  out("PCM %s frames=%llu digest=%08x\n", what, (unsigned long long)written,
      digest);
  digest = 2166136261u;
  written = 0;
}
/* Independent oracle: the reference decode (f5-14C scalar reference for
 * ADPCM, raw samples for PCM) must appear verbatim, channel for channel, in
 * the mixed stereo output at unity gain and equal rates. */
static void oracle_begin(void) {
  capture = malloc(4096 * 4);
  assert(capture);
  capture_frames = 0;
  capture_cap = 4096;
}
static void oracle_end(const char *file, uint32_t channels, uint32_t skip,
                       uint32_t count) {
  int fd = hf_open(file);
  assert(fd >= 0);
  long size = hf_size(fd);
  int16_t *want = malloc((size_t)size);
  assert(want && hf_read(fd, want, (unsigned long)size) == size);
  hf_close(fd);
  uint32_t frames = (uint32_t)(size / 2 / channels);
  if (!count)
    count = frames - skip;
  size_t found = (size_t)-1;
  for (size_t at = 0; at + count <= capture_frames && found == (size_t)-1;
       at++) {
    uint32_t i = 0;
    for (; i < count; i++) {
      int16_t l = want[(skip + i) * channels],
              r = want[(skip + i) * channels + (channels - 1)];
      if (capture[(at + i) * 2] != l || capture[(at + i) * 2 + 1] != r)
        break;
    }
    if (i == count)
      found = at;
  }
  out("ORACLE %s frames=%u..%u match=%d\n", file, skip, skip + count,
      found != (size_t)-1);
  if (found == (size_t)-1) {
    fprintf(stderr, "FAIL oracle %s\n", file);
    exit(1);
  }
  free(want);
  free(capture);
  capture = 0;
}
static uint32_t yields;
static uint32_t yield_proc(uint32_t id, uint32_t data) {
  out("YIELD id=%u data=%u\n", id, data);
  return ++yields >= 3;
}

static void errors(void) {
  out("== errors\n");
  expect("", MCIERR_MISSING_COMMAND_STRING, 0);
  expect("bogus x", MCIERR_UNRECOGNIZED_COMMAND, 0);
  expect("open", MCIERR_MISSING_DEVICE_NAME, 0);
  expect("open missing.wav", MCIERR_FILE_NOT_FOUND, 0);
  expect("open x.xyz", MCIERR_EXTENSION_NOT_FOUND, 0);
  expect("open broken.wav", MCIERR_INVALID_FILE, 0);
  expect("open broken.mid", MCIERR_INVALID_FILE, 0);
  expect("close nothing", MCIERR_INVALID_DEVICE_NAME, 0);
  expect("open cdaudio", MCIERR_DEVICE_NOT_INSTALLED, 0);
  expect("open clip-pcm.avi", MCIERR_CANNOT_LOAD_DRIVER, 0);
  expect("open new type waveaudio alias r", MCIERR_UNSUPPORTED_FUNCTION, 0);
  expect("open \"tone.wav alias q", MCIERR_NO_CLOSING_QUOTE, 0);
  expect("open \"tone.wav\"x", MCIERR_EXTRA_CHARACTERS, 0);
  expect("open tone.wav alias a", 0, "1");
  expect("open tone.wav alias a", MCIERR_DUPLICATE_ALIAS, 0);
  expect("open tone.wav alias all", MCIERR_CANNOT_USE_ALL, 0);
  expect("status a", MCIERR_MISSING_PARAMETER, 0);
  expect("status a sideways", MCIERR_UNRECOGNIZED_KEYWORD, 0);
  expect("status a length length", MCIERR_DUPLICATE_FLAGS, 0);
  expect("status a tempo", MCIERR_UNRECOGNIZED_KEYWORD, 0);
  expect("set a time format tmsf", MCIERR_BAD_TIME_FORMAT, 0);
  expect("set a time format fortnights", MCIERR_BAD_CONSTANT, 0);
  expect("play a from 9999", MCIERR_OUTOFRANGE, 0);
  expect("play a from 300 to 200", MCIERR_OUTOFRANGE, 0);
  expect("play a from x", MCIERR_BAD_INTEGER, 0);
  expect("play a from", MCIERR_MISSING_PARAMETER, 0);
  expect("seek a", MCIERR_MISSING_PARAMETER, 0);
  expect("seek a to start to end", MCIERR_FLAGS_NOT_COMPATIBLE, 0);
  expect("set a bitspersample 8", MCIERR_NONAPPLICABLE_FUNCTION, 0);
  expect("record a", MCIERR_UNSUPPORTED_FUNCTION, 0);
  expect("window a handle 5", MCIERR_UNRECOGNIZED_KEYWORD, 0);
  expect("close a", 0, "");
  char text[64];
  BOOL ok = mciGetErrorStringA(MCIERR_INVALID_DEVICE_NAME, text, sizeof(text));
  out("ERRSTR 263 %d '%s'\n", ok, text);
  assert(ok && text[0]);
  ok = mciGetErrorStringA(12345, text, sizeof(text));
  out("ERRSTR 12345 %d '%s'\n", ok, text);
  assert(!ok && !text[0]);
  ok = mciGetErrorStringA(MCIERR_INVALID_DEVICE_NAME, text, 8);
  assert(ok && strlen(text) == 7);
  WCHAR wide[64];
  assert(mciGetErrorStringW(MCIERR_FILE_NOT_FOUND, wide, 64) && wide[0]);
}

static void waveaudio(void) {
  out("== waveaudio\n");
  expect("open tone.wav type waveaudio alias w", 0, "1");
  expect("status w length", 0, "500");
  expect("status w mode", 0, "stopped");
  expect("status w ready", 0, "true");
  expect("status w number of tracks", 0, "1");
  expect("status w format tag", 0, "pcm");
  expect("status w samplespersec", 0, "22050");
  expect("status w bitspersample", 0, "16");
  expect("status w channels", 0, "1");
  expect("status w alignment", 0, "2");
  expect("status w bytespersec", 0, "44100");
  expect("capability w device type", 0, "waveaudio");
  expect("capability w can play", 0, "true");
  expect("capability w can record", 0, "false");
  expect("capability w outputs", 0, "1");
  expect("info w product", 0, "CiukiOS waveform audio");
  expect("info w file", 0, "tone.wav");
  expect("set w time format samples", 0, "");
  expect("status w time format", 0, "samples");
  expect("status w length", 0, "11025");
  expect("set w time format bytes", 0, "");
  expect("status w length", 0, "22050");
  expect("set w time format ms", 0, "");
  expect("status w time format", 0, "milliseconds");
  /* play with notify, superseded by a later notify request */
  expect("play w from 100 to 300 notify", 0, "");
  wait_ms(50);
  expect("status w mode", 0, "playing");
  cmd("status w position");
  expect("pause w", 0, "");
  expect("status w mode", 0, "paused");
  wait_ms(40);
  cmd("status w position");
  expect("resume w", 0, "");
  expect("status w mode notify", 0, "playing");
  wait_ms(400);
  expect("status w mode", 0, "stopped");
  expect("status w position", 0, "300");
  pcm_mark("w 100-300");
  /* a continuing play keeps its notification; the end arrives once */
  expect("play w from 0 to 200 notify", 0, "");
  wait_ms(30);
  expect("play w to 200 notify", 0, "");
  wait_ms(300);
  /* stop aborts; a new play with another end aborts */
  expect("play w from 0 notify", 0, "");
  wait_ms(20);
  expect("stop w", 0, "");
  expect("play w from 0 notify", 0, "");
  wait_ms(20);
  expect("play w from 0 to 100 notify", 0, "");
  wait_ms(20);
  expect("seek w to 50", 0, "");
  expect("status w position", 0, "50");
  expect("seek w to end", 0, "");
  expect("status w position", 0, "500");
  expect("seek w to start notify", 0, "");
  expect("status w position", 0, "0");
  /* wait: returns after completion; notify+wait notifies too */
  pcm_mark("before wait");
  expect("play w from 0 to 100 wait notify", 0, "");
  expect("status w position", 0, "100");
  expect("status w mode", 0, "stopped");
  pcm_mark("w 0-100 wait");
  /* break key returns control without stopping the play */
  key_down = 1;
  expect("play w from 0 wait", 0, "");
  key_down = 0;
  expect("status w mode", 0, "playing");
  expect("break w off", 0, "");
  expect("stop w", 0, "");
  /* yield procedure */
  uint32_t id = mciGetDeviceIDA("w");
  assert(id == 1);
  assert(mciSetYieldProc(id, (uintptr_t)yield_proc, 77));
  uint32_t data = 0;
  assert(mciGetYieldProc(id, &data) == (uintptr_t)yield_proc && data == 77);
  expect("play w from 0 wait", 0, "");
  expect("status w mode", 0, "playing");
  assert(mciSetYieldProc(id, 0, 0));
  expect("close w notify", 0, "");
  assert(!mciGetDeviceIDA("w"));
  /* compressed: IMA ADPCM and MS ADPCM via msacm32 */
  expect("open ima.wav alias i", 0, "1");
  expect("status i format tag", 0, "17");
  expect("status i bitspersample", 0, "4");
  expect("set i time format samples", 0, "");
  cmd("status i length");
  oracle_begin();
  expect("play i wait", 0, "");
  oracle_end("ima.pcm", 1, 0, 0);
  pcm_mark("ima full");
  oracle_begin();
  expect("play i from 1000 to 2000 wait", 0, "");
  oracle_end("ima.pcm", 1, 1000, 1000);
  pcm_mark("ima 1000-2000");
  expect("close i", 0, "");
  expect("open msadpcm.wav alias m", 0, "1");
  expect("status m channels", 0, "2");
  oracle_begin();
  expect("play m wait", 0, "");
  oracle_end("msadpcm.pcm", 2, 0, 0);
  pcm_mark("msadpcm full");
  expect("close m", 0, "");
  expect("open tone8.wav alias e", 0, "1");
  expect("play e wait", 0, "");
  pcm_mark("pcm8 stereo 11025");
  expect("close e", 0, "");
}

static void sequencer(void) {
  out("== sequencer\n");
  expect("open song.mid alias s", 0, "1");
  expect("status s length", 0, "1500");
  expect("status s division type", 0, "PPQN");
  expect("status s tempo", 0, "120");
  expect("status s port", 0, "mapper");
  expect("capability s device type", 0, "sequencer");
  expect("set s time format song pointer", 0, "");
  expect("status s length", 0, "16");
  expect("status s time format", 0, "song pointer");
  expect("set s time format smpte 30", 0, "");
  expect("status s length", 0, "00:00:01:15");
  expect("set s time format ms", 0, "");
  expect("set s tempo 90", MCIERR_UNSUPPORTED_FUNCTION, 0);
  expect("set s port 7", MCIERR_SEQ_PORT_NONEXISTENT, 0);
  out("-- play all\n");
  expect("play s from 0 notify", 0, "");
  wait_ms(1700);
  expect("status s mode", 0, "stopped");
  expect("status s position", 0, "1500");
  out("-- play from 1100 (chase), pause/resume (re-chase), stop\n");
  expect("play s from 1100 notify", 0, "");
  wait_ms(100);
  expect("status s tempo", 0, "240");
  expect("pause s", 0, "");
  expect("status s mode", 0, "paused");
  cmd("status s position");
  wait_ms(100);
  expect("resume s", 0, "");
  wait_ms(100);
  expect("stop s", 0, "");
  wait_ms(20);
  expect("play s from 0 to 600 wait", 0, "");
  expect("status s position", 0, "600");
  expect("close s", 0, "");
  expect("open song.rmi alias r", 0, "1");
  expect("status r length", 0, "1500");
  expect("close r", 0, "");
  expect("open smpte.mid alias t", 0, "1");
  expect("status t time format", 0, "smpte 25");
  expect("status t length", 0, "00:00:01:00");
  expect("status t division type", 0, "SMPTE 25 Frame");
  expect("status t tempo", 0, "25");
  expect("set t time format song pointer", MCIERR_SEQ_DIV_INCOMPATIBLE, 0);
  expect("play t wait", 0, "");
  expect("close t", 0, "");
}

static void shortcuts_and_commands(void) {
  out("== auto-open, all, sysinfo\n");
  expect("status tone.wav length", 0, "500");
  expect("status tone.wav length notify", MCIERR_NOTIFY_ON_AUTO_OPEN, 0);
  expect("play tone.wav notify", MCIERR_NOTIFY_ON_AUTO_OPEN, 0);
  expect("play tone.wav from 0 to 50 wait", 0, "");
  assert(!mciGetDeviceIDA("tone.wav"));
  expect("play tone.wav from 0 to 50", 0, "");
  assert(mciGetDeviceIDA("tone.wav"));
  wait_ms(120);
  assert(!mciGetDeviceIDA("tone.wav"));
  expect("sysinfo all quantity", 0, "4");
  expect("sysinfo all name 1", 0, "waveaudio");
  expect("sysinfo all name 3", 0, "avivideo");
  expect("sysinfo all name 4", 0, "cdaudio");
  expect("sysinfo all name 5", MCIERR_OUTOFRANGE, 0);
  expect("sysinfo waveaudio quantity", 0, "1");
  expect("sysinfo cdaudio quantity", 0, "1");
  expect("sysinfo waveaudio installname", 0, "waveaudio");
  expect("open tone.wav alias x1", 0, "1");
  expect("open song.mid alias x2", 0, "2");
  expect("sysinfo all quantity open", 0, "2");
  expect("sysinfo all name 2 open", 0, "x2");
  expect("play x1 notify", 0, "");
  expect("play x2 notify", 0, "");
  wait_ms(30);
  expect("stop all", 0, "");
  expect("play x1 from 0 notify", 0, "");
  expect("status all mode", MCIERR_CANNOT_USE_ALL, 0);
  expect("close all", 0, "");
  expect("sysinfo all quantity open", 0, "0");

  out("== command interface\n");
  struct mci_open o = {0};
  o.callback = 0x99;
  o.type = MCI_DEVTYPE_WAVEFORM_AUDIO;
  o.element = (uintptr_t) "tone.wav";
  o.alias = (uintptr_t) "cmd";
  uint32_t r = mciSendCommandA(0, MCI_OPEN,
                               MCI_OPEN_TYPE | MCI_OPEN_TYPE_ID |
                                   MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS |
                                   MCI_NOTIFY,
                               (uintptr_t)&o);
  out("CMD open r=%u id=%u\n", r, o.device);
  assert(!r && o.device == 1);
  assert(mciGetDeviceIDA("CMD") == 1);
  assert(mciGetDeviceIDA("all") == MCI_ALL_DEVICE_ID);
  assert(mciGetDeviceIDA("nothing") == 0);
  WCHAR wname[] = {'c', 'm', 'd', 0};
  assert(mciGetDeviceIDW(wname) == 1);
  assert(mciGetCreatorTask(1) == 4242 && !mciGetCreatorTask(9));
  struct mci_status st = {0};
  st.item = MCI_STATUS_MODE;
  r = mciSendCommandA(1, MCI_STATUS, MCI_STATUS_ITEM, (uintptr_t)&st);
  out("CMD status mode r=%u value=%lu\n", r, (unsigned long)st.value);
  assert(!r && st.value == MCI_MODE_STOP);
  st.item = MCI_STATUS_READY;
  r = mciSendCommandA(1, MCI_STATUS, MCI_STATUS_ITEM, (uintptr_t)&st);
  assert(!r && st.value == 1);
  st.item = MCI_STATUS_LENGTH;
  r = mciSendCommandA(1, MCI_STATUS, MCI_STATUS_ITEM, (uintptr_t)&st);
  assert(!r && st.value == 500);
  struct mci_caps caps = {0};
  caps.item = MCI_GETDEVCAPS_DEVICE_TYPE;
  r = mciSendCommandA(1, MCI_GETDEVCAPS, MCI_GETDEVCAPS_ITEM,
                      (uintptr_t)&caps);
  assert(!r && caps.value == MCI_DEVTYPE_WAVEFORM_AUDIO);
  assert(mciSendCommandA(1, MCI_STATUS, MCI_NOTIFY, 0) ==
         MCIERR_NULL_PARAMETER_BLOCK);
  assert(mciSendCommandA(77, MCI_STOP, 0, 0) == MCIERR_INVALID_DEVICE_ID);
  assert(mciSendCommandA(1, 0x0400, 0, 0) == MCIERR_UNRECOGNIZED_COMMAND);
  struct mci_play play = {0x55, 0, 100};
  r = mciSendCommandA(1, MCI_PLAY, MCI_FROM | MCI_TO | MCI_NOTIFY,
                      (uintptr_t)&play);
  assert(!r);
  wait_ms(150);
  assert(!mciSendCommandA(1, MCI_CLOSE, 0, 0));
  WCHAR wcmd[] = {'o', 'p', 'e', 'n', ' ', 's', 'o', 'n', 'g', '.', 'm',
                  'i', 'd', ' ', 'a', 'l', 'i', 'a', 's', ' ', 'u', 0};
  WCHAR wres[16];
  r = mciSendStringW(wcmd, wres, 16, 0);
  assert(!r && wres[0] == '1' && !wres[1]);
  WCHAR wtype[] = {'w', 'a', 'v', 'e', 'a', 'u', 'd', 'i', 'o', 0};
  WCHAR welem[] = {'t', 'o', 'n', 'e', '.', 'w', 'a', 'v', 0};
  struct mci_open wo = {0};
  wo.type = (uintptr_t)wtype;
  wo.element = (uintptr_t)welem;
  r = mciSendCommandW(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_ELEMENT,
                      (uintptr_t)&wo);
  assert(!r && wo.device == 2);
  WCHAR info[64];
  struct mci_info wi = {0, (uintptr_t)info, 64};
  r = mciSendCommandW(2, MCI_INFO, MCI_INFO_PRODUCT, (uintptr_t)&wi);
  assert(!r && info[0] == 'C');
  assert(!mciSendCommandA(MCI_ALL_DEVICE_ID, MCI_CLOSE, 0, 0));
}

/* Regressions are silent so the original device trace stays pinned. */
void hf_spawn(void *(*)(void *));
void hf_join(void);
void hf_gate_set(int);
void hf_gate_wait(int);
extern void (*mci_wait_hook)(void);
extern _Thread_local void (*mci_unlock_hook)(void);
static unsigned yield_calls;
static void blocked_key(void) {
  assert(++yield_calls == 1); /* a reused ID must not extend this wait */
}
static void unlocked_wait(void) {
  if (!engine.source_count) return; /* ignore name lookup's earlier unlock */
  mci_unlock_hook = 0;
  hf_gate_set(1); hf_gate_wait(2);
}
static void *waiter(void *unused) {
  (void)unused;
  mci_unlock_hook = unlocked_wait;
  assert(!mciSendStringA("play old wait", 0, 0, 0));
  return 0;
}
static void amendment2(void) {
  FILE *saved = trace; trace = tmpfile(); assert(trace);
  assert(!mciSendStringA("open tone.wav alias old", 0, 0, 0));
  mci_wait_hook = blocked_key;
  hf_spawn(waiter); hf_gate_wait(1);
  assert(!mciSendStringA("close old", 0, 0, 0));
  assert(!mciSendStringA("open tone.wav alias replacement", 0, 0, 0));
  assert(!mciSendStringA("play replacement", 0, 0, 0));
  hf_gate_set(2); hf_join(); mci_wait_hook = 0;
  assert(yield_calls == 1);
  /* Exhaust callbacks with queued PCM. The owner must survive close and
   * remain in the cleanup queue until callbacks and close can succeed. */
  unsigned before = engine.source_count;
  engine.callback_count = WA_CALLBACKS;
  assert(!mciSendStringA("close replacement", 0, 0, 0));
  assert(engine.source_count == before && live_allocations > 0);
  mci_service_pass(); assert(engine.source_count == before);
  engine.callback_count = 0;
  for (unsigned i = 0; i < 8; i++) { wa_dispatch(&engine); mci_service_pass(); }
  assert(engine.source_count == 0 && live_allocations == 0);
  const char *types[] = {"waveaudio", "sequencer"};
  const char *files[] = {"tone.wav", "song.mid"};
  for (unsigned t = 0; t < 2; t++) for (unsigned order = 0; order < 2; order++) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "open %s type %s alias first%s", files[t], types[t], order ? " shareable" : "");
    assert(!mciSendStringA(cmd, 0, 0, 0));
    snprintf(cmd, sizeof(cmd), "open ./%s type %s alias second%s", files[t], types[t], order ? "" : " shareable");
    assert(mciSendStringA(cmd, 0, 0, 0) == MCIERR_DEVICE_OPEN);
    snprintf(cmd, sizeof(cmd), "open %s type %s alias third shareable", files[t], types[t]);
    assert(mciSendStringA(cmd, 0, 0, 0) == MCIERR_DEVICE_OPEN);
    assert(!mciSendStringA("close all", 0, 0, 0));
  }
  fclose(trace); trace = saved;
}

int main(int argc, char **argv) {
  if (argc != 3 || hf_chdir(argv[1])) {
    fprintf(stderr, "usage: mci_test fixture-dir trace\n");
    return 2;
  }
  trace = fopen(argv[2], "w");
  if (!trace)
    return 2;
  errors();
  waveaudio();
  sequencer();
  shortcuts_and_commands();
  for (int i = 0; i < 40; i++)
    Sleep(5);
  amendment2();
  out("LIVE allocations=%ld\n", live_allocations);
  fclose(trace);
  if (live_allocations != (engine.opened ? 0 : 0)) {
    fprintf(stderr, "FAIL leaked %ld allocations\n", live_allocations);
    return 1;
  }
  printf("PASS MCI waveaudio sequencer strings commands notify wait\n");
  return 0;
}
