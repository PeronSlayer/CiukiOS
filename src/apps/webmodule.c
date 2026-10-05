#include "webmodule.h"
int webmodule_load(struct webmodule *m,const char *path)
{
    u8 header[22];u16 paras;int h,n;u8 __far *p;u32 size,i;long bytes;
    if(m->segment)return 1;
    h=dos_open(path,0);if(h<0)return 0;
    n=dos_read(h,header,sizeof header);
    if(n!=sizeof header||mem_cmp(header,"CAPP",4))goto fail;
    paras=header[6]|((u16)header[7]<<8);bytes=dos_seek(h,0,2);
    if(paras<32||paras>4096||bytes<22||bytes>0xFE00L||
       (u32)bytes>((u32)paras<<4)-0x100UL)goto fail;
    m->segment=dos_alloc(paras);if(!m->segment)goto fail;
    size=(u32)paras<<4;p=(u8 __far *)((u32)m->segment<<16);
    for(i=0;i<size;++i)p[(u16)i]=0;
    dos_seek(h,0,0);n=dos_read_far(h,m->segment,0x100,0x7F00);
    if(n==0x7F00){int more=dos_read_far(h,m->segment,0x8000,0x7F00);if(more<0)n=-1;else n+=more;}
    if((u16)n!=(u16)bytes){webmodule_free(m);goto fail;}
    m->entry=header[12]|((u16)header[13]<<8);
    if(m->entry<0x116||(u32)m->entry>=0x100UL+bytes){webmodule_free(m);goto fail;}
    dos_close(h);return 1;
fail:
    dos_close(h);return 0;
}
int webmodule_call(struct webmodule *m,int event,void *request)
{
    struct regs r;if(!m->segment)return 0;
    /* Keep timing and service context current for network/UI submodules. */
    far_copy(m->segment,0x108,app_seg(),0x108,4);
    far_copy(m->segment,0x116,app_seg(),0x116,sizeof(struct host));
    mem_set(&r,0,sizeof r);r.ax=event;r.dx=app_seg();r.bx=(u16)request;
    r.ds=r.es=app_seg();far_regs(m->segment,m->entry,&r);return r.ax;
}
void webmodule_free(struct webmodule *m)
{if(m->segment)dos_free(m->segment);m->segment=m->entry=0;}
