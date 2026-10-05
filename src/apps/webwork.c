#include "webwork.h"
static u16 handle,vseg,voff,generation;
static int vm=-1,pending,closing,exit_sent,kill_sent;
static u32 address,sequence;
static u16 started,close_started;
static struct cww_header header;
static u32 port_read32(u16 port);
#pragma aux port_read32 = ".386" "in eax,dx" "mov edx,eax" "shr edx,16" parm [dx] value [dx ax];
static void port_write32(u16 port,u32 value);
#pragma aux port_write32 = ".386" "movzx eax,cx" "shl eax,16" "mov ax,bx" "mov dx,si" "out dx,eax" parm [si] [cx bx] modify [ax dx];
static u8 port_read8(u16 port);
#pragma aux port_read8 = "in al,dx" parm [dx] value [al];
static void port_write8(u16 port,u8 value);
#pragma aux port_write8 = "out dx,al" parm [dx] [al];
static int reset_rng(void)
{
    u32 saved=port_read32(0xCF8),bar;u16 device,base,tries;
    for(device=0;device<32;++device){
        port_write32(0xCF8,0x80000000UL|((u32)device<<11));
        if(port_read32(0xCFC)!=0x10051AF4UL)continue;
        port_write32(0xCF8,0x80000010UL|((u32)device<<11));bar=port_read32(0xCFC);
        if(!(bar&1)||bar>65535UL)continue;base=(u16)bar&~3u;
        port_write8(base+18,0);
        for(tries=0;tries<32;++tries)if(!port_read8(base+18))break;
        port_write32(0xCF8,saved);return tries<32;
    }
    port_write32(0xCF8,saved);return 1;
}
static u8 packet[1026],tail[128],fcb[16],before[16],after[16];
#pragma pack(push,1)
static struct {u16 env,to,ts,f1o,f1s,f2o,f2s;} params;
#pragma pack(pop)
static int vmcall(struct regs *r)
{
    r->ds=r->es=app_seg();
    if(!vseg)return 1;far_regs(vseg,voff,r);return r->flags&1;
}
static int status(void)
{
    struct regs r;if(vm<1)return 0;
    mem_set(&r,0,sizeof r);r.ax=0x4A;r.bx=vm;
    if(vmcall(&r))return -1;
    if(generation&&r.cx!=generation)return 0;return r.bx;
}
void webwork_debug(void)
{
    char info[128];
    if(!handle||!webstore_read(handle,0,&header,sizeof header))return;
    str_copy(info,"op=");fmt_u32(info+str_len(info),header.operation);
    str_cat(info," stage=");fmt_u32(info+str_len(info),header.state);
    str_cat(info," sequence=");fmt_u32(info+str_len(info),header.sequence);
    str_cat(info," completed=");fmt_u32(info+str_len(info),header.completed);
    str_cat(info," error=");fmt_u32(info+str_len(info),header.error);
    str_cat(info," output=");fmt_u32(info+str_len(info),header.output_bytes);
    str_cat(info," flags=");fmt_u32(info+str_len(info),header.flags);
    app_log("[WEBWORK] state",info);
}
int webwork_close(void)
{
    struct regs r;int state=status();
    if(state<0)return 0;
    if(state==2){
        mem_set(&r,0,sizeof r);r.ax=0x45;r.bx=vm;
        (void)vmcall(&r);
        state=0;
    }
    if(state==1){
        if(!closing){closing=1;close_started=HOST.ticks;}
        if(!exit_sent){
            if(pending){
                if(!webstore_read(handle,0,&header,sizeof header))return 0;
                if(header.magic!=CWW_MAGIC||header.abi!=CWW_ABI)return 0;
                if(header.completed==sequence)pending=0;
                else goto grace_timeout;
            }
            if(!webstore_read(handle,0,&header,sizeof header))return 0;
            header.operation=CWW_EXIT;header.error=header.flags=header.output_bytes=header.consumed=0;
            header.input_bytes=0;header.aux=0;header.sequence=sequence;
            if(!webstore_write(handle,0,&header,sizeof header))return 0;
            if(!++sequence)++sequence;
            if(!webstore_write(handle,8,&sequence,4))return 0;
            pending=1;exit_sent=1;
        }
grace_timeout:
        if(!kill_sent&&(u16)(HOST.ticks-close_started)>=18*2){
            app_log("[WEBWORK] forced shutdown",0);
            mem_set(&r,0,sizeof r);r.ax=0x45;r.bx=vm;
            if(vmcall(&r))return 0;
            kill_sent=1;
        }
        return 0;
    }
    if(address&&!reset_rng())return 0;
    if(closing)app_log("[WEBWORK] shutdown complete",0);
    /* The VM is confirmed stopped before its shared physical pages are freed. */
    vm=-1;generation=0;pending=closing=exit_sent=kill_sent=0;sequence=0;
    close_started=0;
    if(address){webstore_unlock(handle);address=0;}
    if(handle){webstore_free(handle);handle=0;}
    return 1;
}
int webwork_open(void)
{
    struct regs r;char hex[9];int i;
    if(closing&&!webwork_close())return 0;
    if(vm>0&&status()==1)return 1;
    if(!webwork_close())return 0;
    mem_set(&r,0,sizeof r);r.ax=0x1684;r.bx=0x4349;intr(0x2F,&r);
    vseg=r.es;voff=r.di;if(!vseg)return 0;
    handle=webstore_alloc(CWW_BYTES);if(!handle)return 0;
    address=webstore_lock(handle);if(address<0x100000UL)goto failed;
    fmt_hex4(hex,(u16)(address>>16));fmt_hex4(hex+4,(u16)address);
    app_log("[WEBWORK] mailbox",hex);
    mem_set(&header,0,sizeof header);header.magic=CWW_MAGIC;header.abi=CWW_ABI;
    if(!webstore_write(handle,0,&header,sizeof header))goto failed;
    mem_set(&r,0,sizeof r);r.ax=0x48;r.di=(u16)before;if(vmcall(&r))goto failed;
    fmt_hex4(hex,(u16)(address>>16));fmt_hex4(hex+4,(u16)address);
    str_copy((char *)tail+1," \\VM\\DPMIRUN.COM /B /C C:\\SYSTEM\\APPS\\WEBWORK.EXE ");
    str_cat((char *)tail+1,hex);tail[0]=str_len((char *)tail+1);tail[tail[0]+1]=13;
    params.env=0;params.to=(u16)tail;params.ts=app_seg();
    params.f1o=params.f2o=(u16)fcb;params.f1s=params.f2s=app_seg();
    mem_set(&r,0,sizeof r);r.ax=0x4B00;r.dx=(u16)"\\VM\\VMFORK.COM";
    r.bx=(u16)&params;r.ds=r.es=app_seg();if(intr(0x21,&r))goto failed;
    mem_set(&r,0,sizeof r);r.ax=0x48;r.di=(u16)after;if(vmcall(&r))goto failed;
    for(i=1;i<4;++i)if(!before[i*4]&&after[i*4]){vm=i;break;}
    if(vm<1)goto failed;
    mem_set(&r,0,sizeof r);r.ax=0x4A;r.bx=vm;if(vmcall(&r))goto failed;
    generation=r.cx;sequence=0;pending=closing=exit_sent=kill_sent=0;return 1;
failed:
    webwork_close();return 0;
}
int webwork_submit(u16 op,const void *data,u16 bytes,u32 aux)
{
    const u8 *src=data;u16 at,n;
    if(closing||pending||bytes>CWW_DATA_BYTES||(!data&&bytes)||!webwork_open())return 0;
    for(at=0;at<bytes;at+=n){
        n=bytes-at;if(n>1024)n=1024;mem_copy(packet,src+at,n);packet[n]=0;
        if(!webstore_write(handle,CWW_INPUT+at,packet,(n+1)&~1))return 0;
    }
    if(!webstore_read(handle,0,&header,sizeof header))return 0;
    header.operation=op;header.error=header.flags=header.output_bytes=header.consumed=0;
    header.input_bytes=bytes;header.aux=aux;header.sequence=sequence;
    if(!webstore_write(handle,0,&header,sizeof header))return 0;
    if(!++sequence)++sequence;
    if(!webstore_write(handle,8,&sequence,4))return 0;
    started=HOST.ticks;pending=1;return 1;
}
int webwork_poll(struct cww_header *reply,void *data,u16 capacity)
{
    u8 *dst=data;u16 at,n,bytes;
    if(!pending||status()<=0)return -1;
    if(!webstore_read(handle,0,&header,sizeof header))return -1;
    if(header.magic!=CWW_MAGIC||header.abi!=CWW_ABI)return -1;
    if(header.completed!=sequence){
        if((u16)(HOST.ticks-started)>18*45){webwork_debug();webwork_close();return -1;}
        return 0;
    }
    pending=0;if(header.output_bytes>CWW_DATA_BYTES||(data&&header.output_bytes>capacity))return -1;
    bytes=data?(u16)header.output_bytes:0;
    for(at=0;at<bytes;at+=n){
        n=bytes-at;if(n>1024)n=1024;
        if(!webstore_read(handle,CWW_OUTPUT+at,packet,(n+1)&~1))return -1;
        mem_copy(dst+at,packet,n);
    }
    if(reply)mem_copy(reply,&header,sizeof header);return 1;
}
int webwork_result(u16 offset,void *data,u16 bytes)
{
    u16 skip=offset&1;
    if(!handle||pending||(u32)offset+bytes>header.output_bytes||bytes>1024)return 0;
    if(!webstore_read(handle,CWW_OUTPUT+(offset&~1u),packet,(bytes+skip+1)&~1))return 0;
    mem_copy(data,packet+skip,bytes);return 1;
}
