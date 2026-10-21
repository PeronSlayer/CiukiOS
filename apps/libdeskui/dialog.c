/* SPDX-License-Identifier: MIT
 * Modal dialogs drawn inside the client window: message box, input box,
 * file open/save and progress with Cancel. */
#include "internal.h"
#include "../desktop/sounds.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__i386__) || defined(DU_NATIVE_MEDIA)
#include <fcntl.h>
#include <unistd.h>
int ciuki_devctl(int, uint32_t, void *, uint32_t);
#endif

#define TITLE_H (DESK_FRAME_TITLE - 3)   /* the window frame: body at box.y + 24 */
#define BTN_W 75
#define BTN_H 23
#define LINE_H 15
#define TAB_X 84

enum { ID_BUTTON = -1000, ID_UP = -2000, ID_LIST = -3000, ID_NAME = -4000, ID_FOLDER = -5000, ID_EDIT = -6000, ID_BAR = -7000, ID_PLACES = -8000 };
enum { F_ACCEPT, F_CANCEL, F_UP, F_FOLDER, F_LIST, F_NAME, F_LOOK, F_FILE, F_PLACES, F_PLACES_TITLE };
#define PLACE_MAX 31
struct du_dialog_places {
    struct du_item rows[PLACE_MAX];
    struct { char path[32], label[64]; } entries[PLACE_MAX];
    unsigned count;
    uint64_t next_poll;
    int fd;
};
static void places_close(struct du_dialog *d)
{
    if (!d->places) return;
#if defined(__i386__) || defined(DU_NATIVE_MEDIA)
    if (d->places->fd>=0) close(d->places->fd);
#endif
    free(d->places); d->places=NULL;
}
static void places_select(struct du_dialog *d)
{
    if (!d->places) return;
    struct du_dialog_places *p=d->places;
    size_t best=0; unsigned active=0;
    for (unsigned i=0;i<p->count;i++) {
        const char *path=p->entries[i].path; size_t n=strlen(path);
        p->rows[i].selected=0;
        if (n>best && !strncmp(d->folder,path,n) && (n==1 || !d->folder[n] || d->folder[n]=='/')) { best=n; active=i; }
    }
    if (best) p->rows[active].selected=1;
    d->items[F_PLACES].active=active;
}
static void place_add(struct du_dialog_places *p, const char *label, const char *path, const char *icon)
{
    if (p->count==PLACE_MAX) return;
    unsigned i=p->count++;
    snprintf(p->entries[i].path,sizeof(p->entries[i].path),"%s",path);
    snprintf(p->entries[i].label,sizeof(p->entries[i].label),"%s",label);
    p->rows[i]=(struct du_item){0}; p->rows[i].text[0]=p->entries[i].label;
    p->rows[i].icon16=du_icon_argb(du_icon_name(icon),16);
}
void du_dialog_tick(struct du_ui *u)
{
    struct du_dialog *d=&u->dialog; struct du_dialog_places *p=d->places;
    if (!p || u->now<p->next_poll) return;
    p->next_poll=u->now+500000000ull;
    /* The allocation retains a second list for polling, off the native stack.
     * Only the first list owns the observer and the polling deadline. */
    struct du_dialog_places *published=p;
    p=published+1; p->count=0;
    place_add(p,"Desktop","/Documents/Desktop","place:desktop");
    place_add(p,"Documents","/Documents","place:documents");
    place_add(p,"Music","/Documents/Music","audio-x-generic");
    place_add(p,"Pictures","/Documents/Pictures","image-x-generic");
    place_add(p,"CiukiOS (C:)","/","drive-harddisk");
#if defined(__i386__) || defined(DU_NATIVE_MEDIA)
    if (published->fd<0) published->fd=open("/dev/media",O_RDONLY|O_CLOEXEC);
    struct ciuki_media_state state={.version=CIUKI_MEDIA_VERSION,.size=sizeof(state)};
    if (published->fd>=0) {
        if (ciuki_devctl(published->fd,MEDIA_GET_STATE,&state,sizeof(state))) { close(published->fd); published->fd=-1; }
        else for (unsigned drive=3;drive<26;drive++) {
            if (!(state.removable_mask&(1u<<drive)) ||
                (state.type[drive]!=CIUKI_MEDIA_USB && state.type[drive]!=CIUKI_MEDIA_OPTICAL)) continue;
            struct ciuki_media_volume v={.version=CIUKI_MEDIA_VERSION,.size=sizeof(v),.drive=drive};
            if (ciuki_devctl(published->fd,MEDIA_GET_VOLUME,&v,sizeof(v)) || !v.generation || !v.flags) continue;
            v.label[sizeof(v.label)-1]=0;
            for (unsigned k=0;v.label[k];k++) if ((unsigned char)v.label[k]<32 || (unsigned char)v.label[k]==127) v.label[k]='?';
            char label[64],path[16];
            snprintf(label,sizeof(label),"%s (%c:)",v.label[0] ? v.label : state.type[drive]==CIUKI_MEDIA_USB ? "Removable Volume" : "CD-ROM",'A'+drive);
            snprintf(path,sizeof(path),"/mnt/%c",'a'+drive);
            place_add(p,label,path,state.type[drive]==CIUKI_MEDIA_OPTICAL ? "drive-optical" : "drive-removable-media");
        }
        /* Mount generations returned by GET_VOLUME differ from the transport
         * generations in GET_STATE. Recheck the latter across enumeration. */
        if (published->fd>=0) {
            struct ciuki_media_state after={.version=CIUKI_MEDIA_VERSION,.size=sizeof(after)};
            if (ciuki_devctl(published->fd,MEDIA_GET_STATE,&after,sizeof(after)) || memcmp(&state,&after,sizeof(state))) {
                p->count=5; published->next_poll=u->now+100000000ull;
            }
        }
    }
#endif
    int changed=p->count!=published->count;
    for (unsigned i=0;!changed && i<p->count;i++)
        changed=strcmp(p->entries[i].path,published->entries[i].path) ||
            strcmp(p->entries[i].label,published->entries[i].label) ||
            p->rows[i].icon16!=published->rows[i].icon16;
    if (!changed) return;
    memcpy(published->entries,p->entries,sizeof(p->entries));
    memcpy(published->rows,p->rows,sizeof(p->rows));
    published->count=p->count; p=published;
    /* Scratch row labels must point into the retained list after publication. */
    for (unsigned i=0;i<p->count;i++) p->rows[i].text[0]=p->entries[i].label;
    struct du_widget *w=&d->items[F_PLACES];
    w->items=p->rows; w->count=p->count;
    int max=(int)p->count-du_list_page(w); if (max<0) max=0;
    if (w->scroll>max) w->scroll=max;
    places_select(d); du_invalidate(u,w->box);
}
uint64_t du_dialog_deadline(const struct du_ui *u)
{ return u->dialog.places ? u->dialog.places->next_poll : UINT64_MAX; }

