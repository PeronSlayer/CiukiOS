/* sec-user-05 H11: per-user mixer preferences on the production desktop
 * settings-worker store (apps/desktop/master_storage.c over libreg). A
 * profile session saves its levels in its own Desktop root
 * (CIUKI_PROFILE_SETTINGS) and never writes the machine System root that the
 * kernel reads at boot; a profile-less bootstrap desktop keeps the machine
 * root; session start re-applies the saved levels through the control fd.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../../../apps/desktop/sound_storage.h"
#include <ciuki/abi.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static unsigned checks;
static struct ciuki_mixer_master applied_master;
static struct ciuki_mixer_line applied_lines[8];
static unsigned masters, lines;
static int apply(int fd, unsigned line, void *record, size_t n)
{
    CHECK(fd == 41);
    if (line) { CHECK(n == sizeof(struct ciuki_mixer_line) && lines < 8); memcpy(&applied_lines[lines++], record, n); }
    else { CHECK(n == sizeof(struct ciuki_mixer_master)); memcpy(&applied_master, record, n); masters++; }
    return 0;
}
static int exists(const char *dir, const char *leaf)
{
    char path[512]; struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    return !stat(path, &st);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    char machine[256], bootstrap[256], profile[256], settings[300];
    snprintf(machine, sizeof(machine), "%s/machine", argv[1]);
    snprintf(bootstrap, sizeof(bootstrap), "%s/bootstrap", argv[1]);
    snprintf(profile, sizeof(profile), "%s/profile", argv[1]);
    snprintf(settings, sizeof(settings), "%s/profile/desktop.cfg", argv[1]);
    CHECK(!mkdir(machine, 0700) && !mkdir(bootstrap, 0700) && !mkdir(profile, 0700) && !mkdir(settings, 0700));
    /* Profile-less bootstrap desktop: the machine root, as before. */
    unsetenv("CIUKI_PROFILE_SETTINGS");
    CHECK(!master_preference_save(bootstrap, 30, 40, 0) && exists(bootstrap, "System.reg"));
    CHECK(mixer_preferences_restore(41, apply) == 0 && !masters);
    /* A profile session: its own Desktop root only. */
    setenv("CIUKI_PROFILE_SETTINGS", settings, 1);
    CHECK(!master_preference_save(machine, 55, 60, 1));
    struct ciuki_mixer_line line = {.version = 1, .size = sizeof(line), .line = 1, .left = 10, .right = 20};
    CHECK(!line_preference_save(machine, &line));
    struct ciuki_mixer_line rec = {.version = 1, .size = sizeof(rec), .line = 3, .recording = 1, .left = 70, .right = 71,
                                   .selected = 1};
    CHECK(!line_preference_save(machine, &rec));
    CHECK(!exists(machine, "System.reg") && exists(settings, "Desktop.reg"));
    /* Session start re-applies exactly the saved levels. */
    CHECK(mixer_preferences_restore(41, apply) == 3 && masters == 1 && lines == 2);
    CHECK(applied_master.left == 55 && applied_master.right == 60 && applied_master.mute == 1 &&
          applied_master.version == 1 && applied_master.size == sizeof(applied_master) && !applied_master.save_state);
    CHECK(applied_lines[0].line == 1 && !applied_lines[0].recording && applied_lines[0].left == 10 &&
          applied_lines[0].right == 20 && !applied_lines[0].selected);
    CHECK(applied_lines[1].line == 3 && applied_lines[1].recording && applied_lines[1].left == 70 &&
          applied_lines[1].right == 71 && applied_lines[1].selected);
    /* Another profile has nothing saved: the machine default stays in force. */
    char other[300]; snprintf(other, sizeof(other), "%s/other", argv[1]); CHECK(!mkdir(other, 0700));
    setenv("CIUKI_PROFILE_SETTINGS", other, 1); masters = lines = 0;
    CHECK(mixer_preferences_restore(41, apply) == 0 && !masters && !lines);
    CHECK(master_preference_save(machine, 101, 0, 0) == -1 && errno == EINVAL);
    printf("H11 mixer preferences: profile session saves to its own Desktop root only, machine root untouched, session start restores master/source/recording; bootstrap keeps the machine root; checks=%u PASS\n", checks);
    return 0;
}
