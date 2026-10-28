/* SPDX-License-Identifier: MIT
 * /Programs/setup: the CiukiOS Setup wizard (directive f9-03, plan
 * docs/design/cd-setup-plan.md §§3.2, 4, 7, 8, 9). A full-screen libdeskui
 * program spawned by the supervisor in Setup mode instead of the desktop.
 *
 * The wizard is a pure state machine over struct st_host: every device,
 * file, clock and power access goes through it, so the host tests drive the
 * whole flow with scripted keys, fixtures and the fake libsetup backend.
 * The only call that writes to an ATA disk is setup_txn_execute(), made from
 * the Install action of the "Ready to install" page (st_install). */
#ifndef CIUKI_SETUP_APP_H
#define CIUKI_SETUP_APP_H
#include "../libdeskui/deskui.h"
#include "../libdeskui/textarea.h"
#include "setup_txn.h"
#include "report_link.h"
#include <ciuki/abi.h>
#include <ciuki/tzdata.h>
#include <stdint.h>
#include "../../sdk/private/provision.h"
struct acct_io; /* apps/accounts/adapter.h: protected store I/O of the repair target */

#define ST_W 640                       /* design size; larger modes centre it */
#define ST_H 480
#define ST_BACKGROUND 0x0a3d62u        /* mockup wallpaper colour */
#define ST_SIDEBAR 0x1b2a5au
#define ST_PORTRAIT 132                /* sidebar portrait (nearest-neighbour from 256) */
#define ST_LICENCES_MAX 256
#define ST_TEXT_BYTES (256u * 1024u)   /* licence/report viewer buffer, allocated once */
#define ST_FILE_BYTES (64u * 1024u)    /* answer file, INDEX.TXT, rescue copies */
#define ST_REPORT_BYTES 8192u
#define ST_POOL_BYTES 16384u
#define ST_COUNTDOWN_S 15u             /* Restart page */
#define ST_EJECT_WAIT_S 60u            /* Restart=eject and force */
#define ST_TIP_S 20u                   /* "Did you know?" rotation */
#define ST_POWER_POLL_NS 200000000ull  /* call 73 STATUS: cached, no AML/BIOS call */

enum st_page {
    ST_PAGE_WELCOME, ST_PAGE_LICENCE, ST_PAGE_HARDWARE, ST_PAGE_DISK, ST_PAGE_COMPONENTS,
    ST_PAGE_USER, ST_PAGE_ACCOUNT /* sec-user-02: passwords */, ST_PAGE_NETWORK, ST_PAGE_SUMMARY, ST_PAGE_COPY, ST_PAGE_RESTART,
    ST_PAGE_STOP,      /* screen 12: hardware refusal (the graphical form of the kernel's) */
    ST_PAGE_FAILED,    /* "Setup did not complete" */
    ST_PAGE_STATUS,    /* unattended stop: the status id */
    ST_PAGE_RESCUE, ST_PAGE_TEXT,
    ST_PAGE_REPAIR,    /* sec-user-02: confirmed lost-password repair from Rescue */
    ST_PAGES
};
/* Kernel-evaluable refusals of plan §3.2 (screen 12). */
enum st_stop { ST_STOP_NONE, ST_STOP_VIDEO, ST_STOP_NO_DISK, ST_STOP_NO_CD, ST_STOP_DISK };
enum st_mark { ST_MARK_OK, ST_MARK_WARN, ST_MARK_STOP, ST_MARK_INFO, ST_MARK_ACTIVE, ST_MARK_PENDING };
enum st_install { ST_INSTALL_NONE, ST_INSTALL_INCOMPLETE, ST_INSTALL_DONE };

/* Read-only view of libdisk's enumeration (f9-02 disk_enumerate). */
#define ST_PARTS 16
struct st_part { uint8_t type, active, logical; uint64_t start, count; char letter; char label[12], fs[8]; };
struct st_disk {
    uint8_t present, slot, unit;          /* ATA channel and unit; disk 0 = 0:0 */
    char model[41], serial[21], mode[16]; /* mode: "UDMA 2" when known */
    uint64_t sectors; unsigned sector_size;
    uint8_t fingerprint[32];
    unsigned part_count; struct st_part parts[ST_PARTS];
    uint8_t occupied;                     /* any partition or filesystem, including an unpartitioned volume */
    uint8_t installable; char refusal[64]; /* libdisk's reason when not installable */
    uint8_t install; char release[65];    /* SETUP.STATE on its first volume */
};
struct st_volume { char letter; uint8_t usb, writable; char label[33], fs[8]; };
struct st_cd { uint8_t present, ready, slot, unit; char model[41]; uint32_t generation; };
struct st_size { uint64_t bytes, alloc_bytes; uint32_t files; uint8_t known; };

