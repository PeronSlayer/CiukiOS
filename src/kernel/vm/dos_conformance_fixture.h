/* Disposable logical removable media, only for the selected D48 controller.
 * No physical device, cache policy or guest file service is replaced.
 * FreeDOS 2043 hdr/device.h N_RETRY=5; kernel/dsk.c LBA_Transfer.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_CONFORMANCE_FIXTURE_H
#define CIUKI_DOS_CONFORMANCE_FIXTURE_H
#include "dos_file.h"
#define D48_VOLUME_SECTORS 5040u
#define D48_PARTITION_START 63u
#define D48_E_START (D48_PARTITION_START+D48_VOLUME_SECTORS)
#define D48_DISK_SECTORS (D48_E_START+D48_VOLUME_SECTORS)
#define D48_DATA_SECTOR 43u /* reserved=1, FATs=2*20, root=2 */
#define D48_BIOS_RETRIES 5u
struct d48_media {
    unsigned mode, failures, remaining, opens;
    uint32_t generation;
    bool pending, armed, bad;
    struct dos_file_frame open;
};
static inline void d48_reset(struct d48_media *m,unsigned mode,uint32_t generation)
{ memset(m,0,sizeof(*m)); m->mode=mode; m->generation=generation; }
static inline void d48_word(uint8_t *p,unsigned value)
{ p[0]=value; p[1]=value>>8; }
static inline void d48_long(uint8_t *p,uint32_t value)
{ d48_word(p,value); d48_word(p+2,value>>16); }
static inline void d48_partition(uint8_t *p,unsigned start)
{
    /* Matching CHS and LBA identities avoid FreeDOS's suspect-CHS path. */
    unsigned c=start/1008,end=start+D48_VOLUME_SECTORS-1,ec=end/1008;
    p[1]=(start/63)%16;p[2]=(start%63+1)|((c>>2)&0xc0);p[3]=c;p[4]=0x0e;
    p[5]=(end/63)%16;p[6]=(end%63+1)|((ec>>2)&0xc0);p[7]=ec;
    d48_long(p+8,start); d48_long(p+12,D48_VOLUME_SECTORS);
}
static inline int d48_sector(struct d48_media *m,uint32_t lba,void *buffer,bool bios)
{
    uint8_t *p=buffer;
    if (!m->mode || lba>=D48_DISK_SECTORS) return -FS_EINVAL;
    /* Logical providers bypass the shared block cache. Only the requested
     * data completion fails: metadata/open/read-ahead cannot consume it. */
    if (m->armed && lba>=D48_E_START+D48_DATA_SECTOR && lba<D48_E_START+D48_DATA_SECTOR+8 &&
        (m->mode==2 || m->remaining)) {
        m->failures++;
        if (m->mode==1 && --m->remaining==0) m->armed=false;
        return -FS_EIO;
    }
    (void)bios;
    memset(p,0,512);
    if (!lba) {
        d48_partition(p+446,D48_PARTITION_START); d48_partition(p+462,D48_E_START);
        p[510]=0x55; p[511]=0xaa; return 0;
    }
    unsigned start=lba>=D48_E_START ? D48_E_START : D48_PARTITION_START;
    if (lba<start) return 0;
    lba-=start;
    if (!lba) {
        p[0]=0xeb;p[1]=0x3c;p[2]=0x90;memcpy(p+3,"CIUKID48",8);
        d48_word(p+11,512);p[13]=1;d48_word(p+14,1);p[16]=2;
        d48_word(p+17,32);d48_word(p+19,D48_VOLUME_SECTORS);p[21]=0xf8;
        d48_word(p+22,20);d48_word(p+24,63);d48_word(p+26,16);
        d48_long(p+28,start);p[36]=0x81;p[38]=0x29;d48_long(p+39,m->generation);
        memcpy(p+43,"D48REMOVABL",11);memcpy(p+54,"FAT16   ",8);p[510]=0x55;p[511]=0xaa;
    } else if (lba==1 || lba==21) {
        d48_word(p,0xfff8);d48_word(p+2,0xffff);
        for (unsigned c=2;c<10;c++) d48_word(p+c*2,c==9 ? 0xffff : c+1);
    } else if (lba==41 && start==D48_E_START) {
        memcpy(p,"D48FAULTDAT",11);p[11]=0x20;
        d48_word(p+26,2);d48_long(p+28,4096);
    } else if (lba>=D48_DATA_SECTOR && lba<D48_DATA_SECTOR+8) memset(p,'F',512);
    return 0;
}
static inline void d48_open_enter(struct d48_media *m,const struct dos_file_frame *f,bool matched)
{
    if (m->mode && matched && !m->opens && !m->pending) { m->open=*f; m->pending=true; }
}
static inline void d48_open_return(struct d48_media *m,const struct dos_file_frame *f,bool bios)
{
    if (!m->pending || f->cs!=m->open.cs || f->ip!=m->open.ip || f->ss!=m->open.ss || f->sp!=m->open.sp) return;
    m->pending=false;
    if (f->flags&1) { m->bad=true; return; }
    m->opens++;m->armed=true;m->remaining=bios ? D48_BIOS_RETRIES : 1;
}
#endif
