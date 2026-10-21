/* File ownership uses handle identity, including FAT long/8.3 spellings.
 * The POSIX adapter models both spellings with hard links to authored media.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "mci_host.h"

int main(int argc, char **argv) {
  assert(argc == 3 && !hf_chdir(argv[1]));
  trace = fopen(argv[2], "w");
  assert(trace);
  const char *types[] = {"waveaudio", "sequencer"};
  const char *names[][3] = {{"Long Tone.wav", "LONGTO~1.WAV", "Long Tone 2.wav"},
                           {"Long Song.mid", "LONGSO~1.MID", "Long Song 2.mid"}};
  unsigned rejected = 0, independent = 0;
  for (unsigned t = 0; t < 2; t++)
    for (unsigned order = 0; order < 2; order++)
      for (unsigned sharing = 0; sharing < 4; sharing++) {
        char command[256];
        snprintf(command, sizeof(command), "open \"%s\" type %s alias first%s",
                 names[t][order], types[t], sharing & 1 ? " shareable" : "");
        assert(!mciSendStringA(command, 0, 0, 0));
        assert(hf_open_handles()); /* includes the cached sequencer's owner */
        snprintf(command, sizeof(command), "open \"%s\" type %s alias second%s",
                 names[t][!order], types[t], sharing & 2 ? " shareable" : "");
        assert(mciSendStringA(command, 0, 0, 0) == MCIERR_DEVICE_OPEN);
        assert(!mciGetDeviceIDA("second"));
        rejected++;
        /* Same content and similar name, but a different file identity. */
        snprintf(command, sizeof(command), "open \"%s\" type %s alias distinct",
                 names[t][2], types[t]);
        assert(!mciSendStringA(command, 0, 0, 0));
        independent++;
        assert(!mciSendStringA("close first", 0, 0, 0));
        /* Releasing the owner permits its other spelling immediately. */
        snprintf(command, sizeof(command), "open \"%s\" type %s alias reopened",
                 names[t][!order], types[t]);
        assert(!mciSendStringA(command, 0, 0, 0));
        assert(!mciSendStringA("close all", 0, 0, 0));
        assert(!live_allocations);
        assert(!hf_open_handles());
      }
  mci_identity_fail = 1;
  assert(mciSendStringA("open tone.wav alias failure", 0, 0, 0) == MCIERR_FILE_READ);
  mci_identity_fail = 0;
  assert(!mciGetDeviceIDA("failure") && !hf_open_handles() && !live_allocations);
  assert(mciSendStringA("open broken.wav alias failure", 0, 0, 0) == MCIERR_INVALID_FILE);
  assert(!hf_open_handles() && !live_allocations);
  fprintf(trace, "OWNERSHIP alias-conflicts=%u distinct-opens=%u allocations=%ld\n",
          rejected, independent, live_allocations);
  fclose(trace);
  printf("PASS MCI file identity conflicts=%u independent=%u\n", rejected, independent);
  fflush(stdout);
  return 0;
}
