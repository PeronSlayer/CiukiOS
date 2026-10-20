/* PROGMAN transport: USER owns local atoms/global blocks; only copied UTF-8
 * bytes and authenticated conversation IDs reach w32srv. No foreign pointer
 * or process-private HGLOBAL enters the kernel message transport.
 * Microsoft Platform SDK, Command-String Interface; Using DDE.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
extern HANDLE W32API GlobalAlloc(DWORD,DWORD);
extern HANDLE W32API GlobalFree(HANDLE);
extern void *W32API GlobalLock(HANDLE);
extern BOOL W32API GlobalUnlock(HANDLE);
extern DWORD W32API GlobalSize(HANDLE);
static UINT atom_name(ATOM atom,char *name,UINT cap)
{
    char utf8[W32R_NAME];struct w32r_reply r;
    if(!u32_directory(W32R_ATOM_NAME,(HWND)(uintptr_t)atom,0,0,0,"",0,0,&r,utf8,sizeof(utf8)) || !r.data_bytes)return 0;
    u32_progman_ansi(utf8);UINT n=w32_strlen(utf8);if(n>=cap)return 0;w32_copy(name,utf8,n+1);return n;
}
static ATOM atom_add(const char *name)
{struct w32r_reply r;return u32_directory(W32R_ATOM_ADD,0,0,0,0,name,0,0,&r,0,0)?(ATOM)r.key:0;}
static void atom_delete(ATOM atom)
{struct w32r_reply r;u32_directory(W32R_ATOM_DELETE,(HWND)(uintptr_t)atom,0,0,0,"",0,0,&r,0,0);}
struct dde_pair { uintptr_t lo,hi; };
static struct dde_pair *pairs[64];
static UINT pair_index(LPARAM lp) {for(UINT i=0;i<64;i++)if(lp && pairs[i]==(void *)lp)return i;return 64;}
/* EXECUTE carries its original HGLOBAL directly; REQUEST/INITIATE are words.
 * Other messages need a pair because ACK's high value may be a full handle. */
