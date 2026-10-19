/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../../../sdk/win32/services/files.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern uint32_t GetFileVersionInfoSizeA(const char *,uint32_t *);
extern int GetFileVersionInfoA(const char *,uint32_t,uint32_t,void *);
extern int VerQueryValueA(const void *,const char *,void **,uint32_t *);
extern int VerQueryValueW(const void *,const WCHAR *,void **,uint32_t *);
extern uint32_t VerFindFileA(uint32_t,const char *,const char *,const char *,char *,uint32_t *,char *,uint32_t *);
extern uint32_t GetFileVersionInfoSizeW(const WCHAR *,uint32_t *);
extern int GetFileVersionInfoW(const WCHAR *,uint32_t,uint32_t,void *);
extern uint32_t last_error_value(void);
extern int w32lz_decode(const uint8_t *,uint32_t,uint8_t **,uint32_t *,char *);
extern int32_t LZOpenFileA(char *,void *,uint32_t);
extern int32_t LZRead(int32_t,char *,int32_t);
extern int32_t LZSeek(int32_t,int32_t,int32_t);
extern int32_t LZCopy(int32_t,int32_t);
extern int32_t LZInit(int32_t);
extern void LZClose(int32_t);
extern int GetExpandedNameA(char *,char *);
void version_tests(const char *image)
{
    uint32_t ignored=99,size=GetFileVersionInfoSizeA(image,&ignored);assert(size>100&&ignored==0);
    uint8_t *bytes=malloc(size);assert(bytes);assert(!GetFileVersionInfoA(image,0,4,bytes));assert(GetFileVersionInfoA(image,0,size,bytes));
    void *out=NULL;uint32_t n=0;assert(VerQueryValueA(bytes,"\\",&out,&n));assert(n==52&&w32_u32(out)==0xfeef04bd);
    assert(VerQueryValueA(bytes,"\\StringFileInfo\\040904E4\\ProductName",&out,&n));assert(!strcmp(out,"CiukiOS"));assert(n==8);
    assert(VerQueryValueA(bytes,"\\VarFileInfo\\Translation",&out,&n));assert(n==4&&w32_u16(out)==0x409&&w32_u16((uint8_t *)out+2)==1252);
    assert(!VerQueryValueA(bytes,"\\Missing",&out,&n));assert(!VerQueryValueA(NULL,"\\",&out,&n));
    WCHAR root[]={'\\',0};assert(VerQueryValueW(bytes,root,&out,&n)&&n==52);
    uint16_t old=w32_u16(bytes);w32_put16(bytes,6);assert(!VerQueryValueW(bytes,root,&out,&n));w32_put16(bytes,old);
    char cur[260],dest[260];uint32_t cl=sizeof(cur),dl=sizeof(dest);
    assert(!VerFindFileA(0,"absent.dll","C:\\WINDOWS","C:\\Game",cur,&cl,dest,&dl));assert(!cur[0]&&cl==1);assert(!strcmp(dest,"C:\\Game"));
    cl=dl=1;assert(VerFindFileA(1,"absent.dll","C:\\WINDOWS","C:\\Game",cur,&cl,dest,&dl)&4);
    /* f5-20A: the Unicode entry points the reused Wine DLLs import open the
     * file through the ANSI path (ACP 1252) and answer like the ANSI ones. */
    WCHAR wide[1024];unsigned i=0;for(;image[i];i++)wide[i]=(uint8_t)image[i];wide[i]=0;
    assert(GetFileVersionInfoSizeW(wide,&ignored)==size);
    uint8_t *again=malloc(size),*fresh=malloc(size);
    assert(again&&fresh&&GetFileVersionInfoA(image,0,size,fresh)&&GetFileVersionInfoW(wide,0,size,again)&&!memcmp(again,fresh,size));free(fresh);
    assert(!GetFileVersionInfoW(wide,0,4,again)&&last_error_value()==122);
    WCHAR beyond[]={'C',':','\\',0x4e00,'.','d','l','l',0};
    assert(!GetFileVersionInfoSizeW(beyond,&ignored)&&last_error_value()==123);
    WCHAR euro[]={'C',':','\\',0x20ac,'.','d','l','l',0};
    w32r_last_error(0);assert(!GetFileVersionInfoSizeW(euro,&ignored)&&last_error_value()!=123);
    free(again);
    free(bytes);
}
static uint8_t *load(const char *path,uint32_t *n)
{
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>=0&&size<1000000);*n=(uint32_t)size;rewind(f);
    uint8_t *b=malloc(*n?*n:1);assert(b);assert(fread(b,1,*n,f)==*n);fclose(f);return b;
}
void lz_tests(const char *dir)
{
    char path[1024];uint8_t *data,*out;uint32_t size,expanded;
    const char *names[]={"szdd.bin","kwaj0.bin","kwaj1.bin","kwaj2.bin","kwaj3.bin","kwaj4.bin"};
    for(unsigned i=0;i<6;i++){
        snprintf(path,sizeof(path),"%s/%s",dir,names[i]);data=load(path,&size);
        int result=w32lz_decode(data,size,&out,&expanded,NULL);if(result)fprintf(stderr,"decode %s: %d\n",names[i],result);
        assert(!result);assert(expanded==5&&!memcmp(out,"hello",5));free(out);
        for(uint32_t cut=0;cut<size;cut++){
            int e=w32lz_decode(data,cut,&out,&expanded,NULL);if(e>=0)fprintf(stderr,"truncate %s at %u/%u\n",names[i],cut,size);assert(e<0);free(out);
        }
        free(data);
    }
    snprintf(path,sizeof(path),"%s/szdd.bin",dir);uint8_t ofs[136];int32_t h=LZOpenFileA(path,ofs,0);assert(h>=0x400);
    char bytes[16];assert(LZRead(h,bytes,2)==2&&!memcmp(bytes,"he",2));assert(LZSeek(h,-1,1)==1);assert(LZRead(h,bytes,4)==4&&!memcmp(bytes,"ello",4));
    assert(LZRead(h,bytes,1)==0);assert(LZSeek(h,-6,2)==-7);assert(LZRead(h,bytes,-1)==-7);assert(LZSeek(h,0,0)==0);
    char output[1024];snprintf(output,sizeof(output),"%s/expanded.txt",dir);int fd=open(output,O_RDWR|O_CREAT|O_TRUNC,0600);assert(fd>=0);assert(LZCopy(h,fd)==5);assert(lseek(fd,0,SEEK_SET)==0);assert(read(fd,bytes,5)==5&&!memcmp(bytes,"hello",5));close(fd);LZClose(h);assert(LZRead(h,bytes,1)==-1);
    fd=open(output,O_RDONLY);assert(fd>=0&&LZInit(fd)==fd);LZClose(fd);
    snprintf(path,sizeof(path),"%s/file.tx_",dir);assert(GetExpandedNameA(path,output)>0);assert(strstr(output,"file.txt"));
}
