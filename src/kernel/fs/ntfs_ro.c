/* Independent Ciuki NTFS read parser. No upstream code imported.
 * Layout/behavior references and decisions: docs/design/f4-acceptance.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/ntfs_ro.h>
#include <stdbool.h>

#define REC_MAX 4096u
#define INDEX_MAX 16384u
#define LIST_MAX 16384u
#define RUN_MAX 256u
#define ATTR_MAX 256u
#define EXT_MAX 128u
#define BLOCK_MAX 4096u
#define ENTRY_MAX 65536u
#define DEPTH_MAX 16u
#define LOG_MAX (64u*1024u*1024u)
#define LOG_RECOVERY (-1001)
#define LOG_HIBERNATED (-1002)
#define REF_MASK UINT64_C(0x0000ffffffffffff)
#define SPARSE UINT64_MAX
#define IO (-NTFS_RO_EIO)
#define UNSUP (-NTFS_RO_EOPNOTSUPP)
#define TRY(x) do { int err_ = (x); if (err_) return err_; } while (0)

struct run { uint64_t vcn, lcn, length; };
struct stream {
    uint64_t size, initialized, coverage, allocated;
    uint32_t count, resident, present;
    struct run runs[RUN_MAX];
    uint8_t value[REC_MAX];
};
struct frame { uint64_t vcn; uint32_t pos, descended; };
struct ntfs_ro {
    ntfs_ro_read_blocks read;
    void *opaque;
    uint64_t sectors, clusters, mft_lcn, mirror_lcn, root_ref;
    uint32_t sector, cluster, sector_shift, cluster_shift, record_size, index_size;
    bool mounted;
    struct stream mft, data, allocation, bitmap, alist;
    uint8_t record[REC_MAX], extension[REC_MAX], list[LIST_MAX];
    uint8_t root[REC_MAX], index[INDEX_MAX], sector_buf[4096];
    uint8_t upcase[131072], visited[BLOCK_MAX / 8];
    struct frame frames[DEPTH_MAX];
    struct ntfs_ro_entry entry;
};

static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p+2) << 16; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p+4) << 32; }
static void copy(void *d, const void *s, size_t n)
{ uint8_t *a=d; const uint8_t *b=s; while (n--) *a++=*b++; }
static void zero(void *d, size_t n) { uint8_t *p=d; while (n--) *p++=0; }
static bool equal(const void *a, const void *b, size_t n)
{ const uint8_t *p=a,*q=b; while (n--) if (*p++ != *q++) return false; return true; }
static bool power(uint32_t n) { return n && !(n & (n-1)); }
static uint32_t shift(uint32_t n) { uint32_t s=0; while (n > 1) { n >>= 1; s++; } return s; }
static bool range(uint64_t off, uint64_t len, uint64_t end)
{ return off <= end && len <= end-off; }

static int blocks(struct ntfs_ro *v, uint64_t lba, uint32_t n, void *buf)
{
    if (!n || !range(lba,n,v->sectors)) return IO;
    return v->read(v->opaque,lba,n,buf) ? IO : 0;
}
static int disk_bytes(struct ntfs_ro *v, uint64_t off, uint8_t *buf, size_t n)
{
    if (!range(off,n,v->sectors << v->sector_shift)) return IO;
    while (n) {
        uint32_t within=(uint32_t)(off & (v->sector-1));
        uint32_t take=v->sector-within;
        if (take > n) take=(uint32_t)n;
        if (!within && take==v->sector) TRY(blocks(v,off >> v->sector_shift,1,buf));
        else {
            TRY(blocks(v,off >> v->sector_shift,1,v->sector_buf));
            copy(buf,v->sector_buf+within,take);
        }
        off+=take; buf+=take; n-=take;
    }
    return 0;
}
/* NTFS multi-sector transfer protection uses 512-byte strides even when
 * the device's logical sector is larger. All trailers are checked first. */
