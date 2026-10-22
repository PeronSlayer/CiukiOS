/* SPDX-License-Identifier: MIT
 * Setup CD: the answers Setup collects (interactively or from SETUP.INF) and
 * the strict SETUP.INF schema-1 parser (docs/design/cd-setup-plan.md §8,
 * decision D10). Owned by f9-03 (apps/setup/inf.c and its host tests);
 * libsetup (f9-04) consumes struct setup_answers through setup_txn.h.
 *
 * The parser is pure: it reads a byte buffer and fills the answers, or stops
 * with a status id and the offending key. Disk rules (Disk=auto, model/serial,
 * Reinstall, EraseDisk consent) and the unattended completeness check run
 * against the read-only disk enumeration after parsing. */
#ifndef CIUKI_SETUP_INF_H
#define CIUKI_SETUP_INF_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SETUP_INF_VERSION 1
#define SETUP_INF_MAX_BYTES 16384u   /* larger files stop with file_too_large */

/* Components (the catalogue bitmask of struct setup_plan.components). The
 * system is implicit and always set. Dependencies: games need the DOS
 * personality; the Windows game lobby (a network answer) needs the Windows
 * personality. */
enum {
    SETUP_COMP_SYSTEM = 1u << 0, SETUP_COMP_DOS = 1u << 1, SETUP_COMP_WINDOWS = 1u << 2,
    SETUP_COMP_NETSURF = 1u << 3, SETUP_COMP_GAMES = 1u << 4, SETUP_COMP_DEVTOOLS = 1u << 5,
    SETUP_COMP_SOUNDFONT = 1u << 6, SETUP_COMP_SOURCES = 1u << 7
};
#define SETUP_COMP_COUNT 8u
#define SETUP_COMP_ALL 0xffu

/* SETUP.INF [Setup] keys; setup_answers.present has bit (1u << key) for
 * every key that holds a valid answer. */
enum setup_key {
    SETUP_KEY_VERSION, SETUP_KEY_ACCEPT_LICENCES, SETUP_KEY_ERASE_DISK, SETUP_KEY_DISK,
    SETUP_KEY_REINSTALL, SETUP_KEY_COMPONENTS, SETUP_KEY_USER_NAME, SETUP_KEY_COMPUTER_NAME,
    SETUP_KEY_KEYBOARD, SETUP_KEY_TIME_ZONE, SETUP_KEY_CLOCK, SETUP_KEY_NETWORK, SETUP_KEY_IPX,
    SETUP_KEY_RESTART, SETUP_KEY_COUNT
};
enum setup_disk_select { SETUP_DISK_UNSET, SETUP_DISK_ATA0, SETUP_DISK_MATCH, SETUP_DISK_AUTO };
enum setup_net_mode { SETUP_NET_UNSET, SETUP_NET_DHCP, SETUP_NET_STATIC, SETUP_NET_NONE };
enum setup_clock { SETUP_CLOCK_LOCAL, SETUP_CLOCK_UTC };
enum setup_restart { SETUP_RESTART_EJECT, SETUP_RESTART_WAIT, SETUP_RESTART_FORCE };

#define SETUP_USER_MAX 32u        /* characters (UTF-8 Latin-1 range) */
#define SETUP_COMPUTER_MAX 15u    /* NetBIOS limit; RFC 1123 label characters */
#define SETUP_TZ_MIN (-720)       /* minutes east of UTC */
#define SETUP_TZ_MAX 840

/* The installation log ring that libsetup writes to \SYSTEM\SETUP.LOG in
 * its configuration step. Setup appends its page transitions and every
 * callbacks.log line; the oldest bytes are dropped when it is full. Live
 * object owned by the application for the whole run. */
#define SETUP_LOG_BYTES 32768u
struct setup_log { char text[SETUP_LOG_BYTES]; size_t start, length; uint64_t dropped; };
static inline void setup_log_append(struct setup_log *log, const char *line)
{
    size_t n = strlen(line);
    for (size_t i = 0; i <= n; i++) {
        char ch = i < n ? line[i] : '\n';
        if (i == n && n && line[n - 1] == '\n') break;
        if (log->length == SETUP_LOG_BYTES) {
            log->start = (log->start + 1) % SETUP_LOG_BYTES; log->length--; log->dropped++;
        }
        log->text[(log->start + log->length) % SETUP_LOG_BYTES] = ch; log->length++;
    }
}
/* Copies the ring oldest first; returns the bytes copied (no terminator). */
static inline size_t setup_log_copy(const struct setup_log *log, char *out, size_t cap)
{
    size_t n = log->length < cap ? log->length : cap;
    for (size_t i = 0; i < n; i++) out[i] = log->text[(log->start + i) % SETUP_LOG_BYTES];
    return n;
}

