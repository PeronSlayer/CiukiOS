/* SPDX-License-Identifier: MIT
 * G2: Files and the Recycle Bin. Delete sends to the bin (confirmation,
 * Shift+Delete, policy), the bin's folder view (columns, menus, sorting,
 * Restore, Delete, Empty, item and bin Properties), drops on the bin window. */
#include "fake_vfs.h"
#include "../../../apps/desktop/media_notice.h"
#include "../../../apps/files/app.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "files_wait.h"

#define WIDTH 760
#define HEIGHT 520
/* Command and dialog ids of apps/files/app.c's enum. */
enum { K_CUT = 4, K_COPY = 5, K_PASTE = 6, K_DELETE = 7, K_PROPERTIES = 8, K_RENAME = 14, K_NEW_FOLDER = 12,
       K_GO_RECYCLE = 305, K_RESTORE = 306, K_EMPTY_BIN = 307, K_BIN_PROPS = 308, K_DELETE_NOW = 309,
       D_DELETE = 200, D_BIN_PURGE = 207, D_BIN_EMPTY = 208, D_BIN_PROPS = 209, D_BIN_ITEM = 210,
       W_ADDRESS = 100, W_LIST = 107, W_STATUS = 108 };
static uint32_t *pixels;
static struct du_ui ui;
static struct files_app app;
static const char *out_dir;
static uint64_t clock_ns = 1000000000ull;
static int64_t rb_time = 1792000000;
static int64_t clock_now(void) { return rb_time; }
static unsigned sounds[32], sound_count;
static void on_sound(void *ctx, unsigned event) { (void)ctx; if (sound_count < 32) sounds[sound_count++] = event; }
static int statuses; static unsigned status_effect, status_allowed;
static void on_status(struct files_app *a, uint32_t serial, unsigned effect, unsigned allowed)
{ (void)a; (void)serial; statuses++; status_effect = effect; status_allowed = allowed; }

