/* Authored restart scenarios, shared verbatim with the shell host adapter.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "exercise.h"
static unsigned length(const char *s){unsigned n=0;while(s[n])n++;return n;}
static void copy(void *to,const void *from,unsigned n){unsigned char *a=to;const unsigned char *b=from;while(n--)*a++=*b++;}
static void zero(void *to,unsigned n){unsigned char *p=to;while(n--)*p++=0;}
static int equal(const void *a,const void *b,unsigned n){const unsigned char *p=a,*q=b;while(n--)if(*p++!=*q++)return 0;return 1;}
static int contains(const char *text,const char *part){for(;*text;text++){unsigned i=0;while(part[i]&&text[i]==part[i])i++;if(!part[i])return 1;}return 0;}
static int link_argument(const char *b,int n,const char *arg)
{
    if(n<40||!equal(b,"CIUKLNK3",8))return 0;
    unsigned offset=40;
    for(unsigned i=0;i<5;i++){
        const unsigned char *p=(const unsigned char *)b+16+i*4;
        unsigned size=(unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24;
        if(size>(unsigned)n-offset)return 0;
        if(i==2)return size==length(arg)&&equal(b+offset,arg,size);
        offset+=size;
    }
    return 0;
}
static void check(struct w32_restart_result *r,unsigned c,int ok,unsigned line)
{if(!ok){r->cases[c].errors++;if(!r->first_error)r->first_error=line;}}
#define CHECK(c,x) check(r,c,(x),__LINE__)
static int bytes(const struct w32_restart_env *e,const char *path,const char *want)
{char b[512];unsigned n=length(want);return n<sizeof(b)&&e->read(path,b,sizeof(b))==(int)n&&equal(b,want,n);}
static int save(struct w32_restart_result *r,const struct w32_restart_env *e,unsigned next)
{struct w32_restart_state s={.magic=W32_RESTART_MAGIC,.version=1,.next=next};copy(&s.result,r,sizeof(*r));return e->write(W32_RESTART_STATE,&s,sizeof(s));}
static void dde(struct w32_restart_result *r,const struct w32_restart_env *e)
{
    static char groups[4096],link[4096];
    for(unsigned raw=0;raw<2;raw++){
        unsigned *count=raw?&r->raw_commands:&r->ddeml_commands;
        CHECK(RESTART_DDE,e->dde(raw,"[CreateGroup(F507D)]",0,0));(*count)++;
        CHECK(RESTART_DDE,e->dde(raw,"[AddItem(\"" W32_RESTART_EXE " --S\",First)]",0,0));(*count)++;
        int n=e->read(W32_RESTART_GROUP "\\First.lnk",link,sizeof(link));
        /* .lnk format is the project's shell-link writer, not MS-SHLLINK. */
        CHECK(RESTART_DDE,link_argument(link,n,"--S"));r->links+=n>0;
        groups[0]=0;CHECK(RESTART_DDE,e->dde(raw,0,groups,sizeof(groups))&&contains(groups,"F507D\r\n"));r->groups++;
        r->checkpoint=raw*2+1;CHECK(RESTART_DDE,e->park(r));
        CHECK(RESTART_DDE,e->dde(raw,"[ReplaceItem(First)][AddItem(\"" W32_RESTART_EXE " --M\",Second)]",0,0));(*count)+=2;
        CHECK(RESTART_DDE,e->exists(W32_RESTART_GROUP "\\First.lnk")==0);
        n=e->read(W32_RESTART_GROUP "\\Second.lnk",link,sizeof(link));
        CHECK(RESTART_DDE,link_argument(link,n,"--M"));r->links+=n>0;
        r->checkpoint=raw*2+2;CHECK(RESTART_DDE,e->park(r));
        CHECK(RESTART_DDE,e->dde(raw,"[DeleteItem(Second)]",0,0));(*count)++;
        CHECK(RESTART_DDE,e->exists(W32_RESTART_GROUP "\\Second.lnk")==0);
        CHECK(RESTART_DDE,e->dde(raw,"[DeleteGroup(F507D)]",0,0));(*count)++;
        CHECK(RESTART_DDE,e->exists(W32_RESTART_GROUP)==0);
    }
    r->checkpoint=0;r->release=0;
    r->cases[RESTART_DDE].checked=1;
}
static const char *const value_names[]={"F507D-S","F507D-M","F507D-U","F507D-R","F507D-T"};
static const unsigned queues[]={0,1,2,0,1};
static const char *const commands[]={W32_RESTART_EXE " --S",W32_RESTART_EXE " --M",W32_RESTART_EXE " --U",W32_RESTART_EXE " --R",W32_RESTART_EXE " --T"};
static void consumed(struct w32_restart_result *r,const struct w32_restart_env *e,unsigned first,unsigned end,unsigned c)
{for(unsigned i=first;i<end;i++){unsigned error=e->registry(queues[i],value_names[i],0);CHECK(c,error==2);r->registry_absent+=error==2;}}
int w32_restart_helper(struct w32_restart_result *r,const struct w32_restart_env *e,unsigned which)
{
    if(which>=5)return 1;
    zero(r,sizeof(*r));r->magic=W32_RESTART_MAGIC;r->version=1;r->helper=which+1;
    char order[16];int n=e->read(W32_RESTART_ORDER,order,sizeof(order));
    const char expected[]="SMURT";
    /* Every child proves both ordering and deletion BEFORE its launch. */
    if(n!=(int)which || !equal(order,expected,which) || e->registry(queues[which],value_names[which],0)!=2)return 1;
    r->registry_absent=1;
    order[which]=expected[which];if(!e->write(W32_RESTART_ORDER,order,which+1))return 1;
    r->interrupt=which==3;
    if(!e->park(r))return 1;
    return 0;
}
int w32_restart_exercise(struct w32_restart_result *r,const struct w32_restart_env *e)
{
    struct w32_restart_state state;
    zero(r,sizeof(*r));r->magic=W32_RESTART_MAGIC;r->version=1;
    int n=e->read(W32_RESTART_STATE,&state,sizeof(state));
    if(n>=0){
        if(n!=sizeof(state)||state.magic!=W32_RESTART_MAGIC||state.version!=1||state.next<1||state.next>3||state.result.magic!=W32_RESTART_MAGIC||state.result.version!=1||state.result.phase+1!=state.next||state.result.done||state.result.release||state.result.checkpoint)return -1;
        copy(r,&state.result,sizeof(*r));r->phase=state.next;
    }else if(e->exists(W32_RESTART_STATE)!=0)return -1;
    if(r->phase==0){
        /* Refuse to overwrite unrelated installation work. The run overlay
         * supplies a fresh namespace; collisions fail visibly. */
        if(e->exists(W32_RESTART_INI)!=0||e->exists(W32_RESTART_JOURNAL)!=0)return -1;
        for(unsigned i=0;i<5;i++)if(e->registry(queues[i],value_names[i],0)!=2)return -1;
        dde(r,e);
        CHECK(RESTART_SESSION,e->session(0,1,r));
        CHECK(RESTART_SESSION,r->queries==2&&r->cancelled==2&&r->accepted==0&&r->logoff==2);
        /* The accepting run ends this process; preserve its accumulated
         * evidence after notifications and before exit in the guest adapter. */
        if(!save(r,e,1))return -1;
        CHECK(RESTART_SESSION,e->session(0,0,r));
        CHECK(RESTART_SESSION,r->queries==4&&r->accepted==2&&r->logoff==4);
        r->cases[RESTART_SESSION].checked=1;
        if(!save(r,e,1))return -1;
        return 1;
    }
    if(r->phase==1){
        CHECK(RESTART_WININIT,e->write(W32_RESTART_TEMP "f507d.source","restart-data",12));
        CHECK(RESTART_WININIT,e->write(W32_RESTART_TEMP "f507d.delete","delete",6));
        static const char ini[]="[rename]\r\n" W32_RESTART_TEMP "f507d.middle=" W32_RESTART_TEMP "f507d.source\r\n" W32_RESTART_TEMP "f507d.final=" W32_RESTART_TEMP "f507d.middle\r\nNUL=" W32_RESTART_TEMP "f507d.delete\r\n";
        CHECK(RESTART_WININIT,e->write(W32_RESTART_INI,ini,sizeof(ini)-1));
        CHECK(RESTART_RUNONCE,e->write(W32_RESTART_ORDER,"",0));
        for(unsigned i=0;i<3;i++)CHECK(RESTART_RUNONCE,e->registry(queues[i],value_names[i],commands[i])==0);
        if(!save(r,e,2))return -1;
        CHECK(RESTART_WININIT,e->session(2,0,r));
        if(!save(r,e,2))return -1;
        return 1;
    }
    if(r->phase==2){
        CHECK(RESTART_WININIT,bytes(e,W32_RESTART_TEMP "f507d.final","restart-data"));
        CHECK(RESTART_WININIT,e->exists(W32_RESTART_TEMP "f507d.source")==0&&e->exists(W32_RESTART_TEMP "f507d.middle")==0&&e->exists(W32_RESTART_TEMP "f507d.delete")==0);
        CHECK(RESTART_WININIT,e->exists(W32_RESTART_INI)==0&&e->exists("C:\\WINDOWS\\WININIT.BAK")==1&&e->exists(W32_RESTART_JOURNAL)==0);
        r->cases[RESTART_WININIT].checked=1;
        CHECK(RESTART_RUNONCE,bytes(e,W32_RESTART_ORDER,"SMU"));consumed(r,e,0,3,RESTART_RUNONCE);r->cases[RESTART_RUNONCE].checked=1;
        for(unsigned i=3;i<5;i++)CHECK(RESTART_RECOVERY,e->registry(queues[i],value_names[i],commands[i])==0);
        if(!save(r,e,3))return -1;
        CHECK(RESTART_RECOVERY,e->session(2,0,r));
        if(!save(r,e,3))return -1;
        return 1;
    }
    CHECK(RESTART_RECOVERY,bytes(e,W32_RESTART_ORDER,"SMURT"));consumed(r,e,3,5,RESTART_RECOVERY);
    CHECK(RESTART_RECOVERY,e->exists(W32_RESTART_JOURNAL)==0);r->cases[RESTART_RECOVERY].checked=1;
    r->cases[RESTART_DESKTOP].checked=1;r->done=1;
    return 0;
}
