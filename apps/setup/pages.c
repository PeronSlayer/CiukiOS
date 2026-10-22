/* SPDX-License-Identifier: MIT
 * Setup screens 2-10 and 12, the summary, failure, status, rescue and text
 * pages at the 640x480 design size. Copy marked "mockup" is the approved
 * English text of the "CiukiOS Setup from CD" artboards, verbatim; other
 * copy is listed in build/codex/f9-03-report.md for the lead. Layout follows
 * the artboards: 28 px wallpaper margin, the wizard frame with its title
 * bar, the 164 px Ciuki sidebar and a 376 px content column. */
#include "setup.h"
#include "../libdeskui/internal.h"
#include "../desktop/theme.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define FX 28
#define FY 28
#define FW 584
#define FH 405
#define TITLE_H 20
#define BODY_Y (FY + 2 + TITLE_H)
#define BODY_B (FY + FH - 2)
#define SIDE_X (FX + 2)
#define SIDE_W 164
#define CX (SIDE_X + SIDE_W + 20)
#define CY (BODY_Y + 18)
#define CW (FX + FW - 2 - 20 - CX)
#define CB (BODY_B - 18)
#define BTN_W 75
#define BTN_H 23
#define BTN_Y (CB - BTN_H)
#define FOOT_Y (FY + FH + 6)
#define H1 22
#define LP 16
#define LS 15
#define F_UI DESK_FONT_UI
#define F_BOLD DESK_FONT_BOLD
#define F_H1 DESK_FONT_TITLE
#define GREY 0x808080u
#define SILVER 0xc0c0c0u
#define WARN_RED 0x800000u
#define SECONDARY 0x404040u
#define LINK 0x000080u
#define WHITE 0xffffffu
#define EN "\xe2\x80\x93"
#define EM "\xe2\x80\x94"
#define MID "\xc2\xb7"

/* ---- copy ---- */
static const char *const steps[8] = {"Welcome", "Licence", "Hardware check", "Destination disk", "Components",
                                     "User and computer", "Network", "Copy files"};          /* mockup */
static const char F_KEYS[] = "F1 Help " MID " F3 Exit Setup " MID " F10 Rescue console";       /* mockup */
static const char F_LOG[] = "Setup log: C:\\System\\Logs\\SETUP.LOG";                        /* mockup */
static const char F_COPY[] = "Cancelling now leaves the disk unbootable; Setup can be restarted from the CD."; /* mockup */
static const char F_DONE[] = "Thank you for trying CiukiOS.";                                /* mockup */
static const char F_STOP[] = "F10 Rescue console (read-only tools still work on this machine)"; /* mockup */
static const char F_MEMORY[] = "Setup log kept in memory only";                              /* mockup */
static const char F_RESCUE[] = "F1 Help " MID " Esc Close";
static const char TAGLINE[] = "Classic computing, one woof at a time.";

static const char W_H[] = "Welcome to CiukiOS Setup";
static const char W_P1[] = "Setup installs CiukiOS 1.0 Beta on this computer: the Ciuki kernel, the desktop, the DOS and Windows game personalities, the NetSurf browser and the bundled shareware games.";
static const char W_P2[] = "The whole installation takes about 15 minutes on a 1998" EN "2004 PC. Setup will restart the computer once; remove the CD when asked.";
static const char W_BOX[] = ST_B "Before you begin" ST_B "\n" MID " Back up any data on the destination disk.\n" MID
                            " Connect the network cable if you want LAN play and Internet.\n" MID " Keep the CD in the drive until Setup asks to remove it.";
static const char L_H[] = "Licence agreements";
static const char L_P[] = "CiukiOS is free software. Read the licences of the system and of the bundled components; Setup installs the full texts in C:\\System\\Licenses.";
static const char *const L_LINES[] = {
    ST_B "CiukiOS kernel, desktop and tools " EM " GNU GPL v2.0 only" ST_B,
    "Unified DOS kernel (FreeDOS 1.3 fork) " EM " GNU GPL v2.0 or later; source archive on the CD in \\SOURCES.",
    "lwIP 2.2.1 " EM " BSD-3-Clause " MID " NetSurf 3.11 " EM " GPL v2 with OpenSSL exception " MID " BearSSL " EM " MIT",
    "FreeType " EM " FTL " MID " TinyGL " EM " BSD " MID " zlib, libpng " EM " zlib/libpng " MID " Tango icons " EM " Public Domain",
    "Reused Wine DLLs " EM " LGPL v2.1 or later " MID " jitterentropy " EM " BSD-3-Clause",
    "Shareware games: DOOM, Quake, Wolfenstein 3D " EM " unchanged shareware packages; see each VENDOR.DOC / licence file.",
};
static const char L_SOURCES[] = "Corresponding source on this CD: \\SOURCES\\CIUKIOS.ZIP (CiukiOS), \\SOURCES\\UNIDOS.ZIP (unified DOS kernel) and \\SOURCES\\WINE100.TXZ (Wine 10.0); \\LICENSES\\SOURCES.TXT says which archive belongs to which program.";
static const char L_NOTICE[] = "CiukiOS comes with ABSOLUTELY NO WARRANTY. It is free software, and you are welcome to redistribute it under the terms of the GNU General Public License version 2.";
static const char L_LIST[] = "Full texts (select a document and press Enter):";
static const char L_ACCEPT[] = "I have read the licences and accept them.";
static const char H_H[] = "Hardware check";
static const char H_P[] = "Setup detected this hardware. Items marked with a warning still work with a fallback; a stop sign would prevent installation.";
static const char H_LINK[] = "Save hardware report to USB\xe2\x80\xa6";
static const char D_H[] = "Where do you want to install CiukiOS?";
static const char D_WHOLE[] = ST_B "Use the entire disk" ST_B " (recommended): MBR, one FAT32 system volume C:";
static const char D_FREE[] = ST_B "Use the free space" ST_B " after the existing partitions (needs 2 GB free) " EM " Not available in this Beta";
static const char D_ADV[] = ST_B "Advanced" ST_B ": choose or create a partition, keep NTFS read-only volumes " EM " Not available in this Beta";
static const char D_INCOMPLETE[] = "A previous installation did not complete.";
static const char C_H[] = "Choose the components";
static const char C_P[] = "The system itself is always installed. Everything here can be added or removed later from Control Panel \xe2\x80\xba Programs.";
static const char C_DEFAULTS[] = "Restore defaults";
static const char U_H[] = "User and computer";
static const char U_P[] = "The computer name identifies this PC to the other players on the LAN. The keyboard layout and clock apply to the whole system.";
static const char U_BOX[] = ST_B "Why the clock matters" ST_B "\nSecure web sites (HTTPS) are refused when the clock is not set from the battery-backed RTC. Setup writes the time you confirm here to the RTC.";
static const char U_UTC[] = "The battery-backed clock keeps UTC";
static const char N_H[] = "Network";
static const char N_DHCP[] = ST_B "Automatic (DHCP)" ST_B " " EM " recommended for home routers and LAN parties with a router";
static const char N_MANUAL[] = ST_B "Manual" ST_B " " EM " for a direct crossover cable or a switch without a router";
static const char N_IPX[] = "Enable IPX for DOS games (DOOM, Duke 3D, Warcraft II\xe2\x80\xa6) on this adapter";
static const char N_LOBBY[] = "Enable the Windows game lobby (DirectPlay class, TCP/IP) on the LAN";
static const char N_NONE[] = "No network adapter was found. CiukiOS is installed without network settings; LAN play and Internet can be set up later.";
static const char S_H[] = "Ready to install";
static const char S_P[] = "Check the summary. Nothing has been written to any disk yet; Install erases the disk below and starts copying.";
static const char K_H[] = "Installing CiukiOS\xe2\x80\xa6";
static const char R_H[] = "Setup is complete";
static const char R_STEPS[3][200] = {
    "Remove the Setup CD from the drive.",
    "Press " ST_B "Restart" ST_B ". The computer starts from the hard disk; the first boot takes about a minute while the hardware is checked.",
    "If the screen stays black after the restart, press " ST_B "S" ST_B " at the boot menu for safe video.",
};
static const char R_LOGS[] = "Setup log saved to C:\\System\\Logs\\SETUP.LOG. A copy of the hardware report is in C:\\System\\Logs\\HARDWARE.TXT.";
static const char E_H[] = "This computer does not meet the minimum requirements";
static const char E_P[] = "Setup checked the hardware and found a problem that no fallback can solve. Nothing has been written to the disk.";
static const char X_H[] = "Setup did not complete";
static const char X_STATE[] = "The disk is not bootable; restart Setup from the CD to install again.";
static const char X_UNTOUCHED[] = "Nothing has been written to the disk.";
static const char Q_H[] = "Setup stopped";
static const char Q_P[] = "Setup is installing from an answer file and stopped because an answer is missing or does not match this computer. Nothing has been written to the disk.";
static const char RC_H[] = "Rescue console";
static const char RC_P[] = "Read-only tools: nothing is written to the hard disks. Logs and the hardware report are copied to the first USB drive.";

/* ---- helpers ---- */
static int para(int font, int w, int lh, const char *s) { return st_text_wrap(NULL, font, (struct du_rect){0, 0, w, 0}, lh, s, 0); }
static struct du_widget *button(struct st_app *a, int id, int x, int w, const char *label, int def, int disabled)
{
    struct du_widget *b = du_add(a->ui, DU_BUTTON, id, (struct du_rect){x, BTN_Y, w, BTN_H}, label);
    if (b) { b->def = (uint8_t)def; b->disabled = (uint8_t)disabled; }
    return b;
}
static void wizard_buttons(struct st_app *a, const char *next, int back_disabled, int next_disabled)
{
    int x = CX + CW - BTN_W;
    button(a, ST_ID_BACK, x - 2 * (BTN_W + 6), BTN_W, "< Back", 0, back_disabled);
    button(a, ST_ID_NEXT, x - (BTN_W + 6), BTN_W, next, 1, next_disabled);
    button(a, ST_ID_EXIT, x, BTN_W, "Exit", 0, 0);
}
static struct du_widget *add(struct st_app *a, enum du_kind k, int id, struct du_rect r, const char *text)
{ return du_add(a->ui, k, id, r, text ? text : ""); }
static const struct st_disk *disk0(const struct st_app *a)
{
    for (int i = 0; i < a->disk_count; i++) if (a->disks[i].present && !a->disks[i].slot && !a->disks[i].unit) return &a->disks[i];
    return NULL;
}
static uint64_t fat32_sectors(uint64_t sectors)
{   /* Microsoft FAT specification: FAT32 FATSz for 8 sectors per cluster, 2 FATs. */
    uint64_t v1 = sectors - 160, v2 = (256u * 8u + 2u) / 2u;
    return (v1 + v2 - 1) / v2;
}
static uint64_t available_bytes(const struct st_app *a)
{
    const struct st_disk *d = disk0(a);
    if (!d) return 0;
    uint64_t part = st_partition_sectors(d->sectors);
    if (part < 160 + 2) return 0;
    uint64_t fat = fat32_sectors(part);
    return part > 160 + 2 * fat ? (part - 160 - 2 * fat) * 512 : 0;
}
static void comma(char *out, size_t cap, uint64_t v)
{   /* "1 284": a space groups thousands as on the mockup's Copy screen. */
    char digits[32]; int n = snprintf(digits, sizeof(digits), "%" PRIu64, v), k = 0;
    for (int i = 0; i < n && k + 2 < (int)cap; i++) {
        if (i && (n - i) % 3 == 0) out[k++] = ' ';
        out[k++] = digits[i];
    }
    out[k] = 0;
}

int st_page_primary(enum st_page p)
{
    switch (p) {
    case ST_PAGE_COPY: return 0;
    case ST_PAGE_RESTART: case ST_PAGE_STOP: case ST_PAGE_FAILED: case ST_PAGE_STATUS: return ST_ID_RESTART;
    case ST_PAGE_RESCUE: case ST_PAGE_TEXT: return ST_ID_CLOSE;
    default: return ST_ID_NEXT;
    }
}

