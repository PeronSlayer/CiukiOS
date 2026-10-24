/* SPDX-License-Identifier: MIT
 * HTML Help text, anchors and HHC/HHK sitemap objects. No active content. */
#include "helpfile.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
static int tag_is(const char *s,size_t n,const char *name)
{ size_t k=strlen(name); return n>=k && !strncasecmp(s,name,k) && (n==k || isspace((unsigned char)s[k]) || s[k]=='/'); }
static int attribute(const char *s,size_t n,const char *name,char *out,size_t cap)
{
    size_t at=0; out[0]=0;
    while (at<n && !isspace((unsigned char)s[at])) at++;
    while (at<n) {
        while (at<n && isspace((unsigned char)s[at])) at++;
        size_t key=at;
        while (at<n && s[at]!='=' && !isspace((unsigned char)s[at])) at++;
        size_t len=at-key;
        while (at<n && isspace((unsigned char)s[at])) at++;
        if (at==n || s[at]!='=') { if (at<n) at++; continue; }
        at++; while (at<n && isspace((unsigned char)s[at])) at++;
        char quote=at<n && (s[at]=='\'' || s[at]=='"') ? s[at++] : 0;
        size_t value=at;
        while (at<n && (quote ? s[at]!=quote : !isspace((unsigned char)s[at]) && s[at]!='>')) at++;
        if (len==strlen(name) && !strncasecmp(s+key,name,len)) {
            size_t size=at-value; if (size>=cap) return -EFBIG;
            memcpy(out,s+value,size); out[size]=0; return 1;
        }
        if (quote && at<n) at++;
    }
    return 0;
}
static char entity(const char *s,size_t n)
{
    if (n==3 && !memcmp(s,"amp",3)) return '&';
    if (n==2 && !memcmp(s,"lt",2)) return '<';
    if (n==2 && !memcmp(s,"gt",2)) return '>';
    if (n==4 && !memcmp(s,"quot",4)) return '"';
    if (n==4 && !memcmp(s,"nbsp",4)) return ' ';
    if (n==4 && !memcmp(s,"apos",4)) return '\'';
    if (n>1 && s[0]=='#') {
        unsigned base=n>2 && (s[1]=='x' || s[1]=='X') ? 16 : 10, v=0;
        for (size_t i=base==16 ? 2 : 1;i<n;i++) {
            unsigned c=(unsigned char)s[i], d=c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : c>='A' && c<='F' ? c-'A'+10 : 99;
            if (d>=base || v>255) return '?';
            v=v*base+d;
        }
        return v>=32 && v<127 ? (char)v : '?';
    }
    return '?';
}
int hf_markup(struct hf_page *page,const char *input,size_t n,int sitemap,struct hf_document *d)
{
    char *text=malloc(HF_PAGE_LIMIT+1);
    struct hf_link *links=calloc(HF_LINKS,sizeof(*links));
    struct hf_anchor *anchors=calloc(HF_LINKS,sizeof(*anchors));
    if (!text || !links || !anchors) { free(text); free(links); free(anchors); return -ENOMEM; }
    size_t used=0; unsigned count=0,anchor_count=0; int anchor=-1,skip=0,error=0;
    char title[128]="",local[256]=""; unsigned depth=0;
    for (size_t at=0;at<n;) {
        if (input[at]=='<') {
            if (n-at>=4 && !memcmp(input+at,"<!--",4)) {
                at+=4; while (n-at>=3 && memcmp(input+at,"-->",3)) at++;
                at=n-at>=3 ? at+3 : n; continue;
            }
            size_t start=++at; char quote=0;
            while (at<n) {
                char c=input[at];
                if (quote) { if (c==quote) quote=0; }
                else if (c=='\'' || c=='"') quote=c;
                else if (c=='>') break;
                at++;
            }
            if (at==n) { error=-EINVAL; break; }
            const char *tag=input+start; size_t len=at-start; at++;
            if (tag_is(tag,len,"script") || tag_is(tag,len,"style")) {
                const char *close=tag_is(tag,len,"script") ? "</script>" : "</style>"; size_t size=strlen(close);
                while (size<=n-at && strncasecmp(input+at,close,size)) at++;
                at=size<=n-at ? at+size : n; continue;
            }
            if (tag_is(tag,len,"/script") || tag_is(tag,len,"/style")) skip=0;
            if (sitemap) {
                if (sitemap==1 && tag_is(tag,len,"ul")) { if (depth==32) { error=-EINVAL; break; } depth++; }
                if (sitemap==1 && tag_is(tag,len,"/ul") && depth) depth--;
                if (tag_is(tag,len,"object")) title[0]=local[0]=0;
                else if (tag_is(tag,len,"param")) {
                    char name[32],value[256];
                    int a=attribute(tag,len,"name",name,sizeof(name)),b=attribute(tag,len,"value",value,sizeof(value));
                    if (a<0 || b<0) { error=-EFBIG; break; }
                    if (a && b) {
                        if (!strcasecmp(name,"Name")) snprintf(title,sizeof(title),"%.127s",value);
                        if (!strcasecmp(name,"Local")) snprintf(local,sizeof(local),"%s",value);
                    }
                } else if (tag_is(tag,len,"/object") && title[0]) {
                    int p=hf_find(d,local);
                    if (sitemap==1) {
                        if (!d->toc) d->toc=calloc(HF_TOC,sizeof(*d->toc));
                        if (!d->toc) { error=-ENOMEM; break; }
                        if (d->toc_count==HF_TOC) { error=-EFBIG; break; }
                        struct hf_toc *t=&d->toc[d->toc_count++];
                        snprintf(t->title,sizeof(t->title),"%s",title); t->page=p; t->depth=depth ? depth-1 : 0;
                    }
                    if (p>=0) {
                        if (sitemap==1) snprintf(d->pages[p].title,sizeof(d->pages[p].title),"%s",title);
                        if (sitemap==2) { error=hf_add_keyword(d,title,(unsigned)p); if (error) break; }
                    }
                }
                continue;
            }
            if (skip) continue;
            if (anchor_count<HF_LINKS) {
                char name[128]; int r=attribute(tag,len,"id",name,sizeof(name));
                if (!r && tag_is(tag,len,"a")) r=attribute(tag,len,"name",name,sizeof(name));
                if (r>0) { snprintf(anchors[anchor_count].name,sizeof(anchors[anchor_count].name),"%s",name); anchors[anchor_count++].position=used; }
            }
            if (tag_is(tag,len,"a")) {
                char target[256]; int r=attribute(tag,len,"href",target,sizeof(target));
                if (r<0) { error=r; break; }
                if (r && count<HF_LINKS) {
                    if (anchor>=0) links[anchor].end=used;
                    anchor=(int)count++; links[anchor].start=used;
                    snprintf(links[anchor].target,sizeof(links[anchor].target),"%s",target);
                }
            } else if (tag_is(tag,len,"/a") && anchor>=0) { links[anchor].end=used; anchor=-1; }
            if (tag_is(tag,len,"p") || tag_is(tag,len,"br") || tag_is(tag,len,"/p") || tag_is(tag,len,"li") ||
                tag_is(tag,len,"/tr") || tag_is(tag,len,"/h1") || tag_is(tag,len,"/h2") || tag_is(tag,len,"/div")) {
                if (used && text[used-1]!='\n') { if (used==HF_PAGE_LIMIT) { error=-EFBIG; break; } text[used++]='\n'; }
            }
        } else {
            unsigned char c=(unsigned char)input[at++];
            if (sitemap || skip) continue;
            if (c=='&') {
                size_t end=at; while (end<n && end-at<12 && input[end]!=';' && input[end]!='<') end++;
                if (end<n && input[end]==';') { c=(unsigned char)entity(input+at,end-at); at=end+1; }
            }
            if (isspace(c)) { c=' '; if (!used || text[used-1]==' ' || text[used-1]=='\n') continue; }
            if (c<32) continue;
            if (used==HF_PAGE_LIMIT) { error=-EFBIG; break; }
            text[used++]=c<127 ? (char)c : '?';
        }
    }
    if (anchor>=0) links[anchor].end=used;
    text[used]=0;
    if (!error && !sitemap) {
        /* Keep the decoded bytes, not a full maximum-sized allocation for
         * every cached topic. Search may visit hundreds of small pages. */
        char *small=realloc(text,used+1);
        if (!small) { free(text); free(links); free(anchors); return -ENOMEM; }
        text=small;
        if (count) {
            struct hf_link *p=realloc(links,count*sizeof(*links));
            if (!p) { free(text); free(links); free(anchors); return -ENOMEM; } links=p;
        } else { free(links); links=NULL; }
        if (anchor_count) {
            struct hf_anchor *p=realloc(anchors,anchor_count*sizeof(*anchors));
            if (!p) { free(text); free(links); free(anchors); return -ENOMEM; } anchors=p;
        } else { free(anchors); anchors=NULL; }
        free(page->text); free(page->links); free(page->anchors);
        page->text=text; page->length=used; page->links=links; page->link_count=count; page->anchors=anchors; page->anchor_count=anchor_count; return 0;
    }
    free(text); free(links); free(anchors); return error;
}
