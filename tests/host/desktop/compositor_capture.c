/* Deterministic production-compositor fixture; never guest evidence.
 * SPDX-License-Identifier: MIT */
#include "fake.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv)
{
    assert(argc==4);
    struct desktop *d=calloc(1,sizeof(*d)); assert(d);
    desk_init(d,&fake_ops,640,480);
    d->now_ns=12345000000000ull; d->realtime_second=43210;
    d->mouse_x=320; d->mouse_y=240;
    uint32_t *portrait=malloc(256*256*4); assert(portrait);
    FILE *f=fopen(argv[1],"rb"); assert(f && fread(portrait,4,256*256,f)==256*256 && !fclose(f));
    d->portrait=portrait;
    uint32_t *guarded=calloc(640*480+2,4); assert(guarded);
    guarded[0]=guarded[640*480+1]=0xfeedface; d->pixels=guarded+1;
    if (!strcmp(argv[3],"shell")) {
        desk_shell_enable(d);
        strcpy(d->programs[0].name,"demo"); strcpy(d->programs[0].path,"/bin/demo");
        d->program_count=1; d->menu_open=1;
    }
    if (!strcmp(argv[3],"about")) {
        desk_shell_enable(d); strcpy(d->build_identity,"f2-30-host-preview"); desk_about(d);
    }
    desk_composite(d,(struct desk_box){0,0,640,480});
    f=fopen(argv[2],"wb"); assert(f && fwrite(d->pixels,4,640*480,f)==640*480 && !fclose(f));
    assert(guarded[0]==0xfeedface && guarded[640*480+1]==0xfeedface);
    free(guarded); free(portrait); free(d);
    return 0;
}