static int line_width(const char *s, size_t n)
{
    const char *tab = memchr(s, '\t', n);
    char part[512];
    size_t k = tab ? (size_t)(tab - s) : n;
    if (k >= sizeof(part)) k = sizeof(part) - 1;
    memcpy(part, s, k); part[k] = 0;
    int w = du_text_width(part);
    if (tab) {
        size_t r = n - k - 1; if (r >= sizeof(part)) r = sizeof(part) - 1;
        memcpy(part, tab + 1, r); part[r] = 0;
        w = (w > TAB_X - 8 ? w + 8 : TAB_X) + du_text_width(part);
    }
    return w;
}

static void measure(const char *text, int *width, int *lines)
{
    *width = 0; *lines = 0;
    for (const char *s = text; *s || !*lines;) {
        const char *end = strchr(s, '\n'); size_t n = end ? (size_t)(end - s) : strlen(s);
        int w = strncmp(s, "---", 3) ? line_width(s, n) : 0;
        if (w > *width) *width = w;
        (*lines)++;
        if (!end) break;
        s = end + 1;
    }
}

static void draw_text(struct du_canvas *c, int x, int y, int width, const char *text)
{
    for (const char *s = text; *s;) {
        const char *end = strchr(s, '\n'); size_t n = end ? (size_t)(end - s) : strlen(s);
        char line[512]; if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, s, n); line[n] = 0;
        if (!strncmp(line, "---", 3)) { du_hline(c, x, y + 3, width, DU_SHADOW); du_hline(c, x, y + 4, width, DU_HILITE); }
        else {
            char *tab = strchr(line, '\t');
            if (tab) {
                *tab = 0; du_text(c, x, y, line, DU_INK);
                int lw = du_text_width(line);
                du_text_fit(c, x + (lw > TAB_X - 8 ? lw + 8 : TAB_X), y, width - TAB_X, tab + 1, DU_INK);
            } else du_text_fit(c, x, y, width, line, DU_INK);
        }
        y += LINE_H;
        if (!end) break;
        s = end + 1;
    }
}

static struct du_widget *item(struct du_dialog *d, unsigned i, enum du_kind kind, int id, const char *text)
{
    struct du_widget *w = &d->items[i];
    memset(w, 0, sizeof(*w));
    w->kind = kind; w->id = id; w->vertical = 1; w->sort = -1; w->focus_rect = 1;
    du_set_text(w, text);
    if (i >= d->count) d->count = i + 1;
    return w;
}

