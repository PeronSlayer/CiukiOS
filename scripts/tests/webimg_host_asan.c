/* Host sanitizer harness for the production webimg CAPP decoder.
 * Run from the repo root:
 *   python3 scripts/tests/generate_webimg_fixtures.py /tmp/webimg-fixtures
 *   cc -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
 *     scripts/tests/webimg_host_asan.c -o /tmp/webimg-host-asan
 *   ASAN_OPTIONS=detect_leaks=1 /tmp/webimg-host-asan \
 *     /tmp/webimg-fixtures/manifest.txt
 * The harness emulates only DOS file/segment services; the PNG/GIF/JPEG
 * decoders and app_event state machine are included from production source. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define CIUKIOS_APP_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define EV_OPEN 1
#define EV_POLL 6
#define EV_CLOSE 7
int dos_open(const char *, int);
int dos_close(int);
int dos_read(int, void *, int);
long dos_seek(int, long, int);
u16 dos_alloc(u16);
void dos_free(u16);
u8 peek8(u16, u16);
void poke8(u16, u16, u8);
void far_copy(u16, u16, u16, u16, u16);
int mem_cmp(const void *, const void *, int);
void mem_set(void *, int, int);
u16 app_seg(void);

struct local_ref { const void *ptr; };
static struct local_ref local_refs[16];
static unsigned local_ref_count;
static u16 host_local_offset(const void *ptr)
{
    unsigned i;
    for (i=0; i<local_ref_count; ++i)
        if (local_refs[i].ptr==ptr) return (u16)(0x1000u+i*0x1000u);
    if (local_ref_count>=16) return 0;
    local_refs[local_ref_count].ptr=ptr;
    return (u16)(0x1000u+local_ref_count++*0x1000u);
}
#define WEBIMG_LOCAL_OFFSET(p) host_local_offset((const void *)(p))
#include "../../src/apps/webimg.c"

#define SEGMENTS 64
static u8 segment_memory[SEGMENTS][65536];
static u32 segment_size[SEGMENTS];
static FILE *files[8];
static int next_alloc_seg=16, allocations, failures;
static const char *expected_path;
static const u8 *local_ptr(u16 off, u16 n)
{
    unsigned i, slot;
    if (off<0x1000u) return 0;
    slot=(unsigned)(off-0x1000u)/0x1000u;
    if (slot>=local_ref_count || (unsigned)(off-0x1000u)%0x1000u) return 0;
    i=slot;
    (void)n;
    return (const u8 *)local_refs[i].ptr;
}
static u8 *local_ptr_mut(u16 off, u16 n)
{
    return (u8 *)local_ptr(off,n);
}

u16 app_seg(void) { return 1; }
int mem_cmp(const void *a,const void *b,int n) { return memcmp(a,b,(size_t)n); }
void mem_set(void *p,int v,int n) { memset(p,v,(size_t)n); }
int dos_open(const char *path,int mode)
{
    int i; (void)mode;
    if (!path || !*path) path=expected_path;
    for(i=0;i<8;i++) if(!files[i]) {
        files[i]=fopen(path,"rb");
        return files[i]?i:-1;
    }
    return -1;
}
int dos_close(int h)
{
    if(h<0||h>=8||!files[h]) return -1;
    fclose(files[h]); files[h]=0; return 0;
}
int dos_read(int h,void *buf,int n)
{
    size_t got;
    if(h<0||h>=8||!files[h]||n<0) return -1;
    got=fread(buf,1,(size_t)n,files[h]);
    if(!got&&ferror(files[h])) return -1;
    return (int)got;
}
long dos_seek(int h,long pos,int whence)
{
    int origin=whence==0?SEEK_SET:whence==1?SEEK_CUR:SEEK_END;
    if(h<0||h>=8||!files[h]||fseek(files[h],pos,origin)) return -1;
    return ftell(files[h]);
}
u16 dos_alloc(u16 paras)
{
    int s=next_alloc_seg++;
    if(s>=SEGMENTS||!paras||paras>4096) return 0;
    memset(segment_memory[s],0,sizeof segment_memory[s]);
    segment_size[s]=(u32)paras*16u;
    ++allocations; return (u16)s;
}
void dos_free(u16 seg)
{
    if(seg>=16&&seg<SEGMENTS&&segment_size[seg]) {
        memset(segment_memory[seg],0xdd,65536); segment_size[seg]=0; --allocations;
    }
}
u8 peek8(u16 seg,u16 off)
{
    if(seg>=SEGMENTS||off>=segment_size[seg]) { ++failures; return 0; }
    return segment_memory[seg][off];
}
void poke8(u16 seg,u16 off,u8 v)
{
    if(seg>=SEGMENTS||off>=segment_size[seg]) { ++failures; return; }
    segment_memory[seg][off]=v;
}
void far_copy(u16 dseg,u16 doff,u16 sseg,u16 soff,u16 n)
{
    u8 *dst; const u8 *src;
    if(dseg==1) dst=local_ptr_mut(doff,n);
    else if(dseg<SEGMENTS&&(u32)doff+n<=segment_size[dseg]) dst=&segment_memory[dseg][doff];
    else dst=0;
    if(sseg==1) src=local_ptr(soff,n);
    else if(sseg<SEGMENTS&&(u32)soff+n<=segment_size[sseg]) src=&segment_memory[sseg][soff];
    else src=0;
    if(!dst||!src) { ++failures; return; }
    memcpy(dst,src,n);
}

static int check(int ok,const char *what)
{
    if(!ok) { fprintf(stderr,"FAIL: %s: %s\n",expected_path?expected_path:"(no fixture)",what); return 0; }
    return 1;
}
static int run_image(const char *image,const char *rgb,unsigned w,unsigned h,int tolerance)
{
    FILE *ef; u8 *expected,*actual; size_t count=(size_t)w*h*3u;
    struct webimg_request request; unsigned polls=0,x,y; int result,ok=1;
    expected=(u8 *)malloc(count); actual=(u8 *)malloc(count);
    if(!expected||!actual) return check(0,"test allocation");
    ef=fopen(rgb,"rb");
    if(!ef||fread(expected,1,count,ef)!=count||fgetc(ef)!=EOF) ok=check(0,"expected RGB fixture length");
    if(ef) fclose(ef);
    memset(actual,0x5a,count);
    memset(segment_memory[2],0,sizeof segment_memory[2]);
    memset(segment_memory[3],0xa5,65536);
    memset(&request,0,sizeof request);
    request.abi_version=WEBIMG_ABI_VERSION;
    request.struct_bytes=sizeof request;
    strncpy(request.path,image,sizeof request.path-1);
    request.output_seg=3; request.output_off=0; request.output_capacity=WEBIMG_RGB_ROW_MAX;
    /* Segment zero is a low-memory object just like a DOS caller packet. */
    memcpy(segment_memory[2],&request,sizeof request);
    expected_path=image;
    result=app_event(EV_OPEN,2,0,0);
    memcpy(&request,segment_memory[2],sizeof request);
    ok &= check(result==1,"EV_OPEN accepted request");
    if(request.status!=WEBIMG_STATUS_READY)
        fprintf(stderr,"  open status=%u error=%u format=%u JPEG=%ux%u ncomp=%u GIF screen=%ux%u frame=%ux%u palette=%u mincode=%u\n",
                request.status,request.error,request.format,jpg.width,jpg.height,jpg.ncomp,
                gif_screen_w,gif_screen_h,gif_w,gif_h,gif_palette_count,gif_min_code);
    ok &= check(request.status==WEBIMG_STATUS_READY,"decoder reports READY");
    if(request.total_width!=w||request.total_height!=h)
        fprintf(stderr,"  dimensions=%ux%u expected=%ux%u\n",request.total_width,request.total_height,w,h);
    ok &= check(request.total_width==w&&request.total_height==h,"decoded dimensions match");
    while(request.status==WEBIMG_STATUS_READY||request.status==WEBIMG_STATUS_PIXELS) {
        memset(segment_memory[3],0xa5,65536);
        result=app_event(EV_POLL,0,0,0);
        memcpy(&request,segment_memory[2],sizeof request);
        if(!result||++polls>h*8u+64u) { ok &= check(0,"poll completes within bound"); break; }
        if(request.status==WEBIMG_STATUS_PIXELS) {
            u32 n=(u32)request.w*request.h*3u;
            if(n>WEBIMG_RGB_ROW_MAX||request.bytes_written!=n||
               (u32)request.output_off+n>65536u) { ok &= check(0,"output rectangle bounds"); break; }
            for(y=0;y<request.h;y++) {
                u32 dst=((u32)request.y+y)*w*3u+(u32)request.x*3u;
                u32 len=(u32)request.w*3u;
                if(dst+len>count) { ok &= check(0,"output rectangle within image"); break; }
                memcpy(actual+dst,segment_memory[3]+request.output_off+y*len,len);
            }
        }
    }
    if(request.status!=WEBIMG_STATUS_DONE)
        fprintf(stderr,"  final status=%u error=%u format=%u polls=%u dimensions=%ux%u JPEG=%ux%u filepos=%ld\n",
                request.status,request.error,request.format,polls,request.total_width,request.total_height,
                jpg.width,jpg.height,files[0]?ftell(files[0]):-1L);
    ok &= check(request.status==WEBIMG_STATUS_DONE,"image reaches DONE");
    for(x=0;x<count;x++) {
        int delta=(int)actual[x]-(int)expected[x]; if(delta<0) delta=-delta;
        if(delta>tolerance) {
            fprintf(stderr,"  RGB mismatch byte=%u actual=%u expected=%u delta=%d tolerance=%d\n",
                    x,actual[x],expected[x],delta,tolerance);
            ok &= check(0,"decoded RGB matches reference"); break;
        }
    }
    ok &= check(segment_memory[3][WEBIMG_RGB_ROW_MAX]==0xa5,"output guard intact");
    app_event(EV_CLOSE,0,0,0);
    ok &= check(allocations==0,"decoder releases DOS heap on close");
    free(actual); free(expected);
    return ok;
}

