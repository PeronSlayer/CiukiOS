/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "users.h"
#include "users_worker.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

static struct users_db db, decoded;
static struct users_view v;
static struct users_request q;
static unsigned char bytes[1500];
static const uint8_t salt[16]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
static void request(unsigned op,const char *name,const char *password)
{
    memset(&q,0,sizeof(q)); q.operation=op;
    snprintf(q.name,sizeof(q.name),"%s",name);
    snprintf(q.password,sizeof(q.password),"%s",password);
    snprintf(q.confirm,sizeof(q.confirm),"%s",password);
}
static void model(void)
{
    assert(users_name_valid("Alice-2_test"));
    for (unsigned i=0;i<7;i++) {
        const char *bad[]={"","Default","../Alice","a/b","a b","CON:","abcdefghijklmnopqrstuvwxyz012345"};
        assert(!users_name_valid(bad[i]));
    }
    uint8_t out[32]; users_derive("password",salt,2,out);
    /* Python hashlib.pbkdf2_hmac oracle, independently generated. */
    static const unsigned char expected[32]={0xa0,0xff,0xd3,0x42,0x92,0xe4,0xe7,0xd5,0x19,0xee,0x0f,0x25,0x12,0x4e,0x0c,0x5b,0x94,0x81,0xd8,0x8b,0xe4,0x0f,0x46,0x62,0xeb,0x6c,0xb4,0xb9,0xe4,0x3f,0x9f,0x41};
    assert(!memcmp(out,expected,32));
    request(USERS_CREATE,"Alice","secret"); db.next_id=1;
    assert(!users_apply(&db,&q,salt,&v)); assert(v.count==1 && v.ids[0]==1);
    request(USERS_CREATE,"alice","other"); assert(users_apply(&db,&q,salt,&v)==-EEXIST);
    request(USERS_AUTH,"ALICE","wrong"); assert(users_apply(&db,&q,NULL,&v)==-EACCES);
    strcpy(q.password,"secret"); assert(!users_apply(&db,&q,NULL,&v) && v.authenticated==1);
    request(USERS_CHANGE,"Alice","new"); strcpy(q.current,"Alice"); strcpy(q.old_password,"wrong");
    assert(users_apply(&db,&q,salt,&v)==-EACCES);
    strcpy(q.old_password,"secret"); strcpy(q.confirm,"mismatch"); assert(users_apply(&db,&q,salt,&v)==-EINVAL);
    strcpy(q.confirm,"new"); assert(!users_apply(&db,&q,salt,&v));
    request(USERS_AUTH,"Alice","secret"); assert(users_apply(&db,&q,NULL,&v)==-EACCES);
    strcpy(q.password,"new"); assert(!users_apply(&db,&q,NULL,&v));
    size_t n=users_encode(&db,bytes,sizeof(bytes)); assert(n && !users_decode(&decoded,bytes,n));
    assert(decoded.count==1 && !memcmp(decoded.records,db.records,sizeof(db.records)));
    bytes[60]^=1; assert(users_decode(&decoded,bytes,n)==-EINVAL); bytes[60]^=1;
    assert(users_decode(&decoded,bytes,n-1)==-EINVAL);
    request(USERS_POLICY,"",""); q.flags=8; assert(users_apply(&db,&q,NULL,&v)==-EINVAL);
    request(USERS_DELETE,"Alice",""); strcpy(q.current,"ALICE"); assert(users_apply(&db,&q,NULL,&v)==-EBUSY);
    q.current[0]=0; assert(!users_apply(&db,&q,NULL,&v) && !v.count);
    request(USERS_CREATE,"Bob",""); assert(!users_apply(&db,&q,salt,&v) && v.ids[0]==2);
    request(USERS_AUTH,"bob",""); assert(!users_apply(&db,&q,NULL,&v));
    struct user_profile a,b;
    users_profile(&a,"Alice",1,USERS_PROFILES); users_profile(&b,"Bob",2,USERS_PROFILES);
    assert(strcmp(a.desktop,b.desktop) && strcmp(a.documents,b.documents) && strcmp(a.settings,b.settings) && strcmp(a.programs,b.programs));
    users_profile(&a,"",0,0); assert(!strcmp(a.desktop,"/Documents/Desktop") && !strcmp(a.settings,"/system/settings/desktop.cfg"));
    puts("PASS G32 model: PBKDF2 oracle, names, auth, change confirmation, deletion, stable isolated profiles, strict persistence");
}
static pthread_mutex_t ready_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_cond=PTHREAD_COND_INITIALIZER;
static int ready;
static void notify(void *ctx)
{
    (void)ctx; pthread_mutex_lock(&ready_lock); ready=1; pthread_cond_signal(&ready_cond); pthread_mutex_unlock(&ready_lock);
}
static void worker_test(const char *root,unsigned operation)
{
    static struct users_worker w; w.root=root;
    request(operation,operation==USERS_AUTH ? "Alice" : "",operation==USERS_AUTH ? "secret" : "");
    ready=0;
    assert(!users_begin(&w,&q,notify,NULL));
    pthread_mutex_lock(&ready_lock); while (!ready) pthread_cond_wait(&ready_cond,&ready_lock); pthread_mutex_unlock(&ready_lock);
    int error=0; assert(users_poll(&w,&v,&error)==1 && !error && !w.settings.clock_24h);
    assert(!w.request.password[0] && users_poll(&w,&v,&error)==0);
    assert(operation==USERS_AUTH ? v.authenticated==1 : !v.authenticated);
}
static void io(const char *root)
{
    request(USERS_READ,"",""); assert(!users_execute(root,&q,&v) && !v.count && !v.flags);
    request(USERS_CREATE,"Alice","secret"); assert(!users_execute(root,&q,&v));
    char path[512]; snprintf(path,sizeof(path),"%s/system/settings/desktop.cfg",root);
    FILE *f=fopen(path,"wb"); assert(f); assert(fputs("clock_24h=0\n",f)>0); assert(!fclose(f));
    request(USERS_POLICY,"",""); q.flags=USERS_FLAGS; assert(!users_execute(root,&q,&v));
    request(USERS_AUTH,"Alice","wrong"); assert(users_execute(root,&q,&v)==-EACCES);
    strcpy(q.password,"secret"); assert(!users_execute(root,&q,&v));
    struct user_profile p; users_profile(&p,"Alice",v.authenticated,v.flags);
    snprintf(path,sizeof(path),"%s%s",root,p.settings); f=fopen(path,"rb"); assert(f);
    char cfg[32]={0}; assert(fread(cfg,1,sizeof(cfg),f)==strlen("clock_24h=0\n")); assert(!fclose(f)); assert(!strcmp(cfg,"clock_24h=0\n"));
    snprintf(path,sizeof(path),"%s/system/settings/users.new",root); assert(!mkdir(path,0755));
    request(USERS_CREATE,"Bob",""); assert(users_execute(root,&q,&v)<0);
    assert(!rmdir(path));
    request(USERS_READ,"",""); assert(!users_execute(root,&q,&v) && v.count==1);
    request(USERS_CREATE,"Bob",""); assert(!users_execute(root,&q,&v));
    char boot[64]; assert(!users_boot_identity(boot));
    snprintf(path,sizeof(path),"%s/system/settings/users.lock",root);
    f=fopen(path,"wb"); assert(f); fprintf(f,"CUSR1 %u %s\n",(unsigned)getpid(),boot); fclose(f);
    request(USERS_DELETE,"Bob",""); assert(users_execute(root,&q,&v)==-EBUSY);
    /* The same PID from an earlier boot must not block account management. */
    f=fopen(path,"wb"); assert(f); fprintf(f,"CUSR1 %u previous-boot\n",(unsigned)getpid()); fclose(f);
    assert(!users_execute(root,&q,&v));
    request(USERS_CREATE,"Bob",""); assert(!users_execute(root,&q,&v));
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        FILE *dead=fopen(path,"wb"); if (!dead) _exit(1);
        fprintf(dead,"CUSR1 %u %s\n",(unsigned)getpid(),boot); fclose(dead); _exit(0);
    }
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    request(USERS_DELETE,"Bob",""); assert(!users_execute(root,&q,&v));
    /* A legacy empty lock is recoverable too, but only under the guard. */
    f=fopen(path,"wb"); assert(f); fclose(f);
    char guard_path[512]; snprintf(guard_path,sizeof(guard_path),"%s/system/settings/users.guard",root);
    int guard=open(guard_path,O_RDWR); assert(guard>=0 && !flock(guard,LOCK_EX|LOCK_NB));
    request(USERS_CREATE,"Bob",""); assert(users_execute(root,&q,&v)==-EBUSY);
    assert(!access(path,F_OK)); assert(!close(guard));
    assert(!users_execute(root,&q,&v));
    request(USERS_DELETE,"Bob",""); assert(!users_execute(root,&q,&v));
    snprintf(path,sizeof(path),"%s%s",root,p.settings); struct stat st; assert(!stat(path,&st));
    worker_test(root,USERS_AUTH);
    snprintf(path,sizeof(path),"%s/system/settings/users.dat",root); f=fopen(path,"wb"); assert(f); fputs("corrupt",f); fclose(f);
    request(USERS_READ,"",""); assert(users_execute(root,&q,&v)==-EINVAL);
    worker_test(root,USERS_DEFAULT);
    puts("PASS G32 I/O: optional default, salted durable accounts, profile seed, lock contention, retained files, corruption rejected");
}
int main(int argc,char **argv) { setvbuf(stdout,NULL,_IONBF,0); assert(argc==2); model(); io(argv[1]); return 0; }
