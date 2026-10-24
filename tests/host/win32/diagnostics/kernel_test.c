/* Real kernel fault/record machinery on the RAM fixture; no guest execution. */
#define W32_KERNEL_BRIDGE
#include "../kernel_core_test.c"
#undef main
#include <stdarg.h>
static char diagnostic_log[65536];
void klog(const char *format,...){
    size_t at=strlen(diagnostic_log);va_list args;va_start(args,format);
    int n=vsnprintf(diagnostic_log+at,sizeof(diagnostic_log)-at,format,args);va_end(args);
    CHECK(n>=0&&(unsigned)n+at+2<sizeof(diagnostic_log));diagnostic_log[at+(unsigned)n]='\n';diagnostic_log[at+(unsigned)n+1]=0;
}
static struct proc_thread *new_win32(struct process **p){
    CHECK(!proc_prepare(proc_supervisor(),p));(*p)->personality=PROC_WIN32;(*p)->memory->win32=true;
    (*p)->w32_image=0x400000;proc_set_name(*p,"C:\\Games\\fixture.exe");
    struct proc_thread *t;CHECK(!proc_thread_prepare(*p,0x400000,0,0,0,0,0,true,&t));
    struct ciuki_w32_vm a=request(0x400000,4096,W32_MEM_RESERVE|W32_MEM_COMMIT,W32_PAGE_EXECUTE_READWRITE);CHECK(!w32_vm_alloc((*p)->memory,&a));
    (*p)->published=true;(*p)->state=PROC_LIVE;t->task->state=T_RUNNING;t->task->win32=true;g_current=t->task;return t;
}
static void destroy_win32(struct process *p){g_current=&controller;proc_stop(p,p->raw_exit,0);proc_collect();}
static void fault_classes(void){
    const unsigned vectors[]={14,0,13,3,6,14,16,16,16,16,16,16,19};
    const uint32_t statuses[]={0xc0000005,0xc0000094,0xc0000096,0x80000003,0xc000001d,0xc00000fd,0xc0000090,0xc000008d,0xc000008e,0xc0000091,0xc0000093,0xc000008f,0xc00002b4};
    for(unsigned i=0;i<sizeof(vectors)/sizeof(*vectors);i++){
        struct process *p;struct proc_thread *t=new_win32(&p);
        uint8_t fp[512] __attribute__((aligned(16)));t->task->fpu_area=fp;memset(fp,0,sizeof(fp));
        g_cpu_fxsr=i==12;g_cpu_sse=g_cpu_fxsr;fpu_init();t->task->fpu_valid=true;
        if(i>=6&&i<12){put16(fp,0x40);put16(fp+4,(uint16_t)(1u<<(i-6)));}
        if(i==12){put16(fp,0x027f);put32(fp+24,1);}
        uint8_t halt=0xf4;CHECK(!ua_write(p->memory,0x400123,&halt,1));
        uint32_t stack=t->stack_base+t->stack_bytes-32;
        uint32_t words[8]={0,1,0xdeadbeef,3,4,5,6,7};CHECK(!ua_write(p->memory,stack,words,sizeof(words)));
        struct trap_frame tf={.vector=vectors[i],.eip=0x400123,.user_esp=stack,.eax=1,.ebx=2,.ecx=3,.edx=4,.esi=5,.edi=6,.ebp=7,.eflags=0x202,.cs=0x1b,.ds=0x23,.es=0x23,.fs=0x4b,.gs=0x23,.user_ss=0x23};
        uint32_t address=0xdead0000;
        if(i==5){
            address=t->stack_base+4096;t->stack_guard=address;
            CHECK(!ua_w32_change(p->memory,address,4096,W32_VM_ALLOC,W32_PAGE_READWRITE|W32_PAGE_GUARD,0,true));
        }
        w32_fault_count=w32_fault_head=0;expect_exit=true;
        if(!setjmp(exited)){w32_fault(&tf,address);CHECK(false);}expect_exit=false;
        CHECK(w32_fault_count==1);const struct ciuki_w32_fault *f=&w32_fault_queue[w32_fault_head];
        CHECK(f->code==statuses[i]&&f->eip==0x400123&&f->offset==0x123&&f->valid==255);
        CHECK(!strcmp(f->exe,"fixture.exe")&&!strcmp(f->module,"fixture.exe"));
        CHECK(f->registers[0]==1&&f->registers[6]==7&&f->registers[7]==stack&&!memcmp(f->stack,words,sizeof(words)));
        CHECK(p->raw_exit==(int32_t)statuses[i]&&p->w32->exit_reason==W32_EXIT_CRASH);
        fpu_task_release(t->task);g_current=&controller;proc_collect();diagnostic_log[0]=0;
    }
    puts("PASS AV/divide/privileged/breakpoint/illegal/guard-stack/x87-six/SSE fault Details and exception exit codes");
}
static void malformed_and_budgets(void){
    struct process *p;struct proc_thread *t=new_win32(&p);diagnostic_log[0]=0;w32_fault_head=w32_fault_count=0;
    struct ciuki_w32_context c={.eip=0x400123,.esp=0xfffffffc,.eax=0x11223344};
    w32_crash_capture(t,&c,0xc0000005,"C:\\Private\\fixture.dll",0x400000);
    CHECK(!w32_fault_queue[0].valid&&!strcmp(w32_fault_queue[0].module,"fixture.dll"));
    CHECK(strstr(diagnostic_log,"11223344")&&!strstr(diagnostic_log,"Private"));
    w32_crash_capture(t,&c,1,"duplicate",0);CHECK(w32_fault_count==1);
    uint32_t ldr=t->teb+600,entry=t->teb+680,wide=t->teb+800;p->w32_peb=t->teb+512;
    CHECK(!ua_write(p->memory,p->w32_peb+12,&ldr,4)&&!ua_write(p->memory,ldr+12,&entry,4));
    uint16_t dll[]={'s','a','f','e','.','d','l','l'};uint32_t row[13]={0};row[0]=ldr+12;row[6]=0x400000;row[8]=4096;row[11]=sizeof(dll);row[12]=wide;
    CHECK(!ua_write(p->memory,entry,row,sizeof(row))&&!ua_write(p->memory,wide,dll,sizeof(dll)));
    char found[64]="unknown";uint32_t found_base=0;diag_module(p,0x400123,found,&found_base);CHECK(!strcmp(found,"safe.dll")&&found_base==0x400000);
    uint32_t bad=0xfffffffc;CHECK(!ua_write(p->memory,p->w32_peb+12,&bad,4));strcpy(found,"unknown");diag_module(p,0x400123,found,&found_base);CHECK(!strcmp(found,"unknown"));
    struct ciuki_w32_report_args report={.header={1,sizeof(report),W32_REPORT,0},.action=W32_REPORT_CRASH,.code=0xc0000005,.context=t->teb+2048};
    report.code=0xc0000005;CHECK(!ua_write(p->memory,report.context,&c,sizeof(c))&&!w32_report_call(t,&report));
    report.action=W32_REPORT_NEXT;CHECK(w32_report_call(t,&report)==W32_STATUS_ACCESS_DENIED);
    report=(struct ciuki_w32_report_args){.header={1,sizeof(report),W32_REPORT,0},.action=W32_REPORT_NEXT,.buffer=t->teb+4096,.bytes=sizeof(struct ciuki_w32_fault)};
    CHECK(!w32_record_validate(&report,sizeof(report),false));
    p->personality=PROC_NATIVE;p->w32->service=W32_SERVICE_DESKTOP;fake_desktop_pid=p->pid;
    CHECK(!w32_report_call(t,&report)&&report.result==1);struct ciuki_w32_fault handoff;
    CHECK(!ua_read(p->memory,&handoff,report.buffer,sizeof(handoff))&&handoff.code==0xc0000005&&!handoff.valid);
    p->personality=PROC_WIN32;p->w32->service=0;fake_desktop_pid=0;
    struct ciuki_w32_diagnostic a={.kind=3};strcpy((char *)a.payload,"stub fixture.dll!Missing");
    for(unsigned i=0;i<80;i++)w32_diagnostic_record(t,&a);CHECK(p->w32->diagnostics->stub_count==1);
    a.kind=1;strcpy((char *)a.payload,"warning check=bounded module=C:\\Private\\fixture.dll");
    for(unsigned i=0;i<70;i++)w32_diagnostic_record(t,&a);
    CHECK(p->w32->diagnostics->warnings==64&&p->w32->diagnostics->suppressed==6);
    strcpy((char *)a.payload,"load check=missing-export module=fixture.dll");w32_diagnostic_record(t,&a);
    CHECK(strstr(diagnostic_log,"load check=missing-export")&&p->w32->diagnostics->suppressed==6);
    strcpy((char *)a.payload,"debug check=PRIVATE_USER_TEXT module=secret");w32_diagnostic_record(t,&a);
    CHECK(!strstr(diagnostic_log,"PRIVATE_USER_TEXT")&&!strstr(diagnostic_log,"Private"));
    uint32_t h;struct w32_object *o=w32_object_new(W32_OBJ_EVENT);CHECK(o&&!w32_handle_install(p,o,~0u,false,&h));CHECK(!w32_handle_close(p,h));w32_release(o);
    w32_diagnostic_exit(p);CHECK(strstr(diagnostic_log,"suppressed=6")&&strstr(diagnostic_log,"reason=crash")&&strstr(diagnostic_log,"peak_handles=1 threads=1"));
    size_t bytes=strlen(diagnostic_log);w32_diagnostic_exit(p);CHECK(bytes==strlen(diagnostic_log));
    destroy_win32(p);diagnostic_log[0]=0;puts("PASS malformed stack, dedup, 64 warnings plus suppression, redaction and lifecycle peaks");
}
static void io_and_refusals(void){
    struct process *p;struct proc_thread *t=new_win32(&p);struct w32_diagnostic_state *s=diag_state(p);CHECK(s);
    s->mask=W32_SHIM_BIT(W32_SH_IO_PIT)|W32_SHIM_BIT(W32_SH_IO_GAMEPORT)|W32_SHIM_BIT(W32_SH_FPU_CW);s->cw=0x037f;
    CHECK(w32_initial_cw(p)==0x037f&&w32_task_cw(t->task)==0x037f);
    struct trap_frame tf={.eax=0x12340000,.eip=0x400000,.edx=0x201};uint8_t dxin[]={0xec},latch[]={0xe6,0x43},counter[]={0xe4,0x40};
    CHECK(w32_io_emulate(t,&tf,dxin,1)&&tf.eax==0x123400ff&&tf.eip==0x400001);
    tf.eax=0;g_ticks=100;CHECK(w32_io_emulate(t,&tf,latch,2));uint16_t saved=s->pit_latch;
    CHECK(w32_io_emulate(t,&tf,counter,2)&&(tf.eax&255)==(saved&255));g_ticks=1;
    CHECK(w32_io_emulate(t,&tf,counter,2)&&(tf.eax&255)==saved>>8&&s->pit_last>=100*1193u);
    uint8_t bad[]={0xe4,0x60},word[]={0xed},prefix[]={0x66,0xec};uint32_t ip=tf.eip;
    CHECK(!w32_io_emulate(t,&tf,bad,2)&&!w32_io_emulate(t,&tf,word,1)&&!w32_io_emulate(t,&tf,prefix,2)&&tf.eip==ip);
    w32_fault_count=w32_fault_head=0;struct ciuki_file file;
    CHECK(w32_loader_open(p,"C:\\title.msi",&file)==-ENOSYS&&w32_fault_queue[0].code==W32_REFUSE_MSI);
    CHECK(w32_loader_open(p,"msiexec.exe",&file)==-ENOSYS&&w32_loader_open(p,"MSI.DLL",&file)==-ENOSYS);
    for(unsigned i=2;i<=7;i++)w32_refusal_queue(p,i,"fixture.dll",i==2?5:193);
    CHECK(w32_fault_count==7);
    w32_fault_count=w32_fault_head=0;destroy_win32(p);diagnostic_log[0]=0;
    make_pe(pe_data,false);put32(pe_data+0x98+96+14*8,0x1000);put32(pe_data+0x98+100+14*8,16);
    struct ciuki_file f={pe_data,sizeof(pe_data),pe_read,0};struct w32_image im;CHECK(w32_pe_validate(&f,&im)==-ENOEXEC&&!strcmp(im.failure,"unsupported-runtime"));
    puts("PASS virtual PIT/gameport allowlist and monotonic latch, fpu policy and MSI/driver/runtime/loader/server refusals");
}
/* G13 (ER-1, SY-8): a native program killed by a processor fault reaches the
 * desktop mailbox with its registers, stack words and faulting address. */
