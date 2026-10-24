/* SPDX-License-Identifier: MIT
 * CiukiOS Help window: Contents, Index and Search tabs on the left, the
 * read-only topic text on the right, Back, a status bar. */
#include "help.h"
#include "../libdeskui/recent.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LIST_W 214
#define TOP 48
static void status(struct help_app *a, const char *text) { du_set_text(du_get(a->ui,H_STATUS),text); du_invalidate_widget(a->ui,H_STATUS); }
static void search_clear(struct help_app *a)
{
    a->hit_count=0; struct du_widget *w=du_get(a->ui,H_SEARCH_LIST);
    if (w) { w->count=0; w->items=NULL; w->active=w->scroll=0; du_invalidate_widget(a->ui,H_SEARCH_LIST); }
}

static void show(struct du_ui *u, int id, int visible)
{
    struct du_widget *w = du_get(u, id);
    if (w && w->hidden == visible) { w->hidden = !visible; du_invalidate(u, w->box); }
}

void help_layout(struct help_app *a)
{
    struct du_rect old[DU_WIDGETS]; uint8_t hidden[DU_WIDGETS];
    for (unsigned i=0;i<a->ui->count;i++) { old[i]=a->ui->widgets[i].box; hidden[i]=a->ui->widgets[i].hidden; }
    struct du_canvas *c = &a->ui->canvas;
    int bottom = c->height - 20, ph = bottom - TOP - 2;
    int x = 6, w = LIST_W - 8, y0 = TOP + DU_TAB_H + 4, h = TOP + ph - y0 - 4;
    du_get(a->ui, H_BACK)->box = (struct du_rect){2, 22, 72, 22};
    du_get(a->ui, H_TABS)->box = (struct du_rect){2, TOP, LIST_W, ph};
    du_get(a->ui, H_TREE)->box = (struct du_rect){x, y0, w, h};
    du_get(a->ui, H_INDEX_EDIT)->box = (struct du_rect){x, y0, w, 22};
    du_get(a->ui, H_INDEX_LIST)->box = (struct du_rect){x, y0 + 26, w, h - 26};
    du_get(a->ui, H_SEARCH_EDIT)->box = (struct du_rect){x, y0, w, 22};
    du_get(a->ui, H_SEARCH_GO)->box = (struct du_rect){x, y0 + 26, w, 23};
    du_get(a->ui, H_SEARCH_LIST)->box = (struct du_rect){x, y0 + 53, w, h - 53};
    struct du_widget *t = du_get(a->ui, H_TEXT);
    t->box = (struct du_rect){LIST_W + 6, TOP, c->width - LIST_W - 8, ph};
    t->box.h-=a->wizard ? 62 : 86;
    int right=LIST_W+6, rw=c->width-right-2, links_y=bottom-82;
    du_get(a->ui,H_LINK_LABEL)->box=(struct du_rect){right,links_y,rw,18};
    du_get(a->ui,H_LINKS)->box=(struct du_rect){right,links_y+18,rw,62};
    du_get(a->ui,H_LINK_LABEL)->hidden=du_get(a->ui,H_LINKS)->hidden=a->wizard!=0;
    du_get(a->ui,H_WIZ_YES)->box=(struct du_rect){right,bottom-58,74,22};
    du_get(a->ui,H_WIZ_NO)->box=(struct du_rect){right+78,bottom-58,74,22};
    du_get(a->ui,H_WIZ_BACK)->box=(struct du_rect){right,bottom-28,68,24};
    du_get(a->ui,H_WIZ_NEXT)->box=(struct du_rect){right+72,bottom-28,68,24};
    du_get(a->ui,H_WIZ_CANCEL)->box=(struct du_rect){right+144,bottom-28,68,24};
    du_ta_fit(&a->ta, t->box);
    du_get(a->ui, H_STATUS)->box = (struct du_rect){0, bottom, c->width, 20};
    for (unsigned i=0;i<a->ui->count;i++) {
        struct du_widget *w=&a->ui->widgets[i];
        if (memcmp(&old[i],&w->box,sizeof(w->box)) || hidden[i]!=w->hidden) { du_invalidate(a->ui,old[i]); du_invalidate(a->ui,w->box); }
    }
}

