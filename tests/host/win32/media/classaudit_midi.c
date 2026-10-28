/* Exercise the production SMF loader with period format-0 header quirks.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../../../../sdk/win32/audio/mciseq.c"
static uint8_t song[]={ 'M','T','h','d',0,0,0,6,0,0,0,1,0,96,
                        'M','T','r','k',0,0,0,4,0,0xff,0x2f,0 };
static unsigned diagnostics;
void *wa_alloc(uint32_t n) { return calloc(1,n); }
void wa_free(void *p) { free(p); }
DWORD GetFileSize(HANDLE h,DWORD *high) { (void)h; *high=0; return sizeof(song); }
DWORD SetFilePointer(HANDLE h,LONG off,LONG *high,DWORD how)
{ (void)h; (void)high; (void)how; assert(!off); return 0; }
BOOL ReadFile(HANDLE h,void *p,DWORD n,DWORD *done,OVERLAPPED *overlap)
{ (void)h; (void)overlap; assert(n==sizeof(song)); memcpy(p,song,n); *done=n; return 1; }
void OutputDebugStringA(const char *s) { assert(strstr(s,"format0_track_count")); diagnostics++; }
int main(void)
{
    const unsigned counts[]={0,1,2,0xffff};
    static struct seq s;
    for (unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);i++) {
        memset(&s,0,sizeof(s)); song[10]=(uint8_t)(counts[i]>>8); song[11]=(uint8_t)counts[i];
        assert(!load(&s,(HANDLE)1) && s.tracks==1 && s.format==0); wa_free(s.file);
    }
    assert(diagnostics==3);
    memset(&s,0,sizeof(s)); song[9]=1; song[10]=song[11]=0;
    assert(load(&s,(HANDLE)1)==MCIERR_INVALID_FILE); wa_free(s.file);
    song[9]=0; song[11]=2; song[21]=32; memset(&s,0,sizeof(s));
    assert(load(&s,(HANDLE)1)==MCIERR_INVALID_FILE); wa_free(s.file);
    song[21]=4; song[13]=0; memset(&s,0,sizeof(s));
    assert(load(&s,(HANDLE)1)==MCIERR_INVALID_FILE); wa_free(s.file);
    puts("PASS MIDI format-0 track-count normalization; format-1, track payload and division bounds retained");
    return 0;
}
