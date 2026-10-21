/* Authored fixture over the existing production shell/session adapter. */
#include "../../../../src/probes/win32_restart/exercise.h"
static struct ws_fs restart_fs;
static char *mapped(const char *p,char out[2048])
{
    char normalized[2048];assert(strlen(p)<sizeof(normalized));strcpy(normalized,p);
    for(char *c=normalized;*c;c++)if(*c=='\\')*c='/';
    const char *s=normalized;if(s[0]=='C'&&s[1]==':')s+=2;
    if(!strncmp(s,"/WINDOWS/Start Menu/Programs",28))snprintf(out,2048,"%s%s",programs,s+28);
    else if(!strncmp(s,"/WINDOWS",8))snprintf(out,2048,"%s%s",root,s+8);
    else if(!strncmp(s,"/System",7))snprintf(out,2048,"%s%s",root,s);
    else snprintf(out,2048,"%s",s);
    return out;
}
static int rf_stat(void *o,const char *p,int *d,uint64_t *n){char b[2048];return restart_fs.stat(o,mapped(p,b),d,n);}
static int rf_mkdir(void *o,const char *p){char b[2048];return restart_fs.mkdir(o,mapped(p,b));}
static int rf_rmdir(void *o,const char *p){char b[2048];return restart_fs.rmdir(o,mapped(p,b));}
static int rf_unlink(void *o,const char *p){char b[2048];return restart_fs.unlink(o,mapped(p,b));}
static int rf_rename(void *o,const char *p,const char *q){char b[2048],c[2048];return restart_fs.rename(o,mapped(p,b),mapped(q,c));}
static int rf_read(void *o,const char *p,uint8_t **b,size_t *n,size_t cap){char path[2048];return restart_fs.read(o,mapped(p,path),b,n,cap);}
static int rf_write(void *o,const char *p,const void *b,size_t n){char path[2048];return restart_fs.write(o,mapped(p,path),b,n);}
static int rf_list(void *o,const char *p,int (*cb)(void *,const char *,int),void *ctx){char path[2048];return restart_fs.list(o,mapped(p,path),cb,ctx);}
static int fixture_read(const char *p,void *b,unsigned cap)
{uint8_t *data=0;size_t n=0;int e=fs.read(fs.opaque,p,&data,&n,cap);if(!e)memcpy(b,data,n);free(data);return e?-1:(int)n;}
static int fixture_write(const char *p,const void *b,unsigned n){return !fs.write(fs.opaque,p,b,n);}
static int fixture_remove(const char *p){int e=fs.unlink(fs.opaque,p);return !e||e==ENOENT;}
static int fixture_exists(const char *p){int e=fs.stat(fs.opaque,p,0,0);return !e?1:e==ENOENT?0:-1;}
static unsigned conversations[2];
static int fixture_dde(unsigned raw,const char *cmd,char *out,unsigned cap)
{
    if(!conversations[raw])conversations[raw]=open_conv(10,raw?"PROGMAN":"Shell",raw?"PROGMAN":"AppProperties");
    struct w32r_reply r;
    if(cmd)return !rpc(10,W32R_PROGMAN_EXECUTE,conversations[raw],0,0,0,cmd,"",NULL,0,&r,NULL,0)&&r.key==0x8000;
    return !rpc(10,W32R_PROGMAN_REQUEST,conversations[raw],0,0,0,"","Groups",NULL,0,&r,out,cap)&&r.data_bytes&&out[r.data_bytes-1]==0;
}
static unsigned fixture_registry(unsigned queue,const char *name,const char *cmd)
{
    struct w32r_reply r;const char *path=queue==0?"Software\\Microsoft\\Windows\\CurrentVersion\\RunServicesOnce":"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
    uint32_t root_key=queue==2?W32R_HKCU:W32R_HKLM;
    uint32_t e=rpc(10,cmd?W32R_CREATE:W32R_OPEN,0,root_key,0,0,path,"",NULL,0,&r,NULL,0);
    if(e)return e;uint32_t h=r.key;
    if(cmd){uint8_t wide[2048];unsigned n=(unsigned)strlen(cmd)+1;assert(n*2<=sizeof(wide));for(unsigned i=0;i<n;i++){wide[i*2]=cmd[i];wide[i*2+1]=0;}e=rpc(10,W32R_SET,0,h,1,0,"",name,wide,n*2,&r,NULL,0);}
    else {uint8_t b[2048];e=rpc(10,W32R_QUERY,0,h,0,0,"",name,NULL,0,&r,b,sizeof(b));}
    assert(!rpc(10,W32R_CLOSE,0,h,0,0,"","",NULL,0,&r,NULL,0));return e;
}
static struct desk_restart fixture_restart;
static unsigned fixture_windows[2],fixture_pid_live,fixture_child,fixture_runs[5],fixture_recovered,fixture_notices;
static struct w32_restart_env fixture_env;
static int fixture_alive(void *o,uint32_t pid){(void)o;return pid==10?fixture_pid_live:pid==80?fixture_child:0;}
static void fixture_notice(void *o,unsigned kind,uint32_t pid,uint32_t flags){(void)o;(void)pid;(void)flags;if(kind==3)fixture_notices++;}
static int fixture_park(struct w32_restart_result *r)
{
    if(!r->helper){assert(r->checkpoint>=1&&r->checkpoint<=4);return 1;}
    if(r->helper==4){
        assert(fixture_exists(W32_RESTART_JOURNAL)==1);assert(ws_restart_phase(shell)==2);
        /* A real registry + shell destroy/load, not just a new ws_shell. */
        assert(!wr_flush(&server));ws_destroy(shell);wr_destroy(&server);assert(wr_init(&server,&io,server.now));server.compat_writer_pid=99;
        struct ws_config c={programs,root};shell=ws_create(&fs,&c,&server);assert(shell);server.shell=shell;server.shell_call=ws_call;server.shell_exit=ws_exit;
        assert(ws_restart_phase(shell)==2);fixture_recovered++;
    }
    r->release=1;return 1;
}
static int fixture_launch(void *o,const char *cmd)
{
    (void)o;const char *tag=strstr(cmd," --");assert(tag);const char *names="SMURT";const char *found=strchr(names,tag[3]);assert(found);
    unsigned which=(unsigned)(found-names);assert(!fixture_runs[which]++);
    struct w32_restart_result r;assert(!w32_restart_helper(&r,&fixture_env,which));fixture_child=0;return 80;
}
static const struct desk_restart_ops fixture_ops={NULL,desktop_call,fixture_alive,terminate,fixture_launch,fixture_notice,record};
static int fixture_session(unsigned flags,unsigned veto,struct w32_restart_result *r)
{
    fixture_pid_live=1;assert(!fixture_restart.stage);
    assert(!rpc(10,W32R_SHUTDOWN,W32R_SHUTDOWN_REQUEST,0,0,flags,"","",NULL,0,&(struct w32r_reply){0},NULL,0));
    char message[256];struct w32r_reply pending;assert(!rpc(99,W32R_SHUTDOWN,W32R_SHUTDOWN_POLL,0,0,0,"","",NULL,0,&pending,message,sizeof(message)));assert(pending.data_bytes);
    /* The desktop's confirmation starts the real coordinator. */
    assert(!desk_restart_begin(&fixture_restart,&fixture_ops,10,flags,server.now));
    for(unsigned end=0;end<2;end++)for(unsigned i=0;i<2;i++){
        struct w32r_reply m;assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&m,NULL,0));
        assert(m.type==(end?0x16u:0x11u));assert(m.key==fixture_windows[i]||m.key==fixture_windows[1-i]);
        if(!end){r->queries++;r->logoff+=!!(m.disposition&0x80000000u);}
        else if(m.count)r->accepted++;else r->cancelled++;
        unsigned answer=end?1:!(veto&&i==1);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,m.key,100,answer,"","",NULL,0,&m,NULL,0));
    }
    tick(&fixture_restart,&fixture_ops,server.now+20);
    if(veto)assert(!fixture_restart.stage);else assert(fixture_restart.stage==2);
    return 1;
}
static unsigned published,failed_predicates;
static void fixture_emit(const char *probe,const char *event,const char *format,...)
{
    assert(!strcmp(probe,"w32-restart")&&!strcmp(event,"DATA"));char b[256];va_list ap;va_start(ap,format);vsnprintf(b,sizeof(b),format,ap);va_end(ap);puts(b);published++;if(!strstr(b,"errors=0 checked=1"))failed_predicates++;
}
static void restart_fixture_test(void)
{
    restart_fs=fs;fs=(struct ws_fs){NULL,rf_stat,rf_mkdir,rf_rmdir,rf_unlink,rf_rename,rf_read,rf_write,rf_list,log_line};
    recreate();server.compat_writer_pid=99;wr_client_join(&server,10);
    assert(!fs.mkdir(NULL,"C:\\WINDOWS\\TEMP"));assert(!fs.mkdir(NULL,"C:\\System"));assert(!fs.mkdir(NULL,"C:\\System\\Win32"));assert(!fs.mkdir(NULL,"C:\\System\\Win32\\Fixtures"));
    assert(fixture_write(W32_RESTART_EXE,"MZ",2));
    fixture_env=(struct w32_restart_env){fixture_read,fixture_write,fixture_remove,fixture_exists,fixture_dde,fixture_registry,fixture_session,fixture_park};
    struct w32_restart_result result;
    for(unsigned phase=0;phase<4;phase++){
        for(unsigned i=0;i<2;i++){struct w32r_reply r;assert(!rpc(10,W32R_WINDOW_ALLOC,100,0,1,0,"","restart",NULL,0,&r,NULL,0));fixture_windows[i]=r.key;}
        int e=w32_restart_exercise(&result,&fixture_env);fprintf(stderr,"fixture phase=%u returned=%d\n",phase,e);assert(e==(phase==3?0:1));
        for(unsigned i=0;i<RESTART_CASES;i++)if(result.cases[i].errors)fprintf(stderr,"fixture phase=%u case=%u errors=%u first=%u\n",phase,i,result.cases[i].errors,result.first_error);
        for(unsigned i=0;i<RESTART_CASES;i++)assert(!result.cases[i].errors);
        for(unsigned i=0;i<2;i++){struct w32r_reply r;assert(!rpc(10,W32R_WINDOW_FREE,0,fixture_windows[i],100,0,"","",NULL,0,&r,NULL,0));}
        fixture_pid_live=0;
        for(unsigned i=0;i<1000&&fixture_restart.stage;i++)tick(&fixture_restart,&fixture_ops,server.now+20);
        assert(!fixture_restart.stage);
    }
    assert(fixture_recovered==1&&fixture_notices==4);
    for(unsigned i=0;i<5;i++)assert(fixture_runs[i]==1);
    struct w32_restart_facts facts={1,1,1,1,1,1};
    assert(w32_restart_publish(&result,&facts,fixture_emit)&&published==6&&!failed_predicates);
    for(unsigned i=0;i<RESTART_CASES;i++){
        struct w32_restart_result bad=result;bad.cases[i].checked=0;assert(!w32_restart_publish(&bad,&facts,fixture_emit));
        bad=result;bad.cases[i].errors=1;assert(!w32_restart_publish(&bad,&facts,fixture_emit));
    }
    uint32_t *f=(uint32_t *)&facts;for(unsigned i=0;i<6;i++){f[i]=0;assert(!w32_restart_publish(&result,&facts,fixture_emit));f[i]=1;}
    /* Persisted corruption and duplicate helpers must fail closed. */
    struct w32_restart_result bad=result;bad.magic=0;assert(!w32_restart_publish(&bad,&facts,fixture_emit));
    assert(w32_restart_helper(&bad,&fixture_env,0)==1);
    assert(fixture_write(W32_RESTART_STATE,"broken",6));assert(w32_restart_exercise(&bad,&fixture_env)==-1);
    puts("PASS authored restart fixture DDE links Groups session veto accept WININIT ordered RunOnce durable service recovery predicates negative evidence");
}
