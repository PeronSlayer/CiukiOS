/* SPDX-License-Identifier: MIT
 * Modal dialogs drawn inside the client window: message box, input box,
 * file open/save and progress with Cancel. */
#include "internal.h"
#include "../desktop/profile.h"
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

enum { ID_BUTTON = -1000, ID_UP = -2000, ID_LIST = -3000, ID_NAME = -4000, ID_FOLDER = -5000, ID_EDIT = -6000, ID_BAR = -7000, ID_PLACES = -8000,
       ID_NEWDIR = -9000, ID_VLIST = -9100, ID_VDETAILS = -9200, ID_TYPE = -9300, ID_SUB_YES = -9400, ID_SUB_NO = -9401, ID_SUB_EDIT = -9402 };
/* File dialog item slots. F_SUB_* form the overlay panel (confirmation, message, new folder name). */
enum { F_ACCEPT, F_CANCEL, F_UP, F_FOLDER, F_LIST, F_NAME, F_LOOK, F_FILE, F_PLACES, F_PLACES_TITLE,
       F_NEWDIR, F_VLIST, F_VDETAILS, F_TYPE, F_TYPE_LABEL, F_SUB_YES, F_SUB_NO, F_SUB_EDIT };
enum { SUB_NONE, SUB_CONFIRM, SUB_MESSAGE, SUB_NEWDIR };
#define PLACE_MAX 31
struct du_dialog_places {
    struct du_item rows[PLACE_MAX];
    struct { char path[128], label[64]; } entries[PLACE_MAX];
    unsigned count;
    uint64_t next_poll;
    int fd;
};
static void dialog_damage(struct du_ui *u)
{
    struct du_rect box=u->dialog.box;
    if (box.w<=0 || box.h<=0) return;
    int s=desk_theme->shadow_size;
    box.x+=s/3-s; box.y+=s/2-s; box.w+=2*s; box.h+=2*s;
    du_invalidate(u,box);
}

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
    char personal[128];
    profile_child(personal,sizeof(personal),"Desktop"); place_add(p,"Desktop",personal,"place:desktop");
    place_add(p,"Documents",profile_home(),"place:documents");
    profile_child(personal,sizeof(personal),"Music"); place_add(p,"Music",personal,"audio-x-generic");
    profile_child(personal,sizeof(personal),"Pictures"); place_add(p,"Pictures",personal,"image-x-generic");
    place_add(p,"CiukiOS (C:)","/","drive-harddisk");