static int expect_open_rejected(const char *image,u16 error)
{
    struct webimg_request r; int ok;
    memset(segment_memory[2],0,sizeof segment_memory[2]);
    memset(&r,0,sizeof r); r.abi_version=WEBIMG_ABI_VERSION; r.struct_bytes=sizeof r;
    strncpy(r.path,image,sizeof r.path-1); r.output_seg=3; r.output_capacity=WEBIMG_RGB_ROW_MAX;
    memcpy(segment_memory[2],&r,sizeof r); expected_path=image;
    app_event(EV_OPEN,2,0,0); memcpy(&r,segment_memory[2],sizeof r);
    ok=check(r.status==WEBIMG_STATUS_ERROR&&r.error==error,"invalid image rejected with expected status");
    app_event(EV_CLOSE,0,0,0);
    return ok;
}

static int expect_stream_rejected(const char *image)
{
    struct webimg_request r; unsigned polls=0; int ok=1;
    memset(segment_memory[2],0,sizeof segment_memory[2]);
    memset(&r,0,sizeof r); r.abi_version=WEBIMG_ABI_VERSION; r.struct_bytes=sizeof r;
    strncpy(r.path,image,sizeof r.path-1); r.output_seg=3; r.output_capacity=WEBIMG_RGB_ROW_MAX;
    memcpy(segment_memory[2],&r,sizeof r); expected_path=image;
    app_event(EV_OPEN,2,0,0); memcpy(&r,segment_memory[2],sizeof r);
    ok &= check(r.status==WEBIMG_STATUS_READY,"damaged stream opens only its header");
    while(r.status==WEBIMG_STATUS_READY||r.status==WEBIMG_STATUS_PIXELS) {
        app_event(EV_POLL,0,0,0); memcpy(&r,segment_memory[2],sizeof r);
        if(++polls>128) { ok &= check(0,"damaged stream is bounded"); break; }
    }
    ok &= check(r.status==WEBIMG_STATUS_ERROR&&r.error==WEBIMG_ERR_FORMAT,
                "CRC/truncated stream rejected during decode");
    app_event(EV_CLOSE,0,0,0);
    ok &= check(allocations==0,"failed decoder releases DOS heap on close");
    return ok;
}

