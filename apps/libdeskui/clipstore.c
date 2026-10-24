/* SPDX-License-Identifier: MIT
 * Desktop-owned clipboard. See docs/design/system-clipboard.md. */
#include "clipstore.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "../../sdk/libreg/casefold.h"

static void changed(struct clip_store *s) { if (!++s->sequence) ++s->sequence; s->dirty=1; }
void clip_init(struct clip_store *s) { memset(s,0,sizeof(*s)); s->locale=0x409; }
static void clear(struct clip_store *s)
{ for (unsigned i=0;i<s->count;i++) free(s->items[i].data); s->count=s->bytes=0; }
void clip_destroy(struct clip_store *s)
{ clear(s); for (unsigned i=0;i<s->names;i++) free(s->registered[i]); clip_init(s); }
void clip_disconnect(struct clip_store *s, uint32_t client)
{
    if (s->opener==client) s->opener=s->open_token=0;
    if (s->owner!=client) return;
    for (unsigned i=0;i<s->count;) {
        if (s->items[i].delayed || s->items[i].format==0x80) { memmove(s->items+i,s->items+i+1,(s->count-i-1)*sizeof(*s->items)); s->count--; changed(s); }
        else i++;
    }
    s->owner=s->token=0;
}
static struct clip_item *item(struct clip_store *s, unsigned f)
{ for (unsigned i=0;i<s->count;i++) if (s->items[i].format==f) return &s->items[i]; return NULL; }
static int text(unsigned f) { return f==1 || f==7 || f==13; }
static int valid_format(const struct clip_store *s, unsigned f)
{
    return (f>=1 && f<=17) || (f>=0x80 && f<=0x83) || f==0x8e ||
           (f>=0x200 && f<=0x2ff) || (f>=0xc000 && f<0xc000+s->names);
}
/* Validated UTF-8, Unicode full folding without normalization, matching the
 * registry atom namespace (sdk/libreg/casefold.h). */
