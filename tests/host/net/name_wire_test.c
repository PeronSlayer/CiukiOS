/* Author-written protocol fixtures, no Internet or system resolver. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../../sdk/libciuki/name_wire.h"
struct input { const char *text;size_t at; };
static int next(void *arg) { struct input *i=arg;return i->text[i->at]?(unsigned char)i->text[i->at++]:-1; }
static int lm(const char *text,const char *name,unsigned suffix,uint32_t *ip)
{ struct input i={text,0};return cu_lmhosts_lookup(next,&i,name,suffix,ip); }
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    uint32_t ip=0;
    assert(lm("#comment\r\n192.0.2.3 server #PRE #DOM:LAN\r\n","SERVER",0x20,&ip) && ip==0x030200c0);
    assert(lm("192.0.2.9 \"Office PC\"\n","office pc",0x20,&ip) && ip==0x090200c0);
    assert(lm("192.0.2.4 \"APP            \\0x14\" #PRE\n","APP",0x14,&ip) && ip==0x040200c0);
    assert(!lm("192.0.2.4 \"APP            \\0x14\"\n","APP",0x20,&ip));
    assert(!lm("999.0.2.4 APP\n192.0.2.4 \"unterminated\n","APP",0x20,&ip));
    assert(!lm("#INCLUDE \\\\server\\share\\lmhosts\n","server",0x20,&ip));
    char *large=malloc(65540);assert(large);memset(large,'x',65539);large[65539]=0;
    assert(!lm(large,"server",0x20,&ip));free(large);
    struct input h={"192.0.2.3 canonical alias\r\n",0};char name[254];
    assert(cu_hosts_reverse(next,&h,0x030200c0,name,sizeof(name)) && !strcmp(name,"canonical"));
    unsigned char query[90],packet[256];size_t qn=cu_ptr_query(query,0x2357,0x030200c0);
    size_t at=12;assert(cu_dns_read_name(query,qn,&at,name,sizeof(name)) && !strcmp(name,"3.2.0.192.in-addr.arpa"));
    memcpy(packet,query,qn);packet[2]=0x81;packet[3]=0x80;packet[7]=1;at=qn;
    packet[at++]=0xc0;packet[at++]=12;packet[at++]=0;packet[at++]=12;packet[at++]=0;packet[at++]=1;
    memset(packet+at,0,4);at+=4;packet[at++]=0;packet[at++]=14;
    packet[at++]=6;memcpy(packet+at,"server",6);at+=6;packet[at++]=5;memcpy(packet+at,"local",5);at+=5;packet[at++]=0;
    assert(cu_name_answer(packet,at,query,qn,NULL,name,sizeof(name)) && !strcmp(name,"server.local"));
    for(size_t n=0;n<at;n++)assert(!cu_name_answer(packet,n,query,qn,NULL,name,sizeof(name)));
    packet[0]^=1;assert(!cu_name_answer(packet,at,query,qn,NULL,name,sizeof(name)));packet[0]^=1;
    packet[qn+1]=(unsigned char)qn;assert(!cu_name_answer(packet,at,query,qn,NULL,name,sizeof(name))); /* pointer loop */
    assert(cu_nbns_query(query,0x1234,"server",0x20)==50);
    memcpy(packet,query,50);packet[2]=0x85;packet[3]=0;packet[4]=packet[5]=0;packet[7]=1;
    at=12;memcpy(packet+at,query+12,34);at+=34;
    packet[at++]=0;packet[at++]=32;packet[at++]=0;packet[at++]=1;memset(packet+at,0,4);at+=4;
    packet[at++]=0;packet[at++]=6;packet[at++]=0;packet[at++]=0;
    packet[at++]=192;packet[at++]=0;packet[at++]=2;packet[at++]=7;
    assert(cu_name_answer(packet,at,query,50,&ip,NULL,0) && ip==0x070200c0);
    packet[13]^=1;assert(!cu_name_answer(packet,at,query,50,&ip,NULL,0));
    unsigned state=1;
    for(unsigned i=0;i<10000;i++) {
        for(unsigned j=0;j<sizeof(packet);j++) { state=state*1664525u+1013904223u;packet[j]=(unsigned char)(state>>24); }
        (void)cu_name_answer(packet,i%sizeof(packet),query,50,&ip,name,sizeof(name));
    }
    puts("PASS G26B LMHOSTS quoted/suffix/case/comments/bounds, HOSTS reverse, PTR/NBNS owner/ID/truncation/cycles and 10000 malformed packets");return 0;
}
