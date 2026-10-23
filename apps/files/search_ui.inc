/* SPDX-License-Identifier: MIT
 * G7 criteria use the existing modal controls; Next Page keeps edits. */
static void find_text(char *out, size_t size, const struct du_dialog *d, unsigned index)
{ snprintf(out, size, "%s", d->items[index].text); }
static int find_accept(struct du_ui *u, int button, void *ctx)
{
    struct files_app *a = ctx; struct du_dialog *d = &u->dialog;
    if (button == 1) return 0;
    if (a->find_page == 0) {
        find_text(a->find_edit.named, sizeof(a->find_edit.named), d, 4);
        find_text(a->find_edit.containing, sizeof(a->find_edit.containing), d, 6);
        find_text(a->find_edit.look_in, sizeof(a->find_edit.look_in), d, 8);
        a->find_edit.recursive = d->items[9].value;
    } else if (a->find_page == 1) {
        find_text(a->find_from, sizeof(a->find_from), d, 4); find_text(a->find_to, sizeof(a->find_to), d, 6);
    } else {
        find_text(a->find_edit.type, sizeof(a->find_edit.type), d, 4); find_text(a->find_size, sizeof(a->find_size), d, 6);
        a->find_edit.size_mode = d->items[7].value ? 2 : 1;
    }
    if (button == 2) return 0;
    a->find_edit.date_from_set = !!*a->find_from; a->find_edit.date_to_set = !!*a->find_to;
    int invalid = (a->find_edit.date_from_set && fm_search_date(a->find_from, 0, &a->find_edit.date_from)) ||
        (a->find_edit.date_to_set && fm_search_date(a->find_to, 1, &a->find_edit.date_to)) ||
        (a->find_edit.date_from_set && a->find_edit.date_to_set && a->find_edit.date_from > a->find_edit.date_to);
    if (!*a->find_size) a->find_edit.size_mode = 0;
    else {
        if (!a->find_edit.size_mode) a->find_edit.size_mode = 1;
        invalid |= !!fm_search_size(a->find_size, &a->find_edit.size_bytes);
    }
    if (invalid) { snprintf(d->title, sizeof(d->title), "Find - Check dates (YYYY-MM-DD) and size (KiB)"); u->dirty = 1; return 1; }
    return 0;
}
static void find_pair(struct files_app *a, unsigned index, const char *label, const char *text, int y)
{
    int w = a->ui->dialog.box.w;
    bin_control(a, index, DU_LABEL, label, 16, y, w-32, 20);
    bin_control(a, index+1, DU_EDIT, text, 16, y+20, w-32, 24);
}
static void find_dialog(struct files_app *a)
{
    static const char *const buttons[] = {"&Find Now", "Cancel", "&Next Page"};
    static const char *const titles[] = {"Find: Name & Location", "Find: Date Modified", "Find: Advanced"};
    du_message(a->ui, D_FIND, titles[a->find_page], "", 0, buttons, 3, 0, 1);
    struct du_dialog *d = &a->ui->dialog;
    int w = a->width-16 < 540 ? a->width-16 : 540, h = 306;
    d->box = (struct du_rect){(a->width-w)/2, (a->height-h)/2, w, h};
    for (unsigned i = 0; i < 3; i++) d->items[i].box = (struct du_rect){d->box.x+w-300+(int)i*96, d->box.y+h-34, 90, 24};
    if (a->find_page == 0) {
        find_pair(a, 3, "&Named (wildcards, or part of a name):", a->find_edit.named, 36);
        find_pair(a, 5, "&Containing text:", a->find_edit.containing, 92);
        find_pair(a, 7, "&Look in (folder, drive or Computer):", a->find_edit.look_in, 148);
        bin_control(a, 9, DU_CHECKBOX, "Include &subfolders", 16, 210, w-32, 24)->value = (uint8_t)a->find_edit.recursive;
    } else if (a->find_page == 1) {
        find_pair(a, 3, "Modified &from (YYYY-MM-DD, inclusive):", a->find_from, 36);
        find_pair(a, 5, "Modified &through (YYYY-MM-DD, inclusive):", a->find_to, 104);
        bin_control(a, 7, DU_LABEL, "Leave either date empty for no limit.", 16, 186, w-32, 28);
    } else {
        find_pair(a, 3, "Of &type (all, folders, files, .txt or *.txt):", a->find_edit.type, 36);
        find_pair(a, 5, "&Size in KiB (blank for any size):", a->find_size, 104);
        bin_control(a, 7, DU_CHECKBOX, "At &most (unchecked: at least)", 16, 176, w-32, 24)->value = a->find_edit.size_mode == 2;
    }
    d->focus = 4; d->accept = find_accept; d->accept_ctx = a; a->ui->dirty = 1;
}
static void find_stop(struct files_app *a)
{
    struct files_worker *w = a->worker;
    pthread_mutex_lock(&w->lock);
    __atomic_add_fetch(&w->revision, 1, __ATOMIC_RELEASE);
    struct files_scan *pending = w->pending, *result = w->result;
    w->pending = w->result = NULL;
    pthread_mutex_unlock(&w->lock);
    scan_free(pending); scan_free(result);
    a->loading = a->reading = a->find_running = 0;
    snprintf(a->note, sizeof(a->note), "Search stopped. Previous results retained.");
    files_update(a);
}
void files_find(struct files_app *a)
{
    if (a->find_running) { find_stop(a); return; }
    if (a->loading) { a->find_initial = 1; return; }
    if (du_dialog_id(a->ui) || a->job_active) return;
    if (!a->find_again) {
        memset(&a->find_edit, 0, sizeof(a->find_edit)); a->find_edit.recursive = 1;
        fm_display_path(a->find_edit.look_in, fm_is_recycle(a->model.path) ? FM_COMPUTER : a->model.path);
        a->find_again = 1;
    }
    if (!a->model.searching) fm_display_path(a->find_edit.look_in, fm_is_recycle(a->model.path) ? FM_COMPUTER : a->model.path);
    a->find_page = 0; find_dialog(a);
}
