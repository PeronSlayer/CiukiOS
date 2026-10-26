/* Production __CIUKIOS__ lock path; only the service and process snapshot are
 * injected. SPDX-License-Identifier: MIT */
#include "replay_base.h"
#include "../../../sdk/sysroot-overlay/include/ciuki/proc.h"
#include "../../../sdk/sysroot-overlay/include/ciuki/recovery.h"
#include "../../../sdk/win32/include/w32/server.h"
#include "users.h"
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "settings_io.h"
static unsigned held,acquires,releases,boot=100,live_pid=9999;
static int snapshot_error,transport_error;
static _Thread_local unsigned writer;
static unsigned held_pid,phase,a_wait,b_wait,resume_a,resume_b;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
pid_t __real_getpid(void);
pid_t __wrap_getpid(void) { return writer ? (pid_t)(10000+writer) : __real_getpid(); }
int __real_rename(const char *,const char *);
int __wrap_rename(const char *from,const char *to)
{
    if(writer==1 && phase==1 && strstr(from,".claim-")) {
        /* The whole record exists before publication; no empty users.lock. */
        struct stat st; assert(!stat(from,&st) && S_ISDIR(st.st_mode));
        assert(access(to,F_OK) && errno==ENOENT);
        DIR *dir=opendir(from);assert(dir);struct dirent *entry;unsigned complete=0;
        while((entry=readdir(dir)))if(!strncmp(entry->d_name,"owner-",6)) {
            char path[800],magic[16],boot_id[64];unsigned pid;
            snprintf(path,sizeof(path),"%s/%s",from,entry->d_name);
            FILE *f=fopen(path,"rb");assert(f && fscanf(f,"%15s %u %63s",magic,&pid,boot_id)==3);
            assert(!strcmp(magic,"CUSR1") && pid==10001 && !strcmp(boot_id,"boot-100"));
            assert(!fclose(f));complete++;
        }
        assert(!closedir(dir) && complete==1);
        pthread_mutex_lock(&mu); a_wait=1; pthread_cond_broadcast(&cv);
        while(!resume_a)pthread_cond_wait(&cv,&mu);
        pthread_mutex_unlock(&mu);
    }
    return __real_rename(from,to);
}
int __real_cfg_write_at(const char *,const char *,const char *,size_t);
int __wrap_cfg_write_at(const char *path,const char *tmp,const char *text,size_t n)
{
    if(phase && ((writer==2 && phase==1) || (writer==1 && phase==2))) {
        pthread_mutex_lock(&mu);
        unsigned *waiting=writer==1 ? &a_wait : &b_wait,*resume=writer==1 ? &resume_a : &resume_b;
        *waiting=1; pthread_cond_broadcast(&cv);
        while(!*resume)pthread_cond_wait(&cv,&mu);
        pthread_mutex_unlock(&mu);
    }
    return __real_cfg_write_at(path,tmp,text,n);
}
static const char *store;
static int results[3];
static void *mutate(void *arg)
{
    writer=(unsigned)(uintptr_t)arg;
    struct users_request q={.operation=USERS_POLICY,.flags=writer==1 ? USERS_LOGON : USERS_LOCK};
    struct users_view v; results[writer]=users_execute(store,&q,&v); return NULL;
}
static void wait_ready(unsigned *ready)
{ pthread_mutex_lock(&mu); while(!*ready)pthread_cond_wait(&cv,&mu); pthread_mutex_unlock(&mu); }
static void resume_writer(unsigned *resume)
{ pthread_mutex_lock(&mu); *resume=1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu); }
static void restart_race(const char *root,unsigned scenario)
{
    store=root; phase=scenario; a_wait=b_wait=resume_a=resume_b=0;
    pthread_t a,b; assert(!pthread_create(&a,NULL,mutate,(void *)(uintptr_t)1)); wait_ready(&a_wait);
    /* Lost in-memory lease, while the first process survives the server. */
    held=held_pid=0;
    assert(!pthread_create(&b,NULL,mutate,(void *)(uintptr_t)2));
    if(scenario==1) {
        wait_ready(&b_wait); resume_writer(&resume_a); assert(!pthread_join(a,NULL));
        assert(results[1]==-EBUSY && held && held_pid==10002);
        resume_writer(&resume_b); assert(!pthread_join(b,NULL)); assert(!results[2]);
    } else {
        assert(!pthread_join(b,NULL)); assert(results[2]==-EBUSY && !held);
        resume_writer(&resume_a); assert(!pthread_join(a,NULL)); assert(!results[1]);
    }
    assert(!held); phase=0;
    puts(scenario==1 ? "PASS g32b restart before atomic publication: replacement writer fenced, no concurrent users.new save" :
         "PASS g32b restart after atomic publication: live previous lease owner remains fenced");
}
int ciuki_recovery_status(struct ciuki_recovery_status *r)
{ memset(r,0,sizeof(*r)); r->flags=CIUKI_RECOVERY_VALID;r->boots=boot;return 0; }
int ciuki_proc_list(void *buffer,uint32_t bytes)
{
    assert(bytes>=sizeof(struct ciuki_proc_system)+2*sizeof(struct ciuki_proc_record));
    if(snapshot_error)return -1;
    struct ciuki_proc_system *s=buffer;s->records=phase ? 2 : live_pid ? 1 : 0;
    struct ciuki_proc_record *r=(void *)(s+1);r->pid=phase ? 10001 : live_pid;r->state=CIUKI_PROC_LIVE;
    if(phase){r[1].pid=10002;r[1].state=CIUKI_PROC_LIVE;}
    return (int)s->records;
}
uint32_t desk_w32_exchange(const void *wire,uint32_t n,void *out,uint32_t cap,uint32_t *used)
{
    const struct w32r_request *q=wire;
    assert(n==sizeof(*q)+2 && cap==sizeof(struct w32r_reply));
    assert(q->path_bytes==1 && q->name_bytes==1 && !q->key && !q->flags && !q->access);
    if(transport_error)return 1062;
    struct w32r_reply r={.version=W32R_VERSION};
    pthread_mutex_lock(&mu);
    if(q->op==W32R_PROFILE_ACQUIRE){acquires++;if(held)r.error=170;else {held=1;held_pid=(unsigned)getpid();}}
    else {assert(q->op==W32R_PROFILE_RELEASE);if(!held || held_pid!=(unsigned)getpid())r.error=5;else {held=held_pid=0;releases++;}}
    pthread_mutex_unlock(&mu);
    memcpy(out,&r,sizeof(r));*used=sizeof(r);return 0;
}
static void leftover(const char *path,unsigned pid,unsigned old_boot)
{ FILE *f=fopen(path,"wb");assert(f);fprintf(f,"CUSR1 %u boot-%u\n",pid,old_boot);assert(!fclose(f)); }
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    assert(argc==2);struct users_request q={.operation=USERS_POLICY};struct users_view v;
    assert(!users_execute(argv[1],&q,&v) && !held && acquires==1 && releases==1);
    char path[512];snprintf(path,sizeof(path),"%s/system/settings/users.lock",argv[1]);
    leftover(path,live_pid,99);assert(!users_execute(argv[1],&q,&v)); /* old boot, even a live reused PID */
    leftover(path,9998,100);assert(!users_execute(argv[1],&q,&v)); /* dead PID */
    leftover(path,live_pid,100);assert(users_execute(argv[1],&q,&v)==-EBUSY && !held && !access(path,F_OK));
    snapshot_error=1;assert(users_execute(argv[1],&q,&v)==-EIO && !held && !access(path,F_OK));snapshot_error=0;
    held=1;unsigned released=releases;assert(users_execute(argv[1],&q,&v)==-EBUSY && held && releases==released && !access(path,F_OK));held=0;
    transport_error=1;assert(users_execute(argv[1],&q,&v)==-EIO && !access(path,F_OK));transport_error=0;
    live_pid=0;assert(!users_execute(argv[1],&q,&v) && !held && access(path,F_OK));
    char record[640];assert(!mkdir(path,0700));snprintf(record,sizeof(record),"%s/owner-old",path);
    leftover(record,9999,99);assert(!users_execute(argv[1],&q,&v)); /* published directory from an earlier boot */
    assert(!mkdir(path,0700));leftover(record,9998,100);assert(!users_execute(argv[1],&q,&v)); /* dead directory owner */
    assert(!mkdir(path,0700));assert(!users_execute(argv[1],&q,&v)); /* interrupted final cleanup: atomic empty rmdir */
    boot=0;assert(users_execute(argv[1],&q,&v)==-EIO && !held);boot=100;
    FILE *empty=fopen(path,"wb");assert(empty && !fclose(empty));
    assert(users_execute(argv[1],&q,&v)==-EBUSY && !access(path,F_OK)); assert(!unlink(path));
    restart_race(argv[1],1); restart_race(argv[1],2);
    unsigned before=acquires;q.operation=USERS_READ;assert(!users_execute(argv[1],&q,&v) && acquires==before);
    puts("PASS g32b guest lock: old boot, dead PID, live contention, recovery serialization, service/snapshot/boot failure, read without lease");
}
