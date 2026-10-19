/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Memory-only libmspack adapter. */
#include "../services/files.h"
#include <mspack.h>
#define LIMIT (64u*1024u*1024u)
struct memory {const uint8_t *input;uint8_t *output;uint32_t size,position,capacity;int writing;};
struct decoder {struct mspack_system system;struct memory input,output;};
struct mspack_system *mspack_default_system;
int mspack_valid_system(struct mspack_system *s){return s&&s->open&&s->close&&s->read&&s->write&&s->seek&&s->tell&&s->alloc&&s->free&&s->copy;}
static struct mspack_file *open_memory(struct mspack_system *s,const char *name,int mode)
{
    (void)name;struct decoder *d=(struct decoder *)s;struct memory *m=mode==MSPACK_SYS_OPEN_READ?&d->input:&d->output;
    m->position=0;return (struct mspack_file *)m;
}
static void close_memory(struct mspack_file *f){(void)f;}
static int read_memory(struct mspack_file *f,void *out,int n)
{
    struct memory *m=(struct memory *)f;if(n<0||m->writing)return -1;
    if((uint32_t)n>m->size-m->position)n=(int)(m->size-m->position);
    w32_copy(out,m->input+m->position,(uint32_t)n);m->position+=(uint32_t)n;return n;
}
static int write_memory(struct mspack_file *f,void *in,int n)
{
    struct memory *m=(struct memory *)f;if(n<0||!m->writing||(uint32_t)n>LIMIT-m->position)return -1;
    uint32_t need=m->position+(uint32_t)n;
    if(need>m->capacity){uint32_t capacity=m->capacity?m->capacity:4096;while(capacity<need)capacity*=2;
        uint8_t *p=w32lz_alloc(capacity);if(!p)return -1;if(m->size)w32_copy(p,m->output,m->size);w32lz_free(m->output);m->output=p;m->capacity=capacity;}
    if(n)w32_copy(m->output+m->position,in,(uint32_t)n);m->position=need;if(need>m->size)m->size=need;return n;
}
static int seek_memory(struct mspack_file *f,off_t offset,int mode)
{
    struct memory *m=(struct memory *)f;int64_t base=mode==MSPACK_SYS_SEEK_START?0:mode==MSPACK_SYS_SEEK_CUR?m->position:mode==MSPACK_SYS_SEEK_END?m->size:-1;
    if(base<0||offset < -base||offset>(int64_t)m->size-base)return -1;m->position=(uint32_t)(base+offset);return 0;
}
static off_t tell_memory(struct mspack_file *f){return ((struct memory *)f)->position;}
static void message_memory(struct mspack_file *f,const char *text,...){(void)f;(void)text;}
static void *allocate(struct mspack_system *s,size_t n){(void)s;return n<=LIMIT?w32lz_alloc((uint32_t)n):NULL;}
static void copy_memory(void *src,void *dest,size_t n){w32_copy(dest,src,(uint32_t)n);}
int w32lz_decode(const uint8_t *input,uint32_t n,uint8_t **output,uint32_t *size,char *filename)
{
    struct decoder d={.system={open_memory,close_memory,read_memory,write_memory,seek_memory,tell_memory,message_memory,allocate,w32lz_free,copy_memory,NULL},
        .input={.input=input,.size=n},.output={.writing=1}};
    *output=NULL;*size=0;if(filename)filename[0]=0;int result=MSPACK_ERR_DATAFORMAT;
    if(n>=4&&w32_u32(input)==0x44445a53){
        struct msszdd_decompressor *z=mspack_create_szdd_decompressor(&d.system);if(!z)return -5;
        struct msszddd_header *h=z->open(z,"input");
        if(h){if(h->length<=LIMIT){result=z->extract(z,h,"output");if(!result&&d.output.size!=(uint64_t)h->length)result=MSPACK_ERR_DATAFORMAT;}z->close(z,h);}
        mspack_destroy_szdd_decompressor(z);
    }else if(n>=4&&w32_u32(input)==0x4a41574b){
        /* The upstream bit reader may synthesize zero bits at EOF. Require
         * the actual MSZIP block terminator, so a truncated tail cannot be
         * accepted solely because its decoded length happens to match. */
        if(n>=14&&w32_u16(input+8)==4){
            uint32_t at=w32_u16(input+10);int terminated=0;
            while(at<=n&&n-at>=2){uint32_t block=w32_u16(input+at);at+=2;if(!block){terminated=1;break;}
                if(block<2||block>n-at||input[at]!='C'||input[at+1]!='K')break;at+=block;}
            if(!terminated)return -3;
        }
        struct mskwaj_decompressor *z=mspack_create_kwaj_decompressor(&d.system);if(!z)return -5;
        struct mskwajd_header *h=z->open(z,"input");
        if(h){if(!(h->headers&MSKWAJ_HDR_HASLENGTH)||h->length<=LIMIT){
                result=z->extract(z,h,"output");if(!result&&(h->headers&MSKWAJ_HDR_HASLENGTH)&&d.output.size!=(uint64_t)h->length)result=MSPACK_ERR_DATAFORMAT;
                if(filename&&h->filename){uint32_t len=w32_strlen(h->filename);if(len<256)w32_copy(filename,h->filename,len+1);}}
            z->close(z,h);}
        mspack_destroy_kwaj_decompressor(z);
    }
    if(result){w32lz_free(d.output.output);return result==MSPACK_ERR_NOMEMORY?-5:-3;}
    *output=d.output.output;*size=d.output.size;return 0;
}