LPARAM W32API PackDDElParam(UINT m, uintptr_t lo, uintptr_t hi)
{
    U32_GUARD;
    if (m==0x3e6) return (LPARAM)((lo&65535u)|((hi&65535u)<<16));
    if (m==0x3e0 || m==0x3e1) return (LPARAM)((lo&65535u)|((hi&65535u)<<16));
    if (m==0x3e8) return (LPARAM)hi;
    struct dde_pair *p=u32_alloc(sizeof(*p));if(!p)return 0;
    for(UINT i=0;i<64;i++)if(!pairs[i]){pairs[i]=p;p->lo=lo;p->hi=hi;return (LPARAM)p;}
    u32_free(p);return 0;
}
BOOL W32API UnpackDDElParam(UINT m, LPARAM lp, uintptr_t *lo, uintptr_t *hi)
{
    if(!lo || !hi)return u32_fail(87);
    if(m==0x3e8){*lo=0;*hi=(uintptr_t)lp;return 1;}
    if(m==0x3e0 || m==0x3e1 || m==0x3e6){*lo=(uintptr_t)lp&65535u;*hi=((uintptr_t)lp>>16)&65535u;return 1;}
    if(pair_index(lp)==64){
        if(m==0x3e4){*lo=(uintptr_t)lp&65535u;*hi=((uintptr_t)lp>>16)&65535u;return 1;}
        return u32_fail(87);
    }
    const struct dde_pair *p=(void *)lp;*lo=p->lo;*hi=p->hi;return 1;
}
BOOL W32API FreeDDElParam(UINT m, LPARAM lp)
{
    if(m!=0x3e8 && m!=0x3e0 && m!=0x3e1 && m!=0x3e6){UINT i=pair_index(lp);if(i!=64){u32_free(pairs[i]);pairs[i]=0;}}
    return 1;
}
LPARAM W32API ReuseDDElParam(LPARAM lp,UINT old,UINT next,uintptr_t lo,uintptr_t hi)
{ FreeDDElParam(old,lp);return PackDDElParam(next,lo,hi); }
DWORD u32_progman_rpc(UINT op,DWORD id,const char *path,const char *name,
                      struct w32r_reply *reply,void *data,DWORD cap)
{
    if(!path)path="";if(!name)name="";
    DWORD p=w32_strlen(path)+1,n=w32_strlen(name)+1;
    if(p>W32R_PATH || n>W32R_NAME)return 87;
    DWORD total=sizeof(struct w32r_request)+p+n;
    uint8_t *in=u32_alloc(total),*out=u32_alloc(W32R_MAX);
    if(!in || !out){u32_free(in);u32_free(out);return 8;}
    struct w32r_request a={.version=1,.op=op,.index=id,.path_bytes=p,.name_bytes=n};
    w32_copy(in,&a,sizeof(a));w32_copy(in+sizeof(a),path,p);w32_copy(in+sizeof(a)+p,name,n);
    DWORD used=0,e=u32_backend.rpc(in,total,out,W32R_MAX,&used);
    if(!e){
        if(used<sizeof(*reply))e=13;
        else{w32_copy(reply,out,sizeof(*reply));e=reply->error;
            if(!e && (reply->version!=1 || reply->name_bytes || reply->data_bytes>cap || used!=sizeof(*reply)+reply->data_bytes))e=13;
            if(!e && reply->data_bytes)w32_copy(data,out+sizeof(*reply),reply->data_bytes);
        }
    }
    u32_free(in);u32_free(out);return e;
}
/* UTF-8 response -> Windows ANSI CF_TEXT, in place (never grows). */
void u32_progman_ansi(char *s)
{
    const uint8_t *p=(void *)s;char *out=s;
    while(*p){unsigned c=*p++;if(c>=0xc0){unsigned extra=c>=0xf0?3:c>=0xe0?2:1;c&=extra==3?7:extra==2?15:31;
        while(extra-- && *p)c=(c<<6)|(*p++&63);}
        *out++=(char)u32_ansi(c);
    }*out=0;
}
static struct {HWND server,client;DWORD id,used;} raw[16];
static LRESULT W32API proxy(HWND h,UINT m,WPARAM wp,LPARAM lp)
{ return DefWindowProcA(h,m,wp,lp); }
static void raw_close(UINT i)
{
    struct w32r_reply r;
    u32_progman_rpc(W32R_PROGMAN_CLOSE,raw[i].id,"","",&r,0,0);
    HWND h=raw[i].server;w32_fill(&raw[i],0,sizeof(raw[i]));
    if(IsWindow(h))DestroyWindow(h);
    BOOL any=0;for(UINT k=0;k<16;k++)if(raw[k].server)any=1;
    if(!any)UnregisterClassA("Ciuki PROGMAN proxy",0);
}
BOOL u32_progman_message(HWND h,UINT m,WPARAM wp,LPARAM lp,LRESULT *result)
{
    if(m<0x3e0 || m>0x3e8)return 0;
    if(m==0x3e0 && (h==(HWND)(uintptr_t)0xffff || h==(HWND)(intptr_t)-1)){
        char service[256]="PROGMAN",topic[256]="PROGMAN";
        ATOM a=(ATOM)(uintptr_t)lp,b=(ATOM)((uintptr_t)lp>>16);
        if((a && !atom_name(a,service,256)) || (b && !atom_name(b,topic,256)))return 0;
        struct w32r_reply r;
        if(u32_progman_rpc(W32R_PROGMAN_OPEN,0,service,topic,&r,0,0))return 0;
        UINT i;
        for(i=0;i<16;i++)if(!raw[i].server)break;
        if(i==16){u32_progman_rpc(W32R_PROGMAN_CLOSE,r.key,"","",&r,0,0);return 0;}
        WNDCLASSA cls={.lpfnWndProc=proxy,.lpszClassName="Ciuki PROGMAN proxy"};
        RegisterClassA(&cls);
        HWND server=CreateWindowExA(0,cls.lpszClassName,"",WS_POPUP,0,0,1,1,0,0,0,0);
        if(!server){u32_progman_rpc(W32R_PROGMAN_CLOSE,r.key,"","",&r,0,0);return 0;}
        raw[i].server=server;raw[i].client=(HWND)wp;raw[i].id=r.key;raw[i].used=u32_ticks();
        ATOM sa=atom_add(service),ta=atom_add(topic);
        SendMessageA((HWND)wp,0x3e4,(WPARAM)server,(LPARAM)((DWORD)sa|((DWORD)ta<<16)));
        atom_delete(sa);atom_delete(ta);*result=0;return 1;
    }
    UINT i;for(i=0;i<16;i++)if(raw[i].server==h && raw[i].client==(HWND)wp)break;
    if(i==16)return 0;
    raw[i].used=u32_ticks();*result=0;
    struct w32r_reply r;uintptr_t lo=0,hi=0;
    if(m==0x3e1){PostMessageA((HWND)wp,m,(WPARAM)h,0);raw_close(i);return 1;}
    if(m==0x3e8){
        HANDLE block=(HANDLE)lp;DWORD n=GlobalSize(block);char *text=GlobalLock(block);DWORD ack=0;
        if(text && n && n<=W32R_PATH && text[n-1]==0){
            char utf8[W32R_PATH];
            if(u32_utf8(utf8,sizeof(utf8),text) && !u32_progman_rpc(W32R_PROGMAN_EXECUTE,raw[i].id,utf8,"",&r,0,0))ack=r.key;
        }
        if(text)GlobalUnlock(block);
        LPARAM answer=PackDDElParam(0x3e4,ack,(uintptr_t)block);
        if(!PostMessageA((HWND)wp,0x3e4,(WPARAM)h,answer))FreeDDElParam(0x3e4,answer);
        return 1;
    }
    if(m==0x3e6){
        UnpackDDElParam(m,lp,&lo,&hi);char item[256];
        char *bytes=u32_alloc(W32R_DATA);BOOL ok=0;
        if(bytes && lo==1 && atom_name((ATOM)hi,item,256)){
            char utf8[1021];u32_utf8(utf8,sizeof(utf8),item);
            ok=!u32_progman_rpc(W32R_PROGMAN_REQUEST,raw[i].id,"",utf8,&r,bytes,W32R_DATA);
        }
        if(ok && r.data_bytes && !bytes[r.data_bytes-1]){
            u32_progman_ansi(bytes);DWORD n=w32_strlen(bytes)+1;
            HANDLE block=GlobalAlloc(0x2002,n+4);uint8_t *p=GlobalLock(block);
            if(p){w32_put16(p,0x3000);w32_put16(p+2,1);w32_copy(p+4,bytes,n);GlobalUnlock(block);
                LPARAM answer=PackDDElParam(0x3e5,(uintptr_t)block,hi);
                if(!PostMessageA((HWND)wp,0x3e5,(WPARAM)h,answer)){FreeDDElParam(0x3e5,answer);GlobalFree(block);}
            }else{if(block)GlobalFree(block);ok=0;}
        }else ok=0;
        if(!ok){LPARAM answer=PackDDElParam(0x3e4,0,hi);if(!PostMessageA((HWND)wp,0x3e4,(WPARAM)h,answer))FreeDDElParam(0x3e4,answer);}
        u32_free(bytes);return 1;
    }
    return 1;
}
void u32_progman_cleanup(DWORD tid)
{
    if(!tid)for(UINT i=0;i<64;i++)if(pairs[i]){u32_free(pairs[i]);pairs[i]=0;}
    for(UINT i=0;i<16;i++)if(raw[i].server){struct u32_window *w=u32_window(raw[i].server);
        if(!tid || !w || w->tid==tid)raw_close(i);}
}