struct setup_answers {
    uint32_t present;                      /* bit (1u << enum setup_key) per answered key */
    uint8_t accept_licences, erase_disk, reinstall;
    enum setup_disk_select disk;
    char disk_model[41], disk_serial[21];  /* SETUP_DISK_MATCH: IDENTIFY strings, trimmed */
    uint32_t components;                   /* SETUP_COMP_*; SETUP_COMP_SYSTEM always set */
    char user_name[4 * SETUP_USER_MAX + 1];
    char computer_name[SETUP_COMPUTER_MAX + 1]; /* upper case */
    char keyboard[8];                      /* layout id of the Control Panel page: "us", "it" */
    int16_t tz_minutes;                    /* offset east of UTC */
    enum setup_clock clock;                /* RTC convention; default local */
    int64_t clock_adjust_s;                /* interactive only: confirmed clock minus the system
                                              clock when confirmed; 0 keeps the current time */
    enum setup_net_mode network;
    uint8_t ip[4], prefix, gateway[4], dns[4]; /* SETUP_NET_STATIC; zero gateway/DNS = none */
    uint8_t ipx, lobby;                    /* lobby: not an INF key; on when Windows is selected */
    enum setup_restart restart;            /* default eject */
    uint8_t unattended;                    /* the answers came from SETUP.INF */
    char source[48];                       /* "D:\SETUP.INF", "F:\SETUP.INF" or empty */
    /* Records the application owns; libsetup writes them in its
     * configuration step (\SYSTEM\HARDWARE.TXT and \SYSTEM\SETUP.LOG). */
    const char *hardware_report; size_t hardware_report_length;
    const struct setup_log *log;
};

/* Stop status ids ([setup] status=stopped reason=<id> key=<k>). */
#define SETUP_ST_SYNTAX "bad_syntax"            /* key=line<n> */
#define SETUP_ST_VERSION "bad_version"
#define SETUP_ST_SECTION "unknown_section"
#define SETUP_ST_UNKNOWN_KEY "unknown_key"
#define SETUP_ST_DUPLICATE "duplicate_key"
#define SETUP_ST_VALUE "bad_value"
#define SETUP_ST_DEPENDENCY "dependency"
#define SETUP_ST_MISSING "missing_answer"
#define SETUP_ST_CONSENT "consent_missing"
#define SETUP_ST_AMBIGUOUS "ambiguous_disk"
#define SETUP_ST_NOT_FOUND "disk_not_found"
#define SETUP_ST_INSTALLED "already_installed"
#define SETUP_ST_TOO_LARGE "file_too_large"
#define SETUP_ST_READ "read_error"
struct setup_inf_status { char reason[24]; char key[40]; unsigned line; };

extern const char *const setup_inf_key_names[SETUP_KEY_COUNT];       /* "Version", "AcceptLicences", ... */
extern const char *const setup_component_names[SETUP_COMP_COUNT];    /* "system", "dos", ... */

/* Defaults of the interactive pages (system, DOS, Windows, NetSurf, games,
 * soundfont; DHCP, IPX and lobby on; keyboard "us"; UTC+00:00 local;
 * restart eject). */
void setup_answers_defaults(struct setup_answers *);
/* Parses SETUP.INF: 0, or -1 with *status. Sets unattended=1. */
int setup_inf_parse(const char *text, size_t length, struct setup_answers *, struct setup_inf_status *);

/* The read-only view of an ATA disk the disk rules need (from libdisk's
 * enumeration). installed: SETUP.STATE on its first volume says
 * phase=installed with release[]. occupied: any partition or filesystem. */
struct setup_inf_disk { uint8_t present, occupied, installed; char model[41], serial[21], release[65]; };
/* Disk=auto/ata0/model+serial, already_installed (unless Reinstall=yes) and
 * the EraseDisk consent, against disks[0..3] (index = ATA slot). 0 or -1. */
int setup_inf_disk_rules(const struct setup_answers *, const struct setup_inf_disk disks[4],
                         const char *release, struct setup_inf_status *);
/* Unattended completeness in page order: 0, or -1 with missing_answer and
 * the first key that has no answer. */
int setup_inf_complete(const struct setup_answers *, struct setup_inf_status *);
/* Validation helpers shared with the interactive pages. */
int setup_valid_user(const char *);
int setup_valid_computer(const char *);       /* letters, digits, '-', not all digits, no edge '-' */
int setup_parse_ipv4(const char *, uint8_t out[4]);
int setup_static_valid(const uint8_t ip[4], unsigned prefix, const uint8_t gateway[4], const uint8_t dns[4]);
int setup_components_valid(uint32_t components);     /* dependencies */
/* "[setup] status=stopped reason=<id> key=<k>" without a newline. */
int setup_status_line(char *out, size_t cap, const struct setup_inf_status *);
#endif
