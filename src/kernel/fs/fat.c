/* On-disk rules researched in fatgen103; see tests/host/fs/VALIDATION.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "fat.h"
static uint32_t mask(struct fat_volume *v) { return v->type==12 ? 0xfff : v->type==16 ? 0xffff : 0xfffffff; }
static uint32_t cleanbit(struct fat_volume *v) { return v->type==16 ? 0x8000 : v->type==32 ? 0x8000000 : 0; }
static uint32_t errorbit(struct fat_volume *v) { return cleanbit(v)>>1; }
static bool valid(struct fat_volume *v, uint32_t c) { return c>=2 && c<v->clusters+2; }
static bool eoc(struct fat_volume *v, uint32_t c) { return c>=(mask(v)-7); }
static int corrupt(struct fat_volume *v) { v->readonly=true; v->ro_reasons|=FAT_RO_CORRUPT; v->diagnostic="chain/directory corruption"; return -FS_EUCLEAN; }
static int readsec(struct fat_volume *v, uint64_t s, void *b) {
    if (s>=v->sectors) return corrupt(v);
    int e=cache_read(v->cache,v->dev,v->start+s,b);
    return e ? (e==-FS_EQUARANTINED ? -FS_EIO : e) : 0;
}
static int write_error(struct fat_volume *v, int e) {
    if (e) { v->readonly=true; v->ro_reasons|=FAT_RO_WRITE_ERROR; v->diagnostic="metadata/data durability failure"; }
    return e;
}
static int writable(struct fat_volume *v) {
    int e=cache_error(v->cache,v->dev); if (e) return write_error(v,e);
    if (!v->mounted || v->readonly) return -FS_EROFS;
    if (!blkdev_durable(v->dev)) return write_error(v,-FS_EIO);
    return 0;
}
static int writesec(struct fat_volume *v, uint64_t s, const void *b) {
    if (s>=v->sectors) return corrupt(v);
    return write_error(v,cache_write(v->cache,v->dev,v->start+s,b));
}
static int barrier(struct fat_volume *v) { return write_error(v,cache_barrier(v->cache,v->dev)); }
static uint32_t offset(struct fat_volume *v, uint32_t c) { return v->type==12 ? c+c/2 : c*(v->type/8); }
static int fat_bytes(struct fat_volume *v, unsigned copy, uint32_t off, uint8_t *p, unsigned n, bool wr) {
    uint8_t b[512];
    while (n) {
        uint32_t s=v->reserved+copy*v->fat_sectors+off/512;
        unsigned take=512-off%512; if (take>n) take=n;
        int e=readsec(v,s,b); if (e) return e;
        if (wr) { memcpy(b+off%512,p,take); if ((e=writesec(v,s,b))) return e; }
        else memcpy(p,b+off%512,take);
        off+=take; p+=take; n-=take;
    }
    return 0;
}
int fat_get_cluster(struct fat_volume *v, uint32_t c, uint32_t *out) {
    if (c>=v->clusters+2) return corrupt(v);
    uint8_t b[4]; int e=fat_bytes(v,v->active_fat,offset(v,c),b,v->type==32 ? 4 : 2,false); if (e) return e;
    uint32_t x=v->type==32 ? fs_rd32(b) : fs_rd16(b);
    if (v->type==12 && (c&1)) x>>=4;
    *out=x&mask(v); return 0;
}
static int set_cluster(struct fat_volume *v, uint32_t c, uint32_t value) {
    uint8_t b[4]; unsigned n=v->type==32 ? 4 : 2;
    for (unsigned f=0;f<v->fats;f++) {
        int e=fat_bytes(v,f,offset(v,c),b,n,false); if (e) return write_error(v,e);
        uint32_t x=n==4 ? fs_rd32(b) : fs_rd16(b);
        if (v->type==12) x=(c&1) ? (x&15)|(value<<4) : (x&0xf000)|value;
        else if (v->type==32) x=(x&0xf0000000)|value; else x=value;
        if (n==4) fs_wr32(b,x); else fs_wr16(b,(uint16_t)x);
        if ((e=fat_bytes(v,f,offset(v,c),b,n,true))) return e;
    }
    return 0;
}
static uint32_t cluster_sector(struct fat_volume *v, uint32_t c) { return v->data_sector+(c-2)*v->spc; }
static int chain_at(struct fat_volume *v, uint32_t first, uint32_t index, uint32_t *out) {
    if (index>=v->clusters || !valid(v,first)) return corrupt(v);
    uint32_t c=first;
    for (uint32_t i=0;i<index;i++) {
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return e;
        if (eoc(v,n)) return -FS_ENOENT;
        if (!valid(v,n)) return corrupt(v);
        c=n;
    }
    *out=c; return 0;
}
static int dir_sector(struct fat_volume *v, uint32_t dir, uint32_t index, uint32_t *sec) {
    if (!dir && v->type!=32) {
        if (index>=v->root_entries) return -FS_ENOENT;
        *sec=v->root_sector+index/16; return 0;
    }
    uint32_t c; int e=chain_at(v,dir ? dir : v->root,index/(16u*v->spc),&c); if (e) return e;
    *sec=cluster_sector(v,c)+(index/16)%v->spc; return 0;
}
static int dir_read(struct fat_volume *v, uint32_t dir, uint32_t i, uint8_t out[32]) {
    uint32_t s; uint8_t b[512]; int e=dir_sector(v,dir,i,&s); if (e) return e;
    if ((e=readsec(v,s,b))) return e; memcpy(out,b+(i%16)*32,32); return 0;
}
static int dir_write(struct fat_volume *v, uint32_t dir, uint32_t i, const uint8_t in[32]) {
    uint32_t s; uint8_t b[512]; int e=dir_sector(v,dir,i,&s); if (e) return e;
    if ((e=readsec(v,s,b))) return write_error(v,e);
    memcpy(b+(i%16)*32,in,32); return writesec(v,s,b);
}
static uint8_t checksum(const uint8_t *s) {
    uint8_t x=0; for (unsigned i=0;i<11;i++) x=(uint8_t)((x>>1)|((x&1)<<7)) + s[i]; return x;
}
static const uint8_t lfn_offsets[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
static int alias_decode(const uint8_t *b, char out[40]) {
    uint16_t u[12]; unsigned n=0;
    for (unsigned i=0;i<11;i++) {
        if (i==8 && b[8]!=' ') u[n++]='.';
        if (b[i]==' ') continue;
        uint16_t c=path_from_cp437(i==0 && b[i]==5 ? 0xe5 : b[i]);
        if (c>='A' && c<='Z' && (b[12]&(i<8 ? 8 : 16))) c+=32;
        u[n++]=c;
    }
    return path_from_ucs2(u,n,out,40);
}
static void decode_entry(struct fat_volume *v, const uint8_t b[32], struct fat_entry *e) {
    e->attr=b[11]; e->first=fs_rd16(b+26); if (v->type==32) e->first|=(uint32_t)fs_rd16(b+20)<<16;
    e->size=fs_rd32(b+28); e->times.create_tenths=b[13]; e->times.create_time=fs_rd16(b+14);
    e->times.create_date=fs_rd16(b+16); e->times.access_date=fs_rd16(b+18);
    e->times.write_time=fs_rd16(b+22); e->times.write_date=fs_rd16(b+24);
}
static void encode_entry(struct fat_volume *v, uint8_t b[32], const struct fat_entry *e) {
    b[11]=e->attr; b[13]=e->times.create_tenths; fs_wr16(b+14,e->times.create_time);
    fs_wr16(b+16,e->times.create_date); fs_wr16(b+18,e->times.access_date);
    fs_wr16(b+20,v->type==32 ? (uint16_t)(e->first>>16) : 0);
    fs_wr16(b+22,e->times.write_time); fs_wr16(b+24,e->times.write_date);
    fs_wr16(b+26,(uint16_t)e->first); fs_wr32(b+28,e->size);
}
int fat_next(struct fat_volume *v, uint32_t dir, uint32_t *cursor, struct fat_entry *entry) {
    uint16_t name[260]; unsigned expected=0,total=0; uint8_t sum=0; uint32_t start=0;
    uint64_t limit=dir || v->type==32 ? (uint64_t)v->clusters*v->spc*16 : v->root_entries;
    if (limit>UINT32_MAX) limit=UINT32_MAX;
    while (*cursor<limit) {
        uint8_t b[32]; uint32_t i=(*cursor)++; int e=dir_read(v,dir,i,b); if (e) return e;
        if (!b[0]) { if (total) v->lfn_orphans++; return -FS_ENOENT; }
        if (b[0]==0xe5) { if (total) v->lfn_orphans++; expected=total=0; continue; }
        if (b[11]==15) {
            unsigned ord=b[0]&31;
            if (b[0]&64) { if (total) v->lfn_orphans++; total=expected=ord; sum=b[13]; start=i; memset(name,0xff,sizeof(name)); }
            if (!ord || ord>20 || ord!=expected || b[0]&(0x80|0x20) || b[12] || fs_rd16(b+26) || b[13]!=sum) { v->lfn_invalid++; expected=total=0; continue; }
            for (unsigned k=0;k<13;k++) name[(ord-1)*13+k]=fs_rd16(b+lfn_offsets[k]);
            expected--; continue;
        }
        if (b[11]&FAT_ATTR_VOLUME) { if (total) v->lfn_orphans++; expected=total=0; continue; }
        memset(entry,0,sizeof(*entry)); entry->parent=dir; entry->index=i; entry->lfn_index=i;
        if ((e=alias_decode(b,entry->alias))) return corrupt(v);
        memcpy(entry->name,entry->alias,strlen(entry->alias)+1);
        if (total && !expected && sum==checksum(b)) {
            unsigned n=0; while (n<total*13 && name[n] && name[n]!=0xffff) n++;
            bool ok=n>0 && n<=255;
            if (n<total*13) { if (name[n]!=0) ok=false; for (unsigned k=n+1;k<total*13;k++) if (name[k]!=0xffff) ok=false; }
            if (ok && !path_from_ucs2(name,n,entry->name,sizeof(entry->name)) && !path_validate_name(entry->name,name,&n)) { entry->lfn_index=start; entry->lfn_count=(uint8_t)total; }
            else { v->lfn_invalid++; memcpy(entry->name,entry->alias,strlen(entry->alias)+1); }
        } else if (total) {
            if (expected) v->lfn_orphans++; else v->lfn_bad_checksum++;
        }
        decode_entry(v,b,entry); return 0;
    }
    return (!dir && v->type!=32) ? -FS_ENOENT : corrupt(v);
}
int fat_lookup(struct fat_volume *v, uint32_t dir, const char *name, struct fat_entry *e) {
    uint32_t i=0; int err;
    while (!(err=fat_next(v,dir,&i,e))) if (path_equal(name,e->name) || path_equal(name,e->alias)) return 0;
    return err;
}
static int update(struct fat_volume *v, const struct fat_entry *e) {
    uint8_t b[32]; int err=dir_read(v,e->parent,e->index,b); if (err) return write_error(v,err);
    encode_entry(v,b,e); if ((err=dir_write(v,e->parent,e->index,b))) return err;
    return barrier(v);
}
static int hints(struct fat_volume *v) {
    if (v->type!=32) return 0;
    uint8_t b[512]; int e=readsec(v,v->fsinfo,b); if (e) return write_error(v,e);
    fs_wr32(b+488,v->free_clusters); fs_wr32(b+492,v->next_free);
    if ((e=writesec(v,v->fsinfo,b))) return e;
    return barrier(v);
}
static int release_chain(struct fat_volume *v, uint32_t c) {
    for (uint32_t i=0;c && i<v->clusters;i++) {
        if (!valid(v,c)) return corrupt(v);
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return write_error(v,e);
        if (!eoc(v,n) && !valid(v,n)) return corrupt(v);
        if ((e=set_cluster(v,c,0))) return e;
        v->free_clusters++; if (c<v->next_free) v->next_free=c;
        if (eoc(v,n)) return barrier(v);
        c=n;
    }
    return c ? corrupt(v) : 0;
}
static bool split12(struct fat_volume *v, uint32_t c) { return v->type==12 && offset(v,c)%512==511; }
static int allocate(struct fat_volume *v, uint32_t n, bool directory, uint32_t *first) {
    *first=0; if (n>v->free_clusters) return -FS_ENOSPC;
    uint32_t tail=0, found=0, c=v->next_free;
    for (uint32_t scanned=0;scanned<v->clusters && found<n;scanned++) {
        if (!valid(v,c)) c=2;
        uint32_t val; int e=fat_get_cluster(v,c,&val); if (e) return write_error(v,e);
        if (!val && !(directory && split12(v,c))) {
            if ((e=set_cluster(v,c,mask(v)))) return e;
            if (tail && (e=set_cluster(v,tail,c))) return e;
            if (!*first) *first=c;
            tail=c; found++; v->free_clusters--;
        }
        c++;
    }
    v->next_free=valid(v,c) ? c : 2;
    if (found!=n) { int e=release_chain(v,*first); *first=0; return e ? e : -FS_ENOSPC; }
    return barrier(v);
}
static int zero_chain(struct fat_volume *v, uint32_t c) {
    uint8_t b[512]; memset(b,0,sizeof(b));
    for (uint32_t i=0;i<v->clusters;i++) {
        for (unsigned s=0;s<v->spc;s++) { int e=writesec(v,cluster_sector(v,c)+s,b); if (e) return e; }
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return write_error(v,e);
        if (eoc(v,n)) return barrier(v); if (!valid(v,n)) return corrupt(v); c=n;
    }
    return corrupt(v);
}
static int grow_directory(struct fat_volume *v, uint32_t dir) {
    if (!dir && v->type!=32) return -FS_ENOSPC;
    uint32_t c=dir ? dir : v->root;
    for (uint32_t i=0;i<v->clusters;i++) {
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return e;
        if (eoc(v,n)) {
            /* There is no atomic two-sector update of a live FAT12 link.
             * New directories avoid such clusters. Imported split tails need
             * a future directory COW/recovery design, not an unsafe update. */
            if (split12(v,c)) return -FS_EOPNOTSUPP;
            uint32_t newc; if ((e=allocate(v,1,true,&newc))) return e;
            if ((e=zero_chain(v,newc))) return e;
            if ((e=set_cluster(v,c,newc))) return e;
            return barrier(v);
        }
        if (!valid(v,n)) return corrupt(v); c=n;
    }
    return corrupt(v);
}
static int slots(struct fat_volume *v, uint32_t dir, unsigned need, uint32_t *start) {
    uint32_t run=0, first_zero=0; bool end=false;
    uint64_t limit=dir || v->type==32 ? (uint64_t)v->clusters*v->spc*16 : v->root_entries;
    if (limit>UINT32_MAX) limit=UINT32_MAX;
    for (uint32_t i=0;(uint64_t)i<limit;i++) {
        uint8_t b[32]; int e=dir_read(v,dir,i,b);
        if (e==-FS_ENOENT) { if ((e=grow_directory(v,dir))) return e; if ((e=dir_read(v,dir,i,b))) return e; }
        else if (e) return e;
        if (!b[0] && !end) { end=true; first_zero=i; }
        if (end || b[0]==0xe5) {
            if (!run) *start=i;
            if (++run==need) {
                if (end) {
                    /* Bytes after the first 00 entry are unspecified on a
                     * foreign volume. Clear every newly exposed slot and a
                     * new terminator DURABLY while the old terminator still
                     * hides them. Otherwise a crash can expose stale owners. */
                    uint8_t zero[32]; memset(zero,0,sizeof(zero));
                    for (uint32_t j=first_zero+1;j<=i;j++)
                        if ((e=dir_write(v,dir,j,zero))) return e;
                    uint32_t sector;
                    e=dir_sector(v,dir,i+1,&sector);
                    if (!e) { if ((e=dir_write(v,dir,i+1,zero))) return e; }
                    else if (e!=-FS_ENOENT) return e;
                    if ((e=barrier(v))) return e;
                }
                return 0;
            }
        } else run=0;
    }
    return -FS_ENOSPC;
}
static bool short_ok(uint16_t c) {
    if (c<=32 || c=='.' || c=='"' || c=='*' || c=='+' || c==',' || c=='/' || c==':' || c==';' || c=='<' || c=='=' || c=='>' || c=='?' || c=='[' || c=='\\' || c==']' || c=='|') return false;
    return true;
}
static int alias_exists(struct fat_volume *v, uint32_t dir, const uint8_t alias[11], const struct fat_entry *ignore) {
    uint8_t b[32]; memcpy(b,alias,11); b[12]=0; char text[40]; int e=alias_decode(b,text); if (e) return e;
    struct fat_entry *entry=&v->alias_scratch; uint32_t i=0;
    while (!(e=fat_next(v,dir,&i,entry))) {
        if (ignore && entry->index==ignore->index && dir==ignore->parent) continue;
        if (path_equal(text,entry->alias) || path_equal(text,entry->name)) return 1;
    }
    return e==-FS_ENOENT ? 0 : e;
}
static int make_alias(struct fat_volume *v, uint32_t dir, const uint16_t *u, unsigned n, uint8_t alias[11], const struct fat_entry *ignore) {
    uint8_t basis[11]; memset(basis,' ',11); unsigned begin=0,last=n;
    while (begin<n && (u[begin]==' ' || u[begin]=='.')) begin++;
    for (unsigned i=begin;i<n;i++) if (u[i]=='.') last=i;
    bool lossy=begin!=0; unsigned b=0,x=0; bool dot=false;
    for (unsigned i=begin;i<n;i++) {
        if (u[i]=='.') { if (i!=last) lossy=true; dot=true; continue; }
        if (u[i]==' ') { lossy=true; continue; }
        uint16_t upper=path_upper(u[i]); int cp=path_cp437(upper);
        /* A case counterpart absent in CP437 retains the representable glyph.
         * A UCS-2 glyph absent in CP437 is rejected by the F1 contract. */
        if (cp<0) cp=path_cp437(u[i]); if (cp<0) return cp;
        if (!short_ok(upper)) { cp='_'; lossy=true; }
        if (i<last) { if (!dot && b<8) basis[b++]=(uint8_t)cp; else lossy=true; }
        else { if (x<3) basis[8+x++]=(uint8_t)cp; else lossy=true; }
    }
    if (!b) { basis[0]='_'; b=1; lossy=true; }
    if (basis[0]==0xe5) basis[0]=5;
    memcpy(alias,basis,11);
    int e=alias_exists(v,dir,alias,ignore); if (e<0) return e;
    if (!lossy && !e) return 0;
    for (unsigned tail=1;tail<=999999;tail++) {
        char digits[6]; unsigned k=0,t=tail; do { digits[k++]=(char)('0'+t%10); t/=10; } while (t);
        memcpy(alias,basis,11); unsigned keep=b; if (keep>7-k) keep=7-k;
        memset(alias+keep,' ',8-keep); alias[keep++]='~';
        while (k) alias[keep++]=(uint8_t)digits[--k];
        e=alias_exists(v,dir,alias,ignore); if (e<0) return e; if (!e) return 0;
    }
    return -FS_ENOSPC;
}
static int publish(struct fat_volume *v, struct fat_entry *e, const uint16_t *u, unsigned n, const uint8_t alias[11]) {
    /* LFN entries own no clusters. Make them durable before the sole owning
     * short entry, including when entries span multiple sectors. */
    uint8_t b[32]; uint8_t sum=checksum(alias);
    for (unsigned j=0;j<e->lfn_count;j++) {
        unsigned ord=e->lfn_count-j; memset(b,0,sizeof(b)); b[0]=(uint8_t)(ord|(j==0 ? 64 : 0)); b[11]=15; b[13]=sum;
        for (unsigned k=0;k<13;k++) { unsigned pos=(ord-1)*13+k; fs_wr16(b+lfn_offsets[k],pos<n ? u[pos] : pos==n ? 0 : 0xffff); }
        int err=dir_write(v,e->parent,e->lfn_index+j,b); if (err) return err;
    }
    int err=barrier(v); if (err) return err;
    memset(b,0,sizeof(b)); memcpy(b,alias,11); encode_entry(v,b,e);
    if ((err=dir_write(v,e->parent,e->index,b))) return err;
    if ((err=barrier(v))) return err;
    alias_decode(b,e->alias); path_from_ucs2(u,n,e->name,sizeof(e->name)); return 0;
}
static int unlink_entry(struct fat_volume *v, const struct fat_entry *e) {
    uint8_t b[32]; int err=dir_read(v,e->parent,e->index,b); if (err) return err;
    b[0]=0xe5; if ((err=dir_write(v,e->parent,e->index,b)) || (err=barrier(v))) return err;
    for (unsigned i=0;i<e->lfn_count;i++) {
        if ((err=dir_read(v,e->parent,e->lfn_index+i,b))) return err;
        b[0]=0xe5; if ((err=dir_write(v,e->parent,e->lfn_index+i,b))) return err;
    }
    return barrier(v);
}
int fat_create(struct fat_volume *v, uint32_t dir, const char *name, uint8_t attr, struct fat_entry *out) {
    int e=writable(v); if (e) return e;
    if (attr&~0x37u) return -FS_EINVAL;
    uint16_t u[256]; unsigned n; if ((e=path_validate_name(name,u,&n))) return e;
    e=fat_lookup(v,dir,name,out); if (!e) return -FS_EEXIST; if (e!=-FS_ENOENT) return e;
    uint8_t alias[11]; if ((e=make_alias(v,dir,u,n,alias,0))) return e;
    memset(out,0,sizeof(*out)); out->parent=dir; out->attr=attr;
    /* Always preserve exact case in LFN; even a short spelling may mix case. */
    out->lfn_count=(uint8_t)((n+12)/13);
    if ((e=slots(v,dir,out->lfn_count+1,&out->lfn_index))) return e;
    out->index=out->lfn_index+out->lfn_count;
    fs_timestamp(&out->times.create_date,&out->times.create_time,&out->times.create_tenths);
    out->times.write_date=out->times.access_date=out->times.create_date;
    out->times.write_time=out->times.create_time;
    if (attr&FAT_ATTR_DIR) {
        if ((e=allocate(v,1,true,&out->first)) || (e=zero_chain(v,out->first))) return e;
        uint8_t b[32]; struct fat_entry dot=*out; dot.size=0;
        memset(b,0,sizeof(b)); memset(b,' ',11); b[0]='.'; encode_entry(v,b,&dot);
        if ((e=dir_write(v,out->first,0,b))) return e;
        b[1]='.'; dot.first=(dir==v->root || !dir) ? 0 : dir; encode_entry(v,b,&dot);
        if ((e=dir_write(v,out->first,1,b)) || (e=barrier(v))) return e;
    }
    if ((e=publish(v,out,u,n,alias))) return e;
    return hints(v);
}
int fat_read(struct fat_volume *v, const struct fat_entry *file, uint64_t pos, void *buf, size_t size, size_t *done) {
    *done=0; if (!buf && size) return -FS_EINVAL;
    if (file->attr&FAT_ATTR_DIR) return -FS_EISDIR;
    if (pos>=file->size || !size) return 0;
    if (size>file->size-pos) size=(size_t)(file->size-pos);
    uint32_t c; uint32_t cluster_bytes=(uint32_t)v->spc*512;
    int e=chain_at(v,file->first,(uint32_t)pos/cluster_bytes,&c); if (e) return e;
    unsigned off=(uint32_t)pos%cluster_bytes; uint32_t walked=0;
    while (size) {
        uint8_t b[512]; if ((e=readsec(v,cluster_sector(v,c)+off/512,b))) return e;
        size_t n=512-off%512; if (n>size) n=size;
        memcpy((uint8_t *)buf+*done,b+off%512,n); *done+=n; size-=n; off+=(unsigned)n;
        if (off==cluster_bytes && size) {
            uint32_t next; if ((e=fat_get_cluster(v,c,&next))) return e;
            if (!valid(v,next) || ++walked>=v->clusters) return corrupt(v); c=next; off=0;
        }
    }
    return 0;
}
/* Full-file COW gives one atomic owning-entry switch, including FAT12 packed
 * entries and shrinking. No private data cache, buffers or heap allocation. */
