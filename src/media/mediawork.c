/* Preemptible DPMI audio decoder. The desktop posts one mailbox command at a
 * time; PCM is committed to shared memory before `completed` is published. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include "media_abi.h"
#include "decoder.h"

static int mailbox_selector;
static __dpmi_meminfo mapping;
static struct media_header request;
static char path[260];
static int16_t pcm[MEDIA_OUTPUT_FRAMES*2];
static void worker_yield(void)
{
    __dpmi_regs r;memset(&r,0,sizeof r);r.x.ax=0x1680;r.x.flags=0x0200;
    __dpmi_simulate_real_mode_interrupt(0x2F,&r);
}
static void perform(void)
{
    request.error=request.output_bytes=request.output_frames=0;
    switch(request.operation){
    case MEDIA_OPEN:
        if(!request.input_bytes||request.input_bytes>=sizeof path){request.error=1;break;}
        movedata(mailbox_selector,MEDIA_INPUT,_my_ds(),(unsigned)path,request.input_bytes);
        path[request.input_bytes]=0;
        if(!media_decoder_open(path,&request.sample_rate,&request.total_frames))request.error=2;
        break;
    case MEDIA_DECODE:
        if(!media_decoder_read(pcm,MEDIA_OUTPUT_FRAMES,&request.output_frames))request.error=3;
        request.output_bytes=request.output_frames*4UL;
        request.position+=request.output_frames;
        break;
    case MEDIA_SEEK:
        if(!media_decoder_seek(request.position))request.error=4;
        request.output_frames=request.output_bytes=0;
        break;
    case MEDIA_CLOSE:media_decoder_close();request.output_frames=request.output_bytes=0;break;
    case MEDIA_EXIT:media_decoder_close();break;
    default:request.error=5;break;
    }
}
int main(int argc,char **argv)
{
    unsigned long physical;char *end;uint32_t sequence;
    if(sizeof request!=64||argc!=2)return 1;
    physical=strtoul(argv[1],&end,16);
    if(*end||physical<0x100000UL||physical>0xFFF00000UL)return 2;
    mapping.address=physical;mapping.size=MEDIA_BYTES;
    if(__dpmi_physical_address_mapping(&mapping))return 3;
    mailbox_selector=__dpmi_allocate_ldt_descriptors(1);if(mailbox_selector<0)return 4;
    if(__dpmi_set_segment_base_address(mailbox_selector,mapping.address)||
       __dpmi_set_segment_limit(mailbox_selector,MEDIA_BYTES-1))return 5;
    movedata(mailbox_selector,0,_my_ds(),(unsigned)&request,sizeof request);
    if(request.magic!=MEDIA_MAGIC||request.abi!=MEDIA_ABI)return 6;
    /* Do not publish the bootstrap copy back as a whole header: the parent
       can submit OPEN after the initial read, and that write would erase it. */
    for(;;){
        sequence=_farpeekl(mailbox_selector,8);
        if(sequence==request.completed){worker_yield();continue;}
        movedata(mailbox_selector,0,_my_ds(),(unsigned)&request,sizeof request);
        if(request.magic!=MEDIA_MAGIC||request.abi!=MEDIA_ABI)break;
        if(request.input_bytes>259||request.output_bytes>MEDIA_OUTPUT_CAPACITY){request.error=6;request.output_bytes=0;}
        else perform();
        if(request.output_bytes)movedata(_my_ds(),(unsigned)pcm,mailbox_selector,MEDIA_OUTPUT,request.output_bytes);
        ++request.heartbeat;
        movedata(_my_ds(),(unsigned)&request,mailbox_selector,0,sizeof request);
        _farpokel(mailbox_selector,12,sequence);request.completed=sequence;
        if(request.operation==MEDIA_EXIT)break;
    }
    media_decoder_close();
    __dpmi_free_ldt_descriptor(mailbox_selector);__dpmi_free_physical_address_mapping(&mapping);return 0;
}
