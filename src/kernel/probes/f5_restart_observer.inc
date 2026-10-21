/* Included by the desktop probe TU to reuse its gate, identity checks and
 * ledger cleanup. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/w32_objects.h>
static bool restart_equal(const char *a,const char *b)
{return strlen(a)==strlen(b)&&!strncmp(a,b,strlen(b));}
static bool restart_contains(const char *a,const char *b)
{for(;*a;a++)if(!strncmp(a,b,strlen(b)))return true;return false;}
static void restart_report(struct process *desktop,const char *line)
{
    uint32_t pid,phase,value,flags;
    if(report_unsigned(line,"failed",&value)){restart_observation.failed=true;return;}
    if(report_unsigned(line,"registry",&value)){restart_observation.registry=value;return;}
    if(report_unsigned(line,"notice",&value)){
        if(!report_unsigned(line,"child",&pid)||!report_unsigned(line,"flags",&flags)||value<1||value>4){restart_observation.failed=true;return;}
        if(value==1)restart_observation.prompts++;
        if(value==2)restart_observation.begins++;
        if(value==3)restart_observation.resumes++;
        return;
    }
    if(report_unsigned(line,"exited",&pid)){
        struct process *child=proc_find(pid);
        if(!report_unsigned(line,"phase",&phase)||phase>=3||pid!=restart_observation.children[phase]||
           !child||child->ppid!=desktop->pid||child->state!=PROC_ZOMBIE||child->raw_exit)restart_observation.failed=true;
        else restart_observation.exits++;
        return;
    }
    if(!report_unsigned(line,"child",&pid)||!report_unsigned(line,"phase",&phase)||
       !report_unsigned(line,"service",&value)||phase>=4||phase!=restart_observation.launches){restart_observation.failed=true;return;}
    struct process *child=proc_find(pid),*service=proc_find(value);
    if(!child||child->ppid!=desktop->pid||child->personality!=PROC_WIN32||!restart_equal(child->w32_source_name,W32_RESTART_PATH)||
       !service||service->ppid!=desktop->pid||service->personality!=PROC_NATIVE||!service->w32||service->w32->service!=W32_SERVICE_W32SRV){restart_observation.failed=true;return;}
    restart_observation.children[phase]=pid;restart_observation.services[phase]=value;restart_observation.launches++;
}
static int restart_read(struct vfs_table *t,const char *path,void *bytes,unsigned cap)
{
    int h=vfs_open(t,path,VFS_READ,VFS_DENY_NONE,0);if(h<0)return h;
    size_t n=0;int e=vfs_read(t,h,bytes,cap,&n);int close=vfs_close(t,h);
    return e?e:close?close:(int)n;
}
static bool restart_absent(struct vfs_table *t,const char *p)
{struct fat_entry entry;return vfs_stat(t,p,&entry)==-FS_ENOENT;}
static bool restart_bytes(struct vfs_table *t,const char *p,const char *want)
{char b[512];int n=restart_read(t,p,b,sizeof(b));return n==(int)strlen(want)&&!memcmp(b,want,(unsigned)n);}
static struct process *restart_service(uint32_t desktop,uint32_t previous)
{
    for(unsigned i=0;i<CIUKI_PROCESS_MAX;i++){
        struct process *p=proc_at_slot(i);
        if(p&&p->pid!=previous&&p->ppid==desktop&&p->state==PROC_LIVE&&p->personality==PROC_NATIVE&&
           p->w32&&p->w32->service==W32_SERVICE_W32SRV)return p;
    }
    return 0;
}
static bool restart_link(struct vfs_table *t,unsigned checkpoint)
{
    /* Only the selected restart controller calls this helper. Keep its
     * bounded scratch out of the kernel's 4 KiB single-frame budget. */
    static uint8_t b[4096];const char *path=(checkpoint&1)?W32_RESTART_GROUP "\\First.lnk":W32_RESTART_GROUP "\\Second.lnk";
    int n=restart_read(t,path,b,sizeof(b));if(n<40||memcmp(b,"CIUKLNK3",8))return false;
    unsigned at=40;
    for(unsigned i=0;i<5;i++){
        unsigned size=(unsigned)b[16+i*4]|(unsigned)b[17+i*4]<<8|(unsigned)b[18+i*4]<<16|(unsigned)b[19+i*4]<<24;
        if(size>(unsigned)n-at)return false;
        if(i==2)return size==3&&!memcmp(b+at,(checkpoint&1)?"--S":"--M",3);
        at+=size;
    }
    return false;
}
static bool restart_release(struct process *p)
{uint32_t one=1;return !ua_write(p->memory,W32_RESTART_RESULT_VA+offsetof(struct w32_restart_result,release),&one,4);}
static int probe_w32_restart(void)
{
    const char *name="w32-restart",*failure="controller_setup";rec_emit(name,"BEGIN",0);
    const struct fb_device *display=fbdev_get();
    if(!display->present||(g_boot.flags&CBI_F_SAFE_MODE)||!native_payload_present()){
        rec_emit(name,"END","status=FAIL reason=desktop_unavailable");return 1;
    }
    int gate=storage_enable_write(storage_get(),2);rec_emit(name,"DATA","case=write-gate expected=0 observed=%d",gate);
    if(gate){rec_emit(name,"END","status=FAIL reason=write_gate");return 1;}
    struct desktop_ledger initial,final;struct proc_ledger before,after;
    desktop_snapshot(&initial);proc_snapshot(&before);
    struct process *owner=0,*server=0;uint32_t generation=0;
    static struct vfs_table table;bool table_open=false,pass=false;
    struct w32_restart_result result={0};struct w32_restart_facts facts={0};
    struct w32_object *helper_objects[5]={0},*runner_objects[5]={0};
    uint32_t dde_seen=0,helper_pids[5]={0},helper_exits=0,interrupted=0,recovered=0,final_pid=0;
    memset(&native_reports,0,sizeof(native_reports));memset(&restart_observation,0,sizeof(restart_observation));
    if(proc_prepare(proc_supervisor(),&owner))goto finished;
    proc_publish(owner,0);
    if(supervisor_spawn_desktop_probe(&server)<0){failure="desktop_launch";goto cleanup;}
    server->ppid=owner->pid;native_reports.active=true;native_reports.server=server->pid;restart_observation.active=true;
    if(vfs_table_init(&storage_get()->vfs,&table,2)){failure="filesystem";goto cleanup;}table_open=true;
    uint64_t deadline=g_ticks+10000;
    while(native_live(server)&&!native_reports.survivor_stage&&g_ticks<deadline)task_sleep_ms(1);
    if(!native_live(server)||native_reports.survivor_stage!=2){failure="survivor_timeout";goto cleanup;}
    if(!native_command(server,GATE_SNAPSHOT,&generation)||!native_command_argument(server,GATE_W32_FIXTURE,&generation,"restart-exercise")){failure=native_command_failure;goto cleanup;}
    deadline=g_ticks+360000;
    while(g_ticks<deadline&&native_live(server)&&!restart_observation.failed&&!native_reports.invalid){
        for(unsigned i=0;i<CIUKI_PROCESS_MAX;i++){
            struct process *p=proc_at_slot(i);struct w32_restart_result r;
            if(!p||p->state!=PROC_LIVE||p->personality!=PROC_WIN32||!restart_equal(p->w32_source_name,W32_RESTART_PATH)||
               ua_read(p->memory,&r,W32_RESTART_RESULT_VA,sizeof(r))||r.magic!=W32_RESTART_MAGIC||r.version!=1)continue;
            if(!r.helper){
                if(!restart_observation.launches||p->pid!=restart_observation.children[restart_observation.launches-1])continue;
                if(r.phase==0&&r.checkpoint&&r.checkpoint<=4&&r.checkpoint==dde_seen+1){
                    if(!restart_link(&table,r.checkpoint)||r.groups!=(r.checkpoint+1)/2||!restart_release(p)){restart_observation.failed=true;break;}
                    dde_seen++;
                }
                if(w32_restart_valid(&r)){result=r;final_pid=p->pid;break;}
                continue;
            }
            unsigned h=r.helper-1;
            if(h>=5||(helper_pids[h]&&helper_pids[h]!=p->pid)){restart_observation.failed=true;break;}
            if(helper_pids[h])continue;
            struct process *parent=proc_find(p->ppid);
            if(!parent||parent->ppid!=server->pid||parent->personality!=PROC_WIN32||!restart_equal(parent->w32_source_name,"/System/Win32/runonce.exe")){
                restart_observation.failed=true;break;
            }
            uint32_t held_pid=p->pid,runner_pid=parent->pid;
            const char *orders[]={"S","SM","SMU","SMUR","SMURT"};
            char journal[160];int j=restart_read(&table,W32_RESTART_JOURNAL,journal,sizeof(journal)-1);
            if(j>0)journal[j]=0;
            /* Journal handed count and durable order both observed while
             * the child is held, before the desktop can request NEXT. */
            char expected[24];ksnprintf(expected,sizeof(expected),"handed=%u\n",h<3?h+1:h-2);
            if(r.registry_absent!=1||!restart_bytes(&table,W32_RESTART_ORDER,orders[h])||j<=0||
               !restart_contains(journal,"phase=2\n")||!restart_contains(journal,expected)){restart_observation.failed=true;break;}
            if(h==3){
                struct process *svc=restart_service(server->pid,0);
                if(!r.interrupt||!svc){restart_observation.failed=true;break;}
                interrupted=svc->pid;proc_stop(svc,0,9);
                uint64_t until=g_ticks+20000;struct process *next=0;
                while(g_ticks<until&&native_live(server)&&!(next=restart_service(server->pid,interrupted)))task_sleep_ms(5);
                if(!next){restart_observation.failed=true;break;}recovered=next->pid;
            }
            /* Filesystem I/O and service startup may sleep. Reacquire the
             * published identities before touching either process again. */
            p=proc_find(held_pid);parent=proc_find(runner_pid);
            if(!p||p->state!=PROC_LIVE||!parent||parent->state!=PROC_LIVE||p->ppid!=runner_pid){restart_observation.failed=true;break;}
            if(!p->w32||!p->w32->identity||!parent->w32||!parent->w32->identity){restart_observation.failed=true;break;}
            helper_objects[h]=p->w32->identity;runner_objects[h]=parent->w32->identity;
            w32_retain(helper_objects[h]);w32_retain(runner_objects[h]);
            helper_pids[h]=p->pid;if(!restart_release(p)){restart_observation.failed=true;break;}
        }
        if(final_pid)break;
        task_sleep_ms(5);
    }
    if(!final_pid||restart_observation.failed||native_reports.invalid){failure="fixture_observation";goto cleanup;}
    /* Retain the real kernel process identities before releasing the child.
     * A waiting runonce.exe may exit and its child may be adopted/reaped
     * before the next poll; the retained identity preserves exact exit facts. */
    uint64_t exits_deadline=g_ticks+5000;
    for(;;){
        helper_exits=0;
        for(unsigned i=0;i<5;i++)if(helper_objects[i]&&runner_objects[i]&&helper_objects[i]->signalled&&!helper_objects[i]->exit_code&&
            runner_objects[i]->signalled&&!runner_objects[i]->exit_code)helper_exits++;
        if(helper_exits==5||g_ticks>=exits_deadline)break;
        task_sleep_ms(1);
    }
    facts.dde=dde_seen==4&&result.raw_commands==6&&result.ddeml_commands==6&&result.links==4&&result.groups==2&&restart_absent(&table,W32_RESTART_GROUP);
    facts.session=result.queries==8&&result.cancelled==2&&result.accepted==6&&result.logoff==4&&restart_observation.exits==3&&restart_observation.begins==4;
    facts.wininit=restart_bytes(&table,W32_RESTART_TEMP "f507d.final","restart-data")&&restart_absent(&table,W32_RESTART_TEMP "f507d.source")&&
        restart_absent(&table,W32_RESTART_TEMP "f507d.middle")&&restart_absent(&table,W32_RESTART_TEMP "f507d.delete")&&restart_absent(&table,W32_RESTART_INI)&&!restart_absent(&table,"C:/WINDOWS/WININIT.BAK");
    facts.runonce=helper_exits==5&&result.registry_absent==5&&restart_observation.registry==5&&restart_bytes(&table,W32_RESTART_ORDER,"SMURT");
    facts.recovery=interrupted&&recovered&&interrupted!=recovered&&restart_absent(&table,W32_RESTART_JOURNAL)&&facts.runonce;
    facts.desktop=restart_observation.prompts==4&&restart_observation.resumes==4&&native_live(server)&&native_command(server,GATE_SNAPSHOT,&generation);
    struct process *child=proc_find(final_pid);pass=child&&restart_release(child);
    uint64_t until=g_ticks+5000;
    while((child=proc_find(final_pid))&&child->state!=PROC_ZOMBIE&&g_ticks<until)task_sleep_ms(1);
    pass=pass&&child&&child->state==PROC_ZOMBIE&&!child->raw_exit;
    pass=w32_restart_publish(&result,&facts,rec_emit)&&pass;failure="restart_predicates";
    rec_emit(name,"DATA","case=recovery_process interrupted=%u recovered=%u helpers=%u exits=%u first_error=%u",interrupted,recovered,helper_exits,restart_observation.exits,result.first_error);
cleanup:
    for(unsigned i=0;i<5;i++){w32_release(helper_objects[i]);w32_release(runner_objects[i]);}
    restart_observation.active=false;native_reports.active=false;
    if(table_open)vfs_table_destroy(&table);
    if(server){
        /* Descendants include runonce.exe and its child; stop deepest first
         * so waiters cannot create new work during cleanup. */
        for(unsigned depth=0;depth<4;depth++){
            static struct ciuki_proc_record children[CIUKI_PROCESS_MAX];unsigned n=proc_list_snapshot(children,CIUKI_PROCESS_MAX,0);
            for(unsigned i=0;i<n;i++){
                struct process *p=proc_find(children[i].pid),*parent=p?proc_find(p->ppid):0;
                if(p&&parent&&parent->ppid==server->pid)desktop_remove(parent,p);
            }
        }
        static struct ciuki_proc_record children[CIUKI_PROCESS_MAX];unsigned n=proc_list_snapshot(children,CIUKI_PROCESS_MAX,0);
        for(unsigned i=0;i<n;i++)if(children[i].ppid==server->pid)desktop_remove(server,proc_find(children[i].pid));
        desktop_remove(owner,server);
    }
    if(owner){proc_stop(owner,0,0);proc_collect();}
finished:
    desktop_snapshot(&final);proc_snapshot(&after);
    bool objects=desktop_ledgers_equal(&initial,&final),processes=native_proc_equal(&before,&after);
    rec_emit(name,"DATA","case=ledger objects_equal=%u processes_equal=%u",objects,processes);
    if(!objects||!processes){pass=false;failure="cleanup_ledger";}
    if(pass)rec_emit(name,"END","status=PASS");else rec_emit(name,"END","status=FAIL reason=%s",failure);
    return pass?0:1;
}
