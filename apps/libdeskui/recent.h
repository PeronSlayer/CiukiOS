/* SPDX-License-Identifier: MIT
 * Recent documents: the last files opened by the desktop applications, most
 * recent first, as the Windows 98 Start menu's Documents list. One absolute
 * path per line in $HOME/recent.lst (HOME=/Documents from the desktop). The Start
 * menu (wt/desktop-shell) reads the same list with du_recent_load. */
#ifndef CIUKI_DESKUI_RECENT_H
#define CIUKI_DESKUI_RECENT_H
#include <stddef.h>
#define DU_RECENT_MAX 15
#define DU_RECENT_PATH 256
#define DU_RECENT_FILE "recent.lst"
struct du_recent { char paths[DU_RECENT_MAX][DU_RECENT_PATH]; unsigned count; };
/* Path of the list file ($HOME or /Documents); 0 or -1 when it does not fit. */
int du_recent_file(char *out, size_t cap);
/* Parses list text (newline separated); invalid lines are skipped. */
void du_recent_parse(struct du_recent *, const char *text, size_t length);
/* Moves (or inserts) path to the front; returns 0, or -1 for a path that is not absolute or too long. */
int du_recent_push(struct du_recent *, const char *path);
/* Serialises the list; returns the length written (always NUL terminated when cap > 0). */
size_t du_recent_format(const struct du_recent *, char *out, size_t cap);
/* File helpers: load (empty when missing) and add (load, push, write back). 0 or -errno. */
int du_recent_load(struct du_recent *);
int du_recent_add(const char *path);
int du_recent_clear(void);
#endif