static void begin(struct du_ui *u, enum du_dialog_kind kind, int id, const char *title, const char *text)
{
    struct du_dialog *d = &u->dialog;
    places_close(d);
    free(d->entries); free(d->rows);
    memset(d, 0, sizeof(*d));
    d->kind = kind; d->id = id; d->capture = -1;
    snprintf(d->title, sizeof(d->title), "%s", title ? title : "");
    snprintf(d->text, sizeof(d->text), "%s", text ? text : "");
    du_menu_close(u);
    du_release(u);
    u->dirty = 1;
}

static void place(struct du_ui *u, int w, int h)
{
    struct du_dialog *d = &u->dialog;
    if (w > u->canvas.width - 8) w = u->canvas.width - 8;
    if (h > u->canvas.height - 8) h = u->canvas.height - 8;
    d->box = (struct du_rect){(u->canvas.width - w) / 2, (u->canvas.height - h) / 2, w, h};
}

static void button_row(struct du_dialog *d, unsigned count, int y)
{
    int width=BTN_W;
    for (unsigned i=0;i<count;i++) {
        int label=du_label_width(d->items[i].text)+20;
        if (width<label) width=label;
    }
    int available=(d->box.w-32-((int)count-1)*6)/(int)count;
    if (width>available) width=available;
    int total=(int)count*width+((int)count-1)*6, x=d->box.x+d->box.w-16-total;
    for (unsigned i=0;i<count;i++) d->items[i].box=(struct du_rect){x+(int)i*(width+6),y,width,BTN_H};
}

static int body_y(const struct du_dialog *d) { return d->box.y + 3 + TITLE_H; }

void du_message(struct du_ui *u, int id, const char *title, const char *text, int icon,
                const char *const *buttons, unsigned count, int def, int cancel)
{
    static const char *const ok[] = {"&OK"};
    if (!buttons || !count) { buttons = ok; count = 1; }
    if (count > 4) count = 4;
    begin(u, DU_DLG_MESSAGE, id, title, text);
    struct du_dialog *d = &u->dialog;
    d->icon = icon; d->buttons = count;
    if (u->sound) u->sound(u->sound_ctx,du_message_sound(icon));
    d->cancel = cancel >= 0 && (unsigned)cancel < count ? cancel : (int)count - 1;
    d->focus = def >= 0 && (unsigned)def < count ? def : 0;
    for (unsigned i = 0; i < count; i++) {
        /* Existing callers with plain English labels get mnemonics too. */
        char label[DU_TEXT_MAX];
        if (!strchr(buttons[i],'&')) snprintf(label,sizeof(label),"&%s",buttons[i]);
        else snprintf(label,sizeof(label),"%s",buttons[i]);
        item(d, i, DU_BUTTON, ID_BUTTON - (int)i, label);
    }
    int w,h; du_message_size(d->text,icon,count,u->canvas.width-8,&w,&h);
    place(u, w, DESK_FRAME_TITLE + h + 3);
    button_row(d, count, d->box.y + d->box.h - 3 - 10 - BTN_H);
}

void du_input_box(struct du_ui *u, int id, const char *title, const char *prompt, const char *initial)
{
    begin(u, DU_DLG_INPUT, id, title, prompt);
    if (u->sound) u->sound(u->sound_ctx,SOUND_QUESTION);
    struct du_dialog *d = &u->dialog;
    d->buttons = 2; d->cancel = 1; d->focus = 2;
    item(d, 0, DU_BUTTON, ID_BUTTON, "OK"); item(d, 1, DU_BUTTON, ID_BUTTON - 1, "Cancel");
    struct du_widget *e = item(d, 2, DU_EDIT, ID_EDIT, initial);
    e->anchor = 0;
    int tw, lines; measure(d->text, &tw, &lines);
    int w = tw + 30; if (w < 340) w = 340;
    place(u, w, 3 + TITLE_H + 12 + lines * LINE_H + 6 + 22 + 16 + BTN_H + 10 + 3);
    e->box = (struct du_rect){d->box.x + 14, body_y(d) + 12 + lines * LINE_H + 6, d->box.w - 28, 22};
    button_row(d, 2, d->box.y + d->box.h - 3 - 10 - BTN_H);
}