/* ---- contents ---- */
static void contents_rebuild(struct help_app *a)
{
    unsigned n = 0, books = help_book_count(); int last_book = -2;
    if (a->topic==-2 && a->document) {
        if (a->document->toc_count) {
            unsigned closed=UINT32_MAX;
            for (unsigned i=0;i<a->document->toc_count;i++) {
                const struct hf_toc *t=&a->document->toc[i];
                if (closed!=UINT32_MAX) { if (t->depth>closed) continue; closed=UINT32_MAX; }
                int children=i+1<a->document->toc_count && a->document->toc[i+1].depth>t->depth;
                a->rows[n]=(struct help_row){t->page,(int)i};
                a->contents[n++]=(struct du_item){.text={t->title},.depth=(uint16_t)t->depth,.children=children,.expanded=a->open_toc[i],.selected=t->page>=0 && (unsigned)t->page==a->external_page};
                if (children && !a->open_toc[i]) closed=t->depth;
            }
            goto ready;
        }
        for (unsigned i=0;i<a->document->count;i++) {
            a->rows[n]=(struct help_row){(int)i,-1};
            a->contents[n++]=(struct du_item){.text={a->document->pages[i].title},.selected=i==a->external_page};
        }
        goto ready;
    }
    for (unsigned i = 0; i < help_topic_count; i++) {
        int book = help_topic_book(i);
        if (book >= 0 && book != last_book) {
            a->rows[n] = (struct help_row){-1, book};
            a->contents[n] = (struct du_item){.text = {help_book((unsigned)book)}, .children = 1, .expanded = a->open_books[book]};
            n++;
        }
        last_book = book;
        if (book >= 0 && !a->open_books[book]) continue;
        a->rows[n] = (struct help_row){(int)i, book};
        a->contents[n] = (struct du_item){.text = {help_topics[i].title}, .depth = book >= 0, .selected = (int)i == a->topic};
        n++;
    }
    (void)books;
ready:
    a->row_count = n;
    struct du_widget *w = du_get(a->ui, H_TREE);
    w->items = a->contents; w->count = n;
    for (unsigned i = 0; i < n; i++) if (a->contents[i].selected) { w->active = i; du_list_ensure(w, i); }
    if (w->active >= n) w->active = 0;
    du_invalidate_widget(a->ui, H_TREE);
}
static void book_toggle(struct help_app *a, unsigned row)
{
    if (a->topic==-2 && a->document) {
        if (row>=a->row_count || a->rows[row].book<0 || !a->contents[row].children) return;
        unsigned i=(unsigned)a->rows[row].book; a->open_toc[i]=!a->open_toc[i]; contents_rebuild(a); return;
    }
    if (row >= a->row_count || a->rows[row].topic >= 0) return;
    int book = a->rows[row].book;
    a->open_books[book] = !a->open_books[book];
    contents_rebuild(a);
    struct du_widget *w = du_get(a->ui, H_TREE);
    for (unsigned i = 0; i < a->row_count; i++) if (a->rows[i].topic < 0 && a->rows[i].book == book) { w->active = i; du_list_ensure(w, i); }
}

int help_load_text(struct help_app *a, const char *data, size_t n, const char *title)
{
    if (n > HELP_FILE_MAX) return -EFBIG;
    for (size_t i = 0; i < n; i++) if (!data[i]) return -EINVAL;    /* binary files are not text */
    /* CR LF and lone CR become LF for the text area. */
    char *copy = malloc(n + 1);
    if (!copy) return -ENOMEM;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\r') { copy[m++] = '\n'; if (i + 1 < n && data[i + 1] == '\n') i++; }
        else copy[m++] = data[i];
    }
    a->ta.readonly = 0;
    int err = du_ta_set_text(&a->ta, copy, m);
    a->ta.readonly = 1; a->ta.cursor = a->ta.anchor = 0; a->ta.top = a->ta.left = 0;
    free(copy);
    if (err) return -EFBIG;
    if (title) status(a, title);
    du_invalidate_widget(a->ui,H_TEXT);
    return 0;
}

