/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_AUTOPLAY_H
#define CIUKI_AUTOPLAY_H
#include "../files/model.h"
#include "shell_link.h"
#include <ciuki/abi.h>
#define AP_INF_MAX 4096
#define AP_CHOICES 5
#define AP_PREFS 64
#define AP_REG_KEY "Desktop\\AutoPlay"
#define AP_ROOT_ENTRIES 256u
#define AP_SUB_ENTRIES 64u
#define AP_SNIFF_FILES 8u
#define AP_SCAN_NS 2000000000ull
#define AP_HEADER_SKIP 65536u        /* furthest e_lfanew the probe reads to */
#define AP_LAUNCH_NS 30000000000ull  /* a queued Windows launch always completes; safety bound */
enum { AP_FOLDER, AP_MUSIC, AP_PICTURES, AP_RUN, AP_NOTHING };
struct ap_inf { char open[97], icon[FM_PATH], label[33]; };
struct ap_content {
    struct ap_inf inf;
    char root[16], music[FM_PATH], picture[FM_PATH], command[97], program[FM_PATH], arguments[97];
    int run_kind; /* 1 DOS, 2 Windows (PE or 16-bit NE: Win32 personality), 3 unavailable */
    int exe_kind; /* exe_kind.h DESK_EXE_* of program */
    const char *run_reason; /* run_kind 3: "not found", "unreadable", "unsupported" */
    const char *budget; /* NULL or root/music/pictures/sniff/time */
    unsigned entries, sniffed, ms;
    unsigned cd_audio, cd_slot;
};
struct ap_preference { char id[40], label[64]; int action; };
struct ap_workspace;
struct ap_worker;
/* AP_LAUNCHING: a Windows program was queued on the desktop's launch worker;
 * arrivals wait until desk_autoplay_completed reports its result. */
enum { AP_IDLE, AP_PROBING, AP_READY, AP_LAUNCH_PENDING, AP_SAVE_PENDING, AP_SAVING, AP_LAUNCHING };
struct desk_autoplay {
    struct ciuki_media_volume pending[26], active;
    uint32_t seen[26], waiting;
    uint32_t cd_seen[4], cd_waiting;
    struct ap_content content;
    struct ap_workspace *workspace;
    struct ap_worker *worker;
    int phase, saved;
    int open, selected, always, want_commit;
    uint64_t launching_ns; /* AP_LAUNCHING since */
};
/* fx-autorun: a drive's AUTORUN.INF label and AutoPlay verb for My Computer. */
struct ap_drive_info {
    char label[33];
    int run_kind, exe_kind; /* as ap_content; 0: no open= */
    struct desk_launch_record record; /* run_kind 1 or 2 */
};
int ap_parse(struct ap_inf *, const char *, size_t);
int ap_drive_label(const struct fm_vfs *, unsigned drive, char out[33]);
int ap_probe(const struct fm_vfs *, unsigned drive, struct ap_content *);
int ap_drive_info(const struct fm_vfs *, unsigned drive, struct ap_drive_info *);
int ap_launch_record(const struct ap_content *, struct desk_launch_record *);
struct ap_workspace *ap_workspace_create(void);
void ap_workspace_observe(struct ap_workspace *, void (*)(void *, const char *), void *);
int ap_probe_workspace(const struct fm_vfs *, unsigned drive, struct ap_content *, struct ap_workspace *);
const char *ap_action_name(int);
/* Registry adapter; directory NULL selects the installed registry. */
int ap_preference_get(const char *directory, const char *id);
int ap_preference_set(const char *directory, const char *id, const char *label, int action);
int ap_preference_list(const char *directory, struct ap_preference *, unsigned capacity);
int ap_preference_forget(const char *directory, const char *id);
struct desktop;
void desk_autoplay_update(struct desktop *, const struct ciuki_media_volume volumes[26]);
void desk_autoplay_cd_update(struct desktop *, const struct ciuki_media_cd_state states[4]);
void desk_autoplay_service(struct desktop *);
/* Dispatch selected applications in the desktop's ordinary launch stage. */
int desk_autoplay_launch(struct desktop *);
/* The launch worker's result for a Windows program AutoPlay queued. */
void desk_autoplay_completed(struct desktop *, int result);
/* Prepare before the first frame; service never creates threads or performs
 * filesystem/registry I/O. Stop is for teardown, after busy() becomes false. */
int desk_autoplay_prepare(struct desktop *);
int desk_autoplay_busy(struct desktop *);
void desk_autoplay_stop(struct desktop *);
const char *desk_autoplay_activity(const struct desktop *);
int desk_autoplay_enabled(const struct desktop *, int);
const char *desk_autoplay_label(const struct desktop *, int, char *, size_t);
#endif