void du_progress(struct du_ui *u, int id, const char *title, const char *text, unsigned value, unsigned total)
{
    struct du_dialog *d = &u->dialog;
    int fresh = d->kind != DU_DLG_PROGRESS || d->id != id;
    int changed = fresh || !total || d->progress != value || d->progress_total != total ||
        (title && strncmp(d->title,title,sizeof(d->title)-1)) ||
        (text && strncmp(d->text,text,sizeof(d->text)-1));
    if (fresh) {
        begin(u, DU_DLG_PROGRESS, id, title, text);
        d->buttons = 1; d->cancel = 0; d->focus = 0;
        item(d, 0, DU_BUTTON, ID_BUTTON, "Cancel");
        item(d, 1, DU_PROGRESS, ID_BAR, "");
        place(u, 360, 3 + TITLE_H + 12 + 3 * LINE_H + 8 + 16 + 14 + BTN_H + 10 + 3);
        d->items[1].box = (struct du_rect){d->box.x + 14, body_y(d) + 12 + 3 * LINE_H + 8, d->box.w - 28, 16};
        d->items[0].box = (struct du_rect){d->box.x + d->box.w - 3 - 14 - BTN_W, d->box.y + d->box.h - 3 - 10 - BTN_H, BTN_W, BTN_H};
    }
    if (title) snprintf(d->title, sizeof(d->title), "%s", title);
    if (text) snprintf(d->text, sizeof(d->text), "%s", text);
    int tw, lines; measure(d->text, &tw, &lines);
    if (lines < 3) lines = 3;
    int height = 3 + TITLE_H + 12 + lines * LINE_H + 8 + 16 + 14 + BTN_H + 10 + 3;
    if (height > u->canvas.height - 8) height = u->canvas.height - 8;
    if (fresh || d->box.h != height) {
        place(u, lines > 3 ? 460 : 360, height);
        d->items[1].box = (struct du_rect){d->box.x + 14, d->box.y + d->box.h - 3 - 10 - BTN_H - 14 - 16, d->box.w - 28, 16};
        d->items[0].box = (struct du_rect){d->box.x + d->box.w - 3 - 14 - BTN_W, d->box.y + d->box.h - 3 - 10 - BTN_H, BTN_W, BTN_H};
        u->dirty = 1;
    }
    d->progress = value; d->progress_total = total;
    d->items[1].value = (int)(value > 0x7fffffffu ? 0x7fffffffu : value);
    d->items[1].total = (int)(total > 0x7fffffffu ? 0x7fffffffu : total);
    /* Keep full invalidation and merge any pending bounded repaint. */
    if (changed) {
        if (!u->dirty) u->dirty = 2;
        else if (u->dirty==3) du_invalidate(u,d->box);
    }
}

/* ---- file dialogs ---- */
static unsigned fold(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int name_cmp(const struct du_file_entry *a, const struct du_file_entry *b)
{
    if (a->directory != b->directory) return a->directory ? -1 : 1;
    const char *x = a->name, *y = b->name;
    while (*x && fold((unsigned char)*x) == fold((unsigned char)*y)) { x++; y++; }
    int r = (int)fold((unsigned char)*x) - (int)fold((unsigned char)*y);
    return r ? r : strcmp(a->name, b->name);
}

int du_posix_lister(void *ctx, const char *path, struct du_file_entry *out, unsigned capacity, unsigned *count)
{
    (void)ctx; *count = 0;
    DIR *dir = opendir(path);
    if (!dir) return -errno;
    struct dirent *e;
    while ((e = readdir(dir)) && *count < capacity) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) >= sizeof(out->name)) continue;
        char full[DU_TEXT_MAX]; struct stat st;
        if (snprintf(full, sizeof(full), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", e->d_name) >= (int)sizeof(full)) continue;
        if (stat(full, &st)) continue;
        struct du_file_entry *f = &out[(*count)++];
        snprintf(f->name, sizeof(f->name), "%s", e->d_name);
        f->directory = S_ISDIR(st.st_mode); f->size = (uint64_t)st.st_size;
    }
    closedir(dir);
    return 0;
}

static int navigate(struct du_ui *u, const char *path)
{
    struct du_dialog *d = &u->dialog;
    unsigned count = 0;
    int r = d->lister(d->lister_ctx, path, d->entries, DU_FILE_ENTRIES, &count);
    if (r) return r;
    /* Insertion sort: deterministic order independent of enumeration. */
    for (unsigned i = 1; i < count; i++) {
        struct du_file_entry key = d->entries[i]; unsigned j = i;
        while (j > 0 && name_cmp(&d->entries[j - 1], &key) > 0) { d->entries[j] = d->entries[j - 1]; j--; }
        d->entries[j] = key;
    }
    d->entry_count = count;
    for (unsigned i = 0; i < count; i++) {
        memset(&d->rows[i], 0, sizeof(d->rows[i]));
        d->rows[i].text[0] = d->entries[i].name;
        d->rows[i].icon16 = du_icon_argb(du_icon_file(d->entries[i].name, d->entries[i].directory, 0), 16);
    }
    snprintf(d->folder, sizeof(d->folder), "%s", path);
    struct du_widget *list = &d->items[F_LIST];
    list->items = d->rows; list->count = count; list->active = 0; list->scroll = list->hscroll = 0;
    du_set_text(&d->items[F_FOLDER], d->folder);
    places_select(d);
    u->dirty = 1;
    return 0;
}

