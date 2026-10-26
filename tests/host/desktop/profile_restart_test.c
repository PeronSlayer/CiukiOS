/* Exercise the actual publication mutex and restart snapshot at the reply
 * boundary, with only kernel transport/spawn calls replaced. SPDX-License-Identifier: MIT */
#include "../../../apps/desktop/win32_native.c"
#include "session_profile.h"
#include <assert.h>
int desk_w32_profile(const struct user_profile *);
static pthread_mutex_t test_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_cv=PTHREAD_COND_INITIALIZER;
static unsigned paused,proceed,restarting,hold_reply=1,reply_error,malformed;
static struct w32r_packet request;
static const char *expected_name="Bob",*expected_root="/Windows/Profiles/u00000002";
static int published;
uint32_t desk_w32_record(void *p,uint32_t n)
{
    struct w32r_service *s=p;assert(n==sizeof(*s) && s->action==2 && s->service==2);
    s->endpoint=7;s->result=1;return 0;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    assert(fd==7 && flags==DONTWAIT && m->length==256);
    memcpy(&request,m->data,256);return 0;
}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{
    assert(fd==7 && flags==DONTWAIT);
    struct w32r_packet r={.version=1,.id=request.id,.kind=request.kind,.total=sizeof(struct w32r_reply)};
    if(request.kind==W32R_BEGIN)r.offset=request.total;
    else {
        assert(request.kind==W32R_READ);
        r.offset=sizeof(struct w32r_reply);
        struct w32r_reply reply={.version=malformed ? 0 : W32R_VERSION,.error=reply_error};
        memcpy(r.bytes,&reply,sizeof(reply));
        if(hold_reply) {
            pthread_mutex_lock(&test_mu);paused=1;pthread_cond_broadcast(&test_cv);
            while(!proceed)pthread_cond_wait(&test_cv,&test_mu);
            pthread_mutex_unlock(&test_mu);
        }
    }
    memset(m,0,sizeof(*m));m->length=256;memcpy(m->data,&r,256);return 1;
}
int ciuki_proc_list(void *p,uint32_t n) { (void)p;(void)n;assert(0);return -1; }
ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *fds,uint32_t n,uint32_t flags)
{
    assert(!strcmp(path,"/System/Win32/w32srv") && !strcmp(argv[0],path) && !argv[1]);
    assert(!fds && !n && !flags);
    char name[64],root[160];snprintf(name,sizeof(name),"CIUKI_SESSION_USER=%s",expected_name);
    snprintf(root,sizeof(root),"CIUKI_SESSION_ROOT=%s",expected_root);
    assert(!strcmp(env[2],name) && !strcmp(env[3],root));return 123;
}
/* Unused native lifecycle callbacks can retain these under ASan. */
int ciuki_channel_pair(int *fds) { (void)fds;assert(0);return -1; }
void desk_shell_message(struct desktop *d,const char *title,const char *message,int error)
{ (void)d;(void)title;(void)message;(void)error;assert(0); }
static void *publish(void *unused)
{
    (void)unused;struct user_profile p={0};strcpy(p.name,"Bob");strcpy(p.root,"/Windows/Profiles/u00000002");
    published=desk_w32_profile(&p);return NULL;
}
static void *restart_snapshot(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&test_mu);restarting=1;pthread_cond_broadcast(&test_cv);pthread_mutex_unlock(&test_mu);
    assert(spawn_service(NULL)==123);return NULL;
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    /* Old cached selection: must never be used by a restart following Bob's acknowledgement. */
    strcpy(session_user,"Alice");strcpy(session_root,"/Windows/Profiles/u00000001");
    __atomic_store_n(&service_registered_once,1,__ATOMIC_RELEASE);
    pthread_t publisher,restarter;assert(!pthread_create(&publisher,NULL,publish,NULL));
    pthread_mutex_lock(&test_mu);while(!paused)pthread_cond_wait(&test_cv,&test_mu);pthread_mutex_unlock(&test_mu);
    assert(pthread_mutex_trylock(&profile_lock)==EBUSY);
    assert(!pthread_create(&restarter,NULL,restart_snapshot,NULL));
    pthread_mutex_lock(&test_mu);while(!restarting)pthread_cond_wait(&test_cv,&test_mu);
    proceed=1;pthread_cond_broadcast(&test_cv);pthread_mutex_unlock(&test_mu);
    assert(!pthread_join(publisher,NULL) && !pthread_join(restarter,NULL) && !published);
    hold_reply=0;
    struct user_profile p={0};strcpy(p.name,"Carol");strcpy(p.root,"/Windows/Profiles/u00000003");
    reply_error=170;assert(desk_w32_profile(&p)<0 && spawn_service(NULL)==123);
    reply_error=0;malformed=1;assert(desk_w32_profile(&p)<0 && spawn_service(NULL)==123);
    puts("PASS g32b publication: reply/cache update serialized with restart snapshot/spawn; failed/malformed acknowledgements preserve identity");
    return 0;
}
