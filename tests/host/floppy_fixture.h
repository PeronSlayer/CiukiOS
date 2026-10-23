/* Independent 82077/8237 register fixture, not the virtual FDC engine.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef FLOPPY_FIXTURE_H
#define FLOPPY_FIXTURE_H
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/kernel/drivers/floppy.h"
static struct resource resources[64];
static unsigned resource_count, resource_serial;
int registry_claim(enum res_type t,uint32_t a,uint32_t b,const char *owner,bool share)
{
    for(unsigned i=0;i<resource_count;i++) if(resources[i].state!=RS_RELEASED && resources[i].type==t &&
        a<resources[i].end && b>resources[i].start) return -16;
    assert(resource_count<64);
    unsigned i=resource_count++;
    resources[i]=(struct resource){t,a,b,owner,++resource_serial,RS_CLAIMED,share}; return i;
}
const struct resource *registry_get(unsigned i) { return i<resource_count ? &resources[i] : 0; }
bool registry_valid(int i,gen_t g)
{ return i>=0 && (unsigned)i<resource_count && resources[i].generation==g && resources[i].state==RS_ACTIVE; }
int registry_activate(int i,gen_t g) { assert(resources[i].generation==g); resources[i].state=RS_ACTIVE; return 0; }
int registry_quiesce(int i,gen_t g,registry_idle_proof_fn proof)
{ assert(registry_valid(i,g));resources[i].state=RS_QUIESCING;assert(proof(i,g));return 0; }
int registry_release(int i,uint32_t g) { assert(resources[i].generation==g && resources[i].state==RS_QUIESCING); resources[i].state=RS_RELEASED; return 0; }
int registry_quarantine(int i,gen_t g) { assert(resources[i].generation==g); resources[i].state=RS_QUARANTINED; return 0; }
struct floppy_fixture {
    struct fdc fdc;
    uint8_t bounce[4096], *disk, *disk_b;
    struct mm_pin pin;
    uint64_t now;
    unsigned bytes, writes, resets, reads, data_writes, formats, commands, irq_enabled, dma_programs;
    uint8_t dor, rate, mode, mask, page, flip, command[16], needed, length;
    uint8_t reply[8], replies, position, sense_st0[4], sense_cyl[4], senses;
    uint8_t cylinder[2], media_type, st1, st2;
    uint16_t address,count;
    bool changed, suppress_irq, short_dma, stuck_mask, stuck_dor, fail_rqm, cancel, blank, absent;
    unsigned transient_errors;
};
static void fixture_irq(struct floppy_fixture *m) { if (m->irq_enabled && !m->suppress_irq) fdc_irq(&m->fdc); }
static void fixture_reply(struct floppy_fixture *m,unsigned n)
{ m->replies=n;m->position=0;m->length=m->needed=0; }
static void fixture_execute(struct floppy_fixture *m)
{
    uint8_t *c=m->command; unsigned op=c[0]&31,unit=c[1]&3;
    m->commands++; m->length=0;
    switch(op) {
    case 3: case 0x12: m->needed=0; return;
    case 8:
        assert(m->senses); m->reply[0]=m->sense_st0[0];m->reply[1]=m->sense_cyl[0];
        m->senses--; memmove(m->sense_st0,m->sense_st0+1,m->senses);memmove(m->sense_cyl,m->sense_cyl+1,m->senses);
        fixture_reply(m,2);return;
    case 7: case 15:
        assert(unit<2);m->cylinder[unit]=op==7 ? 0 : c[2];
        if(op==15 && !m->absent)m->changed=false;
        m->sense_st0[0]=0x20|unit;m->sense_cyl[0]=m->cylinder[unit];m->senses=1;
        m->needed=0;fixture_irq(m);return;
    default: break;
    }
    struct fdc_geometry g;assert(fdc_geometry(unit ? 3 : m->media_type,&g));
    assert((m->dor&3)==unit && (m->dor&(0x10u<<unit)));
    uint8_t *disk=unit ? m->disk_b : m->disk;
    memset(m->reply,0,7);m->reply[0]=c[1]&7;m->reply[3]=m->cylinder[unit];m->reply[5]=1;m->reply[6]=2;
    if(op==10) {
        if(m->rate!=g.rate || m->blank){m->reply[0]|=0x40;m->reply[1]=1;}
    } else {
        assert(!(m->mask&4));assert((m->mode&3)==2);assert(m->page==0x20 && m->address==0);
        assert(m->rate==g.rate);assert(m->count+1==(op==13 ? g.sectors*4u : 512u));
        bool write=op==5 || op==13; assert(m->mode==(write ? 0x4a : 0x46));
        uint8_t st1=m->st1,st2=m->st2;
        if(m->blank && op!=13 && !st1)st1=1;
        if(m->transient_errors){st1=0x20;m->transient_errors--;}
        if(st1 || st2){m->reply[0]|=0x40;m->reply[1]=st1;m->reply[2]=st2;}
        else if(op==13) {
            assert(c[2]==2 && c[3]==g.sectors);
            for(unsigned s=0;s<g.sectors;s++) {
                assert(m->bounce[s*4]==m->cylinder[unit] && m->bounce[s*4+1]==((c[1]>>2)&1) &&
                    m->bounce[s*4+2]==s+1 && m->bounce[s*4+3]==2);
                unsigned lba=(m->cylinder[unit]*g.heads+((c[1]>>2)&1))*g.sectors+s;
                memset(disk+lba*512,c[5],512);
            }
            m->formats++;m->blank=false;
        } else {
            assert(c[2]==m->cylinder[unit] && c[3]<g.heads && c[4]>=1 && c[4]<=g.sectors && c[5]==2 && c[6]==c[4]);
            unsigned lba=(c[2]*g.heads+c[3])*g.sectors+c[4]-1;
            if(write){memcpy(disk+lba*512,m->bounce,512);m->data_writes++;}
            else {memcpy(m->bounce,disk+lba*512,512);m->reads++;}
        }
        if(!m->short_dma && !st1 && !st2)m->count=0xffff;
    }
    fixture_reply(m,7);fixture_irq(m);
}
static uint8_t fixture_read(void *arg,uint16_t p)
{
    struct floppy_fixture *m=arg;
    switch(p) {
    case 0x3f2:return m->stuck_dor ? 0xc : m->dor;
    case 0x3f4:return m->fail_rqm ? 0 : m->replies ? 0xd0 : m->length ? 0x90 : 0x80;
    case 0x3f5:{assert(m->replies);uint8_t b=m->reply[m->position++];if(m->position==m->replies)m->replies=m->position=0;return b;}
    case 0x3f7:return m->changed ? 0x80 : 0;
    case 0x0f:return m->stuck_mask ? 0 : m->mask;
    case 5:{uint8_t b=m->flip ? m->count>>8 : m->count;m->flip^=1;return b;}
    default:assert(!"unexpected FDC read");return 0xff;
    }
}
static void fixture_write(void *arg,uint16_t p,uint8_t b)
{
    struct floppy_fixture *m=arg;m->writes++;
    for(unsigned i=0;i<5;i++)assert(registry_valid(m->fdc.claims[i],m->fdc.generations[i]));
    switch(p) {
    case 0x3f2:{uint8_t old=m->dor;m->dor=b;if(!(b&4)){m->length=m->needed=m->replies=m->position=0;}
        if(!(old&4) && (b&4)){m->resets++;m->senses=4;for(unsigned i=0;i<4;i++){m->sense_st0[i]=0xc0|i;m->sense_cyl[i]=0;}fixture_irq(m);}break;}
    case 0x3f7:m->rate=b;break;
    case 0x3f5:
        if(!m->length){switch(b&31){case 3:case 15:m->needed=3;break;case 7:case 10:case 0x12:m->needed=2;break;case 5:case 6:m->needed=9;break;case 13:m->needed=6;break;default:m->needed=1;}}
        assert(m->length<16);m->command[m->length++]=b;if(m->length==m->needed)fixture_execute(m);break;
    case 0x0a:if(b&4)m->mask|=1u<<(b&3);else m->mask&=~(1u<<(b&3));break;
    case 0x0c:m->flip=0;break;
    case 0x0b:m->mode=b;m->dma_programs++;break;
    case 0x81:m->page=b;break;
    case 4:m->address=m->flip ? (m->address&255)|(b<<8) : (m->address&0xff00)|b;m->flip^=1;break;
    case 5:m->count=m->flip ? (m->count&255)|(b<<8) : (m->count&0xff00)|b;m->flip^=1;break;
    default:assert(!"unexpected FDC write (including ATA 3f6)");
    }
}
static uint64_t fixture_now(void *arg){return ((struct floppy_fixture *)arg)->now;}
static void fixture_pause(void *arg,unsigned ms){((struct floppy_fixture *)arg)->now+=ms;}
static uint32_t fixture_exclude(void *arg){(void)arg;return 0;}
static void fixture_restore(void *arg,uint32_t f){(void)arg;(void)f;}
static void fixture_enable(void *arg,bool on){((struct floppy_fixture *)arg)->irq_enabled=on;}
static bool fixture_cancelled(void *arg){return ((struct floppy_fixture *)arg)->cancel;}
static int fixture_open(struct floppy_fixture *m,unsigned type)
{
    memset(m,0,sizeof(*m));resource_count=0;
    struct fdc_geometry g;assert(fdc_geometry(type,&g));
    m->bytes=g.cylinders*g.heads*g.sectors*512;m->disk=calloc(1,m->bytes);assert(m->disk);m->media_type=type;m->mask=15;
    m->disk_b=calloc(1,720u*1024);assert(m->disk_b);
    m->pin=(struct mm_pin){.generation=1,.first_pfn=0x200,.pages=1,.ticket=1,
        .address_mask=0xffffff,.alignment=4096,.boundary=65536,.direction=MM_PIN_BIDIRECTIONAL};
    struct fdc_evidence ev;assert(!fdc_discover(&ev,0x41,(type<<4)|3));
    struct fdc_io io={fixture_read,fixture_write,fixture_now,fixture_pause,fixture_exclude,fixture_restore,fixture_enable,fixture_cancelled,m};
    return fdc_attach(&m->fdc,&io,&ev,m->bounce,0x200000,&m->pin);
}
static void fixture_close(struct floppy_fixture *m){assert(!fdc_detach(&m->fdc));free(m->disk);free(m->disk_b);m->disk=m->disk_b=0;}
#endif