static int fixup(uint8_t *p, uint32_t size, const char *magic)
{
    if (!equal(p,magic,4) || size < 512 || size % 512) return IO;
    uint32_t off=u16(p+4), count=u16(p+6);
    uint32_t minimum=equal(magic,"FILE",4) ? 42 : equal(magic,"INDX",4) ? 40 : 30;
    if (count != size/512+1 || off < minimum || off & 1 || !range(off,count*2,512-2)) return IO;
    uint16_t mark=u16(p+off);
    if (!mark || mark==0xffff) return IO;
    for (uint32_t i=1;i<count;i++) if (u16(p+i*512-2)!=mark) return IO;
    for (uint32_t i=1;i<count;i++) copy(p+i*512-2,p+off+i*2,2);
    return 0;
}
static int record_header(struct ntfs_ro *v, uint8_t *p, uint64_t ref, bool check_seq)
{
    TRY(fixup(p,v->record_size,"FILE"));
    uint32_t used=u32(p+24), attrs=u16(p+20);
    if (!(u16(p+22)&1) || !u16(p+16) || used > v->record_size ||
        u32(p+28)!=v->record_size || attrs < 42 || attrs & 7 ||
        attrs < u16(p+4)+u16(p+6)*2u || !range(attrs,4,used)) return IO;
    if (check_seq && u16(p+16)!=(uint16_t)(ref >> 48)) return IO;
    if (u16(p+4)>=48 && u32(p+44)!=(uint32_t)(ref&REF_MASK)) return IO;
    return 0;
}
static int stream_read(struct ntfs_ro *v, const struct stream *s, uint64_t off,
                       void *buf, size_t n)
{
    uint8_t *p=buf;
    if (!s->present || !range(off,n,s->size)) return IO;
    if (s->resident) { copy(p,s->value+(size_t)off,n); return 0; }
    while (n) {
        if (off >= s->initialized) { zero(p,n); return 0; }
        uint64_t vcn=off >> v->cluster_shift;
        const struct run *r=0;
        for (uint32_t i=0;i<s->count;i++)
            if (vcn>=s->runs[i].vcn && vcn-s->runs[i].vcn<s->runs[i].length) { r=&s->runs[i]; break; }
        if (!r) return IO;
        uint64_t within=off & (v->cluster-1);
        uint64_t available=((r->length-(vcn-r->vcn)) << v->cluster_shift)-within;
        if (available > s->initialized-off) available=s->initialized-off;
        size_t take=n; if (available < take) take=(size_t)available;
        if (!take) return IO;
        if (r->lcn==SPARSE) zero(p,take);
        else TRY(disk_bytes(v,((r->lcn+vcn-r->vcn) << v->cluster_shift)+within,p,take));
        off+=take; p+=take; n-=take;
    }
    return 0;
}
static int record_read(struct ntfs_ro *v, uint64_t ref, uint8_t *p, bool check_seq)
{
    uint64_t number=ref & REF_MASK;
    if (number > UINT64_MAX/v->record_size) return IO;
    TRY(stream_read(v,&v->mft,number*v->record_size,p,v->record_size));
    return record_header(v,p,ref,check_seq);
}

