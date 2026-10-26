/* Private GL/provider admission arithmetic. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32_GL_ADMISSION_H
#define W32_GL_ADMISSION_H
#include "backend.h"
static inline int gl_hw_format_supported(const struct gl_hw_format *f) {
  return !f->rgb565 && f->depth_bits==16 && !f->stencil_bits;
}
/* Same measure as render3d.c validate(): sum ceil(span X)*ceil(span Y),
 * not raster coverage. Q4 -> Q16 conversion and Y reflection preserve spans. */
static inline uint64_t gl_hw_triangle_work(const struct sw_vertex *v) {
  int xmin=v[0].x,xmax=xmin,ymin=v[0].y,ymax=ymin;
  for (unsigned i=1;i<3;i++) {
    if (v[i].x<xmin) xmin=v[i].x;
    if (v[i].x>xmax) xmax=v[i].x;
    if (v[i].y<ymin) ymin=v[i].y;
    if (v[i].y>ymax) ymax=v[i].y;
  }
  return (uint64_t)(((int64_t)xmax-xmin+15)/16)*
                   (((int64_t)ymax-ymin+15)/16);
}
static inline int gl_hw_batch_room(const struct gl_hw_caps *caps,
    unsigned count,uint64_t work,uint64_t extra) {
  return count<=caps->max_vertices && caps->max_vertices-count>=3 &&
    work<=caps->max_pixel_work && extra<=caps->max_pixel_work-work;
}
#endif