static void join(char *out, const char *folder, const char *name)
{
    if (name[0] == '/') snprintf(out, DU_TEXT_MAX, "%s", name);
    else snprintf(out, DU_TEXT_MAX, "%s%s%s", folder, folder[0] && folder[strlen(folder) - 1] == '/' ? "" : "/", name);
}

int du_file_dialog(struct du_ui *u, int id, int save, const char *title, const char *folder,
                   const char *name, du_lister lister, void *ctx)
{
    begin(u, save ? DU_DLG_SAVE : DU_DLG_OPEN, id, title ? title : save ? "Save As" : "Open", "");
    struct du_dialog *d = &u->dialog;
    d->lister = lister ? lister : du_posix_lister; d->lister_ctx = ctx;
    d->entries = calloc(DU_FILE_ENTRIES, sizeof(*d->entries));
    d->rows = calloc(DU_FILE_ENTRIES, sizeof(*d->rows));
    d->places=calloc(2,sizeof(*d->places)); /* published list + poll scratch */
    if (d->places) d->places->fd=-1;
    if (!d->entries || !d->rows || !d->places) { places_close(d); free(d->entries); free(d->rows); memset(d, 0, sizeof(*d)); return -ENOMEM; }
    d->buttons = 2; d->cancel = 1; d->focus = F_NAME;
    place(u, 540, 360);
    struct du_rect b = d->box; int y = body_y(d) + 10, right = b.x + b.w - 14;
    item(d, F_ACCEPT, DU_BUTTON, ID_BUTTON, save ? "&Save" : "&Open")->box = (struct du_rect){right - BTN_W, b.y + b.h - 3 - 10 - 2 * BTN_H - 6, BTN_W, BTN_H};
    item(d, F_CANCEL, DU_BUTTON, ID_BUTTON - 1, "Cancel")->box = (struct du_rect){right - BTN_W, b.y + b.h - 3 - 10 - BTN_H, BTN_W, BTN_H};
    item(d, F_UP, DU_BUTTON, ID_UP, "Up")->box = (struct du_rect){right - 40, y, 40, 22};
    item(d, F_FOLDER, DU_EDIT, ID_FOLDER, folder)->box = (struct du_rect){b.x + 80, y, right - 46 - b.x - 80, 22};
    d->items[F_FOLDER].icon = du_icon_argb(du_icon_name("type:folder"), 16);
    struct du_widget *list = item(d, F_LIST, DU_LISTVIEW, ID_LIST, "");
    list->view = DU_LIST; list->inactive_selection = 1;
    int pane=b.w>=460 ? 150 : 120;
    list->box = (struct du_rect){b.x + 14 + pane + 8, y + 30, b.w - 36 - pane, d->items[F_ACCEPT].box.y - 10 - (y + 30)};
    struct du_widget *places=item(d,F_PLACES,DU_TREE,ID_PLACES,"");
    places->inactive_selection=1;
    places->box=(struct du_rect){b.x+14,y+48,pane,list->box.h-18};
    item(d,F_PLACES_TITLE,DU_HEADER,0,"Places")->box=(struct du_rect){b.x+14,y+30,pane,18};
    item(d, F_NAME, DU_EDIT, ID_NAME, name)->box = (struct du_rect){b.x + 80, d->items[F_ACCEPT].box.y, right - BTN_W - 10 - b.x - 80, 22};
    d->items[F_NAME].anchor = 0;
    item(d, F_LOOK, DU_LABEL, 0, "Look &in:")->box = (struct du_rect){b.x + 14, y, 64, 22};
    item(d, F_FILE, DU_LABEL, 0, "File &name:")->box = (struct du_rect){b.x + 14, d->items[F_ACCEPT].box.y, 64, 22};
    int r = navigate(u, folder && folder[0] ? folder : "/");
    if (r && navigate(u, "/")) { du_dialog_close(u); return r; }
    du_dialog_tick(u);
    return 0;
}

void du_dialog_icon(struct du_ui *u, const uint32_t *argb32)
{ if (u->dialog.kind == DU_DLG_MESSAGE && u->dialog.icon) { u->dialog.icon_pixels = argb32; u->dirty = 1; } }

int du_dialog_id(const struct du_ui *u) { return u->dialog.kind ? u->dialog.id : 0; }
struct du_rect du_dialog_button(const struct du_ui *u, unsigned i)
{ return i < u->dialog.count ? u->dialog.items[i].box : (struct du_rect){0, 0, 0, 0}; }

void du_dialog_close(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    places_close(d);
    free(d->entries); free(d->rows);
    memset(d, 0, sizeof(*d)); d->capture = -1;
    u->dirty = 1;
}

