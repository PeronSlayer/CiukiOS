/* Exact IS5 handoff text writers. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "private.h"
static unsigned length(const char *s){unsigned n=0;while(s[n])n++;return n;}
static int append(char *p,unsigned cap,unsigned *at,const char *s) {
    unsigned n=length(s);if(*at>=cap || n>=cap-*at)return 0;
    for(unsigned i=0;i<=n;i++)p[*at+i]=s[i];*at+=n;return 1;
}
int is5_join(char *p,const char *dir,const char *name) {
    unsigned at=0,n=length(dir);
    return append(p,IS5_PATH,&at,dir) && (!n || dir[n-1]=='\\' || dir[n-1]=='/' || append(p,IS5_PATH,&at,"\\")) && append(p,IS5_PATH,&at,name);
}
int is5_arguments(char *p,unsigned cap,const char *source,const char *temp,const char *ini,const char *user) {
    unsigned at=0;
    /* IS5's private -f/-x1 operands are concatenated, including spaces. */
    return append(p,cap,&at,"  -f") && append(p,cap,&at,source) &&
        append(p,cap,&at,"SETUP.INS  -z1 -cx -x") && append(p,cap,&at,temp) &&
        append(p,cap,&at," -x1") && append(p,cap,&at,source) && append(p,cap,&at," -q10010") &&
        (!*ini || (append(p,cap,&at," ") && append(p,cap,&at,ini))) &&
        (!*user || (append(p,cap,&at," ") && append(p,cap,&at,user)));
}
int is5_environment(char *p,unsigned cap,const char *env,unsigned bytes) {
    unsigned at=0,start=0;
    if(!cap)return 0;p[0]=0;
    while(start<bytes && env[start]) {
        unsigned end=start;while(end<bytes && env[end])end++;
        if(end==bytes || (at && !append(p,cap,&at," ")))return 0;
        if(!append(p,cap,&at,env+start))return 0;start=end+1;
    }
    return start<bytes && !env[start];
}
int is5_cleanup_text(char *p,unsigned cap,const char *engine,const char *dll0,const char *dll1) {
    unsigned at=0;
    return append(p,cap,&at,"[file0]\r\nmain=") && append(p,cap,&at,engine) &&
        append(p,cap,&at,"\r\nexeostype=1\r\nalt0=") && append(p,cap,&at,dll0) &&
        append(p,cap,&at,"\r\nalt1=") && append(p,cap,&at,dll1) && append(p,cap,&at,"\r\n");
}
const char *is5_tail(const char *p) {
    while(*p==' ' || *p=='\t')p++;
    if(*p=='"'){p++;while(*p && *p!='"')p++;if(*p)p++;}
    else while(*p && *p!=' ' && *p!='\t')p++;
    while(*p==' ' || *p=='\t')p++;return p;
}