struct st_host {
    void *ctx;
    uint64_t (*monotonic)(void *);                                        /* ns */
    int64_t (*realtime)(void *);                                          /* UTC seconds */
    int (*input)(void *, struct ciuki_input_event *, unsigned max);       /* nonblocking: n, 0, or -1 closed */
    void (*present)(void *, const uint32_t *pixels, int width, int height);
    void (*idle)(void *, uint64_t ns);                                    /* wait for input or ns */
    int (*devices)(void *, struct ciuki_devices_info *);
    int (*memory)(void *, uint64_t *usable_kib);
    int (*display)(void *, struct ciuki_display_info *);
    int (*framebuffer)(void *, unsigned *width, unsigned *height, unsigned *bpp);
    int (*network)(void *, struct ciuki_net_state *);
    int (*disks)(void *, struct st_disk *, unsigned cap);                 /* read-only enumeration */
    int (*volumes)(void *, struct st_volume *, unsigned cap);             /* letter order */
    int (*cd)(void *, struct st_cd *);
    int (*catalogue)(void *, struct st_size sizes[SETUP_COMP_COUNT]);
    int (*release)(void *, char *out, size_t cap);
    int (*read_file)(void *, const char *path, char *buf, size_t cap, size_t *length); /* -EFBIG: too big */
    int (*write_file)(void *, const char *path, const char *buf, size_t length);      /* USB only */
    int (*copy_file)(void *, const char *from, const char *to, char *buffer, size_t cap); /* bounded streaming, USB destination */
    int (*make_dir)(void *, const char *path);                                        /* USB only */
    int (*list_dir)(void *, const char *path, char (*names)[64], unsigned cap);
    /* Runs /Programs/hwreport headless (--rescue --text-fd=3) and returns the
     * report text it produced (report_link.h). 0 with the text in buffer and
     * its length, or a negative errno with reason set (missing, spawn, timeout,
     * closed, frame, size, checksum, status, exit, ...). */
    int (*report_app)(void *, char *buffer, size_t cap, size_t *length, char reason[24]);
    void (*record)(void *, const char *line);                             /* kernel log, serial */
    int (*eject)(void *);
    int (*media_present)(void *);                                         /* 1 in, 0 removed, <0 unknown */
    int (*reboot)(void *);
    int (*power_off)(void *);
    int (*power_status)(void *, struct ciuki_power_status *);            /* call 73 STATUS, read-only */
    char (*dest_letter)(void *);                                          /* published destination */
    const uint32_t *portrait;                                             /* 256x256 XRGB or NULL */
    /* sec-user-02: kernel entropy (all bytes or -errno, never a fallback) for
     * password salts, and the protected /System/Security store of an
     * identified installed volume for the confirmed account repair only. */
    int (*entropy)(void *, void *, size_t);
    int (*repair_io)(void *, char letter, struct acct_io *out);
};

/* sec-user-02 accounts (user-security-contract.md 3.6, 5.1-5.3). Secrets
 * live only in these bounded buffers, are masked on screen, never logged,
 * and are wiped on Back, Cancel, finish and error. The seed holds salted
 * verifiers only, built when the Passwords page is accepted. */
#define ST_SECRET_FIELDS 4
#define ST_SECRET_BYTES 64
struct st_accounts {
    char secret[ST_SECRET_FIELDS][ST_SECRET_BYTES];  /* password, confirmation, recovery, confirmation */
    uint8_t seed[CIUKI_PROVISION_SEED_BYTES];
    int seed_ready;
};
struct st_repair {
    uint64_t generation, next_generation, high_generation;
    uint32_t high_uid;
    int slot, recovery_configured, pending;
    unsigned count;
    uint32_t uids[18];
    uint8_t administrator[18], enabled[18];
    char names[18][32];
};

struct st_hw_row { uint8_t mark; char name[20]; char text[200]; };
struct st_line { const char *text; uint8_t entry, font, mark; uint32_t ink; int y, h; };
struct st_view { struct st_line lines[384]; unsigned count; int scroll, content, selected, dragging, grab; struct du_rect box; int id; };

