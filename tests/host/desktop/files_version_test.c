/* SPDX-License-Identifier: MIT
 * G6 bounded, read-only PE32 resource parsing over the native Files VFS. */
#include "../../../apps/files/model.h"
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned char file[1536];
static void put16(unsigned at,unsigned n) { file[at]=(unsigned char)n; file[at+1]=(unsigned char)(n>>8); }
static void put32(unsigned at,uint32_t n) { put16(at,n); put16(at+2,n>>16); }
static unsigned align4(unsigned at) { return (at+3)&~3u; }
static unsigned key(unsigned at,const char *name)
{
    for (unsigned i=0;;i++) { put16(at+i*2,(unsigned char)name[i]); if (!name[i]) return at+(i+1)*2; }
}
static void fixture(void)
{
    memset(file,0,sizeof(file)); file[0]='M'; file[1]='Z'; put32(60,64);
    put32(64,0x4550); put16(68,0x14c); put16(70,1); put16(84,224); put16(88,0x10b);
    put32(64+116,16); put32(64+136,0x1000); put32(64+140,512);
    put32(312+12,0x1000); put32(312+16,512); put32(312+20,512);
    put16(512+14,1); put32(512+16,16); put32(512+20,0x80000018);
    put16(512+24+14,1); put32(512+40,1); put32(512+44,0x80000030);
    put16(512+48+14,1); put32(512+64,1033); put32(512+68,80);
    put32(512+80,0x1100);
    unsigned root=768; put16(root+2,52); unsigned fixed=align4(key(root+6,"VS_VERSION_INFO"));
    assert(fixed==root+40); put32(fixed,0xfeef04bd); put32(fixed+4,0x10000);
    put32(fixed+8,0x10002); put32(fixed+12,0x30004); put32(fixed+16,0x50006); put32(fixed+20,0x70008);
    unsigned strings=fixed+52; put16(strings+4,1); unsigned table=align4(key(strings+6,"StringFileInfo"));
    put16(table+4,1); unsigned company=align4(key(table+6,"040904B0"));
    put16(company+4,1); put16(company+2,5); unsigned value=align4(key(company+6,"CompanyName"));
    put16(value,'C'); put16(value+2,'a'); put16(value+4,'f'); put16(value+6,0xe9); put16(value+8,0);
    unsigned end=value+10; put16(company,end-company); put16(table,end-table); put16(strings,end-strings); put16(root,end-root);
    put32(512+84,end-root);
}
static void save(int fd) { assert(pwrite(fd,file,sizeof(file),0)==sizeof(file)); }
int main(void)
{
    char path[1200],out[768]; const char *tmp=getenv("TMPDIR"); assert(tmp);
    snprintf(path,sizeof(path),"%s/version-XXXXXX",tmp); int fd=mkstemp(path); assert(fd>=0);
    fixture(); save(fd); assert(!fm_native_version(path,out,sizeof(out)));
    assert(strstr(out,"1.2.3.4") && strstr(out,"5.6.7.8") && strstr(out,"Caf\xc3\xa9"));
    char tiny[1]; assert(!fm_native_version(path,tiny,sizeof(tiny)) && !tiny[0]);
    /* Corrupt headers, resource trees, RVA ranges and version lengths never escape bounds. */
    const unsigned offsets[]={60,70,64+136,312+20,512+20,512+68,512+84,768};
    for (unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);i++) {
        fixture(); put32(offsets[i],0xffffffff); save(fd);
        strcpy(out,"sentinel"); assert(fm_native_version(path,out,sizeof(out))<0 && !out[0]);
    }
    fixture(); file[0]=0x7f; save(fd); assert(fm_native_version(path,out,sizeof(out))<0);
    assert(!ftruncate(fd,12) && fm_native_version(path,out,sizeof(out))<0);
    close(fd); unlink(path);
    puts("PASS G6 PE Version: fixed file/product components, UTF-16 strings, short output, malformed bounds/tree/lengths and truncation");
    return 0;
}