int main(int argc,char **argv)
{
    FILE *manifest; char line[1200],kind[24],image[512],rgb[512];
    unsigned w,h; int tolerance,ok=1,tests=0;
    if(argc!=2) { fprintf(stderr,"usage: %s manifest.txt\n",argv[0]); return 2; }
    segment_size[2]=segment_size[3]=65536u;
    manifest=fopen(argv[1],"r");
    if(!manifest) { perror(argv[1]); return 2; }
    while(fgets(line,sizeof line,manifest)) {
        if(line[0]=='#'||line[0]=='\n') continue;
        if(sscanf(line,"%23s %511s %511s %u %u %d",kind,image,rgb,&w,&h,&tolerance)!=6) {
            ok &= check(0,"manifest row parses"); continue;
        }
        ++tests;
        if(!strcmp(kind,"image")) ok &= run_image(image,rgb,w,h,tolerance);
        else if(!strcmp(kind,"stream-error")) ok &= expect_stream_rejected(image);
        else if(!strcmp(kind,"open-error")) ok &= expect_open_rejected(image,WEBIMG_ERR_UNSUPPORTED);
        else ok &= check(0,"known manifest operation");
    }
    fclose(manifest);
    ok &= check(tests>=10,"fixture manifest has meaningful coverage");
    if(failures) ok=0;
    if(ok) puts("webimg production decoder host regression: PASS");
    return ok?0:1;
}
