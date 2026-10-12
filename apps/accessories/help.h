/* SPDX-License-Identifier: MIT
 * CiukiOS Help: built-in help topics (the 0.8 command guide and shortcut
 * lists, the README and the credits) and a read-only text viewer for any
 * text file passed on the command line or opened from the File menu. */
#ifndef CIUKI_ACC_HELP_H
#define CIUKI_ACC_HELP_H
#include "acc.h"
#include "../libdeskui/textarea.h"
#define HELP_W 624
#define HELP_H 400
#define HELP_FILE_MAX DU_TA_LIMIT
struct help_topic { const char *title, *text; };
extern const struct help_topic help_topics[];
extern const unsigned help_topic_count;
struct help_app {
    struct du_ui *ui; struct du_textarea ta;
    struct du_item items[24]; int topic;          /* -1: a file is shown */
    char path[256], note[160];
    void (*quit)(void *host); void *host;
    struct du_menu menus[3]; struct du_menu_item file_items[3], edit_items[2], help_items[1];
};
enum { H_M_OPEN = 1, H_M_EXIT, H_M_COPY, H_M_SELALL, H_M_ABOUT, H_TOPICS = 100, H_TEXT, H_STATUS, H_D_OPEN = 200, H_D_INFO };
int help_attach(struct help_app *, struct du_ui *);
void help_detach(struct help_app *);
void help_show_topic(struct help_app *, int topic);
/* Shows a text file read-only; 0 or -errno (-EFBIG over the text limit, -EINVAL for binary data). */
int help_open_file(struct help_app *, const char *path);
int help_load_text(struct help_app *, const char *data, size_t length, const char *title);
void help_event(struct du_ui *, const struct du_event *, void *);
void help_layout(struct help_app *);
#endif
