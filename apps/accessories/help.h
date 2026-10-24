/* SPDX-License-Identifier: MIT
 * CiukiOS Help (G15): Contents, Index and Search over the built-in topics (a
 * topic for the system and one for every built-in program), opened at a
 * program's topic by F1, Shift+F1 and the Help menus, plus a read-only text
 * viewer for any text file passed on the command line or opened from the File
 * menu. Windows 98 reference: Windows Help (RK-tools, RK-keys). */
#ifndef CIUKI_ACC_HELP_H
#define CIUKI_ACC_HELP_H
#include "acc.h"
#include "../libdeskui/textarea.h"
#include "helpfile.h"
#define HELP_W 624
#define HELP_H 400
#define HELP_FILE_MAX DU_TA_LIMIT
#define HELP_TOPICS_MAX 64
#define HELP_BOOKS_MAX 8
#define HELP_INDEX_MAX HF_KEYWORDS
#define HELP_HITS_MAX 48
#define HELP_BACK_MAX 16
#define HELP_KEYWORD_MAX 48
#define HELP_WIZARDS_MAX 16
/* id: the program name in /Programs for a program topic (what du_help_topic
 * and "help topic:<id>" use), else a lower-case word. book: the contents
 * book that holds the topic (NULL: a page at the top level); the topics of
 * one book are contiguous. keywords: index entries separated by ';'. text
 * starts with the title. */
struct help_topic { const char *id, *title, *book, *keywords, *text; };
extern const struct help_topic help_topics[];
extern const unsigned help_topic_count;

/* ---- the topic model (pure: no UI) ---- */
int help_topic_find(const char *id);                      /* index into help_topics, or -1 */
/* The topic id of an argument "topic:<id>"; -1 when it is not one or unknown. */
int help_topic_argument(const char *argument);
struct help_index_entry { char keyword[HELP_KEYWORD_MAX]; unsigned topic; };
/* Every keyword of every topic, sorted case-insensitively (then by topic);
 * returns the entry count. */
unsigned help_index_build(struct help_index_entry *out, unsigned capacity);
/* The first entry whose keyword starts with prefix (case-insensitive), or the
 * entry count when none does. */
unsigned help_index_seek(const struct help_index_entry *, unsigned count, const char *prefix);
struct help_hit { unsigned topic, score; };
/* Full-text search: every word (or "quoted phrase") of the query must start a
 * word of the topic's title, keywords or text; hits by score, best first.
 * Returns the hit count (0 for an empty query). */
unsigned help_search(const char *query, struct help_hit *hits, unsigned capacity);
/* Books in order of first appearance: the title of book b, or NULL. */
const char *help_book(unsigned book);
unsigned help_book_count(void);
int help_topic_book(unsigned topic);                      /* book index, -1 for a top-level page */

/* ---- the window ---- */
struct help_row { int topic, book; };                     /* contents row: topic >= 0, or a book */
struct help_app {
    struct du_ui *ui; struct du_textarea ta;
    struct du_item *contents; struct help_row *rows; unsigned row_count;
    uint8_t open_books[HELP_BOOKS_MAX];
    uint8_t open_toc[HF_TOC];
    struct help_index_entry *index; struct du_item *index_rows; char (*index_labels)[HELP_KEYWORD_MAX + 64]; unsigned index_count;
    struct help_hit hits[HELP_HITS_MAX]; struct du_item hit_rows[HELP_HITS_MAX]; unsigned hit_count; char hit_status[64];
    int topic;                                            /* -1: a file is shown */
    int back[HELP_BACK_MAX]; unsigned back_count;
    char path[256], note[160];
    struct hf_document *document; unsigned external_page; size_t external_offset;
    struct du_item link_rows[HF_LINKS]; char link_labels[HF_LINKS][160];
    char link_targets[HF_LINKS][256]; unsigned link_count;
    int wizard, wizard_step, wizard_answer;
    unsigned wizard_history[16], wizard_depth;
    void (*quit)(void *host); void *host;
    struct du_menu menus[3]; struct du_menu_item file_items[5], edit_items[2], help_items[3];
    struct du_item tabs[3];
};
enum { H_M_OPEN = 1, H_M_EXIT, H_M_COPY, H_M_SELALL, H_M_ABOUT, H_M_BACK, H_M_HELP,
       H_TABS = 100, H_TREE, H_INDEX_EDIT, H_INDEX_LIST, H_SEARCH_EDIT, H_SEARCH_GO, H_SEARCH_LIST, H_TEXT, H_STATUS, H_BACK,
       H_LINKS, H_LINK_LABEL, H_WIZARD, H_WIZ_BACK, H_WIZ_NEXT, H_WIZ_CANCEL, H_WIZ_YES, H_WIZ_NO,
       H_D_OPEN = 200, H_D_INFO };
enum { HELP_TAB_CONTENTS, HELP_TAB_INDEX, HELP_TAB_SEARCH };
int help_attach(struct help_app *, struct du_ui *);
void help_detach(struct help_app *);
void help_show_topic(struct help_app *, int topic);        /* also expands its book and records Back */
void help_back(struct help_app *);
void help_tab(struct help_app *, int tab);
void help_search_run(struct help_app *);                   /* the Search tab's query */
/* "topic:<id>" opens that topic, anything else is a text file path. */
int help_open_argument(struct help_app *, const char *argument);
/* Shows a text file read-only; 0 or -errno (-EFBIG over the text limit, -EINVAL for binary data). */
int help_open_bytes(struct help_app *, const char *path, const char *, size_t);
int help_open_file(struct help_app *, const char *path);
int help_load_text(struct help_app *, const char *data, size_t length, const char *title);
void help_event(struct du_ui *, const struct du_event *, void *);
void help_layout(struct help_app *);
int help_document_show(struct help_app *, unsigned page);
int help_document_part(struct help_app *, unsigned page, size_t offset);
int help_document_adopt(struct help_app *, struct hf_document *, unsigned page, const char *path);
int help_prepare_document(const char *, struct hf_document **, unsigned *page, char path[161]);
int help_follow(struct help_app *, const char *target);
int help_request_current(struct help_app *, const char *argument);
void help_request_ui(struct help_app *, const char *argument);
void help_links(struct help_app *);
void help_wizard_begin(struct help_app *, int kind);
void help_wizard_action(struct help_app *, int id);
const char *help_wizard_topic(unsigned kind);
#endif
