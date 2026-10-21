/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "profile.h"
#define HISTORY_MAX 2000
static char directory[512], paths[4][560];
static const char *names[]={"Choices","Bookmarks.html","History.html","Bookmarks.tmp"};
static char *history[HISTORY_MAX];
static unsigned count;
static size_t history_bytes;
static size_t history_serial_bytes;
static size_t serial_cost(const char *s)
{
    size_t n=40;
    for(;*s;s++)n+=2*(*s=='&'?5:*s=='"'?6:(*s=='<' || *s=='>')?4:1);
    return n;
}
const char *ciuki_profile_path(const char *name)
{
    for(unsigned i=0;i<4;i++)if(!strcmp(name,names[i]))return paths[i];
    return NULL;
}
int ciuki_profile_init(const char *path)
{
    if(!path || strlen(path)>=sizeof(directory)) { errno=ENAMETOOLONG;return -1; }
    strcpy(directory,path);
    if(mkdir(path,0777) && errno!=EEXIST)return -1;
    for(unsigned i=0;i<4;i++)snprintf(paths[i],sizeof(paths[i]),"%s/%s",path,names[i]);
    /* History contains only escaped link URLs, one per line. Bounded import
     * rejects incomplete/corrupt records before replacing in-memory state. */
    if(ciuki_profile_admit(paths[2],1024*1024,HISTORY_MAX)) {
        if(errno==ENOENT)return 0;
        return -1;
    }
    FILE *f=fopen(paths[2],"r"); if(!f)return -1;
    char line[65536];
    while(fgets(line,sizeof(line),f)) {
        if(strncmp(line,"<p><a href=\"",12))continue;
        char *start=line+12,*end=strchr(start,'"');
        if(!end || (size_t)(end-start)>6*4096) { fclose(f);ciuki_profile_fini();errno=EINVAL;return -1; }
        *end=0;
        /* Decode exactly the attribute escapes this writer emits. */
        char *read=start,*write=start;
        while(*read) {
            if(!strncmp(read,"&amp;",5)) {*write++='&';read+=5;}
            else if(!strncmp(read,"&quot;",6)) {*write++='"';read+=6;}
            else if(!strncmp(read,"&lt;",4)) {*write++='<';read+=4;}
            else if(!strncmp(read,"&gt;",4)) {*write++='>';read+=4;}
            else *write++=*read++;
        }
        *write=0;
        if(ciuki_profile_visit(start)) { fclose(f);return -1; }
    }
    int failed=ferror(f);fclose(f);return failed?-1:0;
}
int ciuki_profile_admit(const char *path,size_t max,unsigned max_entries)
{
    struct stat st;
    if(stat(path,&st))return -1;
    if(st.st_size<0 || (size_t)st.st_size>max) { errno=EFBIG;return -1; }
    FILE *f=fopen(path,"r");if(!f)return -1;
    unsigned entries=0;char line[65536];
    if(!fgets(line,sizeof(line),f) || strncmp(line,"<!DOCTYPE html",14)) { fclose(f);errno=EINVAL;return -1; }
    while(fgets(line,sizeof(line),f)) {
        if(!strchr(line,'\n') && !feof(f)) { fclose(f);errno=EINVAL;return -1; }
        for(char *p=line;(p=strstr(p,"href="));p+=5)if(++entries>max_entries) { fclose(f);errno=EFBIG;return -1; }
    }
    int failed=ferror(f);fclose(f);return failed?-1:0;
}
int ciuki_profile_replace(const char *name,int (*write_file)(const char *,void *),void *context)
{
    const char *path=ciuki_profile_path(name);char temp[600];
    if(!path) { errno=EINVAL;return -1; }
    snprintf(temp,sizeof(temp),"%s.new",path);
    if(write_file(temp,context)) { unlink(temp);return -1; }
    if((!strcmp(name,"Bookmarks.html") && ciuki_profile_admit(temp,512*1024,500)) ||
       (!strcmp(name,"History.html") && ciuki_profile_admit(temp,1024*1024,HISTORY_MAX))) { unlink(temp);return -1; }
    int fd=open(temp,O_RDWR);
    if(fd<0) { unlink(temp);return -1; }
    int result=fsync(fd),saved=errno;
    if(close(fd) && !result) { result=-1;saved=errno; }
    if(!result && rename(temp,path)) { result=-1;saved=errno; }
    if(result)unlink(temp);
    errno=saved;return result;
}
int ciuki_profile_visit(const char *url)
{
    size_t length=strlen(url);
    if(length>4096) { errno=EFBIG;return -1; }
    if(count && !strcmp(history[count-1],url))return 0;
    char *copy=strdup(url);if(!copy)return -1;
    size_t cost=serial_cost(url);
    while(count && (count==HISTORY_MAX || history_bytes+length+1>128*1024 || history_serial_bytes+cost>1024*1024-1024)) {
        history_serial_bytes-=serial_cost(history[0]);
        history_bytes-=strlen(history[0])+1;free(history[0]);
        memmove(history,history+1,(count-1)*sizeof(*history));count--;
    }
    history[count++]=copy;history_bytes+=length+1;history_serial_bytes+=cost;return 0;
}
static int escaped(FILE *f,const char *p)
{
    for(;*p;p++) {
        const char *entity=*p=='&'?"&amp;":*p=='<'?"&lt;":*p=='>'?"&gt;":*p=='"'?"&quot;":NULL;
        if(entity?fputs(entity,f)==EOF:fputc((unsigned char)*p,f)==EOF)return -1;
    }
    return 0;
}
static int save(const char *path,void *unused)
{
    (void)unused;
    FILE *f=fopen(path,"w");if(!f)return -1;
    int err=fputs("<!DOCTYPE html>\n<title>NetSurf History</title>\n<h1>History</h1>\n",f)==EOF;
    for(unsigned i=0;i<count && !err;i++)
        err=fputs("<p><a href=\"",f)==EOF || escaped(f,history[i]) || fputs("\">",f)==EOF || escaped(f,history[i]) || fputs("</a></p>\n",f)==EOF;
    if(fclose(f))err=1;
    return err?-1:0;
}
int ciuki_profile_history_save(void) { return ciuki_profile_replace("History.html",save,NULL); }
void ciuki_profile_fini(void) { while(count)free(history[--count]);history_bytes=history_serial_bytes=0; }
