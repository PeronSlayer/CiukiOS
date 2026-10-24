/* SPDX-License-Identifier: MIT
 * The fonts of the application build (libfont, the installed Liberation faces). */
#ifndef CIUKI_WORDPAD_FONTS_H
#define CIUKI_WORDPAD_FONTS_H
#include "app.h"
const struct wp_draw_ops *wp_fonts(void);
/* Host tests: the folder (with a trailing slash) that holds the Liberation faces; default /System/fonts/. */
void wp_fonts_directory(const char *dir);
#endif
