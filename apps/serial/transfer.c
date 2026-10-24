/* SPDX-License-Identifier: MIT
 * Wire formats: Forsberg X/YMODEM and ZMODEM references; Columbia Kermit
 * Protocol Manual, short stop-and-wait packets and type-1 checksum.
 * The caller chooses the local file: remote names are metadata only. */
#include "transfer.h"
#include "xmodem.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void fail(struct transfer *t,const char *why)
{ t->state=-1;snprintf(t->message,sizeof(t->message),"%s",why); }
static void put(struct transfer *t,uint8_t c)
{ if(t->out_length<sizeof(t->out))t->out[t->out_length++]=c;else fail(t,"Output overflow"); }
static void emit(struct transfer *t,const uint8_t *p,size_t n)
{ for(size_t i=0;i<n;i++)put(t,p[i]); }
static void remember(struct transfer *t,size_t start)
{
    size_t n=t->out_length-start;
    if(n>sizeof(t->last)) { fail(t,"Packet too large");return; }
    memcpy(t->last,t->out+start,n);t->last_length=n;t->quiet=0;
}
static void repeat(struct transfer *t)
{
    if(++t->retries>10) { fail(t,"Retry limit exceeded");return; }
    emit(t,t->last,t->last_length);t->quiet=0;
}
size_t transfer_take(struct transfer *t,uint8_t *out,size_t cap)
{
    size_t n=t->out_length<cap ? t->out_length : cap;
    memcpy(out,t->out,n);memmove(t->out,t->out+n,t->out_length-n);t->out_length-=n;return n;
}
static int write_data(struct transfer *t,const uint8_t *p,size_t n)
{
    if(t->position>UINT32_MAX-n || (t->known_size && n>t->size-t->position)) { fail(t,"Invalid file length");return 0; }
    if(n && t->io.write(t->io.ctx,p,n)) { fail(t,"File write failed");return 0; }
    t->position+=(uint32_t)n;return 1;
}
static void info(struct transfer *t,uint8_t *out,size_t cap)
{
    memset(out,0,cap);size_t n=strlen(t->name);
    memcpy(out,t->name,n+1);snprintf((char *)out+n+1,cap-n-1,"%u",t->size);
}
static int read_info(struct transfer *t,const uint8_t *p,size_t n)
{
    const uint8_t *zero=memchr(p,0,n);
    if(!zero || zero==p) { fail(t,"Missing remote filename");return 0; }
    size_t at=(size_t)(zero-p)+1;t->known_size=0;
    if(at<n && p[at]>='0' && p[at]<='9') {
        uint32_t size=0;
        while(at<n && p[at]>='0' && p[at]<='9') {
            unsigned digit=p[at++]-'0';if(size>(UINT32_MAX-digit)/10) { fail(t,"File too large");return 0; }
            size=size*10+digit;
        }
        if(at==n || (p[at] && p[at]!=' ')) { fail(t,"Invalid file size");return 0; }
        t->size=size;t->known_size=1;
    }
    return 1;
}
/* ---------------- YMODEM ---------------- */
enum { Y_HEADER,Y_HEADER_ACK,Y_DATA_C,Y_DATA,Y_EOT,Y_END_C,Y_END_ACK,Y_RECEIVE,Y_RECEIVE_END };
static void yblock(struct transfer *t,const uint8_t *p,size_t size,unsigned number)
{
    size_t at=t->out_length;
    put(t,size==1024 ? 2 : 1);put(t,(uint8_t)number);put(t,(uint8_t)~number);
    emit(t,p,size);uint16_t crc=xmodem_crc16(p,size);put(t,(uint8_t)(crc>>8));put(t,(uint8_t)crc);remember(t,at);
}
static void ynext(struct transfer *t)
{
    uint8_t data[1024];int n=t->io.read(t->io.ctx,data,sizeof(data));
    if(n<0 || n>1024) { fail(t,"File read failed");return; }
    if(!n) { size_t at=t->out_length;put(t,4);remember(t,at);t->phase=Y_EOT;return; }
    size_t size=n>128 ? 1024 : 128;
    memset(data+n,0x1a,size-(size_t)n);t->position+=(unsigned)n;
    yblock(t,data,size,t->seq);t->phase=Y_DATA;
}
static void ybyte(struct transfer *t,uint8_t c)
{
    if(t->sending) {
        if(t->phase==Y_HEADER && c=='C') {
            uint8_t data[128];info(t,data,sizeof(data));yblock(t,data,sizeof(data),0);t->phase=Y_HEADER_ACK;
        } else if(t->phase==Y_HEADER_ACK && c==6)t->phase=Y_DATA_C;
        else if(t->phase==Y_DATA_C && c=='C') { t->seq=1;ynext(t); }
        else if(t->phase==Y_DATA && c==6) { t->seq=(t->seq+1)&255;t->retries=0;ynext(t); }
        else if(t->phase==Y_EOT && c==6)t->phase=Y_END_C;
        else if(t->phase==Y_END_C && c=='C') {
            uint8_t empty[128]={0};yblock(t,empty,sizeof(empty),0);t->phase=Y_END_ACK;
        } else if(t->phase==Y_END_ACK && c==6)t->state=1;
        else if(c==0x15 && t->last_length)repeat(t);
        return;
    }
    if(!t->used) {
        if(c==4) {
            if(t->phase==Y_RECEIVE_END) { put(t,6);put(t,'C');return; }
            if(t->phase!=Y_RECEIVE || (t->known_size && t->position!=t->size)) { fail(t,"Premature end of file");return; }
            /* Standard two-EOT handshake. */
            if(!t->end) { t->end=1;put(t,0x15);return; }
            t->phase=Y_RECEIVE_END;put(t,6);put(t,'C');return;
        }
        if(c!=1 && c!=2)return;
        t->want=(c==1 ? 128 : 1024)+5;t->frame[t->used++]=c;return;
    }
    t->frame[t->used++]=c;if(t->used<t->want)return;
    t->used=0;size_t size=t->want-5;uint8_t *data=t->frame+3;
    uint16_t crc=xmodem_crc16(data,size);unsigned number=t->frame[1];
    if((uint8_t)(number+t->frame[2])!=255 || data[size]!=(uint8_t)(crc>>8) || data[size+1]!=(uint8_t)crc) { if(++t->retries>10)fail(t,"Too many corrupt blocks");else put(t,0x15);return; }
    if(t->phase==Y_HEADER || t->phase==Y_RECEIVE_END) {
        if(number) { put(t,0x15);return; }
        if(!data[0]) { put(t,6);t->state=1;return; }
        if(t->phase==Y_RECEIVE_END) { fail(t,"Select a new local destination for the next file");return; }
        if(!read_info(t,data,size))return;
        t->phase=Y_RECEIVE;t->seq=1;put(t,6);put(t,'C');return;
    }
    if(number==((t->seq-1)&255)) { put(t,6);if(!t->position)put(t,'C');return; }
    if(number!=t->seq) { put(t,0x15);return; }
    size_t n=size;
    if(t->known_size && n>t->size-t->position)n=t->size-t->position;
    if(!write_data(t,data,n))return;
    t->seq=(t->seq+1)&255;t->retries=0;put(t,6);
}
/* ---------------- Kermit ---------------- */
static const uint8_t kparams[]={94+32,5+32,32,64,13+32,'#','N','1',' '};
static uint8_t kcheck(const uint8_t *p,size_t n)
{ unsigned sum=0;while(n--)sum+=*p++;return (uint8_t)(32+((sum+((sum&0xc0)>>6))&63)); }
static void kpacket(struct transfer *t,unsigned seq,uint8_t type,const uint8_t *data,size_t n,int save)
{
    size_t at=t->out_length;uint8_t packet[100];
    if(n>91) { fail(t,"Kermit packet too long");return; }
    packet[0]=1;packet[1]=(uint8_t)(n+3+32);packet[2]=(uint8_t)(seq+32);packet[3]=type;
    if(n)memcpy(packet+4,data,n);
    packet[4+n]=kcheck(packet+1,n+3);packet[5+n]=(uint8_t)t->keol;
    for(unsigned i=0;i<t->kpad;i++)put(t,(uint8_t)t->kpadchar);
    emit(t,packet,n+6);if(save)remember(t,at);
}
static void knegotiate(struct transfer *t,const uint8_t *p,size_t n)
{
    t->kmax=n>0 && p[0]>=42 ? (unsigned)p[0]-32 : 80;
    if(t->kmax>94)t->kmax=94;
    t->kpad=n>2 && p[2]>=32 ? (unsigned)p[2]-32 : 0;
    t->kpadchar=n>3 ? p[3]^64u : 0;
    t->keol=n>4 && p[4]>=32 && p[4]<64 ? (unsigned)p[4]-32 : 13;
    t->kquote=n>5 && p[5]>32 && p[5]<127 ? p[5] : '#';
    t->k8=n>6 && p[6]!='N' && p[6]!=' ' && p[6]!='Y';t->kbin=t->k8 ? p[6] : '&';
}
static size_t kencode(struct transfer *t,const uint8_t *p,size_t n,uint8_t *out)
{
    size_t at=0;
    for(size_t i=0;i<n;i++) {
        uint8_t c=p[i];
        if(t->k8 && c&128) { out[at++]=(uint8_t)t->kbin;c&=127; }
        if((c&127)<32 || (c&127)==127) { out[at++]=(uint8_t)t->kquote;c^=64; }
        else if(c==t->kquote || (t->k8 && c==t->kbin))out[at++]=(uint8_t)t->kquote;
        out[at++]=c;
    }
    return at;
}
static void knext(struct transfer *t)
{
    uint8_t data[304],raw[30];
    if(t->phase=='S') {
        t->phase='F';size_t n=strlen(t->name);
        size_t encoded=kencode(t,(const uint8_t *)t->name,n,data);
        if(encoded>t->kmax-3) { fail(t,"Filename exceeds negotiated packet size");return; }
        kpacket(t,t->seq,'F',data,encoded,1);
    } else if(t->phase=='F' || t->phase=='D') {
        size_t want=(t->kmax-3)/3;if(want>sizeof(raw))want=sizeof(raw);
        int n=t->io.read(t->io.ctx,raw,want);
        if(n<0 || (size_t)n>want) { fail(t,"File read failed");return; }
        if(!n) { t->phase='Z';kpacket(t,t->seq,'Z',NULL,0,1); }
        else { t->phase='D';t->position+=(unsigned)n;size_t encoded=kencode(t,raw,(size_t)n,data);kpacket(t,t->seq,'D',data,encoded,1); }
    } else if(t->phase=='Z') { t->phase='B';kpacket(t,t->seq,'B',NULL,0,1); }
    else if(t->phase=='B')t->state=1;
}
static void kframe(struct transfer *t)
{
    uint8_t *p=t->frame;size_t len=t->used; /* MARK LEN SEQ TYPE DATA CHECK */
    if(len<5 || p[2]<32 || p[2]>95 || p[len-1]!=kcheck(p+1,len-2)) {
        if(++t->retries>10)fail(t,"Too many corrupt packets");
        else if(!t->sending)kpacket(t,t->seq,'N',NULL,0,0);return;
    }
    unsigned seq=p[2]-32,type=p[3];uint8_t *data=p+4;size_t n=len-5;
    if(type=='E') { fail(t,"Remote Kermit error");return; }
    if(t->sending) {
        if(type=='N' && (seq==t->seq || seq==((t->seq+1)&63))) { repeat(t);return; }
        if(type!='Y' || seq!=t->seq)return;
        if(t->phase=='S') {
            if(n>7 && data[7]!='1' && data[7]!=' ') { fail(t,"Unsupported Kermit checksum negotiation");return; }
            knegotiate(t,data,n);
        }
        t->seq=(t->seq+1)&63;t->retries=0;knext(t);return;
    }
    if(seq==((t->seq+63)&63)) { /* duplicate after lost ACK */
        kpacket(t,seq,'Y',type=='S' ? kparams : NULL,type=='S' ? sizeof(kparams) : 0,0);return;
    }
    if(seq!=t->seq) { kpacket(t,t->seq,'N',NULL,0,0);return; }
    if(type=='S' && !t->phase) { knegotiate(t,data,n);t->phase='S'; }
    else if(type=='F' && t->phase=='S')t->phase='F';
    else if(type=='D' && (t->phase=='F' || t->phase=='D')) {
        uint8_t decoded[94];size_t at=0;
        for(size_t i=0;i<n;i++) {
            uint8_t c=data[i];
            if(c=='#') {
                if(++i==n) { fail(t,"Invalid Kermit quoting");return; }
                c=data[i];if((c&127)>=63 && (c&127)<=95)c^=64;
            }
            decoded[at++]=c;
        }
        if(!write_data(t,decoded,at))return;
        t->phase='D';
    } else if(type=='Z' && (t->phase=='F' || t->phase=='D'))t->phase='Z';
    else if(type=='B' && (t->phase=='Z' || t->phase=='S'))t->state=1;
    else { kpacket(t,t->seq,'N',NULL,0,0);return; }
    kpacket(t,seq,'Y',type=='S' ? kparams : NULL,type=='S' ? sizeof(kparams) : 0,0);
    t->seq=(t->seq+1)&63;t->retries=0;
}
static void kbyte(struct transfer *t,uint8_t c)
{
    if(!t->used) { if(c==1)t->frame[t->used++]=c;return; }
    if(t->used==1) {
        if(c<35 || c>126) { t->used=0;return; }
        t->want=(size_t)c-32+2;
    }
    if(t->used>=sizeof(t->frame)) { fail(t,"Packet overflow");return; }
    t->frame[t->used++]=c;
    if(t->used==t->want) { kframe(t);t->used=0; }
}
/* ---------------- ZMODEM (CRC16, bounded ZCRCW sender) ---------------- */
enum { ZRQINIT=0,ZRINIT=1,ZSINIT=2,ZACK=3,ZFILE=4,ZSKIP=5,ZNAK=6,ZABORT=7,ZFIN=8,ZRPOS=9,ZDATA=10,ZEOF=11,ZFERR=12 };
enum { Z_INIT,Z_FILE,Z_DATA,Z_EOF,Z_FIN,Z_COMPLETE };
static uint32_t position(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void zquote(struct transfer *t,uint8_t c)
{
    if((c&127)<32 || c==127 || c==255) { put(t,24);put(t,c==127 ? 'l' : c==255 ? 'm' : c^64); }
    else put(t,c);
}
static void zheader(struct transfer *t,unsigned type,uint32_t pos,int binary)
{
    uint8_t h[5]={(uint8_t)type,(uint8_t)pos,(uint8_t)(pos>>8),(uint8_t)(pos>>16),(uint8_t)(pos>>24)};
    uint16_t crc=xmodem_crc16(h,sizeof(h));
    put(t,'*');if(!binary)put(t,'*');put(t,24);put(t,binary ? 'A' : 'B');
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<7;i++) {
        uint8_t c=i<5 ? h[i] : i==5 ? (uint8_t)(crc>>8) : (uint8_t)crc;
        if(binary)zquote(t,c);else { put(t,(uint8_t)hex[c>>4]);put(t,(uint8_t)hex[c&15]); }
    }
    if(!binary) { put(t,13);put(t,10);if(type!=ZFIN && type!=ZACK)put(t,17); }
}
static void zdata(struct transfer *t,const uint8_t *p,size_t n,uint8_t end)
{
    uint8_t crcdata[1025];memcpy(crcdata,p,n);crcdata[n]=end;
    uint16_t crc=xmodem_crc16(crcdata,n+1);
    for(size_t i=0;i<n;i++)zquote(t,p[i]);
    put(t,24);put(t,end);zquote(t,(uint8_t)(crc>>8));zquote(t,(uint8_t)crc);
}
static void zfile(struct transfer *t)
{
    uint8_t metadata[256];info(t,metadata,sizeof(metadata));size_t at=t->out_length;
    zheader(t,ZFILE,0,1);zdata(t,metadata,strlen(t->name)+1+strlen((char *)metadata+strlen(t->name)+1)+1,'k');
    remember(t,at);t->phase=Z_FILE;
}
static void znext(struct transfer *t)
{
    uint8_t data[1024];size_t want=t->kmax && t->kmax<1024 ? t->kmax : 1024;
    int n=t->io.read(t->io.ctx,data,want);size_t at=t->out_length;
    if(n<0 || n>1024) { fail(t,"File read failed");return; }
    if(!n) { zheader(t,ZEOF,t->position,0);t->phase=Z_EOF; }
    else { zheader(t,ZDATA,t->position,1);zdata(t,data,(size_t)n,'k');t->position+=(unsigned)n;t->phase=Z_DATA; }
    remember(t,at);
}
static void zresponse(struct transfer *t,unsigned type,uint32_t pos)
{ size_t at=t->out_length;zheader(t,type,pos,0);remember(t,at); }
static void zreceived_header(struct transfer *t)
{
    uint8_t *h=t->header;
    if(xmodem_crc16(h,5)!=((unsigned)h[5]<<8|h[6])) { if(++t->retries>10) { fail(t,"Too many corrupt headers");return; }zresponse(t,t->sending ? ZNAK : ZRPOS,t->position);return; }
    unsigned type=h[0];uint32_t pos=position(h+1);t->header_type=(int)type;
    if(type==ZABORT || type==ZFERR) { fail(t,"Remote transfer failed");return; }
    if(t->sending) {
        if(type==ZRINIT) {
            if(t->phase==Z_INIT || t->phase==Z_FILE) { t->kmax=pos&65535u;zfile(t); }
            else if(t->phase==Z_EOF) { zresponse(t,ZFIN,0);t->phase=Z_FIN; }
        } else if(type==ZRPOS && t->phase!=Z_FIN) {
            if(pos>t->size || t->io.seek(t->io.ctx,pos)) { fail(t,"Invalid restart offset");return; }
            t->position=pos;t->retries=0;znext(t);
        } else if(type==ZACK && t->phase==Z_DATA && pos==t->position) { t->retries=0;znext(t); }
        else if(type==ZFIN && t->phase==Z_FIN) { put(t,'O');put(t,'O');t->state=1; }
        else if(type==ZSKIP) { fail(t,"Receiver skipped the file"); }
        else if(type==ZNAK)repeat(t);
        return;
    }
    if(type==ZRQINIT)zresponse(t,ZRINIT,1024);
    else if(type==ZFILE || type==ZSINIT) { t->parse=10;t->used=0;t->end=0;t->escape=0; }
    else if(type==ZDATA) {
        if(t->phase!=Z_DATA || pos!=t->position)zresponse(t,ZRPOS,t->position);
        else { t->parse=10;t->used=0;t->end=0;t->escape=0; }
    } else if(type==ZEOF) {
        if(t->phase==Z_DATA && pos==t->position && (!t->known_size || pos==t->size)) {
            t->phase=Z_EOF;zresponse(t,ZRINIT,1024);
        } else if(t->phase==Z_EOF && pos==t->position)zresponse(t,ZRINIT,1024);
        else zresponse(t,ZRPOS,t->position);
    } else if(type==ZFIN && (t->phase==Z_EOF || t->phase==Z_COMPLETE)) { zresponse(t,ZFIN,0);t->phase=Z_COMPLETE;t->state=1; }
}
static void zreceived_data(struct transfer *t)
{
    size_t n=t->used-2;uint16_t got=(uint16_t)((unsigned)t->frame[n]<<8|t->frame[n+1]);
    t->frame[n]=(uint8_t)t->end;
    if(xmodem_crc16(t->frame,n+1)!=got) {
        t->parse=0;t->used=0;if(++t->retries>10) { fail(t,"Too many corrupt subpackets");return; }zresponse(t,ZRPOS,t->position);return;
    }
    if(t->header_type==ZFILE) {
        if(t->phase==Z_INIT) {
            if(!read_info(t,t->frame,n))return;
            t->phase=Z_DATA;
        }
        zresponse(t,ZRPOS,t->position);
    } else if(t->header_type==ZSINIT)zresponse(t,ZACK,1);
    else if(t->header_type==ZDATA) {
        if(!write_data(t,t->frame,n))return;
        if(t->end=='j' || t->end=='k')zresponse(t,ZACK,t->position);
    }
    t->retries=0;
    t->parse=t->end=='i' || t->end=='j' ? 10 : 0;t->used=0;t->end=0;t->escape=0;
}
static int unquote(uint8_t c)
{ return c=='l' ? 127 : c=='m' ? 255 : (c&0x60)==0x40 ? c^64 : -1; }
static int hexval(uint8_t c)
{ return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : -1; }
static void zbyte(struct transfer *t,uint8_t c)
{
    /* Transport XON/XOFF never contributes to header/data CRC. */
    if(c==17 || c==19 || c==145 || c==147)return;
    if(t->hex_half<0) { if(c==13 || c==10 || c==138)return;t->hex_half=0; }
    if(t->parse==10) {
        if(t->escape) {
            t->escape=0;
            if(!t->end && c>='h' && c<='k') { t->end=c;t->crc_bytes=0;return; }
            int v=unquote(c);if(v<0) { t->parse=0;t->used=0;zresponse(t,ZRPOS,t->position);return; }
            c=(uint8_t)v;
        } else if(c==24) { t->escape=1;return; }
        if(t->used>=1026) { fail(t,"ZMODEM data too long");return; }
        t->frame[t->used++]=c;
        if(t->end && ++t->crc_bytes==2)zreceived_data(t);
        return;
    }
    if(t->parse==3) { /* binary CRC16 header */
        if(t->escape) { int v=unquote(c);t->escape=0;if(v<0) { t->parse=0;return; }c=(uint8_t)v; }
        else if(c==24) { t->escape=1;return; }
        t->header[t->header_used++]=c;
        if(t->header_used==7) { t->parse=0;zreceived_header(t); }
        return;
    }
    if(t->parse==4) {
        int v=hexval(c);if(v<0) { t->parse=0;return; }
        if(!t->hex_half) { t->header[t->header_used]=(uint8_t)(v<<4);t->hex_half=1; }
        else {
            t->header[t->header_used++]|=(uint8_t)v;t->hex_half=0;
            if(t->header_used==7) { t->parse=0;zreceived_header(t);t->hex_half=-1; }
        }
        return;
    }
    if(c=='*') { t->parse=1;return; }
    if(t->parse==1 && c==24) { t->parse=2;return; }
    if(t->parse==2 && (c=='A' || c=='B')) { t->parse=c=='A' ? 3 : 4;t->header_used=0;t->escape=0;t->hex_half=0;return; }
    t->parse=0;
}

