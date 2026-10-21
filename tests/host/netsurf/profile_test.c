/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sdk/netsurf/port/profile.h"
static int writer(const char *path,void *context)
{
    FILE *out=fopen(path,"w");assert(out);fputs(context,out);assert(!fclose(out));return 0;
}
static int failure(const char *path,void *unused)
{ (void)unused;writer(path,"partial");errno=ENOSPC;return -1; }
int main(int argc,char **argv)
{
    assert(argc==2 && !ciuki_profile_init(argv[1]));
    assert(!ciuki_profile_replace("Choices",writer,"enable_javascript:false\n"));
    assert(ciuki_profile_replace("Choices",failure,NULL)==-1 && errno==ENOSPC);
    char data[64]={0};FILE *f=fopen(ciuki_profile_path("Choices"),"r");assert(f);assert(fgets(data,sizeof(data),f));fclose(f);
    assert(!strcmp(data,"enable_javascript:false\n"));
    for(unsigned i=0;i<2005;i++) { char url[96];snprintf(url,sizeof(url),"file:///Documents/%u.html?a=1&b=2",i);assert(!ciuki_profile_visit(url)); }
    assert(!ciuki_profile_history_save());
    assert(!ciuki_profile_admit(ciuki_profile_path("History.html"),1024*1024,2000));
    assert(ciuki_profile_admit(ciuki_profile_path("History.html"),1024*1024,1999)==-1 && errno==EFBIG);
    ciuki_profile_fini();assert(!ciuki_profile_init(argv[1]));assert(!ciuki_profile_history_save());
    f=fopen(ciuki_profile_path("History.html"),"r");assert(f);unsigned links=0;char line[1024];
    while(fgets(line,sizeof(line),f))if(strstr(line,"href=")) { links++;assert(!strstr(line,"/0.html"));assert(strstr(line,"&amp;b=2")); }
    fclose(f);assert(links==2000);
    assert(!unlink(ciuki_profile_path("Choices")));assert(!mkdir(ciuki_profile_path("Choices"),0777));
    assert(ciuki_profile_replace("Choices",writer,"new")==-1); /* failed rename keeps destination */
    assert(!rmdir(ciuki_profile_path("Choices")));
    for(unsigned i=0;i<100;i++) {
        char long_url[4097];memset(long_url,'&',4096);long_url[0]='A'+i%26;long_url[4096]=0;
        assert(!ciuki_profile_visit(long_url));
    }
    assert(!ciuki_profile_history_save());assert(!ciuki_profile_admit(ciuki_profile_path("History.html"),1024*1024,2000));
    ciuki_profile_fini();assert(!ciuki_profile_init(argv[1]));
    assert(!writer(ciuki_profile_path("History.html"),"<p><a href=\"broken\n"));
    ciuki_profile_fini();assert(ciuki_profile_init(argv[1])==-1 && errno==EINVAL);
    ciuki_profile_fini();puts("NetSurf profile: retention, restart, escaping, corrupt input and atomic ENOSPC/rename failure passed");fflush(stdout);return 0;
}
