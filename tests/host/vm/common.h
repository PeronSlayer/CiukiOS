/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/net_bridge.h>
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %s:%u: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static unsigned checks;
static void ipx_frame(uint8_t *p, const uint8_t *from, const uint8_t *to, unsigned n)
{
    memset(p,0,n); memcpy(p,to,6); memcpy(p+6,from,6);
    p[12]=0x81; p[13]=0x37; p[14]=p[15]=255;
    p[16]=(n-14)>>8; p[17]=(uint8_t)(n-14);
    memcpy(p+24,to,6); memcpy(p+36,from,6);
    p[30]=p[42]=0x86; p[31]=p[43]=0x9c;
}