/* Validate the whole attribute envelope before any value is interpreted. */
static int attribute(const uint8_t *p, uint32_t used, uint32_t off, const uint8_t **out)
{
    if (!range(off,4,used)) return IO;
    if (u32(p+off)==0xffffffff) { *out=0; return 0; }
    if (!range(off,24,used)) return IO;
    const uint8_t *a=p+off;
    uint32_t len=u32(a+4), head=a[8] ? 64 : 24;
    if (a[8]>1 || len<head || len&7 || !range(off,len,used)) return IO;
    uint32_t name=u16(a+10), names=a[9]*2u;
    if (names && (name<head || name&1 || !range(name,names,len))) return IO;
    if (a[8]) {
        uint32_t ro=u16(a+32);
        if (ro<head || ro>=len || (names && ro<name+names)) return IO;
    } else {
        uint32_t vo=u16(a+20), vl=u32(a+16);
        if (vo<head || !range(vo,vl,len) || (names && vo<name+names)) return IO;
    }
    *out=a; return 0;
}
static bool named(const uint8_t *a, const char *name)
{
    uint32_t i=0;
    while (name[i]) i++;
    if (i!=a[9]) return false;
    for (uint32_t j=0;j<i;j++) if (u16(a+u16(a+10)+j*2)!=(uint8_t)name[j]) return false;
    return true;
}
static int find_attr(const uint8_t *p, uint32_t type, const char *name,
                     uint16_t id, bool by_id, const uint8_t **found)
{
    uint32_t used=u32(p+24), off=u16(p+20); *found=0;
    for (uint32_t i=0;i<ATTR_MAX;i++) {
        const uint8_t *a; TRY(attribute(p,used,off,&a));
        if (!a) return 0;
        if (u32(a)==type && named(a,name) && (!by_id || u16(a+14)==id)) {
            if (*found) return IO;
            *found=a;
        }
        off+=u32(a+4);
    }
    return UNSUP;
}
static uint64_t packed(const uint8_t *p, uint32_t n)
{ uint64_t x=0; for (uint32_t i=0;i<n;i++) x |= (uint64_t)p[i] << (8*i); return x; }
static int segment(struct ntfs_ro *v, struct stream *s, const uint8_t *a)
{
    uint16_t flags=u16(a+12);
    if (flags & 0x40ff) return UNSUP;
    if (flags & ~0x8000u) return UNSUP;
    if (!a[8]) {
        if (s->present || flags) return IO;
        s->size=s->initialized=u32(a+16); s->resident=s->present=1;
        if (s->size > sizeof(s->value)) return UNSUP;
        copy(s->value,a+u16(a+20),(size_t)s->size); return 0;
    }
    /* ntfs-3g records c_unit=4 for ordinary sparse streams too; this is
     * not compression when ATTR_FLAG_COMPRESSED is clear. */
    if (a[34] && (!(flags&0x8000) || a[34]!=4)) return UNSUP;
    uint64_t first=u64(a+16), last=u64(a+24);
    if (s->resident || first!=s->coverage) return IO;
    if (!s->present) {
        if (first) return IO;
        s->size=u64(a+48); s->initialized=u64(a+56); s->allocated=u64(a+40);
        if (s->size>INT64_MAX || s->initialized>s->size || s->allocated & (v->cluster-1)) return IO;
        if (!(flags&0x8000) && s->size>s->allocated) return IO;
        s->present=1;
    }
    if (last==UINT64_MAX && !first && !s->size && !s->allocated) return 0;
    if (last<first || last>=((uint64_t)INT64_MAX >> v->cluster_shift)) return IO;
    uint32_t pos=u16(a+32), end=u32(a+4);
    uint64_t vcn=first, prev=0;
    bool terminated=false;
    while (pos<end) {
        uint8_t h=a[pos++]; if (!h) { terminated=true; break; }
        uint32_t ln=h&15, dn=h>>4;
        if (!ln || ln>8 || dn>8 || !range(pos,ln+dn,end)) return IO;
        uint64_t len=packed(a+pos,ln); pos+=ln;
        if (!len || !range(vcn,len,last+1)) return IO;
        uint64_t lcn=SPARSE;
        if (dn) {
            uint64_t delta=packed(a+pos,dn);
            if (a[pos+dn-1]&0x80) {
                if (dn<8) delta |= UINT64_MAX << (dn*8);
                uint64_t magnitude=~delta+1;
                if (magnitude>prev) return IO;
                lcn=prev-magnitude;
            } else {
                if (delta>UINT64_MAX-prev) return IO;
                lcn=prev+delta;
            }
            if (!range(lcn,len,v->clusters)) return IO;
            prev=lcn;
        } else if (!(flags&0x8000)) return IO;
        pos+=dn;
        if (s->count==RUN_MAX) return UNSUP;
        s->runs[s->count++]=(struct run){vcn,lcn,len}; vcn+=len;
    }
    if (!terminated || vcn!=last+1) return IO;
    s->coverage=vcn;
    return 0;
}
static int finish_stream(struct ntfs_ro *v, const struct stream *s)
{
    if (!s->present) return -NTFS_RO_ENOENT;
    if (!s->resident && (s->coverage < ((s->size+v->cluster-1) >> v->cluster_shift) ||
        (s->coverage << v->cluster_shift)!=s->allocated)) return IO;
    return 0;
}

static int build_stream(struct ntfs_ro *v, uint64_t ref, uint32_t type,
                        const char *name, struct stream *s)
{
    zero(s,sizeof(*s));
    TRY(record_read(v,ref,v->record,true));
    if (u64(v->record+32)) return IO; /* public nodes must be base records */
    const uint8_t *list;
    TRY(find_attr(v->record,0x20,"",0,false,&list));
    if (!list) {
        uint32_t off=u16(v->record+20), used=u32(v->record+24);
        for (uint32_t i=0;i<ATTR_MAX;i++) {
            const uint8_t *a; TRY(attribute(v->record,used,off,&a));
            if (!a) return finish_stream(v,s);
            if (u32(a)==type && named(a,name)) TRY(segment(v,s,a));
            off+=u32(a+4);
        }
        return UNSUP;
    }
    if (s==&v->alist) return UNSUP;
    zero(&v->alist,sizeof(v->alist)); TRY(segment(v,&v->alist,list));
    TRY(finish_stream(v,&v->alist));
    if (v->alist.size>LIST_MAX || v->alist.initialized!=v->alist.size) return UNSUP;
    TRY(stream_read(v,&v->alist,0,v->list,(size_t)v->alist.size));
    uint32_t off=0, selected=0, iterations=0;
    while (off<v->alist.size) {
        if (++iterations>512) return UNSUP;
        if (!range(off,26,v->alist.size)) return IO;
        const uint8_t *le=v->list+off;
        uint32_t len=u16(le+4), nl=le[6], no=le[7];
        if (len<26 || len&7 || !range(off,len,v->alist.size) ||
            (nl && (no<26 || !range(no,nl*2,len)))) return IO;
        bool match=u32(le)==type;
        uint32_t name_len=0; while (name[name_len]) name_len++;
        if (nl!=name_len) match=false;
        if (match) for (uint32_t i=0;i<nl;i++)
            if (u16(le+no+i*2)!=(uint8_t)name[i]) match=false;
        if (match) {
            if (++selected>EXT_MAX) return UNSUP;
            uint64_t ext=u64(le+16);
            uint8_t *p=v->extension;
            TRY(record_read(v,ext,p,true));
            if (ext==ref) { if (u64(p+32)) return IO; }
            else {
                if (u64(p+32)!=ref) return IO;
                const uint8_t *nested; TRY(find_attr(p,0x20,"",0,false,&nested));
                if (nested) return IO;
            }
            const uint8_t *a; TRY(find_attr(p,type,name,u16(le+24),true,&a));
            if (!a || (a[8] ? u64(a+16) : 0)!=u64(le+8)) return IO;
            TRY(segment(v,s,a));
            /* $MFT extension records can live in a later extent described
             * by an earlier list entry. Grow the proved bootstrap prefix
             * before reading the next reference, never guessing an LCN. */
            if (!(ref&REF_MASK) && type==0x80 && !name[0]) copy(&v->mft,s,sizeof(*s));
        }
        off+=len;
    }
    return finish_stream(v,s);
}

