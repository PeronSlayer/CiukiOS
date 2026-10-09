/* Independent raw-image ownership oracle: no production FAT/cache helpers.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "scan.h"
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
static uint16_t u16(const uint8_t *p) { return p[0]+256u*p[1]; }
static uint32_t u32(const uint8_t *p) { return u16(p)+65536u*u16(p+2); }
struct disk {
    const uint8_t *bytes,*fat; unsigned type,spc,clusters,data,root,roots,fsz,res;
    uint32_t *owner,*queue; unsigned used,head,id; struct scan_result result;
};
static uint32_t next(struct disk *d, uint32_t c) {
    const uint8_t *p=d->fat+(d->type==12 ? c+c/2 : c*(d->type/8));
    if (d->type==12) return (u16(p)>>(c&1 ? 4 : 0))&4095;
    return d->type==16 ? u16(p) : u32(p)&0xfffffff;
}
static int end(struct disk *d, uint32_t c) { return c>=(d->type==12 ? 4088u : d->type==16 ? 65528u : 0xffffff8u); }
static int valid(struct disk *d, uint32_t c) { return c>=2 && c<d->clusters+2; }
static void entry(struct disk *d, const uint8_t *p) {
    if (!p[0] || p[0]==0xe5 || p[11]==15 || (p[11]&8)) return;
    if (p[0]=='.' && (p[1]==' ' || (p[1]=='.' && p[2]==' '))) return;
    uint32_t first=u16(p+26)+(d->type==32 ? (uint32_t)u16(p+20)*65536 : 0), c=first, size=u32(p+28), len=0;
    unsigned id=++d->id; d->result.files++;
    if (!c) { if (size || (p[11]&16)) d->result.corrupt++; return; }
    for (;;) {
        if (!valid(d,c) || len++>=d->clusters) { d->result.corrupt++; return; }
        if (d->owner[c]) { d->result.crosslinks++; return; }
        d->owner[c]=id; c=next(d,c); if (end(d,c)) break;
    }
    if ((uint64_t)len*d->spc*512<size) d->result.corrupt++;
    if (p[11]&16) { if (d->used>=d->clusters) abort(); d->queue[d->used++]=first; }
}
struct scan_result independent_scan(int fd, unsigned copy) {
    struct stat st; if (fstat(fd,&st)) abort();
    uint8_t *p=mmap(0,(size_t)st.st_size,PROT_READ,MAP_PRIVATE,fd,0); if (p==MAP_FAILED) abort();
    struct disk d={0}; d.bytes=p; d.spc=p[13]; d.res=u16(p+14); d.fsz=u16(p+22) ? u16(p+22) : u32(p+36); d.roots=u16(p+17);
    d.root=d.res+p[16]*d.fsz; d.data=d.root+(d.roots*32+511)/512;
    unsigned total=u16(p+19) ? u16(p+19) : u32(p+32); d.clusters=(total-d.data)/d.spc;
    d.type=d.clusters<4085 ? 12 : d.clusters<65525 ? 16 : 32;
    d.fat=p+(d.res+copy*d.fsz)*512;
    if (p[16]==2 && memcmp(p+d.res*512,p+(d.res+d.fsz)*512,(size_t)d.fsz*512)) d.result.divergent=1;
    if (d.type!=12 && !(next(&d,1)&(d.type==16 ? 0x8000 : 0x8000000))) d.result.dirty=1;
    d.owner=calloc(d.clusters+2,sizeof(*d.owner)); d.queue=calloc(d.clusters+1,sizeof(*d.queue)); if (!d.owner || !d.queue) abort();
    if (d.type==32) {
        uint8_t ent[32]={0}; ent[0]='R'; ent[11]=16; unsigned r=u32(p+44); ent[26]=(uint8_t)r; ent[27]=(uint8_t)(r>>8); ent[20]=(uint8_t)(r>>16); ent[21]=(uint8_t)(r>>24); entry(&d,ent);
    } else for (unsigned i=0;i<d.roots;i++) { const uint8_t *ent=p+d.root*512+i*32; if (!ent[0]) break; entry(&d,ent); }
    while (d.head<d.used) {
        uint32_t c=d.queue[d.head++]; int stop=0;
        for (unsigned guard=0;guard<d.clusters && !stop;guard++) {
            if (!valid(&d,c)) { d.result.corrupt++; break; }
            const uint8_t *b=p+(size_t)(d.data+(c-2)*d.spc)*512;
            for (unsigned i=0;i<d.spc*16;i++) { if (!b[i*32]) { stop=1; break; } entry(&d,b+i*32); }
            c=next(&d,c); if (end(&d,c)) break;
        }
    }
    for (unsigned c=2;c<d.clusters+2;c++) if (next(&d,c) && next(&d,c)!=(d.type==12 ? 4087u : d.type==16 ? 65527u : 0xffffff7u) && !d.owner[c]) d.result.lost++;
    free(d.owner); free(d.queue); munmap(p,(size_t)st.st_size); return d.result;
}