const char *st_help(enum st_page p)
{
    switch (p) {
    case ST_PAGE_WELCOME: return "Setup installs CiukiOS on ATA disk 0 (the primary master hard disk). Use Tab and Shift+Tab to move, Enter for the highlighted button, Esc to go back, F3 to exit and F10 for the read-only rescue console.";
    case ST_PAGE_LICENCE: return "Use the arrow keys to scroll the licences and Enter to read a full text. Select the check box with Space to accept the licences; Setup continues only after you accept them.";
    case ST_PAGE_HARDWARE: return "A warning means the device works with a fallback. Setup stops only when the display, ATA disk 0, the CD drive or the disk itself cannot be used. The report can be saved to a USB drive.";
    case ST_PAGE_DISK: return "This Beta installs only on the whole of ATA disk 0: every partition on it is erased. Nothing is written until you choose Install on the summary page.";
    case ST_PAGE_COMPONENTS: return "Select components with Space. The games need the DOS personality. Sizes include the space the FAT32 file system needs.";
    case ST_PAGE_USER: return "Type your name and a computer name for the LAN (letters, digits and hyphens, up to 15 characters). Use the arrow keys to choose the keyboard layout and the time zone. The clock is written to the RTC only while Setup copies the files.";
    case ST_PAGE_NETWORK: return "Choose Automatic (DHCP) when a router assigns addresses. Choose Manual for a crossover cable or a switch without a router; the gateway and the DNS server may be left empty.";
    case ST_PAGE_SUMMARY: return "Install erases ATA disk 0 and installs CiukiOS. Back returns to the previous pages; nothing has been written yet.";
    case ST_PAGE_COPY: return "Setup is copying the files. Cancel stops at the next safe point and leaves the disk unbootable; restart Setup from the CD to install again.";
    case ST_PAGE_RESTART: return "Remove the CD and restart. The first boot finishes the hardware setup.";
    case ST_PAGE_RESCUE: return "The rescue console reads the disks without writing to them. Copy logs to USB saves SETUP.LOG, SETUP.STATE, HARDWARE.TXT and the boot log of an installed system to the first USB drive.";
    default: return "Choose Restart to start the computer again, or press F10 for the read-only rescue console.";
    }
}

/* ---- build ---- */
static void build_welcome(struct st_app *a)
{
    int y = CY;
    a->r[0] = (struct du_rect){CX, y, CW, H1}; y += H1 + 12;
    int h = para(F_UI, CW, LP, W_P1); a->r[1] = (struct du_rect){CX, y, CW, h}; y += h + 12;
    h = para(F_UI, CW, LP, W_P2); a->r[2] = (struct du_rect){CX, y, CW, h}; y += h + 12;
    h = para(F_UI, CW - 20, LP, W_BOX); a->r[3] = (struct du_rect){CX, y, CW, h + 16};
    a->r[4] = (struct du_rect){CX, a->r[3].y + a->r[3].h, 0, 0};
    wizard_buttons(a, "Next >", 1, 0);
    du_focus(a->ui, ST_ID_NEXT);
}

static void build_licence(struct st_app *a)
{
    int y = CY;
    a->r[0] = (struct du_rect){CX, y, CW, H1}; y += H1 + 10;
    int h = para(F_UI, CW, LP, L_P); a->r[1] = (struct du_rect){CX, y, CW, h}; y += h + 10;
    int check = BTN_Y - 10 - 17;
    a->r[2] = (struct du_rect){CX, y, CW, check - 10 - y};
    st_view_reset(&a->view, ST_ID_VIEW, a->r[2]);
    for (unsigned i = 0; i < sizeof(L_LINES) / sizeof(L_LINES[0]); i++) st_view_add(&a->view, L_LINES[i], 0, F_UI, DU_INK, 0);
    st_view_add(&a->view, L_SOURCES, 0, F_UI, DU_INK, 0);
    st_view_add(&a->view, L_NOTICE, 0, F_UI, DU_INK, 0);
    if (a->licence_count) st_view_add(&a->view, L_LIST, 0, F_UI, GREY, 0);
    for (unsigned i = 0; i < a->licence_count; i++) st_view_add(&a->view, a->licence_labels[i], 1, F_UI, DU_INK, 0);
    st_view_layout(&a->view);
    add(a, DU_CANVAS, ST_ID_VIEW, a->r[2], NULL);
    struct du_widget *w = add(a, DU_CHECKBOX, ST_ID_ACCEPT, (struct du_rect){CX, check, du_label_width(L_ACCEPT) + 30, 17}, L_ACCEPT);
    if (w) w->value = a->answers.accept_licences;
    a->r[3] = (struct du_rect){CX, check, CW, 17};
    wizard_buttons(a, "Next >", 0, !a->answers.accept_licences);
    du_focus(a->ui, ST_ID_VIEW);
}

static int hw_text_width(void) { return CW - 4 - 8 - 18 - 6 - 92 - 6 - 8; }
static int hw_row_height(const struct st_hw_row *r, int width)
{
    int h = para(F_UI, width, LS, r->text);
    return h < 18 ? 18 : h + 3;
}

static void build_hardware(struct st_app *a)
{
    int y = CY;
    a->r[0] = (struct du_rect){CX, y, CW, H1}; y += H1 + 10;
    int h = para(F_UI, CW, LP, H_P); a->r[1] = (struct du_rect){CX, y, CW, h}; y += h + 10;
    int rows = 8;
    for (unsigned i = 0; i < a->row_count; i++) rows += hw_row_height(&a->rows[i], hw_text_width());
    int room = BTN_Y - 10 - y;
    a->r[2] = (struct du_rect){CX, y, CW, rows < room ? rows : room};
    a->r[3] = (struct du_rect){CX, y + rows, 0, 0};       /* unclipped bottom, for the layout test */
    int lw = st_text_width(F_UI, H_LINK) + 4;
    add(a, DU_CANVAS, ST_ID_LINK_SAVE, (struct du_rect){CX, BTN_Y + 3, lw, 17}, NULL);
    wizard_buttons(a, "Next >", 0, a->stop != ST_STOP_NONE);
    du_focus(a->ui, ST_ID_NEXT);
}

static int disk_rows(const struct st_app *a)
{
    int n = 1;
    for (int i = 0; i < a->disk_count; i++) n += a->disks[i].present != 0;
    for (int i = 0; i < a->volume_count; i++) n += a->volumes[i].usb != 0;
    return n;
}

static const char *erase_warning(struct st_app *a, const struct st_disk *d)
{
    if (!d || !d->part_count) return st_pool(a, "Everything on this disk will be erased.");
    char list[600]; size_t k = 0; list[0] = 0;
    for (unsigned i = 0; i < d->part_count && i < ST_PARTS; i++) {
        const struct st_part *p = &d->parts[i];
        char size[24], type[16], item[120];
        st_disk_size(size, sizeof(size), p->count * 512);
        const char *fs = p->fs[0] ? p->fs : st_part_name(p->type);
        if (!fs) { snprintf(type, sizeof(type), "type %02Xh", p->type); fs = type; }
        if (p->letter) snprintf(item, sizeof(item), "%c: %s%s(%s, %s)", p->letter, p->label, p->label[0] ? " " : "", fs, size);
        else snprintf(item, sizeof(item), "partition %u (%s, %s)", i + 1, fs, size);
        const char *sep = !i ? "" : i + 1 == d->part_count ? " and " : ", ";
        int n = snprintf(list + k, sizeof(list) - k, "%s%s", sep, item);
        if (n < 0 || (size_t)n >= sizeof(list) - k) break;
        k += (size_t)n;
    }
    return st_pool(a, "Everything on this disk will be erased, including %s.", list);
}

static void build_disk(struct st_app *a)
{
    const struct st_disk *d = disk0(a);
    int y = CY;
    a->r[0] = (struct du_rect){CX, y, CW, H1}; y += H1 + 10;
    a->r[1] = (struct du_rect){CX, y, CW, 8 + 18 * disk_rows(a)}; y += a->r[1].h + 10;
    int lw = CW - 22 - 18, fy = y + 16;
    const char *labels[3] = {D_WHOLE, D_FREE, D_ADV};
    for (int i = 0; i < 3; i++) {
        int h = para(F_UI, lw, LS, labels[i]);
        a->r[5 + i] = (struct du_rect){CX + 10 + 18, fy, lw, h};
        struct du_widget *w = add(a, DU_RADIO, ST_ID_WHOLE + i, (struct du_rect){CX + 10, fy, 16, LS}, NULL);
        if (w) { w->total = 1; w->value = i == 0; w->disabled = i != 0; }
        fy += h + 4;
    }
    a->r[2] = (struct du_rect){CX, y, CW, fy - y + 4}; y += a->r[2].h + 10;
    const char *warning = erase_warning(a, d);
    a->warning = warning;
    int inner = CW - 16, h = LS + 4 + 14 + 6;
    uint64_t sectors = d ? d->sectors : 0;
    if (sectors > ((uint64_t)1 << 28)) h += para(F_UI, inner, LS, "The disk is larger than 128 GB: CiukiOS uses the first 128 GB and the rest stays unallocated.") + 2;
    if (d && d->install == ST_INSTALL_INCOMPLETE) h += para(F_BOLD, inner, LS, D_INCOMPLETE) + 2;
    h += para(F_UI, inner, LS, warning);
    a->r[3] = (struct du_rect){CX, y, CW, h + 12};
    a->r[4] = (struct du_rect){CX, y + h + 12, 0, 0};
    wizard_buttons(a, "Next >", 0, !d || !d->installable);
    du_focus(a->ui, ST_ID_WHOLE);
}

static int comp_label_width(void) { return CW - 4 - 8 - 16 - 8 - 8 - 56 - 8; }
static void build_components(struct st_app *a)
{
    int y = CY;
    a->r[0] = (struct du_rect){CX, y, CW, H1}; y += H1 + 10;
    int h = para(F_UI, CW, LP, C_P); a->r[1] = (struct du_rect){CX, y, CW, h}; y += h + 10;
    int line = BTN_Y - 10 - LS, ry = y + 4;
    for (unsigned i = 0; i < SETUP_COMP_COUNT; i++) {
        int rh = para(F_UI, comp_label_width(), LS, st_components[i].label);
        if (rh < 19) rh = 19;
        struct du_widget *w = add(a, DU_CHECKBOX, ST_ID_COMP + (int)i, (struct du_rect){CX + 8, ry, 16, 17}, NULL);
        if (w) {
            w->value = (a->answers.components & st_components[i].bit) != 0;
            w->disabled = i == 0 || ((st_components[i].requires & ~a->answers.components) != 0);
        }
        a->r[8 + i] = (struct du_rect){CX + 8 + 16 + 8, ry, comp_label_width(), rh};
        ry += rh + 2;
    }
    a->r[2] = (struct du_rect){CX, y, CW, ry - y + 4};
    a->r[3] = (struct du_rect){CX, line, CW, LS};
    int lw = st_text_width(F_UI, C_DEFAULTS) + 4;
    add(a, DU_CANVAS, ST_ID_DEFAULTS, (struct du_rect){CX + CW - lw, line - 1, lw, LS + 2}, NULL);
    a->r[4] = (struct du_rect){CX, ry + 4, 0, 0};
    wizard_buttons(a, "Next >", 0, 0);
    du_focus(a->ui, ST_ID_COMP + 1);
}