#if defined(__i386__) || defined(DU_NATIVE_MEDIA)
    if (published->fd<0) published->fd=open("/dev/media",O_RDONLY|O_CLOEXEC);
    struct ciuki_media_state state={.version=CIUKI_MEDIA_VERSION,.size=sizeof(state)};
    if (published->fd>=0) {
        if (ciuki_devctl(published->fd,MEDIA_GET_STATE,&state,sizeof(state))) { close(published->fd); published->fd=-1; }
        else {
        /* fx-17b: floppy drives are Places whether or not a disk is in. */
        for (unsigned drive=0;drive<2;drive++) if (state.type[drive]==CIUKI_MEDIA_FLOPPY) {
            char label[32],path[16];
            snprintf(label,sizeof(label),"3.5 Floppy (%c:)",'A'+drive); snprintf(path,sizeof(path),"/mnt/%c",'a'+drive);
            place_add(p,label,path,"media-floppy");
        }
        for (unsigned drive=3;drive<26;drive++) {
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

/* Everything a dialog owns, released before it is replaced or closed. */
static void release(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    du_overlays_forget(u, d->items, DU_DIALOG_ITEMS);
    places_close(d);
    free(d->entries); free(d->rows);
    if (d->free_state) d->free_state(d->state);
}

static void begin(struct du_ui *u, enum du_dialog_kind kind, int id, const char *title, const char *text)
{
    struct du_dialog *d = &u->dialog;
    if (d->kind) dialog_damage(u);
    release(u);
    memset(d, 0, sizeof(*d));
    d->kind = kind; d->id = id; d->capture = -1;
    snprintf(d->title, sizeof(d->title), "%s", title ? title : "");
    snprintf(d->text, sizeof(d->text), "%s", text ? text : "");
    du_menu_close(u);
    du_release(u);
    dialog_damage(u);
}

static void place(struct du_ui *u, int w, int h)
{
    struct du_dialog *d = &u->dialog;
    if (w > u->canvas.width - 8) w = u->canvas.width - 8;
    if (h > u->canvas.height - 8) h = u->canvas.height - 8;
    dialog_damage(u);
    d->box = (struct du_rect){(u->canvas.width - w) / 2, (u->canvas.height - h) / 2, w, h};
    dialog_damage(u);
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

/* Shared with commdlg.c. */
struct du_widget *du_dialog_item(struct du_ui *u, unsigned i, enum du_kind kind, int id, const char *text)
{ return item(&u->dialog, i, kind, id, text); }
void du_dialog_begin(struct du_ui *u, enum du_dialog_kind kind, int id, const char *title, const char *text)
{ begin(u, kind, id, title, text); }
void du_dialog_place(struct du_ui *u, int w, int h) { place(u, w, h); }
void du_dialog_damage(struct du_ui *u) { dialog_damage(u); }
int du_dialog_body_y(const struct du_dialog *d) { return body_y(d); }

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
        dialog_damage(u);
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
/* Truncating copy: names longer than the field are cut, never refused. */
static void copy_str(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n); dst[n] = 0;
}
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

static int posix_mkdir(void *ctx, const char *path)
{ (void)ctx; return mkdir(path, 0777) ? -errno : 0; }

/* ---- wildcard patterns (OPENFILENAME filter strings) ---- */
static int wild(const char *p, const char *pe, const char *s)
{
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (p < pe && *p == '*') { star = p++; mark = s; }
        else if (p < pe && (*p == '?' || fold((unsigned char)*p) == fold((unsigned char)*s))) { p++; s++; }
        else if (star) { p = star + 1; s = ++mark; }
        else return 0;
    }
    while (p < pe && *p == '*') p++;
    return p == pe;
}

int du_pattern_match(const char *patterns, const char *name)
{
    if (!patterns || !patterns[0]) return 1;
    for (const char *p = patterns; *p;) {
        while (*p == ' ' || *p == ';') p++;
        const char *end = p; while (*end && *end != ';') end++;
        const char *trim = end; while (trim > p && trim[-1] == ' ') trim--;
        if (trim > p && (trim - p == 3 && !memcmp(p, "*.*", 3) ? 1 : wild(p, trim, name))) return 1;   /* *.* is every name, with or without a dot */
        p = end;
    }
    return 0;
}

static const char *base_name(const char *path)
{ const char *slash = strrchr(path, '/'); return slash ? slash + 1 : path; }
static int has_extension(const char *name) { return strrchr(base_name(name), '.') != NULL; }

#define FILTER_MAX 12
struct file_state {
    unsigned flags;
    char default_ext[4];
    struct { char label[72], pattern[96]; } filters[FILTER_MAX];
    unsigned filter_count;
    int filter;
    struct du_item type_rows[FILTER_MAX];
    char typed[96];                        /* a pattern typed in the name box: the filter until the folder or type changes */
    unsigned map[DU_FILE_ENTRIES], shown;  /* list row -> entry */
    char size_text[DU_FILE_ENTRIES][24], kind_text[DU_FILE_ENTRIES][16];
    du_mkdir_fn make_dir; void *make_dir_ctx;
    char pending[DU_TEXT_MAX];             /* the path accepted (or being confirmed) */
    char sub_text[320]; int sub_icon;
    int panel_dx, panel_dy, panel_w, panel_h;
    char title[96];
};
#define FILE_STATE(d) ((struct file_state *)(d)->state)
static int file_event(struct du_ui *, const struct du_event *);
static int file_key(struct du_ui *, unsigned);
static int file_press(struct du_ui *, int, int, int);
static void file_draw(struct du_ui *);

static void file_free(void *state) { free(state); }

static const char *active_pattern(const struct file_state *f)
{
    if (f->typed[0]) return f->typed;
    return f->filter_count && f->filter >= 0 ? f->filters[f->filter].pattern : "*.*";
}

static void file_columns(struct du_widget *list, int details)
{
    list->view = details ? DU_DETAILS : DU_LIST;
    list->column_count = 3;
    list->columns[0] = (struct du_column){"Name", list->box.w > 330 ? list->box.w - 200 : 130, 0};
    list->columns[1] = (struct du_column){"Size", 70, 1};
    list->columns[2] = (struct du_column){"Type", 90, 0};
}

static void refilter(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog; struct file_state *f = FILE_STATE(d);
    const char *pattern = active_pattern(f);
    unsigned n = 0;
    for (unsigned i = 0; i < d->entry_count; i++) {
        const struct du_file_entry *e = &d->entries[i];
        if (!e->directory && !du_pattern_match(pattern, e->name)) continue;
        f->map[n] = i;
        struct du_item *row = &d->rows[n];
        memset(row, 0, sizeof(*row));
        row->text[0] = e->name;
        if (e->directory) { snprintf(f->size_text[n], sizeof(f->size_text[n]), "%s", ""); snprintf(f->kind_text[n], sizeof(f->kind_text[n]), "Folder"); }
        else {
            uint64_t kb = (e->size + 1023) / 1024;
            snprintf(f->size_text[n], sizeof(f->size_text[n]), "%llu KB", (unsigned long long)(kb ? kb : (e->size ? 1 : 0)));
            const char *dot = strrchr(e->name, '.'), *ext = dot && dot[1] ? dot + 1 : NULL;
            if (ext && strlen(ext) <= 5) {
                char up[8]; size_t k = 0; for (; ext[k] && k < 5; k++) up[k] = ext[k] >= 'a' && ext[k] <= 'z' ? (char)(ext[k] - 32) : ext[k];
                up[k] = 0; snprintf(f->kind_text[n], sizeof(f->kind_text[n]), "%s File", up);
            } else snprintf(f->kind_text[n], sizeof(f->kind_text[n]), "File");
        }
        row->text[1] = f->size_text[n]; row->text[2] = f->kind_text[n];
        row->icon16 = du_icon_argb(du_icon_file(e->name, e->directory, 0), 16);
        n++;
    }
    f->shown = n;
    struct du_widget *list = &d->items[F_LIST];
    list->items = d->rows; list->count = n; list->active = 0; list->scroll = list->hscroll = 0;
    du_invalidate(u, list->box);
}

static int navigate(struct du_ui *u, const char *path)
{
    struct du_dialog *d = &u->dialog; struct file_state *f = FILE_STATE(d);
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
    if (strcmp(d->folder, path)) f->typed[0] = 0;
    snprintf(d->folder, sizeof(d->folder), "%s", path);
    refilter(u);
    du_set_text(&d->items[F_FOLDER], d->folder);
    places_select(d);
    dialog_damage(u);
    return 0;
}

static void join(char *out, const char *folder, const char *name)
{
    if (name[0] == '/') snprintf(out, DU_TEXT_MAX, "%s", name);
    else snprintf(out, DU_TEXT_MAX, "%s%s%s", folder, folder[0] && folder[strlen(folder) - 1] == '/' ? "" : "/", name);
}

int du_file_open(struct du_ui *u, int id, const struct du_file_options *o)
{
    int save = (o->flags & DU_FILE_SAVE) != 0;
    const char *title = o->title ? o->title : save ? "Save As" : "Open";
    begin(u, save ? DU_DLG_SAVE : DU_DLG_OPEN, id, title, "");
    struct du_dialog *d = &u->dialog;
    d->lister = o->lister ? o->lister : du_posix_lister; d->lister_ctx = o->lister_ctx;
    d->entries = calloc(DU_FILE_ENTRIES, sizeof(*d->entries));
    d->rows = calloc(DU_FILE_ENTRIES, sizeof(*d->rows));
    d->places = calloc(2, sizeof(*d->places)); /* published list + poll scratch */
    struct file_state *f = calloc(1, sizeof(*f));
    if (d->places) d->places->fd = -1;
    if (!d->entries || !d->rows || !d->places || !f) { free(f); release(u); memset(d, 0, sizeof(*d)); d->capture = -1; return -ENOMEM; }
    d->state = f; d->free_state = file_free;
    f->flags = o->flags; f->make_dir = o->make_dir ? o->make_dir : posix_mkdir; f->make_dir_ctx = o->make_dir ? o->make_dir_ctx : NULL;
    snprintf(f->title, sizeof(f->title), "%s", title);
    const char *ext = o->default_ext;
    while (ext && *ext == '.') ext++;
    for (size_t n = 0; ext && n < 3 && ext[n]; n++) f->default_ext[n] = ext[n];   /* only the first three characters, as OPENFILENAME */
    for (unsigned i = 0; i < o->filter_count && i < FILTER_MAX; i++) {
        snprintf(f->filters[i].label, sizeof(f->filters[i].label), "%s", o->filters[i].label ? o->filters[i].label : "");
        snprintf(f->filters[i].pattern, sizeof(f->filters[i].pattern), "%s", o->filters[i].pattern ? o->filters[i].pattern : "*.*");
        f->filter_count++;
    }
    if (!f->filter_count) {
        snprintf(f->filters[0].label, sizeof(f->filters[0].label), "All Files (*.*)"); snprintf(f->filters[0].pattern, sizeof(f->filters[0].pattern), "*.*");
        f->filter_count = 1;
    }
    f->filter = o->filter_index < f->filter_count ? (int)o->filter_index : 0;
    d->result = f->filter;
    for (unsigned i = 0; i < f->filter_count; i++) f->type_rows[i].text[0] = f->filters[i].label;
    d->buttons = 2; d->cancel = 1; d->focus = F_NAME;
    d->on_event = file_event; d->on_key = file_key; d->on_press = file_press; d->on_draw = file_draw; d->overlay = F_SUB_YES;
    place(u, 540, 392);
    struct du_rect b = d->box; int y = body_y(d) + 10, right = b.x + b.w - 14;
    int row2 = b.y + b.h - 3 - 10 - BTN_H, row1 = row2 - BTN_H - 6;
    item(d, F_ACCEPT, DU_BUTTON, ID_BUTTON, save ? "&Save" : "&Open")->box = (struct du_rect){right - BTN_W, row1, BTN_W, BTN_H};
    item(d, F_CANCEL, DU_BUTTON, ID_BUTTON - 1, "Cancel")->box = (struct du_rect){right - BTN_W, row2, BTN_W, BTN_H};
    /* Toolbar row: Look in, folder, Up, New Folder, List, Details. */
    int x = right;
    x -= 56; item(d, F_VDETAILS, DU_BUTTON, ID_VDETAILS, "D&etails")->box = (struct du_rect){x, y, 56, 22}; x -= 4;
    x -= 40; item(d, F_VLIST, DU_BUTTON, ID_VLIST, "&List")->box = (struct du_rect){x, y, 40, 22}; x -= 4;
    int hide_new = (o->flags & DU_FILE_NO_NEW_FOLDER) != 0;
    x -= 80; item(d, F_NEWDIR, DU_BUTTON, ID_NEWDIR, "&New Folder")->box = (struct du_rect){x, y, 80, 22}; x -= 4;
    d->items[F_NEWDIR].hidden = (uint8_t)hide_new;
    x -= 40; item(d, F_UP, DU_BUTTON, ID_UP, "Up")->box = (struct du_rect){x, y, 40, 22}; x -= 4;
    int folder_w = x - (b.x + 80); if (folder_w < 40) folder_w = 40;
    item(d, F_FOLDER, DU_EDIT, ID_FOLDER, o->folder)->box = (struct du_rect){b.x + 80, y, folder_w, 22};
    d->items[F_FOLDER].icon = du_icon_argb(du_icon_name("type:folder"), 16);
    du_set_tip(&d->items[F_UP], "Up One Level"); du_set_tip(&d->items[F_NEWDIR], "Create New Folder");
    du_set_tip(&d->items[F_VLIST], "List"); du_set_tip(&d->items[F_VDETAILS], "Details");
    struct du_widget *list = item(d, F_LIST, DU_LISTVIEW, ID_LIST, "");
    list->view = DU_LIST; list->inactive_selection = 1;
    int pane = b.w >= 460 ? 150 : 120;
    list->box = (struct du_rect){b.x + 14 + pane + 8, y + 30, b.w - 36 - pane, row1 - 10 - (y + 30)};
    file_columns(list, 0); list->view = DU_LIST;
    struct du_widget *places = item(d, F_PLACES, DU_TREE, ID_PLACES, "");
    places->inactive_selection = 1;
    places->box = (struct du_rect){b.x + 14, y + 48, pane, list->box.h - 18};
    item(d, F_PLACES_TITLE, DU_HEADER, 0, "Places")->box = (struct du_rect){b.x + 14, y + 30, pane, 18};
    item(d, F_NAME, DU_EDIT, ID_NAME, o->name)->box = (struct du_rect){b.x + 94, row1, right - BTN_W - 10 - (b.x + 94), 22};
    d->items[F_NAME].anchor = 0;
    item(d, F_LOOK, DU_LABEL, 0, "Look &in:")->box = (struct du_rect){b.x + 14, y, 64, 22};
    item(d, F_FILE, DU_LABEL, 0, "File &name:")->box = (struct du_rect){b.x + 14, row1, 78, 22};
    item(d, F_TYPE_LABEL, DU_LABEL, 0, "Files of &type:")->box = (struct du_rect){b.x + 14, row2, 78, 22};
    struct du_widget *type = item(d, F_TYPE, DU_COMBO, ID_TYPE, "");
    type->box = (struct du_rect){b.x + 94, row2, right - BTN_W - 10 - (b.x + 94), 22};
    du_combo_set(type, f->type_rows, f->filter_count, f->filter);
    item(d, F_SUB_YES, DU_BUTTON, ID_SUB_YES, "&Yes")->hidden = 1;
    item(d, F_SUB_NO, DU_BUTTON, ID_SUB_NO, "&No")->hidden = 1;
    item(d, F_SUB_EDIT, DU_EDIT, ID_SUB_EDIT, "")->hidden = 1;
    int r = navigate(u, o->folder && o->folder[0] ? o->folder : "/");
    if (r && navigate(u, "/")) { du_dialog_close(u); return r; }
    du_dialog_tick(u);
    return 0;
}

int du_file_dialog(struct du_ui *u, int id, int save, const char *title, const char *folder,
                   const char *name, du_lister lister, void *ctx)
{
    struct du_file_options o = {.flags = save ? DU_FILE_SAVE | DU_FILE_OVERWRITE_PROMPT : 0,
                                .title = title, .folder = folder, .name = name, .lister = lister, .lister_ctx = ctx};
    return du_file_open(u, id, &o);
}

void du_property_sheet(struct du_ui *u, int id, const char *title, int width, int height,
                       const struct du_item *pages, unsigned count)
{
    begin(u, DU_DLG_PROGRESS, id, title, "");
    struct du_dialog *d = &u->dialog;
    d->sheet = 1; d->buttons = 3; d->cancel = 1; d->focus = 0;
    place(u, width, height);
    item(d, 0, DU_BUTTON, ID_BUTTON, "&OK");
    item(d, 1, DU_BUTTON, ID_BUTTON - 1, "&Cancel");
    item(d, 2, DU_BUTTON, ID_BUTTON - 2, "&Apply");
    button_row(d, 3, d->box.y + d->box.h - BTN_H - 12);
    struct du_widget *tabs = item(d, 3, DU_TABS, ID_BAR, "");
    tabs->items = (struct du_item *)pages; tabs->count = count;
    tabs->box = (struct du_rect){d->box.x + 10, body_y(d) + 6, d->box.w - 20, d->box.h - 74};
}
struct du_widget *du_property_control(struct du_ui *u, unsigned slot, enum du_kind kind, int id,
                                     struct du_rect box, const char *text)
{
    if (!u->dialog.sheet || slot < 4 || slot >= DU_DIALOG_ITEMS) return NULL;
    struct du_widget *w = item(&u->dialog, slot, kind, id, text);
    box.x += u->dialog.box.x; box.y += body_y(&u->dialog);
    w->box = box;
    return w;
}

void du_dialog_icon(struct du_ui *u, const uint32_t *argb32)
{ if (u->dialog.kind == DU_DLG_MESSAGE && u->dialog.icon) { u->dialog.icon_pixels = argb32; dialog_damage(u); } }

int du_dialog_id(const struct du_ui *u) { return u->dialog.kind ? u->dialog.id : 0; }
struct du_rect du_dialog_button(const struct du_ui *u, unsigned i)
{ return i < u->dialog.count ? u->dialog.items[i].box : (struct du_rect){0, 0, 0, 0}; }

void du_dialog_close(struct du_ui *u)
{
    if (u->dialog.kind) dialog_damage(u);
    struct du_dialog *d = &u->dialog;
    release(u);
    memset(d,0,sizeof(*d)); d->capture=-1;
    if (u->focus>=0 && (unsigned)u->focus<u->count) du_invalidate(u,u->widgets[u->focus].box);
}

static void finish(struct du_ui *u, int index)
{
    struct du_dialog *d = &u->dialog;
    if (d->accept && d->accept(u, index, d->accept_ctx)) { dialog_damage(u); return; }
    char text[DU_TEXT_MAX] = "";
    if (d->kind == DU_DLG_INPUT) snprintf(text, sizeof(text), "%s", d->items[2].text);
    if ((d->kind == DU_DLG_OPEN || d->kind == DU_DLG_SAVE) && index == 0) {
        struct file_state *f = d->state;
        if (f && f->pending[0]) snprintf(text, sizeof(text), "%s", f->pending);
        else join(text, d->folder, d->items[F_NAME].text);
    }
    int id = d->id; unsigned key = (unsigned)d->result;
    du_dialog_close(u);
    du_emit(u, (struct du_event){.type = DU_EV_DIALOG, .id = id, .index = index, .key = key, .text = text});
}

void du_dialog_finish(struct du_ui *u, int index, const char *text)
{
    int id = u->dialog.id; unsigned key = (unsigned)u->dialog.result;
    du_dialog_close(u);
    du_emit(u, (struct du_event){.type = DU_EV_DIALOG, .id = id, .index = index, .key = key, .text = text});
}

static void parent_folder(struct du_ui *u)
{
    char path[DU_TEXT_MAX]; snprintf(path, sizeof(path), "%s", u->dialog.folder);
    char *slash = strrchr(path, '/');
    if (!slash) return;
    if (slash == path) path[1] = 0; else *slash = 0;
    navigate(u, path);
}

/* ---- file dialog: name validation, confirmations, new folder ---- */
/* Collapse "." and ".." components and repeated slashes of an absolute path. */
static void normalize(char *path)
{
    char out[DU_TEXT_MAX]; size_t n = 1; out[0] = '/'; out[1] = 0;
    for (const char *p = path; *p;) {
        while (*p == '/') p++;
        const char *seg = p; while (*p && *p != '/') p++;
        size_t len = (size_t)(p - seg);
        if (!len || (len == 1 && seg[0] == '.')) continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (n > 1 && out[n - 1] != '/') n--;
            if (n > 1) n--;
            out[n] = 0; if (!n) { out[0] = '/'; out[1] = 0; n = 1; }
            continue;
        }
        if (n + len + 2 >= sizeof(out)) return;
        if (n > 1) out[n++] = '/';
        memcpy(out + n, seg, len); n += len; out[n] = 0;
    }
    snprintf(path, DU_TEXT_MAX, "%s", out);
}
static int ci_equal(const char *a, const char *b)
{
    while (*a && fold((unsigned char)*a) == fold((unsigned char)*b)) { a++; b++; }
    return !*a && !*b;
}
static int entry_index(const struct du_dialog *d, const char *name)
{
    for (unsigned i = 0; i < d->entry_count; i++) if (ci_equal(d->entries[i].name, name)) return (int)i;
    return -1;
}

static void sub_close(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    d->sub = SUB_NONE;
    d->items[F_SUB_YES].hidden = d->items[F_SUB_NO].hidden = d->items[F_SUB_EDIT].hidden = 1;
    d->focus = F_NAME; dialog_damage(u);
}

static void sub_open(struct du_ui *u, int kind, const char *text, int icon, const char *edit)
{
    struct du_dialog *d = &u->dialog; struct file_state *f = FILE_STATE(d);
    d->sub = kind; f->sub_icon = icon;
    snprintf(f->sub_text, sizeof(f->sub_text), "%s", text);
    int pw = d->box.w - 40; if (pw > 380) pw = 380; if (pw < 160) pw = d->box.w - 8;
    struct du_rect measure = {0, 0, pw - 28 - (icon ? 46 : 0), 4000};
    int th = du_text_wrap(NULL, measure, f->sub_text, 0) * (desk_font_line(desk_theme->font_ui) + 1);
    if (icon && th < 32) th = 32;
    int ph = 16 + th + (kind == SUB_NEWDIR ? 8 + 22 : 0) + 14 + BTN_H + 12;
    f->panel_w = pw; f->panel_h = ph; f->panel_dx = (d->box.w - pw) / 2; f->panel_dy = (d->box.h - ph) / 2;
    struct du_rect pr = {d->box.x + f->panel_dx, d->box.y + f->panel_dy, pw, ph};
    int by = pr.y + ph - BTN_H - 12;
    du_set_text(&d->items[F_SUB_YES], kind == SUB_CONFIRM ? "&Yes" : "&OK");
    du_set_text(&d->items[F_SUB_NO], kind == SUB_CONFIRM ? "&No" : "Cancel");
    if (kind == SUB_MESSAGE) d->items[F_SUB_YES].box = (struct du_rect){pr.x + (pw - BTN_W) / 2, by, BTN_W, BTN_H};
    else {
        d->items[F_SUB_NO].box = (struct du_rect){pr.x + pw - 14 - BTN_W, by, BTN_W, BTN_H};
        d->items[F_SUB_YES].box = (struct du_rect){pr.x + pw - 14 - 2 * BTN_W - 6, by, BTN_W, BTN_H};
    }
    d->items[F_SUB_YES].hidden = 0; d->items[F_SUB_NO].hidden = kind == SUB_MESSAGE;
    d->items[F_SUB_EDIT].hidden = kind != SUB_NEWDIR;
    if (kind == SUB_NEWDIR) {
        du_set_text(&d->items[F_SUB_EDIT], edit ? edit : "");
        d->items[F_SUB_EDIT].anchor = 0;
        d->items[F_SUB_EDIT].box = (struct du_rect){pr.x + 14, pr.y + 16 + th + 8, pw - 28, 22};
    }
    d->focus = kind == SUB_CONFIRM ? F_SUB_NO : kind == SUB_NEWDIR ? F_SUB_EDIT : F_SUB_YES;
    if (u->sound && icon) u->sound(u->sound_ctx, du_message_sound(icon));
    dialog_damage(u);
}

static void sub_message(struct du_ui *u, int icon, const char *fmt, const char *name)
{
    char text[320]; snprintf(text, sizeof(text), fmt, name);
    sub_open(u, SUB_MESSAGE, text, icon, NULL);
}

/* Look a path up: 1 file, 2 folder, 0 absent, -errno when its folder cannot be listed. */
static int lookup(struct du_ui *u, const char *path)
{
    struct du_dialog *d = &u->dialog;
    const char *base = base_name(path);
    char parent[DU_TEXT_MAX]; size_t n = (size_t)(base - path);
    if (n > 1) n--;
    snprintf(parent, sizeof(parent), "%.*s", (int)n, path);
    if (!parent[0]) strcpy(parent, "/");
    const struct du_file_entry *list = d->entries; unsigned count = d->entry_count;
    struct du_file_entry *scratch = NULL;
    if (strcmp(parent, d->folder)) {
        scratch = calloc(DU_FILE_ENTRIES, sizeof(*scratch));
        if (!scratch) return -ENOMEM;
        unsigned c = 0; int r = d->lister(d->lister_ctx, parent, scratch, DU_FILE_ENTRIES, &c);
        if (r) { free(scratch); return r < 0 ? r : -r; }
        list = scratch; count = c;
    }
    int found = 0;
    for (unsigned i = 0; i < count && !found; i++) if (ci_equal(list[i].name, base)) found = list[i].directory ? 2 : 1;
    free(scratch);
    return found;
}

static void accept_file(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog; struct file_state *f = FILE_STATE(d);
    int save = d->kind == DU_DLG_SAVE;
    char name[DU_TEXT_MAX]; snprintf(name, sizeof(name), "%s", d->items[F_NAME].text);
    char *start = name; while (*start == ' ') start++;
    size_t len = strlen(start); while (len && start[len - 1] == ' ') start[--len] = 0;
    if (!*start) return;
    /* A name with wildcards is a filter, not a file (classic file dialogs). */
    if (strpbrk(start, "*?")) {
        copy_str(f->typed, sizeof(f->typed), start);
        refilter(u); du_set_text(&d->items[F_NAME], ""); return;
    }
    for (const char *c = start; *c; c++)
        if (*c == '"' || *c == '<' || *c == '>' || *c == '|' || *c == ':' || (unsigned char)*c < 32) {
            sub_message(u, DU_DLG_ICON_WARNING, "%s\nThe file name is not valid.", start); return;
        }
    char path[DU_TEXT_MAX]; join(path, d->folder, start); normalize(path);
    int kind = lookup(u, path);
    if (kind == 2) { if (!navigate(u, path)) du_set_text(&d->items[F_NAME], ""); return; }
    if (kind < 0) { sub_message(u, DU_DLG_ICON_WARNING, "%s\nThe path does not exist.\nPlease verify that the path is correct and try again.", start); return; }
    /* The default extension is appended when the user types none; a name that
     * exists exactly as typed is opened as it is. */
    if (f->default_ext[0] && !has_extension(start) && (save || !kind)) {
        char with[DU_TEXT_MAX];
        if (snprintf(with, sizeof(with), "%s.%s", path, f->default_ext) < (int)sizeof(with)) {
            int k = lookup(u, with);
            if (k == 2) { sub_message(u, DU_DLG_ICON_WARNING, "%s\nThat name is a folder.", start); return; }
            if (k >= 0) { snprintf(path, sizeof(path), "%s", with); kind = k; }
        }
    }
    const char *shown = base_name(path);
    if (!kind && !save && (f->flags & DU_FILE_FILE_MUST_EXIST)) {
        sub_message(u, DU_DLG_ICON_WARNING, "%.200s\nFile not found.\nPlease verify the correct file name was given.", shown); return;
    }
    snprintf(f->pending, sizeof(f->pending), "%s", path);
    d->result = f->filter;
    if (kind == 1 && save && (f->flags & DU_FILE_OVERWRITE_PROMPT)) {
        char text[320]; snprintf(text, sizeof(text), "%.200s already exists.\nDo you want to replace it?", shown);
        sub_open(u, SUB_CONFIRM, text, DU_DLG_ICON_WARNING, NULL);
        return;
    }
    finish(u, 0);
}

static void file_newdir(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    char name[64] = "New Folder";
    for (unsigned n = 2; entry_index(d, name) >= 0 && n < 1000; n++) snprintf(name, sizeof(name), "New Folder (%u)", n);
    sub_open(u, SUB_NEWDIR, "Name of the new folder:", 0, name);
}

static void file_newdir_accept(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog; struct file_state *f = FILE_STATE(d);
    char name[DU_TEXT_MAX]; snprintf(name, sizeof(name), "%s", d->items[F_SUB_EDIT].text);
    char *start = name; while (*start == ' ') start++;
    size_t len = strlen(start); while (len && start[len - 1] == ' ') start[--len] = 0;
    sub_close(u);
    if (!*start || strpbrk(start, "/\\*?\"<>|") ) { sub_message(u, DU_DLG_ICON_WARNING, "%s\nThe folder name is not valid.", *start ? start : "(empty)"); return; }
    if (entry_index(d, start) >= 0) { sub_message(u, DU_DLG_ICON_WARNING, "%s\nA file or folder with that name already exists.", start); return; }
    char path[DU_TEXT_MAX]; join(path, d->folder, start);
    int r = f->make_dir(f->make_dir_ctx, path);
    if (r) {
        char text[320]; snprintf(text, sizeof(text), "The folder could not be created:\n%.100s\n%.100s", start, strerror(r < 0 ? -r : r));
        sub_open(u, SUB_MESSAGE, text, DU_DLG_ICON_ERROR, NULL); return;
    }
    char folder[DU_TEXT_MAX]; snprintf(folder, sizeof(folder), "%s", d->folder);
    if (navigate(u, folder)) return;
    struct du_widget *list = &d->items[F_LIST];
    for (unsigned k = 0; k < f->shown; k++) {
        d->rows[k].selected = ci_equal(d->entries[f->map[k]].name, start);
        if (d->rows[k].selected) { list->active = k; du_list_ensure(list, k); }
    }
}

static void sub_yes(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    if (d->sub == SUB_CONFIRM) finish(u, 0);
    else if (d->sub == SUB_NEWDIR) file_newdir_accept(u);
    else sub_close(u);
}
static void sub_no(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    if (d->sub == SUB_CONFIRM) FILE_STATE(d)->pending[0] = 0;
    sub_close(u);
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
        if (d->sheet && d->changed) d->changed(u, e, d->sheet_ctx);
        if (d->on_event && d->on_event(u, e)) continue;
        int file = d->kind == DU_DLG_OPEN || d->kind == DU_DLG_SAVE;
        if (e->type == DU_EV_ACTIVATE && e->id <= ID_BUTTON && e->id > ID_BUTTON - 4) {
            int index = ID_BUTTON - e->id;
            if (file && index == 0) accept_file(u); else finish(u, index);
        } else if (e->type == DU_EV_ACTIVATE && e->id == ID_UP) parent_folder(u);
        else if ((e->type==DU_EV_SELECT || e->type==DU_EV_OPEN) && e->id==ID_PLACES && e->index>=0 && d->places && (unsigned)e->index<d->places->count) {
            if (!navigate(u,d->places->entries[e->index].path)) du_set_text(&d->items[F_NAME],"");
            places_select(d);
        }
        else if (e->type == DU_EV_COMMIT && e->id == ID_NAME) accept_file(u);
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

static int focusable(const struct du_widget *w) { return du_focusable(w) || (w->kind == DU_BUTTON && !w->disabled && !w->hidden); }

void du_dialog_route(struct du_ui *u, void (*fn)(struct du_ui *, struct du_widget *, int), struct du_widget *w, int arg)
{ DIALOG_CALL(fn(u, w, arg)); }

/* ---- file dialog hooks ---- */
static int file_event(struct du_ui *u, const struct du_event *e)
{
    struct du_dialog *d = &u->dialog; struct file_state *f = FILE_STATE(d);
    struct du_widget *list = &d->items[F_LIST];
    if (e->type == DU_EV_ACTIVATE) {
        if (e->id == ID_SUB_YES) { sub_yes(u); return 1; }
        if (e->id == ID_SUB_NO) { sub_no(u); return 1; }
        if (e->id == ID_NEWDIR) { file_newdir(u); return 1; }
        if (e->id == ID_VLIST || e->id == ID_VDETAILS) {
            list->view = e->id == ID_VLIST ? DU_LIST : DU_DETAILS; list->scroll = list->hscroll = 0;
            du_invalidate(u, list->box); return 1;
        }
    }
    if (e->type == DU_EV_COMMIT && e->id == ID_SUB_EDIT) { sub_yes(u); return 1; }
    if (e->type == DU_EV_CANCEL && e->id == ID_SUB_EDIT) { sub_no(u); return 1; }
    if (e->type == DU_EV_SELECT && e->id == ID_TYPE) {
        f->filter = e->index; d->result = e->index; f->typed[0] = 0; refilter(u); return 1;
    }
    if ((e->type == DU_EV_SELECT || e->type == DU_EV_OPEN) && e->id == ID_LIST) {
        if (e->index < 0 || (unsigned)e->index >= f->shown) return 1;
        const struct du_file_entry *entry = &d->entries[f->map[e->index]];
        if (e->type == DU_EV_SELECT) {
            for (unsigned i = 0; i < f->shown; i++) d->rows[i].selected = i == (unsigned)e->index;
            if (!entry->directory) du_set_text(&d->items[F_NAME], entry->name);
        } else if (entry->directory) {
            char path[DU_TEXT_MAX]; join(path, d->folder, entry->name); navigate(u, path);
        } else { du_set_text(&d->items[F_NAME], entry->name); accept_file(u); }
        return 1;
    }
    return 0;
}

static int file_key(struct du_ui *u, unsigned code)
{
    struct du_dialog *d = &u->dialog;
    if (!d->sub) return 0;
    struct du_widget *fw = d->focus >= 0 && (unsigned)d->focus < d->count ? &d->items[d->focus] : NULL;
    if (code == DU_KEY_ESC) { sub_no(u); return 1; }
    int two = !d->items[F_SUB_NO].hidden;
    if (code == DU_KEY_TAB) {
        static const int order[3] = {F_SUB_EDIT, F_SUB_YES, F_SUB_NO};
        int at = 0; for (int i = 0; i < 3; i++) if (order[i] == d->focus) at = i;
        for (int n = 1; n <= 3; n++) {
            int next = order[(at + (u->modifiers & DU_SHIFT ? 3 - n : n)) % 3];
            if (!d->items[next].hidden) { d->focus = next; break; }
        }
        dialog_damage(u); return 1;
    }
    if (fw && fw->kind == DU_EDIT) {
        if (code == DU_KEY_ENTER) { sub_yes(u); return 1; }
        int handled = 0; DIALOG_CALL(handled = du_widget_key(u, fw, code)); (void)handled; return 1;
    }
    if (code == DU_KEY_ENTER || code == DU_KEY_SPACE) { if (d->focus == F_SUB_NO) sub_no(u); else sub_yes(u); return 1; }
    if (two && (code == DU_KEY_LEFT || code == DU_KEY_RIGHT || code == DU_KEY_UP || code == DU_KEY_DOWN)) {
        d->focus = d->focus == F_SUB_YES ? F_SUB_NO : F_SUB_YES; dialog_damage(u); return 1;
    }
    unsigned saved = u->modifiers; u->modifiers &= ~DU_ALT;
    unsigned ch = du_key_char(u, code); u->modifiers = saved;
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    if (ch == 'y' || ch == 'o') sub_yes(u); else if (ch == 'n' && two) sub_no(u);
    return 1;
}

static int file_press(struct du_ui *u, int button, int x, int y)
{
    (void)button;
    struct du_dialog *d = &u->dialog;
    if (!d->sub) return 0;
    for (unsigned i = F_SUB_YES; i <= F_SUB_EDIT; i++)
        if (!d->items[i].hidden && du_contains(d->items[i].box, x, y)) return 0;
    return 1;   /* the panel is modal over the dialog */
}

static void file_draw(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog; struct file_state *f = FILE_STATE(d);
    if (!d->sub) return;
    struct du_canvas *c = &u->canvas;
    struct du_rect pr = {d->box.x + f->panel_dx, d->box.y + f->panel_dy, f->panel_w, f->panel_h};
    du_shadow(c, pr, 4);
    du_fill(c, pr, DU_FACE); du_edge(c, pr, DU_RAISED);
    int icon = f->sub_icon, tx = pr.x + 14 + (icon ? 46 : 0);
    if (icon && du_message_icon_name(icon)) du_icon_draw(c, pr.x + 14, pr.y + 16, du_icon_for(du_message_icon_name(icon), 32), DU_ICON_NORMAL);
    struct du_rect body = {tx, pr.y + 16, pr.x + pr.w - 14 - tx, pr.h};
    int lines = du_text_wrap(NULL, body, f->sub_text, 0), th = lines * (desk_font_line(desk_theme->font_ui) + 1);
    if (icon && th < 32) body.y += (32 - th) / 2;
    du_text_wrap(c, body, f->sub_text, DU_INK);
    for (unsigned i = F_SUB_YES; i <= F_SUB_EDIT; i++) {
        struct du_widget *w = &d->items[i];
        if (w->hidden) continue;
        if (w->kind == DU_BUTTON) w->def = d->sub == SUB_CONFIRM ? i == F_SUB_NO : i == F_SUB_YES;
        struct du_rect old = c->clip; c->clip = du_intersection(old, w->box);
        du_draw_widget(u, w, d->focus == (int)i && u->focused_window);
        c->clip = old;
    }
}

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
    if (d->on_key && d->on_key(u, code)) return;
    if (code == DU_KEY_ESC) { finish(u, d->cancel); return; }
    if (d->sheet && code == DU_KEY_TAB && (u->modifiers & DU_CTRL)) {
        struct du_widget *tabs = &d->items[3];
        if (tabs->count) {
            tabs->active = (tabs->active + ((u->modifiers & DU_SHIFT) ? tabs->count - 1 : 1)) % tabs->count;
            if (d->changed) d->changed(u, &(struct du_event){.type=DU_EV_SELECT, .id=ID_BAR, .index=(int)tabs->active}, d->sheet_ctx);
            dialog_damage(u);
        }
        return;
    }
    if (code == DU_KEY_TAB) {
        for (unsigned n = 0; n < d->count; n++) {
            d->focus = u->modifiers & DU_SHIFT ? (d->focus <= 0 ? (int)d->count - 1 : d->focus - 1) : (d->focus + 1) % (int)d->count;
            if (focusable(&d->items[d->focus])) break;
        }
        dialog_damage(u); return;
    }
    if (f && (f->kind == DU_EDIT || f->kind == DU_TEXTAREA || f->kind == DU_LISTVIEW || f->kind == DU_TREE || f->kind == DU_TABS ||
              f->kind == DU_COMBO || f->kind == DU_TRACKBAR)) {
        int handled = 0; DIALOG_CALL(handled = du_widget_key(u, f, code)); if (handled) return;
    }
    if (f && (f->kind == DU_CHECKBOX || f->kind == DU_RADIO)) {
        int handled = 0; DIALOG_CALL(handled = du_widget_key(u, f, code)); if (handled) return;
    }
    if ((code == DU_KEY_ENTER || code == DU_KEY_SPACE) && f && f->kind == DU_BUTTON && (unsigned)d->focus >= d->buttons && f->id != ID_UP) {
        DIALOG_CALL(du_post(u, DU_EV_ACTIVATE, f->id, 0)); return;   /* a button that is not one of the closing buttons */
    }
    if (code == DU_KEY_ENTER || (code == DU_KEY_SPACE && f && f->kind == DU_BUTTON)) {
        if (d->sheet && f && f->kind==DU_BUTTON && (unsigned)d->focus>=d->buttons) {
            DIALOG_CALL(du_widget_key(u,f,code)); return;
        }
        int index = f && f->kind == DU_BUTTON ? (f->id == ID_UP ? -1 : ID_BUTTON - f->id) : 0;
        if (f && f->id == ID_UP) { parent_folder(u); return; }
        if ((d->kind == DU_DLG_OPEN || d->kind == DU_DLG_SAVE) && index == 0) accept_file(u); else finish(u, index);
        return;
    }
    if ((code == DU_KEY_LEFT || code == DU_KEY_RIGHT || code == DU_KEY_UP || code == DU_KEY_DOWN) && f && f->kind == DU_BUTTON && (unsigned)d->focus < d->buttons) {
        int step = code == DU_KEY_LEFT || code == DU_KEY_UP ? -1 : 1;
        d->focus = (d->focus + step + (int)d->buttons) % (int)d->buttons; dialog_damage(u); return;
    }
    unsigned saved = u->modifiers; u->modifiers &= ~DU_ALT;
    unsigned ch = du_key_char(u, code); u->modifiers = saved;
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    if (ch && (!f || f->kind != DU_EDIT || (u->modifiers & DU_ALT))) {
        for (unsigned i = 0; i < d->count; i++) {
            const char *amp = strchr(d->items[i].text, '&');
            unsigned m = amp && amp[1] ? (unsigned char)amp[1] : 0;
            if (m >= 'A' && m <= 'Z') m += 32;
            if (m != ch || d->items[i].hidden || d->items[i].disabled) continue;
            if (d->items[i].kind == DU_BUTTON && i < d->buttons) {
                if ((d->kind == DU_DLG_OPEN || d->kind == DU_DLG_SAVE) && i == 0) accept_file(u); else finish(u, (int)i);
            } else if (d->sheet && d->items[i].kind==DU_BUTTON) {
                d->focus=(int)i; DIALOG_CALL(du_widget_key(u,&d->items[i],DU_KEY_ENTER));
            } else if (d->sheet && (d->items[i].kind == DU_CHECKBOX || d->items[i].kind == DU_RADIO)) {
                d->focus=(int)i; DIALOG_CALL(du_widget_key(u,&d->items[i],DU_KEY_SPACE));
            } else if (d->items[i].kind == DU_BUTTON) {
                d->focus = (int)i; DIALOG_CALL(du_post(u, DU_EV_ACTIVATE, d->items[i].id, 0));
            } else if (d->items[i].kind == DU_LABEL && i + 1 <= d->count) {
                if (d->kind == DU_DLG_OPEN || d->kind == DU_DLG_SAVE) d->focus = i == F_LOOK ? F_FOLDER : i == F_TYPE_LABEL ? F_TYPE : F_NAME;
                else for (unsigned j = i + 1; j < d->count; j++) if (focusable(&d->items[j])) { d->focus = (int)j; break; }   /* the control a label names follows it */
            }
            dialog_damage(u); return;
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

/* G15: the "?" button takes the slot of minimise, as in a WS_EX_CONTEXTHELP frame. */
struct du_rect du_dialog_help_box(const struct du_ui *u)
{
    const struct du_dialog *d = &u->dialog;
    int b[4] = {0, 0, 0, 0};
    if (d->kind && d->help_button) desk_title_button_box(d->box.x, d->box.y, d->box.w, DESK_TB_HELP, b);
    return (struct du_rect){b[0], b[1], b[2], b[3]};
}
void du_dialog_help_button(struct du_ui *u, int on)
{
    struct du_dialog *d = &u->dialog;
    if (!d->kind) return;
    d->help_button = on != 0; d->help_hot = 0; dialog_damage(u);
}
struct du_widget *du_dialog_item_at(struct du_ui *u, int x, int y)
{
    struct du_dialog *d = &u->dialog;
    for (unsigned i = d->count; i > 0; i--)
        if (!d->items[i - 1].hidden && du_contains(d->items[i - 1].box, x, y)) return &d->items[i - 1];
    return NULL;
}

void du_dialog_press(struct du_ui *u, int button)
{
    struct du_dialog *d = &u->dialog;
    int x = u->mouse_x, y = u->mouse_y;
    if (!du_contains(d->box, x, y)) return;
    if (button == 1 && du_contains(close_box(d), x, y)) { finish(u, d->cancel); return; }
    if (button == 1 && d->help_button && du_contains(du_dialog_help_box(u), x, y)) { du_context_help(u, 1); dialog_damage(u); return; }
    if (button == 1 && y < body_y(d)) { d->dragging = 1; d->drag_x = x - d->box.x; d->drag_y = y - d->box.y; return; }
    if (d->on_press && d->on_press(u, button, x, y)) return;
    for (unsigned i = d->count; i > 0; i--) {
        struct du_widget *w = &d->items[i - 1];
        if (w->hidden || !du_contains(w->box, x, y)) continue;
        if (focusable(w)) d->focus = (int)i - 1;
        if (button == 1) d->capture = (int)i - 1;
        DIALOG_CALL(du_widget_press(u, w, button, x, y));
        dialog_damage(u);
        return;
    }
}

void du_dialog_motion(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    int hot = !d->dragging && du_contains(close_box(d), u->mouse_x, u->mouse_y);
    if (hot != d->close_hot) { d->close_hot = hot; dialog_damage(u); }
    {   /* tooltips of the control under the pointer */
        struct du_widget *under = NULL;
        if (!d->dragging && d->capture < 0)
            for (unsigned i = d->count; i > 0 && !under; i--)
                if (!d->items[i - 1].hidden && du_contains(d->items[i - 1].box, u->mouse_x, u->mouse_y)) under = &d->items[i - 1];
        du_tip_motion(u, under);
    }
    int help_hot = !d->dragging && d->help_button && du_contains(du_dialog_help_box(u), u->mouse_x, u->mouse_y);
    if (help_hot != d->help_hot) { d->help_hot = help_hot; dialog_damage(u); }
    if (d->dragging) {
        int nx = u->mouse_x - d->drag_x, ny = u->mouse_y - d->drag_y;
        if (nx < -d->box.w + 40) nx = -d->box.w + 40;
        if (nx > u->canvas.width - 40) nx = u->canvas.width - 40;
        if (ny < 0) ny = 0;
        if (ny > u->canvas.height - 20) ny = u->canvas.height - 20;
        dialog_damage(u);
        int dx = nx - d->box.x, dy = ny - d->box.y;
        d->box.x += dx; d->box.y += dy;
        for (unsigned i = 0; i < d->count; i++) { d->items[i].box.x += dx; d->items[i].box.y += dy; }
        dialog_damage(u); return;
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
    struct desk_theme role; const struct desk_theme *saved=desk_theme;
    if(d->kind==DU_DLG_MESSAGE) saved=desk_theme_role(&role,AP_MESSAGE);
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
    if (d->help_button) style.buttons |= DESK_TB_HELP;
    if (d->help_button && d->help_hot) style.hot = DESK_TB_HELP;
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
    } else if (d->sheet) {
        /* The tab control draws its page background before the body. */
        du_draw_widget(u, &d->items[3], d->focus == 3 && u->focused_window);
        draw_text(c, b.x + 24, top + 42, b.w - 48, d->text);
        if (d->paint) d->paint(u, d->sheet_ctx);
    } else if (d->kind == DU_DLG_INPUT || d->kind == DU_DLG_PROGRESS) {
        draw_text(c, b.x + 14, top + 12, b.w - 28, d->text);
    }
    int focus_button = d->focus >= 0 && (unsigned)d->focus < d->buttons;
    for (unsigned i = 0; i < d->count; i++) {
        struct du_widget *w = &d->items[i];
        if (w->hidden || (d->sheet && i == 3) || (d->overlay && i >= d->overlay)) continue;
        if (w->kind == DU_BUTTON && i < d->buttons) w->def = focus_button ? (int)i == d->focus : i == 0;
        struct du_rect old = c->clip;
        c->clip = du_intersection(old, w->box);
        du_draw_widget(u, w, d->focus == (int)i && u->focused_window);
        c->clip = old;
    }
    if (d->on_draw) { struct du_rect keep = c->clip; d->on_draw(u); c->clip = keep; }
    desk_theme=saved;
}