static void back_state(struct help_app *a)
{
    struct du_widget *b = du_get(a->ui, H_BACK);
    b->disabled = a->back_count == 0; du_invalidate_widget(a->ui, H_BACK);
    for (unsigned i = 0; i < 5; i++) if (a->file_items[i].id == H_M_BACK) {
        if (a->back_count) a->file_items[i].flags &= ~DU_ITEM_DISABLED; else a->file_items[i].flags |= DU_ITEM_DISABLED;
    }
}
static void index_fill(struct help_app *);
static void topic_load(struct help_app *a, int topic)
{
    if (a->topic<0) search_clear(a);
    a->topic = topic;
    int book = help_topic_book((unsigned)topic);
    if (book >= 0) a->open_books[book] = 1;
    contents_rebuild(a);
    help_load_text(a, help_topics[topic].text, strlen(help_topics[topic].text), help_topics[topic].title);
    help_links(a);
    index_fill(a);
}
static void remember(struct help_app *a,int next)
{
    int current=a->topic==-2 ? -(int)a->external_page-2 : a->topic;
    if (current==-1 || current==next) return;
    if (a->back_count==HELP_BACK_MAX) { memmove(a->back,a->back+1,(HELP_BACK_MAX-1)*sizeof(*a->back)); a->back_count--; }
    a->back[a->back_count++]=current;
}
void help_show_topic(struct help_app *a, int topic)
{
    if (topic < 0 || (unsigned)topic >= help_topic_count) return;
    remember(a,topic);
    a->wizard=0;
    for (int id=H_WIZ_BACK;id<=H_WIZ_NO;id++) { struct du_widget *w=du_get(a->ui,id); if (!w->hidden) du_invalidate(a->ui,w->box); w->hidden=1; }
    topic_load(a, topic); back_state(a);
    help_layout(a);
}
void help_back(struct help_app *a)
{
    if (!a->back_count) return;
    int topic=a->back[--a->back_count];
    if (topic<=-2) {
        unsigned count=a->back_count;
        (void)help_document_show(a,(unsigned)(-topic-2)); a->back_count=count;
    } else topic_load(a,topic);
    back_state(a);
}
static void index_fill(struct help_app *);
int help_document_show(struct help_app *a,unsigned page)
{ return help_document_part(a,page,0); }
int help_document_part(struct help_app *a,unsigned page,size_t offset)
{
    if (!a->document) return -ENOENT;
    int e=hf_page_load(a->document,page); if (e) return e;
    struct hf_page *p=&a->document->pages[page];
    if (offset>p->length) return -EINVAL;
    size_t length=p->length-offset; if (length>HF_TEXT_LIMIT) length=HF_TEXT_LIMIT;
    e=help_load_text(a,p->text+offset,length,p->title); if (e) return e;
    if (a->topic!=-2) search_clear(a);
    remember(a,-(int)page-2); a->topic=-2; a->external_page=page;
    a->external_offset=offset;
    a->wizard=0;
    for (int id=H_WIZ_BACK;id<=H_WIZ_NO;id++) { struct du_widget *w=du_get(a->ui,id); if (!w->hidden) du_invalidate(a->ui,w->box); w->hidden=1; }
    contents_rebuild(a); index_fill(a); help_links(a); back_state(a); help_layout(a); return 0;
}
int help_document_adopt(struct help_app *a,struct hf_document *d,unsigned page,const char *path)
{
    if (d==a->document) return help_document_show(a,page);
    int e=hf_page_load(d,page); if (e) return e;
    struct hf_page *p=&d->pages[page]; size_t n=p->length>HF_TEXT_LIMIT ? HF_TEXT_LIMIT : p->length;
    e=help_load_text(a,p->text,n,p->title); if (e) return e;
    hf_close(a->document); a->document=d; a->back_count=0; a->topic=-2; a->external_page=page; a->external_offset=0;
    memset(a->open_toc,1,sizeof(a->open_toc));
    snprintf(a->path,sizeof(a->path),"%s",path);
    a->wizard=0;
    for (int id=H_WIZ_BACK;id<=H_WIZ_NO;id++) { struct du_widget *w=du_get(a->ui,id); if (!w->hidden) du_invalidate(a->ui,w->box); w->hidden=1; }
    search_clear(a);
    contents_rebuild(a); index_fill(a); help_links(a); back_state(a); help_layout(a); du_focus(a->ui,H_TREE); return 0;
}
int help_open_argument(struct help_app *a, const char *argument)
{
    if (!argument || strncmp(argument, "topic:", 6)) {
        struct hf_document *d=NULL; unsigned page; char path[161];
        int e=help_prepare_document(argument ? argument : "",&d,&page,path);
        if (!e) { e=help_document_adopt(a,d,page,path); if (e) hf_close(d); else help_request_ui(a,argument); return e; }
        if (argument && (!strncmp(argument,"hlp:",4) || !strncmp(argument,"chm:",4))) return e;
        const char *ext=argument ? strrchr(argument,'.') : NULL;
        if (ext && (!strcasecmp(ext,".hlp") || !strcasecmp(ext,".chm"))) return e;
        return help_open_file(a,argument ? argument : "");
    }
    int topic = help_topic_find(argument + 6);
    if (topic < 0) return -ENOENT;
    help_show_topic(a, topic);
    du_focus(a->ui, H_TREE);
    return 0;
}

