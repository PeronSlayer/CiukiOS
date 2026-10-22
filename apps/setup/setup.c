/* SPDX-License-Identifier: GPL-2.0-only
 * /Programs/setup: fullscreen libdeskui consumer of the supervisor's grants.
 * The real libsetup is supplied by f9-04. The fake is host-test-only. */
#include "native.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct st_app app;
static struct du_ui ui;
static uint32_t pixels[ST_W*ST_H];
static int descriptor(const char *s)
{
    if(!*s)return -1;
    unsigned n=0;for(;*s;s++){if(*s<'0'||*s>'9'||n>63)return -1;n=n*10+(unsigned)(*s-'0');}
    return n>=3&&n<64?(int)n:-1;
}
int main(int argc,char **argv)
{
    int display=3,input=4;
    for(int i=1;i<argc;i++){
        if(!strncmp(argv[i],"--display-fd=",13))display=descriptor(argv[i]+13);
        else if(!strncmp(argv[i],"--input-fd=",11))input=descriptor(argv[i]+11);
        else return 2;
    }
    if(display<0||input<0||display==input)return 2;
    struct st_host host;int r=st_native_open(&host,display,input);
    if(r){fprintf(stderr,"[setup] status=stopped reason=%s key=start error=%d\n",r==-ENOMEM?"memory":"display_or_assets",-r);return 1;}
    du_init(&ui,pixels,ST_W,ST_H);ui.focused_window=1;ui.mouse_x=ST_W/2;ui.mouse_y=ST_H/2;
    st_init(&app,&ui,&host);st_run(&app);st_free(&app);st_native_close();return 0;
}
