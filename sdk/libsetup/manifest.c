/* Streaming reader of scripts/build_cd.py's on-disc CDMANIFE.JSN.
 * No vendored parser, no unbounded allocation. Unknown fields are skipped;
 * nesting, strings, integer overflow and record paths are checked.
 * SPDX-License-Identifier: MIT */
#include "internal.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

int st_hex_decode(const char *s, uint8_t out[32])
{
    if (strlen(s) != 64) return -EINVAL;
    for (unsigned i = 0; i < 32; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; k++) {
            unsigned c = (unsigned char)s[2*i+k];
            unsigned n = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : 16;
            if (n == 16) return -EINVAL;
            v = v*16+n;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}
void st_hex_encode(const uint8_t s[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) { out[2*i] = hex[s[i]>>4]; out[2*i+1] = hex[s[i]&15]; }
    out[64] = 0;
}
int st_path_valid(const char *s)
{
    if (*s == '/') s++;
    if (!*s || strlen(s) > 252) return 0;
    while (*s) {
        const char *a = s;
        for (; *s && *s != '/'; s++) if ((unsigned char)*s < 32 || *s == 127 || *s == ':' || *s == '\\') return 0;
        size_t n = (size_t)(s-a);
        if (!n || (n == 1 && *a == '.') || (n == 2 && !memcmp(a,"..",2)) || a[n-1] == '.' || a[n-1] == ' ') return 0;
        if (*s) { s++; if (!*s) return 0; }
    }
    return 1;
}
struct reader {
    const struct st_platform *p; int error, ch;
    uint64_t offset; st_source_retry_fn retry;
    unsigned at, n; unsigned char bytes[2048];
    struct st_manifest *m; st_record_fn visit; void *ctx;
    unsigned schema, builds, boots, lists;
};
static int next(struct reader *r)
{
    if (r->at == r->n) {
        int n;
        unsigned attempt=0;
        /* Never keep a manifest descriptor across visitor callbacks: a
         * media replacement invalidates ALL source-volume descriptions. */
        for(;;) {
            int h=r->p->open(r->p->ctx,'D',ST_MANIFEST,false);
            n=h<0 ? h : r->p->seek(r->p->ctx,h,r->offset);
            if(n>=0)n=r->p->read(r->p->ctx,h,r->bytes,sizeof(r->bytes));
            if(h>=0) { int c=r->p->close(r->p->ctx,h); if(n>=0 && c)n=c; }
            if(n>=0)break;
            int e=r->retry ? r->retry(r->ctx,n,ST_MANIFEST,&attempt) : n;
            if(e) { r->error=e; return r->ch=-1; }
        }
        if (!n) return r->ch = -1;
        if ((unsigned)n > sizeof(r->bytes)) { r->error = -EIO; return r->ch = -1; }
        r->offset+=(unsigned)n;
        r->at = 0; r->n = (unsigned)n;
    }
    return r->ch = r->bytes[r->at++];
}
static void space(struct reader *r) { while (r->ch == ' ' || r->ch == '\r' || r->ch == '\n' || r->ch == '\t') next(r); }
static int take(struct reader *r, int c) { space(r); if (r->ch != c) return -EINVAL; next(r); return 0; }
static int digit(int c) { return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1; }
static int unicode(struct reader *r, unsigned *u)
{
    *u = 0;
    for (unsigned i = 0; i < 4; i++) { int d = digit(next(r)); if (d < 0) return -EINVAL; *u = *u*16+(unsigned)d; }
    next(r); return 0;
}
static int string(struct reader *r, char *out, size_t cap)
{
    if (take(r,'"')) return -EINVAL;
    size_t n = 0;
    while (r->ch != '"') {
        int c = r->ch;
        if (c < 32) return -EINVAL;
        next(r);
        unsigned u = (unsigned)c;
        bool escaped_unicode = false;
        if (c == '\\') {
            c = r->ch;
            if (c == 'u') {
                if (unicode(r,&u)) return -EINVAL;
                if (u >= 0xd800 && u <= 0xdbff) {
                    if (r->ch != '\\' || next(r) != 'u') return -EINVAL;
                    unsigned low; if (unicode(r,&low) || low < 0xdc00 || low > 0xdfff) return -EINVAL;
                    u = 0x10000+((u-0xd800)<<10)+low-0xdc00;
                } else if (u >= 0xdc00 && u <= 0xdfff) return -EINVAL;
                escaped_unicode = true;
            } else {
                const char *a = "\"\\/bfnrt", *b = "\"\\/\b\f\n\r\t";
                const char *v = c < 0 ? NULL : strchr(a,c);
                if (!v) return -EINVAL;
                u = (unsigned char)b[v-a]; next(r);
            }
        }
        if (!u) return -EINVAL;
        unsigned char buf[4]; unsigned len = 1; buf[0] = (unsigned char)u;
        if (escaped_unicode && u >= 0x80) {
            if (u < 0x800) { len=2; buf[0]=0xc0|(u>>6); buf[1]=0x80|(u&63); }
            else if (u < 0x10000) { len=3; buf[0]=0xe0|(u>>12); buf[1]=0x80|((u>>6)&63); buf[2]=0x80|(u&63); }
            else { len=4; buf[0]=0xf0|(u>>18); buf[1]=0x80|((u>>12)&63); buf[2]=0x80|((u>>6)&63); buf[3]=0x80|(u&63); }
        }
        if (out && n+len >= cap) return -EOVERFLOW;
        if (out) memcpy(out+n,buf,len);
        n += len;
    }
    next(r); if (out) out[n]=0; return 0;
}
static int number(struct reader *r, uint64_t *out)
{
    space(r); uint64_t n=0; unsigned count=0;
    if (r->ch == '0') { next(r); *out=0; return 0; }
    while (r->ch >= '0' && r->ch <= '9') {
        unsigned d=(unsigned)(r->ch-'0'); if (n > (UINT64_MAX-d)/10) return -EOVERFLOW;
        n=n*10+d; count++; next(r);
    }
    if (!count) return -EINVAL;
    *out=n; return 0;
}
static int skip(struct reader *, unsigned);
static int skip(struct reader *r, unsigned depth)
{
    if (depth > 32) return -EOVERFLOW;
    space(r);
    if (r->ch == '"') return string(r,NULL,0);
    if (r->ch == '{' || r->ch == '[') {
        int close=r->ch == '{' ? '}' : ']'; next(r); space(r);
        if (r->ch == close) { next(r); return 0; }
        do {
            if (close == '}' && (string(r,NULL,0) || take(r,':'))) return -EINVAL;
            int e=skip(r,depth+1); if (e) return e;
            space(r); if (r->ch == close) { next(r); return 0; }
            if (take(r,',')) return -EINVAL;
        } while (true);
    }
    const char *word = r->ch == 't' ? "true" : r->ch == 'f' ? "false" : r->ch == 'n' ? "null" : NULL;
    if (word) { for (; *word; word++) { if (r->ch != *word) return -EINVAL; next(r); } return 0; }
    /* Unknown metadata numbers can be signed or fractional. */
    if (r->ch == '-') next(r);
    uint64_t n; int e=number(r,&n); if (e) return e;
    if (r->ch == '.') { next(r); unsigned k=0; while (r->ch>='0' && r->ch<='9') { next(r); k++; } if (!k) return -EINVAL; }
    if (r->ch == 'e' || r->ch == 'E') { next(r); if (r->ch == '+' || r->ch == '-') next(r); e=number(r,&n); }
    return e;
}
static uint32_t component(const char *s)
{
    static const char *const names[] = {"system","dos","windows","netsurf","games","devtools","soundfont","sources"};
    for (unsigned i=0; i<8; i++) if (!strcmp(s,names[i])) return 1u<<i;
    /* config/cd-components.json has licence/source eligibility rows more
     * detailed than the eight selectable wizard components. */
    if(!strcmp(s,"dos-reference") || !strcmp(s,"dos-unified"))return SETUP_COMP_DOS;
    if(!strcmp(s,"wolf3d") || !strcmp(s,"doom") || !strcmp(s,"quake"))return SETUP_COMP_GAMES;
    /* sec-user-02: rows added after f9-01. The bundled DOS demos install with
     * the games; the Quake III Arena demo installer (protected store
     * /Programs/Installers) is a Win32 program and installs with Windows. */
    if(!strcmp(s,"war2-demo") || !strcmp(s,"duke3d-shareware"))return SETUP_COMP_GAMES;
    if(!strcmp(s,"q3a-demo"))return SETUP_COMP_WINDOWS;
    if(!strcmp(s,"jitterentropy") || !strcmp(s,"lwip"))return SETUP_COMP_SYSTEM;
    return 0;
}
static int record(struct reader *r, char drive, bool directory)
{
    struct st_record f={.drive=drive,.directory=directory,.component=drive=='D' ? SETUP_COMP_SOURCES : 0};
    unsigned seen=0; char key[128], value[260];
    if (take(r,'{')) return -EINVAL;
    do {
        if (string(r,key,sizeof(key)) || take(r,':')) return -EINVAL;
        unsigned bit = !strcmp(key,"path") ? 1 : !strcmp(key,"size") ? 2 : !strcmp(key,"sha256") ? 4 : !strcmp(key,"component") ? 8 : !strcmp(key,"attributes") ? 16 : 0;
        if (bit && (seen&bit)) return -EINVAL;
        seen|=bit; int e=0;
        if (bit==1) e=string(r,f.path,sizeof(f.path));
        else if (bit==2) e=number(r,&f.size);
        else if (bit==4) { e=string(r,value,sizeof(value)); if (!e) e=st_hex_decode(value,f.sha); }
        else if (bit==8) { e=string(r,value,sizeof(value)); if (!e && !(f.component=component(value))) e=-EINVAL; }
        else if (bit==16) { uint64_t n=0; e=number(r,&n); if (!e && n>63) e=-EINVAL; f.attr=(uint8_t)n; f.has_attr=true; }
        else e=skip(r,1);
        if (e) return e;
        space(r); if (r->ch=='}') { next(r); break; }
        if (take(r,',')) return -EINVAL;
    } while (true);
    if (!(seen&1) || (!directory && ((seen&7)!=7 || !f.component)) || !st_path_valid(f.path)) return -EINVAL;
    if (drive=='D' && strncmp(f.path,"SOURCES/",8)) return 0;
    if (directory) r->m->directories++; else r->m->files++;
    return r->visit ? r->visit(r->ctx,&f) : 0;
}
static int records(struct reader *r, char drive, bool directories)
{
    if (take(r,'[')) return -EINVAL;
    space(r); if (r->ch==']') { next(r); return 0; }
    do {
        int e=record(r,drive,directories); if (e) return e;
        space(r); if (r->ch==']') { next(r); return 0; }
        if (take(r,',')) return -EINVAL;
    } while (true);
}
static int object(struct reader *r, unsigned kind)
{
    char key[128]; unsigned seen=0;
    if (take(r,'{')) return -EINVAL;
    space(r); if (r->ch=='}') { next(r); return -EINVAL; }
    do {
        if (string(r,key,sizeof(key)) || take(r,':')) return -EINVAL;
        unsigned bit=0; int e;
        if (kind==0 && !strcmp(key,"schema_version")) { bit=1; uint64_t n=0; e=number(r,&n); r->schema=(unsigned)n; if (n!=1) e=-EINVAL; }
        else if (kind==0 && !strcmp(key,"build")) { bit=2; r->builds++; e=object(r,1); }
        else if (kind==0 && !strcmp(key,"boot_image")) { bit=4; r->boots++; e=object(r,2); }
        else if (kind==0 && !strcmp(key,"iso_tree")) { bit=8; e=records(r,'D',false); }
        else if (kind==1 && !strcmp(key,"disk_image_sha256")) { bit=1; e=string(r,r->m->image_sha,65); }
        else if (kind==1 && !strcmp(key,"loader_sha256")) { bit=2; e=string(r,r->m->loader_sha,65); }
        else if (kind==2 && !strcmp(key,"iso_path")) { bit=1; e=string(r,r->m->boot_image,260); }
        else if (kind==2 && !strcmp(key,"bytes")) { bit=2; e=number(r,&r->m->boot_image_bytes); }
        else if (kind==2 && !strcmp(key,"boot_options")) { bit=4; e=string(r,r->m->boot_cfg,128); }
        else if (kind==2 && !strcmp(key,"boot_cfg_template")) { bit=32; e=string(r,r->m->boot_template,128); }
        else if (kind==2 && !strcmp(key,"boot_cfg_sha256")) { bit=64; e=string(r,r->m->boot_template_sha,65); }
        else if (kind==2 && !strcmp(key,"files")) { bit=8; r->lists++; e=records(r,'C',false); }
        else if (kind==2 && !strcmp(key,"directories")) { bit=16; e=records(r,'C',true); }
        else e=skip(r,1);
        if (e) return e;
        if (bit && (seen&bit)) return -EINVAL;
        seen|=bit;
        space(r); if (r->ch=='}') { next(r); return 0; }
        if (take(r,',')) return -EINVAL;
    } while (true);
}
int st_manifest_read(const struct st_platform *p, struct st_manifest *m, st_record_fn fn, void *ctx, st_source_retry_fn retry)
{
    memset(m,0,sizeof(*m));
    struct reader r={.p=p,.m=m,.visit=fn,.ctx=ctx,.retry=retry};
    next(&r); int e=object(&r,0); space(&r);
    uint8_t hash[32];
    if (!e && (r.ch!=-1 || r.schema!=1 || r.builds!=1 || r.boots!=1 || r.lists!=1 ||
        st_hex_decode(m->image_sha,hash) || st_hex_decode(m->loader_sha,hash) || !st_path_valid(m->boot_image) || !m->boot_cfg[0] || !m->boot_template[0] || st_hex_decode(m->boot_template_sha,hash))) e=-EINVAL;
    if (r.error) e=r.error;
    return e;
}