int help_open_bytes(struct help_app *a, const char *path, const char *data, size_t n)
{
    char title[200]; snprintf(title,sizeof(title),"%.180s (read-only)",path);
    int error=help_load_text(a,data,n,title); if (error) return error;
    snprintf(a->path,sizeof(a->path),"%s",path);
    a->topic=-1; search_clear(a); contents_rebuild(a); index_fill(a); help_links(a);
    a->wizard=0;
    for (int id=H_WIZ_BACK;id<=H_WIZ_NO;id++) { struct du_widget *w=du_get(a->ui,id); if (!w->hidden) du_invalidate(a->ui,w->box); w->hidden=1; }
    help_layout(a); du_recent_add(path); du_focus(a->ui,H_TEXT); return 0;
}
int help_open_file(struct help_app *a, const char *path)
{
    FILE *f = fopen(path, "rb");
    int err = f ? 0 : -errno;
    char *data = err ? NULL : malloc(HELP_FILE_MAX + 2);
    size_t n = 0;
    if (!err && !data) err = -ENOMEM;
    if (!err) { n = fread(data, 1, HELP_FILE_MAX + 1, f); if (ferror(f)) err = -EIO; }
    if (f) fclose(f);
    if (!err) err=help_open_bytes(a,path,data,n);
    free(data);
    if (err) {
        char text[300];
        snprintf(text, sizeof(text), "The file cannot be shown:\n%.200s\n%s", path,
                 err == -EFBIG ? "It is larger than 60 KB." : err == -EINVAL ? "It is not a text file." :
                 err == -ENOENT ? "The file was not found." : "It could not be read.");
        du_message(a->ui, H_D_INFO, "CiukiOS Help", text, DU_DLG_ICON_WARNING, NULL, 0, 0, 0);
        return err;
    }
    return 0;
}

