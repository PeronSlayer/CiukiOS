/* SPDX-License-Identifier: MIT
 * Files controller and libdeskui rendering against the fake image tree.
 * Replays the files-qemu-t23 script in client coordinates, checks the
 * checkpoints and filesystem outcome, and writes deterministic frames. */
#include "fake_vfs.h"
#include "../../../apps/files/app.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#define WIDTH 760
#define HEIGHT 520
static uint32_t *pixels;
static struct du_ui ui;
static struct files_app app;
static const char *out_dir, *portrait_path;
static uint64_t clock_ns = 1000000000ull;
static int checkpoints[8], checkpoint_count, closed;

static void dump(const char *name)
{
    du_render(&ui);
    assert(pixels[WIDTH * HEIGHT] == 0xdeadbeefu);
    if (!out_dir) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/files-%s.xrgb", out_dir, name);
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(pixels, 4, WIDTH * HEIGHT, f) == WIDTH * HEIGHT);
    fclose(f);
}
static void event(int type, int code, int value, int value2)
{
    clock_ns += 20000000ull;
    struct ciuki_input_event e = {.type = (uint32_t)type, .code = code, .value = value, .value2 = value2, .monotonic_ns = clock_ns};
    du_input(&ui, &e);
}
static void motion(int x, int y) { event(CIUKI_INPUT_MOTION, 0, x, y); }
static void press(int b) { event(CIUKI_INPUT_BUTTON, b, 1, 0); }
static void release(int b) { event(CIUKI_INPUT_BUTTON, b, 0, 0); }
static void click(int x, int y) { motion(x, y); press(1); release(1); clock_ns += 600000000ull; }
static void rclick(int x, int y) { motion(x, y); press(2); release(2); clock_ns += 600000000ull; }
static void key(int code) { event(CIUKI_INPUT_KEY, code, 1, 0); event(CIUKI_INPUT_KEY, code, 0, 0); }
static void chord(int mod, int code) { event(CIUKI_INPUT_KEY, mod, 1, 0); key(code); event(CIUKI_INPUT_KEY, mod, 0, 0); }
static void tick(void) { for (int i = 0; i < 200 && app.job_active; i++) { clock_ns += 5000000ull; du_tick(&ui, clock_ns); } }
static void on_checkpoint(struct files_app *a, int stage) { (void)a; checkpoints[checkpoint_count++] = stage; }
static void on_close(struct files_app *a) { (void)a; closed = 1; }
static int spawned;
static int on_spawn(struct files_app *a, const char *program, char *const argv[]) { (void)a; (void)argv; spawned++; return strcmp(program, "/bin/hello") ? -2 : 77; }
static int portrait(uint32_t *p)
{
    if (!portrait_path) return -1;
    FILE *f = fopen(portrait_path, "rb"); if (!f) return -1;
    size_t n = fread(p, 4, 256 * 256, f); fclose(f);
    return n == 256 * 256 ? 0 : -1;
}
static int list_row_y(struct du_widget *list, unsigned index) { struct du_rect r = du_list_label(list, index); return r.y + r.h / 2; }
static int list_find(const char *name)
{ for (unsigned i = 0; i < app.model.count; i++) if (!strcmp(app.model.entries[i].name, name)) return (int)i; return -1; }

/* Client-coordinate targets shared with the QEMU script (window at 4,36:
 * client origin 7,60). The Python test converts them to screen moves. */
static void script_coordinates(void)
{
    struct du_rect view = du_menu_title(&ui, 2), file = du_menu_title(&ui, 0);
    struct du_rect vbox = du_menu_box(&ui, app.view_items, 10, view.x, view.y + view.h);
    struct du_rect fbox = du_menu_box(&ui, app.file_items, 11, file.x, file.y + file.h);
    struct du_widget *places = du_get(&ui, 103);
    struct du_rect system = du_list_label(places, 2);
    printf("SCRIPT view_title=%d,%d list_item=%d,%d places_system=%d,%d file_title=%d,%d new_folder=%d,%d\n",
           view.x + view.w / 2, view.y + view.h / 2, vbox.x + 40, vbox.y + 3 + 17 + 8,
           system.x + system.w / 2, system.y + system.h / 2, file.x + file.w / 2, file.y + file.h / 2,
           fbox.x + 40, fbox.y + 3 + 17 + 17 + 9 + 8);
}

