/* Bounded process records, virtual I/O, crash facts and desktop mailbox.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/w32_objects.h>
#include <ciuki/supervisor.h>
#include <ciuki/sha256.h>
struct w32_stub_record {struct w32_stub_record *next;char name[212];};
struct w32_diagnostic_state {
    struct w32_stub_record *stubs;
    uint32_t warnings,suppressed,stub_count,mask,cw;
    uint32_t timestamp,image_size;
    uint32_t refused;
    uint64_t pit_last;
    uint16_t pit_latch;
    uint8_t pit_byte;
    bool configured,crashed,exited,dialog_queued,pit_latched;
};
static struct ciuki_w32_fault w32_fault_queue[16];
static unsigned w32_fault_head,w32_fault_count;
static int diag_cmp(const char *a,const char *b){size_t n=strlen(a);return n==strlen(b)?strncmp(a,b,n):1;}
static struct w32_diagnostic_state *diag_state(struct process *p){
    if(!p->w32&&w32_process_init(p))return 0;
    if(!p->w32->diagnostics)p->w32->diagnostics=w32_alloc(sizeof(struct w32_diagnostic_state));
    return p->w32->diagnostics;
}
int w32_diagnostic_image(struct process *p,uint32_t stamp,uint32_t size){
    struct w32_diagnostic_state *s=diag_state(p);if(!s)return -ENOMEM;s->timestamp=stamp;s->image_size=size;return 0;
}
static void diag_name(char *out,unsigned cap,const char *name){
    const char *base=name?name:"unknown";
    for(const char *p=base;*p;p++)if(*p=='/'||*p=='\\')base=p+1;
    unsigned n=0;for(;base[n]&&n+1<cap;n++){
        unsigned c=(uint8_t)base[n];out[n]=(char)((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'||c=='@'||c=='?'||c=='$'||c==':'?c:'_');
    }out[n]=0;
}
static void diag_path(char out[260],const char *path){
    unsigned n=0;for(;path[n]&&n<259;n++){unsigned c=(uint8_t)path[n];
        out[n]=(char)((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'||c=='/'||c=='\\'||c==':'?c:'_');}
    out[n]=0;
}
static bool diag_token(const char *s){
    if(!*s)return false;
    for(;*s;s++){unsigned c=(uint8_t)*s;if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'||c=='!'||c=='@'||c=='?'||c=='$'||c==':'||c=='#'))return false;}
    return true;
}
static void diag_module(struct process *p,uint32_t eip,char name[64],uint32_t *base){
    uint32_t ldr=0,next=0;
    if(!p->w32_peb||ua_read(p->memory,&ldr,p->w32_peb+12,4)||ldr>UINT32_MAX-12||
        ua_read(p->memory,&next,ldr+12,4))return;
    for(unsigned n=0;next&&next!=ldr+12&&n<256;n++){
        uint32_t row[13];if(next>UINT32_MAX-sizeof(row)||ua_read(p->memory,row,next,sizeof(row)))return;
        if(eip>=row[6]&&eip-row[6]<row[8]){
            uint32_t wide=row[12];unsigned bytes=row[11]&65535,at=0;
            if(bytes&1||bytes>510||wide>UINT32_MAX-bytes)return;
            for(unsigned i=0;i<bytes/2&&at<63;i++){uint16_t ch;if(ua_read(p->memory,&ch,wide+2*i,2))return;name[at++]=ch>=32&&ch<127?(char)ch:'_';}
            name[at]=0;*base=row[6];return;
        }
        if(next==row[0])return;next=row[0];
    }
}
void w32_diagnostic_record(struct proc_thread *t,const struct ciuki_w32_diagnostic *a){
    struct w32_diagnostic_state *s=diag_state(t->process);if(!s)return;
    const char *p=(const char *)a->payload;
    if(a->kind==3){
        if(strncmp(p,"stub ",5)||!diag_token(p+5))return;
        for(struct w32_stub_record *r=s->stubs;r;r=r->next)if(!diag_cmp(r->name,p+5))return;
        if(s->stub_count>=4096)return;
        struct w32_stub_record *r=w32_alloc(sizeof(*r));if(!r)return;
        unsigned n=(unsigned)strlen(p+5);if(n>=sizeof(r->name)){w32_free(r,sizeof(*r));return;}
        memcpy(r->name,p+5,n+1);r->next=s->stubs;s->stubs=r;s->stub_count++;
        klog("[win32] stub %s pid=%u tid=%u",r->name,t->process->pid,t->tid);return;
    }
    /* Legacy payload shape is event check=reason module=basename. Never
     * accept arbitrary debug/dialog text at this protected boundary. */
    char event[16],reason[128],module[64];unsigned i=0;
    while(p[i]&&p[i]!=' '&&i+1<sizeof(event)){event[i]=p[i];i++;}event[i]=0;
    if(diag_cmp(event,"load")&&diag_cmp(event,"warning"))return;
    if(strncmp(p+i," check=",7))return;p+=i+7;i=0;
    while(p[i]&&p[i]!=' '&&i+1<sizeof(reason)){reason[i]=p[i];i++;}reason[i]=0;
    if(strncmp(p+i," module=",8)||!diag_token(reason))return;
    diag_name(module,sizeof(module),p+i+8);
    bool warning=!diag_cmp(event,"warning")||!diag_cmp(reason,"newer-subsystem-version")||
        !diag_cmp(reason,"raw-data-zero-filled")||!diag_cmp(reason,"delay-iat-without-names")||
        !diag_cmp(reason,"bound-iat-without-names");
    if(warning&&s->warnings>=64){if(s->suppressed!=UINT32_MAX)s->suppressed++;return;}
    if(warning)s->warnings++;
    klog("[win32] %s check=%s module=%s pid=%u tid=%u",event,reason,module,t->process->pid,t->tid);
}
static bool diag_enqueue(const struct ciuki_w32_fault *f){
    if(w32_fault_count==16){klog("[win32] warning check=dialog-queue-full pid=%u",f->pid);return false;}
    w32_fault_queue[(w32_fault_head+w32_fault_count)%16]=*f;w32_fault_count++;return true;
}
void w32_refusal_queue(struct process *p,uint32_t reason,const char *module,uint32_t error){
    if(reason<1||reason>W32_REFUSE_WIN16)return;
    struct w32_diagnostic_state *s=p->w32?p->w32->diagnostics:0;
    if(s){if(s->refused&(1u<<reason))return;s->refused|=1u<<reason;}
    const char *reasons[]={"","msi","driver","runtime","missing-dll","missing-export","invalid-image","no-server","win16"};
    struct ciuki_w32_fault f={.kind=W32_REPORT_REFUSE,.pid=p->pid,.code=reason,.offset=error};
    diag_name(f.exe,sizeof(f.exe),p->name);diag_name(f.module,sizeof(f.module),module);diag_enqueue(&f);
    klog("[win32] refuse %s pid=%u module=%s error=%u",reasons[reason],p->pid,f.module,error);
}
void w32_crash_capture(struct proc_thread *t,const struct ciuki_w32_context *c,uint32_t code,const char *module,uint32_t base){
    struct w32_diagnostic_state *s=diag_state(t->process);if(!s||s->crashed)return;
    s->crashed=true;t->process->w32->exit_reason=W32_EXIT_CRASH;
    struct ciuki_w32_fault f={.kind=W32_REPORT_CRASH,.pid=t->process->pid,.tid=t->tid,.code=code,.eip=c->eip,.offset=base<=c->eip?c->eip-base:0};
    uint32_t regs[9]={c->eax,c->ebx,c->ecx,c->edx,c->esi,c->edi,c->ebp,c->esp,c->eflags};memcpy(f.registers,regs,sizeof(regs));
    diag_name(f.exe,sizeof(f.exe),t->process->name);diag_name(f.module,sizeof(f.module),module);
    uint32_t resolved=base;diag_module(t->process,c->eip,f.module,&resolved);
    if(resolved<=c->eip)f.offset=c->eip-resolved;
    char clean[64]={0};diag_name(clean,sizeof(clean),f.module);memcpy(f.module,clean,sizeof(clean));
    for(unsigned i=0;i<8;i++)if(c->esp<=UINT32_MAX-4*i&&ua_range(t->process->memory,c->esp+4*i,4,PROT_READ)&&!ua_read(t->process->memory,&f.stack[i],c->esp+4*i,4))f.valid|=1u<<i;
    /* Short keys keep the complete numeric facts inside one protected record. */
    w32_log("[win32] crash pid=%u tid=%u code=%08x module=%s+%08x eip=%08x r=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x s=%02x:%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
        f.pid,f.tid,f.code,f.module,f.offset,f.eip,regs[0],regs[1],regs[2],regs[3],regs[4],regs[5],regs[6],regs[7],regs[8],f.valid,
        f.stack[0],f.stack[1],f.stack[2],f.stack[3],f.stack[4],f.stack[5],f.stack[6],f.stack[7]);
    s->dialog_queued=diag_enqueue(&f);
}
/* G13 (ER-1, SY-8): a native program killed by a processor fault goes to the
 * same desktop mailbox as a Win32 crash, so the desktop shows "has performed
 * an illegal operation" and writes the crash file (the Dr. Watson snapshot,
 * RK-tools Table 27.1). Only while a desktop is running or being restarted:
 * nobody would drain the queue otherwise. */
