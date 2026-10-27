/* Synthetic fixtures only; real CD decoding is in memory and size-only.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../../sdk/win32/is5boot/private.h"
static void put(unsigned char *p,unsigned v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(unsigned char)(v>>(8*i));}
static unsigned bit_at;
static void bits(unsigned char *p,unsigned v,unsigned n){for(unsigned i=0;i<n;i++,bit_at++)p[bit_at/8]|=((v>>i)&1)<<(bit_at%8);}
static unsigned literal(unsigned char *p,const unsigned char *text,unsigned n){
    memset(p,0,n*2+8);p[1]=6;bit_at=16;
    for(unsigned i=0;i<n;i++){bits(p,0,1);bits(p,text[i],8);}
    bits(p,1,1);bits(p,0,7);bits(p,255,8);return (bit_at+7)/8;
}
static unsigned fixture(unsigned char *p){
    memset(p,0,256);const unsigned char sig[]={42,171,121,216,0,1};memcpy(p,sig,6);put(p+78,1,4);
    put(p+82,128,4);put(p+90,5,4);put(p+102,11,2);memcpy(p+104,"INSTALL.EXE",11);
    put(p+115,12,2);memcpy(p+117,"_INS1234._MP",12);
    /* Directory ends at 129: offset must follow it. */
    put(p+82,129,4);unsigned n=literal(p+129,(const unsigned char *)"hello",5);put(p+86,n,4);return 129+n;
}
static void units(void){
    unsigned char packed[1024],out[512],original[256];for(unsigned i=0;i<256;i++)original[i]=(unsigned char)i;
    unsigned size=literal(packed,original,256);assert(is5_dcl(packed,size,out,256));assert(!memcmp(original,out,256));
    assert(!is5_dcl(packed,size,out,255));assert(!is5_dcl(packed,size,out,257));
    for(unsigned i=0;i<size;i++)assert(!is5_dcl(packed,i,out,256));
    packed[0]=2;assert(!is5_dcl(packed,size,out,256));packed[0]=0;packed[1]=3;assert(!is5_dcl(packed,size,out,256));
    const unsigned char match[]={0,4,0x82,0x24,0x25,0x8f,0x80,0x7f};
    assert(is5_dcl(match,sizeof(match),out,13));assert(!memcmp(out,"AIAIAIAIAIAIA",13));
    struct is5_archive a;unsigned char raw[256];size=fixture(raw);assert(is5_directory(raw,size,&a));
    assert(a.count==1 && a.member[0].expanded==5 && !strcmp(a.member[0].runtime,"_INS1234._MP"));
    for(unsigned i=0;i<129;i++)assert(!is5_directory(raw,i,&a));
    raw[0]=0;assert(!is5_directory(raw,size,&a));size=fixture(raw);
    put(raw+82,128,4);assert(!is5_directory(raw,size,&a));size=fixture(raw);
    put(raw+86,0xffffffff,4);assert(!is5_directory(raw,size,&a));size=fixture(raw);
    put(raw+90,0xffffffff,4);assert(!is5_directory(raw,size,&a));size=fixture(raw);
    raw[104]='/';assert(!is5_directory(raw,size,&a));size=fixture(raw);
    put(raw+78,17,4);assert(!is5_directory(raw,size,&a));
    memset(raw,0,sizeof(raw));raw[0]='M';raw[1]='Z';put(raw+60,128,4);raw[128]='N';raw[129]='E';raw[182]=2;
    assert(is5_ne(raw,sizeof(raw)));assert(!is5_ne(raw,191));raw[141]=0x80;assert(!is5_ne(raw,sizeof(raw)));raw[141]=0;
    char text[4096];
    memset(packed,0,sizeof(packed));memcpy(packed,raw,sizeof(raw));packed[141]=0x80;
    put(packed+128+36,64,2);put(packed+194,0x8006,2);put(packed+196,1,2);
    put(packed+202,300,2);put(packed+204,100,2);put(packed+208,0x8000+7068/16+1,2);
    packed[300+7068%16]=8;memcpy(packed+301+7068%16,"Authored",8);
    assert(!is5_ne(packed,sizeof(packed)) && is5_string(packed,sizeof(packed),7068,text,sizeof(text)));
    assert(!strcmp(text,"Authored"));assert(!is5_string(packed,256,7068,text,sizeof(text)));
    assert(is5_arguments(text,sizeof(text),"D:\\","C:\\WINDOWS\\TEMP\\","",""));
    assert(!strcmp(text,"  -fD:\\SETUP.INS  -z1 -cx -xC:\\WINDOWS\\TEMP\\ -x1D:\\ -q10010"));
    assert(is5_arguments(text,sizeof(text),"D:\\","T:\\","-s -f1\"C:\\a b.iss\"","-f2x -SMS -zcustom"));
    assert(!strcmp(text,"  -fD:\\SETUP.INS  -z1 -cx -xT:\\ -x1D:\\ -q10010 -s -f1\"C:\\a b.iss\" -f2x -SMS -zcustom"));
    assert(!is5_arguments(text,8,"D:\\","T:\\","",""));
    const char env[]="A=one\0B=two words\0\0";
    assert(is5_environment(text,sizeof(text),env,sizeof(env)));assert(!strcmp(text,"A=one B=two words"));
    assert(!is5_environment(text,4,env,sizeof(env)));assert(!is5_environment(text,sizeof(text),"A=x",3));
    assert(is5_environment(text,sizeof(text),"\0",1) && !text[0]);
    assert(is5_cleanup_text(text,sizeof(text),"T:\\_INS1234._MP","T:\\ZDataI51.dll","T:\\_WUTL951.DLL"));
    assert(!strcmp(text,"[file0]\r\nmain=T:\\_INS1234._MP\r\nexeostype=1\r\nalt0=T:\\ZDataI51.dll\r\nalt1=T:\\_WUTL951.DLL\r\n"));
    assert(!strcmp(is5_tail("  \"D:\\a b\\SETUP.EXE\" -s -zfoo"),"-s -zfoo"));
    assert(!strcmp(is5_tail("setup.exe -SMS"),"-SMS"));assert(!*is5_tail(""));
    puts("PASS synthetic IS5 archive, bounded DCL, NE admission and exact handoff bytes");
}
static void local(const char *path){
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0 && n<16*1024*1024);rewind(f);
    unsigned char *raw=malloc((size_t)n);assert(raw && fread(raw,1,(size_t)n,f)==(size_t)n);fclose(f);
    struct is5_archive a;assert(is5_directory(raw,(unsigned)n,&a));unsigned found=0;
    for(unsigned i=0;i<a.count;i++){
        struct is5_member *m=a.member+i;unsigned expected=0;
        if(!strcmp(m->original,"INSTALL.EXE"))expected=547840;
        if(!strcmp(m->runtime,"ZDataI51.dll"))expected=53760;
        if(!strcmp(m->runtime,"_WUTL951.DLL"))expected=46592;
        if(!expected)continue;
        assert(m->expanded==expected);unsigned char *data=malloc(expected);assert(data);
        assert(is5_dcl(raw+m->offset,m->packed,data,expected));assert(data[0]=='M' && data[1]=='Z');free(data);found++;
    }
    free(raw);assert(found==3);puts("PASS local CD archive: 547840 / 53760 / 46592 bytes (memory only)");
}
static void local_errors(const char *path){
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0 && n<1024*1024);rewind(f);
    unsigned char *raw=malloc((size_t)n);assert(raw && fread(raw,1,(size_t)n,f)==(size_t)n);fclose(f);
    const unsigned ids[]={7068,7063,7064,7110};char text[512];
    for(unsigned i=0;i<4;i++)assert(is5_string(raw,(unsigned)n,ids[i],text,sizeof(text)) && *text);
    free(raw);puts("PASS local NE DLL: all four localized startup errors found (text not logged)");
}
int main(int argc,char **argv){if(argc==3 && !strcmp(argv[1],"--errors"))local_errors(argv[2]);else if(argc==2)local(argv[1]);else units();return 0;}