/* ---- tabs, index, search ---- */
void help_tab(struct help_app *a, int tab)
{
    struct du_ui *u = a->ui;
    if (tab < 0 || tab > HELP_TAB_SEARCH) return;
    struct du_widget *t = du_get(u, H_TABS);
    if (t->active != (unsigned)tab) { t->active = (unsigned)tab; du_invalidate(u, t->box); }
    show(u, H_TREE, tab == HELP_TAB_CONTENTS);
    show(u, H_INDEX_EDIT, tab == HELP_TAB_INDEX); show(u, H_INDEX_LIST, tab == HELP_TAB_INDEX);
    show(u, H_SEARCH_EDIT, tab == HELP_TAB_SEARCH); show(u, H_SEARCH_GO, tab == HELP_TAB_SEARCH); show(u, H_SEARCH_LIST, tab == HELP_TAB_SEARCH);
    du_focus(u, tab == HELP_TAB_CONTENTS ? H_TREE : tab == HELP_TAB_INDEX ? H_INDEX_EDIT : H_SEARCH_EDIT);
}
static void index_fill(struct help_app *a)
{
    if (a->topic==-2 && a->document) {
        a->index_count=a->document->keyword_count;
        if (a->index_count>HELP_INDEX_MAX) a->index_count=HELP_INDEX_MAX;
        for (unsigned i=0;i<a->index_count;i++) {
            struct hf_keyword *k=&a->document->keywords[i];
            snprintf(a->index[i].keyword,sizeof(a->index[i].keyword),"%.47s",k->name); a->index[i].topic=k->page;
        }
        for (unsigned i=1;i<a->index_count;i++) {
            struct help_index_entry key=a->index[i]; unsigned j=i;
            while (j && strcasecmp(a->index[j-1].keyword,key.keyword)>0) { a->index[j]=a->index[j-1]; j--; } a->index[j]=key;
        }
    } else
    a->index_count = help_index_build(a->index, HELP_INDEX_MAX);
    for (unsigned i = 0; i < a->index_count; i++) {
        int dup = (i && !strcasecmp(a->index[i].keyword, a->index[i - 1].keyword)) ||
                  (i + 1 < a->index_count && !strcasecmp(a->index[i].keyword, a->index[i + 1].keyword));
        if (dup) snprintf(a->index_labels[i], sizeof(a->index_labels[i]), "%s - %.63s", a->index[i].keyword, a->topic==-2 ? a->document->pages[a->index[i].topic].title : help_topics[a->index[i].topic].title);
        else snprintf(a->index_labels[i], sizeof(a->index_labels[i]), "%s", a->index[i].keyword);
        a->index_rows[i] = (struct du_item){.text = {a->index_labels[i]}};
    }
    struct du_widget *w = du_get(a->ui, H_INDEX_LIST);
    w->items = a->index_rows; w->count = a->index_count;
    if (w->active>=w->count) w->active=0;
    du_invalidate_widget(a->ui,H_INDEX_LIST);
}
static void index_seek(struct help_app *a)
{
    struct du_widget *e = du_get(a->ui, H_INDEX_EDIT), *l = du_get(a->ui, H_INDEX_LIST);
    unsigned at = help_index_seek(a->index, a->index_count, e->text);
    if (at < a->index_count) {
        for (unsigned i = 0; i < a->index_count; i++) a->index_rows[i].selected = i == at;
        l->active = at; du_list_ensure(l, at);
    }
    du_invalidate_widget(a->ui, H_INDEX_LIST);
}
void help_search_run(struct help_app *a)
{
    struct du_widget *e = du_get(a->ui, H_SEARCH_EDIT), *l = du_get(a->ui, H_SEARCH_LIST);
    if (a->topic==-2 && a->document) {
        a->hit_count=0;
        for (unsigned i=0;e->text[0] && i<a->document->count && a->hit_count<HELP_HITS_MAX;i++) {
            if (hf_page_load(a->document,i)) continue;
            const char *text=a->document->pages[i].text; size_t len=strlen(e->text);
            for (;*text;text++) if (!strncasecmp(text,e->text,len)) { a->hits[a->hit_count++]=(struct help_hit){i,1}; break; }
        }
    } else a->hit_count = help_search(e->text, a->hits, HELP_HITS_MAX);
    for (unsigned i = 0; i < a->hit_count; i++) a->hit_rows[i] = (struct du_item){.text = {a->topic==-2 ? a->document->pages[a->hits[i].topic].title : help_topics[a->hits[i].topic].title}};
    l->items = a->hit_rows; l->count = a->hit_count; l->active = 0; l->scroll = 0;
    if (!e->text[0]) snprintf(a->hit_status, sizeof(a->hit_status), "Type a word to search for.");
    else if (!a->hit_count) snprintf(a->hit_status, sizeof(a->hit_status), "No topics found.");
    else snprintf(a->hit_status, sizeof(a->hit_status), "%u topic%s found.", a->hit_count, a->hit_count == 1 ? "" : "s");
    status(a, a->hit_status);
    du_invalidate_widget(a->ui, H_SEARCH_LIST);
}