struct st_app {
    struct du_ui *ui;
    struct st_host h;
    enum st_page page, back_page, rescue_return;   /* where Text and Rescue return */
    enum st_stop stop;
    int quit, restart_requested, power_requested;
    /* ACPI power button with no desktop (lead decision 2026-10-23): the
     * coalesced request count last seen, a request waiting for a free dialog
     * slot, and a confirmed power off that waits for the copy to stop. */
    uint32_t power_seen; int power_pending, power_after_copy; uint64_t power_poll_at;
    int unattended, answers_found;
    struct setup_answers answers;
    struct setup_inf_status status; int status_valid;
    struct setup_plan plan; int plan_ok;
    struct setup_log log;
    /* inventory (read once at start, refreshed by Rescue) */
    struct ciuki_devices_info devices; int devices_ok;
    struct ciuki_display_info display; int display_ok;
    unsigned fb_w, fb_h, fb_bpp; int fb_ok;
    struct ciuki_net_state net; int net_ok;
    uint64_t mem_kib; int mem_ok;
    struct st_disk disks[4]; int disk_count;
    struct st_volume volumes[26]; int volume_count;
    struct st_cd cd; int cd_ok;
    struct st_size sizes[SETUP_COMP_COUNT]; int sizes_ok;
    char release[65];
    struct st_hw_row rows[12]; unsigned row_count;
    char report[ST_REPORT_BYTES]; size_t report_length;
    char *link_text; size_t link_length;    /* CIUKIREP.TXT text received from the f9-07 program */
    /* pages */
    int kbd, tz, clock_utc, net_manual;
    int tz_chosen;                      /* G1: the user picked the zone (no keyboard default) */
    char clock_text[24]; int clock_edited; int64_t clock_shown;
    char status_line[96];
    char licence_names[ST_LICENCES_MAX][64]; char licence_labels[ST_LICENCES_MAX][96]; unsigned licence_count;
    struct st_view view;                    /* licence, summary, rescue, report lists */
    char pool[ST_POOL_BYTES]; size_t pool_used;
    struct du_textarea text; int text_ready; char text_title[96];
    char *file;                             /* ST_FILE_BYTES, allocated once */
    /* copy */
    struct setup_progress progress; char step[260];
    unsigned execute_calls; int executing, cancel, cancel_asked, copy_row_system, copy_row_programs;
    struct setup_error error; int error_valid; int failed_class;
    unsigned tip; uint64_t tip_deadline, last_frame;
    /* modal callbacks during execute */
    int modal_id, modal_result;
    /* layout of the current page (pages.c) and the open drop-down list */
    struct du_rect r[24];
    const char *warning, *adapter; int adapter_none;
    int popup, popup_sel, popup_top;
    /* restart */
    uint64_t countdown_deadline, eject_deadline; int restart_state;
    char dest_letter;
    uint64_t now;
    /* sec-user-02 */
    struct st_accounts acct;
    struct st_repair repair; int repair_state, repair_choice, repair_volumes;
    char repair_letters[8]; char repair_status[200];
};

