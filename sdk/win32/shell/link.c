/* IShellLink A/W + IPersistFile, using f5-09's counted CIUKLNK3 format.
 * No host shell or command interpolation. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shell.h>
#define E_INVALID ((HRESULT)0x80070057u)
#define E_NOINTERFACE ((HRESULT)0x80004002u)
#define E_NOTIMPL ((HRESULT)0x80004001u)
struct link;
struct interface {const void *const *vt;struct link *owner;};
struct link {
  struct interface a,w,p;
  ULONG refs;BOOL dirty;DWORD show;LONG icon_index;uint16_t hotkey;
  char label[64],target[260],args[1024],cwd[260],icon[260],filename[260];
};
int shell_guid(const SHELL_GUID *g,DWORD a) {
  static const uint8_t tail[]={0xc0,0,0,0,0,0,0,0x46};
  if(!g || g->a!=a || g->b || g->c)return 0;
  for(unsigned i=0;i<8;i++)if(g->d[i]!=tail[i])return 0;
  return 1;
}
static HRESULT failure(void){DWORD e=GetLastError();return (HRESULT)(0x80070000u|(e?e:5));}
static ULONG W32API addref(struct interface *i){return ++i->owner->refs;}
static ULONG W32API release(struct interface *i){struct link *l=i->owner;ULONG n=--l->refs;if(!n)shell_free(l);return n;}
static HRESULT W32API query(struct interface *i,const SHELL_GUID *id,void **out){
  if(!out)return E_INVALID;*out=0;struct link *l=i->owner;
  if(shell_guid(id,0) || shell_guid(id,0x214ee))*out=&l->a;
  else if(shell_guid(id,0x214f9))*out=&l->w;
  else if(shell_guid(id,0x10b) || shell_guid(id,0x10c))*out=&l->p;
  else return E_NOINTERFACE;
  addref(i);return 0;
}
static HRESULT set(char *out,DWORD cap,const void *s,int wide){
  if(!s)return E_INVALID;
  WCHAR *w=shell_alloc(1024*sizeof(*w)+3072);if(!w)return (HRESULT)0x8007000eu;
  char *u=(char *)(w+1024);HRESULT result=E_INVALID;
  if(!wide){if(!MultiByteToWideChar(1252,0,s,-1,w,1024))goto done;s=w;}
  if(!shell_utf8_encode(s,u,3072))goto done;
  DWORD n=w32_strlen(u)+1;if(n>cap)goto done;
  w32_copy(out,u,n);result=0;
done:
  shell_free(w);return result;
}
static HRESULT get(const char *s,void *out,int cap,int wide){
  WCHAR w[1024];
  if(!out || cap<=0 || !shell_utf8_decode(s,w,1024))return E_INVALID;
  if(wide){unsigned n=0;while(w[n])n++;if(n>=(unsigned)cap)return E_INVALID;w32_copy(out,w,(n+1)*2);return 0;}
  BOOL used=0;return WideCharToMultiByte(1252,0,w,-1,out,cap,0,&used)>0 && !used?0:E_INVALID;
}
static HRESULT set_field(struct interface *i,char *field,DWORD cap,const void *s,int wide){
  HRESULT r=set(field,cap,s,wide);if(!r)i->owner->dirty=1;return r;
}
static HRESULT W32API path_a(struct interface *i,char *out,int cap,void *data,DWORD flags){(void)flags;if(data)w32_fill(data,0,sizeof(WIN32_FIND_DATAA));return get(i->owner->target,out,cap,0);}
static HRESULT W32API path_w(struct interface *i,WCHAR *out,int cap,void *data,DWORD flags){(void)flags;if(data)w32_fill(data,0,sizeof(WIN32_FIND_DATAW));return get(i->owner->target,out,cap,1);}
static HRESULT W32API id_get(struct interface *i,void **out){(void)i;if(out)*out=0;return E_NOTIMPL;}
static HRESULT W32API id_set(struct interface *i,const void *id){(void)i;(void)id;return E_NOTIMPL;}
#define FIELD(name,field) \
static HRESULT W32API name##_get_a(struct interface *i,char *s,int n){return get(i->owner->field,s,n,0);} \
static HRESULT W32API name##_get_w(struct interface *i,WCHAR *s,int n){return get(i->owner->field,s,n,1);} \
static HRESULT W32API name##_set_a(struct interface *i,const char *s){return set_field(i,i->owner->field,sizeof(i->owner->field),s,0);} \
static HRESULT W32API name##_set_w(struct interface *i,const WCHAR *s){return set_field(i,i->owner->field,sizeof(i->owner->field),s,1);}
FIELD(description,label)
FIELD(directory,cwd)
FIELD(arguments,args)
static HRESULT W32API path_set_a(struct interface *i,const char *s){return set_field(i,i->owner->target,sizeof(i->owner->target),s,0);}
static HRESULT W32API path_set_w(struct interface *i,const WCHAR *s){return set_field(i,i->owner->target,sizeof(i->owner->target),s,1);}
static int hot_valid(DWORD value){return !value || (!(value&0xfffff000u) && (value&255u));}
static HRESULT W32API hot_get(struct interface *i,uint16_t *out){if(!out)return E_INVALID;*out=i->owner->hotkey;return 0;}
static HRESULT W32API hot_set(struct interface *i,uint16_t value){if(!hot_valid(value))return E_INVALID;i->owner->hotkey=value;i->owner->dirty=1;return 0;}
static HRESULT W32API show_get(struct interface *i,int *out){if(!out)return E_INVALID;*out=(int)i->owner->show;return 0;}
static HRESULT W32API show_set(struct interface *i,int show){if(show!=1 && show!=3 && show!=7)return E_INVALID;i->owner->show=(DWORD)show;i->owner->dirty=1;return 0;}
static HRESULT W32API icon_get_a(struct interface *i,char *s,int cap,int *index){if(!index)return E_INVALID;*index=i->owner->icon_index;return get(i->owner->icon,s,cap,0);}
static HRESULT W32API icon_get_w(struct interface *i,WCHAR *s,int cap,int *index){if(!index)return E_INVALID;*index=i->owner->icon_index;return get(i->owner->icon,s,cap,1);}
static HRESULT icon_set(struct interface *i,const void *s,int index,int wide){HRESULT r=set_field(i,i->owner->icon,sizeof(i->owner->icon),s,wide);if(!r)i->owner->icon_index=index;return r;}
static HRESULT W32API icon_set_a(struct interface *i,const char *s,int index){return icon_set(i,s,index,0);}
static HRESULT W32API icon_set_w(struct interface *i,const WCHAR *s,int index){return icon_set(i,s,index,1);}
static HRESULT W32API relative_a(struct interface *i,const char *s,DWORD reserved){(void)i;(void)s;return reserved?E_INVALID:E_NOTIMPL;}
static HRESULT W32API relative_w(struct interface *i,const WCHAR *s,DWORD reserved){(void)i;(void)s;return reserved?E_INVALID:E_NOTIMPL;}
static HRESULT W32API resolve(struct interface *i,HANDLE owner,DWORD flags){(void)owner;(void)flags;char path[260];HRESULT r=get(i->owner->target,path,260,0);if(r)return r;return GetFileAttributesA(path)==~0u?failure():0;}
static HRESULT W32API classid(struct interface *i,SHELL_GUID *out){(void)i;if(!out)return E_INVALID;*out=(SHELL_GUID){.a=0x21401,.d={0xc0,0,0,0,0,0,0,0x46}};return 0;}
static HRESULT W32API dirty(struct interface *i){return i->owner->dirty?0:1;}
static int valid(const struct link *l){
  if(!l->target[0] || !l->label[0] || !hot_valid(l->hotkey) || (l->show!=1 && l->show!=3 && l->show!=7))return 0;
  return l->target[0]=='/' || (((l->target[0]>='A' && l->target[0]<='Z') || (l->target[0]>='a' && l->target[0]<='z')) && l->target[1]==':' && (l->target[2]=='\\' || l->target[2]=='/'));
}
static HRESULT W32API save(struct interface *i,const WCHAR *name,BOOL remember){
  struct link *l=i->owner;char filename[260];
  if(name){if(!shell_ansi(name,filename,260))return E_INVALID;}
  else if(!shell_copy(filename,260,l->filename) || !filename[0])return E_INVALID;
  /* Description is optional in IShellLink; the desktop requires a label. */
  if(!l->label[0]) {
    const char *base=l->target;for(const char *p=base;*p;p++)if(*p=='\\' || *p=='/')base=p+1;
    if(!shell_copy(l->label,sizeof(l->label),base))return E_INVALID;
  }
  if(!valid(l))return E_INVALID;
  uint8_t *bytes=shell_alloc(4096);if(!bytes)return (HRESULT)0x8007000eu;
  w32_copy(bytes,"CIUKLNK3",8);w32_put32(bytes+8,l->show);w32_put32(bytes+12,(DWORD)l->icon_index);w32_put32(bytes+36,l->hotkey);
  const char *fields[]={l->label,l->target,l->args,l->cwd,l->icon};DWORD at=40;
  for(unsigned k=0;k<5;k++){DWORD n=w32_strlen(fields[k]);if(n>4096-at){shell_free(bytes);return E_INVALID;}w32_put32(bytes+16+k*4,n);w32_copy(bytes+at,fields[k],n);at+=n;}
  HANDLE h=CreateFileA(filename,0x40000000u,0,0,2,128,0);
  if(h==K32_INVALID){shell_free(bytes);return failure();}
  DWORD n=0;BOOL ok=WriteFile(h,bytes,at,&n,0) && n==at && FlushFileBuffers(h);DWORD e=GetLastError();CloseHandle(h);
  shell_free(bytes);
  if(!ok){DeleteFileA(filename);SetLastError(e?e:112);return failure();}
  l->dirty=0;if(remember)shell_copy(l->filename,260,filename);return 0;
}
static HRESULT W32API load(struct interface *i,const WCHAR *name,DWORD mode){
  (void)mode;char filename[260];if(!shell_ansi(name,filename,260))return E_INVALID;
  HANDLE h=CreateFileA(filename,0x80000000u,1,0,3,0,0);if(h==K32_INVALID)return failure();
  DWORD size=GetFileSize(h,0),got=0;uint8_t *bytes=shell_alloc(4096);
  if(!bytes){CloseHandle(h);return (HRESULT)0x8007000eu;}
  BOOL ok=size>=40 && size<=4096 && ReadFile(h,bytes,size,&got,0) && got==size;CloseHandle(h);
  if(!ok || w32_u32(bytes)!=0x4b554943u || w32_u32(bytes+4)!=0x334b4e4cu || !hot_valid(w32_u32(bytes+36))){shell_free(bytes);return E_INVALID;}
  struct link *tmp=shell_alloc(sizeof(*tmp));if(!tmp){shell_free(bytes);return (HRESULT)0x8007000eu;}
  tmp->show=w32_u32(bytes+8);tmp->icon_index=(LONG)w32_u32(bytes+12);tmp->hotkey=(uint16_t)w32_u32(bytes+36);
  char *fields[]={tmp->label,tmp->target,tmp->args,tmp->cwd,tmp->icon};
  const DWORD caps[]={64,260,1024,260,260};DWORD at=40;
  for(unsigned k=0;k<5;k++){
    DWORD n=w32_u32(bytes+16+k*4);WCHAR check[1024];
    if(n>=caps[k] || n>size-at){ok=0;break;}
    for(DWORD j=0;j<n;j++)if(!bytes[at+j])ok=0;
    w32_copy(fields[k],bytes+at,n);
    if(!shell_utf8_decode(fields[k],check,1024))ok=0;
    at+=n;
  }
  shell_free(bytes);
  if(!ok || at!=size || !valid(tmp)){shell_free(tmp);return E_INVALID;}
  struct link *l=i->owner;
  w32_copy(l->label,tmp->label,sizeof(l->label));w32_copy(l->target,tmp->target,sizeof(l->target));
  w32_copy(l->args,tmp->args,sizeof(l->args));w32_copy(l->cwd,tmp->cwd,sizeof(l->cwd));w32_copy(l->icon,tmp->icon,sizeof(l->icon));
  l->show=tmp->show;l->icon_index=tmp->icon_index;l->hotkey=tmp->hotkey;l->dirty=0;shell_copy(l->filename,260,filename);shell_free(tmp);return 0;
}
static HRESULT W32API completed(struct interface *i,const WCHAR *name){(void)i;(void)name;return 0;}
static HRESULT W32API current(struct interface *i,WCHAR **out){
  if(!out)return E_INVALID;*out=0;DWORD n=w32_strlen(i->owner->filename)+1;
  WCHAR *p=shell_alloc(n*2);if(!p)return (HRESULT)0x8007000eu;
  if(!shell_wide(i->owner->filename,p,n)){shell_free(p);return E_INVALID;}*out=p;return 0;
}
#define LINK_VTABLE(suffix) {query,addref,release,path_##suffix,id_get,id_set,description_get_##suffix,description_set_##suffix,directory_get_##suffix,directory_set_##suffix,arguments_get_##suffix,arguments_set_##suffix,hot_get,hot_set,show_get,show_set,icon_get_##suffix,icon_set_##suffix,relative_##suffix,resolve,path_set_##suffix}
static const void *const va[]=LINK_VTABLE(a),*const vw[]=LINK_VTABLE(w);
static const void *const vp[]={query,addref,release,classid,dirty,load,save,completed,current};
struct factory {const void *const *vt;ULONG refs;};
static ULONG W32API f_add(struct factory *f){return ++f->refs;}
static ULONG W32API f_release(struct factory *f){if(f->refs>1)f->refs--;return f->refs;}
static HRESULT W32API f_query(struct factory *f,const SHELL_GUID *id,void **out){if(!out)return E_INVALID;*out=0;if(!shell_guid(id,0) && !shell_guid(id,1))return E_NOINTERFACE;*out=f;f_add(f);return 0;}
static HRESULT W32API create(struct factory *f,void *outer,const SHELL_GUID *id,void **out){
  (void)f;if(!out)return E_INVALID;*out=0;if(outer)return (HRESULT)0x80040110u;
  struct link *l=shell_alloc(sizeof(*l));if(!l)return (HRESULT)0x8007000eu;
  l->a=(struct interface){va,l};l->w=(struct interface){vw,l};l->p=(struct interface){vp,l};l->refs=1;l->show=1;
  HRESULT r=query(&l->a,id,out);release(&l->a);return r;
}
static HRESULT W32API lock_server(struct factory *f,BOOL lock){(void)f;(void)lock;return 0;}
static const void *const vf[]={f_query,f_add,f_release,create,lock_server};
static struct factory factory={vf,1};
HRESULT W32API DllGetClassObject(const SHELL_GUID *cls,const SHELL_GUID *iid,void **out){
  if(!out)return E_INVALID;*out=0;
  if(!shell_guid(cls,0x21401)){
    extern HANDLE W32API GetModuleHandleA(const char *);
    HANDLE shell=GetModuleHandleA("shell32.dll");
    HRESULT(W32API *original)(const SHELL_GUID *,const SHELL_GUID *,void **)=shell?
      (HRESULT(W32API *)(const SHELL_GUID *,const SHELL_GUID *,void **))GetProcAddress(shell,"__ciuki_reused_DllGetClassObject"):0;
    return original?original(cls,iid,out):(HRESULT)0x80040111u;
  }
  return f_query(&factory,iid,out);
}