static void finish(struct du_ui *u, int index)
{
    struct du_dialog *d = &u->dialog;
    if (d->accept && d->accept(u, index, d->accept_ctx)) { u->dirty = 1; return; }
    char text[DU_TEXT_MAX] = "";
    if (d->kind == DU_DLG_INPUT) snprintf(text, sizeof(text), "%s", d->items[2].text);
    if ((d->kind == DU_DLG_OPEN || d->kind == DU_DLG_SAVE) && index == 0) join(text, d->folder, d->items[F_NAME].text);
    int id = d->id;
    du_dialog_close(u);
    du_emit(u, (struct du_event){.type = DU_EV_DIALOG, .id = id, .index = index, .text = text});
}

static void parent_folder(struct du_ui *u)
{
    char path[DU_TEXT_MAX]; snprintf(path, sizeof(path), "%s", u->dialog.folder);
    char *slash = strrchr(path, '/');
    if (!slash) return;
    if (slash == path) path[1] = 0; else *slash = 0;
    navigate(u, path);
}

static void accept_file(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    const char *name = d->items[F_NAME].text;
    if (!name[0]) return;
    for (unsigned i = 0; i < d->entry_count; i++)
        if (d->entries[i].directory && !strcmp(d->entries[i].name, name)) {
            char path[DU_TEXT_MAX]; join(path, d->folder, name);
            if (!navigate(u, path)) du_set_text(&d->items[F_NAME], "");
            return;
        }
    if (name[0] == '/') {
        unsigned count = 0;
        /* An absolute folder path navigates; anything else is a file name. */
        if (!d->lister(d->lister_ctx, name, d->entries, DU_FILE_ENTRIES, &count)) { navigate(u, name); du_set_text(&d->items[F_NAME], ""); return; }
        navigate(u, d->folder);
    }
    finish(u, 0);
}

static void dialog_event(struct du_ui *u, const struct du_event *e, void *arg)
{
    (void)u;
    struct du_dialog *d = arg;
    if (d->pending_count < 4) d->pending[d->pending_count++] = *e;
}

static void process(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    struct du_event pending[4]; unsigned n = d->pending_count;
    memcpy(pending, d->pending, sizeof(pending)); d->pending_count = 0;
    for (unsigned k = 0; k < n && d->kind; k++) {
        struct du_event *e = &pending[k];
        int file = d->kind == DU_DLG_OPEN || d->kind == DU_DLG_SAVE;
        if (e->type == DU_EV_ACTIVATE && e->id <= ID_BUTTON && e->id > ID_BUTTON - 4) {
            int index = ID_BUTTON - e->id;
            if (file && index == 0) accept_file(u); else finish(u, index);
        } else if (e->type == DU_EV_ACTIVATE && e->id == ID_UP) parent_folder(u);
        else if ((e->type==DU_EV_SELECT || e->type==DU_EV_OPEN) && e->id==ID_PLACES && e->index>=0 && d->places && (unsigned)e->index<d->places->count) {
            if (!navigate(u,d->places->entries[e->index].path)) du_set_text(&d->items[F_NAME],"");
            places_select(d);
        }
        else if (e->type == DU_EV_SELECT && e->id == ID_LIST && e->index >= 0 && (unsigned)e->index < d->entry_count) {
            for (unsigned i = 0; i < d->entry_count; i++) d->rows[i].selected = i == (unsigned)e->index;
            if (!d->entries[e->index].directory) du_set_text(&d->items[F_NAME], d->entries[e->index].name);
        } else if (e->type == DU_EV_OPEN && e->id == ID_LIST && e->index >= 0 && (unsigned)e->index < d->entry_count) {
            if (d->entries[e->index].directory) {
                char path[DU_TEXT_MAX]; join(path, d->folder, d->entries[e->index].name); navigate(u, path);
            } else { du_set_text(&d->items[F_NAME], d->entries[e->index].name); accept_file(u); }
        } else if (e->type == DU_EV_COMMIT && e->id == ID_NAME) accept_file(u);
        else if (e->type == DU_EV_COMMIT && e->id == ID_FOLDER) { if (navigate(u, d->items[F_FOLDER].text)) du_set_text(&d->items[F_FOLDER], d->folder); }
        else if (e->type == DU_EV_COMMIT && e->id == ID_EDIT) finish(u, 0);
        else if (e->type == DU_EV_COMMIT && d->accept) finish(u, 0);
        else if (e->type == DU_EV_CANCEL) finish(u, d->cancel);
    }
}

#define DIALOG_CALL(stmt) do { \
        void (*saved)(struct du_ui *, const struct du_event *, void *) = u->event; void *user = u->user; \
        u->event = dialog_event; u->user = &u->dialog; u->dialog.pending_count = 0; \
        stmt; \
        u->event = saved; u->user = user; process(u); \
    } while (0)

