/* SPDX-License-Identifier: MIT
 * WinHelp 3.1/4 topic text reader. Format research: Wine winhlp32/hlpfile.c.
 * All offsets use checked integer ranges; no cast of disk data to a struct. */
#include "helpfile.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct span { const uint8_t *p; size_t n; };
struct hlp {
    struct hf_document *d;
    unsigned version, block_size, dictionary;
    uint32_t *phrases; unsigned phrase_count; uint8_t *phrase_text; size_t phrase_size;
    uint8_t *topics; size_t topic_size; size_t *blocks; unsigned block_count;
};
static int internal(struct hf_document *d,uint32_t offset,struct span *s)
{
    if (!hf_range(d->input_size,offset,9)) return -EINVAL;
    const uint8_t *p=d->input+offset; uint32_t total=hf_u32(p),used=hf_u32(p+4);
    if (total<9 || used>total-9 || !hf_range(d->input_size,offset,total)) return -EINVAL;
    *s=(struct span){p+9,used}; return 0;
}
/* B+ tree enumeration starts at the leftmost leaf. The page budget detects
 * cycles both in the index descent and the linked leaf chain. */
typedef int (*leaf_fn)(struct span *,void *);
static int tree(struct span s,leaf_fn visit,void *ctx)
{
    if (s.n<38 || hf_u16(s.p)!=0x293b) return -EINVAL;
    unsigned size=hf_u16(s.p+4),page=hf_u16(s.p+26),levels=hf_u16(s.p+32);
    if (size<8 || !levels || levels>32) return -EINVAL;
    size_t pages=(s.n-38)/size; if (!pages || pages>65535) return -EINVAL;
    for (unsigned depth=1;depth<levels;depth++) {
        if (page>=pages) return -EINVAL;
        page=hf_u16(s.p+38+(size_t)page*size+4);
    }
    for (size_t visited=0;page!=0xffff;visited++) {
        if (visited>=pages || page>=pages) return -EINVAL;
        const uint8_t *p=s.p+38+(size_t)page*size;
        unsigned count=hf_u16(p+2),unused=hf_u16(p);
        if (unused>size-8) return -EINVAL;
        struct span leaf={p+8,size-8-unused};
        for (unsigned i=0;i<count;i++) { int e=visit(&leaf,ctx); if (e) return e; }
        page=hf_u16(p+6);
    }
    return 0;
}
struct directory { struct hf_document *d; const char *name; struct span *out; };
static int directory_entry(struct span *s,void *ctx)
{
    struct directory *c=ctx; const uint8_t *zero=memchr(s->p,0,s->n); if (!zero) return -EINVAL;
    size_t n=(size_t)(zero-s->p)+1; if (!hf_range(s->n,n,4)) return -EINVAL;
    int match=!strcmp((const char *)s->p,c->name);
    uint32_t offset=hf_u32(s->p+n); s->p+=n+4; s->n-=n+4;
    if (match) { int e=internal(c->d,offset,c->out); return e ? e : 1; }
    return 0;
}
static int subfile(struct hlp *h,const char *name,struct span *out)
{
    struct span dir; int e=internal(h->d,hf_u32(h->d->input+4),&dir); if (e) return e;
    struct directory c={h->d,name,out}; e=tree(dir,directory_entry,&c);
    if (e==0 && name[0]=='|') { c.name=name+1; e=tree(dir,directory_entry,&c); }
    return e==1 ? 0 : e ? e : -ENOENT;
}
static int lz77(struct span in,uint8_t *out,size_t cap,size_t *size)
{
    size_t used=0;
    while (in.n) {
        unsigned mask=*in.p++; in.n--;
        for (unsigned bit=0;bit<8 && in.n;bit++,mask>>=1) {
            if (mask&1) {
                if (in.n<2) return -EINVAL;
                unsigned code=hf_u16(in.p),length=3+(code>>12),back=(code&0xfff)+1; in.p+=2; in.n-=2;
                if (back>used || length>cap-used) return -EINVAL;
                for (unsigned i=0;i<length;i++,used++) out[used]=out[used-back];
            } else { if (used==cap) return -EFBIG; out[used++]=*in.p++; in.n--; }
        }
    }
    *size=used; return 0;
}
struct bit_reader { struct span s; size_t bit; };
static int bit(struct bit_reader *r)
{
    if (r->bit/8>=r->s.n) return -1;
    int b=(r->s.p[r->bit/8]>>(r->bit%8))&1; r->bit++; return b;
}
static int dictionary(struct hlp *h)
{
    struct span s; int e=subfile(h,"|Phrases",&s);
    if (!e) {
        if (s.n<8) return -EINVAL;
        unsigned count=hf_u16(s.p),head=h->version<=16 ? 4 : 8;
        if (!hf_range(s.n,head,2u*(count+1))) return -EINVAL;
        h->phrases=calloc(count+1,sizeof(*h->phrases)); h->phrase_text=malloc(HF_STORAGE_LIMIT);
        if (!h->phrases || !h->phrase_text) return -ENOMEM;
        for (unsigned i=0;i<=count;i++) {
            unsigned v=hf_u16(s.p+head+2*i); if (v<2*count+2) return -EINVAL;
            h->phrases[i]=v-2*count-2;
            if (i && h->phrases[i]<h->phrases[i-1]) return -EINVAL;
        }
        size_t at=head+2u*(count+1);
        if (h->version<=16) { h->phrase_size=s.n-at; memcpy(h->phrase_text,s.p+at,h->phrase_size); }
        else { e=lz77((struct span){s.p+at,s.n-at},h->phrase_text,HF_STORAGE_LIMIT,&h->phrase_size); if (e) return e; }
        if (h->phrases[count]>h->phrase_size) return -EINVAL;
        h->phrase_count=count; h->dictionary=1; return 0;
    }
    if (e!=-ENOENT) return e;
    struct span image;
    e=subfile(h,"|PhrIndex",&s); if (e==-ENOENT) return 0; if (e) return e;
    e=subfile(h,"|PhrImage",&image); if (e) return e;
    if (s.n<28) return -EINVAL;
    uint32_t count=hf_u32(s.p+4),expanded=hf_u32(s.p+12),compressed=hf_u32(s.p+16);
    unsigned bc=hf_u16(s.p+24)&15;
    if (count>65535 || expanded>HF_STORAGE_LIMIT || compressed!=image.n || bc>5) return -EINVAL;
    h->phrases=calloc(count+1,sizeof(*h->phrases)); h->phrase_text=malloc(expanded+1);
    if (!h->phrases || !h->phrase_text) return -ENOMEM;
    struct bit_reader bits={{s.p+28,s.n-28},0};
    for (unsigned i=0;i<count;i++) {
        size_t length=1; int b;
        while ((b=bit(&bits))==1) { length+=1u<<bc; if (length>expanded) return -EINVAL; }
        if (b<0) return -EINVAL;
        for (unsigned j=0;j<bc;j++) { b=bit(&bits); if (b<0) return -EINVAL; length+=(unsigned)b<<j; }
        if (length>expanded-h->phrases[i]) return -EINVAL;
        h->phrases[i+1]=h->phrases[i]+(uint32_t)length;
    }
    if (h->phrases[count]!=expanded) return -EINVAL;
    if (expanded==compressed) { memcpy(h->phrase_text,image.p,expanded); h->phrase_size=expanded; }
    else { e=lz77(image,h->phrase_text,expanded,&h->phrase_size); if (e || h->phrase_size!=expanded) return -EINVAL; }
    h->phrase_count=count; h->dictionary=2; return 0;
}
static int phrase_text(struct hlp *h,struct span in,uint8_t *out,size_t expected)
{
    size_t used=0;
    while (in.n && used<expected) {
        unsigned c=*in.p++; in.n--;
        unsigned index=0; size_t length=0; const uint8_t *source=NULL; int repeat=-1,space=0;
        if (h->dictionary==1) {
            if (!c || c>=16) { out[used++]=(uint8_t)c; continue; }
            if (!in.n) return -EINVAL;
            unsigned code=(c<<8)|*in.p++; in.n--; index=(code-256)/2; space=code&1;
        } else if (h->dictionary==2) {
            if (!(c&1)) index=c/2;
            else if ((c&3)==1) { if (!in.n) return -EINVAL; index=(c+1)*64+*in.p++; in.n--; }
            else if ((c&7)==3) { length=c/8+1; if (length>in.n) return -EINVAL; source=in.p; in.p+=length; in.n-=length; }
            else { length=c/16+1; repeat=(c&15)==7 ? ' ' : 0; }
        } else return -EINVAL;
        if (!length) {
            if (index>=h->phrase_count) return -EINVAL;
            length=h->phrases[index+1]-h->phrases[index]; source=h->phrase_text+h->phrases[index];
        }
        if (length>expected-used || (space && length==expected-used)) return -EINVAL;
        if (repeat>=0) memset(out+used,repeat,length); else memcpy(out+used,source,length);
        used+=length; if (space) out[used++]=' ';
    }
    return used==expected && !in.n ? 0 : -EINVAL;
}
static int record_text(struct hlp *h,struct span record,uint8_t **text,size_t *n)
{
    if (record.n<21) return -EINVAL;
    size_t header=hf_u32(record.p+16),size=hf_u32(record.p+4);
    if (header<21 || header>record.n || size>HF_PAGE_LIMIT) return -EINVAL;
    uint8_t *p=malloc(size+1); if (!p) return -ENOMEM;
    int e=0;
    if (size>record.n-header) e=phrase_text(h,(struct span){record.p+header,record.n-header},p,size);
    else memcpy(p,record.p+header,size);
    if (e) { free(p); return e; }
    p[size]=0; *text=p; *n=size; return 0;
}
static int consume(struct span *s,size_t n)
{ if (n>s->n) return -EINVAL; s->p+=n; s->n-=n; return 0; }
static int packed(struct span *s,int wide,int sign,int32_t *value)
{
    if (!s->n) return -EINVAL;
    unsigned bytes=wide ? (s->p[0]&1) ? 4 : 2 : (s->p[0]&1) ? 2 : 1;
    if (bytes>s->n) return -EINVAL;
    uint32_t raw=bytes==4 ? hf_u32(s->p) : bytes==2 ? hf_u16(s->p) : s->p[0];
    int64_t n=raw; if (sign) n-=bytes==4 ? 0x80000000LL : bytes==2 ? 0x8000 : 0x80;
    *value=(int32_t)(n/2); return consume(s,bytes);
}
static int append_text(char *body,size_t *used,const uint8_t *text,size_t n)
{
    if (n>HF_PAGE_LIMIT-*used) return -EFBIG;
    for (size_t i=0;i<n;i++) body[(*used)++]=text[i]=='\r' ? '\n' : text[i] ? (char)text[i] : ' ';
    return 0;
}
static int paragraph(struct hlp *h,struct hf_page *topic,const uint8_t *record,size_t length,const uint8_t *text,size_t n,uint32_t *extent)
{
    unsigned type=record[20]; size_t header=hf_u32(record+16);
    if (header<21 || header>length) return -EINVAL;
    struct span format={record+21,header-21}; int32_t value;
    *extent=0;
    if (type==0x20 || type==0x23) {
        if (packed(&format,1,1,&value) || packed(&format,0,0,&value)) return -EINVAL;
        *extent=(uint32_t)value;
    } else *extent=(uint32_t)n;
    unsigned columns=1;
    if (type==0x23) {
        if (format.n<2) return -EINVAL;
        columns=format.p[0]; unsigned layout=format.p[1]; consume(&format,2);
        if (!columns || ((layout==0 || layout==2) && consume(&format,2)) || consume(&format,4u*columns)) return -EINVAL;
    }
    char *body=malloc(HF_PAGE_LIMIT+1); if (!body) return -ENOMEM;
    size_t old=topic->length,used=old,at=0; int active=-1,e=0;
    if (old) memcpy(body,topic->text,old);
    for (unsigned col=0;col<columns && !e;col++) {
        if (type==0x23) {
            if (format.n<2) { e=-EINVAL; break; }
            if (hf_u16(format.p)==0xffff) break;
            if (consume(&format,5)) { e=-EINVAL; break; }
        }
        if (consume(&format,type==1 ? 6 : 4) || format.n<2) { e=-EINVAL; break; }
        unsigned flags=hf_u16(format.p); consume(&format,2);
        if ((flags&1) && packed(&format,1,1,&value)) { e=-EINVAL; break; }
        for (unsigned bit=2;bit<=64;bit<<=1) if ((flags&bit) && packed(&format,0,1,&value)) e=-EINVAL;
        if (e) break;
        if ((flags&0x100) && consume(&format,3)) { e=-EINVAL; break; }
        if (flags&0x200) {
            if (packed(&format,0,1,&value) || value<0 || value>256) { e=-EINVAL; break; }
            unsigned tabs=(unsigned)value;
            for (unsigned i=0;i<tabs;i++) {
                if (packed(&format,0,0,&value) || ((value&0x4000) && packed(&format,0,0,&value))) { e=-EINVAL; break; }
            }
        }
        while (!e && at<n && format.n) {
            size_t size=0; while (size<n-at && text[at+size]) size++;
            e=append_text(body,&used,text+at,size); if (e) break;
            at+=size; if (at<n) at++;
            unsigned code=format.p[0]; size_t skip=1;
            if (code==0xff) { consume(&format,1); break; }
            if (code==0x20) skip=5;
            else if (code==0x21 || code==0x80 || code==0x84) skip=3;
            else if (code==0x81 || code==0x82 || code==0x83 || code==0x8b || code==0x8c) {
                uint8_t c=code==0x83 ? '\t' : code==0x8b ? ' ' : code==0x8c ? '-' : '\n';
                e=append_text(body,&used,&c,1);
            } else if (code==0x89) {
                if (active>=0) topic->links[active].end=used;
                active=-1;
            } else if (code==0x86 || code==0x87 || code==0x88) {
                if (format.n<2) { e=-EINVAL; break; }
                unsigned graphic=format.p[1]; consume(&format,2);
                if (packed(&format,1,1,&value) || value<0) { e=-EINVAL; break; }
                size_t bytes=(size_t)value;
                if ((graphic==0x22 && packed(&format,0,0,&value)) || consume(&format,bytes)) { e=-EINVAL; break; }
                e=append_text(body,&used,(const uint8_t *)"[Image]",7); continue;
            } else {
                char target[256]="";
                if (code==0xe0 || code==0xe1 || code==0xe2 || code==0xe3 || code==0xe6 || code==0xe7) {
                    if (format.n<5) { e=-EINVAL; break; }
                    snprintf(target,sizeof(target),"hash:%u",hf_u32(format.p+1)); skip=5;
                } else if (code==0xc8 || code==0xcc) {
                    if (format.n<3) { e=-EINVAL; break; }
                    unsigned len=hf_u16(format.p+1); skip=3u+len;
                    if (skip>format.n || !len || !memchr(format.p+3,0,len)) { e=-EINVAL; break; }
                    if (len<sizeof(target)-6) snprintf(target,sizeof(target),"macro:%s",format.p+3);
                } else if (code==0xea || code==0xeb || code==0xee || code==0xef) {
                    if (format.n<8) { e=-EINVAL; break; }
                    skip=3u+hf_u16(format.p+1);
                    if (skip>format.n) { e=-EINVAL; break; }
                    /* External-file hotspots are labelled separately. */
                    unsigned kind=format.p[3]; size_t name=8;
                    if (kind==1) name++;
                    if ((kind==4 || kind==6) && name<skip) { const uint8_t *zero=memchr(format.p+name,0,skip-name); if (!zero) { e=-EINVAL; break; } name=(size_t)(zero-format.p)+1; }
                    if (name<skip && memchr(format.p+name,0,skip-name)) {
                        const char *file=(const char *)format.p+name;
                        if (!*file) snprintf(target,sizeof(target),"hash:%u",hf_u32(format.p+4));
                        else snprintf(target,sizeof(target),"external:%u:%.220s",hf_u32(format.p+4),file);
                    }
                }
                if (target[0] && topic->link_count<HF_LINKS) {
                    if (!topic->links) topic->links=calloc(HF_LINKS,sizeof(*topic->links));
                    if (!topic->links) { e=-ENOMEM; break; }
                    if (active>=0) topic->links[active].end=used;
                    active=(int)topic->link_count++;
                    topic->links[active].start=used;
                    snprintf(topic->links[active].target,sizeof(topic->links[active].target),"%s",target);
                }
            }
            if (consume(&format,skip)) e=-EINVAL;
        }
    }
    if (active>=0) topic->links[active].end=used;
    if (!e && at<n) {
        /* Unstyled trailing text is still readable. */
        while (at<n && !e) { size_t size=0; while (size<n-at && text[at+size]) size++; e=append_text(body,&used,text+at,size); at+=size; if (at<n) at++; }
    }
    if (!e && (used==0 || body[used-1]!='\n')) e=append_text(body,&used,(const uint8_t *)"\n",1);
    if (!e && used-old>HF_STORAGE_LIMIT-h->d->storage) e=-EFBIG;
    if (!e) {
        char *small=realloc(body,used+1); if (!small) e=-ENOMEM;
        else { small[used]=0; free(topic->text); topic->text=small; topic->length=used; h->d->storage+=used-old; return 0; }
    }
    free(body); return e;
}
static int topics(struct hlp *h,int compressed)
{
    struct span s; int e=subfile(h,"|TOPIC",&s); if (e || !s.n) return e ? e : -EINVAL;
    h->block_count=(unsigned)((s.n-1)/h->block_size+1);
    if (h->block_count>HF_STORAGE_LIMIT/16384) return -EFBIG;
    h->topics=malloc(HF_STORAGE_LIMIT); h->blocks=calloc(h->block_count+1,sizeof(*h->blocks));
    if (!h->topics || !h->blocks) return -ENOMEM;
    for (unsigned i=0;i<h->block_count;i++) {
        size_t at=(size_t)i*h->block_size,len=s.n-at; if (len>h->block_size) len=h->block_size;
        if (len<12) return -EINVAL;
        h->blocks[i]=h->topic_size; size_t size=len-12;
        if (compressed) {
            e=lz77((struct span){s.p+at+12,size},h->topics+h->topic_size,HF_STORAGE_LIMIT-h->topic_size,&size); if (e) return e;
        } else { if (size>HF_STORAGE_LIMIT-h->topic_size) return -EFBIG; memcpy(h->topics+h->topic_size,s.p+at+12,size); }
        h->topic_size+=size;
    }
    h->blocks[h->block_count]=h->topic_size;
    uint32_t ref=12; int page=-1; unsigned last_block=0; uint32_t offset=0;
    for (unsigned records=0;ref!=UINT32_MAX;records++) {
        if (records>h->topic_size/21 || ref<12) return -EINVAL;
        unsigned block=(ref-12)>>14,within=(ref-12)&0x3fff;
        if (h->version<=16) { block=(ref-12)/(h->block_size-12); within=(ref-12)%(h->block_size-12); }
        if (block>=h->block_count || within>=h->blocks[block+1]-h->blocks[block]) return -EINVAL;
        size_t at=h->blocks[block]+within;
        if (!hf_range(h->topic_size,at,21)) return -EINVAL;
        const uint8_t *p=h->topics+at; size_t length=hf_u32(p);
        if (length<21 || !hf_range(h->topic_size,at,length)) return -EINVAL;
        if (block!=last_block) offset=0;
        last_block=block;
        unsigned type=p[20];
        if (type==2 || type==0x20 || type==0x23 || type==1) {
            uint8_t *text=NULL; size_t n=0;
            e=record_text(h,(struct span){p,length},&text,&n); if (e) return e;
            if (type==2) {
                char name[32]; snprintf(name,sizeof(name),"topic-%u",h->d->count);
                page=hf_add_page(h->d,name,(const char *)text);
                if (page>=0) { h->d->pages[page].address=h->version<=16 ? ref+block*12 : block*0x8000u+offset; h->d->pages[page].reference=ref; }
                if (page>=0 && hf_u32(p+16)>=33) { h->d->pages[page].browse_prev=hf_u32(p+25); h->d->pages[page].browse_next=hf_u32(p+29); }
                if (page<0) { free(text); return page; }
            } else {
                if (page<0) { free(text); return -EINVAL; }
                uint32_t extent=0;
                e=paragraph(h,&h->d->pages[page],p,length,text,n,&extent);
                if (e) { free(text); return e; }
                offset+=extent;
            }
            free(text);
        }
        uint32_t next=hf_u32(p+12);
        if (h->version<=16) { if (!next) break; if (next>UINT32_MAX-ref) return -EINVAL; next+=ref; }
        if (next!=UINT32_MAX && next<=ref) return -EINVAL;
        ref=next;
    }
    return 0;
}
static int address_page(struct hf_document *d,uint32_t address)
{
    int result=-1;
    for (unsigned i=0;i<d->count;i++) if (d->pages[i].address<=address && (result<0 || d->pages[i].address>d->pages[result].address)) result=(int)i;
    return result;
}
struct context_tree { struct hf_document *d; };
static int context_entry(struct span *s,void *ctx)
{
    struct hf_document *d=((struct context_tree *)ctx)->d;
    if (s->n<8 || d->hash_count==HF_KEYWORDS) return -EINVAL;
    d->hashes[d->hash_count++]=(struct hf_context){hf_u32(s->p),hf_u32(s->p+4)}; s->p+=8; s->n-=8; return 0;
}
struct keyword_tree { struct hf_document *d; struct span data; };
static int keyword_entry(struct span *s,void *ctx)
{
    struct keyword_tree *k=ctx; const uint8_t *zero=memchr(s->p,0,s->n); if (!zero) return -EINVAL;
    size_t len=(size_t)(zero-s->p)+1; if (!hf_range(s->n,len,6)) return -EINVAL;
    unsigned count=hf_u16(s->p+len); size_t at=hf_u32(s->p+len+2);
    if (!hf_range(k->data.n,at,4u*count)) return -EINVAL;
    for (unsigned i=0;i<count;i++) {
        int p=address_page(k->d,hf_u32(k->data.p+at+i*4)); if (p<0) return -EINVAL;
        int e=hf_add_keyword(k->d,(const char *)s->p,(unsigned)p); if (e) return e;
    }
    s->p+=len+6; s->n-=len+6; return 0;
}
int hf_hlp_open(struct hf_document *d)
{
    struct hlp h={.d=d}; struct span s; d->format=1;
    if (hf_u32(d->input+12)>d->input_size) return -EINVAL;
    int e=subfile(&h,"|SYSTEM",&s);
    if (e || s.n<12 || hf_u16(s.p)!=0x036c || hf_u16(s.p+4)!=1) return e ? e : -EINVAL;
    h.version=hf_u16(s.p+2); unsigned flags=hf_u16(s.p+10);
    h.block_size=h.version<=16 || (flags!=0 && flags!=4) ? 2048 : 4096;
    uint32_t home=0;
    if (h.version>16) for (size_t at=12;at<s.n;) {
        if (!hf_range(s.n,at,4)) return -EINVAL;
        unsigned type=hf_u16(s.p+at),len=hf_u16(s.p+at+2); at+=4;
        if (!hf_range(s.n,at,len)) return -EINVAL;
        if (type==3 && len==4) home=hf_u32(s.p+at);
        at+=len;
    }
    e=dictionary(&h);
    if (!e) e=topics(&h,h.version>16 && flags!=0);
    free(h.phrases); free(h.phrase_text); free(h.topics); free(h.blocks);
    if (e) return e;
    int page=address_page(d,home); if (page>=0) d->home=(unsigned)page;
    e=subfile(&h,"|CONTEXT",&s);
    if (!e) {
        d->hashes=calloc(HF_KEYWORDS,sizeof(*d->hashes)); if (!d->hashes) return -ENOMEM;
        struct context_tree c={d}; e=tree(s,context_entry,&c); if (e) return e;
    } else if (e!=-ENOENT) return e;
    e=subfile(&h,"|CTXOMAP",&s);
    if (!e) {
        if (s.n<2) return -EINVAL;
        unsigned count=hf_u16(s.p); if (count>HF_KEYWORDS || !hf_range(s.n,2,8u*count)) return -EINVAL;
        d->contexts=calloc(count+1,sizeof(*d->contexts)); if (!d->contexts) return -ENOMEM;
        for (unsigned i=0;i<count;i++) d->contexts[d->context_count++]=(struct hf_context){hf_u32(s.p+2+i*8),hf_u32(s.p+6+i*8)};
    } else if (e!=-ENOENT) return e;
    e=subfile(&h,"|KWBTREE",&s);
    if (!e) { struct keyword_tree k={.d=d}; e=subfile(&h,"|KWDATA",&k.data); if (!e) e=tree(s,keyword_entry,&k); if (e) return e; }
    else if (e!=-ENOENT) return e;
    for (unsigned i=0;i<d->count;i++) if (!d->pages[i].text) { e=hf_page_text(d,i,d->pages[i].title,strlen(d->pages[i].title)); if (e) return e; }
    for (unsigned i=0;i<d->count;i++) if (d->pages[i].link_count) {
        struct hf_link *links=realloc(d->pages[i].links,d->pages[i].link_count*sizeof(*links));
        if (!links) return -ENOMEM;
        d->pages[i].links=links;
    }
    return 0;
}
