/* 32-bit, preemptible user-mode web services. No desktop painting here. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include "worker_abi.h"
#ifndef CWW_CORE_ONLY
#include "worker_tls.h"
#include "worker_ca.h"
#include "worker_rng.h"
#include "worker_js.h"
#include "worker_page.h"
#endif

static int mailbox_selector;
static __dpmi_meminfo mapping;
static struct cww_header request;
static uint8_t input[CWW_DATA_BYTES+1],output[CWW_DATA_BYTES];
static void stage(uint32_t value)
{request.state=value;_farpokel(mailbox_selector,(unsigned long)__builtin_offsetof(struct cww_header,state),value);}
static void worker_yield(void)
{
    __dpmi_regs r;
    memset(&r,0,sizeof r);
    r.x.ax=0x1680;
    r.x.flags=0x0200;
    __dpmi_simulate_real_mode_interrupt(0x2F,&r);
}
#ifndef CWW_CORE_ONLY
static wca_store roots;
static int entropy_ready;
static int entropy(void *opaque,void *out,uint32_t bytes)
{int ok;(void)opaque;stage(104);ok=entropy_ready&&wrng_read(out,bytes)==0;stage(105);return ok;}
static void tls_output(void)
{
    uint32_t count=0;
    int rc=wtls_pull_record(output,sizeof output,&count);
    if(count){request.flags|=CWW_F_CIPHER;request.output_bytes=count;}
    else{
        rc=wtls_pull_plaintext(output,sizeof output,&count);
        if(count){request.flags|=CWW_F_PLAIN;request.output_bytes=count;}
    }
    if(wtls_ready())request.flags|=CWW_F_READY;
    if(rc==WTLS_CLOSED)request.flags|=CWW_F_CLOSED;
    if(rc==WTLS_FAILED)request.error=0x10000UL+(uint32_t)wtls_last_error();
}
static void js_eval(void)
{
    char *source=(char *)input;
    uint32_t length=request.input_bytes;
    if(request.aux==1){
        FILE *file;long bytes;
        if(length>127||strncmp(source,"C:\\NET\\",7)){request.error=1;return;}
        file=fopen(source,"rb");if(!file){request.error=2;return;}
        if(fseek(file,0,SEEK_END)||(bytes=ftell(file))<0||bytes>65536){fclose(file);request.error=3;return;}
        rewind(file);source=malloc((size_t)bytes+1);
        if(!source){fclose(file);request.error=4;return;}
        length=(uint32_t)bytes;
        if(fread(source,1,length,file)!=length){fclose(file);free(source);request.error=5;return;}
        fclose(file);source[length]=0;
    }
    request.error=wjs_eval(source,length,output,sizeof output,&request.output_bytes);
    if(request.error)request.output_bytes=0;
    if(source!=(char *)input)free(source);
}
#endif
static void perform(void)
{
    uint32_t consumed=0;
    request.error=request.flags=request.output_bytes=request.consumed=0;
    switch(request.operation){
    case CWW_PING:
        memcpy(output,input,request.input_bytes);request.output_bytes=request.input_bytes;
        request.flags=CWW_F_READY;break;
    case CWW_EXIT:break;
#ifndef CWW_CORE_ONLY
    case CWW_TLS_OPEN:
        stage(101);
        if(!entropy_ready){request.error=0x21000UL+(uint32_t)wrng_last_error();break;}
        stage(102);
        if(!roots.count&&wca_load_pem("C:\\SYSTEM\\WEB\\CACERT.PEM",&roots)!=0){request.error=0x20002;break;}
        if(!request.input_bytes||request.input_bytes>253){request.error=1;break;}
        stage(103);
        if(wtls_init((char *)input,request.aux,entropy,0,roots.anchors,roots.count)==WTLS_FAILED)
            request.error=0x10000UL+(uint32_t)wtls_last_error();
        else tls_output();stage(106);break;
    case CWW_TLS_STEP:
        if(wtls_step(input,request.input_bytes,&consumed)==WTLS_FAILED)
            request.error=0x10000UL+(uint32_t)wtls_last_error();
        request.consumed=consumed;tls_output();break;
    case CWW_TLS_WRITE:
        if(wtls_send_plaintext(input,request.input_bytes,&consumed)==WTLS_FAILED)
            request.error=0x10000UL+(uint32_t)wtls_last_error();
        request.consumed=consumed;tls_output();break;
    case CWW_TLS_CLOSE:wtls_close();tls_output();break;
    case CWW_JS_RESET:wjs_reset();break;
    case CWW_JS_ELEMENT:
        if(request.input_bytes<2){request.error=1;break;}
        consumed=(uint32_t)input[0]|((uint32_t)input[1]<<8);
        if(consumed>request.input_bytes-2){request.error=1;break;}
        request.error=wjs_set_element((char *)input+2,consumed,
            (char *)input+2+consumed,request.input_bytes-2-consumed);break;
    case CWW_JS_EVAL:case CWW_JS_EVENT:js_eval();break;
    case CWW_PAGE_SCAN:
        if(request.input_bytes>127||strncmp((char *)input,"C:\\NET\\",7)){request.error=1;break;}
        stage(201);request.error=wpage_scan((char *)input,output,sizeof output,&request.output_bytes);stage(202);break;
    case CWW_PAGE_RESOURCE:
        if(request.input_bytes>127||strncmp((char *)input,"C:\\NET\\",7)){request.error=1;break;}
        stage(203);request.error=wpage_resource(request.aux,(char *)input);stage(204);break;
    case CWW_PAGE_RUN:
        if(request.input_bytes>127||strcmp((char *)input,"C:\\NET\\CWRENDER.HTM")){request.error=1;break;}
        output[0]=0;
        stage(205);request.error=wpage_run((char *)input,(char *)output,sizeof output);stage(206);
        if(output[0]){request.output_bytes=strlen((char *)output);request.flags|=CWW_F_MORE;}break;
#endif
    default:request.error=1;break;
    }
    (void)consumed;
}
int main(int argc,char **argv)
{
    unsigned long physical;char *end;uint32_t sequence;
    if(sizeof request!=64||argc!=2)return 1;
    physical=strtoul(argv[1],&end,16);
    if(*end||physical<0x100000UL||physical>0xFFF00000UL)return 2;
    mapping.address=physical;mapping.size=CWW_BYTES;
    if(__dpmi_physical_address_mapping(&mapping))return 3;
    mailbox_selector=__dpmi_allocate_ldt_descriptors(1);
    if(mailbox_selector<0)return 4;
    if(__dpmi_set_segment_base_address(mailbox_selector,mapping.address)||
       __dpmi_set_segment_limit(mailbox_selector,CWW_BYTES-1))return 5;
    movedata(mailbox_selector,0,_my_ds(),(unsigned)&request,sizeof request);
    if(request.magic!=CWW_MAGIC||request.abi!=CWW_ABI)return 6;
#ifndef CWW_CORE_ONLY
    stage(1);
    entropy_ready=wrng_init((uint32_t)physical+CWW_DMA_OFFSET)==0;
    stage(2);
    wjs_reset();
    stage(3);
#endif
    for(;;){
        sequence=_farpeekl(mailbox_selector,8);
        if(sequence==request.completed){worker_yield();continue;}
        movedata(mailbox_selector,0,_my_ds(),(unsigned)&request,sizeof request);
        if(request.magic!=CWW_MAGIC||request.abi!=CWW_ABI)break;
        if(request.input_bytes>CWW_DATA_BYTES){request.error=2;request.output_bytes=0;}
        else{
            movedata(mailbox_selector,CWW_INPUT,_my_ds(),(unsigned)input,request.input_bytes);
            input[request.input_bytes]=0;perform();
        }
        if(request.output_bytes>CWW_DATA_BYTES){request.error=3;request.output_bytes=0;}
        if(request.output_bytes)movedata(_my_ds(),(unsigned)output,mailbox_selector,CWW_OUTPUT,request.output_bytes);
        ++request.heartbeat;
        /* Keep completed unchanged until all response bytes are visible. */
        movedata(_my_ds(),(unsigned)&request,mailbox_selector,0,sizeof request);
        _farpokel(mailbox_selector,12,sequence);request.completed=sequence;
        if(request.operation==CWW_EXIT)break;
    }
#ifndef CWW_CORE_ONLY
    wrng_shutdown();
    wca_free(&roots);
#endif
    __dpmi_free_ldt_descriptor(mailbox_selector);
    __dpmi_free_physical_address_mapping(&mapping);return 0;
}