static void dump(const char *name)
{
    du_render(&ui);
    uint32_t *partial = malloc(WIDTH * HEIGHT * 4); assert(partial); memcpy(partial, pixels, WIDTH * HEIGHT * 4);
    ui.dirty = 1; du_render(&ui);
    assert(!memcmp(partial, pixels, WIDTH * HEIGHT * 4)); free(partial);
    assert(pixels[WIDTH * HEIGHT] == 0xdeadbeefu);
    if (!out_dir) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/recycle-%s.xrgb", out_dir, name);
    FILE *f = fopen(path, "wb"); assert(f && fwrite(pixels, 4, WIDTH * HEIGHT, f) == WIDTH * HEIGHT); fclose(f);
}
static void event(int type, int code, int value, int value2)
{
    clock_ns += 20000000ull;
    struct ciuki_input_event e = {.type = (uint32_t)type, .code = code, .value = value, .value2 = value2, .monotonic_ns = clock_ns};
    du_input(&ui, &e); files_test_wait(&app);
}
static void motion(int x, int y) { event(CIUKI_INPUT_MOTION, 0, x, y); }
static void press(int b) { event(CIUKI_INPUT_BUTTON, b, 1, 0); }
static void release(int b) { event(CIUKI_INPUT_BUTTON, b, 0, 0); }
static void click(int x, int y) { motion(x, y); press(1); release(1); clock_ns += 600000000ull; }
static void key(int code) { event(CIUKI_INPUT_KEY, code, 1, 0); event(CIUKI_INPUT_KEY, code, 0, 0); }
static void chord(int mod, int code) { event(CIUKI_INPUT_KEY, mod, 1, 0); key(code); event(CIUKI_INPUT_KEY, mod, 0, 0); }
static void tick(void)
{
    for (int i = 0; i < 400 && app.job_active; i++) {
        clock_ns += 100000000ull; du_tick(&ui, clock_ns); files_test_wait(&app);
        struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
    }
    assert(!app.job_active);
}
static void activate(int id) { struct du_event e = {.type = DU_EV_ACTIVATE, .id = id}; files_event(&ui, &e, &app); }
static struct du_widget *LIST(void) { return du_get(&ui, W_LIST); }
static int list_find(const char *name)
{ for (unsigned i = 0; i < app.model.count; i++) if (!strcmp(app.model.entries[i].name, name)) return (int)i; return -1; }
static int row_of(const char *original)
{
    for (unsigned i = 0; i < app.model.count; i++) if (app.model.entries[i].bin && !strcmp(app.rows[i].text[0], original)) return (int)i;
    return -1;
}
static void select_row(int row)
{
    assert(row >= 0);
    struct du_rect r = du_list_label(LIST(), (unsigned)row);
    click(r.x + r.w / 2, r.y + r.h / 2);
    assert(app.model.entries[row].selected);
}
static void yes(void) { key(DU_KEY_ENTER); }                    /* the Yes button is the default */
static void address(const char *text);
static void setup(const char *path)
{
    fake_image_tree();
    fake_create("/mnt/d", FM_DIR); fake_create("/mnt/e", FM_DIR);
    rb_clock_hook = clock_now; rb_capacity_hook = NULL; rb_time = 1792000000;
    memset(&app, 0, sizeof(app)); du_init(&ui, pixels, WIDTH, HEIGHT);
    ui.sound = on_sound; sound_count = 0;
    assert(!files_setup(&app, &ui, &fake_vfs, path));
}
static void type(const char *text)
{
    static const unsigned char letters[] = {0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
    for (; *text; text++) {
        unsigned c = (unsigned char)*text, code; int shift = 0;
        if (c >= 'A' && c <= 'Z') { shift = 1; c += 'a' - 'A'; }
        if (c >= 'a' && c <= 'z') code = letters[c - 'a']; else if (c == ' ') code = 0x39; else { assert(!"unmapped"); code = 0; }
        if (shift) chord(DU_KEY_LSHIFT, (int)code); else key((int)code);
    }
}
static void address(const char *text) { chord(DU_KEY_LALT, 0x20); type(text); key(DU_KEY_ENTER); }

static void delete_tests(void)
{
    setup("/Documents/Desktop");
    fake_file("/Documents/Desktop/n1.txt", 100, 1); fake_file("/Documents/Desktop/n2.dat", 50, 2); fake_create("/Documents/Desktop/Folder", FM_DIR);
    fake_file("/Documents/Desktop/Folder/in.txt", 5, 3);
    activate(K_PASTE); activate(K_GO_RECYCLE + 100);                              /* unknown ids do nothing */
    assert(!files_navigate(&app, "/Documents/Desktop"));
    select_row(list_find("n1.txt"));
    key(DU_KEY_DELETE);
    assert(du_dialog_id(&ui) == D_DELETE && !strcmp(ui.dialog.title, "Confirm File Delete") &&
           !strcmp(ui.dialog.text, "Are you sure you want to send 'n1.txt' to the Recycle Bin?"));
    dump("confirm-recycle");
    key(DU_KEY_ESC); assert(!du_dialog_id(&ui) && fake_lookup("/Documents/Desktop/n1.txt") >= 0);   /* No: nothing happens */
    key(DU_KEY_DELETE); yes(); tick();
    assert(fake_lookup("/Documents/Desktop/n1.txt") < 0 && fake_lookup("/RECYCLED/Dc1.txt") >= 0 && strstr(app.note, "sent to the Recycle Bin"));
    assert(list_find("n1.txt") < 0 && list_find("RECYCLED") < 0);                   /* hidden system folder */
    /* A folder: its own wording; a multi-selection: counted. */
    select_row(list_find("Folder"));
    key(DU_KEY_DELETE);
    assert(!strcmp(ui.dialog.title, "Confirm Folder Delete") && strstr(ui.dialog.text, "remove the folder 'Folder' and move all its contents to the Recycle Bin"));
    key(DU_KEY_ESC);
    fm_select_all(&app.model, 0); files_update(&app);
    key(DU_KEY_DELETE);
    assert(!strcmp(ui.dialog.title, "Confirm Multiple File Delete") && strstr(ui.dialog.text, "send these 2 items to the Recycle Bin"));
    yes(); tick();
    assert(fake_lookup("/RECYCLED/Dc3.dat") >= 0 && fake_lookup("/RECYCLED/Dc2") >= 0 && fake_lookup("/RECYCLED/Dc2/in.txt") >= 0 && app.model.count == 0);
    /* Shift+Delete asks about a permanent delete and bypasses the bin. */
    fake_file("/Documents/Desktop/sh.txt", 9, 1);
    assert(!files_navigate(&app, "/Documents/Desktop"));
    select_row(list_find("sh.txt"));
    chord(DU_KEY_LSHIFT, DU_KEY_DELETE);
    assert(du_dialog_id(&ui) == D_DELETE && !strcmp(ui.dialog.text, "Are you sure you want to delete 'sh.txt'?"));
    dump("confirm-permanent");
    yes(); tick();
    assert(fake_lookup("/Documents/Desktop/sh.txt") < 0 && fake_lookup("/RECYCLED/Dc4.txt") < 0 && strstr(app.note, "deleted"));
    /* Removable media: no bin, so the plain Delete is permanent and says so. */
    fake_file("/mnt/e/usb.txt", 3, 1);
    assert(!files_navigate(&app, "/mnt/e"));
    select_row(list_find("usb.txt")); key(DU_KEY_DELETE);
    assert(!strcmp(ui.dialog.text, "Are you sure you want to delete 'usb.txt'?"));
    yes(); tick(); assert(fake_lookup("/mnt/e/usb.txt") < 0 && fake_lookup("/mnt/e/RECYCLED") < 0);
    /* A fixed second drive (the media state says E: is the only removable one) has its own bin. */
    app.media.version = CIUKI_MEDIA_VERSION; app.media.removable_mask = 1u << 4;
    fake_file("/mnt/d/disk.bin", 3, 1);
    assert(!files_navigate(&app, "/mnt/d"));
    select_row(list_find("disk.bin")); key(DU_KEY_DELETE);
    assert(strstr(ui.dialog.text, "send 'disk.bin' to the Recycle Bin")); yes(); tick();
    assert(fake_lookup("/mnt/d/RECYCLED/Dd1.bin") >= 0);
    /* With the confirmation switched off there is no dialog. */
    struct rb_policy p; rb_policy_default(&p); p.confirm = 0; assert(!rb_policy_save(&fake_vfs, &p));
    fake_file("/Documents/Desktop/quiet.txt", 3, 1);
    assert(!files_navigate(&app, "/Documents/Desktop"));
    select_row(list_find("quiet.txt")); key(DU_KEY_DELETE);
    assert(du_dialog_id(&ui) != D_DELETE); tick(); assert(fake_lookup("/RECYCLED/Dc4.txt") >= 0);
    /* "Do not move files to the Recycle Bin" deletes at once. */
    p.nuke = 1; assert(!rb_policy_save(&fake_vfs, &p));
    fake_file("/Documents/Desktop/gone.txt", 3, 1);
    assert(!files_navigate(&app, "/Documents/Desktop"));
    select_row(list_find("gone.txt")); key(DU_KEY_DELETE); tick();
    assert(fake_lookup("/Documents/Desktop/gone.txt") < 0 && fake_lookup("/RECYCLED/Dc5.txt") < 0);
    files_destroy(&app); fake_assert_clean();
    puts("PASS Files Delete: Recycle Bin confirmation wording, Shift+Delete, folders and several items, removable media, fixed second drive, confirmation off, 'do not move files'");
}

static void bin_view_tests(void)
{
    setup("/Documents/Desktop");
    fake_create("/Documents/Desktop/Dir One", FM_DIR); fake_file("/Documents/Desktop/Dir One/f", 2048, 1);
    fake_file("/Documents/Desktop/pic.bmp", 100, 2);
    fake_file("/Documents/Desktop/zeta.txt", 4000, 3);
    char *paths[] = {"/Documents/Desktop/Dir One", "/Documents/Desktop/pic.bmp"};
    struct rb_policy policy; rb_policy_default(&policy);
    struct fm_job *j = calloc(1, sizeof(*j)); assert(j);
    assert(!rb_delete_job(j, &fake_vfs, paths, 2, &policy, 0, 0));
    for (unsigned i = 0; i < 10000 && j->state == FM_RUNNING; i++) fm_job_step(j);
    rb_time += 3600;
    fm_job_destroy(j);
    char *later[] = {"/Documents/Desktop/zeta.txt"};
    assert(!rb_delete_job(j, &fake_vfs, later, 1, &policy, 0, 0));
    for (unsigned i = 0; i < 10000 && j->state == FM_RUNNING; i++) fm_job_step(j);
    fm_job_destroy(j); free(j);
    /* Go > Recycle Bin and the typed address both reach it. */
    address("Computer"); assert(!app.model.path[0]);
    address("Recycle Bin");
    assert(!strcmp(app.model.path, FM_RECYCLE) && !strcmp(du_get(&ui, W_ADDRESS)->text, "Recycle Bin") && app.model.count == 3);
    assert(!files_navigate(&app, "/Documents")); activate(K_GO_RECYCLE); files_test_wait(&app); assert(!strcmp(app.model.path, FM_RECYCLE));
    assert(LIST()->column_count == 6 && !strcmp(LIST()->columns[1].title, "Original Location") && !strcmp(LIST()->columns[2].title, "Date Deleted") &&
           !strcmp(LIST()->columns[3].title, "Type") && !strcmp(LIST()->columns[4].title, "Size") && !strcmp(LIST()->columns[5].title, "Modified"));
    int d = row_of("Dir One"), z = row_of("zeta.txt"), pic = row_of("pic.bmp");
    assert(d >= 0 && z >= 0 && pic >= 0);
    assert(!strcmp(app.rows[d].text[1], "C:\\Documents\\Desktop") && !strcmp(app.rows[d].text[2], "2026-10-14 17:46") &&
           !strcmp(app.rows[d].text[3], "File Folder") && !strcmp(app.rows[d].text[4], "2 KB"));
    assert(!strcmp(app.rows[pic].text[3], "Bitmap Image") && !strcmp(app.rows[z].text[4], "4 KB") && strcmp(app.rows[z].text[2], app.rows[d].text[2]));
    assert(app.rows[d].icon16 && app.rows[d].icon32 && app.rows[pic].icon16);
    assert(ui.menus[0].items == app.bin_items && ui.menus[0].count == 7);
    assert(du_get(&ui, K_CUT)->disabled && du_get(&ui, K_COPY)->disabled && du_get(&ui, K_PASTE)->disabled && du_get(&ui, K_DELETE)->disabled);
    dump("bin-details");
    /* Items are read-only: no cut, copy, rename, new folder, no drag out. */
    unsigned ops = (unsigned)fake_operations;
    select_row(d);
    activate(K_CUT); activate(K_COPY); activate(K_RENAME); activate(K_NEW_FOLDER); activate(K_PASTE);
    assert(!app.model.clipboard_count && app.rename_index < 0 && (unsigned)fake_operations == ops && !du_get(&ui, K_DELETE)->disabled);
    /* Sorting by the bin's own headers: Original Location is key 4, Date Deleted key 5. */
    struct du_event sort = {.type = DU_EV_SORT, .id = W_LIST, .index = 2};
    files_event(&ui, &sort, &app);
    assert(app.model.sort == 5 && LIST()->sort == 2 && !app.model.descending);
    assert(app.model.entries[0].meta.type == FM_DIR && !strcmp(app.rows[1].text[0], "pic.bmp") && !strcmp(app.rows[2].text[0], "zeta.txt"));   /* oldest first after the folder */
    files_event(&ui, &sort, &app); assert(app.model.descending && !strcmp(app.rows[1].text[0], "zeta.txt"));
    sort.index = 4; files_event(&ui, &sort, &app); assert(app.model.sort == 1 && LIST()->sort == 4);
    sort.index = 1; files_event(&ui, &sort, &app); assert(app.model.sort == 4);
    sort.index = 0; files_event(&ui, &sort, &app); assert(app.model.sort == 0 && !strcmp(app.rows[0].text[0], "Dir One"));
    /* Item properties (double click / Enter): original location and deletion time. */
    select_row(row_of("zeta.txt")); key(DU_KEY_ENTER);
    assert(du_dialog_id(&ui) == D_BIN_ITEM && strstr(ui.dialog.text, "zeta.txt") && strstr(ui.dialog.text, "Original location:\tC:\\Documents\\Desktop") &&
           strstr(ui.dialog.text, "Deleted:\t2026-10-14 18:46") && strstr(ui.dialog.text, "4,000 bytes"));
    dump("bin-item-properties");
    key(DU_KEY_ENTER);
    /* Restore through the File menu: selected items return, folders keep their contents. */
    select_row(row_of("Dir One"));
    struct du_rect file = du_menu_title(&ui, 0); click(file.x + file.w / 2, file.y + file.h / 2);
    assert(ui.menu == 0); dump("bin-menu-file");
    struct du_rect fbox = du_menu_box(&ui, app.bin_items, 7, file.x, file.y + file.h);
    click(fbox.x + 40, fbox.y + 3 + DU_MENU_ITEM_H / 2);                              /* Restore */
    tick();
    assert(fake_lookup("/Documents/Desktop/Dir One/f") >= 0 && fake_lookup("/RECYCLED/Dc1") < 0 && app.model.count == 2 && strstr(app.note, "restored"));
    /* Delete inside the bin: permanent, with its own confirmation. */
    select_row(row_of("pic.bmp")); key(DU_KEY_DELETE);
    assert(du_dialog_id(&ui) == D_BIN_PURGE && !strcmp(ui.dialog.text, "Are you sure you want to delete 'pic.bmp'?"));
    yes(); tick();
    assert(fake_lookup("/RECYCLED/Dc2.bmp") < 0 && app.model.count == 1 && strstr(app.note, "deleted"));
    /* Shift+Delete in the bin means the same. */
    /* Right-click menus. */
    struct du_rect r = du_list_label(LIST(), 0);
    motion(r.x + 4, r.y + r.h / 2); press(2); release(2);
    assert(ui.popup && app.popup[0].id == K_RESTORE && app.popup[2].id == K_DELETE && app.popup[4].id == K_PROPERTIES); key(DU_KEY_ESC);
    struct du_rect body = du_list_body(LIST());
    clock_ns += 600000000ull; motion(body.x + body.w - 20, body.y + body.h - 20); press(2); release(2);
    assert(ui.popup && app.popup[0].id == K_EMPTY_BIN && !(app.popup[0].flags & DU_ITEM_DISABLED));
    int props = 0; for (unsigned i = 0; i < app.popup_count; i++) props |= app.popup[i].id == K_BIN_PROPS;
    assert(props); key(DU_KEY_ESC);
    /* Empty Recycle Bin: confirmation, the sound event, an empty folder view. */
    activate(K_EMPTY_BIN);
    assert(du_dialog_id(&ui) == D_BIN_EMPTY && !strcmp(ui.dialog.text, "Are you sure you want to delete 'zeta.txt'?"));
    key(DU_KEY_ESC); assert(app.model.count == 1);
    fake_file("/Documents/Desktop/extra.txt", 1, 1);
    assert(!files_navigate(&app, "/Documents/Desktop"));
    select_row(list_find("extra.txt")); key(DU_KEY_DELETE); yes(); tick();
    activate(K_GO_RECYCLE); files_test_wait(&app);
    assert(app.model.count == 2);
    activate(K_EMPTY_BIN);
    assert(du_dialog_id(&ui) == D_BIN_EMPTY && !strcmp(ui.dialog.text, "Are you sure you want to delete these 2 items?") && !strcmp(ui.dialog.title, "Confirm Multiple File Delete"));
    dump("bin-confirm-empty");
    unsigned before = sound_count; yes(); tick();
    assert(app.model.count == 0 && strstr(app.note, "empty") && strstr(du_get(&ui, W_STATUS)->text, "empty"));
    int emptied = 0; for (unsigned i = before; i < sound_count; i++) emptied |= sounds[i] == 13;   /* SOUND_EMPTY_RECYCLE */
    assert(emptied);
    int info = fake_lookup("/RECYCLED/INFO2"); assert(info >= 0 && fake_nodes[info].meta.size == RB_HEADER);
    dump("bin-empty");
    activate(K_EMPTY_BIN);
    assert(du_dialog_id(&ui) != D_BIN_EMPTY); key(DU_KEY_ENTER);
    /* The normal File menu returns outside the bin. */
    assert(!files_navigate(&app, "/Documents")); assert(ui.menus[0].items == app.file_items && ui.menus[0].count == 11);
    assert(!strcmp(LIST()->columns[1].title, "Size") && LIST()->column_count == 4);
    files_destroy(&app); fake_assert_clean();
    puts("PASS Files Recycle Bin view: columns, original name/location/date, menus, sorting, read-only items, item properties, Restore, Delete, Empty with the sound event");
}

static void properties_tests(void)
{
    setup(FM_RECYCLE);
    assert(!app.model.count);
    activate(K_BIN_PROPS);
    assert(du_dialog_id(&ui) == D_BIN_PROPS);
    struct du_dialog *d = &ui.dialog;
    assert(d->items[6].value == 0 && !strcmp(d->items[8].text, "10") && d->items[9].value == 1 && d->items[4].value == 1 && d->items[5].value == 0);
    assert(strstr(d->items[7].text, "Maximum size") && strstr(d->items[7].text, "51.1 MB"));    /* 10 % of the assumed 512 MiB (the size text truncates) */
    dump("bin-properties");
    /* One setting for all drives: the global values. */
    du_set_text(&d->items[8], "25"); d->items[6].value = 1; d->items[9].value = 0;
    click(d->items[0].box.x + 30, d->items[0].box.y + 10);
    assert(!du_dialog_id(&ui));
    struct rb_policy p; assert(!rb_policy_load(&fake_vfs, &p));
    assert(p.percent == 25 && p.nuke && !p.confirm && !p.independent);
    /* A bad size keeps the dialog open. */
    activate(K_BIN_PROPS); d = &ui.dialog;
    assert(d->items[6].value == 1 && !strcmp(d->items[8].text, "25") && d->items[9].value == 0);
    du_set_text(&d->items[8], "250"); click(d->items[0].box.x + 30, d->items[0].box.y + 10);
    assert(du_dialog_id(&ui) == D_BIN_PROPS && strstr(d->items[7].text, "1 to 100"));
    du_set_text(&d->items[8], "abc"); click(d->items[0].box.x + 30, d->items[0].box.y + 10);
    assert(du_dialog_id(&ui) == D_BIN_PROPS);
    click(d->items[1].box.x + 30, d->items[1].box.y + 10);                                         /* Cancel */
    assert(!du_dialog_id(&ui)); assert(!rb_policy_load(&fake_vfs, &p) && p.percent == 25);
    /* The Drive button needs "Configure drives independently"; then it walks the drives. */
    app.media.version = CIUKI_MEDIA_VERSION; app.media.removable_mask = 1u << 4;                   /* E: removable: D: is fixed */
    activate(K_BIN_PROPS); d = &ui.dialog;
    click(d->items[2].box.x + 20, d->items[2].box.y + 10);
    assert(du_dialog_id(&ui) == D_BIN_PROPS && strstr(d->items[3].text, "Select"));
    click(d->items[5].box.x + 10, d->items[5].box.y + 8); assert(d->items[5].value && !d->items[4].value);
    click(d->items[2].box.x + 20, d->items[2].box.y + 10);
    assert(!strcmp(d->items[3].text, "Settings for drive C:") && d->items[4].hidden && d->items[5].hidden);
    du_set_text(&d->items[8], "40"); d->items[6].value = 1;
    click(d->items[2].box.x + 20, d->items[2].box.y + 10);
    assert(!strcmp(d->items[3].text, "Settings for drive D:") && !strcmp(d->items[8].text, "10") && !d->items[6].value);
    du_set_text(&d->items[8], "5");
    click(d->items[2].box.x + 20, d->items[2].box.y + 10);
    assert(strstr(d->items[3].text, "Global") && !d->items[4].hidden && d->items[5].value);
    dump("bin-properties-global");
    click(d->items[0].box.x + 30, d->items[0].box.y + 10);
    assert(!rb_policy_load(&fake_vfs, &p));
    assert(p.independent && p.drive_percent[2] == 40 && p.drive_nuke[2] && p.drive_percent[3] == 5 && !p.drive_nuke[3] && p.percent == 25);
    assert(rb_policy_nuke(&p, 'C') && !rb_policy_nuke(&p, 'D') && rb_policy_percent(&p, 'D') == 5);
    files_destroy(&app); fake_assert_clean();
    puts("PASS Files Recycle Bin Properties: global and per-drive size and 'do not move files', confirmation, validation, Cancel, saved to recycle.cfg");
}

static void drop_tests(void)
{
    setup("/Documents/Desktop");
    fake_file("/Documents/Desktop/dropme.txt", 10, 1); fake_file("/mnt/e/stick.txt", 10, 2); fake_file("/Documents/Desktop/second.txt", 10, 3);
    assert(!files_navigate(&app, "Recycle Bin"));
    app.drag_status = on_status;
    struct du_rect body = du_list_body(LIST());
    int x = body.x + body.w / 2, y = body.y + body.h / 2;
    char *one[] = {"/Documents/Desktop/dropme.txt"}; char type = 'F';
    assert(!fm_drag_save(&fake_vfs, one, &type, 1));
    int before = statuses;
    files_drag_over(&app, 21, 0, x, y, 0);
    assert(statuses == before + 1 && status_effect == FM_EFFECT_MOVE && status_allowed == FM_EFFECT_MOVE);
    files_drag_over(&app, 21, 0, x, y, FM_KEY_CTRL); assert(status_effect == FM_EFFECT_NONE);          /* a bin cannot copy */
    files_drag_over(&app, 21, 0, x, y, FM_KEY_ALT); assert(status_effect == FM_EFFECT_NONE);
    files_drag_drop(&app, 21, x, y, FM_EFFECT_MOVE);
    assert(du_dialog_id(&ui) == D_DELETE && strstr(ui.dialog.text, "send 'dropme.txt' to the Recycle Bin"));
    key(DU_KEY_ESC); assert(fake_lookup("/Documents/Desktop/dropme.txt") >= 0 && !app.drop_count);
    files_drag_drop(&app, 21, x, y, FM_EFFECT_MOVE); yes(); tick();
    assert(fake_lookup("/Documents/Desktop/dropme.txt") < 0 && fake_lookup("/RECYCLED/Dc1.txt") >= 0 && app.model.count == 1);
    /* A CD cannot be deleted from; the bin's own items are already in it. */
    app.media.removable_mask = 1u << 4; app.media.type[4] = CIUKI_MEDIA_OPTICAL;
    char *cd[] = {"/mnt/e/stick.txt"}; assert(!fm_drag_save(&fake_vfs, cd, &type, 1));
    files_drag_over(&app, 22, 0, x, y, 0); assert(status_effect == FM_EFFECT_NONE && status_allowed == 0);
    app.media.removable_mask = 0; app.media.type[4] = 0;
    char *inside[] = {"/RECYCLED/Dc1.txt"}; assert(!fm_drag_save(&fake_vfs, inside, &type, 1));
    files_drag_over(&app, 23, 0, x, y, 0); assert(status_effect == FM_EFFECT_NONE);
    /* Removable-media items dropped on the bin are deleted for good, with the plain wording. */
    char *usb[] = {"/mnt/e/stick.txt"}; assert(!fm_drag_save(&fake_vfs, usb, &type, 1));
    files_drag_over(&app, 24, 0, x, y, 0); assert(status_effect == FM_EFFECT_MOVE);
    files_drag_drop(&app, 24, x, y, FM_EFFECT_MOVE);
    assert(!strcmp(ui.dialog.text, "Are you sure you want to delete 'stick.txt'?")); yes(); tick();
    assert(fake_lookup("/mnt/e/stick.txt") < 0 && fake_lookup("/mnt/e/RECYCLED") < 0);
    files_destroy(&app); fake_assert_clean();
    puts("PASS Files drop on the Recycle Bin window: move only, Ctrl/Alt refused, confirmation, CD and bin items refused, removable media delete for good");
}

int main(int argc, char **argv)
{
    out_dir = argc > 1 ? argv[1] : NULL;
    pixels = calloc(WIDTH * HEIGHT + 1, 4); assert(pixels);
    pixels[WIDTH * HEIGHT] = 0xdeadbeefu;
    delete_tests(); bin_view_tests(); properties_tests(); drop_tests();
    puts("PASS Files Recycle Bin UI");
    return 0;
}