static int replace(struct fat_volume *v, struct fat_entry *file, uint32_t size, uint64_t pos, const void *data, size_t amount) {
    uint32_t cb=(uint32_t)v->spc*512, count=size/cb+(size%cb!=0), first=0;
    int e=allocate(v,count,false,&first); if (e) return e;
    uint32_t c=first, oldc=file->first; uint64_t at=0;
    for (uint32_t i=0;i<count;i++) {
        for (unsigned s=0;s<v->spc;s++,at+=512) {
            uint8_t b[512]; memset(b,0,sizeof(b));
            if (at<size && at<file->size) {
                if (!valid(v,oldc)) { e=corrupt(v); goto abandon; }
                if ((e=readsec(v,cluster_sector(v,oldc)+s,b))) goto abandon;
                uint64_t keep=file->size<size ? file->size : size;
                if (at+512>keep) memset(b+(size_t)(keep-at),0,(size_t)(at+512-keep));
            }
            uint64_t lo=at>pos ? at : pos, hi=at+512<pos+amount ? at+512 : pos+amount;
            if (lo<hi) memcpy(b+(size_t)(lo-at),(const uint8_t *)data+(size_t)(lo-pos),(size_t)(hi-lo));
            if ((e=writesec(v,cluster_sector(v,c)+s,b))) return e;
        }
        if (i+1<count) {
            uint32_t next;
            if ((e=fat_get_cluster(v,c,&next))) goto abandon;
            if (!valid(v,next)) { e=corrupt(v); goto abandon; } c=next;
            if (at<file->size) {
                if ((e=fat_get_cluster(v,oldc,&next))) goto abandon;
                if (!valid(v,next)) { e=corrupt(v); goto abandon; } oldc=next;
            }
        }
    }
    if ((e=barrier(v))) return e;
    struct fat_entry changed=*file; changed.first=first; changed.size=size; changed.attr|=FAT_ATTR_ARCHIVE;
    uint8_t tenths; fs_timestamp(&changed.times.write_date,&changed.times.write_time,&tenths);
    if ((e=update(v,&changed))) return e;
    uint32_t old=file->first; *file=changed;
    if ((e=release_chain(v,old))) return e;
    return hints(v);
abandon:
    /* I/O failures revoke writing; no speculative cleanup after an error. */
    return write_error(v,e);
}
int fat_write(struct fat_volume *v, struct fat_entry *file, uint64_t pos, const void *data, size_t size, size_t *done) {
    *done=0; int e=writable(v); if (e) return e;
    if (file->attr&FAT_ATTR_DIR) return -FS_EISDIR;
    if (file->attr&FAT_ATTR_RO) return -FS_EACCES;
    if (!data && size) return -FS_EINVAL;
    if (pos>UINT32_MAX || size>UINT32_MAX-pos) return -FS_EFBIG;
    if (!size) return 0;
    uint32_t end=(uint32_t)(pos+size); if (end<file->size) end=file->size;
    e=replace(v,file,end,pos,data,size); if (!e) *done=size; return e;
}
int fat_truncate(struct fat_volume *v, struct fat_entry *file, uint64_t size) {
    int e=writable(v); if (e) return e;
    if (file->attr&FAT_ATTR_DIR) return -FS_EISDIR; if (file->attr&FAT_ATTR_RO) return -FS_EACCES;
    if (size>UINT32_MAX) return -FS_EFBIG;
    if (size==file->size) return 0;
    return replace(v,file,(uint32_t)size,0,0,0);
}
int fat_remove(struct fat_volume *v, struct fat_entry *file, bool directory) {
    int e=writable(v); if (e) return e;
    if (!!(file->attr&FAT_ATTR_DIR)!=directory) return directory ? -FS_ENOTDIR : -FS_EISDIR;
    if (file->attr&FAT_ATTR_RO) return -FS_EACCES;
    if (directory) {
        uint32_t i=0; struct fat_entry child;
        while (!(e=fat_next(v,file->first,&i,&child))) if (!path_equal(child.alias,".") && !path_equal(child.alias,"..")) return -FS_ENOTEMPTY;
        if (e!=-FS_ENOENT) return e;
    }
    if ((e=unlink_entry(v,file)) || (e=release_chain(v,file->first))) return e;
    return hints(v);
}
int fat_rename(struct fat_volume *v, struct fat_entry *file, uint32_t parent, const char *name) {
    int e=writable(v); if (e) return e;
    if (file->attr&FAT_ATTR_RO) return -FS_EACCES;
    uint16_t u[256]; unsigned n; if ((e=path_validate_name(name,u,&n))) return e;
    struct fat_entry dest; e=fat_lookup(v,parent,name,&dest);
    if (!e && !(parent==file->parent && dest.index==file->index)) return -FS_EEXIST;
    if (e && e!=-FS_ENOENT) return e;
    uint8_t alias[11]; if ((e=make_alias(v,parent,u,n,alias,file))) return e;
    dest=*file; dest.parent=parent; dest.lfn_count=(uint8_t)((n+12)/13);
    if (parent==file->parent) {
        /* Keep the owning short entry at the SAME index. Extend only into
         * contiguous deleted entries before it; never create two owners. */
        if (dest.index<dest.lfn_count) return -FS_ENOSPC;
        dest.lfn_index=dest.index-dest.lfn_count;
        for (uint32_t i=dest.lfn_index;i<file->lfn_index;i++) {
            uint8_t b[32]; if ((e=dir_read(v,parent,i,b))) return e;
            if (b[0]!=0xe5) return -FS_ENOSPC;
        }
        for (uint32_t i=file->lfn_index;i<dest.lfn_index;i++) {
            uint8_t b[32]; if ((e=dir_read(v,parent,i,b))) return e; b[0]=0xe5;
            if ((e=dir_write(v,parent,i,b))) return e;
        }
    } else {
        if ((e=slots(v,parent,dest.lfn_count+1,&dest.lfn_index))) return e;
        dest.index=dest.lfn_index+dest.lfn_count;
        if ((e=unlink_entry(v,file))) return e;
        if (file->attr&FAT_ATTR_DIR) {
            uint8_t b[32]; if ((e=dir_read(v,file->first,1,b))) return e;
            uint32_t first=(parent==v->root || !parent) ? 0 : parent;
            fs_wr16(b+26,(uint16_t)first); fs_wr16(b+20,v->type==32 ? (uint16_t)(first>>16) : 0);
            if ((e=dir_write(v,file->first,1,b)) || (e=barrier(v))) return e;
        }
    }
    if ((e=publish(v,&dest,u,n,alias))) return e;
    *file=dest; return hints(v);
}
int fat_set_metadata(struct fat_volume *v, struct fat_entry *file, uint8_t attr, const struct fat_times *times) {
    int e=writable(v); if (e) return e;
    if ((attr&~0x37u) || ((attr^file->attr)&FAT_ATTR_DIR)) return -FS_EINVAL;
    struct fat_entry changed=*file; changed.attr=attr; if (times) changed.times=*times;
    e=update(v,&changed); if (!e) *file=changed; return e;
}
static uint8_t *scan_byte(struct fat_volume *v, uint32_t c) { return cache_workspace_byte(v->cache,c); }
static int scan_chain(struct fat_volume *v, uint32_t first, uint32_t size, bool dir) {
    if (!first) return size || dir ? corrupt(v) : 0;
    uint32_t c=first, length=0;
    for (;length<v->clusters;length++) {
        if (!valid(v,c) || (*scan_byte(v,c)&1)) return corrupt(v);
        *scan_byte(v,c)|=1;
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return e;
        if (eoc(v,n)) {
            if ((uint64_t)(length+1)*v->spc*512<size) return corrupt(v);
            if (dir) *scan_byte(v,first)|=2;
            return 0;
        }
        if (!valid(v,n)) return corrupt(v);
        c=n;
    }
    return corrupt(v);
}
static int scan_dir(struct fat_volume *v, uint32_t dir) {
    uint32_t i=0; struct fat_entry file; int e;
    while (!(e=fat_next(v,dir,&i,&file))) {
        if (path_equal(file.alias,".") || path_equal(file.alias,"..")) continue;
        if (file.attr&0xc0) return corrupt(v);
        if ((e=scan_chain(v,file.first,file.size,(file.attr&FAT_ATTR_DIR)!=0))) return e;
    }
    return e==-FS_ENOENT ? 0 : e;
}
int fat_scan(struct fat_volume *v) {
    int e=cache_workspace_begin(v->cache,v->clusters+2); if (e) { v->diagnostic="cache scan workspace too small"; return e; }
    v->lost_clusters=0; v->free_clusters=0; v->next_free=2;
    if (v->type==32) e=scan_chain(v,v->root,0,true); else e=scan_dir(v,0);
    if (e) goto out;
    bool more=true;
    while (more) {
        more=false;
        for (uint32_t c=2;c<v->clusters+2;c++) {
            if (!(c&127)) fs_service();
            if ((*scan_byte(v,c)&6)==2) {
                *scan_byte(v,c)|=4; more=true; if ((e=scan_dir(v,c))) goto out;
            }
        }
    }
    for (uint32_t c=2;c<v->clusters+2;c++) {
        uint32_t val; if ((e=fat_get_cluster(v,c,&val))) goto out;
        if (!val) { if (!v->free_clusters) v->next_free=c; v->free_clusters++; }
        else if (val!=mask(v)-8 && !(*scan_byte(v,c)&1)) v->lost_clusters++;
    }
    if (v->lost_clusters) { v->readonly=true; v->ro_reasons|=FAT_RO_LOST; v->diagnostic="lost allocated clusters"; }
out:
    cache_workspace_end(v->cache); return e;
}
static int reject(struct fat_volume *v, const char *why) { v->diagnostic=why; v->mounted=false; return -FS_EINVAL; }
int fat_mount(struct fat_volume *v, struct block_cache *cache, struct blkdev *dev, uint64_t start, uint64_t sectors, bool wr) {
    memset(v,0,sizeof(*v)); v->cache=cache; v->dev=dev; v->start=start; v->sectors=sectors;
    if (!dev || dev->sector_size!=512 || !sectors || start>=dev->capacity || sectors>dev->capacity-start) return reject(v,"device/partition bounds");
    uint8_t b[512], other[512]; int e=readsec(v,0,b); if (e) return e;
    if (fs_rd16(b+510)!=0xaa55 || fs_rd16(b+11)!=512 || (b[0]!=0xe9 && !(b[0]==0xeb && b[2]==0x90))) return reject(v,"BPB signature/sector size/jump");
    v->spc=b[13]; v->reserved=fs_rd16(b+14); v->fats=b[16]; v->root_entries=fs_rd16(b+17); v->media=b[21];
    uint32_t total=fs_rd16(b+19); if (!total) total=fs_rd32(b+32);
    uint32_t fat16=fs_rd16(b+22); v->fat_sectors=fat16 ? fat16 : fs_rd32(b+36);
    if (!v->spc || (v->spc&(v->spc-1)) || v->spc>128 || !v->reserved || !v->fats || v->fats>2 || !v->fat_sectors || !total || total>sectors || (v->media!=0xf0 && v->media<0xf8)) return reject(v,"BPB geometry");
    uint64_t roots=((uint64_t)v->root_entries*32+511)/512;
    uint64_t data=(uint64_t)v->reserved+(uint64_t)v->fats*v->fat_sectors+roots;
    if (data>=total) return reject(v,"BPB extent overflow");
    v->sectors=total; v->root_sector=v->reserved+v->fats*v->fat_sectors; v->data_sector=(uint32_t)data;
    v->clusters=(total-v->data_sector)/v->spc;
    v->type=v->clusters<4085 ? 12 : v->clusters<65525 ? 16 : 32;
    if (!v->clusters || v->clusters>=0x0ffffff5 || ((uint64_t)(v->clusters+2)*v->type+7)/8>(uint64_t)v->fat_sectors*512) return reject(v,"FAT capacity/cluster count");
    if ((v->type==32 && (fat16 || v->root_entries || fs_rd16(b+19))) || (v->type!=32 && (!fat16 || !v->root_entries || (v->root_entries%16)))) return reject(v,"cluster type contradicts BPB layout");
    if (v->type==32) {
        v->root=fs_rd32(b+44); v->fsinfo=fs_rd16(b+48); v->backup=fs_rd16(b+50);
        uint16_t flags=fs_rd16(b+40);
        if (!valid(v,v->root) || fs_rd16(b+42) || !v->fsinfo || v->fsinfo>=v->reserved || (v->backup!=0 && v->backup!=0xffff && (v->backup>=v->reserved || v->backup==v->fsinfo)) || (flags&0xff70)) return reject(v,"FAT32 root/version/reserved geometry");
        if (flags&0x80) {
            v->active_fat=(uint8_t)(flags&15); if (v->active_fat>=v->fats) return reject(v,"active FAT index");
            v->ro_reasons|=FAT_RO_ACTIVE_ONLY;
        } else if (flags&15) return reject(v,"mirrored FAT active bits");
        if ((e=readsec(v,v->fsinfo,other))) return e;
        if (fs_rd32(other)!=0x41615252 || fs_rd32(other+484)!=0x61417272 || fs_rd32(other+508)!=0xaa550000) return reject(v,"FSInfo signatures");
        if (v->backup && v->backup!=0xffff) {
            if ((e=readsec(v,v->backup,other))) return e;
            if (memcmp(b+11,other+11,79) || fs_rd16(other+510)!=0xaa55) v->ro_reasons|=FAT_RO_CORRUPT;
        }
    }
    uint32_t zero,one;
    if ((e=fat_get_cluster(v,0,&zero)) || (e=fat_get_cluster(v,1,&one))) return e;
    if (zero!=((mask(v)&~255u)|v->media) || (v->type==12 ? !eoc(v,one) : (one|(cleanbit(v)|errorbit(v)))!=mask(v))) return reject(v,"FAT reserved entries/media/flags");
    if (cleanbit(v) && !(one&cleanbit(v))) v->ro_reasons|=FAT_RO_DIRTY;
    if (errorbit(v) && !(one&errorbit(v))) v->ro_reasons|=FAT_RO_IO_FLAG;
    for (uint32_t s=0;s<v->fat_sectors && v->fats==2;s++) {
        if ((e=readsec(v,v->reserved+s,b)) || (e=readsec(v,v->reserved+v->fat_sectors+s,other))) return e;
        if (memcmp(b,other,512)) v->ro_reasons|=FAT_RO_COPIES;
    }
    if (!wr) v->ro_reasons|=FAT_RO_REQUEST;
    if (!blkdev_durable(dev)) v->ro_reasons|=FAT_RO_DURABILITY;
    v->readonly=v->ro_reasons!=0; v->mounted=true;
    e=fat_scan(v);
    if (e && e!=-FS_EUCLEAN) { v->mounted=false; return e; }
    if (v->ro_reasons) v->readonly=true;
    v->diagnostic=v->readonly ? "read-only; inspect ro_reasons" : "validated writable mount";
    if (!v->readonly) {
        v->writable_session=true;
        if (cleanbit(v) && ((e=set_cluster(v,1,one&~cleanbit(v))) || (e=barrier(v)))) return e;
    }
    return 0;
}
int fat_enable_write(struct fat_volume *v) {
    if (!v || !v->mounted) return -FS_EINVAL;
    int e=cache_error(v->cache,v->dev); if (e) return write_error(v,e);
    if (!v->readonly) return 0;
    if (!blkdev_durable(v->dev)) { v->ro_reasons|=FAT_RO_DURABILITY; return -FS_EROFS; }
    if (v->ro_reasons&~(FAT_RO_REQUEST|FAT_RO_DURABILITY)) return -FS_EROFS;
    uint32_t one;
    if ((e=fat_get_cluster(v,1,&one))) return e;
    v->ro_reasons=0; v->readonly=false; v->writable_session=true;
    if (cleanbit(v) && ((e=set_cluster(v,1,one&~cleanbit(v))) || (e=barrier(v)))) return e;
    v->diagnostic="read gate passed; writable";
    return 0;
}
int fat_commit(struct fat_volume *v) {
    if (!v->mounted) return -FS_EINVAL;
    int e=cache_error(v->cache,v->dev); if (e) return write_error(v,e);
    if (v->readonly) return v->writable_session ? -FS_EROFS : 0;
    return barrier(v);
}
int fat_unmount(struct fat_volume *v) {
    int e=fat_commit(v); if (e) return e;
    if (v->writable_session && !v->readonly && cleanbit(v)) {
        uint32_t one; if ((e=fat_get_cluster(v,1,&one))) return e;
        if ((e=set_cluster(v,1,one|cleanbit(v))) || (e=barrier(v))) return e;
    }
    v->mounted=false; return 0;
}
