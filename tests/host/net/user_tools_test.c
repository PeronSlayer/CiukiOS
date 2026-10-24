/* G26 storage, HOSTS and RFC 792 oracle. SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "../../../sdk/libciuki/hosts.h"
#include "../../../apps/terminal/icmp.h"
#include "../../../apps/control/internet_storage.h"
#include "../../../sdk/netsurf/port/download_store.h"
struct source { const char *text;size_t at; };
static int next(void *context) { struct source *s=context;return s->text[s->at]?(unsigned char)s->text[s->at++]:-1; }
static int lookup(const char *text,const char *name,uint32_t *ip)
{ struct source s={text,0};return cu_hosts_lookup(next,&s,name,ip); }
static void hosts(void)
{
    uint32_t ip=42;
    assert(lookup("# ignored\r\n  192.0.2.17 canonical Alias\tSECOND # comment\r\n","aLIas",&ip) && ip==0x110200c0);
    assert(!lookup("300.0.0.1 bad\n1.2.3 missing\n1.2.3.4suffix bad\n1.2.3.4 badsuffix\n","bad",&ip));
    assert(lookup("0.0.0.0 blocked","blocked",&ip) && !ip);
    char longline[2100];memset(longline,'x',sizeof(longline));memcpy(longline,"1.2.3.4 target ",15);strcpy(longline+2000,"\n4.3.2.1 target\n");
    assert(lookup(longline,"target",&ip) && ip==0x01020304);
    assert(!lookup("1.2.3.4 first # alias\n","alias",&ip));
    puts("PASS G26 HOSTS: aliases/case/CRLF/comments/zero/malformed/overlong/EOF");
}
static void checksum(uint8_t *p,size_t n)
{ p[2]=p[3]=0;uint16_t sum=term_icmp_checksum(p,n);p[2]=(uint8_t)(sum>>8);p[3]=(uint8_t)sum; }
static void icmp(void)
{
    uint8_t packet[64]={0x45,0,0,32,0,0,0,0,64,1};packet[12]=127;packet[15]=1;
    uint8_t *p=packet+20;p[4]=0x53;p[5]=0x26;p[7]=9;memcpy(p+8,"woof",4);checksum(p,12);
    assert(term_icmp_match(packet,32,0x5326,9,0x0100007f)==1);
    assert(!term_icmp_match(packet,31,0x5326,9,0x0100007f));
    assert(!term_icmp_match(packet,32,0x5326,8,0x0100007f));
    p[8]^=1;assert(!term_icmp_match(packet,32,0x5326,9,0x0100007f));p[8]^=1;
    uint8_t quoted[56]={0x45,0,0,56,0,0,0,0,64,1};
    quoted[20]=11;memcpy(quoted+28,packet,28);quoted[44]=127;quoted[47]=1;quoted[48]=8;checksum(quoted+20,36);
    assert(term_icmp_match(quoted,56,0x5326,9,0x0100007f)==2);
    quoted[20]=3;checksum(quoted+20,36);assert(term_icmp_match(quoted,56,0x5326,9,0x0100007f)==3);
    assert(!term_icmp_match(quoted,56,0x5326,10,0x0100007f));
    for(unsigned n=0;n<56;n++)assert(!term_icmp_match(quoted,n,0x5326,9,0x0100007f));
    puts("PASS G26 ICMP: echo/TTL/unreachable/checksum/length/quoted identifier/sequence/destination");
}
static void storage(const char *directory)
{
    struct ciuki_download_store s;
    assert(!ciuki_download_begin(&s,directory,"../../unsafe\\name?.exe"));assert(!strcmp(s.name,"name_.exe"));
    char temporary[560];strcpy(temporary,s.temporary);
    assert(!ciuki_download_write(&s,"woof",4));assert(!ciuki_download_complete(&s));
    char destination[560];snprintf(destination,sizeof(destination),"%s/saved.bin",directory);
    FILE *f=fopen(destination,"wb");assert(f);assert(fwrite("old",1,3,f)==3);assert(!fclose(f));
    assert(ciuki_download_save(&s,destination)<0 && errno==EEXIST);
    f=fopen(destination,"rb");assert(f);char data[8]={0};assert(fread(data,1,8,f)==3 && !memcmp(data,"old",3));fclose(f);
    assert(!unlink(destination));assert(!ciuki_download_save(&s,destination));assert(access(temporary,F_OK)<0);
    f=fopen(destination,"rb");assert(f && fread(data,1,8,f)==4 && !memcmp(data,"woof",4));fclose(f);unlink(destination);
    assert(!ciuki_download_begin(&s,directory,".."));assert(!strcmp(s.name,"download"));strcpy(temporary,s.temporary);
    assert(ciuki_download_write(&s,"",8u*1024u*1024u+1)<0 && errno==EFBIG);ciuki_download_discard(&s);assert(access(temporary,F_OK)<0);
    s=(struct ciuki_download_store){.fd=open("/dev/full",O_WRONLY)};assert(s.fd>=0);assert(ciuki_download_write(&s,"x",1)<0);ciuki_download_discard(&s);
    puts("PASS G26 download: streamed bytes/safe names/no overwrite/save/cancel/size/disk errors/cleanup");
}
static void options(const char *directory)
{
    char path[560];snprintf(path,sizeof(path),"%s/internet.cfg",directory);
    struct cp_internet_options o,loaded;assert(!internet_load(path,&o) && o.cache_mb==2);
    strcpy(o.home,"https://example.test/home");strcpy(o.proxy,"proxy.example.test");o.port=3128;o.cache_mb=4;
    assert(!internet_save(path,&o));assert(!internet_load(path,&loaded));assert(!strcmp(o.home,loaded.home) && !strcmp(o.proxy,loaded.proxy) && loaded.port==3128 && loaded.cache_mb==4);
    o.cache_mb=17;assert(internet_save(path,&o)<0 && errno==EINVAL);assert(!internet_load(path,&loaded) && loaded.cache_mb==4);
    o.cache_mb=2;strcpy(o.proxy,"bad\r\nInjected");assert(!internet_valid(&o));
    FILE *f=fopen(path,"wb");assert(f);fputs("home=javascript:bad\n",f);fclose(f);assert(internet_load(path,&o)<0 && !strcmp(o.home,"about:welcome"));unlink(path);
    puts("PASS G26 Internet: defaults/round trip/home/proxy/port/cache validation/preserve previous settings");
}
int main(int argc,char **argv)
{ setvbuf(stdout,NULL,_IOLBF,0);assert(argc==2);hosts();icmp();storage(argv[1]);options(argv[1]);return 0; }
