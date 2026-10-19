/* SPDX-License-Identifier: MIT
 * Compile with -O2; CPU time includes the production scalar adapter and I/O. */
#include "../../../apps/music/decoder.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double cpu(void)
{ struct timespec t; assert(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)); return t.tv_sec+t.tv_nsec/1e9; }
int main(int argc,char **argv)
{
    assert(argc==2 || argc==3);
    unsigned iterations=argc==3 ? (unsigned)strtoul(argv[2],NULL,10) : 200;
    unsigned long long total=0; int16_t pcm[1152*2];
    double start=cpu();
    for (unsigned i=0;i<iterations;i++) {
        struct music_decoder *d=NULL; assert(!music_decoder_open(&d,argv[1],48000,NULL,NULL));
        assert(music_decoder_rate(d)>0);
        int n; while ((n=music_decoder_read(d,pcm,1152))>0) total+=(unsigned)n;
        assert(!n); music_decoder_close(d);
    }
    double elapsed=cpu()-start;
    printf("scalar adapter -O2: %.3f us/1152-sample MP3 frame; %.6f us/output PCM frame (%llu samples)\n",
        elapsed*1e6/(total/1152.0),elapsed*1e6/total,total);
}
