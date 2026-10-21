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
/* f5-15B: ProductName of the Microsoft DirectX redistributable launchers
 * varies by release ("Microsoft DirectX", "Microsoft DirectX for Windows",
 * the DirectX 3 "Microsoft(R) DirectX for Windows(R)  95" with registered
 * signs, measured on the Diablo shareware CD): the word DirectX in a
 * Microsoft-company image named DXSETUP.EXE is the identity. */
static int contains(const uint8_t *p,ULONG n,const char *word){
  ULONG length=w32_strlen(word);if(n<2 || (n&1))return 0;
  for(ULONG start=0;start+length*2<=n;start+=2){
    ULONG i=0;
    for(;i<length;i++){
      unsigned a=w32_u16(p+start+i*2),b=(uint8_t)word[i];
      if(a>='a' && a<='z')a-=32;if(b>='a' && b<='z')b-=32;
      if(a!=b)break;
    }
    if(i==length)return 1;
  }
  return 0;
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
    if(text(p+6,key_bytes,"ProductName"))id->product=contains(value,bytes,"DirectX");
  }
  at=(at+bytes+3)&~3u;
  while(at+6<=size){ULONG child=w32_u16(p+at);if(child<6 || child>size-at || !version(p+at,child,depth+1,id))return 0;at=(at+child+3)&~3u;}
  return 1;
}
/* f5-15B: the DirectX 3 DXSETUP.EXE (September 1996, on the Diablo shareware
 * CD) carries no VS_VERSION_INFO at all. Its identity is then structural:
 * the image imports DSETUP.DLL!DirectXSetup or DirectXSetupA by name, which
 * only the redistributable's launcher does. An image with a version resource
 * is judged by that resource alone (the authored parser above). */
static int imports_dsetup(struct w32_pe *p){
  struct w32_directory d=p->dirs[1];
  if(!d.size)return 0;
  for(ULONG i=0;i<256;i++){
    const uint8_t *e=w32_pe_span(p,d.rva+i*20,20);
    if(!e || !w32_u32(e+12))return 0;
    const char *dll=w32_pe_string(p,w32_u32(e+12));
    if(!dll || w32_stricmp(dll,"DSETUP.DLL"))continue;
    ULONG thunks=w32_u32(e)?w32_u32(e):w32_u32(e+16);
    for(ULONG j=0;j<4096;j++){
      const uint8_t *t=w32_pe_span(p,thunks+j*4,4);
      if(!t || !w32_u32(t))break;
      if(w32_u32(t)&0x80000000u)continue;
      const char *name=w32_pe_string(p,w32_u32(t)+2);
      if(name && (!w32_strcmp(name,"DirectXSetupA") || !w32_strcmp(name,"DirectXSetup")))return 1;
    }
  }
  return 0;
}
int w32_installer_intercept(struct w32_loader *l,const char *path){
  const char *base=path;for(const char *p=path;*p;p++)if(*p=='\\' || *p=='/')base=p+1;
  if(w32_stricmp(base,"DXSETUP.EXE"))return 0;
  struct w32_error saved=l->error;struct w32_module *m=0;
  if(!w32_ldr_load(l,path,W32_LOAD_DATAFILE,&m)){l->error=saved;return 0;}
  const void *data=0;ULONG bytes=0;struct identity id={0};
  int ok=0;
  if(!(m->pe.flags&0x2000)){
    if(w32_pe_resource(&m->pe,(struct w32_resource_key){16,0},(struct w32_resource_key){1,0},0xffffffffu,&data,&bytes))
      ok=bytes<=65535 && version(data,bytes,0,&id) && id.fixed && id.original && id.company && id.product;
    else ok=imports_dsetup(&m->pe);
  }
  w32_ldr_free(l,m);l->error=saved;
  if(ok)w32_diag("installer","dxsetup","already-installed-4.07.00.0716");
  return ok;
}
