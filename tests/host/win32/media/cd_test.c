/* MCI CD census and extraction oracle through production mixer.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "mci_host.h"
#include "cd_model.h"
static char result[256];
static void command(const char *s, uint32_t expected) {
  uint32_t r = mciSendStringA(s, result, sizeof(result), 0x1234);
  out("> %s\n< %u '%s'\n", s, r, result);
  if (r != expected) fprintf(stderr, "%s: got %u want %u\n", s, r, expected);
  assert(r == expected);
}
static void status(const char *s, const char *value) {
  command(s, 0); if (strcmp(result, value)) fprintf(stderr, "%s: '%s' != '%s'\n", s, result, value);
  assert(!strcmp(result, value));
}
static void finish(void) { command("close all", 0); cd_tick(); assert(!live_allocations && !cd_model.handles); }
static void changed(void) { cd_model.generation++; }
static void stopped(void) { command("stop c", 0); }
static void seeked(void) { command("seek c to end", 0); }
static void replayed(void) { command("play c from 00:02:10 to 00:02:30 notify", 0); }
static void closed(void) { command("close c", 0); }
int main(int argc, char **argv) {
  assert(argc == 3 && !hf_chdir(argv[1])); trace = fopen(argv[2], "w"); assert(trace);
  cd_load("mixed");
  command("open cdaudio alias c", 0);
  struct mci_caps caps = {.item=MCI_GETDEVCAPS_DEVICE_TYPE};
  assert(!mciSendCommandA(1,MCI_GETDEVCAPS,MCI_GETDEVCAPS_ITEM,(uintptr_t)&caps) && caps.value==MCI_DEVTYPE_CD_AUDIO);
  caps.item=MCI_GETDEVCAPS_CAN_PLAY;
  assert(!mciSendCommandA(1,MCI_GETDEVCAPS,MCI_GETDEVCAPS_ITEM,(uintptr_t)&caps) && caps.value==1);
  command("open d: type cdaudio alias duplicate", MCIERR_DEVICE_OPEN);
  command("open e: type cdaudio alias wrong", MCIERR_DEVICE_NOT_INSTALLED);
  command("open cdaudio!@0 alias wrongslot", MCIERR_DEVICE_NOT_INSTALLED);
  status("status c number of tracks", "3");
  status("status c media present", "true"); status("status c ready", "true"); status("status c mode", "stopped");
  status("status c type track 1", "other"); status("status c type track 2", "audio");
  status("status c position start", "00:02:00"); status("status c position track 2", "00:02:08");
  status("status c length track 2", "00:00:36"); status("status c length", "00:00:74");
  status("status c time format", "msf");
  command("info c product", 0); assert(!strcmp(result, "CiukiOS CD Audio"));
  command("set c time format bytes", MCIERR_BAD_TIME_FORMAT);
  command("set c time format tmsf", 0);
  status("status c position track 2", "02:00:00:00"); status("status c length track 2", "00:00:36");
  command("seek c to 2", 0); status("status c position", "02:00:00:00");
  uint32_t reads = cd_model.reads;
  command("play c from 1 to 2", MCIERR_OUTOFRANGE); assert(reads == cd_model.reads);
  command("play c from 2:0:60:0", MCIERR_OUTOFRANGE);
  command("status c type track 99", MCIERR_OUTOFRANGE);
  /* First track's complete samples: independent authored square-wave oracle. */
  capture = malloc(4); capture_cap = 1; capture_frames = 0;
  command("play c from 2 to 3 notify", 0); cd_tick(); cd_tick();
  assert(cd_model.reads == reads + 2 && !cd_model.positions);
  advance(100); command("pause c", 0); status("status c mode", "paused");
  status("status c position", "02:00:00:07");
  command("set c door open", MCIERR_DEVICE_LOCKED);
  command("resume c", 0); advance(1000); cd_tick();
  status("status c mode", "stopped"); assert(!cd_model.positions);
  assert(cd_model.last_notify==MCI_NOTIFY_SUCCESSFUL && cd_model.notifications==1);
  size_t start = 0;
  while (start < capture_frames && !capture[start*2] && !capture[start*2+1]) start++;
  assert(start + 36*588 <= capture_frames);
  for (size_t i = 0; i < 36*588; i++) {
    assert(capture[(start+i)*2] == (i/32 % 2 ? 6000 : -6000));
    assert(capture[(start+i)*2+1] == (i/48 % 2 ? 3000 : -3000));
  }
  free(capture); capture = 0; capture_frames = capture_cap = 0;
  out("ORACLE CD-DA frames=21168 match=1\n");
  /* Quake uses a per-track MSF length packed into a TMSF end address. */
  struct mci_status st = {.item = MCI_STATUS_LENGTH, .track = 2};
  assert(!mciSendCommandA(1, MCI_STATUS, MCI_STATUS_ITEM | MCI_TRACK, (uintptr_t)&st));
  assert(st.value == 36u << 16);
  struct mci_play p = {0x1234, 2, (uint32_t)(st.value << 8) | 2};
  assert(!mciSendCommandA(1, MCI_PLAY, MCI_FROM | MCI_TO | MCI_NOTIFY, (uintptr_t)&p));
  command("play c to 3 notify", 0); /* continued operation supersedes notify */
  assert(cd_model.last_notify==MCI_NOTIFY_SUPERSEDED);
  cd_tick(); advance(30); command("stop c", 0);
  assert(cd_model.last_notify==MCI_NOTIFY_ABORTED);
  command("seek c to end", 0); command("seek c to start", 0);
  command("set c time format milliseconds", 0); status("status c position start", "2000");
  command("seek c to 2107", 0); command("set c time format msf", 0);
  status("status c position", "00:02:09");
  command("set c door open", 0); status("status c media present", "false");
  command("play c", MCIERR_DEVICE_NOT_READY); command("set c door closed", 0);
  status("status c media present", "true"); finish();

  /* Capability-selected analog path, subchannel position and all controls. */
  cd_load("audio"); cd_model.drive = 26; cd_model.caps &= ~CIUKI_CD_CAP_CDDA_READ;
  command("open cdaudio alias c", 0); command("set c time format tmsf", 0);
  command("play c from 1 to 2 notify", 0); assert(cd_model.analog && !cd_model.reads && cd_model.plays == 1);
  cd_model.absolute = 15; cd_tick(); status("status c position", "01:00:00:15");
  command("pause c", 0); command("resume c", 0);
  cd_model.audio_status = CIUKI_CD_AUDIO_COMPLETED; cd_tick(); status("status c mode", "stopped");
  command("play c from 1 to 2 notify", 0); command("stop c", 0); finish();
  /* Failed extraction falls back at the stream's consumed position. */
  cd_load("mixed"); command("open cdaudio alias c", 0); command("set c time format tmsf", 0);
  command("play c from 2 to 3 notify", 0); cd_model.read_error = -EIO; cd_tick();
  assert(cd_model.analog && cd_model.plays == 1 && cd_model.play_from == 8 && cd_model.play_to == 44);
  finish();
  /* Forced digital: no fallback; forced analog ignores digital capability. */
  cd_load("audio"); cd_model.force = 1; command("open cdaudio alias c", 0);
  command("play c notify", 0); cd_model.read_error = -EIO; cd_tick(); assert(!cd_model.plays);
  assert(cd_model.last_notify==MCI_NOTIFY_FAILURE);
  status("status c mode", "stopped"); finish();
  cd_load("audio"); cd_model.force = 2; command("open cdaudio!@1 alias c", 0);
  command("play c", 0); assert(cd_model.analog && !cd_model.reads); finish();
  /* No analog capability, medium removal, generation change, timeout. */
  const int errors[] = {-ENXIO, -EAGAIN, -EIO, -EOPNOTSUPP, -ENOMEM};
  for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); i++) {
    cd_load("audio"); cd_model.caps &= ~CIUKI_CD_CAP_AUDIO_PLAY;
    command("open cdaudio alias c", 0); command("play c notify", 0);
    cd_model.read_error = errors[i];
    for (unsigned attempt=0;attempt<6;attempt++) cd_tick();
    assert(!cd_model.plays && cd_model.last_notify==MCI_NOTIFY_FAILURE);
    finish();
  }
  /* In-flight IO owns a reference and an epoch, never the MCI lock. */
  void (*actions[])(void) = {changed, stopped, seeked, replayed, closed};
  for (unsigned i = 0; i < sizeof(actions)/sizeof(actions[0]); i++) {
    cd_load("audio"); command("open cdaudio alias c", 0); command("play c notify", 0);
    cd_model.during_read = actions[i]; cd_tick();
    assert(!cd_model.plays); finish();
  }
  cd_load("audio"); command("open cdaudio alias c", 0); command("play c notify", 0); cd_tick();
  cd_model.present = 0; advance(120); status("status c media present", "false"); finish();
  /* Literal census spelling, using the default device name rather than
   * an alias. Numeric from/to values select tracks in TMSF format. */
  cd_load("mixed"); command("open cdaudio", 0);
  command("set cdaudio time format tmsf", 0);
  command("play cdaudio from 2 to 3", 0); cd_tick();
  assert(cd_model.reads && !cd_model.plays);
  command("stop cdaudio", 0); finish();
  cd_load("data"); command("open cdaudio alias c", 0); command("play c", MCIERR_OUTOFRANGE); assert(!cd_model.reads); finish();
  cd_destroy(); assert(!live_allocations); out("LIVE allocations=0\n"); fclose(trace);
  puts("PASS CD audio: MCI census, exact PCM, digital and analog paths, errors, epochs, notifications, tray");
  return 0;
}
