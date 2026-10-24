/* SPDX-License-Identifier: MIT
 * Bounded document ownership and navigation shared by HLP and CHM readers. */
#include "helpfile.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
uint16_t hf_u16(const void *v) { const uint8_t *p=v; return (uint16_t)(p[0]|p[1]<<8); }
uint32_t hf_u32(const void *v) { const uint8_t *p=v; return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
int hf_range(size_t n,size_t at,size_t len) { return at<=n && len<=n-at; }
void hf_close(struct hf_document *d)
{
    if (!d) return;
    hf_chm_close(d);
    for (unsigned i=0;i<d->count;i++) { free(d->pages[i].text); free(d->pages[i].links); free(d->pages[i].anchors); }
    free(d->pages); free(d->keywords); free(d->contexts); free(d->hashes); free(d->toc); free(d->input); free(d);
}
int hf_open(struct hf_document **out,const void *data,size_t n)
{
    *out=NULL;
    if (n>HF_INPUT_LIMIT) return -EFBIG;
    if (n<16 || !data) return -EINVAL;
    struct hf_document *d=calloc(1,sizeof(*d));
    if (!d) return -ENOMEM;
    d->input=malloc(n); d->pages=calloc(HF_TOPICS,sizeof(*d->pages));
    d->keywords=calloc(HF_KEYWORDS,sizeof(*d->keywords));
    if (!d->input || !d->pages || !d->keywords) { hf_close(d); return -ENOMEM; }
    memcpy(d->input,data,n); d->input_size=n;
    int e=hf_u32(data)==0x00035f3fu ? hf_hlp_open(d) : !memcmp(data,"ITSF",4) ? hf_chm_open(d) : -EINVAL;
    if (!e && !d->count) e=-EINVAL;
    if (e) { hf_close(d); return e; }
    *out=d; return 0;
}
int hf_open_file(struct hf_document **out,const char *path)
{
    *out=NULL; FILE *f=fopen(path,"rb"); if (!f) return -errno;
    int e=0; long n;
    if (fseek(f,0,SEEK_END) || (n=ftell(f))<0 || fseek(f,0,SEEK_SET)) { fclose(f); return -EIO; }
    if ((unsigned long)n>HF_INPUT_LIMIT) { fclose(f); return -EFBIG; }
    char *data=malloc((size_t)n+1);
    if (!data) e=-ENOMEM;
    else if (fread(data,1,(size_t)n,f)!=(size_t)n) e=-EIO;
    else {
        e=hf_open(out,data,(size_t)n);
        if (!e) snprintf((*out)->source,sizeof((*out)->source),"%s",path);
    }
    free(data); fclose(f); return e;
}
int hf_add_page(struct hf_document *d,const char *name,const char *title)
{
    if (d->count==HF_TOPICS || strlen(name)>=sizeof(d->pages[0].name)) return -EFBIG;
    unsigned i=d->count++;
    d->pages[i].browse_next=d->pages[i].browse_prev=UINT32_MAX;
    snprintf(d->pages[i].name,sizeof(d->pages[i].name),"%s",name);
    snprintf(d->pages[i].title,sizeof(d->pages[i].title),"%s",title);
    return (int)i;
}
int hf_add_keyword(struct hf_document *d,const char *name,unsigned page)
{
    if (page>=d->count) return -EINVAL;
    if (d->keyword_count==HF_KEYWORDS) return -EFBIG;
    struct hf_keyword *k=&d->keywords[d->keyword_count++];
    snprintf(k->name,sizeof(k->name),"%s",name); k->page=page; return 0;
}
int hf_page_text(struct hf_document *d,unsigned page,const void *text,size_t n)
{
    if (page>=d->count) return -EINVAL;
    struct hf_page *p=&d->pages[page];
    if (n>HF_PAGE_LIMIT || n>HF_STORAGE_LIMIT-(d->storage-p->length)) return -EFBIG;
    char *copy=malloc(n+1); if (!copy) return -ENOMEM;
    memcpy(copy,text,n); copy[n]=0;
    d->storage=d->storage-p->length+n; free(p->text); p->text=copy; p->length=n; return 0;
}
int hf_page_load(struct hf_document *d,unsigned page)
{
    if (page>=d->count) return -ENOENT;
    return d->pages[page].text ? 0 : d->format==2 ? hf_chm_load(d,page) : -ENOENT;
}
int hf_find(struct hf_document *d,const char *name)
{
    if (!name) return -ENOENT;
    while (*name=='/') name++;
    size_t n=strcspn(name,"#>");
    for (unsigned i=0;i<d->count;i++) { const char *p=d->pages[i].name; while (*p=='/') p++;
        if (strlen(p)==n && !strncasecmp(name,p,n)) return (int)i; }
    return -ENOENT;
}
static int by_address(struct hf_document *d,uint32_t address)
{
    if (address==UINT32_MAX) return -ENOENT;
    int found=-ENOENT;
    for (unsigned i=0;i<d->count;i++) if (d->pages[i].address<=address &&
        (found<0 || d->pages[i].address>d->pages[found].address)) found=(int)i;
    return found;
}
int hf_context(struct hf_document *d,uint32_t id)
{
    for (unsigned i=0;i<d->context_count;i++) if (d->contexts[i].id==id)
        return d->format==2 ? (int)d->contexts[i].address : by_address(d,d->contexts[i].address);
    return -ENOENT;
}
int hf_hash(struct hf_document *d,uint32_t hash)
{
    for (unsigned i=0;i<d->hash_count;i++) if (d->hashes[i].id==hash) return by_address(d,d->hashes[i].address);
    return -ENOENT;
}
uint32_t hf_context_hash(const char *s)
{
    uint32_t hash=0;
    for (;*s;s++) { unsigned c=(unsigned char)*s, v;
        if (c>='a' && c<='z') c-=32;
        v=c>='A' && c<='Z' ? c-'A'+17 : c>='1' && c<='9' ? c-'0' : c=='0' ? 10 : c=='.' ? 12 : c=='_' ? 13 : 0;
        if (v) hash=hash*43+v; }
    return hash;
}
int hf_keyword(struct hf_document *d,const char *name,int prefix)
{
    if (!name || !*name) return -ENOENT;
    for (unsigned i=0;i<d->keyword_count;i++) if (!strcasecmp(d->keywords[i].name,name)) return (int)d->keywords[i].page;
    if (prefix) for (unsigned i=0;i<d->keyword_count;i++) if (!strncasecmp(d->keywords[i].name,name,strlen(name))) return (int)d->keywords[i].page;
    return -ENOENT;
}
/* Navigation macros used by compiled help. Parsing never executes code. Each
 * complete call is validated; a refused call leaves the caller's page alone. */
int hf_macro(struct hf_document *d,const char *macro,unsigned current)
{
    const char *s=macro; int result=(int)current;
    if (!s || strlen(s)>1024) return -EINVAL;
    while (*s) {
        while (*s==' ' || *s==';' || *s==':') s++;
        if (!*s) break;
        char name[32], args[3][256]; unsigned n=0,argc=0;
        while ((*s>='A' && *s<='Z') || (*s>='a' && *s<='z')) { if (n+1>=sizeof(name)) return -EINVAL; name[n++]=*s++; }
        name[n]=0; while (*s==' ') s++;
        if (!n || *s++!='(') return -EINVAL;
        memset(args,0,sizeof(args)); while (*s==' ') s++;
        while (*s && *s!=')') {
            if (argc==3) return -EINVAL;
            n=0; char quote=(*s=='"' || *s=='`' || *s=='\'') ? *s++ : 0;
            char close=quote=='`' ? '\'' : quote;
            while (*s && (quote ? *s!=close : *s!=',' && *s!=')')) {
                if (n+1>=sizeof(args[0])) return -EFBIG;
                if (*s=='\\' && s[1] && (s[1]=='"' || s[1]=='\\')) s++;
                args[argc][n++]=*s++;
            }
            if (quote && *s++!=close) return -EINVAL;
            while (n && args[argc][n-1]==' ') args[argc][--n]=0;
            argc++; while (*s==' ') s++; if (*s!=',') break;
            s++; while (*s==' ') s++;
        }
        if (*s++!=')') return -EINVAL;
        if (!strcasecmp(name,"JumpId") || !strcasecmp(name,"JI") || !strcasecmp(name,"PopupId") || !strcasecmp(name,"PI")) {
            if (argc!=2) return -EINVAL;
            if (args[0][0]) {
                const char *base=strrchr(d->source,'/'); base=base ? base+1 : d->source;
                if (strcasecmp(args[0],base) && strcasecmp(args[0],d->source)) return -ENOSYS;
            }
            result=hf_hash(d,hf_context_hash(args[1]));
        } else if (!strcasecmp(name,"JumpContext") || !strcasecmp(name,"JC") || !strcasecmp(name,"PopupContext") || !strcasecmp(name,"PC")) {
            if (argc!=2) return -EINVAL;
            if (args[0][0]) {
                const char *base=strrchr(d->source,'/'); base=base ? base+1 : d->source;
                if (strcasecmp(args[0],base) && strcasecmp(args[0],d->source)) return -ENOSYS;
            }
            char *end; unsigned long id=strtoul(args[1],&end,0); if (*end || id>UINT32_MAX) return -EINVAL;
            result=hf_context(d,(uint32_t)id);
        } else if (!strcasecmp(name,"Contents") || !strcasecmp(name,"Finder") || !strcasecmp(name,"Index")) {
            if (argc) return -EINVAL;
            result=(int)d->home;
        } else if (!strcasecmp(name,"Next") || !strcasecmp(name,"Prev") || !strcasecmp(name,"BrowseNext") || !strcasecmp(name,"BrowsePrev")) {
            if (argc) return -EINVAL;
            if (result<0 || (unsigned)result>=d->count) return -ENOENT;
            uint32_t address=(!strcasecmp(name,"Next") || !strcasecmp(name,"BrowseNext")) ? d->pages[result].browse_next : d->pages[result].browse_prev;
            if (address==UINT32_MAX) return -ENOENT;
            result=by_address(d,address);
        } else if (!strcasecmp(name,"JumpHash") || !strcasecmp(name,"JH") || !strcasecmp(name,"PopupHash") || !strcasecmp(name,"PH")) {
            if (argc!=2 || args[0][0]) return -ENOSYS;
            char *end; unsigned long hash=strtoul(args[1],&end,0); if (*end || hash>UINT32_MAX) return -EINVAL;
            result=hf_hash(d,(uint32_t)hash);
        } else if (!strcasecmp(name,"KLink") || !strcasecmp(name,"KL") || !strcasecmp(name,"ALink") || !strcasecmp(name,"AL")) {
            if (!argc) return -EINVAL;
            result=hf_keyword(d,args[0],0);
        } else return -ENOSYS;
        if (result<0) return result;
        while (*s==' ') s++;
        if (*s && *s!=';' && *s!=':') return -EINVAL;
    }
    return result;
}