static void native_faults(void){
    struct process *p;CHECK(!proc_prepare(proc_supervisor(),&p));proc_set_name(p,"/Programs/Crash Test");
    struct proc_thread *t;CHECK(!proc_thread_prepare(p,0x400000,0,0,0,0,0,true,&t));
    p->published=true;p->state=PROC_LIVE;t->task->state=T_RUNNING;g_current=t->task;
    uint32_t stack=t->stack_base+t->stack_bytes-32;uint32_t words[8]={9,8,7,6,5,4,3,2};
    CHECK(!ua_write(p->memory,stack,words,sizeof(words)));
    struct trap_frame tf={.vector=14,.err=6,.eip=0x401234,.cs=0x1b,.user_esp=stack,.eax=1,.ebx=2,.ecx=3,.edx=4,.esi=5,.edi=6,.ebp=7,.eflags=0x10246};
    diagnostic_log[0]=0;w32_fault_count=w32_fault_head=0;
    fake_desktop_pid=0;native_fault_report(t,&tf,0x10);
    CHECK(!w32_fault_count&&strstr(diagnostic_log,"[fault] native")&&strstr(diagnostic_log,"vector=14")&&strstr(diagnostic_log,"eip=00401234"));
    fake_desktop_pid=2;native_fault_report(t,&tf,0x10);
    CHECK(w32_fault_count==1);const struct ciuki_w32_fault *f=&w32_fault_queue[w32_fault_head];
    CHECK(f->kind==W32_REPORT_NATIVE_FAULT&&f->pid==p->pid&&f->tid==t->tid&&f->code==(14u|6u<<8)&&f->eip==0x401234&&f->offset==0x10&&f->valid==255);
    CHECK(!strcmp(f->exe,"Crash_Test")&&!f->module[0]);
    CHECK(f->registers[0]==1&&f->registers[1]==2&&f->registers[2]==3&&f->registers[3]==4&&f->registers[4]==5&&f->registers[5]==6&&
        f->registers[6]==7&&f->registers[7]==stack&&f->registers[8]==0x10246&&!memcmp(f->stack,words,sizeof(words)));
    native_fault_report(t,&tf,0x10);CHECK(w32_fault_count==1);   /* one record per process */
    tf.user_esp=0xfffffffc;                                         /* unreadable stack: valid mask clear */
    struct process *q;CHECK(!proc_prepare(proc_supervisor(),&q));struct proc_thread *u;CHECK(!proc_thread_prepare(q,0x400000,0,0,0,0,0,true,&u));
    q->published=true;q->state=PROC_LIVE;
    native_fault_report(u,&tf,0);CHECK(w32_fault_count==2);
    f=&w32_fault_queue[(w32_fault_head+1)%16];CHECK(f->valid==0&&f->stack[0]==0&&f->registers[7]==0xfffffffc);
    tf.cs=8;tf.user_esp=stack;native_fault_report(u,&tf,0);CHECK(w32_fault_count==2);  /* kernel mode frame: ignored */
    tf.cs=0x1b;q->personality=PROC_WIN32;native_fault_report(u,&tf,0);CHECK(w32_fault_count==2);   /* Win32 has its own path */
    q->personality=PROC_NATIVE;w32_fault_count=w32_fault_head=0;
    for(unsigned i=0;i<16;i++){struct ciuki_w32_fault x={.kind=W32_REPORT_NATIVE_FAULT,.pid=5000+i};diag_enqueue(&x);}
    diagnostic_log[0]=0;native_fault_report(u,&tf,0);
    CHECK(w32_fault_count==16&&strstr(diagnostic_log,"dialog-queue-full"));
    w32_fault_count=w32_fault_head=0;
    g_current=&controller;proc_stop(q,0,0);proc_stop(p,0,0);proc_collect();fake_desktop_pid=0;diagnostic_log[0]=0;
    puts("PASS native fault record: registers, stack, address, vector/error packing, one per process, no desktop = log only");
}
int main(void){
    ram=calloc(HOST_PAGES,PAGE_SIZE);CHECK(ram);g_current=&controller;controller.state=T_RUNNING;proc_init();
    fault_classes();malformed_and_budgets();io_and_refusals();native_faults();
    printf("diagnostic teardown pages=%u mappings=%u heap=%u\n",pages_used,g_user_mappings,heap_blocks);
    CHECK(!pages_used&&!g_user_mappings&&heap_blocks==1);free(ram);return 0;
}
