/* SPDX-License-Identifier: MIT */
#define FOOTPRINT_ALLOC_IMPLEMENTATION
#include "footprint_alloc.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static const char *phase;
static unsigned used;
static const char *fail_file;
static struct allocation { size_t bytes; const char *file, *function, *kind; unsigned line; } records[512];
static int record(size_t bytes,const char *file,unsigned line,const char *function,const char *kind)
{
    pthread_mutex_lock(&lock);
    if (phase) {
        assert(used<sizeof(records)/sizeof(*records));
        records[used++]=(struct allocation){bytes,file,function,kind,line};
    }
    int fail=fail_file && strstr(file,fail_file);
    if (fail) fail_file=NULL;
    pthread_mutex_unlock(&lock); return fail;
}
void *footprint_malloc(size_t n,const char *file,unsigned line,const char *function)
{ return record(n,file,line,function,"malloc") ? NULL : malloc(n); }
void *footprint_calloc(size_t n,size_t s,const char *file,unsigned line,const char *function)
{ return record(n*s,file,line,function,"calloc") ? NULL : calloc(n,s); }
void *footprint_realloc(void *p,size_t n,const char *file,unsigned line,const char *function)
{ return record(n,file,line,function,"realloc") ? NULL : realloc(p,n); }
char *footprint_strdup(const char *s,const char *file,unsigned line,const char *function)
{
    size_t n=strlen(s)+1;
    char *p=record(n,file,line,function,"strdup") ? NULL : malloc(n);
    if (p) memcpy(p,s,n);
    return p;
}
void footprint_fail_once(const char *file)
{ pthread_mutex_lock(&lock); assert(!fail_file); fail_file=file; pthread_mutex_unlock(&lock); }
void footprint_begin(const char *name)
{ pthread_mutex_lock(&lock); assert(!phase); phase=name; used=0; pthread_mutex_unlock(&lock); }
unsigned footprint_end(void)
{
    pthread_mutex_lock(&lock);
    assert(phase);
    for (unsigned i=0;i<used;i++) {
        const struct allocation *a=records+i;
        const char *path=strstr(a->file,"apps/");
        printf("ALLOC phase=%s bytes=%zu kind=%s site=%s:%u function=%s\n",
               phase,a->bytes,a->kind,path ? path : a->file,a->line,a->function);
    }
    printf("ALLOC_TOTAL phase=%s count=%u\n",phase,used);
    unsigned count=used; phase=NULL;
    pthread_mutex_unlock(&lock); return count;
}
