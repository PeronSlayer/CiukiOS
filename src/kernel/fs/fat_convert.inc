/* G29b: offline FAT16 -> FAT32, redo journal in old free clusters.
 * Included by fat.c for its validated FAT/directory readers.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CV_MAGIC "CIUKICVT"
#define CV_MAX_PREFIX 1024u
#define CV_TAIL_PAGES 16u
struct cv_plan {
    struct fat_volume *v;
    struct blkdev *parent;
    uint64_t start;
    uint32_t spc, fat, data, clusters, reserved, prefix;
    uint32_t root_old, root_sectors, journal, journal_sectors, records, written;
    uint32_t directory_sectors, free_clusters, table_index, partition_sectors, table_lba;
    uint32_t *map;
    uint8_t *tails[CV_TAIL_PAGES];
};
_Static_assert(sizeof(struct cv_plan)<=4096,"converter plan page");
static uint32_t cv_crc(const void *ptr, unsigned size)
{
    const uint8_t *p=ptr; uint32_t crc=~0u;
    for (unsigned i=0;i<size;i++) {
        crc^=p[i];
        for (unsigned j=0;j<8;j++) crc=(crc>>1)^(0xedb88320u& (0u-(crc&1)));
    }
    return ~crc;
}
static int cv_read(struct blkdev *d, uint64_t lba, void *b)
{ return blkdev_request(d,BLK_READ,lba,1,b,BLK_BACKGROUND,0); }
static int cv_write(struct blkdev *d, uint64_t lba, const void *b)
{ return blkdev_request(d,BLK_WRITE,lba,1,(void *)b,BLK_BACKGROUND,0); }
static int cv_flush(struct blkdev *d)
{
    if (!blkdev_durable(d)) return -FS_EROFS;
    return d->write_cache_state==BLKDEV_CACHE_DISABLED ? 0 :
        blkdev_request(d,BLK_FLUSH,0,0,0,BLK_BACKGROUND,0);
}
int fat_convert_plan(struct fat_volume *v, uint32_t *bytes)
{
    if (!v || !bytes || !v->mounted || v->type!=16 || v->fats!=2) return -FS_EINVAL;
    if (v->sectors<=66600 || v->sectors>4194304) return -FS_EINVAL;
    unsigned spc=v->sectors<=532480 ? 1 : 8;
    if (v->spc<spc || v->spc%spc) return -FS_EOPNOTSUPP;
    uint32_t fat=(uint32_t)((v->sectors-32+128*spc)/(128*spc+2));
    uint32_t data=32+2*fat;
    if (data<v->data_sector) data=v->data_sector;
    uint32_t prefix=(data-v->data_sector+v->spc-1)/v->spc;
    data=v->data_sector+prefix*v->spc;
    if (data-2*fat>65535 || prefix>CV_MAX_PREFIX || data>=v->sectors ||
        (v->sectors-data)/spc<65525) return -FS_EINVAL;
    *bytes=spc*512; return 0;
}
static uint32_t cv_sector(struct cv_plan *p, uint32_t old)
{ return p->v->data_sector+(old-2)*p->v->spc; }
static uint32_t cv_destination(struct cv_plan *p, uint32_t old)
{ return old>=2 && old<2+p->prefix ? p->map[old-2] : old; }
static uint32_t cv_cluster(struct cv_plan *p, uint32_t old)
{ return (cv_sector(p,cv_destination(p,old))-p->data)/p->spc+2; }
static int cv_append(struct cv_plan *p, uint32_t target, const uint8_t b[512])
{
    if (p->written>=p->records ||
        (target!=UINT32_MAX && (target>=p->v->sectors ||
         (target>=p->journal && target<p->journal+p->journal_sectors)))) return -FS_EIO;
    uint8_t h[512]={0}; memcpy(h,"CVR1",4); fs_wr32(h+4,p->written);
    fs_wr32(h+8,target); fs_wr32(h+12,cv_crc(b,512)); fs_wr32(h+16,cv_crc(h,16));
    uint64_t pos=p->start+p->journal+2*p->written;
    int e=cv_write(p->parent,pos,h); if (!e) e=cv_write(p->parent,pos+1,b);
    if (!e) p->written++;
    fs_service(); return e;
}
static bool cv_used(struct cv_plan *p, uint32_t c)
{
    if (c<2+p->prefix) return true;
    if (p->root_old && c>=p->root_old && c<p->root_old+p->root_sectors/p->v->spc) return true;
    if (p->journal && cv_sector(p,c)>=p->journal && cv_sector(p,c)<p->journal+p->journal_sectors) return true;
    for (unsigned i=0;i<p->prefix;i++) if (p->map[i]==c) return true;
    return false;
}
static int cv_free_run(struct cv_plan *p, uint32_t count, uint32_t *out)
{
    uint32_t run=0;
    for (uint32_t c=2+p->prefix;c<p->v->clusters+2;c++) {
        if (!(c&127)) fs_service();
        uint32_t n; int e=fat_get_cluster(p->v,c,&n); if (e) return e;
        run=(n || cv_used(p,c)) ? 0 : run+1;
        if (run==count) { *out=c-count+1; return 0; }
    }
    return -FS_ENOSPC;
}
static int cv_tail(struct cv_plan *p, const struct fat_entry *f)
{
    if (!f->size) return 0;
    uint32_t old_bytes=(uint32_t)p->v->spc*512;
    uint32_t count=(f->size+ (uint64_t)old_bytes-1)/old_bytes;
    uint32_t last; int e=chain_at(p->v,f->first,count-1,&last); if (e) return e;
    unsigned page=last/4096;
    if (page>=CV_TAIL_PAGES) return -FS_EIO;
    if (!p->tails[page]) {
        p->tails[page]=fs_page_alloc(); if (!p->tails[page]) return -FS_ENOMEM;
        memset(p->tails[page],0,4096);
    }
    uint32_t remaining=f->size-(count-1)*old_bytes;
    p->tails[page][last%4096]=(uint8_t)((remaining+p->spc*512-1)/(p->spc*512));
    return 0;
}
static int cv_directory(struct cv_plan *p, uint32_t dir)
{
    struct fat_volume *v=p->v;
    uint32_t sectors=dir ? 0 : p->root_sectors;
    if (dir) {
        uint32_t n; bool fragmented;
        int e=defrag_chain(v,dir,&n,&fragmented,0,0); if (e) return e;
        sectors=n*v->spc;
    }
    if (!p->journal) { p->directory_sectors+=sectors; return 0; }
    bool ended=false;
    for (uint32_t i=0;i<sectors;i++) {
        uint8_t b[512]; uint32_t target; int e;
        if (!dir) {
            if (i<(v->root_entries*32+511)/512) e=readsec(v,v->root_sector+i,b);
            else { memset(b,0,512); e=0; }
            target=cv_sector(p,p->root_old)+i;
        } else {
            uint32_t c; e=chain_at(v,dir,i/v->spc,&c);
            if (e) return e;
            e=readsec(v,cluster_sector(v,c)+i%v->spc,b);
            target=cv_sector(p,cv_destination(p,c))+i%v->spc;
        }
        if (e) return e;
        if (ended) memset(b,0,512);
        for (unsigned j=0;j<16;j++) {
            uint8_t *entry=b+j*32;
            if (!entry[0]) { memset(entry,0,512-j*32); ended=true; break; }
            if (entry[0]==0xe5 || entry[11]==15 || (entry[11]&FAT_ATTR_VOLUME)) continue;
            uint32_t old=fs_rd16(entry+26), cluster=0;
            if (old) {
                if (!valid(v,old) || (old<2+p->prefix && !p->map[old-2])) return -FS_EIO;
                cluster=cv_cluster(p,old);
            }
            fs_wr16(entry+20,(uint16_t)(cluster>>16)); fs_wr16(entry+26,(uint16_t)cluster);
        }
        if ((e=cv_append(p,target,b))) return e;
    }
    return 0;
}
static int cv_tree(struct cv_plan *p)
{
    struct { uint32_t dir, cursor; } stack[SCAN_DEPTH];
    stack[0].dir=0; stack[0].cursor=0; unsigned depth=1;
    int e=cv_directory(p,0); if (e) return e;
    while (depth) {
        fs_service(); struct fat_entry f;
        e=fat_next(p->v,stack[depth-1].dir,&stack[depth-1].cursor,&f);
        if (e==-FS_ENOENT) { depth--; continue; }
        if (e) return e;
        if (path_equal(f.alias,".") || path_equal(f.alias,"..")) continue;
        if (f.attr&FAT_ATTR_DIR) {
            if (depth==SCAN_DEPTH) return -FS_EIO;
            if ((e=cv_directory(p,f.first))) return e;
            stack[depth].dir=f.first; stack[depth++].cursor=0;
        } else if (!p->journal && (e=cv_tail(p,&f))) return e;
    }
    return 0;
}
static int cv_fat_value(struct cv_plan *p, uint32_t c, uint32_t *out)
{
    *out=0;
    if (c==0) { *out=0x0fffff00u|p->v->media; return 0; }
    if (c==1) { *out=0xfffffff; return 0; }
    if (c>=p->clusters+2) return 0;
    uint32_t sector=p->data+(c-2)*p->spc;
    uint32_t root=cv_sector(p,p->root_old);
    if (sector>=root && sector<root+p->root_sectors) {
        *out=sector+p->spc==root+p->root_sectors ? 0xfffffff : c+1;
        return 0;
    }
    uint32_t old=(sector-p->v->data_sector)/p->v->spc+2;
    unsigned sub=(sector-p->v->data_sector)%p->v->spc/p->spc;
    if (old>=p->v->clusters+2) return 0;
    /* Free destinations contain copied prefix clusters, but the old FAT
     * stays untouched until the locator has committed. */
    for (unsigned i=0;i<p->prefix;i++) if (p->map[i]==old) { old=i+2; break; }
    uint32_t value; int e=fat_get_cluster(p->v,old,&value); if (e) return e;
    if (!value) return 0;
    if (value==0xfff7) { *out=0xffffff7; return 0; }
    unsigned tail=p->tails[old/4096] ? p->tails[old/4096][old%4096] : 0;
    if (tail && sub>=tail) return 0;
    if (tail && sub+1==tail) *out=0xfffffff;
    else if (sub+1<p->v->spc/p->spc) *out=c+1;
    else if (value>=0xfff8) *out=0xfffffff;
    else if (!valid(p->v,value) || (value<2+p->prefix && !p->map[value-2])) return -FS_EIO;
    else *out=cv_cluster(p,value);
    return 0;
}
static int cv_build(struct cv_plan *p, uint8_t boot[512], uint8_t mbr[512])
{
    struct fat_volume *v=p->v; int e;
    for (unsigned i=0;i<p->prefix;i++) if (p->map[i]) {
        for (unsigned s=0;s<v->spc;s++) {
            uint8_t b[512];
            if ((e=readsec(v,cluster_sector(v,i+2)+s,b)) ||
                (e=cv_write(p->parent,p->start+cv_sector(p,p->map[i])+s,b))) return e;
        }
    }
    for (unsigned copy=0;copy<2;copy++) for (uint32_t s=0;s<p->fat;s++) {
        uint8_t b[512];
        for (unsigned i=0;i<128;i++) {
            uint32_t value,c=s*128+i;
            if ((e=cv_fat_value(p,c,&value))) return e;
            fs_wr32(b+4*i,value);
            if (!copy && c>=2 && c<p->clusters+2 && !value) p->free_clusters++;
        }
        if ((e=cv_append(p,p->reserved+copy*p->fat+s,b))) return e;
    }
    if ((e=cv_tree(p))) return e;
    /* Preserve geometry, label and serial; no FAT16 boot code is executable
     * under the new BPB. This converter is for fixed data partitions. */
    uint8_t serial_label[15]; memcpy(serial_label,boot+39,15);
    memset(boot+36,0,474); boot[0]=0xeb; boot[1]=0xfe; boot[2]=0x90;
    memcpy(boot+3,"CIUKIOS ",8); boot[13]=(uint8_t)p->spc;
    fs_wr16(boot+14,(uint16_t)p->reserved); fs_wr16(boot+17,0);
    fs_wr16(boot+19,0); fs_wr16(boot+22,0); fs_wr32(boot+32,(uint32_t)v->sectors);
    fs_wr32(boot+36,p->fat); fs_wr32(boot+44,cv_cluster(p,p->root_old));
    fs_wr16(boot+48,1); fs_wr16(boot+50,6); boot[64]=0x80; boot[66]=0x29;
    memcpy(boot+67,serial_label,15); memcpy(boot+82,"FAT32   ",8); fs_wr16(boot+510,0xaa55);
    uint8_t info[512]={0}; fs_wr32(info,0x41615252); fs_wr32(info+484,0x61417272);
    fs_wr32(info+488,p->free_clusters); fs_wr32(info+492,0xffffffff); fs_wr32(info+508,0xaa550000);
    if ((e=cv_append(p,1,info)) || (e=cv_append(p,7,info)) || (e=cv_append(p,6,boot))) return e;
    uint8_t *entry=mbr+446+16*p->table_index;
    entry[4]=entry[4]==0x0e ? 0x0c : 0x0b;
    if ((e=cv_append(p,UINT32_MAX,mbr))) return e;
    return cv_append(p,0,boot); /* primary boot MUST be the final record */
}
int fat_convert_recover(struct blkdev *dev, uint64_t start, uint64_t sectors)
{
    if (!dev || start>=dev->capacity || !sectors || sectors>dev->capacity-start) return -FS_EINVAL;
    uint8_t locator[512]; int e=cv_read(dev,start,locator); if (e) return e;
    if (memcmp(locator+3,CV_MAGIC,8)) return 0;
    if (!dev->write || !blkdev_durable(dev)) return -FS_EROFS;
    if (memcmp(locator+96,"CVT1",4) || fs_rd32(locator+124)!=cv_crc(locator+96,28) ||
        fs_rd32(locator+112)!=sectors || fs_rd32(locator+116)!=start || start>UINT32_MAX || sectors>UINT32_MAX)
        return -FS_EIO;
    uint32_t journal=fs_rd32(locator+100), count=fs_rd32(locator+104), slot=fs_rd32(locator+108);
    uint32_t table_lba=fs_rd32(locator+120);
    if (table_lba>=start) return -FS_EIO;
    if (!count || slot>=4 || journal<32 || journal>=sectors || count>(sectors-journal)/2) return -FS_EIO;
    uint8_t table[512];
    if ((e=cv_read(dev,table_lba,table))) return e;
    /* Validate the ENTIRE journal first, then replay. A bad record cannot
     * cause partial application of a corrupt transaction. */
    for (unsigned pass=0;pass<2;pass++) for (uint32_t i=0;i<count;i++) {
        uint8_t h[512],b[512]; uint64_t pos=start+journal+2*i;
        if ((e=cv_read(dev,pos,h)) || (e=cv_read(dev,pos+1,b))) return e;
        uint32_t target=fs_rd32(h+8);
        if (memcmp(h,"CVR1",4) || fs_rd32(h+4)!=i || fs_rd32(h+16)!=cv_crc(h,16) ||
            fs_rd32(h+12)!=cv_crc(b,512) ||
            (target!=UINT32_MAX && (target>=sectors || (target>=journal && target<journal+2*count))) ||
            (i+1==count && target!=0) || (i+1<count && target==0)) return -FS_EIO;
        if (target==UINT32_MAX) {
            const uint8_t *part=b+446+16*slot;
            if (fs_rd16(b+510)!=0xaa55 || fs_rd32(part+8)!=start-table_lba || fs_rd32(part+12)!=sectors ||
                (part[4]!=0x0b && part[4]!=0x0c) || i+2!=count) return -FS_EIO;
            for (unsigned j=0;j<512;j++)
                if (j!=450+16*slot && b[j]!=table[j]) return -FS_EIO;
        }
        if (pass) {
            if (i+1==count && (e=cv_flush(dev))) return e;
            if ((e=cv_write(dev,target==UINT32_MAX ? table_lba : start+target,b))) return e;
        }
        fs_service();
    }
    return cv_flush(dev);
}
static int cv_find_table(struct cv_plan *p, uint8_t table[512])
{
    uint32_t base=0, lba=0;
    for (unsigned depth=0;depth<65;depth++) {
        int e=cv_read(p->parent,lba,table); if (e) return e;
        if (fs_rd16(table+510)!=0xaa55) return -FS_EINVAL;
        unsigned entries=depth ? 1 : 4;
        for (unsigned i=0;i<entries;i++) {
            const uint8_t *entry=table+446+16*i;
            uint64_t start=(uint64_t)lba+fs_rd32(entry+8), count=fs_rd32(entry+12);
            if (start==p->start && count>=p->v->sectors && count<=p->parent->capacity-start &&
                (entry[4]==4 || entry[4]==6 || entry[4]==0x0e)) {
                p->table_index=i; p->table_lba=lba; p->partition_sectors=(uint32_t)count; return 0;
            }
        }
        if (!depth) {
            for (unsigned i=0;i<4;i++) {
                const uint8_t *entry=table+446+16*i;
                if (entry[4]==5 || entry[4]==0x0f || entry[4]==0x85) { base=fs_rd32(entry+8); break; }
            }
            lba=base;
        } else {
            const uint8_t *next=table+462;
            if (!next[4] || (next[4]!=5 && next[4]!=0x0f && next[4]!=0x85)) break;
            uint64_t address=(uint64_t)base+fs_rd32(next+8);
            if (address>UINT32_MAX) return -FS_EINVAL;
            lba=(uint32_t)address;
        }
        if (!lba || lba>=p->start || lba>=p->parent->capacity) break;
    }
    return -FS_EOPNOTSUPP;
}
static int cv_run(struct fat_volume *v, struct blkdev *parent, uint64_t start, bool boot_volume)
{
    uint32_t bytes; int e=fat_convert_plan(v,&bytes); if (e) return e;
    if (!parent || !parent->write || !blkdev_durable(parent) || start>UINT32_MAX ||
        !start || start>=parent->capacity || v->sectors>parent->capacity-start ||
        v->transactions || !v->readonly || v->writable_session) return -FS_EBUSY;
    if ((e=fat_scan(v))) return e;
    if (v->ro_reasons&~(FAT_RO_REQUEST|FAT_RO_DURABILITY)) return -FS_EIO;
    struct cv_plan *p=fs_page_alloc(); if (!p) return -FS_ENOMEM;
    memset(p,0,sizeof(*p));
    p->map=fs_page_alloc(); if (!p->map) { fs_page_free(p); return -FS_ENOMEM; }
    memset(p->map,0,4096); p->v=v; p->parent=parent; p->start=start; p->spc=bytes/512;
    p->fat=(uint32_t)((v->sectors-32+128*p->spc)/(128*p->spc+2));
    uint32_t data=32+2*p->fat; if (data<v->data_sector) data=v->data_sector;
    p->prefix=(data-v->data_sector+v->spc-1)/v->spc;
    p->data=v->data_sector+p->prefix*v->spc; p->reserved=p->data-2*p->fat;
    p->clusters=(uint32_t)((v->sectors-p->data)/p->spc);
    p->root_sectors=((v->root_entries*32+511)/512+v->spc-1)/v->spc*v->spc;
    uint8_t boot[512],mbr[512];
    if ((e=cv_find_table(p,mbr)) || (e=readsec(v,0,boot))) goto out;
    if (fs_rd32(boot+28)!=start) { e=-FS_EINVAL; goto out; }
    if ((e=cv_tree(p))) goto out;
    p->records=2*p->fat+p->directory_sectors+5;
    if (boot_volume && p->records>8192) { e=-FS_EOPNOTSUPP; goto out; }
    p->journal_sectors=(2*p->records+v->spc-1)/v->spc*v->spc;
    if ((e=cv_free_run(p,p->root_sectors/v->spc,&p->root_old))) goto out;
    uint32_t journal;
    if ((e=cv_free_run(p,p->journal_sectors/v->spc,&journal))) goto out;
    p->journal=cv_sector(p,journal);
    for (unsigned i=0;i<p->prefix;i++) {
        uint32_t n; if ((e=fat_get_cluster(v,i+2,&n))) goto out;
        if (n==0xfff7) { e=-FS_EIO; goto out; }
        if (n && (e=cv_free_run(p,1,&p->map[i]))) goto out;
    }
    if ((e=cv_build(p,boot,mbr))) goto out;
    if (p->written!=p->records) { e=-FS_EIO; goto out; }
    if ((e=cv_flush(parent))) goto out;
    /* Publish the locator only after all redo images and relocated data
     * are durable. Invalidate the BPB so foreign FAT drivers also refuse
     * to mount the transaction while replay changes directory references. */
    if ((e=readsec(v,0,boot))) goto out;
    memcpy(boot+3,CV_MAGIC,8); memcpy(boot+96,"CVT1",4);
    fs_wr16(boot+11,0); boot[0]=0xeb; boot[1]=0xfe; boot[2]=0x90;
    fs_wr32(boot+100,p->journal); fs_wr32(boot+104,p->records);
    fs_wr32(boot+108,p->table_index); fs_wr32(boot+112,p->partition_sectors);
    fs_wr32(boot+116,(uint32_t)start); fs_wr32(boot+120,p->table_lba); fs_wr32(boot+124,cv_crc(boot+96,28));
    if ((e=cv_write(parent,start,boot)) || (e=cv_flush(parent))) goto out;
    e=fat_convert_recover(parent,start,p->partition_sectors);
out:
    for (unsigned i=0;i<CV_TAIL_PAGES;i++) if (p->tails[i]) fs_page_free(p->tails[i]);
    fs_page_free(p->map); fs_page_free(p); return e;
}

