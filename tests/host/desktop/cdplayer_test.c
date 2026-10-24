/* G22 CD front-end controller. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../apps/desktop/cdplayer_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char last[128]; static unsigned plays; static int failed;
static uint32_t command(void *ctx,const char *s,char *out,uint32_t cap)
{
    (void)ctx; snprintf(last,sizeof(last),"%s",s);
    if (failed) return 1;
    if (!strcmp(s,"status cd number of tracks")) { snprintf(out,cap,"4"); return 0; }
    unsigned track=0;
    if (sscanf(s,"status cd type track %u",&track)==1) {
        snprintf(out,cap,track==2 ? "other" : "audio"); return 0;
    }
    if (sscanf(s,"status cd length track %u",&track)==1) {
        snprintf(out,cap,"03:14:00"); return 0;
    }
    if (!strncmp(s,"play cd from ",13)) { plays++; return 0; }
    assert(0); return 1;
}
int main(void)
{
    struct cdp_player p={.command=command,.seed=1};
    assert(!cdp_load(&p) && p.tracks==4 && p.track==1 && !p.audio[2]);
    char label[64]; cdp_label(&p,1,label); assert(!strcmp(label,"Track 01  03:14:00"));
    cdp_label(&p,2,label); assert(!strcmp(label,"Track 02  Data"));
    assert(!cdp_play(&p) && !strcmp(last,"play cd from 01 to 01:03:14:00 notify"));
    assert(cdp_advance(&p,1,1) && p.track==3);
    assert(cdp_advance(&p,1,1) && p.track==4);
    assert(!cdp_advance(&p,1,1) && p.track==4);
    p.repeat=1; assert(cdp_advance(&p,1,1) && p.track==1);
    assert(cdp_advance(&p,-1,0) && p.track==4);
    p.track=2; unsigned before=plays; assert(cdp_play(&p) && plays==before);
    assert(!cdp_load(&p)); p.repeat=0; p.random=1;
    assert(!cdp_play(&p));
    unsigned seen=1u<<1;
    while(cdp_advance(&p,1,1)) {
        assert(p.track!=2 && !(seen&(1u<<p.track))); seen|=1u<<p.track; assert(!cdp_play(&p));
    }
    assert(seen==((1u<<1)|(1u<<3)|(1u<<4)));
    p.repeat=1; assert(cdp_advance(&p,1,1) && p.audio[p.track]);
    failed=1; assert(cdp_load(&p) && !p.tracks && !p.track && cdp_play(&p));
    puts("cdplayer: PASS track list/time, data-track exclusion, bounded PLAY, next/previous, repeat, random without repeats, failed refresh");
}