int transfer_init(struct transfer *t,int protocol,int sending,const struct transfer_io *io,const char *name,uint32_t size)
{
    if(protocol<TRANSFER_YMODEM || protocol>TRANSFER_KERMIT || !io || !io->read || !io->write || !io->seek || !name || !*name || strlen(name)>100)return -EINVAL;
    memset(t,0,sizeof(*t));t->io=*io;t->protocol=protocol;t->sending=sending;t->size=size;t->known_size=sending;
    snprintf(t->name,sizeof(t->name),"%s",name);snprintf(t->message,sizeof(t->message),"Waiting for peer");
    t->kmax=80;t->kquote='#';t->keol=13;
    if(protocol==TRANSFER_KERMIT && sending) { t->phase='S';kpacket(t,0,'S',kparams,sizeof(kparams),1); }
    if(protocol==TRANSFER_ZMODEM) { if(sending)zresponse(t,ZRQINIT,0);else zresponse(t,ZRINIT,1024); }
    if(protocol==TRANSFER_YMODEM && !sending)put(t,'C');
    return 0;
}
size_t transfer_input(struct transfer *t,const uint8_t *in,size_t n)
{
    size_t at=0;
    while(at<n && t->state>=0 && sizeof(t->out)-t->out_length>=2400) {
        uint8_t c=in[at++];t->quiet=0;
        if(t->protocol!=TRANSFER_KERMIT) {
            if(c==24 && (t->protocol==TRANSFER_ZMODEM || !t->used)) { if(++t->cans>=(t->protocol==TRANSFER_YMODEM ? 2u : 5u)) { t->state=-2;snprintf(t->message,sizeof(t->message),"Cancelled by peer");break; } }
            else t->cans=0;
        }
        if(t->protocol==TRANSFER_YMODEM)ybyte(t,c);
        else if(t->protocol==TRANSFER_KERMIT)kbyte(t,c);
        else zbyte(t,c);
    }
    return at;
}
void transfer_tick(struct transfer *t,unsigned ms)
{
    if(t->state || t->out_length)return;
    if(ms>10000)ms=10000;
    t->quiet+=ms;
    unsigned timeout=t->sending ? 10000 : 20000;
    if(t->quiet<timeout)return;
    t->quiet=0;
    if(++t->retries>10) { fail(t,"Peer timed out");return; }
    if(t->sending) { if(t->last_length)emit(t,t->last,t->last_length); }
    else if(t->protocol==TRANSFER_YMODEM) {
        t->used=0;put(t,t->phase==Y_HEADER || t->phase==Y_RECEIVE_END ? 'C' : 0x15);
    } else if(t->protocol==TRANSFER_KERMIT) { t->used=0;kpacket(t,t->seq,'N',NULL,0,0); }
    else { t->parse=t->used=t->escape=0;zresponse(t,t->phase==Z_INIT ? ZRINIT : ZRPOS,t->phase==Z_INIT ? 1024 : t->position); }
}
void transfer_cancel(struct transfer *t)
{
    t->out_length=0;
    if(t->protocol==TRANSFER_KERMIT) {
        static const uint8_t text[]="Transfer cancelled";kpacket(t,t->seq,'E',text,sizeof(text)-1,0);
    } else for(unsigned i=0;i<8;i++)put(t,24);
    t->state=-2;snprintf(t->message,sizeof(t->message),"Transfer cancelled");
}
