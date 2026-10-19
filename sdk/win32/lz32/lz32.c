/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include "../services/files.h"
#ifdef W32_HOST_MODEL
#define IMPORT
#else
#define IMPORT __declspec(dllimport)
#endif
IMPORT int32_t W32API _lread(int32_t,void *,uint32_t);
IMPORT int32_t W32API _lwrite(int32_t,const void *,uint32_t);
IMPORT int32_t W32API _llseek(int32_t,int32_t,int32_t);
IMPORT int32_t W32API _lclose(int32_t);
IMPORT int32_t W32API OpenFile(const char *,void *,uint32_t);
IMPORT int W32API WideCharToMultiByte(uint32_t,uint32_t,const WCHAR *,int32_t,char *,int32_t,const char *,int *);
IMPORT int W32API MultiByteToWideChar(uint32_t,uint32_t,const char *,int32_t,WCHAR *,int32_t);
extern int w32lz_decode(const uint8_t *,uint32_t,uint8_t **,uint32_t *,char *);
static struct {int32_t fd;uint8_t *data;uint32_t size,position;int live;} slots[16];
static volatile uint32_t lock;
static void enter(void){while(__atomic_exchange_n(&lock,1,__ATOMIC_ACQUIRE)){} }
static void leave(void){__atomic_store_n(&lock,0,__ATOMIC_RELEASE);}
int W32API LZStart(void){return 1;}
void W32API LZDone(void){}
int32_t W32API LZInit(int32_t fd)
{
    uint8_t magic[14];if(_llseek(fd,0,0)<0)return -1;int32_t got=_lread(fd,magic,14);if(got<0)return -1;
    if(_llseek(fd,0,0)<0)return -1;
    if(got<4||(w32_u32(magic)!=0x44445a53&&w32_u32(magic)!=0x4a41574b))return fd;
    int32_t n=_llseek(fd,0,2);if(n<14||n>64*1024*1024||_llseek(fd,0,0)<0)return -3;
    uint8_t *input=w32lz_alloc((uint32_t)n);if(!input)return -5;
    uint32_t at=0;while(at<(uint32_t)n){int32_t r=_lread(fd,input+at,(uint32_t)n-at);if(r<=0){w32lz_free(input);return -3;}at+=(uint32_t)r;}
    uint8_t *out;uint32_t size;int e=w32lz_decode(input,(uint32_t)n,&out,&size,NULL);w32lz_free(input);if(e)return e;
    enter();unsigned i=0;while(i<16&&slots[i].live)i++;
    if(i==16){leave();w32lz_free(out);return -5;}
    slots[i].fd=fd;slots[i].data=out;slots[i].size=size;slots[i].position=0;slots[i].live=1;leave();return (int32_t)i+0x400;
}
int32_t W32API LZRead(int32_t fd,char *out,int32_t count)
{
    if(count<0||(!out&&count))return -7;if(fd<0x400||fd>=0x410)return _lread(fd,out,(uint32_t)count);
    enter();unsigned i=(unsigned)fd-0x400;if(!slots[i].live){leave();return -1;}
    uint32_t n=(uint32_t)count,available=slots[i].position<slots[i].size?slots[i].size-slots[i].position:0;if(n>available)n=available;
    if(n)w32_copy(out,slots[i].data+slots[i].position,n);slots[i].position+=n;leave();return (int32_t)n;
}
int32_t W32API LZSeek(int32_t fd,int32_t offset,int32_t origin)
{
    if(origin<0||origin>2)return -7;if(fd<0x400||fd>=0x410)return _llseek(fd,offset,origin);
    enter();unsigned i=(unsigned)fd-0x400;if(!slots[i].live){leave();return -1;}
    int64_t pos=(int64_t)offset+(origin==1?slots[i].position:origin==2?slots[i].size:0);
    if(pos<0||pos>INT32_MAX){leave();return -7;}slots[i].position=(uint32_t)pos;leave();return (int32_t)pos;
}
void W32API LZClose(int32_t fd)
{
    if(fd<0x400||fd>=0x410){_lclose(fd);return;}enter();unsigned i=(unsigned)fd-0x400;
    if(slots[i].live){uint8_t *data=slots[i].data;int32_t raw=slots[i].fd;w32_fill(&slots[i],0,sizeof(slots[i]));leave();w32lz_free(data);_lclose(raw);return;}leave();
}
int32_t W32API LZCopy(int32_t source,int32_t dest)
{
    int32_t input=source;int owned=0;
    if(source<0x400||source>=0x410){input=LZInit(source);if(input<0)return input;owned=input!=source;}
    uint8_t buffer[4096];int32_t total=0,result;
    for(;;){result=LZRead(input,(char *)buffer,sizeof(buffer));if(result<=0)break;
        if(total>INT32_MAX-result){result=-7;break;}int32_t n=_lwrite(dest,buffer,(uint32_t)result);if(n!=result){result=-4;break;}total+=result;}
    /* Copy does not close the caller's original raw input handle. */
    if(owned){enter();unsigned i=(unsigned)input-0x400;w32lz_free(slots[i].data);w32_fill(&slots[i],0,sizeof(slots[i]));leave();}
    return result<0?result:total;
}
int32_t W32API CopyLZFile(int32_t s,int32_t d){return LZCopy(s,d);}
int32_t W32API LZOpenFileA(char *name,void *ofs,uint32_t style)
{
    int32_t fd=OpenFile(name,ofs,style);if(fd<0||(style&3u)||(style&0x7400u))return fd;
    int32_t out=LZInit(fd);if(out<0)_lclose(fd);return out;
}
int32_t W32API LZOpenFileW(WCHAR *name,void *ofs,uint32_t style)
{
    char path[1040];if(!name||!WideCharToMultiByte(0,0,name,-1,path,sizeof(path),NULL,NULL))return -7;return LZOpenFileA(path,ofs,style);
}
int W32API GetExpandedNameA(char *name,char *out)
{
    if(!name||!out)return -7;uint8_t ofs[136],h[14];int32_t fd=OpenFile(name,ofs,0);if(fd<0)return -1;
    int32_t n=_lread(fd,h,14);_lclose(fd);uint32_t len=w32_strlen(name);w32_copy(out,name,len+1);
    if(n==14&&w32_u32(h)==0x44445a53&&len&&out[len-1]=='_'){
        uint8_t c=h[9];int lower=1;for(uint32_t i=len-1;i>0;i--){char x=out[i-1];if(x>='a'&&x<='z'){lower=1;break;}if(x>='A'&&x<='Z'){lower=0;break;}}
        if(lower&&c>='A'&&c<='Z')c+=32;if(!lower&&c>='a'&&c<='z')c-=32;out[len-1]=(char)c;
    }
    if(len&&out[len-1]=='.')out[len-1]=0;return 1;
}
int W32API GetExpandedNameW(WCHAR *name,WCHAR *out)
{
    char a[1040],b[1040];if(!name||!out||!WideCharToMultiByte(0,0,name,-1,a,sizeof(a),NULL,NULL))return -7;
    int e=GetExpandedNameA(a,b);if(e>0&&!MultiByteToWideChar(0,0,b,-1,out,1040))return -7;return e;
}