int fat_convert(struct fat_volume *v,struct blkdev *parent,uint64_t start)
{ return cv_run(v,parent,start,false); }
int fat_convert_boot(struct fat_volume *v,struct blkdev *parent,uint64_t start)
{ return cv_run(v,parent,start,true); }
/* The capability is carried by both stages. Check the same complete loader
 * CRC as the native MBR, not merely a string in an otherwise foreign disk. */
int fat_convert_boot_loader(struct blkdev *dev,uint64_t start,uint64_t sectors)
{
    uint8_t b[512]; int e=cv_read(dev,0,b); if (e) return e;
    if (start!=2048 || memcmp(b+436,"CKD1",4) || fs_rd16(b+510)!=0xaa55 ||
        fs_rd32(b+454)!=start || fs_rd32(b+458)!=sectors || !(b[446]&0x80)) return -FS_EOPNOTSUPP;
    if ((e=cv_read(dev,1,b))) return e;
    if (memcmp(b,"CLDR",4) || fs_rd16(b+4)!=1 || fs_rd16(b+6)!=48 ||
        fs_rd16(b+14)!=16 || fs_rd16(b+12)<16 || fs_rd16(b+12)>=24576) return -FS_EOPNOTSUPP;
    uint32_t expected=fs_rd32(b+8),crc=~0u;
    for (unsigned sector=0;sector<48;sector++) {
        if (sector && (e=cv_read(dev,sector+1,b))) return e;
        for (unsigned i=sector ? 0 : 16;i<512;i++) {
            crc^=b[i];
            for (unsigned j=0;j<8;j++) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));
        }
    }
    return ~crc==expected && !memcmp(b+472,"G29DCVT1",8) ? 0 : -FS_EOPNOTSUPP;
}
int fat_convert_boot_schedule(struct fat_volume *v,struct blkdev *parent,uint64_t start)
{
    uint32_t bytes; int e=fat_convert_plan(v,&bytes); if (e) return e;
    if (v->spc>64) return -FS_EOPNOTSUPP; /* Windows FAT16: at most 32 KiB clusters */
    uint8_t b[512]; if ((e=cv_read(parent,0,b))) return e;
    uint32_t partition_sectors=fs_rd32(b+458);
    if (partition_sectors<v->sectors) return -FS_EINVAL;
    if ((e=fat_convert_boot_loader(parent,start,partition_sectors))) return e;
    if ((e=fat_commit(v))) return e;
    if ((e=cv_read(parent,start,b))) return e;
    memcpy(b+128,"CIUKINXT",8); fs_wr32(b+136,1); fs_wr32(b+140,v->volume_serial);
    fs_wr32(b+144,partition_sectors); fs_wr32(b+148,(uint32_t)start);
    fs_wr32(b+152,cv_crc(b+128,24));
    if ((e=cv_write(parent,start,b))) return e;
    return cv_flush(parent);
}
int fat_convert_boot_pending(struct blkdev *parent,uint64_t start,uint64_t sectors,bool *pending)
{
    if (!pending) return -FS_EINVAL;
    *pending=false;
    uint8_t b[512]; int e=cv_read(parent,start,b); if (e) return e;
    if (memcmp(b+128,"CIUKINXT",8)) return 0;
    if (fs_rd32(b+136)!=1 || fs_rd32(b+140)!=fs_rd32(b+39) ||
        fs_rd32(b+144)!=sectors || fs_rd32(b+148)!=start ||
        fs_rd32(b+152)!=cv_crc(b+128,24)) return -FS_EIO;
    e=fat_convert_boot_loader(parent,start,sectors);
    if (!e) *pending=true;
    return e;
}

/* Only an intact FAT16 pre-commit state can cancel a failed offline job.
 * A committed locator must instead be replayed; never erase its recovery path. */
int fat_convert_boot_cancel(struct blkdev *parent,uint64_t start)
{
    uint8_t b[512]; int e=cv_read(parent,start,b); if (e) return e;
    if (!memcmp(b+3,CV_MAGIC,8) || fs_rd16(b+11)!=512 || !fs_rd16(b+22) || fs_rd16(b+510)!=0xaa55 ||
        memcmp(b+128,"CIUKINXT",8) || fs_rd32(b+148)!=start ||
        fs_rd32(b+152)!=cv_crc(b+128,24)) return -FS_EBUSY;
    memset(b+128,0,28);
    if ((e=cv_write(parent,start,b))) return e;
    return cv_flush(parent);
}
