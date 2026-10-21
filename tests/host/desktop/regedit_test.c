/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../../apps/regedit/regedit.h"
#include <ciuki/devices.h>
#include "acc_test.h"
#include "device_fixture.h"
int ciuki_devices(struct ciuki_devices_info *s) { device_fixture(s); return 0; }
static void action(struct re_app *a, int id)
{
    struct du_event e = {.type = DU_EV_ACTIVATE, .id = id}; re_event(a->ui, &e, a);
}
static void input(struct re_app *a, int id, const char *text)
{
    assert(du_dialog_id(a->ui) == id);
    /* The real toolkit emits DU_EV_DIALOG with a copied edit field. */
    du_set_text(&a->ui->dialog.items[2], text); at_tap(a->ui, DU_KEY_ENTER);
}
static void approve(struct re_app *a, int yes)
{
    assert(du_dialog_id(a->ui) == RE_CONFIRM);
    struct du_rect box = du_dialog_button(a->ui, yes ? 0 : 1); at_click(a->ui, box.x + box.w / 2, box.y + box.h / 2);
}
static void dismiss(struct re_app *a) { at_tap(a->ui, DU_KEY_ESC); assert(!du_dialog_id(a->ui)); }
int main(int argc, char **argv)
{
    assert(argc == 2); struct re_app *a = calloc(1, sizeof(*a)); struct du_ui *u = calloc(1, sizeof(*u));
    uint32_t *pixels = calloc(RE_WIDTH * RE_HEIGHT, 4); assert(a && u && pixels);
    du_init(u, pixels, RE_WIDTH, RE_HEIGHT); assert(!re_attach(a, u, argv[1]));
    assert(du_get(u, RE_TREE)->count == 9); /* five roots + Hardware children */
    du_render(u);
    du_tick(u,2000000000ull); assert(!u->dirty);
    du_message(u,RE_ERROR,"Question","Unchanged",0,NULL,0,0,0); du_render(u);
    du_tick(u,4000000000ull); assert(!u->dirty); du_dialog_close(u);
    action(a, RE_NEW_KEY); input(a, RE_NAME, "Test"); approve(a, 0);
    assert(re_select(a, "System\\Test") && errno == ENOENT);
    action(a, RE_NEW_KEY); input(a, RE_NAME, "Test"); approve(a, 1); assert(!re_select(a, "System\\Test"));
    action(a, RE_NEW_STRING); input(a, RE_NAME, "message"); input(a, RE_DATA, "\"hello\"");
    uint64_t g = reg_generation(a->roots[0]); approve(a, 0); assert(reg_generation(a->roots[0]) == g && !a->value_count);
    action(a, RE_NEW_STRING); input(a, RE_NAME, "message"); input(a, RE_DATA, "\"hello\""); approve(a, 1); assert(a->value_count == 1);
    /* Selection through the actual list/tree widgets, not synthetic app state. */
    struct du_widget *values = du_get(u, RE_VALUES); struct du_rect label = du_list_label(values, 0);
    at_click(u, label.x + 3, label.y + 6); assert(du_focused(u) == RE_VALUES);
    action(a, RE_EDIT); input(a, RE_DATA, "\"changed\""); approve(a, 1);
    char text[128]; size_t n = sizeof(text); assert(!reg_get_string(a->roots[0], "System\\Test", "message", text, &n) && !strcmp(text, "changed"));
    assert(!re_find(a, "CHANGED", 0) && a->value_selected == 0 && !strcmp(a->nodes[a->selected].path, "System\\Test"));
    action(a, RE_NEW_INT); input(a, RE_NAME, "count"); input(a, RE_DATA, "-12"); approve(a, 1);
    action(a, RE_NEW_BOOL); input(a, RE_NAME, "enabled"); input(a, RE_DATA, "true"); approve(a, 1);
    action(a, RE_NEW_BINARY); input(a, RE_NAME, "bytes"); input(a, RE_DATA, "hex:00ff"); approve(a, 1); assert(a->value_count == 4);
    /* Bad literals report an error without changing the persisted generation. */
    g = reg_generation(a->roots[0]); action(a, RE_NEW_INT); input(a, RE_NAME, "bad"); input(a, RE_DATA, "2147483648"); approve(a, 1);
    assert(du_dialog_id(u) == RE_ERROR && reg_generation(a->roots[0]) == g); dismiss(a);
    /* File dialog completion: exercise production routing with a host file path,
     * since the guest /Documents volume is not mounted on the host. */
    size_t cap = strlen(argv[1]) + 32; char *file = malloc(cap); assert(file); char *slash = strrchr(argv[1], '/'); assert(slash);
    snprintf(file, cap, "%.*s/export.reg", (int)(slash - argv[1]), argv[1]);
    action(a, RE_EXPORT); assert(du_dialog_id(u) == RE_FILE); du_dialog_close(u);
    struct du_event event = {.type = DU_EV_DIALOG, .id = RE_FILE, .index = 0, .text = file}; re_event(u, &event, a);
    assert(!access(file, F_OK));
    values = du_get(u, RE_VALUES); label = du_list_label(values, 0); at_click(u, label.x + 3, label.y + 6);
    action(a, RE_DELETE); approve(a, 0); assert(a->value_count == 4);
    action(a, RE_DELETE); approve(a, 1); assert(a->value_count == 3);
    action(a, RE_IMPORT); assert(du_dialog_id(u) == RE_FILE); du_dialog_close(u); re_event(u, &event, a); approve(a, 0); assert(a->value_count == 3);
    action(a, RE_IMPORT); assert(du_dialog_id(u) == RE_FILE); du_dialog_close(u); re_event(u, &event, a); approve(a, 1); assert(a->value_count == 4);
    /* Poll follows writes from an owning app, and preserves tree selection. */
    struct reg_root *other = reg_open_root_at("System", argv[1]); assert(other);
    assert(!reg_set_bool(other, "System\\Test", "external", 1)); reg_close_root(other);
    du_render(u);
    event = (struct du_event){.type = DU_EV_TICK, .id = RE_POLL}; re_event(u, &event, a); assert(a->value_count == 5 && u->dirty);
    assert(!re_select(a, "Hardware\\PCI\\00:05.0")); assert(a->edit_menu[0].flags & DU_ITEM_DISABLED);
    action(a, RE_NEW_KEY); assert(du_dialog_id(u) == RE_ERROR); dismiss(a);
    action(a, RE_DELETE); assert(du_dialog_id(u) == RE_ERROR); dismiss(a);
    /* Tree collapse/expand through pointer input. */
    struct du_widget *tree = du_get(u, RE_TREE); unsigned before = tree->count;
    tree->active = 0; du_focus(u, RE_TREE); at_tap(u, DU_KEY_LEFT); assert(tree->count < before);
    at_tap(u, DU_KEY_RIGHT); assert(tree->count == before);
    assert(!re_select(a, "System\\Test")); du_focus(u, RE_TREE); action(a, RE_DELETE); approve(a, 1);
    assert(re_select(a, "System\\Test") && errno == ENOENT);
    du_render(u); re_destroy(a); du_release(u); free(file); free(pixels); free(u); free(a);
    puts("PASS Registry Editor: real toolkit tree/list/input/confirmation, typed CRUD, search, import/export, generation polling, Hardware read-only"); return 0;
}
