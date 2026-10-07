/* 16-bit desktop client for the preemptible MEDIAWORK.EXE worker. */
#include "mediawork.h"
#include "webstore.h"

static u16 handle,vseg,voff,generation;
static int vm=-1,pending,closing,exit_sent,kill_sent;
static u32 address,sequence;
static u16 close_started;
static struct media_header header;
static u8 tail[128],fcb[16],before[16],after[16],packet[260];
#pragma pack(push,1)
static struct {u16 env,to,ts,f1o,f1s,f2o,f2s;} params;
#pragma pack(pop)

static int vmcall(struct regs *r)
{
    r->ds=r->es=app_seg();if(!vseg)return 1;far_regs(vseg,voff,r);return r->flags&1;
}
static int status(void)
{
    struct regs r;if(vm<1)return 0;mem_set(&r,0,sizeof r);r.ax=0x4A;r.bx=vm;
    if(vmcall(&r))return -1;if(generation&&r.cx!=generation)return 0;return r.bx;
}
static void release_mailbox(void)
{
    if(address){webstore_unlock(handle);address=0;}
    if(handle){webstore_free(handle);handle=0;}
    sequence=0;pending=closing=exit_sent=kill_sent=0;vm=-1;generation=0;
}
static int launch(void)
{
    struct regs r;char hex[9],error[64];int i;const char *stage="short path";
    mem_set(&r,0,sizeof r);
    if(dos_short_path("C:\\SYSTEM\\APPS\\MEDIAWORK.EXE",(char *)packet)<0||!packet[0])goto failed;
    stage="session entry";
    mem_set(&r,0,sizeof r);r.ax=0x1684;r.bx=0x4349;intr(0x2F,&r);
    vseg=r.es;voff=r.di;if(!vseg)goto failed;
    stage="XMS allocation";handle=webstore_alloc(MEDIA_BYTES);if(!handle)goto failed;
    stage="XMS lock";
    address=webstore_lock(handle);if(address<0x100000UL)goto failed;
    mem_set(&header,0,sizeof header);header.magic=MEDIA_MAGIC;header.abi=MEDIA_ABI;
    stage="mailbox initialize";if(!webstore_write(handle,0,&header,sizeof header))goto failed;
    stage="VM list before";
    mem_set(&r,0,sizeof r);r.ax=0x48;r.di=(u16)before;if(vmcall(&r))goto failed;
    fmt_hex4(hex,(u16)(address>>16));fmt_hex4(hex+4,(u16)address);
    str_copy((char *)tail+1," \\VM\\DPMIRUN.COM /B /C ");
    str_cat((char *)tail+1,(char *)packet);str_cat((char *)tail+1," ");
    str_cat((char *)tail+1,hex);tail[0]=str_len((char *)tail+1);tail[tail[0]+1]=13;
    params.env=0;params.to=(u16)tail;params.ts=app_seg();
    params.f1o=params.f2o=(u16)fcb;params.f1s=params.f2s=app_seg();
    stage="DOS EXEC";mem_set(&r,0,sizeof r);r.ax=0x4B00;r.dx=(u16)"\\VM\\VMFORK.COM";
    r.bx=(u16)&params;r.ds=r.es=app_seg();if(intr(0x21,&r))goto failed;
    stage="VM list after";mem_set(&r,0,sizeof r);r.ax=0x48;r.di=(u16)after;if(vmcall(&r))goto failed;
    for(i=1;i<4;++i)if(!before[i*4]&&after[i*4]){vm=i;break;}
    stage="new VM discovery";if(vm<1)goto failed;stage="new VM status";mem_set(&r,0,sizeof r);r.ax=0x4A;r.bx=vm;
    if(vmcall(&r))goto failed;generation=r.cx;return 1;
failed:
    str_copy(error,stage);str_cat(error," AX=");fmt_hex4(error+str_len(error),r.ax);
    app_log("[MEDIA] launch failed",error);
    /* VMFORK may have started the child even when discovery or its final
       status query failed. Stop it before releasing the shared mailbox. */
    if(vm>0){mem_set(&r,0,sizeof r);r.ax=0x45;r.bx=vm;(void)vmcall(&r);}
    if(address){webstore_unlock(handle);address=0;}if(handle){webstore_free(handle);handle=0;}
    vm=-1;generation=0;return 0;
}
static int submit(u32 operation,const void *input,u32 bytes,u32 position)
{
    u8 temp[260];u32 next;
    if(!handle||pending||bytes>259||(!input&&bytes))return 0;
    if(bytes){u16 moved=(u16)((bytes+1UL)&~1UL);mem_copy(temp,input,(u16)bytes);if(bytes&1UL)temp[bytes]=0;if(!webstore_write(handle,MEDIA_INPUT,temp,moved))return 0;}
    if(!webstore_read(handle,0,&header,sizeof header))return 0;
    header.operation=operation;header.error=header.output_bytes=header.output_frames=0;
    header.input_bytes=bytes;header.position=position;header.sequence=sequence;
    if(!webstore_write(handle,0,&header,sizeof header))return 0;
    next=sequence+1;if(!next)++next;
    if(!webstore_write(handle,8,&next,4))return 0;
    sequence=next;pending=1;return 1;
}
int mediawork_start(const char *path)
{
    u16 n=0;
    if(vm>0||handle)return 0;
    if(!path)return 0;
    while(n<259&&path[n])++n;
    if(!n||n>=259)return 0;
    ++n;
    if(!launch())return 0;
    mem_copy(packet,path,n);
    if(!submit(MEDIA_OPEN,packet,n,0)){
        struct regs r;mem_set(&r,0,sizeof r);r.ax=0x45;r.bx=vm;
        if(vm>0)(void)vmcall(&r);
        release_mailbox();return 0;
    }
    return 1;
}
int mediawork_decode(void){return submit(MEDIA_DECODE,0,0,0);}
int mediawork_seek(u32 output_frame){return submit(MEDIA_SEEK,0,0,output_frame);}
int mediawork_poll(struct media_header *reply)
{
    int state=status();if(state<0)return -1;if(state==0)return pending?-1:0;
    if(!handle||!pending||!webstore_read(handle,0,&header,sizeof header))return -1;
    if(header.magic!=MEDIA_MAGIC||header.abi!=MEDIA_ABI)return -1;
    if(header.completed!=sequence)return 0;
    pending=0;if(reply)mem_copy(reply,&header,sizeof header);
    return header.error?-1:1;
}
int mediawork_read(u32 offset,void *buffer,u16 bytes)
{return handle&&bytes<=MEDIA_OUTPUT_CAPACITY&&offset<=MEDIA_OUTPUT_CAPACITY-bytes&&webstore_read(handle,MEDIA_OUTPUT+offset,buffer,bytes);}
int mediawork_close(void)
{
    struct regs r;int state=status();
    if(state<0)return 0;
    if(state==2){
        mem_set(&r,0,sizeof r);r.ax=0x45;r.bx=vm;
        if(vmcall(&r))return 0;
        state=status();
        if(state!=0)return 0;
    }
    if(state==1){
        if(!closing){closing=1;close_started=HOST.ticks;}
        if(pending){
            int rc=mediawork_poll(0);
            if(rc==0){
                if((u16)(HOST.ticks-close_started)<36)return 0;
                mem_set(&r,0,sizeof r);r.ax=0x45;r.bx=vm;
                if(vmcall(&r))return 0;kill_sent=1;return 0;
            }
            if(rc<0&&!kill_sent){mem_set(&r,0,sizeof r);r.ax=0x45;r.bx=vm;if(vmcall(&r))return 0;kill_sent=1;}
        }
        if(!exit_sent&&!pending){
            if(!submit(MEDIA_EXIT,0,0,0))return 0;exit_sent=1;
        }
        if(!kill_sent&&(u16)(HOST.ticks-close_started)>=36){
            mem_set(&r,0,sizeof r);r.ax=0x45;r.bx=vm;if(vmcall(&r))return 0;kill_sent=1;
        }
        return 0;
    }
    release_mailbox();return 1;
}
