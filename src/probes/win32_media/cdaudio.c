/* QEMU ide-cd exposes data tracks, not CD-DA: this fixture verifies the
 * production no-audio refusal and tray commands. Listening belongs to T23.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#include "../../../sdk/win32/audio/mci.h"
#include "controller.h"
extern void *W32API VirtualAlloc(void *,uint32_t,uint32_t,uint32_t);
extern void W32API ExitProcess(uint32_t);
extern uint32_t W32API mciSendStringA(const char *,char *,uint32_t,uintptr_t);
static struct w32_media_result *r;
#define CHECK(e) do { if (!(e)) { r->errors++; if (!r->first_error) r->first_error=__LINE__; } } while (0)
static uint32_t cmd(const char *s,char *out,uint32_t size)
{ return mciSendStringA(s,out,size,0); }
void W32API entry(void)
{
    r=VirtualAlloc((void *)(uintptr_t)W32_MEDIA_RESULT_VA,4096,0x3000,4);
    if (!r) ExitProcess(1);
    r->magic=W32_MEDIA_MAGIC; r->version=1;
    char s[64];
    CHECK(!cmd("sysinfo cdaudio quantity",s,sizeof(s)) && s[0]=='1'); r->cdaudio=!r->errors;
    CHECK(!cmd("open cdaudio alias cd",s,sizeof(s)));
    CHECK(!cmd("status cd media present",s,sizeof(s)) && s[0]=='t'); r->present=s[0]=='t';
    CHECK(!cmd("status cd number of tracks",s,sizeof(s)));
    for (const char *p=s;*p>='0' && *p<='9';p++) r->tracks=r->tracks*10+(uint32_t)(*p-'0');
    CHECK(r->tracks==1);
    CHECK(!cmd("status cd type track 1",s,sizeof(s)) && s[0]=='o'); r->data_only=s[0]=='o';
    CHECK(!cmd("set cd time format tmsf",0,0));
    r->range_refused=cmd("play cd from 1",0,0)==MCIERR_OUTOFRANGE; CHECK(r->range_refused);
    CHECK(!cmd("set cd door closed",0,0)); r->tray=!r->errors;
    CHECK(!cmd("close cd",0,0));
    __atomic_store_n(&r->done,1,__ATOMIC_RELEASE);
    for (unsigned i=0;i<1000 && !__atomic_load_n(&r->release,__ATOMIC_ACQUIRE);i++) Sleep(5);
    ExitProcess(r->errors ? 1 : 0);
}
