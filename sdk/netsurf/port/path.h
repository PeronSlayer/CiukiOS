/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_NETSURF_PATH_H
#define CIUKI_NETSURF_PATH_H
#include <stdbool.h>
#include <stdint.h>
#include <libnsfb.h>
#include "utils/errors.h"
#include "netsurf/plot_style.h"
/* fx-24: the framebuffer plotter table's path entry (SVG shapes). Upstream's
 * framebuffer front end logs "path unimplemented" and draws nothing, so SVG
 * images showed only their text. p holds NetSurf path commands
 * (PLOTTER_PATH_MOVE x y, LINE x y, BEZIER x1 y1 x2 y2 x y, CLOSE), mapped
 * through transform [a b c d e f]: x' = a x + c y + e, y' = b x + d y + f.
 * Like the GTK front end, colour NS_TRANSPARENT disables fill or stroke;
 * the style's fill/stroke types are not read (svg.c leaves them unset). */
nserror ciuki_plot_path(nsfb_t *nsfb, const plot_style_t *style, const float *p, unsigned int n,
                        const float transform[6]);
/* Statistics for host tests: paths, filled and stroked rasters. */
struct ciuki_path_stats { unsigned paths, fills, strokes, rejected; };
struct ciuki_path_stats ciuki_path_stats(void);
#endif