static int focusable(const struct du_widget *w) { return du_focusable(w) || (w->kind == DU_BUTTON && !w->disabled); }

void du_dialog_wheel(struct du_ui *u, int detents)
{
    struct du_dialog *d=&u->dialog;
    if (d->dragging || d->capture>=0 || !du_contains(d->box,u->mouse_x,u->mouse_y)) return;
    for (unsigned i=d->count;i>0;i--) {
        struct du_widget *w=&d->items[i-1];
        if (w->hidden || !du_contains(w->box,u->mouse_x,u->mouse_y)) continue;
        DIALOG_CALL(du_widget_wheel(u,w,detents));
        return;
    }
}

void du_dialog_key(struct du_ui *u, unsigned code)
{
    struct du_dialog *d = &u->dialog;
    struct du_widget *f = d->focus >= 0 && (unsigned)d->focus < d->count ? &d->items[d->focus] : NULL;
    if (code == DU_KEY_ESC) { finish(u, d->cancel); return; }
    if (code == DU_KEY_TAB) {
        for (unsigned n = 0; n < d->count; n++) {
            d->focus = u->modifiers & DU_SHIFT ? (d->focus <= 0 ? (int)d->count - 1 : d->focus - 1) : (d->focus + 1) % (int)d->count;
            if (focusable(&d->items[d->focus])) break;
        }
        u->dirty = 1; return;
    }
    if (f && (f->kind == DU_EDIT || f->kind == DU_TEXTAREA || f->kind == DU_LISTVIEW || f->kind == DU_TREE)) {
        int handled = 0; DIALOG_CALL(handled = du_widget_key(u, f, code)); if (handled) return;
    }
    if (f && (f->kind == DU_CHECKBOX || f->kind == DU_RADIO)) {
        int handled = 0; DIALOG_CALL(handled = du_widget_key(u, f, code)); if (handled) return;
    }
    if (code == DU_KEY_ENTER || (code == DU_KEY_SPACE && f && f->kind == DU_BUTTON)) {
        int index = f && f->kind == DU_BUTTON ? (f->id == ID_UP ? -1 : ID_BUTTON - f->id) : 0;
        if (f && f->id == ID_UP) { parent_folder(u); return; }
        if ((d->kind == DU_DLG_OPEN || d->kind == DU_DLG_SAVE) && index == 0) accept_file(u); else finish(u, index);
        return;
    }
    if ((code == DU_KEY_LEFT || code == DU_KEY_RIGHT || code == DU_KEY_UP || code == DU_KEY_DOWN) && f && f->kind == DU_BUTTON && (unsigned)d->focus < d->buttons) {
        int step = code == DU_KEY_LEFT || code == DU_KEY_UP ? -1 : 1;
        d->focus = (d->focus + step + (int)d->buttons) % (int)d->buttons; u->dirty = 1; return;
    }
    unsigned saved = u->modifiers; u->modifiers &= ~DU_ALT;
    unsigned ch = du_key_char(u, code); u->modifiers = saved;
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    if (ch && (!f || f->kind != DU_EDIT || (u->modifiers & DU_ALT))) {
        for (unsigned i = 0; i < d->count; i++) {
            const char *amp = strchr(d->items[i].text, '&');
            unsigned m = amp && amp[1] ? (unsigned char)amp[1] : 0;
            if (m >= 'A' && m <= 'Z') m += 32;
            if (m != ch) continue;
            if (d->items[i].kind == DU_BUTTON && i < d->buttons) {
                if ((d->kind == DU_DLG_OPEN || d->kind == DU_DLG_SAVE) && i == 0) accept_file(u); else finish(u, (int)i);
            } else if (d->items[i].kind == DU_LABEL && i + 1 <= d->count) d->focus = i == F_LOOK ? F_FOLDER : F_NAME;
            u->dirty = 1; return;
        }
    }
}

/* The close button's hit box: the same place and size as on every window (theme.c). */
static struct du_rect close_box(const struct du_dialog *d)
{
    int b[4];
    desk_title_button_box(d->box.x, d->box.y, d->box.w, DESK_TB_CLOSE, b);
    return (struct du_rect){b[0], b[1], b[2], b[3]};
}

void du_dialog_press(struct du_ui *u, int button)
{
    struct du_dialog *d = &u->dialog;
    int x = u->mouse_x, y = u->mouse_y;
    if (!du_contains(d->box, x, y)) return;
    if (button == 1 && du_contains(close_box(d), x, y)) { finish(u, d->cancel); return; }
    if (button == 1 && y < body_y(d)) { d->dragging = 1; d->drag_x = x - d->box.x; d->drag_y = y - d->box.y; return; }
    for (unsigned i = d->count; i > 0; i--) {
        struct du_widget *w = &d->items[i - 1];
        if (w->hidden || !du_contains(w->box, x, y)) continue;
        if (focusable(w)) d->focus = (int)i - 1;
        if (button == 1) d->capture = (int)i - 1;
        DIALOG_CALL(du_widget_press(u, w, button, x, y));
        u->dirty = 1;
        return;
    }
}

