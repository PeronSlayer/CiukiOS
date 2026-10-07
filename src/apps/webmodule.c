#include "webmodule.h"

static u16 module_alloc(u16 paras,u16 *error,u16 *largest)
{
    struct regs r;
    mem_set(&r,0,sizeof r);r.ax=0x4800;r.bx=paras;r.ds=r.es=app_seg();
    if(intr(0x21,&r)) {
        *error=r.ax;*largest=r.bx;return 0;
    }
    *error=0;*largest=0;return r.ax;
}

static int load_fail(struct webmodule *m,int h,u16 error,u16 dos_error,u16 largest)
{
    if(m->segment)dos_free(m->segment);
    m->segment=m->entry=0;
    m->error=error;m->dos_error=dos_error;m->largest_block=largest;
    if(h>=0)dos_close(h);
    return 0;
}

int webmodule_load(struct webmodule *m,const char *path)
{
    u8 header[22];u16 paras,dos_error,largest,read_bytes;int h,n,more;
    u8 __far *p;u32 size,i;long bytes,seek;
    if(m->segment){m->error=WEBMODULE_OK;m->dos_error=m->largest_block=0;return 1;}
    m->error=WEBMODULE_OK;m->dos_error=m->largest_block=0;
    h=dos_open(path,0);
    if(h<0)return load_fail(m,-1,WEBMODULE_OPEN,(u16)-h,0);
    n=dos_read(h,header,sizeof header);
    if(n<0)return load_fail(m,h,WEBMODULE_HEADER_READ,(u16)-n,0);
    if(n!=sizeof header)return load_fail(m,h,WEBMODULE_HEADER_READ,0,0);
    if(mem_cmp(header,"CAPP",4))return load_fail(m,h,WEBMODULE_HEADER,0,0);
    paras=header[6]|((u16)header[7]<<8);bytes=dos_seek(h,0,2);
    if(bytes<0)return load_fail(m,h,WEBMODULE_SEEK,(u16)-bytes,0);
    if(paras<32||paras>4096||bytes<22||bytes>0xFE00L||
       (u32)bytes>((u32)paras<<4)-0x100UL)
        return load_fail(m,h,WEBMODULE_SIZE,0,0);
    m->segment=module_alloc(paras,&dos_error,&largest);
    if(!m->segment)return load_fail(m,h,WEBMODULE_ALLOC,dos_error,largest);
    size=(u32)paras<<4;p=(u8 __far *)((u32)m->segment<<16);
    for(i=0;i<size;++i)p[(u16)i]=0;
    seek=dos_seek(h,0,0);
    if(seek<0)return load_fail(m,h,WEBMODULE_REWIND,(u16)-seek,0);
    n=dos_read_far(h,m->segment,0x100,0x7F00);
    if(n<0)return load_fail(m,h,WEBMODULE_IMAGE_READ,(u16)-n,0);
    read_bytes=(u16)n;
    if(n==0x7F00) {
        more=dos_read_far(h,m->segment,0x8000,0x7F00);
        if(more<0)return load_fail(m,h,WEBMODULE_IMAGE_READ,(u16)-more,0);
        read_bytes=(u16)(read_bytes+(u16)more);
    }
    if((u32)read_bytes!=(u32)bytes)return load_fail(m,h,WEBMODULE_IMAGE_READ,0,0);
    m->entry=header[12]|((u16)header[13]<<8);
    if(m->entry<0x116||(u32)m->entry>=0x100UL+(u32)bytes)
        return load_fail(m,h,WEBMODULE_ENTRY,0,0);
    dos_close(h);return 1;
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
{if(m->segment)dos_free(m->segment);m->segment=m->entry=0;m->error=WEBMODULE_OK;m->dos_error=m->largest_block=0;}

const char *webmodule_error_text(u16 error)
{
    switch(error) {
    case WEBMODULE_OPEN:return "file open";
    case WEBMODULE_HEADER_READ:return "header read";
    case WEBMODULE_HEADER:return "invalid CAPP header";
    case WEBMODULE_SEEK:return "file size query";
    case WEBMODULE_SIZE:return "invalid image size";
    case WEBMODULE_ALLOC:return "DOS memory allocation";
    case WEBMODULE_REWIND:return "file rewind";
    case WEBMODULE_IMAGE_READ:return "image read or length";
    case WEBMODULE_ENTRY:return "invalid entry point";
    default:return "unknown loader failure";
    }
}
