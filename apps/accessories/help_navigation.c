/* SPDX-License-Identifier: MIT */
#include "help.h"
#include "../desktop/help_request.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
static int select_request(struct hf_document *d,const struct desk_help_request *r)
{
    if (!r) return (int)d->home;
    if (r->format!=d->format) return -EINVAL;
    if (d->format==1) {
        switch (r->command) {
        case 1: case 8: return hf_context(d,r->context);
        case 3: case 0x105: return hf_keyword(d,r->query,r->command==0x105);
        case 0x102: return hf_macro(d,r->query,d->home);
        case 0xb: case 4: case 9: return (int)d->home;
        default: return -ENOSYS;
        }
    }
    switch (r->command) {
    case 0: return r->query[0] ? hf_find(d,r->query) : (int)d->home;
    case 1: case 2: case 3: return (int)d->home;
    case 0xf: return hf_context(d,r->context);
    default: return -ENOSYS;
    }
}
int help_prepare_document(const char *argument,struct hf_document **out,unsigned *page,char path[161])
{
    *out=NULL;
    struct desk_help_request request,*r=NULL;
    if (desk_help_decode(argument,&request)) { r=&request; argument=request.path; }
    size_t n=strlen(argument); if (!n || n>160) return -EINVAL;
    memcpy(path,argument,n+1);
    int e=hf_open_file(out,path); if (e) return e;
    int selected=select_request(*out,r);
    if (selected>=0) e=hf_page_load(*out,(unsigned)selected); else e=selected;
    if (e) { hf_close(*out); *out=NULL; return e; }
    *page=(unsigned)selected; return 0;
}
int help_request_current(struct help_app *a,const char *argument)
{
    struct desk_help_request r;
    if (!a->document || !desk_help_decode(argument,&r) || strcmp(a->path,r.path)) return -EAGAIN;
    if (r.format==1 && r.command==0x102) {
        char macro[168]; snprintf(macro,sizeof(macro),"macro:%s",r.query); return help_follow(a,macro);
    }
    int selected=select_request(a->document,&r); if (selected<0) return selected;
    int e=help_document_show(a,(unsigned)selected); if (!e) help_request_ui(a,argument); return e;
}
void help_request_ui(struct help_app *a,const char *argument)
{
    struct desk_help_request r;
    if (!desk_help_decode(argument,&r) || r.format!=2) return;
    if (r.command>=1 && r.command<=3) {
        help_tab(a,(int)r.command-1);
        if (r.command==2 && r.query[0]) {
            du_set_text(du_get(a->ui,H_INDEX_EDIT),r.query);
            struct du_event e={.type=DU_EV_CHANGE,.id=H_INDEX_EDIT}; help_event(a->ui,&e,a);
            du_invalidate_widget(a->ui,H_INDEX_EDIT);
        }
    } else if (r.command==0 && r.query[0]) {
        const char *fragment=strchr(r.query,'#'); if (fragment) (void)help_follow(a,fragment);
    }
}
static void link_add(struct help_app *a,const char *label,const char *target)
{
    if (a->link_count==HF_LINKS) return;
    unsigned i=a->link_count++;
    snprintf(a->link_labels[i],sizeof(a->link_labels[i]),"%s",label);
    snprintf(a->link_targets[i],sizeof(a->link_targets[i]),"%s",target);
    a->link_rows[i]=(struct du_item){.text={a->link_labels[i]}};
}
void help_links(struct help_app *a)
{
    a->link_count=0;
    if (a->topic==-2 && a->document) {
        struct hf_page *p=&a->document->pages[a->external_page];
        char target[32];
        if (a->external_offset) { snprintf(target,sizeof(target),"part:%u",(unsigned)(a->external_offset>HF_TEXT_LIMIT ? a->external_offset-HF_TEXT_LIMIT : 0)); link_add(a,"Previous part of this topic",target); }
        if (p->length-a->external_offset>HF_TEXT_LIMIT) { snprintf(target,sizeof(target),"part:%u",(unsigned)(a->external_offset+HF_TEXT_LIMIT)); link_add(a,"Next part of this topic",target); }
        if (a->document->format==1) {
            if (p->browse_prev!=UINT32_MAX) link_add(a,"Previous topic in sequence","macro:Prev()");
            if (p->browse_next!=UINT32_MAX) link_add(a,"Next topic in sequence","macro:Next()");
        }
        for (unsigned i=0;i<p->link_count;i++) {
            struct hf_link *l=&p->links[i]; char label[160];
            size_t n=l->end>=l->start && l->end<=p->length ? l->end-l->start : 0;
            if (n>sizeof(label)-1) n=sizeof(label)-1;
            if (n) { memcpy(label,p->text+l->start,n); label[n]=0; for (size_t k=0;k<n;k++) if ((unsigned char)label[k]<32) label[k]=' '; }
            else snprintf(label,sizeof(label),"Related topic %u",i+1);
            link_add(a,label,l->target);
        }
    } else if (a->topic>=0) {
        const struct help_topic *t=&help_topics[a->topic];
        for (unsigned i=0;i<HELP_WIZARDS_MAX;i++) {
            const char *id=help_wizard_topic(i); if (!id) break;
            if (!strcmp(t->id,id) || !strcmp(t->id,"troubleshooting")) {
                char target[32],label[160]; snprintf(target,sizeof(target),"wizard:%u",i);
                int topic=help_topic_find(id); if (topic<0) continue;
                snprintf(label,sizeof(label),"Troubleshooter: %s",help_topics[topic].title); link_add(a,label,target);
            }
        }
        for (unsigned i=0;i<help_topic_count;i++) {
            const struct help_topic *other=&help_topics[i];
            if ((int)i==a->topic || !other->book || !t->book || strcmp(other->book,t->book)) continue;
            char target[64]; snprintf(target,sizeof(target),"topic:%s",other->id); link_add(a,other->title,target);
        }
        link_add(a,"Help contents","topic:welcome");
        link_add(a,"Troubleshooting","topic:troubleshooting");
    }
    struct du_widget *w=du_get(a->ui,H_LINKS);
    w->items=a->link_rows; w->count=a->link_count; w->active=w->scroll=0;
    du_invalidate_widget(a->ui,H_LINKS);
}
int help_follow(struct help_app *a,const char *target)
{
    /* Targets live in the widget rows, which navigation rebuilds. */
    char copy[256]; if (!target || strlen(target)>=sizeof(copy)) return -EINVAL;
    snprintf(copy,sizeof(copy),"%s",target); target=copy;
    if (!strncmp(target,"topic:",6)) { int t=help_topic_find(target+6); if (t<0) return -ENOENT; help_show_topic(a,t); return 0; }
    if (!strncmp(target,"wizard:",7)) { char *end; unsigned long k=strtoul(target+7,&end,10); if (*end || k>=HELP_WIZARDS_MAX || !help_wizard_topic((unsigned)k)) return -EINVAL; help_wizard_begin(a,(int)k); return 0; }
    if (!a->document || a->topic!=-2) return -ENOENT;
    if (!strncmp(target,"part:",5)) { char *end; unsigned long offset=strtoul(target+5,&end,10); if (*end || offset>HF_PAGE_LIMIT) return -EINVAL; return help_document_part(a,a->external_page,(size_t)offset); }
    int p;
    const char *fragment=strchr(target,'#');
    if (!strncmp(target,"external:",9)) {
        char *end; unsigned long hash=strtoul(target+9,&end,10); if (*end++!=':' || hash>UINT32_MAX || !*end) return -EINVAL;
        char path[256]; size_t at=0; const char *file=end;
        if (file[0] && file[1]==':') {
            unsigned drive=(unsigned char)file[0]; if (drive>='a' && drive<='z') drive-=32;
            if (drive<'A' || drive>'Z' || (file[2]!='/' && file[2]!='\\')) return -EINVAL;
            if (drive!='C') { memcpy(path,"/mnt/",5); path[5]=(char)drive; at=6; } file+=2;
        } else if (*file!='/' && *file!='\\') {
            const char *slash=strrchr(a->path,'/'); if (slash) { at=(size_t)(slash-a->path)+1; if (at>=sizeof(path)) return -EFBIG; memcpy(path,a->path,at); }
        }
        for (;*file;file++) { if (at+1>=sizeof(path)) return -EFBIG; path[at++]=*file=='\\' ? '/' : *file; } path[at]=0;
        struct hf_document *next=NULL; int e=hf_open_file(&next,path); if (e) return e;
        p=hf_hash(next,(uint32_t)hash);
        if (p<0) { hf_close(next); return p; }
        e=help_document_adopt(a,next,(unsigned)p,path); if (e) hf_close(next); return e;
    }
    if (!strncmp(target,"hash:",5)) { char *end; unsigned long h=strtoul(target+5,&end,10); if (*end || h>UINT32_MAX) return -EINVAL; p=hf_hash(a->document,(uint32_t)h); }
    else if (!strncmp(target,"macro:",6)) {
        const char *macro=target+6;
        if (!strcasecmp(macro,"Back()") || !strcasecmp(macro,"B()")) { help_back(a); return 0; }
        if (!strcasecmp(macro,"Exit()") || !strcasecmp(macro,"CloseWindow()")) { if (a->quit) a->quit(a->host); return 0; }
        p=hf_macro(a->document,macro,a->external_page);
    }
    else {
        /* Resolve relative member paths, including ../, inside the archive.
         * URI schemes and archive escapes never leave the Help viewer. */
        if (strchr(target,':')) return -ENOSYS;
        char path[512]; const char *name=a->document->pages[a->external_page].name;
        const char *slash=strrchr(name,'/'); size_t base=target[0]=='/' || !slash ? 0 : (size_t)(slash-name)+1;
        if (target[0]=='#') { p=(int)a->external_page; goto selected; }
        if (base+strlen(target)>=sizeof(path)) return -EFBIG;
        memcpy(path,name,base); snprintf(path+base,sizeof(path)-base,"%s",target);
        char normal[256]; size_t used=0; const char *part=path;
        while (*part) {
            while (*part=='/' || *part=='\\') part++;
            size_t len=strcspn(part,"/\\"); if (!len) break;
            if (len==1 && part[0]=='.') { part+=len; continue; }
            if (len==2 && part[0]=='.' && part[1]=='.') {
                if (!used) return -EINVAL;
                while (used && normal[used-1]!='/') used--;
                if (used) used--;
            } else {
                if (used+len+1>=sizeof(normal)) return -EFBIG;
                if (used) normal[used++]='/';
                memcpy(normal+used,part,len); used+=len;
            }
            part+=len;
        }
        normal[used]=0; p=hf_find(a->document,normal);
    }
selected:
    if (p<0) return p;
    int e=hf_page_load(a->document,(unsigned)p); if (e) return e;
    size_t position=0;
    if (fragment && fragment[1]) {
        struct hf_page *page=&a->document->pages[p]; int found=0;
        for (unsigned i=0;i<page->anchor_count;i++) if (!strcmp(page->anchors[i].name,fragment+1)) { position=page->anchors[i].position; found=1; break; }
        if (!found) return -ENOENT;
    }
    size_t part=position/HF_TEXT_LIMIT*HF_TEXT_LIMIT;
    e=help_document_part(a,(unsigned)p,part);
    if (!e && position) { du_ta_select(&a->ta,position-part,position-part); du_ta_reveal(&a->ta); du_invalidate_widget(a->ui,H_TEXT); }
    return e;
}