void du_dialog_motion(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    int hot = !d->dragging && du_contains(close_box(d), u->mouse_x, u->mouse_y);
    if (hot != d->close_hot) { d->close_hot = hot; u->dirty = 1; }
    if (d->dragging) {
        int nx = u->mouse_x - d->drag_x, ny = u->mouse_y - d->drag_y;
        if (nx < -d->box.w + 40) nx = -d->box.w + 40;
        if (nx > u->canvas.width - 40) nx = u->canvas.width - 40;
        if (ny < 0) ny = 0;
        if (ny > u->canvas.height - 20) ny = u->canvas.height - 20;
        int dx = nx - d->box.x, dy = ny - d->box.y;
        d->box.x += dx; d->box.y += dy;
        for (unsigned i = 0; i < d->count; i++) { d->items[i].box.x += dx; d->items[i].box.y += dy; }
        u->dirty = 1; return;
    }
    if (d->capture >= 0 && (unsigned)d->capture < d->count) {
        struct du_widget *w = &d->items[d->capture];
        DIALOG_CALL(du_widget_motion(u, w, u->mouse_x, u->mouse_y));
    }
}

void du_dialog_release(struct du_ui *u, int button)
{
    struct du_dialog *d = &u->dialog;
    if (button == 1 && d->dragging) { d->dragging = 0; return; }
    if (d->capture >= 0 && (unsigned)d->capture < d->count) {
        struct du_widget *w = &d->items[d->capture];
        if (button == 1) d->capture = -1;
        DIALOG_CALL(du_widget_release(u, w, button, u->mouse_x, u->mouse_y));
    }
}

void du_draw_dialog(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    if (!d->kind) return;
    struct du_canvas *c = &u->canvas;
    struct du_rect b = d->box;
    const struct desk_theme *t = desk_theme;
    if (t->shadow_size) du_shadow(c, b, t->shadow_size);
    /* The frame of every window (theme.c desk_paint_frame): same title bar,
     * height, close button, outline and corners; a modal dialog has only
     * the close button (it cannot be minimised or rolled up on its own). */
    struct du_rect v = du_intersection(c->clip, (struct du_rect){0, 0, c->width, c->height});
    struct desk_paint p = {c->pixels, c->width, c->height, v.x, v.y, v.w, v.h};
    struct desk_frame_style style = {.title = d->title, .active = u->focused_window, .buttons = DESK_TB_CLOSE,
                                     .hot = d->close_hot && du_contains(close_box(d), u->mouse_x, u->mouse_y) ? DESK_TB_CLOSE : 0};
    desk_paint_frame(&p, b.x, b.y, b.w, b.h, &style);
    int top = body_y(d);
    if (d->kind == DU_DLG_MESSAGE) {
        int ix = b.x + 16, tx = ix + (d->icon ? 46 : 0);
        int bottom = d->items[0].box.y - 18;
        struct du_rect body = {tx, top + 16, b.x + b.w - 16 - tx, bottom - top - 16};
        int lines = du_text_wrap(NULL,body,d->text,0);
        int th = lines * (desk_font_line(t->font_ui)+1);
        if (d->icon && th<32) { body.y+=(32-th)/2; body.h-=(32-th)/2; }
        if (d->icon_pixels) du_icon_blit(c, ix, top + 16, d->icon_pixels, 32, DU_ICON_NORMAL);
        else if (du_message_icon_name(d->icon)) du_icon_draw(c, ix, top + 16, du_icon_for(du_message_icon_name(d->icon), 32), DU_ICON_NORMAL);
        du_text_wrap(c,body,d->text,DU_INK);
    } else if (d->kind == DU_DLG_INPUT || d->kind == DU_DLG_PROGRESS) {
        draw_text(c, b.x + 14, top + 12, b.w - 28, d->text);
    }
    int focus_button = d->focus >= 0 && (unsigned)d->focus < d->buttons;
    for (unsigned i = 0; i < d->count; i++) {
        struct du_widget *w = &d->items[i];
        if (w->hidden) continue;
        if (w->kind == DU_BUTTON && i < d->buttons) w->def = focus_button ? (int)i == d->focus : i == 0;
        struct du_rect old = c->clip;
        c->clip = du_intersection(old, w->box);
        du_draw_widget(u, w, d->focus == (int)i && u->focused_window);
        c->clip = old;
    }
}
