/* SPDX-License-Identifier: MIT */
#ifndef CIUKIDE_APP_H
#define CIUKIDE_APP_H
#include "job.h"
#include "../ciuknote/doc.h"
#include "../libdeskui/textarea.h"
enum { IDE_NEW_WINDOW=1, IDE_NEW_CONSOLE, IDE_OPEN, IDE_SAVE, IDE_BUILD, IDE_RUN, IDE_GUIDE,
       IDE_UNDO, IDE_CUT, IDE_COPY, IDE_PASTE, IDE_ALL, IDE_EDITOR=100, IDE_FILES, IDE_ERROR_LIST,
       IDE_OUTPUT, IDE_STATUS, IDE_NEW_DIALOG=200, IDE_OPEN_DIALOG, IDE_MESSAGE };
struct ide_app {
    struct du_ui *ui;
    const struct cn_fs *fs;
    const struct ide_process_ops *process;
    void *process_ctx;
    int (*launch)(void *, const char *path); void *launch_ctx;
    struct ide_model model; struct ide_job job;
    struct du_textarea editor, output;
    char folder[IDE_NAME], file[IDE_NAME]; int crlf, template_window;
    unsigned char syntax[CN_LIMIT];
    struct du_item files[IDE_SOURCES+1], errors[IDE_ERRORS];
    char labels[IDE_ERRORS][IDE_LINE];
    struct du_menu menus[3]; struct du_menu_item project_menu[8], edit_menu[5], help_menu[1];
};
int ide_attach(struct ide_app *, struct du_ui *);
void ide_detach(struct ide_app *);
void ide_adopt_project(struct ide_app *, struct ide_app *loaded);
int ide_open_project(struct ide_app *, const char *folder);
int ide_save(struct ide_app *);
int ide_new_project(struct ide_app *, const char *folder, int window);
void ide_event(struct du_ui *, const struct du_event *, void *);
void ide_refresh(struct ide_app *);
void ide_idle(struct ide_app *);
#endif
