/* SPDX-License-Identifier: MIT
 * EDID 1.x base block decoder. Monitor capabilities are not scanout state.
 * Caller supplies a zeroed versioned record. No firmware pointers escape. */
#ifndef CIUKI_EDID_H
#define CIUKI_EDID_H
#include <ciuki/abi.h>
static inline int ciuki_edid_decode(const uint8_t *e,struct ciuki_monitor_info *out)
{
    static const uint8_t magic[8]={0,255,255,255,255,255,255,0};
    unsigned sum=0;
    for(unsigned i=0;i<128;i++) sum+=e[i];
    for(unsigned i=0;i<8;i++) if(e[i]!=magic[i]) return 0;
    if((sum&255) || e[18]!=1 || e[19]>4) return 0;
    unsigned manufacturer=((unsigned)e[8]<<8)|e[9];
    for(unsigned i=0;i<3;i++) {
        unsigned letter=(manufacturer>>(10-i*5))&31;
        if(!letter || letter>26) return 0;
        out->manufacturer[i]=(char)('A'+letter-1);
    }
    out->manufacturer[3]=0;
    out->product=e[10]|(uint32_t)e[11]<<8;
    out->serial=e[12]|(uint32_t)e[13]<<8|(uint32_t)e[14]<<16|(uint32_t)e[15]<<24;
    out->manufacture_week=e[16]; out->manufacture_year=1990+e[17];
    out->timing_count=0;
    for(unsigned block=0;block<4;block++) {
        const uint8_t *d=e+54+block*18;
        unsigned clock=(d[0]|(unsigned)d[1]<<8)*10;
        if(!clock) {
            if(d[2] || d[4]) continue;
            if(d[3]==0xfc) {
                unsigned n=0;
                for(unsigned i=5;i<18 && d[i]!=10;i++) {
                    if(d[i]<32 || d[i]>126) break;
                    out->name[n++]=(char)d[i];
                }
                while(n && out->name[n-1]==' ') n--;
                out->name[n]=0;
            } else if(d[3]==0xfd && d[5] && d[6]>=d[5]) { out->minimum_hz=d[5]; out->maximum_hz=d[6]; }
            continue;
        }
        unsigned w=d[2]|(unsigned)(d[4]&0xf0)<<4, hb=d[3]|(unsigned)(d[4]&15)<<8;
        unsigned h=d[5]|(unsigned)(d[7]&0xf0)<<4, vb=d[6]|(unsigned)(d[7]&15)<<8;
        if(!w || !h || !hb || !vb) continue;
        uint32_t flags=(block==0 && (e[24]&2) ? CIUKI_TIMING_PREFERRED : 0) |
                       (d[17]&128 ? CIUKI_TIMING_INTERLACED : 0);
        /* EDID interlace DTD vertical active/blanking are per field. */
        unsigned vt=(h+vb)*(flags&CIUKI_TIMING_INTERLACED ? 2u : 1u)+(flags&CIUKI_TIMING_INTERLACED ? 1u : 0u);
        unsigned height=h*(flags&CIUKI_TIMING_INTERLACED ? 2u : 1u);
        uint64_t numerator=(uint64_t)clock*1000000u*(flags&CIUKI_TIMING_INTERLACED ? 2u : 1u);
        uint32_t rate=(uint32_t)((numerator+(uint64_t)(w+hb)*vt/2)/((uint64_t)(w+hb)*vt));
        out->timings[out->timing_count++]=(struct ciuki_monitor_timing){w,height,clock,w+hb,vt,rate,flags};
    }
    for(unsigned i=0;i<128;i++) out->edid[i]=e[i];
    out->flags=CIUKI_MONITOR_EDID_VALID|CIUKI_MONITOR_DDC;
    return 1;
}
#endif
