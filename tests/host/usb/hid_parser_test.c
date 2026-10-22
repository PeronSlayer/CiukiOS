/* Pure parser/decoder corpus, transactional failure and deterministic fuzz.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/hid.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hid_corpus.h"
#include "mouse_descriptor.h"
static struct hid_layout layout;
static void parse(const uint8_t *b, unsigned n)
{ assert(hid_parse(&layout,b,n)); }
static void refused(const uint8_t *b, unsigned n)
{
    struct hid_layout output, before;
    memset(&output,0x5a,sizeof(output)); before=output;
    assert(!hid_parse(&output,b,n)); assert(!memcmp(&output,&before,sizeof(output)));
}
static void corpus(void)
{
    struct hid_values v;
    parse(hid_boot_keyboard,sizeof(hid_boot_keyboard));
    uint8_t key[]={0x42,0xff,4,5,6,7,8,9};
    assert(hid_decode(&layout,key,sizeof(key),&v)==1);
    assert(v.keyboard[0]==0xf0 && v.keyboard[1]==3 && v.keyboard[0xe0/8]==0x42);
    for (unsigned i=1;i<=3;i++) {
        struct hid_values before=v; key[2]=i;
        assert(hid_decode(&layout,key,sizeof(key),&v)==2 && !memcmp(&v,&before,sizeof(v)));
    }
    assert(layout.report[0].bits[HID_INPUT]==64 && layout.report[0].bits[HID_OUTPUT]==8);
    parse(hid_nkro_keyboard,sizeof(hid_nkro_keyboard));
    uint8_t nkro[16]={3,0x81,0xff,0xff};
    assert(hid_decode(&layout,nkro,sizeof(nkro),&v)==1 && v.keyboard[0]==0xf0 && v.keyboard[1]==0xff && v.keyboard[2]==0x0f);
    assert(v.keyboard[28]==0x81 && layout.report[0].bits[HID_OUTPUT]==8);
    parse(hid_consumer,sizeof(hid_consumer));
    uint8_t consumer[]={4,63}; assert(hid_decode(&layout,consumer,2,&v)==1 && v.consumers==63);
    parse(hid_consumer_array,sizeof(hid_consumer_array));
    uint8_t selector[]={0xcd,0}; assert(hid_decode(&layout,selector,2,&v)==1 && v.consumers==8);
    selector[0]=0xff; selector[1]=3; assert(hid_decode(&layout,selector,2,&v)==1 && !v.consumers);
    parse(hid_joystick,sizeof(hid_joystick));
    uint8_t joy[]={0,255,9}; assert(hid_decode(&layout,joy,3,&v)==1 && v.axes==3 && v.axis[0]==0 && v.axis[1]==255 && v.buttons==9);
    assert(layout.field[0].physical_minimum==-100 && layout.field[0].physical_maximum==100);
    parse(hid_gamepad,sizeof(hid_gamepad));
    uint8_t pad[]={5,1,0x80,0xff,0x7f,0xf3,0xff};
    assert(hid_decode(&layout,pad,7,&v)==1 && v.axis[0]==-32767 && v.axis[1]==32767 && v.hat[0]==3 && v.buttons==4095);
    pad[5]=0xf8; assert(hid_decode(&layout,pad,7,&v)==1 && v.hat[0]==-1);
    /* 4-way hat and physical metadata are independent from axis state. */
    uint8_t four[sizeof(hid_gamepad)]; memcpy(four,hid_gamepad,sizeof(four));
    for (unsigned i=0;i+1<sizeof(four);i++) if (four[i]==0x25 && four[i+1]==7) four[i+1]=3;
    parse(four,sizeof(four)); pad[5]=0xf3;
    assert(hid_decode(&layout,pad,7,&v)==1 && v.hat[0]==6);
    /* Two top-level applications share a pipe, with independent IDs. */
    uint8_t composite[200];
    memcpy(composite,hid_boot_keyboard,6); composite[6]=0x85; composite[7]=1;
    memcpy(composite+8,hid_boot_keyboard+6,sizeof(hid_boot_keyboard)-6);
    unsigned at=sizeof(hid_boot_keyboard)+2;
    memcpy(composite+at,qemu_mouse_descriptor,6); at+=6; composite[at++]=0x85; composite[at++]=2;
    memcpy(composite+at,qemu_mouse_descriptor+6,sizeof(qemu_mouse_descriptor)-6); at+=sizeof(qemu_mouse_descriptor)-6;
    parse(composite,at); assert(layout.reports==2 && (layout.classes&3)==3);
    uint8_t kb[]={1,0,0,4,0,0,0,0,0}; assert(hid_decode(&layout,kb,9,&v)==1 && v.keyboard[0]==16);
    uint8_t mouse[]={2,1,1,0xff,1}; assert(hid_decode(&layout,mouse,5,&v)==1 && v.classes==CIUKI_HID_MOUSE);
    struct hid_values before=v;
    mouse[0]=99; assert(!hid_decode(&layout,mouse,5,&v) && !memcmp(&before,&v,sizeof(v)));
    mouse[0]=1; assert(hid_decode(&layout,mouse,5,&v)<0 && !memcmp(&before,&v,sizeof(v)));
    /* Array selector indices need not equal usages; explicit lists can be sparse. */
    uint8_t sparse[]={5,1,9,6,0xa1,1,5,7,0x15,1,0x25,2,9,4,9,7,0x75,8,0x95,1,0x81,0,0xc0};
    parse(sparse,sizeof(sparse)); uint8_t selected[]={2};
    assert(hid_decode(&layout,selected,1,&v)==1 && v.keyboard[0]==0x80);
    selected[0]=3; before=v;
    assert(hid_decode(&layout,selected,1,&v)<0 && !memcmp(&before,&v,sizeof(v)));
    puts("PASS HID corpus: boot/NKRO/consumer/composite/joystick/gamepad, IDs, rollover, physical limits, hats");
}
static uint32_t random_state=0x104;
static uint32_t rnd(void)
{ random_state^=random_state<<13; random_state^=random_state>>17; random_state^=random_state<<5; return random_state; }
static void malformed(void)
{
    const uint8_t *corpus[]={hid_boot_keyboard,hid_nkro_keyboard,hid_consumer,hid_consumer_array,hid_joystick,hid_gamepad};
    const unsigned lengths[]={sizeof(hid_boot_keyboard),sizeof(hid_nkro_keyboard),sizeof(hid_consumer),sizeof(hid_consumer_array),sizeof(hid_joystick),sizeof(hid_gamepad)};
    for (unsigned c=0;c<6;c++) for (unsigned n=0;n<lengths[c];n++) refused(corpus[c],n);
    uint8_t pop[]={0xb4}, end[]={0xc0}, long_item[]={0xfe,1,0,0}, id0[]={0x85,0};
    refused(pop,sizeof(pop)); refused(end,sizeof(end)); refused(long_item,sizeof(long_item)); refused(id0,sizeof(id0));
    uint8_t pushes[]={0xa4,0xa4,0xa4,0xa4,0xa4}; refused(pushes,sizeof(pushes));
    uint8_t delimiter[]={0xa9,1}; refused(delimiter,sizeof(delimiter));
    uint8_t count_overflow[]={5,1,9,6,0xa1,1,0x75,32,0x97,255,255,255,255,0x81,1,0xc0};
    refused(count_overflow,sizeof(count_overflow));
    uint8_t fields[256]={5,1,9,6,0xa1,1,5,7,0x15,0,0x25,1,0x75,1,0x95,1};
    unsigned used=16;
    for (unsigned i=0;i<33;i++) { fields[used++]=9; fields[used++]=4; fields[used++]=0x81; fields[used++]=2; }
    fields[used++]=0xc0; refused(fields,used);
    uint8_t bad[512];
    for (unsigned c=0;c<6;c++) for (unsigned round=0;round<4000;round++) {
        unsigned n=lengths[c]; memcpy(bad,corpus[c],n); bad[rnd()%n]^=(uint8_t)(1u<<(rnd()%8));
        struct hid_layout out, before; memset(&out,0xa5,sizeof(out)); before=out;
        if (!hid_parse(&out,bad,n)) assert(!memcmp(&out,&before,sizeof(out)));
        else {
            uint8_t data[HID_PACKET_MAX]; for (unsigned i=0;i<sizeof(data);i++) data[i]=rnd();
            struct hid_values v, old; memset(&v,0x3c,sizeof(v)); old=v;
            int result=hid_decode(&out,data,rnd()%(HID_PACKET_MAX+1),&v);
            if (result!=1) assert(!memcmp(&v,&old,sizeof(v)));
        }
    }
    for (unsigned round=0;round<8000;round++) {
        unsigned n=rnd()%513; for (unsigned i=0;i<n;i++) bad[i]=rnd();
        struct hid_layout out, before; memset(&out,0xa5,sizeof(out)); before=out;
        if (!hid_parse(&out,bad,n)) assert(!memcmp(&out,&before,sizeof(out)));
    }
    printf("PASS HID transactional malformed/truncation corpus and 32000 deterministic fuzz cases; layout_bytes=%zu runtime_bytes=%zu\n",sizeof(struct hid_layout),sizeof(struct usb_hid_reports));
}
int main(void) { setvbuf(stdout,NULL,_IOLBF,0); corpus(); malformed(); return 0; }