static const struct du_menu_item file_template[] = {
    {"&Back", H_M_BACK, DU_KEY_LEFT, DU_ALT, DU_ITEM_DISABLED}, {"", 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Open...", H_M_OPEN, 0x18, DU_CTRL, 0}, {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"E&xit", H_M_EXIT, 0, 0, 0} };
static const struct du_menu_item edit_template[] = {
    {"&Copy", H_M_COPY, 0x2e, DU_CTRL, 0}, {"Select &All", H_M_SELALL, 0x1e, DU_CTRL, 0} };
static const struct du_menu_item help_template[] = {
    {"&Using Help", H_M_HELP, DU_KEY_F1, 0, 0}, {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"&About CiukiOS Help", H_M_ABOUT, 0, 0, 0} };

static void resize_layout(struct du_ui *u, void *user)
{ (void)u; help_layout(user); }

int help_attach(struct help_app *a, struct du_ui *u)
{
    memset(a, 0, sizeof(*a));
    a->ui = u; u->event = help_event; u->user = a; u->layout = resize_layout;
    a->topic = -1;
    a->contents = calloc(HF_TOC + HELP_BOOKS_MAX, sizeof(*a->contents));
    a->rows = calloc(HF_TOC + HELP_BOOKS_MAX, sizeof(*a->rows));
    a->index = calloc(HELP_INDEX_MAX, sizeof(*a->index));
    a->index_rows = calloc(HELP_INDEX_MAX, sizeof(*a->index_rows));
    a->index_labels = calloc(HELP_INDEX_MAX, sizeof(*a->index_labels));
    if (!a->contents || !a->rows || !a->index || !a->index_rows || !a->index_labels || help_topic_count > HELP_TOPICS_MAX ||
        help_book_count() > HELP_BOOKS_MAX || du_ta_init(&a->ta, HELP_FILE_MAX)) { help_detach(a); return -ENOMEM; }
    a->ta.wrap = 1; a->ta.readonly = 1;
    memcpy(a->file_items, file_template, sizeof(file_template));
    memcpy(a->edit_items, edit_template, sizeof(edit_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&File", a->file_items, 5};
    a->menus[1] = (struct du_menu){"&Edit", a->edit_items, 2};
    a->menus[2] = (struct du_menu){"&Help", a->help_items, 3};
    u->menus = a->menus; u->menu_count = 3;
    du_help_topic(u, "help");
    struct du_rect zero = {0, 0, 0, 0};
    struct du_widget *w = du_add(u, DU_BUTTON, H_BACK, zero, "&Back");
    du_set_tip(w, "Back to the previous topic"); w->help = "Returns to the topic you read before this one."; w->disabled = 1;
    w = du_add(u, DU_TABS, H_TABS, zero, "");
    a->tabs[0].text[0] = "Contents"; a->tabs[1].text[0] = "Index"; a->tabs[2].text[0] = "Search";
    w->items = a->tabs; w->count = 3;
    w = du_add(u, DU_TREE, H_TREE, zero, "");
    w->inactive_selection = 1; w->help = "Books hold the topics. Click a topic to read it; press Enter or double-click a book to open it.";
    w = du_add(u, DU_EDIT, H_INDEX_EDIT, zero, "");
    w->help = "Type the first letters of a word to find it in the index list.";
    w = du_add(u, DU_LISTVIEW, H_INDEX_LIST, zero, "");
    w->view = DU_DETAILS; w->inactive_selection = 1; w->columns[0] = (struct du_column){"Index", LIST_W - 28, 0}; w->column_count = 1;
    w->help = "Click an entry to read its topic.";
    w = du_add(u, DU_EDIT, H_SEARCH_EDIT, zero, "");
    w->help = "Type the words to look for. A topic is listed when it contains every word.";
    w = du_add(u, DU_BUTTON, H_SEARCH_GO, zero, "List &Topics");
    du_set_tip(w, "Search all topics"); w->help = "Lists the topics that contain every word you typed.";
    w = du_add(u, DU_LISTVIEW, H_SEARCH_LIST, zero, "");
    w->view = DU_DETAILS; w->inactive_selection = 1; w->columns[0] = (struct du_column){"Topics found", LIST_W - 28, 0}; w->column_count = 1;
    w->help = "Click a topic to read it.";
    w = du_add(u, DU_TEXTAREA, H_TEXT, zero, "");
    w->data = &a->ta;
    du_add(u, DU_STATUS, H_STATUS, zero, "");
    du_add(u,DU_LABEL,H_LINK_LABEL,zero,"Related topics");
    w=du_add(u,DU_LISTVIEW,H_LINKS,zero,""); w->view=DU_DETAILS; w->columns[0]=(struct du_column){"Links",380,0}; w->column_count=1;
    du_set_tip(w, "Open a related topic"); w->help="Select a link and press Enter or double-click to open it. Back returns to the previous topic.";
    const int ids[]={H_WIZ_BACK,H_WIZ_NEXT,H_WIZ_CANCEL,H_WIZ_YES,H_WIZ_NO};
    const char *labels[]={"< &Back","&Next >","Cancel","&Yes","&No"};
    for (unsigned i=0;i<5;i++) { w=du_add(u,i>=3 ? DU_RADIO : DU_BUTTON,ids[i],zero,labels[i]); w->hidden=1; if (i>=3) w->total=1; }
    index_fill(a);
    help_layout(a);
    contents_rebuild(a);
    help_tab(a, HELP_TAB_CONTENTS);
    help_show_topic(a, 0);
    du_focus(u, H_TREE);
    return 0;
}
void help_detach(struct help_app *a)
{
    hf_close(a->document); a->document=NULL;
    du_ta_free(&a->ta);
    free(a->contents); free(a->rows); free(a->index); free(a->index_rows); free(a->index_labels);
    a->contents = NULL; a->rows = NULL; a->index = NULL; a->index_rows = NULL; a->index_labels = NULL;
}

static void copy_selection(struct help_app *a)
{
    size_t lo, hi; du_ta_selection(&a->ta, &lo, &hi);
    if (hi > lo) {
        char *copy = malloc(hi - lo + 1);
        if (copy) { memcpy(copy, a->ta.text + lo, hi - lo); copy[hi - lo] = 0; du_clipboard_set(copy); free(copy); }
    }
}

void help_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct help_app *a = user;
    switch (e->type) {
    case DU_EV_HELP:
        /* F1 inside Help opens "Using Help" here, not a second window. */
        if (e->index == DU_HELP_F1) { help_show_topic(a, help_topic_find("help")); u->help_consumed = 1; }
        break;
    case DU_EV_SELECT:
        if (e->id == H_TABS) help_tab(a, e->index);
        else if (e->id == H_TREE && e->index >= 0 && (unsigned)e->index < a->row_count) {
            if (a->rows[e->index].topic >= 0) { if (a->topic==-2) (void)help_document_show(a,(unsigned)a->rows[e->index].topic); else help_show_topic(a, a->rows[e->index].topic); }
        } else if (e->id == H_INDEX_LIST && e->index >= 0 && (unsigned)e->index < a->index_count) {
            if (a->topic==-2) (void)help_document_show(a,a->index[e->index].topic); else help_show_topic(a, (int)a->index[e->index].topic);
        } else if (e->id == H_SEARCH_LIST && e->index >= 0 && (unsigned)e->index < a->hit_count) {
            int hit = (int)a->hits[e->index].topic;
            if (a->topic==-2) (void)help_document_show(a,(unsigned)hit); else help_show_topic(a, hit); status(a, a->hit_status);
        }
        break;
    case DU_EV_OPEN:
        if (e->id==H_LINKS && e->index>=0 && (unsigned)e->index<a->link_count) {
            int error=help_follow(a,a->link_targets[e->index]);
            if (error) du_message(u,H_D_INFO,"CiukiOS Help",error==-ENOSYS ? "This help command or link is not supported." : "The requested help topic was not found.",DU_DLG_ICON_WARNING,NULL,0,0,0);
        } else if (e->id == H_TREE && e->index >= 0 && (unsigned)e->index < a->row_count) {
            if (a->rows[e->index].topic < 0 || a->contents[e->index].children) book_toggle(a, (unsigned)e->index);
            else if (a->topic==-2) (void)help_document_show(a,(unsigned)a->rows[e->index].topic);
            else help_show_topic(a, a->rows[e->index].topic);
        }
        break;
    case DU_EV_CHANGE:
        if (e->id==H_WIZ_YES || e->id==H_WIZ_NO) help_wizard_action(a,e->id);
        else if (e->id == H_TREE && e->index >= 0) book_toggle(a, (unsigned)e->index);
        else if (e->id == H_INDEX_EDIT) index_seek(a);
        break;
    case DU_EV_COMMIT:
        if (e->id == H_INDEX_EDIT) {
            struct du_widget *l = du_get(u, H_INDEX_LIST);
            if (l->count && l->active < a->index_count) { if (a->topic==-2) (void)help_document_show(a,a->index[l->active].topic); else help_show_topic(a, (int)a->index[l->active].topic); }
        } else if (e->id == H_SEARCH_EDIT) help_search_run(a);
        break;
    case DU_EV_DIALOG:
        if (e->id == H_D_OPEN && !e->index && e->text && e->text[0]) {
            int error=help_open_argument(a,e->text);
            if (error) du_message(u,H_D_INFO,"CiukiOS Help","The help file could not be opened.",DU_DLG_ICON_WARNING,NULL,0,0,0);
        }
        break;
    case DU_EV_ACTIVATE:
        if (e->id>=H_WIZ_BACK && e->id<=H_WIZ_NO) help_wizard_action(a,e->id);
        else if (e->id == H_M_OPEN) du_file_dialog(u, H_D_OPEN, 0, "Open", "/", "", du_posix_lister, NULL);
        else if (e->id == H_M_EXIT) { if (a->quit) a->quit(a->host); }
        else if (e->id == H_M_COPY) copy_selection(a);
        else if (e->id == H_M_SELALL) { du_ta_select_all(&a->ta); du_focus(u, H_TEXT); }
        else if (e->id == H_M_BACK || e->id == H_BACK) help_back(a);
        else if (e->id == H_M_HELP) help_show_topic(a, help_topic_find("help"));
        else if (e->id == H_SEARCH_GO) help_search_run(a);
        else if (e->id == H_M_ABOUT)
            du_message(u, H_D_INFO, "About CiukiOS Help", "CiukiOS Help - Classic computing, one woof at a time.\nContents, Index, Search, troubleshooters and Windows help files.\nViewer: MIT. CHM/LZX decoder: libmspack, LGPL 2.1. See /system/licenses for notices.",
                       DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        break;
    default: return;
    }
    du_invalidate_widget(u,H_TEXT);
}
