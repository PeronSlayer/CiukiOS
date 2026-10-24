/* Included by disk.c to share Setup's backend, probe and lease session.
 * SPDX-License-Identifier: MIT */
#include "partition_tool.h"
static int pt_error(struct pt_table *t,int err,const char *what)
{ return fail(DISK_EINTERNAL,err,t ? t->disk : 0,0,0,what); }
static uint8_t *pt_slot(struct pt_table *t,unsigned sector,unsigned slot)
{ return t->sector[sector]+446+16*slot; }
static bool pt_extent(uint64_t start,uint64_t count,uint64_t low,uint64_t high)
{ return count && start>=low && start<high && count<=high-start; }
static int pt_extended(struct pt_table *t)
{ for(unsigned i=0;i<4;i++)if(extended_type(pt_slot(t,0,i)[4]))return (int)i; return -1; }
static int pt_validate(struct pt_table *t)
{
    uint64_t limit=t->id.sectors<DISK_LBA28 ? t->id.sectors : DISK_LBA28;
    unsigned ext=0;
    for(unsigned i=0;i<4;i++) {
        uint8_t *p=pt_slot(t,0,i); if(!p[4])continue;
        uint64_t start=disk_rd32(p+8),count=disk_rd32(p+12);
        if((p[0] && p[0]!=0x80) || !pt_extent(start,count,1,limit))return pt_error(t,EINVAL,"invalid primary extent");
        if(extended_type(p[4]) && ++ext>1)return pt_error(t,EINVAL,"multiple extended partitions");
        for(unsigned j=0;j<i;j++) { uint8_t *q=pt_slot(t,0,j); if(!q[4])continue;
            uint64_t a=disk_rd32(q+8),n=disk_rd32(q+12);
            if(start<a+n && a<start+count)return pt_error(t,EINVAL,"overlapping primary partitions"); }
    }
    int slot=pt_extended(t);
    if(t->count>1 && slot<0)return pt_error(t,EINVAL,"orphan EBR");
    uint64_t base=slot<0 ? 0 : disk_rd32(pt_slot(t,0,(unsigned)slot)+8);
    uint64_t end=slot<0 ? 0 : base+disk_rd32(pt_slot(t,0,(unsigned)slot)+12);
    for(unsigned i=1;i<t->count;i++) {
        if(!pt_extent(t->lba[i],1,base,end) || (i==1 && t->lba[i]!=base))return pt_error(t,EINVAL,"invalid EBR location");
        uint8_t *p=pt_slot(t,i,0),*link=pt_slot(t,i,1);
        if(pt_slot(t,i,2)[4] || pt_slot(t,i,3)[4] || extended_type(p[4]) || p[0])return pt_error(t,EINVAL,"nonstandard EBR");
        uint64_t start=t->lba[i]+(uint64_t)disk_rd32(p+8),count=disk_rd32(p+12);
        if(p[4] && (!disk_rd32(p+8) || !pt_extent(start,count,base,end)))return pt_error(t,EINVAL,"invalid logical extent");
        if(link[4] && (!extended_type(link[4]) || i+1>=t->count || base+disk_rd32(link+8)!=t->lba[i+1] ||
            !pt_extent(base+disk_rd32(link+8),disk_rd32(link+12),base,end)))return pt_error(t,EINVAL,"invalid EBR link");
        if(!link[4] && i+1<t->count)return pt_error(t,EINVAL,"broken EBR chain");
        for(unsigned j=1;j<t->count;j++) {
            if(p[4] && t->lba[j]>=start && t->lba[j]<start+count)return pt_error(t,EINVAL,"EBR overlaps logical data");
            if(j<i && p[4]) { uint8_t *q=pt_slot(t,j,0); if(!q[4])continue;
                uint64_t a=t->lba[j]+(uint64_t)disk_rd32(q+8),n=disk_rd32(q+12);
                if(start<a+n && a<start+count)return pt_error(t,EINVAL,"overlapping logical drives"); }
        }
    }
    return 0;
}
int pt_read(unsigned disk,struct pt_table *t)
{
    if(!t)return DISK_EINTERNAL;
    memset(t,0,sizeof(*t)); t->disk=disk;
    int e=disk_probe(disk,&scratch); if(e)return e;
    if(!scratch.present || scratch.gpt || scratch.overlay || scratch.table_error || scratch.id.sector_size!=512)
        return pt_error(t,EINVAL,"uneditable partition table");
    t->id=scratch.id; t->count=1;
    const struct disk_backend *b=backend(); int h=b->open(b->ctx,disk,false); if(h<0)return pt_error(t,h,"open");
    e=raw_read(h,disk,0,1,t->sector[0]);
    if(!e && !scratch.table_present) {
        /* Blank table only: never reinterpret a FAT superfloppy as an MBR. */
        bool empty=true; for(unsigned i=0;i<512;i++)if(t->sector[0][i])empty=false;
        if(!empty)e=pt_error(t,EINVAL,"nonblank disk without MBR");
        else { t->sector[0][510]=0x55; t->sector[0][511]=0xaa; }
    }
    unsigned containers=0;
    for(unsigned slot=0;!e && slot<4;slot++) {
        uint8_t *p=pt_slot(t,0,slot); if(!extended_type(p[4]))continue;
        if(++containers>1) { e=pt_error(t,EINVAL,"multiple extended partitions"); break; }
        uint64_t base=disk_rd32(p+8),end=base+disk_rd32(p+12),cur=base;
        for(;;) {
            if(t->count==PT_SECTORS || !pt_extent(cur,1,base,end)) { e=pt_error(t,EINVAL,"EBR limit"); break; }
            bool seen=false; for(unsigned i=1;i<t->count;i++)if(t->lba[i]==cur)seen=true;
            if(seen) { e=pt_error(t,EINVAL,"EBR loop"); break; }
            unsigned i=t->count++; t->lba[i]=(uint32_t)cur;
            e=raw_read(h,disk,cur,1,t->sector[i]); if(e)break;
            if(disk_rd16(t->sector[i]+510)!=0xaa55) { e=pt_error(t,EINVAL,"invalid EBR signature"); break; }
            uint8_t *link=pt_slot(t,i,1); if(!link[4])break;
            cur=base+disk_rd32(link+8);
        }
    }
    b->close(b->ctx,h);
    memcpy(t->original,t->sector,sizeof(t->original));
    /* The initial blank MBR is not yet signed on disk. */
    if(!scratch.table_present)memset(t->original[0],0,512);
    return e ? e : pt_validate(t);
}
static void pt_entry(uint8_t *p,uint8_t type,uint32_t start,uint32_t count)
{
    memset(p,0,16); p[1]=p[5]=0xfe; p[2]=p[3]=p[6]=p[7]=0xff;
    p[4]=type; disk_wr32(p+8,start); disk_wr32(p+12,count);
}
int pt_create(struct pt_table *t,enum pt_kind kind,uint32_t start,uint32_t sectors)
{
    if(!t || kind>PT_LOGICAL || !sectors || start<2048)return pt_error(t,EINVAL,"invalid creation request");
    uint64_t limit=t->id.sectors<DISK_LBA28 ? t->id.sectors : DISK_LBA28;
    if(!pt_extent(start,sectors,2048,limit))return pt_error(t,EINVAL,"outside LBA28 disk");
    if(kind!=PT_EXTENDED && ((!t->large && sectors>4194304u) || sectors<32768u))return pt_error(t,EINVAL,"FAT16 size limit (16 MiB to 2 GiB)");
    uint8_t type=t->large && sectors>=1048576u ? 0x0c : 0x0e;
    int slot=pt_extended(t); unsigned target=0;
    if(kind==PT_LOGICAL) {
        if(slot<0 || t->count<2 || t->count==PT_SECTORS)return pt_error(t,EINVAL,"extended partition required");
        uint32_t base=disk_rd32(pt_slot(t,0,(unsigned)slot)+8),end=base+disk_rd32(pt_slot(t,0,(unsigned)slot)+12);
        uint32_t ebr=t->count==2 && !pt_slot(t,1,0)[4] ? base : start-1;
        if(start<=ebr || !pt_extent(ebr,(uint64_t)sectors+start-ebr,base,end))return pt_error(t,EINVAL,"logical outside container");
        /* One edit is atomic at one existing parent. New EBR is durable
         * before the previous tail links to it. */
        target=t->count==2 && !pt_slot(t,1,0)[4] ? 1 : t->count;
        uint8_t old[16]; memcpy(old,pt_slot(t,target,0),16);
        pt_entry(pt_slot(t,target,0),type,start-ebr,sectors);
        if(target==t->count) {
            memset(t->sector[target],0,512); pt_entry(pt_slot(t,target,0),type,start-ebr,sectors);
            t->sector[target][510]=0x55; t->sector[target][511]=0xaa; t->lba[target]=ebr; t->count++;
            pt_entry(pt_slot(t,target-1,1),0x0f,ebr-base,end-ebr);
        }
        int e=pt_validate(t);
        if(e) { if(target==t->count-1 && target>1) { t->count--; memset(pt_slot(t,target-1,1),0,16); } else memcpy(pt_slot(t,target,0),old,16); }
        return e;
    }
    if(kind==PT_EXTENDED && slot>=0)return pt_error(t,EEXIST,"extended partition exists");
    unsigned primary=0; while(primary<4 && pt_slot(t,0,primary)[4])primary++;
    if(primary==4)return pt_error(t,ENOSPC,"four primary slots used");
    pt_entry(pt_slot(t,0,primary),kind==PT_EXTENDED ? 0x0f : type,start,sectors);
    int e=pt_validate(t); if(e) { memset(pt_slot(t,0,primary),0,16); return e; }
    if(kind==PT_EXTENDED) {
        t->lba[1]=start; t->count=2; memset(t->sector[1],0,512);
        t->sector[1][510]=0x55; t->sector[1][511]=0xaa;
    }
    return 0;
}
int pt_delete(struct pt_table *t,unsigned number)
{
    if(!t || !number || number>t->count+3)return pt_error(t,EINVAL,"partition number");
    if(number<=4) {
        uint8_t *p=pt_slot(t,0,number-1); if(!p[4])return pt_error(t,ENOENT,"empty slot");
        if(extended_type(p[4])) { for(unsigned i=1;i<t->count;i++)if(pt_slot(t,i,0)[4])return pt_error(t,EBUSY,"delete logical drives first"); t->count=1; }
        memset(p,0,16); return 0;
    }
    unsigned i=number-4; if(i>=t->count || !pt_slot(t,i,0)[4])return pt_error(t,ENOENT,"logical drive");
    if(i==1) { memset(pt_slot(t,1,0),0,16); return 0; } /* keep first EBR anchor */
    memcpy(pt_slot(t,i-1,1),pt_slot(t,i,1),16);
    for(unsigned j=i;j+1<t->count;j++) {
        t->lba[j]=t->lba[j+1]; memcpy(t->sector[j],t->sector[j+1],512); memcpy(t->original[j],t->original[j+1],512);
    }
    t->count--; return 0;
}
int pt_active(struct pt_table *t,unsigned number)
{
    if(!t || !number || number>4 || !pt_slot(t,0,number-1)[4] || extended_type(pt_slot(t,0,number-1)[4]))return pt_error(t,EINVAL,"active primary required");
    for(unsigned i=0;i<4;i++)pt_slot(t,0,i)[0]=i==number-1 ? 0x80 : 0;
    return 0;
}
int pt_commit(struct pt_table *t)
{
    int e=t ? pt_validate(t) : DISK_EINTERNAL; if(e)return e;
    if(session.open)return pt_error(t,EBUSY,"another libdisk session");
    const struct disk_backend *b=backend(); int h=b->open(b->ctx,t->disk,true); if(h<0)return pt_error(t,h,"open editor");
    session.open=true; session.disk=t->disk; session.handle=h;
    struct ciuki_disk_info info={.version=CIUKI_DISK_VERSION,.size=sizeof(info)};
    e=control(DISK_PARTITION_ACQUIRE,&info,sizeof(info));
    if(e) { session.open=false; b->close(b->ctx,h); return fail(DISK_ELEASE,e,t->disk,0,0,"partition lease"); }
    session.leased=true;
    struct pt_table *current=malloc(sizeof(*current));
    if(!current)e=pt_error(t,ENOMEM,"editor validation");
    if(!e)e=pt_read(t->disk,current);
    if(!e && (memcmp(&current->id,&t->id,sizeof(t->id))))e=fail(DISK_ECHANGED,0,t->disk,0,0,"disk/table changed");
    unsigned parent=PT_SECTORS,changed=0;
    for(unsigned i=0;!e && i<t->count;i++) {
        uint8_t check[512]; e=raw_read(h,t->disk,t->lba[i],1,check); if(e)break;
        bool existed=false;
        for(unsigned j=0;j<current->count;j++)if(t->lba[i]==current->lba[j]) {
            existed=true;
            if(memcmp(check,t->original[i],512))e=fail(DISK_ECHANGED,0,t->disk,t->lba[i],1,"table changed");
        }
        if(existed && memcmp(t->sector[i],t->original[i],512)) { parent=i; changed++; }
    }
    if(!e && changed>1)e=pt_error(t,EINVAL,"commit one operation at a time");
    for(unsigned i=0;!e && i<t->count;i++) {
        if(i==parent)continue;
        bool existed=false; for(unsigned j=0;j<current->count;j++)if(t->lba[i]==current->lba[j])existed=true;
        if(!existed) { e=raw_write(h,t->disk,t->lba[i],1,t->sector[i]); if(!e)e=flush_disk(t->disk); }
    }
    if(!e && parent<PT_SECTORS) { e=raw_write(h,t->disk,t->lba[parent],1,t->sector[parent]); if(!e)e=flush_disk(t->disk); }
    for(unsigned i=0;!e && i<t->count;i++) {
        uint8_t check[512]; e=raw_read(h,t->disk,t->lba[i],1,check);
        if(!e && memcmp(check,t->sector[i],512))e=fail(DISK_EIO,EIO,t->disk,t->lba[i],1,"table read-back");
    }
    free(current); struct disk_error_info saved=last;
    int released=disk_release(t->disk); if(e)last=saved; return e ? e : released;
}
