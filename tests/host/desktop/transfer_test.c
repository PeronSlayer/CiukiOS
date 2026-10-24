/* SPDX-License-Identifier: MIT
 * Production wire engines, binary round trips over fragmented lossy links. */
#include "../../../apps/serial/transfer.h"
#include "../../../apps/serial/xmodem.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct file { uint8_t data[12000];size_t n,at;int bad; };
static int get(void *ctx,uint8_t *p,size_t n)
{ struct file *f=ctx;if(f->bad)return -1;if(n>f->n-f->at)n=f->n-f->at;memcpy(p,f->data+f->at,n);f->at+=n;return (int)n; }
static int put(void *ctx,const uint8_t *p,size_t n)
{ struct file *f=ctx;if(f->bad || n>sizeof(f->data)-f->n)return -1;memcpy(f->data+f->n,p,n);f->n+=n;return 0; }
static int seek(void *ctx,uint32_t at)
{ struct file *f=ctx;if(at>f->n)return -1;f->at=at;return 0; }
static struct transfer sender,receiver;
static struct file source,dest;
static void run(int proto,size_t size,int damage)
{
    memset(&source,0,sizeof(source));memset(&dest,0,sizeof(dest));source.n=size;
    for(size_t i=0;i<size;i++)source.data[i]=(uint8_t)(i*73+(i/256)*19);
    if(size)source.data[size-1]=0x1a; /* a real binary tail, never padding */
    struct transfer_io a={get,put,seek,&source},b={get,put,seek,&dest};
    assert(!transfer_init(&sender,proto,1,&a,"binary.dat",(uint32_t)size));
    assert(!transfer_init(&receiver,proto,0,&b,"local.dat",0));
    int changed=0;
    for(unsigned loop=0;loop<200000 && (!sender.state || !receiver.state);loop++) {
        uint8_t bytes[2400];size_t n;
        /* Corrupt/drop one complete frame/response. Later iterations vary
         * fragmentation down to one byte, exercising parser state retention. */
        if((n=transfer_take(&sender,bytes,changed ? loop%31+1 : sizeof(bytes)))) {
            if(damage==1 && !changed && sender.phase && sender.position) { bytes[n/2]^=1;changed=1; }
            assert(transfer_input(&receiver,bytes,n)==n || receiver.state<0);
        }
        if((n=transfer_take(&receiver,bytes,changed ? loop%17+1 : sizeof(bytes)))) {
            if(damage==2 && !changed && receiver.position)changed=1;
            else assert(transfer_input(&sender,bytes,n)==n || sender.state<0);
        }
        if(sender.state<0 || receiver.state<0)break;
        transfer_tick(&sender,20);transfer_tick(&receiver,20);
    }
    if(sender.state!=1 || receiver.state!=1)fprintf(stderr,"protocol %d size %zu damage %d sender %d/%d %s receiver %d/%d %s at %u/%u\n",proto,size,damage,sender.state,sender.phase,sender.message,receiver.state,receiver.phase,receiver.message,sender.position,receiver.position);
    assert(sender.state==1 && receiver.state==1);
    assert(dest.n==size && !memcmp(source.data,dest.data,size));
    if(damage && size)assert(changed);
}
static void vectors(void)
{
    assert(xmodem_crc16((const uint8_t *)"123456789",9)==0x31c3);
    struct file f={0};struct transfer_io io={get,put,seek,&f};uint8_t bytes[2400];
    assert(!transfer_init(&sender,TRANSFER_YMODEM,1,&io,"a",0));
    assert(transfer_input(&sender,(const uint8_t *)"C",1)==1);
    size_t n=transfer_take(&sender,bytes,sizeof(bytes));
    assert(n==133 && bytes[0]==1 && bytes[1]==0 && bytes[2]==255 && bytes[3]=='a' && bytes[4]==0 && bytes[5]=='0');
    assert(!transfer_init(&sender,TRANSFER_ZMODEM,1,&io,"a",0));
    n=transfer_take(&sender,bytes,sizeof(bytes));
    assert(n==21 && !memcmp(bytes,"**\030B00000000000000\r\n\021",21));
    assert(!transfer_init(&sender,TRANSFER_KERMIT,1,&io,"a",0));
    n=transfer_take(&sender,bytes,sizeof(bytes));assert(n==15 && bytes[0]==1 && bytes[1]==44 && bytes[2]==32 && bytes[3]=='S');
    transfer_cancel(&sender);assert(sender.state==-2 && sender.out_length);
    assert(!transfer_init(&sender,TRANSFER_YMODEM,1,&io,"a",0));
    for(unsigned i=0;i<12;i++)transfer_tick(&sender,10000);
    assert(sender.state==-1);
    assert(!transfer_init(&receiver,TRANSFER_YMODEM,0,&io,"a",0));
    uint8_t cancel[]={24,24};transfer_input(&receiver,cancel,2);assert(receiver.state==-2);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    vectors();
    for(int protocol=1;protocol<=3;protocol++) {
        run(protocol,0,0);run(protocol,1,0);run(protocol,128,0);run(protocol,1024,0);
        for(int damage=0;damage<=2;damage++)run(protocol,11011,damage);
    }
    puts("transfer: PASS (all byte values, exact lengths, empty files, fragmentation, CRC/checksum recovery, lost ACK, cancellation, timeout)");
}
