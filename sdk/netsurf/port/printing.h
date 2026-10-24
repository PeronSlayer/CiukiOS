/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BROWSER_PRINTING_H
#define CIUKI_BROWSER_PRINTING_H
#include <stdio.h>
#include "apps/libprint/print.h"
struct browser_window;
struct cp_ui;
struct du_ui;
void ciuki_print_document(struct cp_ui *,struct du_ui *,struct browser_window *);
int ciuki_print_render(void *,FILE *,unsigned,const struct cp_page *);
#endif
