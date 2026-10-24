/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "profile.h"
#include "apps/control/profile_lock.h"
#define HISTORY_MAX 2000
static char directory[512], paths[4][560];
static const char *names[]={"Choices","Bookmarks.html","History.html","Bookmarks.tmp"};
static char *history[HISTORY_MAX];
static uint64_t visits[HISTORY_MAX],pending_visits[HISTORY_MAX],import_time;
static unsigned history_days=20;
void ciuki_profile_history_days(unsigned days) { if(days<=999)history_days=days; }
static unsigned count;
static char *pending[HISTORY_MAX];
static unsigned pending_count;
static size_t pending_bytes;
static uint64_t profile_now(void) { time_t now=time(NULL);return now>0?(uint64_t)now:0; }
static int importing;
static size_t history_bytes;
static size_t history_serial_bytes;
static char clear_marker[96];
static char current_marker[96];
static void marker_read(void)
{
    char path[600];snprintf(path,sizeof(path),"%s/History.clear",directory);current_marker[0]=0;
    FILE *f=fopen(path,"r");if(f) { if(!fgets(current_marker,sizeof(current_marker),f))current_marker[0]=0;fclose(f); }
}
static void marker_sync(void)
{
    marker_read();if(!strcmp(clear_marker,current_marker))return;
    while(count)free(history[--count]);while(pending_count)free(pending[--pending_count]);
    pending_bytes=0;history_bytes=history_serial_bytes=0;strcpy(clear_marker,current_marker);
}
static size_t serial_cost(const char *s)
{
    size_t n=80;
    for(;*s;s++)n+=2*(*s=='&'?5:*s=='"'?6:(*s=='<' || *s=='>')?4:1);
    return n;
}
const char *ciuki_profile_path(const char *name)
{
    for(unsigned i=0;i<4;i++)if(!strcmp(name,names[i]))return paths[i];
    return NULL;
}
static int history_import(void);
int ciuki_profile_init(const char *path)
{
    if(!path || strlen(path)>=sizeof(directory)) { errno=ENAMETOOLONG;return -1; }
    strcpy(directory,path);
    if(mkdir(path,0777) && errno!=EEXIST)return -1;
    for(unsigned i=0;i<4;i++)snprintf(paths[i],sizeof(paths[i]),"%s/%s",path,names[i]);
    struct internet_profile_lock lock;
    if(internet_profile_lock(directory,&lock))return -1;
    marker_read();strcpy(clear_marker,current_marker);
    importing=1;int result=history_import();importing=0;
    internet_profile_unlock(&lock);return result;
}
static int history_import(void)
{
    /* History contains only escaped link URLs, one per line. Bounded import
     * rejects incomplete/corrupt records before replacing in-memory state. */
    if(ciuki_profile_admit(paths[2],1024*1024,HISTORY_MAX)) {
        if(errno==ENOENT)return 0;
        return -1;
    }
    FILE *f=fopen(paths[2],"r"); if(!f)return -1;
    char *line=malloc(65536);if(!line) { fclose(f);return -1; }
    while(fgets(line,65536,f)) {
        if(strncmp(line,"<p><a href=\"",12))continue;
        char *start=line+12,*end=strchr(start,'"');
        if(!end || (size_t)(end-start)>6*4096) { free(line);fclose(f);errno=EINVAL;return -1; }
        uint64_t now=profile_now();char *stamp=strstr(end,"data-visited=\"");
        if(stamp) { char *tail;import_time=strtoull(stamp+14,&tail,10);if(tail==stamp+14 || *tail!='"') { free(line);fclose(f);errno=EINVAL;return -1; } }
        else import_time=now;
        if(now && import_time>now)import_time=now;
        if(!history_days || (now>(uint64_t)history_days*86400 && import_time<now-(uint64_t)history_days*86400))continue;
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
        if(ciuki_profile_visit(start)) { free(line);fclose(f);return -1; }
    }
    int failed=ferror(f);free(line);if(fclose(f))failed=1;return failed?-1:0;
}
int ciuki_profile_admit(const char *path,size_t max,unsigned max_entries)
{
    struct stat st;
    if(stat(path,&st))return -1;
    if(st.st_size<0 || (size_t)st.st_size>max) { errno=EFBIG;return -1; }
    FILE *f=fopen(path,"r");if(!f)return -1;
    unsigned entries=0;char *line=malloc(65536);if(!line) { fclose(f);return -1; }
    if(!fgets(line,65536,f) || strncmp(line,"<!DOCTYPE html",14)) { free(line);fclose(f);errno=EINVAL;return -1; }
    while(fgets(line,65536,f)) {
        if(!strchr(line,'\n') && !feof(f)) { free(line);fclose(f);errno=EINVAL;return -1; }
        for(char *p=line;(p=strstr(p,"href="));p+=5)if(++entries>max_entries) { free(line);fclose(f);errno=EFBIG;return -1; }
    }
    int failed=ferror(f);free(line);if(fclose(f))failed=1;return failed?-1:0;
}
int ciuki_profile_replace(const char *name,int (*write_file)(const char *,void *),void *context)
{
    const char *path=ciuki_profile_path(name);char temp[600];
    if(!path) { errno=EINVAL;return -1; }
    snprintf(temp,sizeof(temp),"%s.XXXXXX",path);
    int reservation=mkstemp(temp);if(reservation<0)return -1;
    if(close(reservation)) { int e=errno;unlink(temp);errno=e;return -1; }
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
    if(!history_days)return 0;
    if(!importing)marker_sync();
    size_t length=strlen(url);
    if(length>4096) { errno=EFBIG;return -1; }

    char *copy=strdup(url);if(!copy)return -1;
    if(!importing) {
        char *delta=strdup(url);if(!delta) { free(copy);return -1; }
        while(pending_count && (pending_count==HISTORY_MAX || pending_bytes+length+1>128*1024)) { pending_bytes-=strlen(pending[0])+1;free(pending[0]);memmove(pending,pending+1,(HISTORY_MAX-1)*sizeof(*pending));memmove(pending_visits,pending_visits+1,(HISTORY_MAX-1)*sizeof(*pending_visits));pending_count--; }
        pending_visits[pending_count]=profile_now();pending[pending_count++]=delta;pending_bytes+=length+1;
    }
    for(unsigned i=0;i<count;i++)if(!strcmp(history[i],url)) {
        history_bytes-=strlen(history[i])+1;history_serial_bytes-=serial_cost(history[i]);free(history[i]);
        memmove(history+i,history+i+1,(count-i-1)*sizeof(*history));memmove(visits+i,visits+i+1,(count-i-1)*sizeof(*visits));count--;break;
    }
    size_t cost=serial_cost(url);
    while(count && (count==HISTORY_MAX || history_bytes+length+1>128*1024 || history_serial_bytes+cost>1024*1024-1024)) {
        history_serial_bytes-=serial_cost(history[0]);
        history_bytes-=strlen(history[0])+1;free(history[0]);
        memmove(history,history+1,(count-1)*sizeof(*history));memmove(visits,visits+1,(count-1)*sizeof(*visits));count--;
    }
    visits[count]=importing?import_time:profile_now();history[count++]=copy;history_bytes+=length+1;history_serial_bytes+=cost;return 0;
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
        err=fputs("<p><a href=\"",f)==EOF || escaped(f,history[i]) || fprintf(f,"\" data-visited=\"%llu\">",(unsigned long long)visits[i])<0 || escaped(f,history[i]) || fputs("</a></p>\n",f)==EOF;
    if(fclose(f))err=1;
    return err?-1:0;
}
int ciuki_profile_history_save(void)
{
    struct internet_profile_lock lock;if(internet_profile_lock(directory,&lock))return -1;
    marker_sync();
    while(count)free(history[--count]);history_bytes=history_serial_bytes=0;
    importing=1;int result=history_import();
    if(!result)for(unsigned i=0;i<pending_count;i++){ import_time=pending_visits[i];if(ciuki_profile_visit(pending[i])) { result=-1;break; } }
    importing=0;
    if(!result)result=ciuki_profile_replace("History.html",save,NULL);
    if(!result) { while(pending_count)free(pending[--pending_count]);pending_bytes=0; }
    int error=errno;internet_profile_unlock(&lock);errno=error;return result;
}
int ciuki_profile_transaction_begin(struct internet_profile_lock *lock)
{ return internet_profile_lock(directory,lock); }
void ciuki_profile_fini(void)
{
    while(count)free(history[--count]);while(pending_count)free(pending[--pending_count]);
    pending_bytes=0;history_bytes=history_serial_bytes=0;
}