static void clock_refresh(struct st_app *a)
{
    if (a->clock_edited) return;
    int64_t now = a->h.realtime ? a->h.realtime(a->h.ctx) : 0;
    time_t local = (time_t)(now + (int64_t)st_zones[a->tz].minutes * 60);
    struct tm tm; gmtime_r(&local, &tm);
    snprintf(a->clock_text, sizeof(a->clock_text), "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min);
    a->clock_shown = now;
    struct du_widget *w = du_get(a->ui, ST_ID_CLOCK);
    if (w) du_set_text(w, a->clock_text);
}

static void build_user(struct st_app *a)
{
    int y = CY;
    a->r[0] = (struct du_rect){CX, y, CW, H1}; y += H1 + 8;
    int h = para(F_UI, CW, LP, U_P); a->r[1] = (struct du_rect){CX, y, CW, h}; y += h + 8;
    int fx = CX + 130, fw = CW - 130;
    a->r[2] = (struct du_rect){CX, y, 120, 21};
    add(a, DU_EDIT, ST_ID_USER, (struct du_rect){fx, y, fw, 21}, a->answers.user_name); y += 27;
    a->r[3] = (struct du_rect){CX, y, 120, 21};
    add(a, DU_EDIT, ST_ID_COMPUTER, (struct du_rect){fx, y, fw, 21}, a->answers.computer_name); y += 27;
    a->r[4] = (struct du_rect){CX, y, 120, 21};
    add(a, DU_CANVAS, ST_ID_KBD, (struct du_rect){fx, y, fw, 21}, NULL); y += 27;
    a->r[5] = (struct du_rect){CX, y, 120, 21};
    add(a, DU_EDIT, ST_ID_CLOCK, (struct du_rect){fx, y, 130, 21}, a->clock_text); y += 27;
    add(a, DU_CANVAS, ST_ID_TZ, (struct du_rect){fx, y, fw, 21}, NULL); y += 27;
    struct du_widget *w = add(a, DU_CHECKBOX, ST_ID_UTC, (struct du_rect){fx, y, du_label_width(U_UTC) + 30, 17}, U_UTC);
    if (w) w->value = a->clock_utc;
    y += 17 + 8;
    h = para(F_UI, CW - 16, LS, U_BOX);
    a->r[6] = (struct du_rect){CX, y, CW, h + 12};
    a->r[7] = (struct du_rect){CX, y + h + 12, 0, 0};
    clock_refresh(a);
    wizard_buttons(a, "Next >", 0, 0);
    du_focus(a->ui, ST_ID_USER);
}

static const char *adapter_sentence(struct st_app *a)
{
    const struct ciuki_device_record *nic = NULL;
    for (unsigned i = 0; a->devices_ok && i < a->devices.count && i < CIUKI_DEVICES_MAX; i++)
        if (a->devices.devices[i].class_id == CIUKI_DEVICE_NETWORK && a->devices.devices[i].state == CIUKI_DEVICE_READY) { nic = &a->devices.devices[i]; break; }
    if (!nic) return NULL;
    const char *cable = a->net_ok && a->net.link == CIUKI_NET_LINK_DOWN ? ", cable not connected" :
                        a->net_ok && a->net.link == CIUKI_NET_LINK_UP ? ", cable connected" : "";
    return st_pool(a, "Adapter found: " ST_B "%.60s" ST_B "%s. Choose how this computer gets its address.", nic->name, cable);
}

static void build_network(struct st_app *a)
{
    const char *sentence = adapter_sentence(a);
    int none = !sentence;
    if (none) sentence = N_NONE;
    a->adapter = sentence; a->adapter_none = none;
    int y = CY;
    a->r[0] = (struct du_rect){CX, y, CW, H1}; y += H1 + 8;
    int h = para(F_UI, CW, LP, sentence); a->r[1] = (struct du_rect){CX, y, CW, h}; y += h + 8;
    int lw = CW - 22 - 18, fy = y + 16;
    const char *labels[2] = {N_DHCP, N_MANUAL};
    for (int i = 0; i < 2; i++) {
        int lh = para(F_UI, lw, LS, labels[i]);
        a->r[5 + i] = (struct du_rect){CX + 10 + 18, fy, lw, lh};
        struct du_widget *w = add(a, DU_RADIO, ST_ID_DHCP + i, (struct du_rect){CX + 10, fy, 16, LS}, NULL);
        if (w) { w->total = 2; w->value = i == a->net_manual; w->disabled = (uint8_t)none; }
        fy += lh + 6;
    }
    static const char *const names[4] = {"IP address", "Subnet mask", "Gateway", "DNS server"};
    char text[4][20] = {"", "", "", ""};
    if (a->answers.network == SETUP_NET_STATIC) {
        uint32_t mask = a->answers.prefix ? 0xffffffffu << (32 - a->answers.prefix) : 0;
        snprintf(text[0], 20, "%u.%u.%u.%u", a->answers.ip[0], a->answers.ip[1], a->answers.ip[2], a->answers.ip[3]);
        snprintf(text[1], 20, "%u.%u.%u.%u", mask >> 24, (mask >> 16) & 255, (mask >> 8) & 255, mask & 255);
        if (a->answers.gateway[0]) snprintf(text[2], 20, "%u.%u.%u.%u", a->answers.gateway[0], a->answers.gateway[1], a->answers.gateway[2], a->answers.gateway[3]);
        if (a->answers.dns[0]) snprintf(text[3], 20, "%u.%u.%u.%u", a->answers.dns[0], a->answers.dns[1], a->answers.dns[2], a->answers.dns[3]);
    }
    for (int i = 0; i < 4; i++) {
        int col = i & 1, rowy = fy + (i / 2) * 25, x = CX + 10 + col * 182;
        a->r[10 + i] = (struct du_rect){x, rowy, 70, 19};
        struct du_widget *w = add(a, DU_EDIT, ST_ID_IP + i, (struct du_rect){x + 74, rowy, 96, 19}, text[i]);
        if (w) w->disabled = (uint8_t)(!a->net_manual || none);
    }
    fy += 50;
    a->r[2] = (struct du_rect){CX, y, CW, fy - y + 2}; y += a->r[2].h + 8;
    fy = y + 16;
    const char *checks[2] = {N_IPX, N_LOBBY};
    for (int i = 0; i < 2; i++) {
        int lh = para(F_UI, lw, LS, checks[i]);
        a->r[7 + i] = (struct du_rect){CX + 10 + 18, fy, lw, lh};
        struct du_widget *w = add(a, DU_CHECKBOX, ST_ID_IPX + i, (struct du_rect){CX + 10, fy, 16, LS}, NULL);
        if (w) {
            w->value = i ? a->answers.lobby && (a->answers.components & SETUP_COMP_WINDOWS) : a->answers.ipx;
            w->disabled = i && !(a->answers.components & SETUP_COMP_WINDOWS);
        }
        fy += lh + 4;
    }
    a->r[3] = (struct du_rect){CX, y, CW, fy - y + 4};
    a->r[4] = (struct du_rect){CX, y + a->r[3].h, 0, 0};
    wizard_buttons(a, "Install", 0, 0);
    du_focus(a->ui, none ? ST_ID_IPX : ST_ID_DHCP + a->net_manual);
}

static void hex16(char *out, const uint8_t *b) { for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", b[i]); }

static void build_summary(struct st_app *a)
{
    int y = CY;
    a->r[0] = (struct du_rect){CX, y, CW, H1}; y += H1 + 8;
    int h = para(F_UI, CW, LP, S_P); a->r[1] = (struct du_rect){CX, y, CW, h}; y += h + 8;
    a->r[2] = (struct du_rect){CX, y, CW, BTN_Y - 10 - y};
    st_view_reset(&a->view, ST_ID_VIEW, a->r[2]);
    const struct setup_plan *p = &a->plan;
    char size[24], size2[24], size3[24], size4[24], size5[24], fp[24], model[48];
    snprintf(model, sizeof(model), "%s", p->disk.model);
    for (size_t n = strlen(model); n && model[n - 1] == ' '; n--) model[n - 1] = 0;
    st_disk_size(size, sizeof(size), p->disk.sectors * 512);
    st_view_add(&a->view, st_pool(a, ST_B "Disk:" ST_B " ATA 0:0 %s, serial %.20s, %s (%" PRIu64 " sectors)", model,
                p->disk.serial[0] ? p->disk.serial : "none", size, p->disk.sectors), 0, F_UI, DU_INK, 0);
    hex16(fp, p->disk.table_fingerprint);
    st_view_add(&a->view, st_pool(a, ST_B "Partition table fingerprint:" ST_B " %s\xe2\x80\xa6 (SHA-256)", fp), 0, F_UI, DU_INK, 0);
    if (!p->erased_partitions) st_view_add(&a->view, st_pool(a, ST_B "Erased:" ST_B " nothing; the disk holds no partition"), 0, F_UI, DU_INK, 0);
    for (unsigned i = 0; i < p->erased_partitions && i < 16; i++) {
        const char *fs = st_part_name(p->erased[i].type);
        st_disk_size(size2, sizeof(size2), p->erased[i].count * 512);
        st_view_add(&a->view, st_pool(a, ST_B "Erased:" ST_B " partition %u, type %02Xh%s%s%s, %s at LBA %" PRIu64 "%s%.11s",
                    i + 1, p->erased[i].type, fs ? " (" : "", fs ? fs : "", fs ? ")" : "", size2, p->erased[i].start,
                    p->erased[i].label[0] ? ", label " : "", p->erased[i].label), 0, F_UI, WARN_RED, 0);
    }
    st_disk_size(size, sizeof(size), p->partition_count * 512);
    st_view_add(&a->view, st_pool(a, ST_B "New layout:" ST_B " one FAT32 partition of %s at LBA %" PRIu64 ", volume CIUKIOS, Ciuki loader in LBA 1" EN "1023",
                size, p->partition_start), 0, F_UI, DU_INK, 0);
    st_disk_size(size, sizeof(size), p->required_bytes); st_disk_size(size2, sizeof(size2), p->overhead_bytes);
    st_disk_size(size3, sizeof(size3), p->reserve_bytes); st_disk_size(size4, sizeof(size4), p->free_after_bytes);
    st_disk_size(size5, sizeof(size5), p->required_bytes + p->overhead_bytes + p->reserve_bytes);
    st_view_add(&a->view, st_pool(a, ST_B "Space:" ST_B " %s of files + %s FAT allocation + %s pagefile reserve = %s; %s free after installation",
                size, size2, size3, size5, size4), 0, F_UI, DU_INK, 0);
    char list[200]; size_t k = 0; list[0] = 0;
    for (unsigned i = 0; i < SETUP_COMP_COUNT; i++) if (p->components & st_components[i].bit) {
        int n = snprintf(list + k, sizeof(list) - k, "%s%s", k ? ", " : "", setup_component_names[i]);
        if (n > 0 && (size_t)n < sizeof(list) - k) k += (size_t)n;
    }
    st_view_add(&a->view, st_pool(a, ST_B "Components:" ST_B " %s", list), 0, F_UI, DU_INK, 0);
    st_view_add(&a->view, st_pool(a, ST_B "Sources:" ST_B " C: boot image %.16s\xe2\x80\xa6, D: CD manifest %.16s\xe2\x80\xa6",
                p->source_image_sha[0] ? p->source_image_sha : "unknown", p->source_iso_sha[0] ? p->source_iso_sha : "unknown"), 0, F_UI, DU_INK, 0);
    const struct setup_answers *s = &p->answers;
    const char *kbd = s->keyboard;
    for (unsigned i = 0; i < st_keyboard_count; i++) if (!strcmp(st_keyboards[i].id, s->keyboard)) kbd = st_keyboards[i].label;
    st_view_add(&a->view, st_pool(a, ST_B "User:" ST_B " %s " MID " computer %s " MID " keyboard %s", s->user_name, s->computer_name, kbd), 0, F_UI, DU_INK, 0);
    int zone = st_zone_index(s->tz_minutes);
    st_view_add(&a->view, st_pool(a, ST_B "Clock:" ST_B " %s, the RTC keeps %s%s", zone >= 0 ? st_zones[zone].label : "UTC offset",
                s->clock == SETUP_CLOCK_UTC ? "UTC" : "local time", s->clock_adjust_s ? st_pool(a, ", set to %s when the files are copied", a->clock_text) : ""),
                0, F_UI, DU_INK, 0);
    if (s->network == SETUP_NET_STATIC)
        st_view_add(&a->view, st_pool(a, ST_B "Network:" ST_B " manual %u.%u.%u.%u/%u, gateway %u.%u.%u.%u, DNS %u.%u.%u.%u " MID " IPX %s " MID " game lobby %s",
                    s->ip[0], s->ip[1], s->ip[2], s->ip[3], s->prefix, s->gateway[0], s->gateway[1], s->gateway[2], s->gateway[3],
                    s->dns[0], s->dns[1], s->dns[2], s->dns[3], s->ipx ? "on" : "off", s->lobby ? "on" : "off"), 0, F_UI, DU_INK, 0);
    else st_view_add(&a->view, st_pool(a, ST_B "Network:" ST_B " %s " MID " IPX %s " MID " game lobby %s", s->network == SETUP_NET_DHCP ? "automatic (DHCP)" : "none",
                     s->ipx ? "on" : "off", s->lobby ? "on" : "off"), 0, F_UI, DU_INK, 0);
    st_view_add(&a->view, s->unattended ? st_pool(a, ST_B "Answers:" ST_B " %s (unattended)", s->source)
                                        : st_pool(a, ST_B "Answers:" ST_B " entered on these pages"), 0, F_UI, DU_INK, 0);
    st_view_layout(&a->view);
    add(a, DU_CANVAS, ST_ID_VIEW, a->r[2], NULL);
    wizard_buttons(a, "Install", 0, 0);
    du_focus(a->ui, ST_ID_VIEW);
}

static void build_restart(struct st_app *a)
{
    button(a, ST_ID_RESTART, CX + CW - 2 * BTN_W - 6, BTN_W, "Restart", 1, 0);
    button(a, ST_ID_SHUTDOWN, CX + CW - BTN_W, BTN_W, "Shut down", 0, 0);
    du_focus(a->ui, ST_ID_RESTART);
}

static void build_stop(struct st_app *a)
{
    if (a->page == ST_PAGE_STOP) button(a, ST_ID_SAVE_REPORT, CX + CW - BTN_W - 6 - 130, 130, "Save report to USB", 0, 0);
    if (a->page == ST_PAGE_FAILED) button(a, ST_ID_RESCUE, CX + CW - 2 * BTN_W - 6, BTN_W, "Rescue", 0, 0);
    button(a, ST_ID_RESTART, CX + CW - BTN_W, BTN_W, "Restart", 1, 0);
    du_focus(a->ui, ST_ID_RESTART);
}

static void build_rescue(struct st_app *a)
{
    int y = CY;
    a->r[0] = (struct du_rect){CX, y, CW, H1}; y += H1 + 8;
    int h = para(F_UI, CW, LP, RC_P); a->r[1] = (struct du_rect){CX, y, CW, h}; y += h + 8;
    a->r[3] = (struct du_rect){CX, BTN_Y - 10 - LS * 2, CW, LS * 2};
    a->r[2] = (struct du_rect){CX, y, CW, a->r[3].y - 8 - y};
    st_view_reset(&a->view, ST_ID_VIEW, a->r[2]);
    char size[24], model[48];
    for (int i = 0; i < a->disk_count; i++) {
        const struct st_disk *d = &a->disks[i];
        snprintf(model, sizeof(model), "%s", d->model);
        for (size_t n = strlen(model); n && model[n - 1] == ' '; n--) model[n - 1] = 0;
        st_disk_size(size, sizeof(size), d->sectors * (d->sector_size ? d->sector_size : 512));
        st_view_add(&a->view, st_pool(a, ST_B "ATA %u:%u" ST_B " %s, %s%s", d->slot, d->unit, model, size,
                    d->install == ST_INSTALL_DONE ? ", CiukiOS installed" : d->install == ST_INSTALL_INCOMPLETE ? ", installation incomplete" : ""), 0, F_UI, DU_INK, 0);
        for (unsigned p = 0; p < d->part_count && p < ST_PARTS; p++) {
            const struct st_part *q = &d->parts[p];
            const char *fs = q->fs[0] ? q->fs : st_part_name(q->type);
            st_disk_size(size, sizeof(size), q->count * 512);
            st_view_add(&a->view, st_pool(a, "    %c%s partition %u, %s, %s%s%s", q->letter ? q->letter : '-', q->letter ? ":" : "", p + 1,
                        fs ? fs : "unknown type", size, q->label[0] ? ", " : "", q->label), 0, F_UI, DU_INK, 0);
        }
    }
    if (!a->disk_count) st_view_add(&a->view, "No ATA hard disk was found.", 0, F_UI, DU_INK, 0);
    for (int i = 0; i < a->volume_count; i++) {
        const struct st_volume *v = &a->volumes[i];
        st_view_add(&a->view, st_pool(a, ST_B "%c:" ST_B " %s%s%s%s", v->letter, v->usb ? "USB drive" : v->letter == 'C' ? "Setup CD boot image" :
                    v->letter == 'D' ? "Setup CD" : "volume", v->label[0] ? ", " : "", v->label, v->usb && !v->writable ? ", read-only" : ""), 0, F_UI, DU_INK, 0);
    }
    st_view_layout(&a->view);
    add(a, DU_CANVAS, ST_ID_VIEW, a->r[2], NULL);
    int x = CX + CW - BTN_W;
    button(a, ST_ID_COPY_LOGS, x - 6 - 110 - 6 - 120, 120, "Copy logs to USB", 0, 0);
    button(a, ST_ID_REPORT, x - 6 - 110, 110, "Hardware report", 0, 0);
    button(a, ST_ID_CLOSE, x, BTN_W, "Close", 1, 0);
    du_focus(a->ui, ST_ID_COPY_LOGS);
}

static void build_text(struct st_app *a)
{
    int y = CY;
    a->r[0] = (struct du_rect){CX, y, CW, H1}; y += H1 + 8;
    a->r[1] = (struct du_rect){CX, y, CW, BTN_Y - 10 - y};
    struct du_widget *w = add(a, DU_TEXTAREA, ST_ID_TEXT, a->r[1], NULL);
    if (w) w->data = &a->text;
    if (a->text_ready) du_ta_fit(&a->text, a->r[1]);
    int x = CX + CW - BTN_W;
    if (a->back_page == ST_PAGE_RESCUE || a->back_page == ST_PAGE_STOP || a->back_page == ST_PAGE_HARDWARE)
        button(a, ST_ID_SAVE_REPORT, x - 6 - 130, 130, "Save report to USB", 0, 0);
    button(a, ST_ID_CLOSE, x, BTN_W, "Close", 1, 0);
    du_focus(a->ui, ST_ID_TEXT);
}

void st_page_build(struct st_app *a)
{
    struct du_ui *u = a->ui;
    u->count = 0; u->focus = -1; u->capture = -1; u->hover = -1;
    a->pool_used = 0; a->popup = 0;
    memset(a->r, 0, sizeof(a->r));
    switch (a->page) {
    case ST_PAGE_WELCOME: build_welcome(a); break;
    case ST_PAGE_LICENCE: build_licence(a); break;
    case ST_PAGE_HARDWARE: build_hardware(a); break;
    case ST_PAGE_DISK: build_disk(a); break;
    case ST_PAGE_COMPONENTS: build_components(a); break;
    case ST_PAGE_USER: build_user(a); break;
    case ST_PAGE_NETWORK: build_network(a); break;
    case ST_PAGE_SUMMARY: build_summary(a); break;
    case ST_PAGE_COPY:
        button(a, ST_ID_CANCEL, CX + CW - BTN_W, BTN_W, "Cancel", 0, 0);
        du_focus(u, ST_ID_CANCEL);
        break;
    case ST_PAGE_RESTART: build_restart(a); break;
    case ST_PAGE_STOP: case ST_PAGE_FAILED: case ST_PAGE_STATUS: build_stop(a); break;
    case ST_PAGE_RESCUE: build_rescue(a); break;
    case ST_PAGE_TEXT: build_text(a); break;
    case ST_PAGES: break;
    }
    u->dirty = 1;
}

/* Content bottom of the page's own layout: the layout test checks that every
 * page's content ends above the button row at 640x480. */
int st_page_height(struct st_app *a, enum st_page p)
{
    switch (p) {
    case ST_PAGE_WELCOME: case ST_PAGE_DISK: case ST_PAGE_COMPONENTS: case ST_PAGE_NETWORK: return a->r[4].y;
    case ST_PAGE_HARDWARE: return a->r[3].y;
    case ST_PAGE_USER: return a->r[7].y;
    default: return 0;
    }
}

/* ---- collect: widgets -> answers ---- */
static const char *text_of(struct st_app *a, int id) { struct du_widget *w = du_get(a->ui, id); return w ? w->text : ""; }
static int value_of(struct st_app *a, int id) { struct du_widget *w = du_get(a->ui, id); return w ? w->value : 0; }

void st_page_collect(struct st_app *a)
{
    struct setup_answers *s = &a->answers;
    switch (a->page) {
    case ST_PAGE_LICENCE: s->accept_licences = (uint8_t)value_of(a, ST_ID_ACCEPT); break;
    case ST_PAGE_COMPONENTS: {
        uint32_t c = SETUP_COMP_SYSTEM;
        for (unsigned i = 1; i < SETUP_COMP_COUNT; i++) if (value_of(a, ST_ID_COMP + (int)i)) c |= st_components[i].bit;
        s->components = c;
        if (!(c & SETUP_COMP_WINDOWS)) s->lobby = 0;
        break;
    }
    case ST_PAGE_USER: {
        const char *name = text_of(a, ST_ID_USER);
        while (*name == ' ') name++;
        snprintf(s->user_name, sizeof(s->user_name), "%s", name);
        for (size_t n = strlen(s->user_name); n && s->user_name[n - 1] == ' '; n--) s->user_name[n - 1] = 0;
        snprintf(s->computer_name, sizeof(s->computer_name), "%.15s", text_of(a, ST_ID_COMPUTER));
        for (char *p = s->computer_name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        snprintf(s->keyboard, sizeof(s->keyboard), "%s", st_keyboards[a->kbd].id);
        s->tz_minutes = (int16_t)st_zones[a->tz].minutes;
        a->clock_utc = value_of(a, ST_ID_UTC);
        s->clock = a->clock_utc ? SETUP_CLOCK_UTC : SETUP_CLOCK_LOCAL;
        snprintf(a->clock_text, sizeof(a->clock_text), "%s", text_of(a, ST_ID_CLOCK));
        break;
    }
    case ST_PAGE_NETWORK:
        a->net_manual = value_of(a, ST_ID_MANUAL);
        s->ipx = (uint8_t)value_of(a, ST_ID_IPX);
        s->lobby = (uint8_t)(value_of(a, ST_ID_LOBBY) && (s->components & SETUP_COMP_WINDOWS));
        break;
    default: break;
    }
}

/* ---- painting ---- */
static void text_at(struct du_canvas *c, int font, int x, int y, int lh, const char *s, uint32_t ink) { st_text_line(c, font, x, y, lh, s, ink); }
static int wrap(struct du_canvas *c, int font, struct du_rect r, int lh, const char *s, uint32_t ink) { return st_text_wrap(c, font, r, lh, s, ink); }
static void right_text(struct du_canvas *c, int font, int right, int y, int lh, const char *s, uint32_t ink)
{ text_at(c, font, right - st_text_width(font, s), y, lh, s, ink); }

static int step_of(const struct st_app *a)
{
    enum st_page p = a->page == ST_PAGE_TEXT ? a->back_page : a->page;
    if (p <= ST_PAGE_NETWORK) return (int)p;
    if (p == ST_PAGE_SUMMARY || p == ST_PAGE_COPY) return 7;
    return -1;
}

static void portrait(struct st_app *a, struct du_canvas *c, int x, int y)
{
    du_fill(c, (struct du_rect){x, y, ST_PORTRAIT + 4, ST_PORTRAIT + 4}, SILVER);
    if (!a->h.portrait) { du_fill(c, (struct du_rect){x + 2, y + 2, ST_PORTRAIT, ST_PORTRAIT}, 0x375564u); return; }
    for (int row = 0; row < ST_PORTRAIT; row++) {
        int py = y + 2 + row;
        if (py < c->clip.y || py >= c->clip.y + c->clip.h) continue;
        const uint32_t *src = a->h.portrait + (row * 256 / ST_PORTRAIT) * 256;
        uint32_t *dst = c->pixels + (size_t)py * (size_t)c->width + x + 2;
        for (int col = 0; col < ST_PORTRAIT; col++) {
            int px = x + 2 + col;
            if (px >= c->clip.x && px < c->clip.x + c->clip.w) dst[col] = src[col * 256 / ST_PORTRAIT] & 0xffffffu;
        }
    }
}

static void frame(struct st_app *a, struct du_canvas *c)
{
    du_fill(c, (struct du_rect){0, 0, c->width, c->height}, ST_BACKGROUND);
    struct du_rect f = {FX, FY, FW, FH};
    du_fill(c, f, DU_FACE);
    du_edge(c, f, DU_RAISED);
    struct desk_paint p = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
    desk_paint_hgradient(&p, FX + 2, FY + 2, FW - 4, TITLE_H, desk_theme->title[0], desk_theme->title[1]);
    text_at(c, F_BOLD, FX + 8, FY + 2, TITLE_H, "CiukiOS Setup", WHITE);
    const char *right = NULL; char step[24];
    int s = step_of(a);
    if (s >= 0) { snprintf(step, sizeof(step), "Step %d of 8", s + 1); right = step; }
    else if (a->page == ST_PAGE_RESTART) right = "Finished";
    else if (a->page == ST_PAGE_STOP) right = "Setup cannot continue";
    else if (a->page == ST_PAGE_FAILED) right = "Setup did not complete";
    else if (a->page == ST_PAGE_STATUS) right = "Setup stopped";
    else right = "Rescue console";
    right_text(c, F_UI, FX + FW - 8, FY + 2, TITLE_H, right, WHITE);
    /* Sidebar: the approved Ciuki portrait, the release, the tagline, the steps. */
    du_fill(c, (struct du_rect){SIDE_X, BODY_Y, SIDE_W, BODY_B - BODY_Y}, ST_SIDEBAR);
    int px = SIDE_X + (SIDE_W - ST_PORTRAIT - 4) / 2, py = BODY_Y + 18;
    portrait(a, c, px, py);
    int y = py + ST_PORTRAIT + 4 + 12;
    const char *release = "CiukiOS 1.0 Beta";
    text_at(c, F_H1, SIDE_X + (SIDE_W - st_text_width(F_H1, release)) / 2, y, 17, release, WHITE);
    y += 17 + 12;
    if (a->page == ST_PAGE_WELCOME || a->page == ST_PAGE_RESTART) {
        /* Centred two-line tagline. */
        const char *t = TAGLINE, *space = NULL;
        for (const char *q = t; *q; q++) if (*q == ' ' && st_text_width(F_UI, t) > SIDE_W - 20) {
            char head[64]; size_t n = (size_t)(q - t); if (n >= sizeof(head)) break;
            memcpy(head, t, n); head[n] = 0;
            if (st_text_width(F_UI, head) <= SIDE_W - 20) space = q;
        }
        if (space) {
            char head[64]; size_t n = (size_t)(space - t); memcpy(head, t, n); head[n] = 0;
            text_at(c, F_UI, SIDE_X + (SIDE_W - st_text_width(F_UI, head)) / 2, y, LS, head, SILVER);
            text_at(c, F_UI, SIDE_X + (SIDE_W - st_text_width(F_UI, space + 1)) / 2, y + LS, LS, space + 1, SILVER);
        } else text_at(c, F_UI, SIDE_X + (SIDE_W - st_text_width(F_UI, t)) / 2, y, LS, t, SILVER);
    }
    if (s >= 0) {
        int sy = BODY_B - 18 - 8 * 17;
        for (int i = 0; i < 8; i++) {
            uint32_t ink = i == s ? WHITE : SILVER;
            if (i == s) du_triangle(c, SIDE_X + 13, sy + i * 17 + 8, 3, 7, WHITE);
            text_at(c, F_UI, SIDE_X + 10 + (i == s ? 10 : 0), sy + i * 17, 17, steps[i], ink);
        }
    }
    /* Footer on the wallpaper. */
    const char *left = F_KEYS, *log = F_LOG;
    if (a->page == ST_PAGE_COPY) left = F_COPY;
    else if (a->page == ST_PAGE_RESTART) left = F_DONE;
    else if (a->page == ST_PAGE_STOP || a->page == ST_PAGE_FAILED || a->page == ST_PAGE_STATUS) { left = F_STOP; log = F_MEMORY; }
    else if (a->page == ST_PAGE_RESCUE || (a->page == ST_PAGE_TEXT && s < 0)) { left = F_RESCUE; log = F_MEMORY; }
    text_at(c, F_UI, FX, FOOT_Y, LS, left, SILVER);
    right_text(c, F_UI, FX + FW, FOOT_Y, LS, log, SILVER);
}

static void heading(struct du_canvas *c, struct du_rect r, const char *s) { text_at(c, F_H1, r.x, r.y, r.h, s, DU_INK); }
static void well(struct du_canvas *c, struct du_rect r) { du_well(c, r, DU_WINDOW); }
static void legend(struct du_canvas *c, struct du_rect r, const char *s)
{
    du_edge(c, (struct du_rect){r.x, r.y + 7, r.w, r.h - 7}, DU_ETCHED);
    int w = st_text_width(F_UI, s);
    du_fill(c, (struct du_rect){r.x + 6, r.y, w + 8, LS}, DU_FACE);
    text_at(c, F_UI, r.x + 10, r.y, LS, s, DU_INK);
}

static void paint_welcome(struct st_app *a, struct du_canvas *c)
{
    heading(c, a->r[0], W_H);
    wrap(c, F_UI, a->r[1], LP, W_P1, DU_INK);
    wrap(c, F_UI, a->r[2], LP, W_P2, DU_INK);
    well(c, a->r[3]);
    wrap(c, F_UI, (struct du_rect){a->r[3].x + 10, a->r[3].y + 8, CW - 20, a->r[3].h - 16}, LP, W_BOX, DU_INK);
}

static void paint_hardware_rows(struct st_app *a, struct du_canvas *c, struct du_rect box, int name_w)
{
    struct du_rect old = c->clip;
    c->clip = du_intersection(old, (struct du_rect){box.x + 2, box.y + 2, box.w - 4, box.h - 4});
    int y = box.y + 4, tw = box.w - 4 - 8 - 18 - 6 - name_w - 6 - 8;
    for (unsigned i = 0; i < a->row_count; i++) {
        const struct st_hw_row *r = &a->rows[i];
        if (a->page == ST_PAGE_STOP && r->mark != ST_MARK_STOP) continue;
        st_mark_paint(c, box.x + 6, y + 3, 12, (enum st_mark)r->mark);
        text_at(c, F_BOLD, box.x + 6 + 18 + 6, y, 18, r->name, DU_INK);
        int h = wrap(c, F_UI, (struct du_rect){box.x + 6 + 18 + 6 + name_w + 6, y + 1, tw, 0x7fff}, LS, r->text, DU_INK);
        y += h < 18 ? 18 : h + 3;
    }
    if (a->page == ST_PAGE_STOP) {
        /* "Everything else": the categories that passed. */
        char list[160] = ""; size_t k = 0;
        static const char *const names[] = {"Firmware", "Hard disk", "CD-ROM", "Display", "Input"};
        static const char *const lower[] = {"firmware", "disk", "CD-ROM", "display", "input"};
        unsigned ok = 0, total = 0;
        for (unsigned n = 0; n < 5; n++) for (unsigned i = 0; i < a->row_count; i++)
            if (!strcmp(a->rows[i].name, names[n]) && a->rows[i].mark != ST_MARK_STOP) ok++;
        for (unsigned n = 0; n < 5; n++) for (unsigned i = 0; i < a->row_count; i++) {
            if (strcmp(a->rows[i].name, names[n]) || a->rows[i].mark == ST_MARK_STOP) continue;
            total++;
            int w = snprintf(list + k, sizeof(list) - k, "%s%s", total == 1 ? "" : total == ok ? " and " : ", ", lower[n]);
            if (w > 0 && (size_t)w < sizeof(list) - k) k += (size_t)w;
        }
        if (k) {
            list[0] = (char)(list[0] >= 'a' && list[0] <= 'z' ? list[0] - 32 : list[0]);
            st_mark_paint(c, box.x + 6, y + 3, 12, ST_MARK_OK);
            text_at(c, F_BOLD, box.x + 6 + 18 + 6, y, 18, "Everything else", DU_INK);
            char line[200]; snprintf(line, sizeof(line), "%s %s supported.", list, ok == 1 ? "is" : "are");
            wrap(c, F_UI, (struct du_rect){box.x + 6 + 18 + 6 + name_w + 6, y + 1, tw, 0x7fff}, LS, line, DU_INK);
        }
    }
    c->clip = old;
}

static void paint_licence(struct st_app *a, struct du_canvas *c)
{
    heading(c, a->r[0], L_H);
    wrap(c, F_UI, a->r[1], LP, L_P, DU_INK);
}

static void paint_hardware(struct st_app *a, struct du_canvas *c)
{
    heading(c, a->r[0], H_H);
    wrap(c, F_UI, a->r[1], LP, H_P, DU_INK);
    well(c, a->r[2]);
    paint_hardware_rows(a, c, a->r[2], 92);
}

static void paint_disk(struct st_app *a, struct du_canvas *c)
{
    const struct st_disk *d = disk0(a);
    heading(c, a->r[0], D_H);
    struct du_rect t = a->r[1];
    well(c, t);
    int cols[4] = {t.x + 10, t.x + 10 + 70, t.x + t.w - 10 - 140, t.x + t.w - 10 - 70};
    static const char *const head[4] = {"Disk", "Model", "Size", "Contents"};
    for (int i = 0; i < 4; i++) text_at(c, F_UI, cols[i], t.y + 4, 18, head[i], GREY);
    int y = t.y + 4 + 18;
    struct du_rect old = c->clip;
    for (int i = 0; i < a->disk_count; i++) {
        const struct st_disk *k = &a->disks[i];
        if (!k->present) continue;
        int selected = k == d;
        uint32_t ink = selected ? DU_SELECT_INK : k->installable || selected ? DU_INK : GREY;
        if (selected) du_fill(c, (struct du_rect){t.x + 2, y, t.w - 4, 18}, DU_SELECT);
        char cell[64], model[48];
        snprintf(cell, sizeof(cell), "ATA %u:%u", k->slot, k->unit);
        text_at(c, F_UI, cols[0], y, 18, cell, ink);
        snprintf(model, sizeof(model), "%s", k->model);
        for (size_t n = strlen(model); n && model[n - 1] == ' '; n--) model[n - 1] = 0;
        c->clip = du_intersection(old, (struct du_rect){cols[1], y, cols[2] - cols[1] - 6, 18});
        text_at(c, F_UI, cols[1], y, 18, model, ink);
        c->clip = old;
        st_disk_size(cell, sizeof(cell), k->sectors * (k->sector_size ? k->sector_size : 512));
        text_at(c, F_UI, cols[2], y, 18, cell, ink);
        st_disk_contents(k, cell, sizeof(cell));
        c->clip = du_intersection(old, (struct du_rect){cols[3], y, t.x + t.w - 4 - cols[3], 18});
        text_at(c, F_UI, cols[3], y, 18, cell, ink);
        c->clip = old;
        y += 18;
    }
    for (int i = 0; i < a->volume_count; i++) {
        const struct st_volume *v = &a->volumes[i];
        if (!v->usb) continue;
        char cell[48];
        snprintf(cell, sizeof(cell), "USB %c:", v->letter);
        text_at(c, F_UI, cols[0], y, 18, cell, GREY);
        c->clip = du_intersection(old, (struct du_rect){cols[1], y, cols[2] - cols[1] - 6, 18});
        text_at(c, F_UI, cols[1], y, 18, v->label[0] ? v->label : "USB drive", GREY);
        c->clip = old;
        snprintf(cell, sizeof(cell), "%s data", v->fs[0] ? v->fs : "FAT");
        text_at(c, F_UI, cols[3], y, 18, cell, GREY);
        y += 18;
    }
    legend(c, a->r[2], "Installation type");
    const char *labels[3] = {D_WHOLE, D_FREE, D_ADV};
    for (int i = 0; i < 3; i++) wrap(c, F_UI, a->r[5 + i], LS, labels[i], i ? GREY : DU_INK);
    struct du_rect l = a->r[3];
    well(c, l);
    char title[64];
    snprintf(title, sizeof(title), "Resulting layout on ATA %u:%u", d ? d->slot : 0, d ? d->unit : 0);
    int x = l.x + 8, w = l.w - 16;
    y = l.y + 6;
    text_at(c, F_BOLD, x, y, LS, title, DU_INK); y += LS + 4;
    uint64_t sectors = d ? d->sectors : 0, part = st_partition_sectors(sectors);
    int used = sectors > part + 2048 && sectors ? (int)((uint64_t)(w - 4) * (part + 2048) / sectors) : w - 4;
    if (used < 40) used = 40;
    du_outline(c, (struct du_rect){x, y, w, 14}, 0x000000u);
    du_fill(c, (struct du_rect){x + 1, y + 1, 2, 12}, 0x000000u);
    du_fill(c, (struct du_rect){x + 3, y + 1, used - 2, 12}, 0x000080u);
    if (used < w - 4) du_fill(c, (struct du_rect){x + 1 + used, y + 1, w - 2 - used, 12}, SILVER);
    char bar[160], size[24];
    st_disk_size(size, sizeof(size), part * 512);
    snprintf(bar, sizeof(bar), "C: CIUKIOS  FAT32  %s  (system, programs, games, pagefile 16 MB)", size);
    c->clip = du_intersection(old, (struct du_rect){x + 3, y + 1, used - 4, 12});
    text_at(c, F_UI, x + 7, y, 14, bar, WHITE);
    c->clip = old;
    y += 14 + 6;
    if (sectors > ((uint64_t)1 << 28))
        y += wrap(c, F_UI, (struct du_rect){x, y, w, 0x7fff}, LS, "The disk is larger than 128 GB: CiukiOS uses the first 128 GB and the rest stays unallocated.", DU_INK) + 2;
    if (d && d->install == ST_INSTALL_INCOMPLETE) y += wrap(c, F_BOLD, (struct du_rect){x, y, w, 0x7fff}, LS, D_INCOMPLETE, WARN_RED) + 2;
    wrap(c, F_UI, (struct du_rect){x, y, w, 0x7fff}, LS, a->warning ? a->warning : "", WARN_RED);
}

static void paint_components(struct st_app *a, struct du_canvas *c)
{
    heading(c, a->r[0], C_H);
    wrap(c, F_UI, a->r[1], LP, C_P, DU_INK);
    well(c, a->r[2]);
    uint32_t chosen = 0;
    for (unsigned i = 0; i < SETUP_COMP_COUNT; i++) {
        struct du_widget *w = du_get(a->ui, ST_ID_COMP + (int)i);
        if (w && w->value) chosen |= st_components[i].bit;
        uint32_t ink = w && w->disabled ? GREY : DU_INK;
        struct du_rect r = a->r[8 + i];
        wrap(c, F_UI, (struct du_rect){r.x, r.y + 1, r.w, r.h}, LS, st_components[i].label, ink);
        if (a->sizes_ok && a->sizes[i].known) {
            char size[24];
            st_disk_size(size, sizeof(size), a->sizes[i].alloc_bytes ? a->sizes[i].alloc_bytes : a->sizes[i].bytes);
            right_text(c, F_UI, a->r[2].x + a->r[2].w - 8, r.y + 1, LS, size, ink);
        }
    }
    char line[160], need[24], avail[24];
    st_disk_size(avail, sizeof(avail), available_bytes(a));
    if (a->sizes_ok) {
        st_disk_size(need, sizeof(need), st_required_estimate(a, chosen | SETUP_COMP_SYSTEM));
        snprintf(line, sizeof(line), "Space required: " ST_B "%s" ST_B " " MID " available on C: %s", need, avail);
    } else snprintf(line, sizeof(line), "Space required: " ST_B EM ST_B " " MID " available on C: %s", avail);
    text_at(c, F_UI, a->r[3].x, a->r[3].y, LS, line, DU_INK);
}

static void paint_user(struct st_app *a, struct du_canvas *c)
{
    heading(c, a->r[0], U_H);
    wrap(c, F_UI, a->r[1], LP, U_P, DU_INK);
    static const char *const labels[4] = {"Your name", "Computer name", "Keyboard layout", "Clock"};
    for (int i = 0; i < 4; i++) text_at(c, F_UI, a->r[2 + i].x, a->r[2 + i].y, 21, labels[i], DU_INK);
    well(c, a->r[6]);
    wrap(c, F_UI, (struct du_rect){a->r[6].x + 8, a->r[6].y + 6, CW - 16, a->r[6].h - 12}, LS, U_BOX, DU_INK);
}

static void paint_network(struct st_app *a, struct du_canvas *c)
{
    heading(c, a->r[0], N_H);
    wrap(c, F_UI, a->r[1], LP, a->adapter ? a->adapter : "", DU_INK);
    int none = a->adapter_none;
    legend(c, a->r[2], "Address");
    wrap(c, F_UI, a->r[5], LS, N_DHCP, none ? GREY : DU_INK);
    wrap(c, F_UI, a->r[6], LS, N_MANUAL, none ? GREY : DU_INK);
    static const char *const names[4] = {"IP address", "Subnet mask", "Gateway", "DNS server"};
    for (int i = 0; i < 4; i++) text_at(c, F_UI, a->r[10 + i].x, a->r[10 + i].y, 19, names[i], a->net_manual && !none ? DU_INK : GREY);
    legend(c, a->r[3], "Games on the LAN");
    struct du_widget *lobby = du_get(a->ui, ST_ID_LOBBY);
    wrap(c, F_UI, a->r[7], LS, N_IPX, DU_INK);
    wrap(c, F_UI, a->r[8], LS, N_LOBBY, lobby && lobby->disabled ? GREY : DU_INK);
}

static void paint_summary(struct st_app *a, struct du_canvas *c)
{
    heading(c, a->r[0], S_H);
    wrap(c, F_UI, a->r[1], LP, S_P, DU_INK);
}

static const char *const copy_rows[6] = {
    "Partition table written, volume C: formatted FAT32 (8 sectors per cluster)",
    "Copying system files",
    "Copying programs and games",
    "Writing registry, user and network settings",
    "Creating the pagefile and verifying the installation",
    "Boot sector and Ciuki loader installed (LBA 1" EN "62), verified by read-back",
};

static enum st_mark copy_mark(const struct st_app *a, int row)
{
    enum setup_phase p = a->progress.phase;
    switch (row) {
    case 0: return p > SETUP_PHASE_FORMAT ? ST_MARK_OK : p >= SETUP_PHASE_PARTITION ? ST_MARK_ACTIVE : ST_MARK_PENDING;
    case 1: return p > SETUP_PHASE_COPY ? ST_MARK_OK : p == SETUP_PHASE_COPY && a->copy_row_system ? ST_MARK_ACTIVE : ST_MARK_PENDING;
    case 2: return p > SETUP_PHASE_COPY ? ST_MARK_OK : p == SETUP_PHASE_COPY && a->copy_row_programs ? ST_MARK_ACTIVE : ST_MARK_PENDING;
    case 3: return p > SETUP_PHASE_CONFIGURE ? ST_MARK_OK : p == SETUP_PHASE_CONFIGURE ? ST_MARK_ACTIVE : ST_MARK_PENDING;
    case 4: return p > SETUP_PHASE_PUBLISH ? ST_MARK_OK : p == SETUP_PHASE_PUBLISH ? ST_MARK_ACTIVE : ST_MARK_PENDING;
    default: return p == SETUP_PHASE_COMPLETE ? ST_MARK_OK : ST_MARK_PENDING;
    }
}

static void paint_copy(struct st_app *a, struct du_canvas *c)
{
    int y = CY;
    heading(c, (struct du_rect){CX, y, CW, H1}, K_H); y += H1 + 8;
    int tw = CW - 24;
    for (int i = 0; i < 6; i++) {
        enum st_mark m = copy_mark(a, i);
        const char *text = copy_rows[i];
        char line[400];
        if (m == ST_MARK_ACTIVE && (i == 1 || i == 2) && a->step[0]) {
            snprintf(line, sizeof(line), ST_B "%s" ST_B " " EM " %s", copy_rows[i], a->step);
            text = line;
        } else if (m == ST_MARK_ACTIVE) { snprintf(line, sizeof(line), ST_B "%s" ST_B, copy_rows[i]); text = line; }
        st_mark_paint(c, CX + 2, y + 2, 12, m);
        y += wrap(c, F_UI, (struct du_rect){CX + 24, y, tw, 0x7fff}, LS, text, m == ST_MARK_PENDING ? SECONDARY : DU_INK) + 2;
    }
    y += 6;
    const struct setup_progress *p = &a->progress;
    unsigned percent = p->bytes_total ? (unsigned)(p->bytes_done * 100 / p->bytes_total) : 0;
    char right[96], files_done[24], files_total[24], line[160], done[24], total[24];
    unsigned minutes = (p->eta_ms + 59999u) / 60000u;
    if (!p->bytes_total || (!p->eta_ms && percent < 100)) snprintf(right, sizeof(right), "%u %%", percent);
    else if (minutes <= 1) snprintf(right, sizeof(right), "%u %% " MID " less than a minute left", percent);
    else snprintf(right, sizeof(right), "%u %% " MID " about %u minutes left", percent, minutes);
    text_at(c, F_UI, CX, y, LS, "Overall progress", DU_INK);
    right_text(c, F_UI, CX + CW, y, LS, right, DU_INK);
    y += LS + 3;
    struct du_rect bar = {CX, y, CW, 16};
    du_well(c, bar, DU_WINDOW);
    int fill = p->bytes_total ? (int)((uint64_t)(bar.w - 6) * (p->bytes_done > p->bytes_total ? p->bytes_total : p->bytes_done) / p->bytes_total) : 0;
    if (fill > 0) du_fill(c, (struct du_rect){bar.x + 3, bar.y + 3, fill, bar.h - 6}, 0x000080u);
    y += 16 + 3;
    comma(files_done, sizeof(files_done), p->files_done); comma(files_total, sizeof(files_total), p->files_total);
    st_disk_size(done, sizeof(done), p->bytes_done); st_disk_size(total, sizeof(total), p->bytes_total);
    snprintf(line, sizeof(line), "%s of %s files " MID " %s of %s", files_done, files_total, done, total);
    text_at(c, F_UI, CX, y, LS, line, SECONDARY);
    unsigned s = p->elapsed_ms / 1000u;
    snprintf(right, sizeof(right), "CD %u.%u MB/s " MID " elapsed %u:%02u", p->throughput_kbps / 1000u, (p->throughput_kbps % 1000u) / 100u, s / 60u, s % 60u);
    right_text(c, F_UI, CX + CW, y, LS, right, SECONDARY);
    y += LS + 8;
    struct du_rect tip = {CX, y, CW, BTN_Y - 8 - y};
    if (tip.h > 30) {
        well(c, tip);
        du_fill(c, (struct du_rect){tip.x + 10, tip.y + 8, 3, tip.h - 16}, 0x000080u);
        struct du_rect old = c->clip;
        c->clip = du_intersection(old, (struct du_rect){tip.x + 2, tip.y + 2, tip.w - 4, tip.h - 4});
        text_at(c, F_BOLD, tip.x + 23, tip.y + 6, LS, "Did you know?", DU_INK);
        wrap(c, F_UI, (struct du_rect){tip.x + 23, tip.y + 6 + LS, tip.w - 33, tip.h}, LS, st_tips[a->tip % st_tip_count], DU_INK);
        c->clip = old;
    }
}

static void paint_restart(struct st_app *a, struct du_canvas *c)
{
    int y = CY;
    heading(c, (struct du_rect){CX, y, CW, H1}, R_H); y += H1 + 12;
    char files[24], p[300];
    comma(files, sizeof(files), a->progress.files_total);
    snprintf(p, sizeof(p), "CiukiOS 1.0 Beta has been installed on " ST_B "C:" ST_B " and the disk is bootable. The installation was verified: %s files read back with matching sizes and checksums.", files);
    y += wrap(c, F_UI, (struct du_rect){CX, y, CW, 0x7fff}, LP, p, DU_INK) + 12;
    int h = 10;
    for (int i = 0; i < 3; i++) h += para(F_UI, CW - 24 - 28, 18, R_STEPS[i]) + 6;
    struct du_rect box = {CX, y, CW, h + 4};
    well(c, box);
    int sy = y + 10;
    for (int i = 0; i < 3; i++) {
        char n[4]; snprintf(n, sizeof(n), "%d.", i + 1);
        text_at(c, F_BOLD, CX + 12, sy, 18, n, DU_INK);
        sy += wrap(c, F_UI, (struct du_rect){CX + 12 + 28, sy, CW - 24 - 28, 0x7fff}, 18, R_STEPS[i], DU_INK) + 6;
    }
    y += box.h + 12;
    wrap(c, F_UI, (struct du_rect){CX, y, CW, 0x7fff}, LS, R_LOGS, SECONDARY);
    char left[96];
    uint64_t now = a->now, left_s = a->countdown_deadline > now ? (a->countdown_deadline - now + 999999999ull) / 1000000000ull : 0;
    if (a->restart_state == 0) snprintf(left, sizeof(left), "Restarting automatically in " ST_B "%" PRIu64 ST_B " seconds", left_s);
    else if (a->restart_state == 1) snprintf(left, sizeof(left), "Remove the Setup CD from the drive.");
    else snprintf(left, sizeof(left), "Restarting\xe2\x80\xa6");
    text_at(c, F_UI, CX, BTN_Y, BTN_H, left, DU_INK);
}

static void stop_header(struct du_canvas *c, const char *title, const char *text, int *y)
{
    int cx = CX + 16, cy = *y + 16;
    for (int dy = -16; dy < 16; dy++) for (int dx = -16; dx < 16; dx++) {
        int d2 = (2 * dx + 1) * (2 * dx + 1) + (2 * dy + 1) * (2 * dy + 1);
        if (d2 <= 32 * 32) du_fill(c, (struct du_rect){cx + dx, cy + dy, 1, 1}, 0xc00000u);
    }
    for (int k = -6; k <= 6; k++) {
        du_fill(c, (struct du_rect){cx + k - 1, cy + k - 1, 3, 2}, WHITE);
        du_fill(c, (struct du_rect){cx - k - 1, cy + k - 1, 3, 2}, WHITE);
    }
    int h = wrap(c, F_H1, (struct du_rect){CX + 44, *y, CW - 44, 0x7fff}, 19, title, DU_INK);
    h += 6 + wrap(c, F_UI, (struct du_rect){CX + 44, *y + h + 6, CW - 44, 0x7fff}, LP, text, DU_INK);
    *y += (h > 32 ? h : 32) + 12;
}

static const char *status_text(struct st_app *a)
{
    const char *r = a->status.reason, *k = a->status.key;
    if (!strcmp(r, SETUP_ST_SYNTAX)) return st_pool(a, "Line %u is not a [Section] header, a comment or a key=value line.", a->status.line);
    if (!strcmp(r, SETUP_ST_VERSION)) return "This Setup reads answer files of version 1 only.";
    if (!strcmp(r, SETUP_ST_SECTION)) return k[0] ? st_pool(a, "The section [%s] is not part of the answer file.", k) : "The answer file has no [Setup] section.";
    if (!strcmp(r, SETUP_ST_UNKNOWN_KEY)) return st_pool(a, "The key %s is not part of the answer file.", k);
    if (!strcmp(r, SETUP_ST_DUPLICATE)) return st_pool(a, "%s appears more than once.", k);
    if (!strcmp(r, SETUP_ST_VALUE)) return st_pool(a, "The value of %s is not valid.", k);
    if (!strcmp(r, SETUP_ST_DEPENDENCY)) return "The shareware games need the DOS personality: add dos to Components.";
    if (!strcmp(r, SETUP_ST_MISSING)) return st_pool(a, "The answer file has no value for %s.", k);
    if (!strcmp(r, SETUP_ST_CONSENT)) return "ATA disk 0 is not empty: EraseDisk=yes is required before Setup erases it.";
    if (!strcmp(r, SETUP_ST_AMBIGUOUS)) return "Disk=auto needs exactly one ATA disk; this computer has more. Use Disk=ata0 or the disk's model and serial.";
    if (!strcmp(r, SETUP_ST_NOT_FOUND)) return "The disk named by Disk= is not ATA disk 0 of this computer.";
    if (!strcmp(r, SETUP_ST_INSTALLED)) return "This CiukiOS release is already installed on ATA disk 0. Remove the CD and restart, or set Reinstall=yes to install it again.";
    if (!strcmp(r, SETUP_ST_TOO_LARGE)) return "The answer file is larger than 16 KB.";
    if (!strcmp(r, SETUP_ST_READ)) return "The answer file could not be read.";
    return "The answer file cannot be used.";
}

static void info_rows(struct du_canvas *c, struct du_rect box, const char *const *names, const char *const *texts, unsigned n)
{
    well(c, box);
    int y = box.y + 6, tw = box.w - 4 - 8 - 100 - 6 - 8;
    for (unsigned i = 0; i < n; i++) {
        if (!texts[i]) continue;
        text_at(c, F_BOLD, box.x + 8, y, 17, names[i], DU_INK);
        y += wrap(c, F_UI, (struct du_rect){box.x + 8 + 100 + 6, y + 1, tw, 0x7fff}, LS, texts[i], DU_INK) + 2;
        if (y < box.y + 6 + 17 * (int)(i + 1)) y = box.y + 6 + 17 * (int)(i + 1);
    }
}

static int info_height(const char *const *texts, unsigned n, int w)
{
    int h = 12;
    for (unsigned i = 0; i < n; i++) if (texts[i]) { int t = para(F_UI, w - 4 - 8 - 100 - 6 - 8, LS, texts[i]) + 2; h += t < 17 ? 17 : t; }
    return h;
}

static void paint_stop(struct st_app *a, struct du_canvas *c)
{
    int y = CY;
    if (a->page == ST_PAGE_STOP) {
        stop_header(c, E_H, E_P, &y);
        int rows = 12;
        for (unsigned i = 0; i < a->row_count; i++) if (a->rows[i].mark == ST_MARK_STOP) rows += hw_row_height(&a->rows[i], CW - 4 - 8 - 18 - 6 - 100 - 6 - 8);
        rows += 18 + 15;
        struct du_rect box = {CX, y, CW, rows};
        well(c, box);
        paint_hardware_rows(a, c, box, 100);
        y += rows + 12;
        char text[400];
        snprintf(text, sizeof(text), "Error code " ST_B "%s" ST_B ". The full report is on the CD-ROM screen behind this message and can be saved to a USB drive. The hardware baseline is documented at ciukios.alcybercloud.it/hardware.", st_stop_code(a->stop));
        wrap(c, F_UI, (struct du_rect){CX, y, CW, 0x7fff}, LS, text, DU_INK);
        return;
    }
    if (a->page == ST_PAGE_FAILED) {
        enum setup_error_class cls = (enum setup_error_class)a->failed_class;
        stop_header(c, X_H, st_error_text(cls), &y);
        static const char *const phases[] = {"plan", "partition table", "format", "copy", "configuration", "publication", "complete"};
        enum setup_phase reached = SETUP_PHASE_PLAN;
        int state_ok = !setup_txn_state(&reached);
        const char *names[5] = {"Phase", "File", "Sector", "Attempts", "State"};
        const char *texts[5] = {0};
        if (a->error_valid) {
            texts[0] = phases[a->error.phase <= SETUP_PHASE_COMPLETE ? a->error.phase : 0];
            if (a->error.path[0]) texts[1] = a->error.path;
            if (a->error.count) texts[2] = st_pool(a, "LBA %" PRIu64 ", %" PRIu64 " sectors", a->error.lba, a->error.count);
            if (a->error.attempt) texts[3] = st_pool(a, "%u of %u", a->error.attempt, SETUP_RETRY_MAX);
        }
        texts[4] = state_ok && reached == SETUP_PHASE_PLAN ? X_UNTOUCHED : X_STATE;
        struct du_rect box = {CX, y, CW, info_height(texts, 5, CW)};
        info_rows(c, box, names, texts, 5);
        y += box.h + 12;
        wrap(c, F_UI, (struct du_rect){CX, y, CW, 0x7fff}, LS, st_pool(a, "Error " ST_B "%s" ST_B ". Rescue copies the in-memory Setup log to a USB drive.", st_error_id(cls)), DU_INK);
        return;
    }
    stop_header(c, Q_H, Q_P, &y);
    const char *names[4] = {"Answer file", "Status", "Key", "Reason"};
    const char *texts[4] = {a->answers.source[0] ? a->answers.source : "none", a->status.reason, a->status.key[0] ? a->status.key : "-", status_text(a)};
    struct du_rect box = {CX, y, CW, info_height(texts, 4, CW)};
    info_rows(c, box, names, texts, 4);
    y += box.h + 12;
    wrap(c, F_UI, (struct du_rect){CX, y, CW, 0x7fff}, LS, a->status_line, SECONDARY);
}

static void paint_rescue(struct st_app *a, struct du_canvas *c)
{
    heading(c, a->r[0], RC_H);
    wrap(c, F_UI, a->r[1], LP, RC_P, DU_INK);
    if (a->status_line[0]) wrap(c, F_UI, a->r[3], LS, a->status_line, DU_INK);
}

static void paint_link(struct du_canvas *c, const struct du_widget *w, const char *s, int focused)
{
    int width = st_text_line(c, F_UI, w->box.x + 2, w->box.y + 1, LS, s, LINK);
    du_hline(c, w->box.x + 2, w->box.y + 1 + LS - 2, width, LINK);
    if (focused) du_dotted(c, (struct du_rect){w->box.x, w->box.y, width + 4, w->box.h});
}

static void paint_combo(struct st_app *a, struct du_canvas *c, const struct du_widget *w, int focused)
{
    const char *label = w->id == ST_ID_KBD ? st_keyboards[a->kbd].label : st_zones[a->tz].label;
    du_well(c, w->box, w->disabled ? DU_FACE : DU_WINDOW);
    struct du_rect arrow = {w->box.x + w->box.w - w->box.h + 2, w->box.y + 2, w->box.h - 4, w->box.h - 4};
    du_raised(c, arrow, a->popup == w->id, 0);
    du_triangle(c, arrow.x + arrow.w / 2, arrow.y + arrow.h / 2, 1, 4, DU_INK);
    struct du_rect text = {w->box.x + 3, w->box.y + 3, arrow.x - w->box.x - 5, w->box.h - 6};
    uint32_t ink = DU_INK;
    if (focused && a->popup != w->id) { du_fill(c, text, DU_SELECT); ink = DU_SELECT_INK; du_dotted(c, text); }
    struct du_rect old = c->clip;
    c->clip = du_intersection(old, text);
    text_at(c, F_UI, text.x + 2, text.y, text.h, label, ink);
    c->clip = old;
}

static void canvas_paint(struct st_app *a, struct du_canvas *c, struct du_widget *w, int focused)
{
    switch (w->id) {
    case ST_ID_VIEW: st_view_paint(c, &a->view, focused); break;
    case ST_ID_LINK_SAVE: paint_link(c, w, H_LINK, focused); break;
    case ST_ID_DEFAULTS: paint_link(c, w, C_DEFAULTS, focused); break;
    case ST_ID_KBD: case ST_ID_TZ: paint_combo(a, c, w, focused); break;
    }
}

/* The open drop-down list of a combo box. */
#define POPUP_ROWS 8
static struct du_rect popup_box(struct st_app *a, unsigned *count)
{
    struct du_widget *w = du_get(a->ui, a->popup);
    *count = a->popup == ST_ID_KBD ? st_keyboard_count : st_zone_count;
    if (!w) return (struct du_rect){0, 0, 0, 0};
    unsigned rows = *count < POPUP_ROWS ? *count : POPUP_ROWS;
    struct du_rect r = {w->box.x, w->box.y + w->box.h, w->box.w, (int)rows * 16 + 4};
    if (r.y + r.h > ST_H - 4) r.y = w->box.y - r.h;
    return r;
}

static void paint_popup(struct st_app *a, struct du_canvas *c)
{
    unsigned count; struct du_rect r = popup_box(a, &count);
    if (!r.w) return;
    du_fill(c, r, DU_WINDOW); du_outline(c, r, 0x000000u);
    unsigned rows = count < POPUP_ROWS ? count : POPUP_ROWS;
    struct du_rect old = c->clip;
    c->clip = du_intersection(old, (struct du_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2});
    for (unsigned i = 0; i < rows; i++) {
        unsigned item = (unsigned)a->popup_top + i;
        if (item >= count) break;
        const char *label = a->popup == ST_ID_KBD ? st_keyboards[item].label : st_zones[item].label;
        struct du_rect row = {r.x + 2, r.y + 2 + (int)i * 16, r.w - 4, 16};
        uint32_t ink = DU_INK;
        if ((int)item == a->popup_sel) { du_fill(c, row, DU_SELECT); ink = DU_SELECT_INK; }
        text_at(c, F_UI, row.x + 3, row.y, 16, label, ink);
    }
    c->clip = old;
}

void st_page_paint(struct st_app *a)
{
    struct du_canvas *c = &a->ui->canvas;
    frame(a, c);
    switch (a->page) {
    case ST_PAGE_WELCOME: paint_welcome(a, c); break;
    case ST_PAGE_LICENCE: paint_licence(a, c); break;
    case ST_PAGE_HARDWARE: paint_hardware(a, c); break;
    case ST_PAGE_DISK: paint_disk(a, c); break;
    case ST_PAGE_COMPONENTS: paint_components(a, c); break;
    case ST_PAGE_USER: paint_user(a, c); break;
    case ST_PAGE_NETWORK: paint_network(a, c); break;
    case ST_PAGE_SUMMARY: paint_summary(a, c); break;
    case ST_PAGE_COPY: paint_copy(a, c); break;
    case ST_PAGE_RESTART: paint_restart(a, c); break;
    case ST_PAGE_STOP: case ST_PAGE_FAILED: case ST_PAGE_STATUS: paint_stop(a, c); break;
    case ST_PAGE_RESCUE: paint_rescue(a, c); break;
    case ST_PAGE_TEXT: heading(c, a->r[0], a->text_title); break;
    case ST_PAGES: break;
    }
}

void st_render(struct st_app *a)
{
    struct du_ui *u = a->ui;
    struct du_canvas *c = &u->canvas;
    c->clip = (struct du_rect){0, 0, c->width, c->height};
    st_page_paint(a);
    for (unsigned i = 0; i < u->count; i++) {
        struct du_widget *w = &u->widgets[i];
        if (w->hidden) continue;
        int focused = u->focus == (int)i && !u->dialog.kind;
        c->clip = du_intersection((struct du_rect){0, 0, c->width, c->height}, w->box);
        if (w->kind == DU_CANVAS) {
            c->clip = (struct du_rect){0, 0, c->width, c->height};
            canvas_paint(a, c, w, focused);
        } else du_draw_widget(u, w, focused);
    }
    c->clip = (struct du_rect){0, 0, c->width, c->height};
    if (a->popup) paint_popup(a, c);
    du_draw_dialog(u);
    c->clip = (struct du_rect){0, 0, c->width, c->height};
    u->dirty = 0;
}

/* ---- page events: canvases, check boxes, radios, edits ---- */
static void combo_set(struct st_app *a, int id, int index)
{
    unsigned count = id == ST_ID_KBD ? st_keyboard_count : st_zone_count;
    if (index < 0) index = 0;
    if (index >= (int)count) index = (int)count - 1;
    if (id == ST_ID_KBD) a->kbd = index;
    else { a->tz = index; clock_refresh(a); }
    a->ui->dirty = 1;
}
static int combo_value(struct st_app *a, int id) { return id == ST_ID_KBD ? a->kbd : a->tz; }
static void popup_open(struct st_app *a, int id)
{
    a->popup = id; a->popup_sel = combo_value(a, id);
    unsigned count = id == ST_ID_KBD ? st_keyboard_count : st_zone_count;
    a->popup_top = a->popup_sel - POPUP_ROWS / 2;
    if (a->popup_top > (int)count - POPUP_ROWS) a->popup_top = (int)count - POPUP_ROWS;
    if (a->popup_top < 0) a->popup_top = 0;
    a->ui->dirty = 1;
}

/* Keys while a drop-down list is open; 1 when consumed. */
int st_popup_key(struct st_app *a, unsigned code)
{
    unsigned count = a->popup == ST_ID_KBD ? st_keyboard_count : st_zone_count;
    int sel = a->popup_sel;
    switch (code) {
    case DU_KEY_UP: sel--; break;
    case DU_KEY_DOWN: sel++; break;
    case DU_KEY_PGUP: sel -= POPUP_ROWS; break;
    case DU_KEY_PGDN: sel += POPUP_ROWS; break;
    case DU_KEY_HOME: sel = 0; break;
    case DU_KEY_END: sel = (int)count - 1; break;
    case DU_KEY_ENTER: case DU_KEY_SPACE: case DU_KEY_TAB: case DU_KEY_F4:
        combo_set(a, a->popup, a->popup_sel); a->popup = 0; a->ui->dirty = 1;
        return code != DU_KEY_TAB;
    case DU_KEY_ESC: a->popup = 0; a->ui->dirty = 1; return 1;
    default: return 1;
    }
    if (sel < 0) sel = 0;
    if (sel >= (int)count) sel = (int)count - 1;
    a->popup_sel = sel;
    if (sel < a->popup_top) a->popup_top = sel;
    if (sel >= a->popup_top + POPUP_ROWS) a->popup_top = sel - POPUP_ROWS + 1;
    a->ui->dirty = 1;
    return 1;
}

/* Pointer while a drop-down list is open: a row selects, outside closes. */
int st_popup_click(struct st_app *a, int x, int y)
{
    unsigned count; struct du_rect r = popup_box(a, &count);
    if (du_contains(r, x, y)) {
        int row = (y - r.y - 2) / 16, item = a->popup_top + row;
        if (row >= 0 && item < (int)count) combo_set(a, a->popup, item);
    }
    a->popup = 0; a->ui->dirty = 1;
    return 1;
}

static int canvas_key(struct st_app *a, int id, unsigned code)
{
    switch (id) {
    case ST_ID_VIEW:
        if (code == DU_KEY_ENTER && a->page == ST_PAGE_LICENCE && a->view.selected >= 0) {
            unsigned index = 0;
            for (int i = 0; i < a->view.selected; i++) index += a->view.lines[i].entry;
            st_licence_open(a, index);
            return 1;
        }
        if (st_view_key(&a->view, code)) { a->ui->dirty = 1; return 1; }
        return 0;
    case ST_ID_LINK_SAVE: case ST_ID_DEFAULTS:
        if (code == DU_KEY_ENTER || code == DU_KEY_SPACE) { st_action(a, id); return 1; }
        return 0;
    case ST_ID_KBD: case ST_ID_TZ: {
        unsigned count = id == ST_ID_KBD ? st_keyboard_count : st_zone_count;
        int v = combo_value(a, id);
        if ((code == DU_KEY_DOWN && (a->ui->modifiers & DU_ALT)) || code == DU_KEY_F4 || code == DU_KEY_SPACE) { popup_open(a, id); return 1; }
        if (code == DU_KEY_UP) v--; else if (code == DU_KEY_DOWN) v++;
        else if (code == DU_KEY_PGUP) v -= POPUP_ROWS; else if (code == DU_KEY_PGDN) v += POPUP_ROWS;
        else if (code == DU_KEY_HOME) v = 0; else if (code == DU_KEY_END) v = (int)count - 1;
        else return 0;
        combo_set(a, id, v);
        return 1;
    }
    }
    return 0;
}

static void components_changed(struct st_app *a)
{
    struct du_widget *dos = du_get(a->ui, ST_ID_COMP + 1), *games = du_get(a->ui, ST_ID_COMP + 4);
    if (dos && games) {
        games->disabled = !dos->value;
        if (!dos->value) games->value = 0;
    }
    a->ui->dirty = 1;
}

int st_page_event(struct st_app *a, const struct du_event *e)
{
    switch (e->type) {
    case DU_EV_KEY: return canvas_key(a, e->id, e->key);
    case DU_EV_POINTER:
        if (e->index != DU_POINTER_UP) return 1;
        if (e->id == ST_ID_VIEW) {
            int was = a->view.selected, hit = st_view_click(&a->view, e->x, e->y);
            if (hit >= 0 && hit == was && a->page == ST_PAGE_LICENCE) canvas_key(a, ST_ID_VIEW, DU_KEY_ENTER);
            a->ui->dirty = 1;
        } else if (e->id == ST_ID_LINK_SAVE || e->id == ST_ID_DEFAULTS) st_action(a, e->id);
        else if (e->id == ST_ID_KBD || e->id == ST_ID_TZ) popup_open(a, e->id);
        return 1;
    case DU_EV_CHANGE:
        if (e->id == ST_ID_ACCEPT) {
            a->answers.accept_licences = (uint8_t)e->index;
            struct du_widget *next = du_get(a->ui, ST_ID_NEXT);
            if (next) next->disabled = !e->index;
            a->ui->dirty = 1;
            return 1;
        }
        if (e->id >= ST_ID_COMP && e->id < ST_ID_COMP + (int)SETUP_COMP_COUNT) { components_changed(a); return 1; }
        if (e->id == ST_ID_DHCP || e->id == ST_ID_MANUAL) {
            a->net_manual = e->id == ST_ID_MANUAL;
            for (int i = 0; i < 4; i++) { struct du_widget *w = du_get(a->ui, ST_ID_IP + i); if (w) w->disabled = !a->net_manual; }
            a->ui->dirty = 1;
            return 1;
        }
        if (e->id == ST_ID_COMPUTER) {
            struct du_widget *w = du_get(a->ui, ST_ID_COMPUTER);
            if (w) for (char *p = w->text; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
            return 1;
        }
        if (e->id == ST_ID_CLOCK) { a->clock_edited = 1; return 1; }
        if (e->id == ST_ID_UTC) { a->clock_utc = e->index; return 1; }
        return 0;
    default: return 0;
    }
}

/* Restore defaults on screen 6. */
void st_components_defaults(struct st_app *a)
{
    struct setup_answers d; setup_answers_defaults(&d);
    for (unsigned i = 1; i < SETUP_COMP_COUNT; i++) {
        struct du_widget *w = du_get(a->ui, ST_ID_COMP + (int)i);
        if (w) w->value = (d.components & st_components[i].bit) != 0;
    }
    components_changed(a);
}
