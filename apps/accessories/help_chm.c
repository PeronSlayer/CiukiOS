/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Native bounded memory system for the pinned libmspack ITSF/LZX reader. */
#include "helpfile.h"
#include "../../sdk/win32/lz32/mspack/mspack.h"
#include "../../sdk/win32/lz32/mspack/chm.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
struct chm {
    struct mspack_system system;
    struct hf_document *document;
    struct mschm_decompressor *decoder;
    struct mschmd_header *header;
    size_t allocated;
    char *output; size_t output_size;
};
struct allocation { struct chm *owner; size_t size; };
struct memory_file { struct chm *owner; size_t position; int writing; };
static int memory_write(struct mspack_file *,void *,int);
struct mspack_system *mspack_default_system;
int mspack_valid_system(struct mspack_system *s)
{ return s && s->open && s->close && s->read && s->write && s->seek && s->tell && s->alloc && s->free && s->copy; }
int mspack_sys_filelen(struct mspack_system *s,struct mspack_file *f,off_t *length)
{
    off_t old=s->tell(f); if (old<0 || s->seek(f,0,MSPACK_SYS_SEEK_END)) return MSPACK_ERR_SEEK;
    *length=s->tell(f); return s->seek(f,old,MSPACK_SYS_SEEK_START) ? MSPACK_ERR_SEEK : MSPACK_ERR_OK;
}
static void *allocate(struct mspack_system *s,size_t n)
{
    /* chmd gives LZX an embedded copy of the I/O table, with a forwarding
     * write callback. Recover this copy's owner from its input file, rather
     * than assuming the system pointer is always our outer structure. */
    struct chm *c;
    if (s->write==memory_write) c=(struct chm *)s;
    else {
        struct mschmd_decompress_state *state=(void *)((char *)s - offsetof(struct mschmd_decompress_state,sys));
        c=((struct memory_file *)state->infh)->owner;
    }
    if (n>HF_STORAGE_LIMIT-c->allocated) return NULL;
    struct allocation *p=malloc(sizeof(*p)+n); if (!p) return NULL;
    p->owner=c; p->size=n; c->allocated+=n; return p+1;
}
static void release(void *v)
{ if (v) { struct allocation *p=(struct allocation *)v-1; p->owner->allocated-=p->size; free(p); } }
static struct mspack_file *memory_open(struct mspack_system *s,const char *name,int mode)
{
    if ((mode!=MSPACK_SYS_OPEN_READ || strcmp(name,"input")) && (mode!=MSPACK_SYS_OPEN_WRITE || strcmp(name,"output"))) return NULL;
    struct memory_file *f=allocate(s,sizeof(*f)); if (!f) return NULL;
    *f=(struct memory_file){(struct chm *)s,0,mode==MSPACK_SYS_OPEN_WRITE};
    if (f->writing) { free(f->owner->output); f->owner->output=NULL; f->owner->output_size=0; }
    return (struct mspack_file *)f;
}
static void memory_close(struct mspack_file *f) { release(f); }
static int memory_read(struct mspack_file *file,void *out,int n)
{
    struct memory_file *f=(struct memory_file *)file; struct hf_document *d=f->owner->document;
    if (n<0 || f->writing || f->position>d->input_size) return -1;
    size_t len=(size_t)n; if (len>d->input_size-f->position) len=d->input_size-f->position;
    memcpy(out,d->input+f->position,len); f->position+=len; return (int)len;
}
static int memory_write(struct mspack_file *file,void *in,int n)
{
    struct memory_file *f=(struct memory_file *)file; struct chm *c=f->owner;
    if (n<0 || !f->writing || (size_t)n>HF_STORAGE_LIMIT-f->position) return -1;
    size_t end=f->position+(size_t)n;
    if (end>c->output_size) { char *p=realloc(c->output,end+1); if (!p) return -1; c->output=p; c->output_size=end; }
    memcpy(c->output+f->position,in,(size_t)n); f->position=end; c->output[end]=0; return n;
}
static int memory_seek(struct mspack_file *file,off_t offset,int mode)
{
    struct memory_file *f=(struct memory_file *)file;
    size_t size=f->writing ? f->owner->output_size : f->owner->document->input_size;
    int64_t base=mode==MSPACK_SYS_SEEK_START ? 0 : mode==MSPACK_SYS_SEEK_CUR ? (int64_t)f->position : mode==MSPACK_SYS_SEEK_END ? (int64_t)size : -1;
    if (base<0 || (int64_t)offset < -base || (int64_t)offset>(int64_t)size-base) return -1;
    f->position=(size_t)(base+offset); return 0;
}
static off_t memory_tell(struct mspack_file *file) { return (off_t)((struct memory_file *)file)->position; }
static void memory_message(struct mspack_file *file,const char *format,...) { (void)file; (void)format; }
static void memory_copy(void *source,void *dest,size_t n) { memcpy(dest,source,n); }
static struct mschmd_file *member(struct chm *c,const char *name)
{
    while (*name=='/') name++;
    for (struct mschmd_file *f=c->header->files;f;f=f->next) { const char *p=f->filename; while (*p=='/') p++; if (!strcasecmp(p,name)) return f; }
    for (struct mschmd_file *f=c->header->sysfiles;f;f=f->next) { const char *p=f->filename; while (*p=='/') p++; if (!strcasecmp(p,name)) return f; }
    return NULL;
}
static int extract(struct chm *c,const char *name)
{
    struct mschmd_file *f=member(c,name); if (!f) return -ENOENT;
    if (f->length<0 || (uint64_t)f->length>HF_STORAGE_LIMIT) return -EFBIG;
    /* Bound discarded prefix decoding as well as output allocation. */
    if (f->section->id==1 && (f->offset<0 || (uint64_t)f->offset>32u*1024u*1024u ||
        (uint64_t)f->length>32u*1024u*1024u-(uint64_t)f->offset)) return -EFBIG;
    free(c->output); c->output=NULL; c->output_size=0;
    int r=c->decoder->extract(c->decoder,f,"output");
    if (r || c->output_size!=(uint64_t)f->length) return r==MSPACK_ERR_NOMEMORY ? -ENOMEM : -EINVAL;
    return 0;
}
void hf_chm_close(struct hf_document *d)
{
    struct chm *c=d->archive; if (!c) return;
    if (c->header) c->decoder->close(c->decoder,c->header);
    if (c->decoder) mspack_destroy_chm_decompressor(c->decoder);
    free(c->output); free(c); d->archive=NULL;
}
int hf_chm_load(struct hf_document *d,unsigned page)
{
    struct chm *c=d->archive; int r=extract(c,d->pages[page].name); if (r) return r;
    struct hf_page parsed={0}; r=hf_markup(&parsed,c->output,c->output_size,0,d);
    if (!r && parsed.length>HF_STORAGE_LIMIT-d->storage) r=-EFBIG;
    if (r) { free(parsed.text); free(parsed.links); free(parsed.anchors); return r; }
    struct hf_page *p=&d->pages[page]; p->text=parsed.text; p->length=parsed.length; p->links=parsed.links; p->link_count=parsed.link_count;
    p->anchors=parsed.anchors; p->anchor_count=parsed.anchor_count;
    d->storage+=p->length; return 0;
}
int hf_chm_open(struct hf_document *d)
{
    d->format=2; struct chm *c=calloc(1,sizeof(*c)); if (!c) return -ENOMEM;
    c->document=d; d->archive=c;
    c->system=(struct mspack_system){memory_open,memory_close,memory_read,memory_write,memory_seek,memory_tell,memory_message,allocate,release,memory_copy,NULL};
    c->decoder=mspack_create_chm_decompressor(&c->system); if (!c->decoder) return -ENOMEM;
    c->header=c->decoder->open(c->decoder,"input"); if (!c->header) return -EINVAL;
    char contents[256]="",index[256]="",home[256]="";
    for (struct mschmd_file *f=c->header->files;f;f=f->next) {
        const char *ext=strrchr(f->filename,'.'); if (!ext) continue;
        if (!strcasecmp(ext,".hhc") && !contents[0]) snprintf(contents,sizeof(contents),"%s",f->filename);
        if (!strcasecmp(ext,".hhk") && !index[0]) snprintf(index,sizeof(index),"%s",f->filename);
        if (!strcasecmp(ext,".htm") || !strcasecmp(ext,".html")) {
            int r=hf_add_page(d,f->filename,f->filename); if (r<0) return r;
        }
    }
    int r=extract(c,"#SYSTEM");
    if (!r) {
        if (c->output_size<4) return -EINVAL;
        for (size_t at=4;at<c->output_size;) {
            if (!hf_range(c->output_size,at,4)) return -EINVAL;
            unsigned type=hf_u16(c->output+at),len=hf_u16(c->output+at+2); at+=4;
            if (!hf_range(c->output_size,at,len)) return -EINVAL;
            char *dest=type==0 ? contents : type==1 ? index : type==2 ? home : NULL;
            if (dest) {
                if (!len || len>256 || c->output[at+len-1]) return -EINVAL;
                memcpy(dest,c->output+at,len);
            }
            at+=len;
        }
    } else if (r!=-ENOENT) return r;
    for (unsigned i=0;i<2;i++) {
        const char *name=i ? index : contents; if (!*name) continue;
        r=extract(c,name); if (r) return r;
        r=hf_markup(NULL,c->output,c->output_size,(int)i+1,d); if (r) return r;
    }
    int h=hf_find(d,home); if (h>=0) d->home=(unsigned)h;
    r=extract(c,"#IVB");
    if (!r) {
        if (c->output_size<4 || hf_u32(c->output)>c->output_size-4 || hf_u32(c->output)%8) return -EINVAL;
        size_t n=hf_u32(c->output)/8; if (n>HF_KEYWORDS) return -EFBIG;
        struct hf_context *ids=calloc(n+1,sizeof(*ids)); if (!ids) return -ENOMEM;
        for (size_t i=0;i<n;i++) ids[i]=(struct hf_context){hf_u32(c->output+4+i*8),hf_u32(c->output+8+i*8)};
        r=extract(c,"#STRINGS"); if (r) { free(ids); return r; }
        d->contexts=ids;
        for (size_t i=0;i<n;i++) {
            size_t at=ids[i].address;
            if (at>=c->output_size || !memchr(c->output+at,0,c->output_size-at)) return -EINVAL;
            int p=hf_find(d,c->output+at); if (p<0) continue;
            d->contexts[d->context_count++]=(struct hf_context){ids[i].id,(uint32_t)p};
        }
    } else if (r!=-ENOENT) return r;
    return 0;
}