extern bool supervisor_desktop_expected(void) __attribute__((weak));
void native_fault_report(struct proc_thread *t,const struct trap_frame *tf,uint32_t address){
    struct process *p=t->process;
    if(!tf||(tf->cs&3)!=3||p->personality!=PROC_NATIVE)return;
    uint32_t error=tf->err&0xffffffu;
    klog("[fault] native pid=%u tid=%u name=%s vector=%u error=%08x eip=%08x address=%08x",
        p->pid,t->tid,p->name,tf->vector,tf->err,tf->eip,address);
    if(!supervisor_desktop_pid()&&!(supervisor_desktop_expected&&supervisor_desktop_expected()))return;
    for(unsigned i=0;i<w32_fault_count;i++)
        if(w32_fault_queue[(w32_fault_head+i)%16].pid==p->pid&&w32_fault_queue[(w32_fault_head+i)%16].kind==W32_REPORT_NATIVE_FAULT)return;
    struct ciuki_w32_fault f={.kind=W32_REPORT_NATIVE_FAULT,.pid=p->pid,.tid=t->tid,.code=(tf->vector&0xffu)|error<<8,.eip=tf->eip,.offset=address};
    uint32_t regs[9]={tf->eax,tf->ebx,tf->ecx,tf->edx,tf->esi,tf->edi,tf->ebp,tf->user_esp,tf->eflags};memcpy(f.registers,regs,sizeof(regs));
    diag_name(f.exe,sizeof(f.exe),p->name);
    for(unsigned i=0;i<8;i++)
        if(tf->user_esp<=UINT32_MAX-4*i-3&&ua_range(p->memory,tf->user_esp+4*i,4,PROT_READ)&&!ua_read(p->memory,&f.stack[i],tf->user_esp+4*i,4))f.valid|=1u<<i;
    diag_enqueue(&f);
}
static bool diag_crash_closed(void *arg){struct process *p=arg;return p->state!=PROC_LIVE||!proc_find(supervisor_desktop_pid());}
uint32_t w32_report_call(struct proc_thread *t,struct ciuki_w32_report_args *a){
    struct process *p=t->process;
    if(a->action==W32_REPORT_NEXT||a->action==W32_REPORT_CLOSE){
        if(!p->w32||p->w32->service!=W32_SERVICE_DESKTOP||p->pid!=supervisor_desktop_pid())return W32_STATUS_ACCESS_DENIED;
        if(a->action==W32_REPORT_CLOSE){
            struct process *victim=proc_find(a->code);
            if(victim&&victim->w32&&victim->w32->diagnostics&&victim->w32->diagnostics->crashed)proc_stop(victim,victim->raw_exit,0);
            return 0;
        }
        if(w32_fault_count){
            if(copy_to_user(a->buffer,&w32_fault_queue[w32_fault_head],sizeof(struct ciuki_w32_fault)))return W32_STATUS_ACCESS_VIOLATION;
            w32_fault_head=(w32_fault_head+1)%16;w32_fault_count--;a->result=1;
        }return 0;
    }
    if(p->personality!=PROC_WIN32)return W32_STATUS_ACCESS_DENIED;
    if(a->action==W32_REPORT_REASON){if(a->code!=W32_EXIT_RESTART)return W32_STATUS_INVALID_PARAMETER;p->w32->exit_reason=a->code;return 0;}
    char module[64]={0};
    if(a->module){for(unsigned i=0;i<sizeof(module)-1;i++){if(a->module>UINT32_MAX-i||copy_from_user(module+i,a->module+i,1))return W32_STATUS_ACCESS_VIOLATION;if(!module[i])break;}}
    if(a->action==W32_REPORT_CRASH){
        struct ciuki_w32_context c;if(copy_from_user(&c,a->context,sizeof(c)))return W32_STATUS_ACCESS_VIOLATION;
        w32_crash_capture(t,&c,a->code,module,a->base);p->raw_exit=(int)a->code;
        if(p->w32->diagnostics&&p->w32->diagnostics->dialog_queued&&p->w32->endpoint[0]){
            while(!diag_crash_closed(p))kwait_wait_until(&w32_changed,diag_crash_closed,p,deadline_after_ms(1000));
        }
        return 0;
    }
    if(a->code<1||a->code>7)return W32_STATUS_INVALID_PARAMETER;
    w32_refusal_queue(p,a->code,module,a->base);return 0;
}
uint32_t w32_initial_cw(struct process *p){
    return p&&p->w32&&p->w32->diagnostics&&(p->w32->diagnostics->mask&W32_SHIM_BIT(W32_SH_FPU_CW))?p->w32->diagnostics->cw:0x027f;
}
uint32_t w32_task_cw(const struct task *task){struct proc_thread *t=proc_thread_for(task);return w32_initial_cw(t?t->process:0);}
uint32_t w32_compat_call(struct proc_thread *t,struct ciuki_w32_compat_args *a){
    struct process *p=t->process;
    if(a->action==1){
        uint8_t chunk[256],digest[32];struct sha256_ctx hash;sha256_init(&hash);
        uint32_t source=a->source?a->source:p->w32_source,bytes=a->source?a->source_bytes:p->w32_source_bytes;
        if(!source||bytes<84||bytes>512u*1024*1024||!ua_range(p->memory,source,bytes,PROT_READ))return W32_STATUS_INVALID_IMAGE_FORMAT;
        struct ua_pin pin;if(ua_pin(p->memory,&pin,source,bytes,false))return W32_STATUS_ACCESS_VIOLATION;
        for(uint32_t at=0;at<bytes;){unsigned n=bytes-at;if(n>sizeof(chunk))n=sizeof(chunk);
            if(ua_read(p->memory,chunk,source+at,n)){ua_unpin(p->memory,&pin);return W32_STATUS_ACCESS_VIOLATION;}sha256_update(&hash,chunk,n);at+=n;kpreempt_point();}
        ua_unpin(p->memory,&pin);
        sha256_final(&hash,digest);char hex[65];sha256_hex(digest,hex);
        if(copy_to_user(a->settings,hex,65))return W32_STATUS_ACCESS_VIOLATION;
        uint32_t off;if(ua_read(p->memory,&off,source+60,4)||off>bytes-84)return W32_STATUS_INVALID_IMAGE_FORMAT;
        if(ua_read(p->memory,&a->timestamp,source+off+8,4)||ua_read(p->memory,&a->image_size,source+off+80,4))return W32_STATUS_ACCESS_VIOLATION;
        struct w32_diagnostic_state *s=diag_state(p);if(!s)return W32_STATUS_NO_MEMORY;
        s->timestamp=a->timestamp;s->image_size=a->image_size;
        a->file_size=bytes;return 0;
    }
    struct w32_diagnostic_state *s=diag_state(p);if(!s)return W32_STATUS_NO_MEMORY;
    if(s->configured)return W32_STATUS_ACCESS_DENIED;
    struct ciuki_w32_compat c;if(copy_from_user(&c,a->settings,sizeof(c)))return W32_STATUS_ACCESS_VIOLATION;
    if(c.mask&~4095u||c.heap_pad>65536||c.memory_mib>4096||c.version>5||c.scale>8||c.width>2048||c.height>2048||
        c.fpu_cw>65535||(c.fpu_cw&~0x1f7fu)||(c.fpu_cw&0x300)==0x100||c.native_dlls[255]||c.w_exports[511])return W32_STATUS_INVALID_PARAMETER;
    char list[1024];unsigned at=0;list[0]=0;
    const char *names[]={"version","ddraw-555","ddraw-scale","ddraw-windowed","heap-zero","heap-pad","memstatus-cap","io-pit","io-gameport","dll-native","fpu-cw","w-stubs"};
    for(unsigned i=0;i<12;i++)if(c.mask&W32_SHIM_BIT(i)){
        int n=ksnprintf(list+at,sizeof(list)-at,"%s%s@1",at?",":"",names[i]);if(n<0||(unsigned)n>=sizeof(list)-at)return W32_STATUS_INVALID_PARAMETER;at+=(unsigned)n;
        uint32_t value=i==W32_SH_HEAP_PAD?c.heap_pad:i==W32_SH_MEMSTATUS_CAP?c.memory_mib:i==W32_SH_FPU_CW?c.fpu_cw:i==W32_SH_DDRAW_SCALE?c.scale:0;
        const char *arg=i==W32_SH_DLL_NATIVE?c.native_dlls:i==W32_SH_W_STUBS?c.w_exports:0;
        if(i==W32_SH_VERSION){const char *v[]={"","95","me","nt4","2000","xp"};arg=v[c.version];}
        if(arg){
            for(const char *p=arg;*p;p++){unsigned ch=(uint8_t)*p;if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='.'||ch=='_'||ch=='-'||ch=='|'||ch=='@'||ch=='?'||ch=='$'))return W32_STATUS_INVALID_PARAMETER;}
            n=ksnprintf(list+at,sizeof(list)-at,"(%s)",arg);
        }else if(i==W32_SH_DDRAW_SCALE&&!c.scale)n=ksnprintf(list+at,sizeof(list)-at,"(mode:%ux%u)",c.width,c.height);
        else if(value||i==W32_SH_HEAP_PAD)n=ksnprintf(list+at,sizeof(list)-at,"(%u)",value);
        else n=0;
        if(n<0||(unsigned)n>=sizeof(list)-at)return W32_STATUS_INVALID_PARAMETER;at+=(unsigned)n;
    }
    s->mask=c.mask;s->cw=c.fpu_cw;s->configured=true;
    uint32_t stamp=s->timestamp,image=s->image_size;
    char exe[260];diag_path(exe,p->w32_source_name[0]?p->w32_source_name:p->name);
    w32_log("[win32] start pid=%u exe=%s pe=%08x:%08x shims=%s",p->pid,exe,stamp,image,list);
    if(c.mask&W32_SHIM_BIT(W32_SH_FPU_CW)){
        struct ciuki_w32_context context={0};fpu_w32_export(t->task,&context);context.flags=W32_CONTEXT_FLOAT;
        context.floating.control=c.fpu_cw;fpu_w32_import(t->task,&context);
    }
    return 0;
}
bool w32_io_emulate(struct proc_thread *t,struct trap_frame *tf,const uint8_t *code,unsigned n){
    struct w32_diagnostic_state *s=t->process->w32?t->process->w32->diagnostics:0;
    if(!s||!n)return false;
    uint8_t op=code[0];bool in=op==0xe4||op==0xec,out=op==0xe6||op==0xee;
    if(!in&&!out)return false;
    bool imm=op==0xe4||op==0xe6;if(imm&&n<2)return false;
    uint32_t port=imm?code[1]:tf->edx&65535;
    if(port==0x201&&(s->mask&W32_SHIM_BIT(W32_SH_IO_GAMEPORT))){if(in)tf->eax=(tf->eax&~255u)|255;}
    else if((s->mask&W32_SHIM_BIT(W32_SH_IO_PIT))&&((port==0x40&&in)||(port==0x43&&out&&(tf->eax&255)==0))){
        if((out&&!s->pit_latched)||(in&&!s->pit_latched&&!s->pit_byte)){
            uint64_t now=arch_w32_counter();if(now<s->pit_last)now=s->pit_last;s->pit_last=now;
            s->pit_latch=(uint16_t)(0u-(uint16_t)now);s->pit_byte=0;s->pit_latched=true;
        }
        if(in){uint32_t v=s->pit_byte?s->pit_latch>>8:s->pit_latch&255;tf->eax=(tf->eax&~255u)|v;
            s->pit_byte^=1;if(!s->pit_byte)s->pit_latched=false;}
    }else return false;
    tf->eip+=imm?2:1;return true;
}
void w32_diagnostic_exit(struct process *p){
    struct w32_diagnostic_state *s=p->w32->diagnostics;
    if(p->personality!=PROC_WIN32||!p->published||!s||s->exited)return;s->exited=true;
    if(s->suppressed)klog("[win32] warning pid=%u suppressed=%u",p->pid,s->suppressed);
    const char *reasons[]={"normal","crash","killed","restart"};unsigned reason=p->w32->exit_reason;if(reason>3)reason=2;
    klog("[win32] exit pid=%u code=%08x reason=%s peak_commit=%u peak_handles=%u threads=%u",p->pid,(uint32_t)p->raw_exit,reasons[reason],p->memory->peak_backing*PAGE_SIZE,p->w32->peak_handles,p->w32->peak_threads);
}
void w32_process_stopping(struct process *p,uint32_t signal){
    if(p->personality!=PROC_WIN32)return;
    struct w32_diagnostic_state *s=diag_state(p);if(!s)return;
    if(signal&&p->w32->exit_reason==W32_EXIT_NORMAL)p->w32->exit_reason=W32_EXIT_KILLED;
    if(!s->configured){
        char name[260];diag_path(name,p->w32_source_name[0]?p->w32_source_name:p->name);
        w32_log("[win32] start pid=%u exe=%s pe=%08x:%08x shims=",p->pid,name,s->timestamp,s->image_size);s->configured=true;
    }
}
void w32_diagnostic_cleanup(struct process *p){
    struct w32_diagnostic_state *s=p->w32->diagnostics;if(!s)return;
    while(s->stubs){struct w32_stub_record *r=s->stubs;s->stubs=r->next;w32_free(r,sizeof(*r));}
    w32_free(s,sizeof(*s));p->w32->diagnostics=0;
}