int main(int argc, char **argv)
{
    out_dir = argc > 1 ? argv[1] : NULL;
    portrait_path = argc > 2 ? argv[2] : NULL;
    pixels = calloc(WIDTH * HEIGHT + 1, 4); assert(pixels);
    pixels[WIDTH * HEIGHT] = 0xdeadbeefu;
    fake_image_tree();
    du_init(&ui, pixels, WIDTH, HEIGHT);
    app.test = 1; app.checkpoint = on_checkpoint; app.close = on_close; app.spawn = on_spawn; app.portrait = portrait;
    assert(!files_setup(&app, &ui, &fake_vfs, "/"));
    assert(!strcmp(app.model.path, "/") && app.model.count == 2);
    dump("root-details");
    script_coordinates();

    /* View > List through the menu bar (the QEMU script does the same). */
    struct du_rect view = du_menu_title(&ui, 2);
    click(view.x + view.w / 2, view.y + view.h / 2);
    assert(ui.menu == 2);
    dump("menu-view");
    struct du_rect vbox = du_menu_box(&ui, app.view_items, 10, view.x, view.y + view.h);
    click(vbox.x + 40, vbox.y + 3 + 17 + 8);
    assert(ui.menu < 0 && du_get(&ui, 107)->view == DU_LIST);

    /* Places > System: checkpoint 1. */
    struct du_widget *places = du_get(&ui, 103);
    struct du_rect system = du_list_label(places, 2);
    click(system.x + system.w / 2, system.y + system.h / 2);
    assert(!strcmp(app.model.path, "/system") && checkpoint_count == 1 && checkpoints[0] == 1);
    dump("cp1-system");

    /* File > New Folder: checkpoint 2, inline rename open. */
    struct du_rect file = du_menu_title(&ui, 0);
    click(file.x + file.w / 2, file.y + file.h / 2);
    struct du_rect fbox = du_menu_box(&ui, app.file_items, 11, file.x, file.y + file.h);
    dump("menu-file");
    click(fbox.x + 40, fbox.y + 3 + 17 + 17 + 9 + 8);
    assert(checkpoint_count == 2 && app.rename_index >= 0 && fake_lookup("/system/New Folder") >= 0);
    dump("cp2-new-folder");

    /* Type "Ciuki" (Shift+C) and Enter: checkpoint 3. */
    chord(DU_KEY_LSHIFT, 0x2e); key(0x17); key(0x16); key(0x25); key(0x17); key(DU_KEY_ENTER);
    assert(checkpoint_count == 3 && fake_lookup("/system/Ciuki") >= 0 && fake_lookup("/system/New Folder") < 0);
    dump("cp3-renamed");

    /* b selects BOOT.CFG, Ctrl+C, c selects Ciuki, Enter opens, Ctrl+V. */
    key(0x30); assert(!strcmp(app.model.entries[app.model.active].name, "BOOT.CFG"));
    chord(DU_KEY_LCTRL, 0x2e); assert(app.model.clipboard_count == 1);
    key(0x2e); assert(!strcmp(app.model.entries[app.model.active].name, "Ciuki"));
    key(DU_KEY_ENTER); assert(!strcmp(app.model.path, "/system/Ciuki") && app.model.count == 0);
    chord(DU_KEY_LCTRL, 0x2f); assert(app.job_active);
    dump("copy-progress");
    tick();
    assert(!app.job_active && checkpoint_count == 4 && fake_lookup("/system/Ciuki/BOOT.CFG") >= 0);
    assert(app.model.count == 1 && app.model.entries[0].selected);
    dump("cp4-copied");

    /* Del, confirm with Enter: checkpoint 5. */
    key(DU_KEY_DELETE); assert(du_dialog_id(&ui));
    dump("confirm-delete");
    key(DU_KEY_ENTER); tick();
    assert(checkpoint_count == 5 && fake_lookup("/system/Ciuki/BOOT.CFG") < 0 && fake_lookup("/system/BOOT.CFG") >= 0);
    dump("cp5-deleted");
    for (int i = 0; i < 5; i++) assert(checkpoints[i] == i + 1);

    /* Backspace goes up and selects the folder that was left. */
    key(DU_KEY_BACKSPACE); assert(!strcmp(app.model.path, "/system") && !strcmp(app.model.entries[app.model.active].name, "Ciuki"));

    /* Views, sorting, selection keys. */
    click(du_get(&ui, 9)->box.x + 10, du_get(&ui, 9)->box.y + 10);
    assert(du_get(&ui, 107)->view == DU_DETAILS);
    struct du_widget *list = du_get(&ui, 107);
    dump("system-details");
    int size_header_x = list->box.x + 2 + list->columns[0].width + 10;
    click(size_header_x, list->box.y + 8);
    assert(app.model.sort == 1 && !app.model.descending);
    click(size_header_x, list->box.y + 8);
    assert(app.model.sort == 1 && app.model.descending);
    dump("sort-size-desc");
    click(list->box.x + 2 + 10, list->box.y + 8);
    assert(app.model.sort == 0);
    chord(DU_KEY_LCTRL, 0x1e); assert(fm_selected(&app.model, NULL) == app.model.count);
    key(DU_KEY_HOME); assert(app.model.active == 0 && fm_selected(&app.model, NULL) == 1);
    chord(DU_KEY_LSHIFT, DU_KEY_DOWN); event(CIUKI_INPUT_KEY, DU_KEY_LSHIFT, 1, 0); key(DU_KEY_DOWN); event(CIUKI_INPUT_KEY, DU_KEY_LSHIFT, 0, 0);
    assert(fm_selected(&app.model, NULL) == 3);
    int vmm = list_find("VMM.ELF");
    event(CIUKI_INPUT_KEY, DU_KEY_LCTRL, 1, 0); click(list->box.x + 40, list_row_y(list, (unsigned)vmm)); event(CIUKI_INPUT_KEY, DU_KEY_LCTRL, 0, 0);
    assert(fm_selected(&app.model, NULL) == 4 && app.model.entries[vmm].selected);
    dump("multi-select");
    click(du_get(&ui, 9)->box.x + 10, du_get(&ui, 9)->box.y + 10);
    assert(list->view == DU_ICONS);
    dump("system-icons");
    rclick(list->box.x + list->box.w - 30, list->box.y + list->box.h - 30);
    assert(ui.popup && fm_selected(&app.model, NULL) == 0);
    dump("context-background");
    key(DU_KEY_ESC); assert(!ui.popup);

    /* Item context menu, Properties of a folder, then the replace prompt. */
    int assets = list_find("assets");
    struct du_rect ar = du_list_label(list, (unsigned)assets);
    rclick(ar.x + ar.w / 2, ar.y - 10);
    assert(ui.popup && app.model.entries[assets].selected);
    dump("context-item");
    key(DU_KEY_ESC);
    chord(DU_KEY_LALT, DU_KEY_ENTER); tick();
    assert(du_dialog_id(&ui));
    dump("properties");
    key(DU_KEY_ENTER); assert(!du_dialog_id(&ui));
    int boot = list_find("BOOT.CFG");
    click(du_list_label(list, (unsigned)boot).x + 4, du_list_label(list, (unsigned)boot).y + 4);
    chord(DU_KEY_LCTRL, 0x2e);
    fake_file("/system/Ciuki/BOOT.CFG", 5, 'z');
    int ciuki = list_find("Ciuki");
    struct du_rect cr = du_list_label(list, (unsigned)ciuki);
    rclick(cr.x + cr.w / 2, cr.y - 10);
    /* "Paste" in the folder's context menu pastes into it. */
    unsigned paste = 0; for (unsigned i = 0; i < ui.popup_count; i++) if (ui.popup[i].label && !strcmp(ui.popup[i].label, "&Paste")) paste = i;
    struct du_rect pbox = du_menu_box(&ui, ui.popup, ui.popup_count, ui.popup_x, ui.popup_y);
    int y = pbox.y + 3; for (unsigned i = 0; i < paste; i++) y += ui.popup[i].flags & DU_ITEM_SEPARATOR ? 9 : 17;
    click(pbox.x + 30, y + 8);
    tick();
    assert(du_dialog_id(&ui) && app.job.state == FM_REPLACE);
    dump("replace-prompt");
    key(DU_KEY_ENTER); tick();
    assert(!app.job_active && fake_nodes[fake_lookup("/system/Ciuki/BOOT.CFG")].meta.size == 16);

    /* Same-folder paste makes "Copy of", Rename via F2, error on a bad name. */
    chord(DU_KEY_LCTRL, 0x2f); tick();
    assert(fake_lookup("/system/Copy of BOOT.CFG") >= 0);
    key(DU_KEY_F2); assert(app.rename_index >= 0);
    chord(DU_KEY_LSHIFT, 0x35); key(DU_KEY_ENTER);  /* "?" is invalid */
    assert(du_dialog_id(&ui) && fake_lookup("/system/Copy of BOOT.CFG") >= 0);
    dump("rename-error");
    key(DU_KEY_ENTER);

    /* Tree: collapse the root, expand again; address bar navigation. */
    struct du_widget *tree = du_get(&ui, 105);
    dump("tree");
    chord(DU_KEY_LALT, 0x20); assert(du_focused(&ui) == 100);
    const char *bin = "/bin";
    for (const char *p = bin; *p; p++) {
        static const unsigned char map[128] = {['/'] = 0x35, ['b'] = 0x30, ['i'] = 0x17, ['n'] = 0x31};
        key(map[(unsigned char)*p]);
    }
    key(DU_KEY_ENTER); assert(!strcmp(app.model.path, "/bin") && app.model.count == 6);
    assert(tree->count >= 3);
    dump("bin-icons");
    int hello = list_find("hello");
    app.model.active = (unsigned)hello; fm_select(&app.model, (unsigned)hello, 0); files_update(&app);
    key(DU_KEY_ENTER); assert(spawned == 1);
    /* f2-32: the desktop's refusal of a LAUNCH request (CiukNote) is reported in a dialog. */
    files_launch_failed(&app, EPERM);
    assert(ui.dialog.kind == DU_DLG_MESSAGE && strstr(ui.dialog.text, "Cannot start CiukNote:") && strstr(ui.dialog.text, "Access is denied"));
    key(DU_KEY_ENTER); assert(!ui.dialog.kind);
    /* Help and About dialogs. */
    key(DU_KEY_F1); dump("help"); key(DU_KEY_ESC);
    struct du_rect help = du_menu_title(&ui, 4); click(help.x + 5, help.y + 5);
    struct du_rect hbox = du_menu_box(&ui, app.help_items, 3, help.x, help.y + help.h);
    click(hbox.x + 20, hbox.y + 3 + 17 + 9 + 8);
    dump("about"); key(DU_KEY_ENTER);
    /* A close request during a job cancels it, cleans up, then closes. */
    chord(DU_KEY_LCTRL, 0x1e); chord(DU_KEY_LCTRL, 0x2e);
    assert(!files_navigate(&app, "/system/Ciuki")); chord(DU_KEY_LCTRL, 0x2f); assert(app.job_active);
    assert(!files_close_request(&app) && !closed);
    tick(); assert(closed && !app.job_active && app.job.state == FM_CANCELLED);
    closed = 0;
    /* Alt+F4 closes. */
    chord(DU_KEY_LALT, DU_KEY_F4); assert(closed);
    files_destroy(&app);
    fake_assert_clean();
    free(pixels);
    puts("PASS Files UI: script checkpoints 1-5, views, sorting, selection, context menus, properties, replace, Copy of, rename errors, tree, address, open, help/about");
    return 0;
}
