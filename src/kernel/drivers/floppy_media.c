/* Standard PC floppy geometry, from physical media formats and 82077 rates.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "floppy.h"
bool fdc_geometry(unsigned type, struct fdc_geometry *g)
{
    if (!g) return false;
    *g=(struct fdc_geometry){80,2,18,0,0x1b,0x6c,4,false};
    switch (type) {
    case 1: g->cylinders=40; g->sectors=9; g->rate=2; g->gap=0x2a; g->format_gap=0x50; break;
    case 2: g->sectors=15; g->format_gap=0x54; break;
    case 3: g->sectors=9; g->rate=2; g->gap=0x2a; g->format_gap=0x50; break;
    case 4: break;
    case 5: g->sectors=36; g->rate=3; g->format_gap=0x53; g->perpendicular=true; break;
    default: return false;
    }
    g->type=type; return true;
}