/* app.c */
int st_init(struct st_app *, struct du_ui *, const struct st_host *);
void st_free(struct st_app *);
int st_turn(struct st_app *);              /* one input/tick/render/present cycle; 0 continues */
void st_run(struct st_app *);              /* until quit */
void st_key(struct st_app *, unsigned code, int value);   /* synthesised keys (tests) */
void st_goto(struct st_app *, enum st_page);
void st_render(struct st_app *);
const char *st_page_name(enum st_page);
void st_log(struct st_app *, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void st_record(struct st_app *, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Widget and dialog ids. */
enum {
    ST_ID_BACK = 1, ST_ID_NEXT, ST_ID_EXIT, ST_ID_CANCEL, ST_ID_RESTART, ST_ID_SHUTDOWN, ST_ID_SAVE_REPORT,
    ST_ID_RESCUE, ST_ID_COPY_LOGS, ST_ID_REPORT, ST_ID_CLOSE,
    ST_ID_VIEW = 20, ST_ID_ACCEPT, ST_ID_LINK_SAVE, ST_ID_WHOLE, ST_ID_FREE, ST_ID_ADVANCED, ST_ID_DEFAULTS,
    ST_ID_COMP = 30,                       /* + component index */
    ST_ID_USER = 40, ST_ID_COMPUTER, ST_ID_KBD, ST_ID_CLOCK, ST_ID_TZ, ST_ID_UTC,
    ST_ID_DHCP = 50, ST_ID_MANUAL, ST_ID_IP, ST_ID_MASK, ST_ID_GATEWAY, ST_ID_DNS, ST_ID_IPX, ST_ID_LOBBY,
    ST_ID_TEXT = 60,
    ST_ID_SECRET = 70,                     /* + field index (sec-user-02 masked password fields) */
    ST_ID_REPAIR = 76, ST_ID_REPAIR_RUN, ST_ID_REPAIR_VOLUME = 80, /* + volume index */
    ST_DLG_EXIT = 100, ST_DLG_HELP, ST_DLG_ERROR, ST_DLG_CANCEL, ST_DLG_MESSAGE, ST_DLG_PREPARE, ST_DLG_SHUTDOWN,
    ST_DLG_POWER, ST_DLG_REPAIR
};
/* app.c actions shared with pages.c */
void st_action(struct st_app *, int id);
void st_licence_open(struct st_app *, unsigned index);
void st_message(struct st_app *, const char *title, const char *text, int icon);
/* pages.c */
void st_page_build(struct st_app *);
void st_page_paint(struct st_app *);
int st_page_event(struct st_app *, const struct du_event *);
void st_page_collect(struct st_app *);    /* widgets -> answers */
int st_page_height(struct st_app *, enum st_page); /* content bottom (layout test) */
int st_page_primary(enum st_page);         /* id of the default button */
const char *st_help(enum st_page);
int st_popup_key(struct st_app *, unsigned code);
int st_popup_click(struct st_app *, int x, int y);
void st_components_defaults(struct st_app *);
/* hw.c */
void st_inventory(struct st_app *);
void st_hw_evaluate(struct st_app *);
const char *st_stop_code(enum st_stop);
void st_disk_size(char *out, size_t cap, uint64_t bytes);
const char *st_part_name(uint8_t type);
void st_disk_contents(const struct st_disk *, char *out, size_t cap);
/* catalog.c */
struct st_component { uint32_t bit; const char *id, *label; uint32_t requires; };
extern const struct st_component st_components[SETUP_COMP_COUNT];
struct st_keyboard { const char *id, *label; };
extern const struct st_keyboard st_keyboards[]; extern const unsigned st_keyboard_count;
/* G1: the bundled zones with their daylight-saving rules (ciuki/tzdata.h). */
#define st_zones ciuki_tz_zones
#define st_zone_count ((unsigned)CIUKI_TZ_ZONE_COUNT)
int st_zone_find(const char *id);                       /* -1 when not bundled */
int32_t st_zone_offset(int zone, int minutes, int64_t utc);         /* seconds east, zone<0: fixed minutes */
int64_t st_zone_local_to_utc(int zone, int minutes, int64_t local); /* wall time to UTC */
extern const char *const st_tips[]; extern const unsigned st_tip_count;
int st_zone_index(int minutes);   /* first bundled zone with this standard offset */
uint64_t st_required_estimate(const struct st_app *, uint32_t components);
uint64_t st_partition_sectors(uint64_t capacity);
const char *st_error_title(enum setup_error_class);
const char *st_error_text(enum setup_error_class);
const char *st_error_id(enum setup_error_class);
/* text.c: rich text in the shared desktop faces. ST_B toggles bold. */
#define ST_B "\001"
int st_text_width(int font, const char *s);
int st_text_line(struct du_canvas *, int font, int x, int y, int line_h, const char *s, uint32_t ink);
int st_text_wrap(struct du_canvas *, int font, struct du_rect box, int line_h, const char *s, uint32_t ink);
void st_mark_paint(struct du_canvas *, int x, int y, int size, enum st_mark);
/* view.c: scrollable lists of wrapped paragraphs and selectable entries. */
void st_view_reset(struct st_view *, int id, struct du_rect box);
void st_view_add(struct st_view *, const char *text, int entry, int font, uint32_t ink, int mark);
void st_view_layout(struct st_view *);
void st_view_paint(struct du_canvas *, const struct st_view *, int focused);
int st_view_key(struct st_view *, unsigned code);   /* 1 consumed */
int st_view_pointer(struct st_view *, const struct du_event *);
void st_view_wheel(struct st_view *, int ticks);
int st_view_click(struct st_view *, int x, int y);  /* entry index or -1 */
const char *st_pool(struct st_app *, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* rescue.c */
int st_rescue_copy(struct st_app *, char *result, size_t cap);
int st_save_report(struct st_app *, char *result, size_t cap);
int st_usb_volume(const struct st_app *);   /* index in volumes[] of the first USB FAT volume, or -1 */
/* accounts.c (sec-user-02): seed recipe and confirmed lost-password repair */
int st_password_valid(const char *);          /* 8..63 printable ASCII bytes */
int st_account_name_valid(const char *);      /* 1..31 [A-Za-z0-9_-], not reserved */
void st_secret_wipe(void *, size_t);
int st_seed_build(uint8_t out[CIUKI_PROVISION_SEED_BYTES], const char *name, const char *user_password,
                  const char *recovery_password, int (*entropy)(void *, void *, size_t), void *ctx);
int st_repair_inspect(const struct acct_io *, struct st_repair *);
int st_repair_execute(const struct acct_io *, const char *password, int (*entropy)(void *, void *, size_t), void *ctx,
                      struct st_repair *);
void st_accounts_clear(struct st_app *);      /* wipe secrets and the seed */
/* inf.c helpers used by the flow */
int st_answers_load(struct st_app *);       /* 1 found and valid, 0 none, -1 stop (status set) */
int st_licences_load(struct st_app *);
int st_text_load(struct st_app *, const char *title, const char *path, const char *text, size_t length);
void st_input_event(struct st_app *, const struct ciuki_input_event *);
#endif
