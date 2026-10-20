/* DirectSetup executable admission before import binding or any callback.
 * Authored resource parser, not a filename-only success shim.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/ldr.h>
static int text(const uint8_t *p,ULONG n,const char *expected){
  ULONG length=w32_strlen(expected);if(n!=(length+1)*2)return 0;
  for(ULONG i=0;i<=length;i++){
    unsigned a=w32_u16(p+i*2),b=(uint8_t)expected[i];
    if(a>='a' && a<='z')a-=32;if(b>='a' && b<='z')b-=32;
    if(a!=b)return 0;
  }return 1;
}
struct identity {unsigned original,company,product,fixed;};
static int version(const uint8_t *p,ULONG available,unsigned depth,struct identity *id){
  if(depth>4 || available<6)return 0;
  ULONG size=w32_u16(p),values=w32_u16(p+2),type=w32_u16(p+4);
  if(size<8 || size>available || type>1)return 0;
  ULONG at=6;while(at+2<=size && w32_u16(p+at))at+=2;
  if(at+2>size)return 0;
  ULONG key_bytes=at-6+2;at=(at+2+3)&~3u;
  ULONG bytes=type?values*2:values;
  if(at>size || bytes>size-at)return 0;
  const uint8_t *value=p+at;
  if(!depth){
    if(!text(p+6,key_bytes,"VS_VERSION_INFO") || type || bytes!=52 ||
       w32_u32(value)!=0xfeef04bdu || w32_u32(value+36)!=1)return 0;
    id->fixed=1;
  }else if(type){
    if(text(p+6,key_bytes,"OriginalFilename"))id->original=text(value,bytes,"DXSETUP.EXE");
    if(text(p+6,key_bytes,"CompanyName"))id->company=text(value,bytes,"Microsoft Corporation");
    if(text(p+6,key_bytes,"ProductName"))id->product=text(value,bytes,"Microsoft DirectX") || text(value,bytes,"Microsoft DirectX for Windows");
  }
  at=(at+bytes+3)&~3u;
  while(at+6<=size){ULONG child=w32_u16(p+at);if(child<6 || child>size-at || !version(p+at,child,depth+1,id))return 0;at=(at+child+3)&~3u;}
  return 1;
}
int w32_installer_intercept(struct w32_loader *l,const char *path){
  const char *base=path;for(const char *p=path;*p;p++)if(*p=='\\' || *p=='/')base=p+1;
  if(w32_stricmp(base,"DXSETUP.EXE"))return 0;
  struct w32_error saved=l->error;struct w32_module *m=0;
  if(!w32_ldr_load(l,path,W32_LOAD_DATAFILE,&m)){l->error=saved;return 0;}
  const void *data=0;ULONG bytes=0;struct identity id={0};
  int ok=!(m->pe.flags&0x2000) &&
    w32_pe_resource(&m->pe,(struct w32_resource_key){16,0},(struct w32_resource_key){1,0},0xffffffffu,&data,&bytes) &&
    bytes<=65535 && version(data,bytes,0,&id) && id.fixed && id.original && id.company && id.product;
  w32_ldr_free(l,m);l->error=saved;
  if(ok)w32_diag("installer","dxsetup","already-installed-4.07.00.0716");
  return ok;
}