struct name_cursor { const unsigned char *s; uint32_t pending[3]; unsigned at; };
static uint32_t name_fold(struct name_cursor *p)
{
    if (p->at<3 && p->pending[p->at]) return p->pending[p->at++];
    unsigned c=*p->s++; if (!c) return 0;
    if (c>=128) {
        unsigned n=c<0xe0 ? 1 : c<0xf0 ? 2 : 3;
        c&=(1u<<(6-n))-1;
        while (n--) c=(c<<6)|(*p->s++&63);
    }
    size_t lo=0,hi=sizeof(folds)/sizeof(*folds);
    while (lo<hi) { size_t m=(lo+hi)/2; if (folds[m].from<c) lo=m+1; else hi=m; }
    memset(p->pending,0,sizeof(p->pending)); p->at=1;
    if (lo<sizeof(folds)/sizeof(*folds) && folds[lo].from==c) {
        memcpy(p->pending,folds[lo].to,sizeof(p->pending)); c=p->pending[0];
    }
    return c;
}
static int name_equal(const char *a,const char *b)
{
    struct name_cursor x={.s=(const unsigned char *)a},y={.s=(const unsigned char *)b};
    for (;;) { unsigned c=name_fold(&x),d=name_fold(&y); if (c!=d) return 0; if (!c) return 1; }
}
/* CP1252's non-Latin-1 characters; undefined codes retain their control value. */
static const uint16_t cp1252[32]={0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,0x90,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
static const uint16_t cp437[128]={
0xc7,0xfc,0xe9,0xe2,0xe4,0xe0,0xe5,0xe7,0xea,0xeb,0xe8,0xef,0xee,0xec,0xc4,0xc5,
0xc9,0xe6,0xc6,0xf4,0xf6,0xf2,0xfb,0xf9,0xff,0xd6,0xdc,0xa2,0xa3,0xa5,0x20a7,0x192,
0xe1,0xed,0xf3,0xfa,0xf1,0xd1,0xaa,0xba,0xbf,0x2310,0xac,0xbd,0xbc,0xa1,0xab,0xbb,
0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255d,0x255c,0x255b,0x2510,
0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0x255e,0x255f,0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0x2567,
0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256b,0x256a,0x2518,0x250c,0x2588,0x2584,0x258c,0x2590,0x2580,
0x3b1,0xdf,0x393,0x3c0,0x3a3,0x3c3,0xb5,0x3c4,0x3a6,0x398,0x3a9,0x3b4,0x221e,0x3c6,0x3b5,0x2229,
0x2261,0xb1,0x2265,0x2264,0x2320,0x2321,0xf7,0x2248,0xb0,0x2219,0xb7,0x221a,0x207f,0xb2,0x25a0,0xa0};
static const uint16_t cp850[128]={0xc7,0xfc,0xe9,0xe2,0xe4,0xe0,0xe5,0xe7,0xea,0xeb,0xe8,0xef,0xee,0xec,0xc4,0xc5,0xc9,0xe6,0xc6,0xf4,0xf6,0xf2,0xfb,0xf9,0xff,0xd6,0xdc,0xf8,0xa3,0xd8,0xd7,0x192,0xe1,0xed,0xf3,0xfa,0xf1,0xd1,0xaa,0xba,0xbf,0xae,0xac,0xbd,0xbc,0xa1,0xab,0xbb,0x2591,0x2592,0x2593,0x2502,0x2524,0xc1,0xc2,0xc0,0xa9,0x2563,0x2551,0x2557,0x255d,0xa2,0xa5,0x2510,0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0xe3,0xc3,0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0xa4,0xf0,0xd0,0xca,0xcb,0xc8,0x131,0xcd,0xce,0xcf,0x2518,0x250c,0x2588,0x2584,0xa6,0xcc,0x2580,0xd3,0xdf,0xd4,0xd2,0xf5,0xd5,0xb5,0xfe,0xde,0xda,0xdb,0xd9,0xfd,0xdd,0xaf,0xb4,0xad,0xb1,0x2017,0xbe,0xb6,0xa7,0xf7,0xb8,0xb0,0xa8,0xb7,0xb9,0xb3,0xb2,0x25a0,0xa0};
static unsigned decode(unsigned f, unsigned c, int western)
{ return f==7 && c>=128 ? (western ? cp850[c-128] : cp437[c-128]) : f==1 && c>=128 && c<160 ? cp1252[c-128] : c; }
static unsigned encode(unsigned f, unsigned c, int western)
{ for (unsigned i=0;i<256;i++) if (decode(f,i,western)==c) return i; return '?'; }
static int synth(struct clip_item *p, unsigned f, void **out, uint32_t *bytes, int western)
{
    unsigned units=p->bytes/(p->format==13 ? 2 : 1), step=f==13 ? 2 : 1;
    if (units>DESK_CLIP_ITEM_MAX/step-1) return -ENOMEM;
    unsigned char *b=calloc(units+1,step); if (!b) return -ENOMEM;
    unsigned n=0;
    for (unsigned i=0;i<units;i++) {
        unsigned c=p->format==13 ? p->data[2*i]|((unsigned)p->data[2*i+1]<<8) : decode(p->format,p->data[i],western);
        if (!c) break;
        if (f==13) { b[2*n]=c; b[2*n+1]=c>>8; }
        else {
            if (c>=0xd800 && c<=0xdbff && i+1<units) {
                unsigned low=p->data[2*i+2]|((unsigned)p->data[2*i+3]<<8);
                if (low>=0xdc00 && low<=0xdfff) i++;
            }
            b[n]=encode(f,c,western);
        }
        n++;
    }
    *out=b; *bytes=(n+1)*step; return 0;
}
static int put(struct clip_store *s, const struct desk_clip_wire *w, const void *data)
{
    struct clip_item *p=item(s,w->format);
    if (!valid_format(s,w->format) || (w->bytes && !data) || w->bytes>DESK_CLIP_ITEM_MAX ||
        (w->flags&~3u) || w->flags==3 || ((w->flags&1) && w->bytes)) return -EINVAL;
    if (w->format==0x80 && (w->bytes || w->flags!=DESK_CLIP_DELAYED)) return -EINVAL;
    if ((w->flags&2) && (!p || !p->delayed)) return -EINVAL;
    if (!(w->flags&1) && text(w->format)) {
        if (w->format==13 ? w->bytes<2 || (w->bytes&1) || ((const unsigned char *)data)[w->bytes-1] || ((const unsigned char *)data)[w->bytes-2] :
            !w->bytes || ((const unsigned char *)data)[w->bytes-1]) return -EINVAL;
    }
    if (w->format==16 && !(w->flags&1) && w->bytes!=4) return -EINVAL;
    if (!p && s->count==DESK_CLIP_FORMATS_MAX) return -ENOMEM;
    unsigned old=p ? p->bytes : 0;
    if (w->bytes>DESK_CLIP_BYTES_MAX-(s->bytes-old)) return -ENOMEM;
    void *copy=w->bytes ? malloc(w->bytes) : NULL;
    if (w->bytes && !copy) return -ENOMEM;
    if (copy) memcpy(copy,data,w->bytes);
    if (!p) p=&s->items[s->count++]; else free(p->data);
    *p=(struct clip_item){w->format,w->bytes,copy,!!(w->flags&1) && w->format!=0x80};
    s->bytes=s->bytes-old+w->bytes; changed(s); return 0;
}
static void add_format(struct desk_clip_wire *r, unsigned f)
{ for (unsigned i=0;i<r->count;i++) if (r->formats[i]==f) return; if (r->count<DESK_CLIP_LIST_MAX) r->formats[r->count++]=f; }
int clip_request(struct clip_store *s, uint32_t client, const struct desk_clip_wire *w,
                 const void *data, struct desk_clip_wire *r, void **out)
{
    *out=NULL; memset(r,0,sizeof(*r)); r->serial=w->serial; r->kind=DESK_CLIP_REPLY; r->op=w->op; r->format=w->format;
    int error=0;
    switch (w->op) {
    case DESK_CLIP_OPEN:
        if (s->opener) error=-EBUSY; else { s->opener=client; s->open_token=w->token; } break;
    case DESK_CLIP_QUERY:
        r->bytes=s->owner ? DESK_CLIP_OWNER|(s->owner==client ? DESK_CLIP_MINE : 0) : 0;
        for (unsigned i=0;i<s->count;i++) {
            unsigned f=s->items[i].format; add_format(r,f);
            if (text(f)) { add_format(r,1); add_format(r,7); add_format(r,13); }
            if (f==8 || f==17) add_format(r,2);
            if (f==2) add_format(r,8);
        } break;
    case DESK_CLIP_REGISTER: {
        if (!w->count || w->count>DESK_CLIP_NAME_MAX || !desk_utf8_valid((const uint8_t *)w->name,w->count) || memchr(w->name,0,w->count)) { error=-EINVAL; break; }
        unsigned i; char name[DESK_CLIP_NAME_MAX+1]; memcpy(name,w->name,w->count); name[w->count]=0;
        for (i=0;i<s->names;i++) if (name_equal(name,s->registered[i])) break;
        if (i==s->names) {
            if (i==DESK_CLIP_NAMES_MAX) { error=-ENOMEM; break; }
            s->registered[i]=malloc(w->count+1); if (!s->registered[i]) { error=-ENOMEM; break; }
            memcpy(s->registered[i],name,w->count+1); s->names++;
        }
        r->format=0xc000+i; break;
    }
    case DESK_CLIP_NAME:
        if (w->format<0xc000 || w->format>=0xc000+s->names) error=-EINVAL;
        else { strcpy(r->name,s->registered[w->format-0xc000]); r->count=strlen(r->name); } break;
    case DESK_CLIP_EMPTY:
        if (s->opener!=client) { error=-EPERM; break; }
        if (w->token && w->token!=s->sequence) { error=-EAGAIN; break; }
        clear(s); s->owner=client; s->token=s->open_token; changed(s); break;
    case DESK_CLIP_PUT:
        if (s->owner!=client || (!(w->flags&DESK_CLIP_RENDERED) && s->opener!=client)) error=-EPERM;
        else error=put(s,w,data); break;
    case DESK_CLIP_GET: {
        if (s->opener!=client) { error=-EPERM; break; }
        unsigned f=w->format; struct clip_item *p=item(s,f);
        if (!p && f==2) p=item(s,8);
        if (!p && f==8) p=item(s,2);
        if (!p && (f==8 || f==2)) p=item(s,17);
        if (!p && text(f)) { p=item(s,13); if (!p) p=item(s,1); if (!p) p=item(s,7); }
        if (!p) { error=-ENOENT; break; }
        if (p->delayed) { r->format=p->format; error=-EAGAIN; break; }
        if (p->format!=f && text(f)) { struct clip_item *locale=item(s,16);
            if (locale && locale->delayed) { r->format=16; error=-EAGAIN; break; }
            uint32_t lcid=locale ? desk_get32(locale->data) : s->locale;
            unsigned lang=lcid&1023;
            int western=lcid==0x809 || lang==16 || lang==7 || lang==12 || lang==10 || lang==19 || lang==22 || lang==29 || lang==6 || lang==20 || lang==11;
            error=synth(p,f,out,&r->bytes,western); break; }
        if (p->bytes) { *out=malloc(p->bytes); if (!*out) { error=-ENOMEM; break; } memcpy(*out,p->data,p->bytes); }
        r->bytes=p->bytes; break;
    }
    case DESK_CLIP_CLOSE:
        if (s->opener!=client) { error=-EPERM; break; }
        if (s->owner==client && (item(s,1)||item(s,7)||item(s,13)) && !item(s,16)) {
            unsigned char b[4]; desk_put32(b,s->locale);
            struct desk_clip_wire locale={.format=16,.bytes=4}; error=put(s,&locale,b);
        }
        s->opener=s->open_token=0; break;
    case DESK_CLIP_RELEASE:
        if (s->owner==client && s->token==w->token) {
            uint32_t opener=s->opener,token=s->open_token;
            clip_disconnect(s,client); s->opener=opener; s->open_token=token;
        }
        break;
    case DESK_CLIP_WATCH: break; /* subscriptions belong to the transport */
    default: error=-EINVAL; break;
    }
    r->sequence=s->sequence; r->token=s->token; r->result=error; return error;
}
static int text_utf8_valid(const unsigned char *s, unsigned n)
{
    for (unsigned i=0;i<n;) {
        unsigned c=s[i++],minimum,extra;
        if (c<128) { if (!c) return 0; continue; }
        if (c>=0xc2 && c<=0xdf) { c&=31; extra=1; minimum=128; }
        else if (c>=0xe0 && c<=0xef) { c&=15; extra=2; minimum=2048; }
        else if (c>=0xf0 && c<=0xf4) { c&=7; extra=3; minimum=65536; }
        else return 0;
        if (extra>n-i) return 0;
        while (extra--) { unsigned b=s[i++]; if ((b&0xc0)!=0x80) return 0; c=(c<<6)|(b&63); }
        if (c<minimum || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) return 0;
    }
    return 1;
}
int clip_text_encode(const char *s, size_t n, void **out, uint32_t *bytes)
{
    if ((!s && n) || n>DESK_CLIP_ITEM_MAX/4-1 || (n && (!text_utf8_valid((const uint8_t *)s,n) || memchr(s,0,n)))) return -EINVAL;
    unsigned char *b=malloc(4*n+2); if (!b) return -ENOMEM;
    unsigned used=0;
    for (size_t i=0;i<n;) {
        unsigned c=(unsigned char)s[i++];
        if (c>=0xc0) { unsigned left=c<0xe0 ? 1 : c<0xf0 ? 2 : 3; c&=(1u<<(6-left))-1; while (left--) c=(c<<6)|((unsigned char)s[i++]&63); }
        if (c=='\n' && (i<2 || s[i-2]!='\r')) { b[used++]='\r'; b[used++]=0; }
        if (c>=0x10000) { unsigned h=0xd800+((c-0x10000)>>10); b[used++]=h; b[used++]=h>>8; c=0xdc00+((c-0x10000)&1023); }
        b[used++]=c; b[used++]=c>>8;
    }
    b[used++]=0; b[used++]=0; *out=b; *bytes=used; return 0;
}
int clip_text_decode(const void *data, uint32_t n, char **out)
{
    const unsigned char *b=data;
    if (!b || n<2 || (n&1) || n>DESK_CLIP_ITEM_MAX || b[n-2] || b[n-1]) return -EINVAL;
    char *s=malloc((size_t)n*2+1); if (!s) return -ENOMEM;
    unsigned used=0;
    for (unsigned i=0;i+1<n;i+=2) {
        unsigned c=b[i]|((unsigned)b[i+1]<<8); if (!c) break;
        if (c=='\r' && i+3<n && b[i+2]=='\n' && !b[i+3]) continue;
        if (c>=0xd800 && c<=0xdbff && i+3<n) { unsigned lo=b[i+2]|((unsigned)b[i+3]<<8); if (lo>=0xdc00 && lo<=0xdfff) { c=0x10000+((c-0xd800)<<10)+(lo-0xdc00); i+=2; } else c=0xfffd; }
        else if (c>=0xd800 && c<=0xdfff) c=0xfffd;
        if (c<128) s[used++]=c;
        else { if (c>=0x10000) s[used++]=0xf0|(c>>18); else if (c>=2048) s[used++]=0xe0|(c>>12); else s[used++]=0xc0|(c>>6);
            if (c>=0x10000) s[used++]=0x80|((c>>12)&63);
            if (c>=2048) s[used++]=0x80|((c>>6)&63);
            s[used++]=0x80|(c&63); }
    }
    s[used]=0; *out=s; return 0;
}
int clip_dib_encode(const uint32_t *pixels, unsigned stride, unsigned w, unsigned h, void **out, uint32_t *bytes)
{
    if (!pixels || !w || !h || w>2048 || h>2048 || stride<w) return -EINVAL;
    unsigned pitch=(w*3+3)&~3u, n=40+pitch*h;
    if (n>DESK_CLIP_ITEM_MAX) return -ENOMEM;
    unsigned char *b=calloc(1,n); if (!b) return -ENOMEM;
    desk_put32(b,40); desk_put32(b+4,w); desk_put32(b+8,h); b[12]=1; b[14]=24; desk_put32(b+20,pitch*h);
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) { unsigned c=pixels[(h-1-y)*stride+x], k=40+y*pitch+3*x; b[k]=c; b[k+1]=c>>8; b[k+2]=c>>16; }
    *out=b; *bytes=n; return 0;
}
static unsigned component(unsigned value, unsigned mask)
{
    if (!mask) return 0;
    while (!(mask&1)) { value>>=1; mask>>=1; }
    return (unsigned)(((uint64_t)(value&mask)*255)/mask);
}
int clip_dib_decode(const void *data, uint32_t n, uint32_t **out, unsigned *width, unsigned *height)
{
    const unsigned char *b=data;
    if (!b || n<40 || desk_get32(b)<40 || desk_get32(b)>n || b[12]!=1 || b[13]) return -EINVAL;
    int32_t wi=desk_get32(b+4), hi=desk_get32(b+8);
    unsigned bits=b[14]|((unsigned)b[15]<<8), compression=desk_get32(b+16), header=desk_get32(b);
    if (wi<=0 || wi>2048 || !hi || hi< -2048 || hi>2048 ||
        (bits!=1 && bits!=4 && bits!=8 && bits!=16 && bits!=24 && bits!=32) ||
        (compression!=0 && compression!=3) || (compression==3 && bits!=16 && bits!=32)) return -EINVAL;
    unsigned masks[3]={bits==16 ? 0x7c00 : 0xff0000,bits==16 ? 0x3e0 : 0xff00,bits==16 ? 31 : 255};
    unsigned offset=header;
    if (compression==3) {
        if (header==40) { if (n<52) return -EINVAL; offset+=12; }
        else if (header<52) return -EINVAL;
        for (unsigned i=0;i<3;i++) {
            masks[i]=desk_get32(b+40+4*i);
            if (!masks[i] || (bits==16 && masks[i]>65535)) return -EINVAL;
            unsigned mask=masks[i]; while (!(mask&1)) mask>>=1;
            if (mask&(mask+1)) return -EINVAL;
        }
        if ((masks[0]&masks[1]) || (masks[0]&masks[2]) || (masks[1]&masks[2])) return -EINVAL;
    }
    unsigned colors=desk_get32(b+32), table=offset;
    if (bits<=8) { if (!colors) colors=1u<<bits; if (colors>(1u<<bits) || colors>(n-offset)/4) return -EINVAL; offset+=colors*4; }
    else if (colors) { if (colors>(n-offset)/4) return -EINVAL; offset+=colors*4; }
    unsigned w=wi,h=hi<0 ? -hi : hi,pitch=((w*bits+31)/32)*4;
    if (offset>n || pitch*h>n-offset) return -EINVAL;
    uint32_t *p=malloc(w*h*4); if (!p) return -ENOMEM;
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        unsigned row=offset+(hi<0 ? y : h-y-1)*pitch,c;
        if (bits<=8) {
            unsigned v=bits==8 ? b[row+x] : bits==4 ? (b[row+x/2]>>(x%2 ? 0 : 4))&15 : (b[row+x/8]>>(7-x%8))&1;
            if (v>=colors) { free(p); return -EINVAL; }
            unsigned k=table+4*v; c=b[k]|((unsigned)b[k+1]<<8)|((unsigned)b[k+2]<<16);
        } else if (bits==24) { unsigned k=row+3*x; c=b[k]|((unsigned)b[k+1]<<8)|((unsigned)b[k+2]<<16); }
        else {
            unsigned k=row+x*(bits/8), v=bits==16 ? b[k]|((unsigned)b[k+1]<<8) : desk_get32(b+k);
            c=(component(v,masks[0])<<16)|(component(v,masks[1])<<8)|component(v,masks[2]);
        }
        p[y*w+x]=c;
    }
    *out=p; *width=w; *height=h; return 0;
}
