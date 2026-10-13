/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_AUTOPLAY_H
#define CIUKI_AUTOPLAY_H
#include "../files/model.h"
#include <ciuki/abi.h>
#define AP_INF_MAX 4096
#define AP_CHOICES 5
#define AP_PREFS 64
#define AP_REG_KEY "Desktop\\AutoPlay"
enum { AP_FOLDER, AP_MUSIC, AP_PICTURES, AP_RUN, AP_NOTHING };
struct ap_inf { char open[97], icon[FM_PATH], label[33]; };
struct ap_content {
    struct ap_inf inf;
    char root[16], music[FM_PATH], picture[FM_PATH], command[97], program[FM_PATH];
    int run_kind; /* 1 DOS, 2 Windows, 3 unsupported command */
};
struct ap_preference { char id[40], label[64]; int action; };
struct ap_workspace;
struct desk_autoplay {
    struct ciuki_media_volume pending[26], active;
    uint32_t seen[26], waiting;
    struct ap_content content;
    struct ap_workspace *workspace;
    int open, selected, always, want_commit;
};
int ap_parse(struct ap_inf *, const char *, size_t);
int ap_drive_label(const struct fm_vfs *, unsigned drive, char out[33]);
int ap_probe(const struct fm_vfs *, unsigned drive, struct ap_content *);
struct ap_workspace *ap_workspace_create(void);
int ap_probe_workspace(const struct fm_vfs *, unsigned drive, struct ap_content *, struct ap_workspace *);
const char *ap_action_name(int);
/* Registry adapter; directory NULL selects the installed registry. */
int ap_preference_get(const char *directory, const char *id);
int ap_preference_set(const char *directory, const char *id, const char *label, int action);
int ap_preference_list(const char *directory, struct ap_preference *, unsigned capacity);
int ap_preference_forget(const char *directory, const char *id);
struct desktop;
void desk_autoplay_update(struct desktop *, const struct ciuki_media_volume volumes[26]);
void desk_autoplay_service(struct desktop *);
int desk_autoplay_enabled(const struct desktop *, int);
const char *desk_autoplay_label(const struct desktop *, int, char *, size_t);
#endif