static int node_info(struct ntfs_ro *v, uint64_t ref, struct ntfs_ro_node *n)
{
    TRY(record_read(v,ref,v->record,true));
    if (u64(v->record+32)) return IO;
    uint32_t off=u16(v->record+20), used=u32(v->record+24);
    bool directory=(u16(v->record+22)&2)!=0, ended=false;
    for (uint32_t i=0;i<ATTR_MAX;i++) {
        const uint8_t *a; TRY(attribute(v->record,used,off,&a));
        if (!a) { ended=true; break; }
        if (u32(a)==0xc0) return UNSUP;
        if (u32(a)==0x10) {
            if (a[8] || u32(a+16)<36) return IO;
            uint32_t flags=u32(a+u16(a+20)+32);
            if (flags&(0x400|0x4000) || (!directory && (flags&0x800))) return UNSUP;
        }
        off+=u32(a+4);
    }
    if (!ended) return UNSUP;
    uint64_t size=0;
    if (!directory) { TRY(build_stream(v,ref,0x80,"",&v->data)); size=v->data.size; }
    *n=(struct ntfs_ro_node){ref,size,directory}; return 0;
}

static int utf8_name(const uint8_t *p, uint32_t units, char *out)
{
    uint32_t j=0;
    for (uint32_t i=0;i<units;i++) {
        uint32_t c=u16(p+i*2);
        if (!c || c=='/' || c=='\\') return IO;
        if (c>=0xd800 && c<=0xdbff) {
            if (++i>=units) return IO;
            uint32_t lo=u16(p+i*2); if (lo<0xdc00 || lo>0xdfff) return IO;
            c=0x10000+((c-0xd800)<<10)+lo-0xdc00;
        } else if (c>=0xdc00 && c<=0xdfff) return IO;
        if (c<0x80) out[j++]=(char)c;
        else if (c<0x800) { out[j++]=(char)(0xc0|(c>>6)); out[j++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) {
            out[j++]=(char)(0xe0|(c>>12)); out[j++]=(char)(0x80|((c>>6)&63)); out[j++]=(char)(0x80|(c&63));
        } else {
            out[j++]=(char)(0xf0|(c>>18)); out[j++]=(char)(0x80|((c>>12)&63));
            out[j++]=(char)(0x80|((c>>6)&63)); out[j++]=(char)(0x80|(c&63));
        }
    }
    out[j]=0; return 0;
}
static int component(const char *name, uint16_t *out, uint32_t *units)
{
    const uint8_t *p=(const uint8_t *)name; uint32_t n=0, bytes=0;
    while (*p) {
        if (++bytes>1020) return -NTFS_RO_ENAMETOOLONG;
        uint32_t c=*p++, need=0, min=0;
        if (c>=0xc2 && c<=0xdf) { c&=31; need=1; min=0x80; }
        else if (c>=0xe0 && c<=0xef) { c&=15; need=2; min=0x800; }
        else if (c>=0xf0 && c<=0xf4) { c&=7; need=3; min=0x10000; }
        else if (c>=0x80) return -NTFS_RO_EILSEQ;
        for (uint32_t i=0;i<need;i++) {
            if ((*p&0xc0)!=0x80) return -NTFS_RO_EILSEQ;
            c=(c<<6)|(*p++&63); bytes++;
        }
        if (c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) return -NTFS_RO_EILSEQ;
        if (c==':') return UNSUP;
        if (c=='/' || c=='\\') return -NTFS_RO_EINVAL;
        if (n+(c>=0x10000 ? 2u : 1u)>255) return -NTFS_RO_ENAMETOOLONG;
        if (c>=0x10000) { c-=0x10000; out[n++]=(uint16_t)(0xd800+(c>>10)); out[n++]=(uint16_t)(0xdc00+(c&1023)); }
        else out[n++]=(uint16_t)c;
    }
    if (!n) return -NTFS_RO_EINVAL;
    *units=n; return 0;
}

static int index_header(const uint8_t *h, uint32_t bytes, uint32_t *start, uint32_t *end)
{
    if (bytes<16) return IO;
    *start=u32(h); *end=u32(h+4);
    if (*start<16 || *start&7 || *end<*start || *end&7 ||
        u32(h+8)>bytes || *end>u32(h+8) || u32(h+12)>1 || !range(*start,16,*end)) return IO;
    return 0;
}
static int directory_walk(struct ntfs_ro *v, uint64_t ref, ntfs_ro_emit emit, void *opaque)
{
    TRY(record_read(v,ref,v->record,true));
    if (!(u16(v->record+22)&2)) return -NTFS_RO_ENOTDIR;
    TRY(build_stream(v,ref,0x90,"$I30",&v->data));
    if (!v->data.resident || v->data.size<32 || v->data.size>REC_MAX) return IO;
    uint32_t root_size=(uint32_t)v->data.size;
    copy(v->root,v->data.value,root_size);
    if (u32(v->root)!=0x30 || u32(v->root+4)!=1 || u32(v->root+8)!=v->index_size) return IO;
    uint32_t unit=v->index_size<v->cluster ? 512 : v->cluster;
    if ((uint32_t)v->root[12]*unit!=v->index_size) return IO;
    int alloc=build_stream(v,ref,0xa0,"$I30",&v->allocation);
    int bitmap=build_stream(v,ref,0xb0,"$I30",&v->bitmap);
    if (alloc && alloc!=-NTFS_RO_ENOENT) return alloc;
    if (bitmap && bitmap!=-NTFS_RO_ENOENT) return bitmap;
    if (!!alloc!=!!bitmap || (v->root[28] && alloc)) return IO;
    uint64_t block_count=0;
    if (!alloc) {
        if (v->allocation.resident || v->allocation.size & (v->index_size-1)) return IO;
        block_count=v->allocation.size >> shift(v->index_size);
        if (block_count>BLOCK_MAX || v->bitmap.size>(BLOCK_MAX/8)) return UNSUP;
        if (v->bitmap.size*8<block_count || v->bitmap.initialized!=v->bitmap.size ||
            v->allocation.initialized!=v->allocation.size) return IO;
    }
    zero(v->visited,sizeof(v->visited));
    v->frames[0]=(struct frame){UINT64_MAX,0,0};
    uint32_t depth=0, entries=0, visited=0;
    for (;;) {
        struct frame *f=&v->frames[depth];
        uint8_t *h; uint32_t bytes;
        if (!depth) { h=v->root+16; bytes=root_size-16; }
        else {
            uint64_t off=f->vcn << shift(unit);
            TRY(stream_read(v,&v->allocation,off,v->index,v->index_size));
            TRY(fixup(v->index,v->index_size,"INDX"));
            if (u64(v->index+16)!=f->vcn) return IO;
            h=v->index+24; bytes=v->index_size-24;
        }
        uint32_t start,end; TRY(index_header(h,bytes,&start,&end));
        if (depth && start+24 < u16(v->index+4)+u16(v->index+6)*2u) return IO;
        if (!f->pos) f->pos=start;
        if (!range(f->pos,16,end)) return IO;
        const uint8_t *e=h+f->pos;
        uint32_t len=u16(e+8), key=u16(e+10), flags=u16(e+12);
        if (len<16 || len&7 || !range(f->pos,len,end) || flags&~3u ||
            len<16+((flags&1)?8u:0u) || key>len-16-((flags&1)?8u:0u) ||
            ((flags&1)!=0)!=(u32(h+12)!=0)) return IO;
        if ((flags&1) && !f->descended) {
            if (alloc || depth+1>=DEPTH_MAX) return alloc ? IO : UNSUP;
            uint64_t child=u64(e+len-8);
            if (child>UINT64_MAX/unit) return IO;
            uint64_t off=child*unit;
            if (off & (v->index_size-1)) return IO;
            uint64_t bit=off >> shift(v->index_size);
            if (bit>=block_count) return IO;
            uint8_t b; TRY(stream_read(v,&v->bitmap,bit>>3,&b,1));
            if (!(b&(1u<<(bit&7))) || v->visited[bit>>3]&(1u<<(bit&7))) return IO;
            v->visited[bit>>3] |= (uint8_t)(1u<<(bit&7));
            if (++visited>BLOCK_MAX) return UNSUP;
            f->descended=1; v->frames[++depth]=(struct frame){child,0,0}; continue;
        }
        if (++entries>ENTRY_MAX) return UNSUP;
        if (flags&2) {
            if (key || f->pos+len!=end) return IO;
            if (!depth) break;
            depth--; continue;
        }
        if (key<66) return IO;
        const uint8_t *fn=e+16; uint32_t units=fn[64];
        if (!units || fn[65]>3 || 66+units*2!=key || u64(fn)!=ref) return IO;
        uint64_t node_ref=u64(e);
        /* Sequence checking is mandatory even for unsupported content. */
        TRY(record_read(v,node_ref,v->record,true));
        if (u64(v->record+32)) return IO;
        v->entry.reference=node_ref; v->entry.size=u64(fn+48);
        v->entry.directory=(u16(v->record+22)&2)!=0; v->entry.name_space=fn[65];
        TRY(utf8_name(fn+66,units,v->entry.name));
        if (emit) { int err=emit(opaque,&v->entry); if (err) return err<0 ? err : -NTFS_RO_EINVAL; }
        f->pos+=len; f->descended=0;
    }
    if (!bitmap) {
        /* Unreachable allocated blocks and padding bits cannot hide entries. */
        for (uint64_t i=0;i<v->bitmap.size;i++) {
            uint8_t b; TRY(stream_read(v,&v->bitmap,i,&b,1));
            if (b!=v->visited[i]) return IO;
        }
    }
    return 0;
}

struct lookup_state { struct ntfs_ro *v; const uint16_t *name; uint32_t units; uint64_t ref; };
static int lookup_emit(void *opaque, const struct ntfs_ro_entry *e)
{
    struct lookup_state *s=opaque;
    uint16_t name[255]; uint32_t units;
    TRY(component(e->name,name,&units));
    if (units!=s->units) return 0;
    for (uint32_t i=0;i<units;i++)
        if (u16(s->v->upcase+(uint32_t)name[i]*2)!=u16(s->v->upcase+(uint32_t)s->name[i]*2)) return 0;
    if (s->ref && s->ref!=e->reference) return IO; /* ambiguous collation */
    s->ref=e->reference; return 0;
}
static int lookup_ref(struct ntfs_ro *v, uint64_t dir, const char *name, uint64_t *ref)
{
    uint16_t units[255]; uint32_t count;
    TRY(component(name,units,&count));
    struct lookup_state state={v,units,count,0};
    TRY(directory_walk(v,dir,lookup_emit,&state));
    if (!state.ref) return -NTFS_RO_ENOENT;
    *ref=state.ref; return 0;
}

static int log_clean(struct ntfs_ro *v)
{
    TRY(record_read(v,2,v->record,false));
    uint64_t ref=2 | (uint64_t)u16(v->record+16)<<48;
    TRY(build_stream(v,ref,0x80,"",&v->data));
    if (v->data.size<8192 || v->data.initialized<v->data.size) return IO;
    /* mkntfs creates a deliberately empty log (all FF). Otherwise require
     * both supported restart pages to prove clean; never select a salvage copy. */
    TRY(stream_read(v,&v->data,0,v->index,4096));
    bool empty=true; for (uint32_t i=0;i<4096;i++) if (v->index[i]!=0xff) empty=false;
    if (empty) {
        if (v->data.size>LOG_MAX) return UNSUP;
        for (uint64_t off=4096;off<v->data.size;off+=4096) {
            size_t n=v->data.size-off<4096 ? (size_t)(v->data.size-off) : 4096;
            TRY(stream_read(v,&v->data,off,v->index,n));
            for (size_t i=0;i<n;i++) if (v->index[i]!=0xff) return IO;
        }
        return 0;
    }
    uint32_t page=u32(v->index+16);
    if (!power(page) || page<512 || page>INDEX_MAX) return UNSUP;
    for (uint32_t i=0;i<2;i++) {
        TRY(stream_read(v,&v->data,(uint64_t)i*page,v->index,page));
        TRY(fixup(v->index,page,"RSTR"));
        uint32_t ro=u16(v->index+24);
        /* Version 2.0 can retain Windows cached metadata (fast restart).
         * No read-only mount is exposed while that ownership is unresolved. */
        if (u16(v->index+28)==2 && !u16(v->index+26)) return LOG_HIBERNATED;
        if (u32(v->index+16)!=page || u32(v->index+20)!=page ||
            u16(v->index+28)!=1 || u16(v->index+26)>1 || ro<32 || ro&7 ||
            !range(ro,64,page) || u16(v->index+ro+20)<64 ||
            !range(ro,u16(v->index+ro+20),page)) return IO;
        if (u16(v->index+ro+12)!=0xffff && !(u16(v->index+ro+14)&2)) return LOG_RECOVERY;
    }
    return 0;
}

size_t ntfs_ro_workspace_size(void) { return sizeof(struct ntfs_ro); }
static int mount_impl(void *workspace, size_t bytes, ntfs_ro_read_blocks read, void *opaque,
                      uint32_t sector, uint64_t sectors, struct ntfs_ro **out,
                      enum ntfs_ro_reason *reason)
{
    if (out) *out=0;
    if (reason) *reason=NTFS_RO_INVALID;
    if (!out || !workspace || (uintptr_t)workspace % _Alignof(struct ntfs_ro) || !read ||
        !power(sector) || sector<512 || sector>4096 || !sectors || sectors>INT64_MAX/sector)
        return -NTFS_RO_EINVAL;
    if (bytes<sizeof(struct ntfs_ro)) return -NTFS_RO_ENOMEM;
    struct ntfs_ro *v=workspace; zero(v,sizeof(*v));
    v->read=read; v->opaque=opaque; v->sector=sector; v->sector_shift=shift(sector); v->sectors=sectors;
    int err=blocks(v,0,1,v->sector_buf); if (err) return err;
    const uint8_t *b=v->sector_buf;
    if (!equal(b+3,"NTFS    ",8) || u16(b+510)!=0xaa55 || u16(b+11)!=sector ||
        !power(b[13]) || (uint32_t)b[13]*sector>65536 ||
        u16(b+14) || b[16] || u16(b+17) || u16(b+19) || u16(b+22) || u32(b+32)) return IO;
    uint64_t advertised=u64(b+40);
    if (!advertised || advertised>sectors) return IO;
    v->sectors=advertised; v->cluster=b[13]*sector; v->cluster_shift=shift(v->cluster);
    v->clusters=advertised >> shift(b[13]);
    v->mft_lcn=u64(b+48); v->mirror_lcn=u64(b+56);
    if (v->mft_lcn>=v->clusters || v->mirror_lcn>=v->clusters) return IO;
    uint8_t rs=b[64], is=b[68];
    if (!rs || !is || (rs>=128 && (256u-rs)>30) || (is>=128 && (256u-is)>30)) return IO;
    v->record_size=rs<128 ? rs*v->cluster : 1u << (256u-rs);
    v->index_size=is<128 ? is*v->cluster : 1u << (256u-is);
    if (!power(v->record_size) || !power(v->index_size) || v->record_size<512 || v->index_size<512) return IO;
    if (v->record_size>REC_MAX || v->index_size>INDEX_MAX) { if (reason) *reason=NTFS_RO_UNSUPPORTED; return UNSUP; }
    err=disk_bytes(v,v->mft_lcn << v->cluster_shift,v->record,v->record_size);
    if (!err) err=record_header(v,v->record,0,false);
    if (err) return err;
    uint64_t mft_ref=(uint64_t)u16(v->record+16)<<48;
    const uint8_t *a;
    err=find_attr(v->record,0x80,"",0,false,&a);
    if (err || !a || !a[8] || u64(a+16)) return err ? err : IO;
    err=segment(v,&v->mft,a); if (err) return err;
    /* Bootstrap extensions using only the already mapped MFT prefix. */
    err=build_stream(v,mft_ref,0x80,"",&v->data);
    if (err) return err;
    copy(&v->mft,&v->data,sizeof(v->mft));
    if (v->mft.resident || v->mft.initialized!=v->mft.size) return IO;
    /* Never repair/fallback to the mirror; verify its first base record. */
    err=disk_bytes(v,v->mirror_lcn << v->cluster_shift,v->extension,v->record_size);
    if (!err) err=record_header(v,v->extension,mft_ref,true);
    if (err) return err;
    err=record_read(v,0,v->record,false); if (err) return err;
    if (!equal(v->record,v->extension,u32(v->record+24))) return IO;
    err=record_read(v,3,v->record,false); if (err) return err;
    err=find_attr(v->record,0x70,"",0,false,&a);
    if (err || !a || a[8] || u32(a+16)<12) return err ? err : IO;
    const uint8_t *info=a+u16(a+20);
    if (info[8]!=3 || info[9]>1) { if (reason) *reason=NTFS_RO_UNSUPPORTED; return UNSUP; }
    uint16_t flags=u16(info+10);
    if (flags&1) { if (reason) *reason=NTFS_RO_DIRTY; return IO; }
    if (flags&~8u) { if (reason) *reason=NTFS_RO_RECOVERY_REQUIRED; return IO; }
    err=log_clean(v);
    if (err) {
        if (reason) *reason=err==LOG_RECOVERY ? NTFS_RO_RECOVERY_REQUIRED :
                            err==LOG_HIBERNATED ? NTFS_RO_HIBERNATED : NTFS_RO_INVALID;
        return err==LOG_RECOVERY || err==LOG_HIBERNATED ? IO : err;
    }
    err=record_read(v,10,v->record,false); if (err) return err;
    uint64_t up_ref=10 | (uint64_t)u16(v->record+16)<<48;
    err=build_stream(v,up_ref,0x80,"",&v->data);
    if (err || v->data.size!=sizeof(v->upcase) || v->data.initialized!=v->data.size) return err ? err : IO;
    err=stream_read(v,&v->data,0,v->upcase,sizeof(v->upcase)); if (err) return err;
    for (uint32_t i=0;i<65536;i++) {
        uint16_t upper=u16(v->upcase+i*2);
        if (u16(v->upcase+(uint32_t)upper*2)!=upper) return IO;
    }
    err=record_read(v,5,v->record,false); if (err) return err;
    v->root_ref=5 | (uint64_t)u16(v->record+16)<<48;
    err=directory_walk(v,v->root_ref,0,0); if (err) return err;
    uint64_t hiber;
    err=lookup_ref(v,v->root_ref,"hiberfil.sys",&hiber);
    if (err && err!=-NTFS_RO_ENOENT) return err;
    if (!err) {
        err=build_stream(v,hiber,0x80,"",&v->data); if (err) return err;
        if (v->data.size<4096) { if (reason) *reason=NTFS_RO_HIBERNATED; return IO; }
        err=stream_read(v,&v->data,0,v->index,4096); if (err) return err;
        for (uint32_t i=0;i<4096;i++) if (v->index[i]) { if (reason) *reason=NTFS_RO_HIBERNATED; return IO; }
    }
    v->mounted=true; *out=v; if (reason) *reason=NTFS_RO_OK; return 0;
}
int ntfs_ro_mount(void *workspace, size_t bytes, ntfs_ro_read_blocks read, void *opaque,
                  uint32_t sector, uint64_t sectors, struct ntfs_ro **out,
                  enum ntfs_ro_reason *reason)
{
    int err=mount_impl(workspace,bytes,read,opaque,sector,sectors,out,reason);
    if (err==UNSUP && reason) *reason=NTFS_RO_UNSUPPORTED;
    return err;
}
void ntfs_ro_unmount(struct ntfs_ro *v) { if (v) v->mounted=false; }
int ntfs_ro_root(struct ntfs_ro *v, struct ntfs_ro_node *out)
{ if (!v || !v->mounted || !out) return -NTFS_RO_EINVAL; return node_info(v,v->root_ref,out); }
int ntfs_ro_open(struct ntfs_ro *v, uint64_t ref, struct ntfs_ro_node *out)
{ if (!v || !v->mounted || !out) return -NTFS_RO_EINVAL; return node_info(v,ref,out); }
int ntfs_ro_list(struct ntfs_ro *v, const struct ntfs_ro_node *dir, ntfs_ro_emit emit, void *opaque)
{
    if (!v || !v->mounted || !dir || !emit) return -NTFS_RO_EINVAL;
    return directory_walk(v,dir->reference,emit,opaque);
}
int ntfs_ro_lookup(struct ntfs_ro *v, const struct ntfs_ro_node *dir,
                   const char *name, struct ntfs_ro_node *out)
{
    if (!v || !v->mounted || !dir || !name || !out) return -NTFS_RO_EINVAL;
    uint64_t ref; TRY(lookup_ref(v,dir->reference,name,&ref)); return node_info(v,ref,out);
}
int ntfs_ro_pread(struct ntfs_ro *v, const struct ntfs_ro_node *n, uint64_t off,
                  void *buf, size_t size, size_t *read)
{
    if (read) *read=0;
    if (!v || !v->mounted || !n || !read || (!buf && size) || size>1048576) return -NTFS_RO_EINVAL;
    struct ntfs_ro_node actual; TRY(node_info(v,n->reference,&actual));
    if (actual.directory) return -NTFS_RO_EISDIR;
    if (off>=actual.size || !size) return 0;
    if (size>actual.size-off) size=(size_t)(actual.size-off);
    TRY(stream_read(v,&v->data,off,buf,size)); *read=size; return 0;
}
