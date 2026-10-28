/* SPDX-License-Identifier: MIT
 * Settings-worker-only libreg instance. No registry grammar or transaction
 * implementation is duplicated: compile the SDK units with a bounded arena.
 * Workspace exhaustion returns ENOMEM through the existing unsaved result. */
#define _POSIX_C_SOURCE 200809L
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include "master_registry.h"
#include "sound_storage.h"

#define MASTER_WORK_BYTES (4u * 1024u * 1024u)
union master_block { struct { size_t bytes; int available; } h; max_align_t alignment; };
static union { max_align_t alignment; unsigned char bytes[MASTER_WORK_BYTES]; } workspace;
static pthread_mutex_t workspace_lock=PTHREAD_MUTEX_INITIALIZER;
static union master_block *next_block(union master_block *b)
{
    unsigned char *next=(unsigned char *)(b+1)+b->h.bytes;
    return next<workspace.bytes+sizeof(workspace.bytes) ? (union master_block *)next : NULL;
}
static void *master_alloc(size_t n)
{
    size_t align=_Alignof(max_align_t);
    if (!n) n=1;
    if (n>MASTER_WORK_BYTES-align) { errno=ENOMEM; return NULL; }
    n=(n+align-1)/align*align;
    for (union master_block *b=(void *)workspace.bytes;b;b=next_block(b)) {
        if (!b->h.available || b->h.bytes<n) continue;
        if (b->h.bytes>=n+sizeof(*b)+align) {
            union master_block *tail=(void *)((unsigned char *)(b+1)+n);
            tail->h.bytes=b->h.bytes-n-sizeof(*b); tail->h.available=1; b->h.bytes=n;
        }
        b->h.available=0; return b+1;
    }
    errno=ENOMEM; return NULL;
}
static void master_free(void *p)
{
    if (!p) return;
    ((union master_block *)p-1)->h.available=1;
    for (union master_block *b=(void *)workspace.bytes;b;) {
        union master_block *next=next_block(b);
        if (next && b->h.available && next->h.available) b->h.bytes+=sizeof(*b)+next->h.bytes;
        else b=next;
    }
}
static void *master_calloc(size_t count,size_t size)
{
    if (size && count>SIZE_MAX/size) { errno=ENOMEM; return NULL; }
    void *p=master_alloc(count*size); if (p) memset(p,0,count*size); return p;
}
static void *master_realloc(void *p,size_t n)
{
    if (!p) return master_alloc(n);
    if (!n) { master_free(p); return NULL; }
    size_t old=((union master_block *)p-1)->h.bytes;
    if (old>=n) return p;
    void *next=master_alloc(n);
    if (next) { memcpy(next,p,old); master_free(p); }
    return next;
}
static char *master_strdup(const char *s)
{ size_t n=strlen(s)+1; char *p=master_alloc(n); if (p) memcpy(p,s,n); return p; }

#undef malloc
#undef calloc
#undef realloc
#undef strdup
#define malloc master_alloc
#define calloc master_calloc
#define realloc master_realloc
#define free master_free
#define strdup master_strdup
/* Newlib's feature header raises this with _DEFAULT_SOURCE. The included
 * SDK units each declare their original POSIX baseline before their headers. */
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "../../sdk/libreg/registry.c"
#include "../../sdk/libreg/storage.c"
/* The private instance is only opened on the persistent System root. */
struct rt *reg_hardware(uint64_t previous)
{ (void)previous; errno=ENODEV; return NULL; }

/* sec-user-05 (user-security-contract.md K-Q8): mixer levels are a session
 * preference. A profile session saves them in its own personal Desktop root
 * (CIUKI_PROFILE_SETTINGS, owner-writable, under the committed profile) and
 * the desktop applies them again when the session starts. The machine
 * default (System\Audio, read by the kernel at boot, and SYSTEM/AUDIO.CFG) is
 * never written by an ordinary session; only a profile-less bootstrap
 * desktop keeps the machine root, which the registry broker admits for
 * administrators alone. */
