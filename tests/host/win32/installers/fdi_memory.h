/* Called by the existing confined H1 loader, with cabinet.dll's real PE
 * exports. Caller callbacks use authored memory files; no Win32 file shim.
 * SPDX-License-Identifier: MIT */
#define w32_upper_run installer_original_upper_run
#include "../../../../src/probes/win32_upper/exercise.c"
#undef w32_upper_run
struct cab_notice { LONG size; char *name,*disk,*path; void *context; intptr_t file;
    uint16_t date,time,attributes,set,cabinet,folder; int error; };
static ULONG cab_position,cab_sizes[3],cab_positions[3],cab_frees,cab_allocs;
static uint8_t cab_output[3][65536];
static void *cab_allocate(ULONG n){
    uint8_t *p=host.services.alloc(&host,n+4);if(!p)return 0;*(ULONG *)p=n+4;cab_allocs++;return p+4;
}
static void cab_release(void *p){if(p){uint8_t *b=(uint8_t *)p-4;cab_frees++;host.services.free(&host,b,*(ULONG *)b);}}
/* Wine FDICopy asks for _O_RDONLY | _O_BINARY (0x8000). Binary is a mode,
 * not write access; rejecting it makes a valid in-memory cabinet unreadable. */
static intptr_t cab_open(char *path,int flags,int mode){(void)path;(void)mode;if(flags&~0x8000)return -1;cab_position=0;return 1;}
static unsigned cab_read(intptr_t h,void *out,unsigned n){
    if(h!=1 || cab_position>sizeof(cab_source))return ~0u;
    if(n>sizeof(cab_source)-cab_position)n=sizeof(cab_source)-cab_position;
    w32_copy(out,cab_source+cab_position,n);cab_position+=n;return n;
}
static unsigned cab_write(intptr_t h,void *in,unsigned n){
    if(h<2 || h>4 || n>sizeof(cab_output[0])-cab_positions[h-2])return ~0u;
    unsigned i=(unsigned)h-2;w32_copy(cab_output[i]+cab_positions[i],in,n);cab_positions[i]+=n;
    if(cab_positions[i]>cab_sizes[i])cab_sizes[i]=cab_positions[i];return n;
}
static int cab_close(intptr_t h){return h>=1 && h<=4?0:-1;}
static LONG cab_seek(intptr_t h,LONG n,int origin){
    if(h!=1 || origin<0 || origin>2)return -1;
    int64_t at=(origin==0?0:origin==1?cab_position:sizeof(cab_source))+(int64_t)n;
    if(at<0 || at>sizeof(cab_source))return -1;cab_position=(ULONG)at;return (LONG)at;
}
static intptr_t cab_notify(int kind,struct cab_notice *n){
    if(kind==2){if(!w32_strcmp(n->name,"payload.txt"))return 2;if(!w32_strcmp(n->name,"child.exe"))return 3;if(!w32_strcmp(n->name,"chain.inf"))return 4;return -1;}
    return kind==3?1:kind==1 || kind==4?-1:0;
}
static void w32_upper_run(const struct w32_upper_env *e,const struct w32_upper_entry *table,uint32_t count,struct w32_upper_result *r)
{
    installer_original_upper_run(e,table,count,r);
    void *m=e->load("cabinet.dll");
    void *(*create)(void *,void *,void *,void *,void *,void *,void *,int,ERF *)=e->proc(m,"FDICreate");
    int (*copy)(void *,char *,char *,int,void *,void *,void *)=e->proc(m,"FDICopy");
    int (*destroy)(void *)=e->proc(m,"FDIDestroy");
    if(!m || !create || !copy || !destroy){r->errors++;return;}
    ERF error={0};void *context=create(cab_allocate,cab_release,cab_open,cab_read,cab_write,cab_close,cab_seek,-1,&error);
    int ok=context && copy(context,"fixture.cab","",0,cab_notify,0,0);
    if(!ok){host_print("FDI operation=");host_number((ULONG)error.erfOper);
        host_print(" type=");host_number((ULONG)error.erfType);host_print(" error=");host_number((ULONG)error.fError);host_print("\n");}
    if(ok)for(unsigned i=0;i<3;i++){
        const uint8_t *expected=i==0?cab_payload:i==1?cab_child:cab_inf;unsigned n=i==0?sizeof(cab_payload):i==1?sizeof(cab_child):sizeof(cab_inf);
        if(cab_sizes[i]!=n)ok=0;else for(unsigned j=0;j<n;j++)if(cab_output[i][j]!=expected[j])ok=0;
    }
    if(context){
        cab_source[sizeof(cab_source)-1]^=1;cab_position=0;for(unsigned i=0;i<3;i++)cab_sizes[i]=cab_positions[i]=0;
        if(copy(context,"fixture.cab","",0,cab_notify,0,0))ok=0;
        cab_source[sizeof(cab_source)-1]^=1;
        if(!destroy(context))ok=0;
    }
    if(cab_allocs!=cab_frees)ok=0;
    e->free(m);
    if(!ok)r->errors++;
    host_print(ok?"PASS FDI MSZIP exact extracted bytes and damaged CFDATA rejection\n":"FAIL FDI MSZIP extraction\n");
}
