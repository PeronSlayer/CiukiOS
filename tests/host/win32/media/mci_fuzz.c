/* f5-12B MCI fuzz: mutated authored MIDI/WAV files through the production
 * parsers (open/status/play/seek/close) and random command strings through
 * the production string parser, under ASan/UBSan, with a final allocation
 * balance check. Deterministic LCG; no external media.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "mci_host.h"
static uint32_t seed = 0x5eed1234u;
static uint32_t rnd(uint32_t n) {
  seed = seed * 1664525u + 1013904223u;
  return n ? (seed >> 8) % n : 0;
}
static uint8_t *load(const char *name, long *size) {
  int fd = hf_open(name);
  assert(fd >= 0);
  *size = hf_size(fd);
  uint8_t *p = malloc((size_t)*size + 64);
  assert(p && hf_read(fd, p, (unsigned long)*size) == *size);
  hf_close(fd);
  return p;
}
static void run(const char *s) {
  char result[64];
  uint32_t n = rnd(4) ? sizeof(result) : rnd(4);
  mciSendStringA(s, n ? result : 0, n, 0x77);
}
static const char *const words[] = {
    "open", "close", "play", "stop", "pause", "resume", "seek", "status",
    "set", "info", "capability", "sysinfo", "break", "window", "put", "where",
    "step", "update", "all", "a", "b", "tone.wav", "song.mid", "smpte.mid",
    "x.avi", "cdaudio", "waveaudio", "alias", "type", "from", "to", "start",
    "end", "notify", "length", "position", "mode", "time", "format", "ms",
    "samples", "bytes", "song", "pointer", "smpte", "30", "drop", "track",
    "1", "0", "4294967295", "99999999999", "1:2:3:4", "1:2:3:4:5", "300:1",
    "-1", "\"", "\"q q\"", "quantity", "name", "open", "tempo", "port",
    "none", "mapper", "on", "off", "door", "product", "file", "at", "by"};
int main(int argc, char **argv) {
  if (argc != 3 || hf_chdir(argv[1]))
    return 2;
  trace = fopen("fuzz.trace", "w");
  assert(trace);
  uint32_t iterations = (uint32_t)strtoul(argv[2], 0, 10);
  const char *seeds[] = {"song.mid", "smpte.mid", "song.rmi", "tone.wav",
                         "ima.wav", "msadpcm.wav", "tone8.wav"};
  long sizes[7];
  uint8_t *data[7];
  for (unsigned i = 0; i < 7; i++)
    data[i] = load(seeds[i], &sizes[i]);
  uint8_t *buffer = malloc(1 << 20);
  assert(buffer);
  uint32_t opened = 0;
  for (uint32_t it = 0; it < iterations; it++) {
    if (it % 4 == 3) { /* random command string */
      char s[256];
      uint32_t at = 0, n = 1 + rnd(8);
      for (uint32_t i = 0; i < n && at < 200; i++) {
        const char *w = words[rnd(sizeof(words) / sizeof(words[0]))];
        if (!rnd(16)) {
          s[at++] = (char)(1 + rnd(255));
          continue;
        }
        while (*w && at < 200)
          s[at++] = *w++;
        s[at++] = ' ';
      }
      s[at] = 0;
      /* never wait on a device the fuzzer opened */
      if (!strstr(s, "wait"))
        run(s);
      continue;
    }
    unsigned k = rnd(7);
    long n = sizes[k];
    memcpy(buffer, data[k], (size_t)n);
    uint32_t mutations = 1 + rnd(6);
    for (uint32_t m = 0; m < mutations; m++) {
      uint32_t kind = rnd(5), at = rnd((uint32_t)n);
      if (kind == 0)
        buffer[at] ^= (uint8_t)(1u << rnd(8));
      else if (kind == 1)
        buffer[at] = (uint8_t)rnd(256);
      else if (kind == 2 && n > 8)
        n = 1 + rnd((uint32_t)n);
      else if (kind == 3 && at + 4 <= (uint32_t)n) {
        uint32_t v = rnd(2) ? 0xffffffffu : rnd(1u << 20);
        memcpy(buffer + at, &v, 4);
      } else if (n + 16 < (1 << 20)) {
        for (int i = 0; i < 16; i++)
          buffer[n + i] = (uint8_t)rnd(256);
        n += 16;
      }
    }
    const char *name = k < 3 ? "fz.mid" : "fz.wav";
    assert(!hf_write_file(name, buffer, (unsigned long)n));
    char command[64];
    snprintf(command, sizeof(command), "open %s alias f", name);
    if (mciSendStringA(command, 0, 0, 0))
      continue;
    opened++;
    run("status f length");
    run("set f time format ms");
    run("status f position");
    run("status f tempo");
    mciSendStringA("play f from 0 to 60 wait", 0, 0, 0);
    run("play f notify");
    for (int i = 0; i < 4; i++)
      Sleep(5);
    run("seek f to end");
    mciSendStringA("close f", 0, 0, 0);
  }
  mciSendStringA("close all", 0, 0, 0);
  for (int i = 0; i < 100; i++)
    Sleep(5);
  for (unsigned i = 0; i < 7; i++)
    free(data[i]);
  free(buffer);
  fclose(trace);
  if (live_allocations) {
    fprintf(stderr, "FAIL leaked %ld allocations\n", live_allocations);
    return 1;
  }
  printf("PASS MCI fuzz iterations=%u opened=%u\n", iterations, opened);
  return 0;
}
