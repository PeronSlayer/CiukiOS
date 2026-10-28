/* SPDX-License-Identifier: MIT
 * Stream the JSON manifest's file records, counting exact 4 KiB allocation.
 * No copy of the manifest or payload is resident. The JSON subset here is
 * the one emitted by Python json.dumps in scripts/build_cd.py; unsupported
 * syntax, nesting or numbers fail closed. */
#include "native.h"
#include <limits.h>
#include <string.h>
struct reader { FILE *f; int ch; unsigned depth, records; struct st_size *out; };
static void advance(struct reader *r){r->ch=fgetc(r->f);}
static void space(struct reader *r){while(r->ch==' '||r->ch=='\n'||r->ch=='\r'||r->ch=='\t')advance(r);}
static int string(struct reader *r,char *out,size_t cap)
{
    if(r->ch!='"')return -1;
    size_t n=0;advance(r);
    while(r->ch!='"'){
        if(r->ch<32||r->ch==EOF)return -1;
        int c=r->ch;
        if(c=='\\'){
            advance(r);c=r->ch;
            if(c=='u'){
                unsigned v=0;
                for(unsigned i=0;i<4;i++){advance(r);int h=r->ch;
                    if(h>='0'&&h<='9')h-='0';else if(h>='a'&&h<='f')h=h-'a'+10;else if(h>='A'&&h<='F')h=h-'A'+10;else return -1;
                    v=(v<<4)|(unsigned)h;
                }
                /* Manifest paths/components are ASCII. Descriptive strings
                 * can contain Unicode and are ignored by this accounting. */
                c=v<128?(int)v:'?';if(!c)return -1;
            }else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='b')c='\b';else if(c=='f')c='\f';else if(c!='"'&&c!='\\'&&c!='/')return -1;
        }
        if(n+1<cap)out[n++]=(char)c;
        advance(r);
    }
    if(cap)out[n]=0;
    advance(r);return 0;
}
static int value(struct reader *r);
static int component(const char *id)
{
    for(unsigned i=0;i<SETUP_COMP_COUNT;i++)if(!strcmp(id,setup_component_names[i]))return (int)i;
    if(!strcmp(id,"dos-reference")||!strcmp(id,"dos-unified"))return 1;
    if(!strcmp(id,"doom")||!strcmp(id,"quake")||!strcmp(id,"wolf3d"))return 4;
    /* sec-user-02: as libsetup's catalogue (manifest.c): DOS demos with the
     * games, the Quake III Arena demo installer with Windows. */
    if(!strcmp(id,"war2-demo")||!strcmp(id,"duke3d-shareware"))return 4;
    if(!strcmp(id,"q3a-demo"))return 2;
    /* Notices/fonts/runtime libraries belong to the mandatory system. */
    if(!strcmp(id,"jitterentropy")||!strcmp(id,"lwip"))return 0;
    return -1;
}
static int unsigned_number(struct reader *r,uint64_t *out)
{
    if(r->ch<'0'||r->ch>'9')return -1;
    uint64_t n=(unsigned)(r->ch-'0');int leading=r->ch=='0';advance(r);
    if(leading && r->ch>='0'&&r->ch<='9')return -1;
    while(r->ch>='0'&&r->ch<='9'){
        unsigned d=(unsigned)(r->ch-'0');if(n>(UINT64_MAX-d)/10)return -1;
        n=n*10+d;advance(r);
    }
    *out=n;return 0;
}
static int object(struct reader *r)
{
    char id[48]="",path[300]="";uint64_t size=0;int has_size=0;
    advance(r);space(r);
    if(r->ch=='}'){advance(r);return 0;}
    for(;;){
        char key[64];if(string(r,key,sizeof(key)))return -1;
        space(r);if(r->ch!=':')return -1;advance(r);space(r);
        if(!strcmp(key,"component")){if(string(r,id,sizeof(id)))return -1;}
        else if(!strcmp(key,"path")){if(string(r,path,sizeof(path)))return -1;}
        else if(!strcmp(key,"size")){if(unsigned_number(r,&size))return -1;has_size=1;}
        else if(value(r))return -1;
        space(r);if(r->ch=='}'){advance(r);break;}
        if(r->ch!=',')return -1;advance(r);space(r);
    }
    if(has_size && path[0] && (id[0] || !strncmp(path,"SOURCES/",8))){
        int c=id[0]?component(id):7;if(c<0)return -1;struct st_size *s=&r->out[c];
        if(size>UINT64_MAX-4095)return -1;
        uint64_t alloc=(size+4095)&~4095ull;
        if(s->bytes>UINT64_MAX-size || s->alloc_bytes>UINT64_MAX-alloc || s->files==UINT32_MAX)return -1;
        s->bytes+=size;s->alloc_bytes+=alloc;s->files++;r->records++;
    }
    return 0;
}
static int value(struct reader *r)
{
    if(++r->depth>12)return -1;
    space(r);int result=0;
    if(r->ch=='{')result=object(r);
    else if(r->ch=='['){
        advance(r);space(r);
        if(r->ch!=']')for(;;){if(value(r)){result=-1;break;}space(r);if(r->ch==']')break;if(r->ch!=','){result=-1;break;}advance(r);}
        if(!result)advance(r);
    }else if(r->ch=='"')result=string(r,NULL,0);
    else if(r->ch>='0'&&r->ch<='9'){uint64_t n;result=unsigned_number(r,&n);}
    else {
        const char *literal=r->ch=='t'?"true":r->ch=='f'?"false":r->ch=='n'?"null":"";
        if(!*literal)result=-1;
        for(;*literal&&!result;literal++){if(r->ch!=*literal)result=-1;else advance(r);}
    }
    r->depth--;return result;
}
int st_manifest_sizes(FILE *f,struct st_size out[SETUP_COMP_COUNT])
{
    memset(out,0,sizeof(struct st_size)*SETUP_COMP_COUNT);
    struct reader r={.f=f,.out=out};advance(&r);space(&r);
    if(r.ch!='{'||value(&r))return -1;
    space(&r);if(r.ch!=EOF||ferror(f)||!r.records||!out[0].files)return -1;
    for(unsigned i=0;i<SETUP_COMP_COUNT;i++)out[i].known=1;
    return 0;
}