#define PERSONAL_AUDIO_KEY "Desktop\\Audio"
static const char *personal_settings(void)
{
    const char *s=getenv("CIUKI_PROFILE_SETTINGS");
    return s && s[0]=='/' && strlen(s)<200 ? s : NULL;
}
static struct reg_root *preference_root(const char *directory,const char **key)
{
    const char *personal=personal_settings();
    *key=personal ? PERSONAL_AUDIO_KEY : CIUKI_MASTER_KEY;
    return personal ? reg_open_root_at("Desktop",personal) : reg_open_root_at("System",directory);
}
static void workspace_reset(void)
{
    union master_block *first=(void *)workspace.bytes;
    first->h.bytes=sizeof(workspace.bytes)-sizeof(*first); first->h.available=1;
}
int master_preference_save(const char *directory,unsigned left,unsigned right,unsigned mute)
{
    if (left>100 || right>100 || mute>1) { errno=EINVAL; return -1; }
    int error=pthread_mutex_lock(&workspace_lock);
    if (error) { errno=error; return -1; }
    workspace_reset();
    const char *key;
    struct reg_root *r=preference_root(directory,&key);
    int result=r ? reg_set_int32(r,key,CIUKI_MASTER_VALUE,ciuki_master_pack(left,right,mute)) : -1;
    error=errno; reg_close_root(r);
    pthread_mutex_unlock(&workspace_lock); errno=error; return result;
}

/* Each source preference is one registry transaction, using the same bounded
 * private libreg instance as the master. No partially updated stereo pair. */
#include <ciuki/abi.h>
static int line_value_name(char key[16],unsigned line,unsigned recording)
{
    if (recording) snprintf(key,16,"Recording");
    else snprintf(key,16,"Source%u",line);
    return 0;
}
int line_preference_save(const char *directory,const struct ciuki_mixer_line *line)
{
    if (!line || line->line<1 || line->line>5 || line->recording>1 || (line->recording && line->line==CIUKI_MIXER_MIDI) || line->left>100 || line->right>100 || line->mute>1 || line->selected>1) { errno=EINVAL; return -1; }
    int error=pthread_mutex_lock(&workspace_lock);
    if (error) { errno=error; return -1; }
    workspace_reset();
    char name[16]; line_value_name(name,line->line,line->recording);
    int32_t value=ciuki_master_pack(line->left,line->right,line->mute) | (line->recording ? (int32_t)(line->line<<24) : 0);
    const char *key;
    struct reg_root *r=preference_root(directory,&key);
    int result=r ? reg_set_int32(r,key,name,value) : -1;
    error=errno; reg_close_root(r);
    pthread_mutex_unlock(&workspace_lock); errno=error; return result;
}
/* sec-user-05: the session's saved levels, applied through the desktop's
 * designated mixer control fd (session scope, never a machine save). Absent
 * values keep the current machine default. Returns the number of levels
 * applied, or -1 with errno when the personal root cannot be read. */
int mixer_preferences_restore(int fd,int (*apply)(int,unsigned,void *,size_t))
{
    const char *personal=personal_settings();
    if (!personal || fd<0 || !apply) return 0;
    int error=pthread_mutex_lock(&workspace_lock);
    if (error) { errno=error; return -1; }
    workspace_reset();
    struct reg_root *r=reg_open_root_at("Desktop",personal);
    int applied=0; error=r ? 0 : errno;
    int32_t packed; unsigned left, right, mute;
    if (r && !reg_get_int32(r,PERSONAL_AUDIO_KEY,CIUKI_MASTER_VALUE,&packed) && ciuki_master_unpack(packed,&left,&right,&mute)) {
        struct ciuki_mixer_master m={.version=1,.size=sizeof(m),.left=left,.right=right,.mute=mute};
        if (!apply(fd,0,&m,sizeof(m))) applied++;
    }
    for (unsigned i=0; r && i<6; i++) {
        char name[16]; line_value_name(name,i<5 ? i+1 : 0,i==5);
        if (reg_get_int32(r,PERSONAL_AUDIO_KEY,name,&packed)) continue;
        unsigned source=i<5 ? i+1 : ((uint32_t)packed>>24)&7u;
        if (!ciuki_master_unpack(packed&0xffffff,&left,&right,&mute) || source<1 || source>5) continue;
        struct ciuki_mixer_line line={.version=1,.size=sizeof(line),.line=source,.recording=i==5,
            .left=left,.right=right,.mute=mute,.selected=i==5};
        if (!apply(fd,1,&line,sizeof(line))) applied++;
    }
    reg_close_root(r);
    pthread_mutex_unlock(&workspace_lock);
    if (error) { errno=error; return -1; }
    return applied;
}
