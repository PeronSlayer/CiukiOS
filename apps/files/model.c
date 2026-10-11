/* SPDX-License-Identifier: MIT */
#include "model.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static unsigned fold(unsigned c) { return c>='A' && c<='Z'?c+32:c; }
static int compare(const char *a,const char *b)
{ while (*a && fold((unsigned char)*a)==fold((unsigned char)*b)) { a++; b++; } return (int)fold((unsigned char)*a)-(int)fold((unsigned char)*b); }
const char *fm_basename(const char *path) { const char *p=strrchr(path,'/'); return p?p+1:path; }
int fm_join(char *out,const char *parent,const char *name)
{
    if (!*name || strchr(name,'/') || strchr(name,'\\') || strchr(name,':') || !strcmp(name,".") || !strcmp(name,"..")) return -EINVAL;
    size_t n=strlen(parent),m=strlen(name); if (n+m+2>FM_PATH || m>=FM_NAME) return -ENAMETOOLONG;
    memcpy(out,parent,n); if (n && parent[n-1]!='/') out[n++]='/'; memcpy(out+n,name,m+1); return 0;
}
int fm_beneath(const char *path,const char *parent)
{
    size_t n=strlen(parent); if (n==1 && parent[0]=='/') return path[0]=='/';
    for (size_t i=0;i<n;i++) if (fold((unsigned char)path[i])!=fold((unsigned char)parent[i])) return 0;
    return !path[n] || path[n]=='/';
}
const char *fm_type_name(const struct fm_entry *e)
{
    if (e->meta.type==FM_DIR) return "Folder";
    if (e->meta.type!=FM_FILE) return "Device";
    const char *suffix=strrchr(e->name,'.');
    if (suffix && (!compare(suffix,".txt") || !compare(suffix,".md") || !compare(suffix,".cfg") || !compare(suffix,".lua"))) return "Text document";
    return (e->meta.mode&0111)?"Application":"File";
}
struct listing { struct fm_entry *entries; unsigned count,capacity; };
static int collect(const char *name,const struct fm_meta *meta,void *ctx)
{
    struct listing *l=ctx; if (!strcmp(name,".") || !strcmp(name,"..")) return 0;
    if (!*name || strlen(name)>=FM_NAME || strchr(name,'/')) return -EIO;
    if (l->count==FM_ENTRIES) return -E2BIG;
    if (l->count==l->capacity) {
        unsigned cap=l->capacity?l->capacity*2:32; struct fm_entry *p=realloc(l->entries,cap*sizeof(*p));
        if (!p) return -ENOMEM; l->entries=p; l->capacity=cap;
    }
    struct fm_entry *e=&l->entries[l->count++]; memset(e,0,sizeof(*e)); strcpy(e->name,name); e->meta=*meta; return 0;
}
static int listing(struct fm_vfs *fs,const char *path,struct listing *l)
{ memset(l,0,sizeof(*l)); int r=fs->list(fs->ctx,path,collect,l); if (r) { free(l->entries); memset(l,0,sizeof(*l)); } return r; }
void fm_init(struct fm_model *m,const struct fm_vfs *fs) { memset(m,0,sizeof(*m)); m->fs=*fs; strcpy(m->path,"/"); }
static void clear_clipboard(struct fm_model *m)
{ for (unsigned i=0;i<m->clipboard_count;i++) free(m->clipboard[i]); free(m->clipboard); m->clipboard=NULL; m->clipboard_count=0; }
void fm_destroy(struct fm_model *m) { free(m->entries); clear_clipboard(m); memset(m,0,sizeof(*m)); }
void fm_sort(struct fm_model *m,unsigned column,int descending)
{
    if (column>3) return; m->sort=column; m->descending=descending;
    /* Stable bottom-up mergesort; selection follows the entries, not indices. */
    struct fm_entry *tmp=malloc(m->count*sizeof(*tmp)); if (!tmp) return;
    char active[FM_NAME]="",anchor[FM_NAME]="";
    if (m->active<m->count) strcpy(active,m->entries[m->active].name);
    if (m->anchor<m->count) strcpy(anchor,m->entries[m->anchor].name);
    for (unsigned span=1;span<m->count;span*=2) for (unsigned start=0;start<m->count;start+=span*2) {
        unsigned middle=start+span,end=start+span*2; if (middle>m->count) middle=m->count; if (end>m->count) end=m->count;
        unsigned left=start,right=middle;
        for (unsigned i=start;i<end;i++) {
            int result=0;
            if (left<middle && right<end) {
                struct fm_entry *a=&m->entries[left],*b=&m->entries[right];
                if (a->meta.type==FM_DIR && b->meta.type!=FM_DIR) result=-1;
                else if (a->meta.type!=FM_DIR && b->meta.type==FM_DIR) result=1;
                else {
                    if (column==1) result=a->meta.size<b->meta.size?-1:a->meta.size>b->meta.size;
                    else if (column==2) result=compare(fm_type_name(a),fm_type_name(b));
                    else if (column==3) result=a->meta.modified<b->meta.modified?-1:a->meta.modified>b->meta.modified;
                    if (!result) result=compare(a->name,b->name);
                    if (descending) result=-result;
                }
            }
            tmp[i]=left<middle && (right==end || result<=0)?m->entries[left++]:m->entries[right++];
        }
        memcpy(m->entries+start,tmp+start,(end-start)*sizeof(*tmp));
    }
    for (unsigned i=0;i<m->count;i++) { if (!strcmp(active,m->entries[i].name)) m->active=i; if (!strcmp(anchor,m->entries[i].name)) m->anchor=i; }
    free(tmp);
}
static int load(struct fm_model *m,const char *path)
{
    struct listing l; int r=listing(&m->fs,path,&l); if (r) return r;
    free(m->entries); m->entries=l.entries; m->count=l.count; m->capacity=l.capacity;
    m->active=m->anchor=0; m->find_length=0; m->find[0]=0; strcpy(m->path,path); fm_sort(m,m->sort,m->descending); return 0;
}
int fm_navigate(struct fm_model *m,const char *path)
{
    char canonical[FM_PATH]; int r=m->fs.canonical(m->fs.ctx,path,canonical); if (r) return r;
    if ((r=load(m,canonical))) return r;
    if (m->history_count && !strcmp(m->history[m->history_at],canonical)) return 0;
    m->history_count=m->history_count?m->history_at+1:0;
    if (m->history_count==FM_HISTORY) { memmove(m->history,m->history+1,(FM_HISTORY-1)*FM_PATH); m->history_count--; }
    m->history_at=m->history_count; strcpy(m->history[m->history_count++],canonical); return 0;
}
int fm_refresh(struct fm_model *m)
{
    /* Preserve selection and focus by spelling across a refresh. */
    struct listing l; int r=listing(&m->fs,m->path,&l); if (r) return r;
    char active[FM_NAME]=""; if (m->active<m->count) strcpy(active,m->entries[m->active].name);
    for (unsigned i=0;i<l.count;i++) for (unsigned j=0;j<m->count;j++) if (!compare(l.entries[i].name,m->entries[j].name)) l.entries[i].selected=m->entries[j].selected;
    free(m->entries); m->entries=l.entries; m->count=l.count; m->capacity=l.capacity; m->active=m->anchor=0;
    for (unsigned i=0;i<m->count;i++) if (!compare(active,m->entries[i].name)) m->active=m->anchor=i;
    fm_sort(m,m->sort,m->descending); return 0;
}
int fm_history(struct fm_model *m,int delta)
{
    int n=(int)m->history_at+delta; if (n<0 || (unsigned)n>=m->history_count) return -ENOENT;
    int r=load(m,m->history[n]); if (!r) m->history_at=(unsigned)n; return r;
}
int fm_up(struct fm_model *m)
{ char path[FM_PATH]; strcpy(path,m->path); size_t n=strlen(path); if (n==1) return 0; char *slash=strrchr(path,'/'); if (slash==path) slash[1]=0; else *slash=0; return fm_navigate(m,path); }
void fm_select(struct fm_model *m,unsigned index,unsigned modifiers)
{
    if (index>=m->count) return; m->active=index;
    if (modifiers&1) { unsigned first=index<m->anchor?index:m->anchor,last=index>m->anchor?index:m->anchor;
        for (unsigned i=0;i<m->count;i++) m->entries[i].selected=i>=first && i<=last; }
    else if (modifiers&2) { m->entries[index].selected=!m->entries[index].selected; m->anchor=index; }
    else { for (unsigned i=0;i<m->count;i++) m->entries[i].selected=i==index; m->anchor=index; }
}
void fm_select_all(struct fm_model *m,int invert)
{ for (unsigned i=0;i<m->count;i++) m->entries[i].selected=invert?!m->entries[i].selected:1; }
int fm_find(struct fm_model *m,unsigned ch,uint64_t now)
{
    if (ch<32 || ch>126) return -EINVAL;
    if (now>m->find_deadline) m->find_length=0;
    if (m->find_length+1>=sizeof(m->find)) m->find_length=0;
    m->find[m->find_length++]=(char)ch; m->find[m->find_length]=0; m->find_deadline=now+1000000000u;
    for (unsigned i=0;i<m->count;i++) { size_t j=0; while (j<m->find_length && fold((unsigned char)m->find[j])==fold((unsigned char)m->entries[i].name[j])) j++;
        if (j==m->find_length) { fm_select(m,i,0); return (int)i; } }
    return -ENOENT;
}
int fm_clipboard(struct fm_model *m,int cut)
{
    unsigned count=0; for (unsigned i=0;i<m->count;i++) count+=m->entries[i].selected;
    if (!count) return -ENOENT;
    char **paths=calloc(count,sizeof(*paths)); if (!paths) return -ENOMEM;
    unsigned next=0; int r=0;
    for (unsigned i=0;i<m->count;i++) if (m->entries[i].selected) {
        paths[next]=malloc(FM_PATH); if (!paths[next]) { r=-ENOMEM; break; }
        if ((r=fm_join(paths[next++],m->path,m->entries[i].name))) break;
    }
    if (r) { for (unsigned i=0;i<count;i++) free(paths[i]); free(paths); return r; }
    clear_clipboard(m); m->clipboard=paths; m->clipboard_count=count; m->cut=cut; return 0;
}
int fm_create(struct fm_model *m,const char *name,int directory)
{
    char path[FM_PATH]; int r=fm_join(path,m->path,name); if (r) return r;
    if (directory) r=m->fs.mkdir(m->fs.ctx,path);
    else { int fd=m->fs.open(m->fs.ctx,path,1); if (fd<0) return fd;
        r=m->fs.sync(m->fs.ctx,fd); int closed=m->fs.close(m->fs.ctx,fd); if (!r) r=closed;
        if (r) m->fs.unlink(m->fs.ctx,path);
    }
    if (!r) { r=fm_refresh(m); for (unsigned i=0;i<m->count;i++) if (!compare(m->entries[i].name,name)) fm_select(m,i,0); } return r;
}
int fm_rename(struct fm_model *m,unsigned index,const char *name)
{
    if (index>=m->count) return -ENOENT;
    char source[FM_PATH],target[FM_PATH]; int r=fm_join(source,m->path,m->entries[index].name);
    if (!r) r=fm_join(target,m->path,name); if (r) return r;
    struct fm_meta meta; r=m->fs.stat_path(m->fs.ctx,target,&meta);
    if (!r && compare(source,target)) return -EEXIST; if (r && r!=-ENOENT) return r;
    r=m->fs.rename(m->fs.ctx,source,target); if (!r) { r=fm_refresh(m); for (unsigned i=0;i<m->count;i++) if (!compare(m->entries[i].name,name)) fm_select(m,i,0); } return r;
}
static void free_frame(struct fm_frame *f) { free(f->entries); memset(f,0,sizeof(*f)); }
static int cleanup(struct fm_job *j)
{
    int error=0,r;
    if (j->input>=0) { r=j->fs.close(j->fs.ctx,j->input); j->input=-1; if (r) error=r; }
    if (j->output>=0) { r=j->fs.close(j->fs.ctx,j->output); j->output=-1; if (r && !error) error=r; }
    if (*j->temporary) { r=j->fs.unlink(j->fs.ctx,j->temporary); if (r && r!=-ENOENT && !error) error=r; if (!r || r==-ENOENT) j->temporary[0]=0; }
    return error;
}
static int fail(struct fm_job *j,int r)
{ j->error=-r; j->cleanup_error=-cleanup(j); j->state=FM_FAILED; return r; }
static void pop(struct fm_job *j) { free_frame(&j->frames[--j->depth]); if (!j->depth) j->completed++; }
static int push(struct fm_job *j,const char *source,const char *target)
{
    if (j->depth==FM_DEPTH) return -EINVAL;
    struct fm_frame *f=&j->frames[j->depth++]; memset(f,0,sizeof(*f)); strcpy(f->source,source); if (target) strcpy(f->target,target); return 0;
}
int fm_job_start(struct fm_job *j,const struct fm_vfs *fs,enum fm_job_kind kind,char *const *paths,unsigned n,const char *destination)
{
    memset(j,0,sizeof(*j)); j->input=j->output=-1; j->fs=*fs; j->kind=kind;
    if (!n || n>FM_ENTRIES) return -EINVAL;
    if ((kind==FM_COPY || kind==FM_MOVE) && (!destination || strlen(destination)>=FM_PATH)) return -EINVAL;
    if (destination) strcpy(j->destination,destination);
    j->sources=calloc(n,sizeof(*j->sources)); if (!j->sources) return -ENOMEM;
    j->count=n;
    for (unsigned i=0;i<n;i++) {
        if (paths[i][0]!='/' || strlen(paths[i])>=FM_PATH) { fm_job_destroy(j); return -EINVAL; }
        j->sources[i]=malloc(strlen(paths[i])+1); if (!j->sources[i]) { fm_job_destroy(j); return -ENOMEM; }
        strcpy(j->sources[i],paths[i]);
        if ((kind==FM_COPY || kind==FM_MOVE) && fm_beneath(destination,paths[i])) { fm_job_destroy(j); return -EINVAL; }
        if (!strcmp(paths[i],"/") && kind==FM_DELETE) { fm_job_destroy(j); return -EBUSY; }
    }
    j->state=FM_RUNNING; return 0;
}
static void metadata(struct fm_job *j,struct fm_frame *f)
{
    if (!j->fs.metadata) { j->metadata_warnings++; return; }
    int r=j->fs.metadata(j->fs.ctx,f->target,&f->meta); if (r) j->metadata_warnings++;
}
static int begin_file(struct fm_job *j,struct fm_frame *f)
{
    j->input=j->fs.open(j->fs.ctx,f->source,0); if (j->input<0) return j->input;
    char parent[FM_PATH]; strcpy(parent,f->target); char *slash=strrchr(parent,'/'); if (slash==parent) slash[1]=0; else *slash=0;
    for (unsigned attempt=0;attempt<32;attempt++) {
        char name[64]; snprintf(name,sizeof(name),".ciuki-copy-%u.tmp",++j->serial);
        int r=fm_join(j->temporary,parent,name); if (r) { j->temporary[0]=0; return r; }
        j->output=j->fs.open(j->fs.ctx,j->temporary,1);
        if (j->output>=0) break;
        r=j->output; j->output=-1; j->temporary[0]=0; if (r!=-EEXIST) return r;
    }
    if (j->output<0) return -EEXIST;
    j->used=j->written=0; f->phase=1; return 0;
}
int fm_job_step(struct fm_job *j)
{
    if (j->state!=FM_RUNNING) return 0;
    if (j->cancel) { int r=cleanup(j); j->cleanup_error=-r; j->state=r?FM_FAILED:FM_CANCELLED; if (r) j->error=-r; return r; }
    if (!j->depth) {
        if (j->next==j->count) { j->state=FM_DONE; return 0; }
        const char *source=j->sources[j->next++]; char target[FM_PATH]=""; int r=0;
        if (j->kind==FM_COPY || j->kind==FM_MOVE) r=fm_join(target,j->destination,fm_basename(source));
        if (!r) r=push(j,source,target); if (r) return fail(j,r);
    }
    struct fm_frame *f=&j->frames[j->depth-1]; int r;
    if (!f->phase) {
        r=j->fs.stat_path(j->fs.ctx,f->source,&f->meta); if (r) return fail(j,r);
        if (f->meta.type==FM_OTHER) return fail(j,-ENOTSUP);
        if (j->kind==FM_PROPERTIES) {
            if (f->meta.type==FM_FILE) { j->files++; j->bytes+=f->meta.size; pop(j); return 1; }
            j->folders++;
        } else if (j->kind==FM_DELETE && f->meta.type==FM_FILE) {
            r=j->fs.unlink(j->fs.ctx,f->source); if (r) return fail(j,r); j->files++; pop(j); return 1;
        } else if (j->kind==FM_COPY || j->kind==FM_MOVE) {
            struct fm_meta to; r=j->fs.stat_path(j->fs.ctx,f->target,&to); f->exists=!r;
            if (r && r!=-ENOENT) return fail(j,r);
            if (f->exists && to.dev==f->meta.dev && to.ino==f->meta.ino) return fail(j,-EINVAL);
            if (f->exists && to.type!=f->meta.type) return fail(j,f->meta.type==FM_DIR?-ENOTDIR:-EISDIR);
            if (f->exists && f->meta.type==FM_FILE && !f->approved && !j->yes_all) { j->state=FM_REPLACE; return 0; }
            if (j->kind==FM_MOVE && !(f->exists && f->meta.type==FM_DIR)) {
                r=j->fs.rename(j->fs.ctx,f->source,f->target);
                if (!r) { if (f->meta.type==FM_FILE) j->files++; else j->folders++; pop(j); return 1; }
                if (r!=-EXDEV) return fail(j,r);
            }
            if (f->meta.type==FM_FILE) { r=begin_file(j,f); if (r) return fail(j,r); return 1; }
            if (!f->exists) { r=j->fs.mkdir(j->fs.ctx,f->target); if (r) return fail(j,r); }
            j->folders++;
        }
        struct listing l; r=listing(&j->fs,f->source,&l); if (r) return fail(j,r);
        f->entries=l.entries; f->count=l.count; f->phase=2; return 1;
    }
    if (f->phase==1) {
        if (j->written<j->used) {
            r=j->fs.write(j->fs.ctx,j->output,j->buffer+j->written,j->used-j->written);
            if (r==-EINTR) return 1; if (r<=0) return fail(j,r?r:-EIO);
            if ((unsigned)r>j->used-j->written) return fail(j,-EIO);
            j->written+=(unsigned)r; j->bytes+=(unsigned)r; return 1;
        }
        r=j->fs.read(j->fs.ctx,j->input,j->buffer,FM_CHUNK);
        if (r==-EINTR) return 1; if (r<0) return fail(j,r); if (r>FM_CHUNK) return fail(j,-EIO);
        if (r) { j->used=(unsigned)r; j->written=0; return 1; }
        r=j->fs.sync(j->fs.ctx,j->output); if (r) return fail(j,r);
        r=j->fs.close(j->fs.ctx,j->output); j->output=-1; if (r) return fail(j,r);
        r=j->fs.close(j->fs.ctx,j->input); j->input=-1; if (r) return fail(j,r);
        struct fm_meta current; r=j->fs.stat_path(j->fs.ctx,f->target,&current);
        if (!f->exists && !r) { f->exists=1; j->state=FM_REPLACE; f->phase=3; return 0; }
        if (r && r!=-ENOENT) return fail(j,r);
        f->phase=3;
    }
    if (f->phase==3) {
        r=j->fs.rename(j->fs.ctx,j->temporary,f->target); if (r) return fail(j,r); j->temporary[0]=0;
        metadata(j,f);
        if (j->kind==FM_MOVE) { r=j->fs.unlink(j->fs.ctx,f->source); if (r) return fail(j,r); }
        j->files++; pop(j); return 1;
    }
    if (f->next<f->count) {
        struct fm_entry *e=&f->entries[f->next++]; char source[FM_PATH],target[FM_PATH]="";
        r=fm_join(source,f->source,e->name);
        if (!r && (j->kind==FM_COPY || j->kind==FM_MOVE)) r=fm_join(target,f->target,e->name);
        if (!r) r=push(j,source,target); if (r) return fail(j,r); return 1;
    }
    if (j->kind==FM_DELETE || (j->kind==FM_MOVE && !j->skip_seen)) { r=j->fs.rmdir(j->fs.ctx,f->source); if (r) return fail(j,r); }
    if (j->kind==FM_COPY || j->kind==FM_MOVE) metadata(j,f);
    pop(j); return 1;
}
void fm_job_answer(struct fm_job *j,enum fm_replace answer)
{
    if (j->state!=FM_REPLACE) return;
    j->state=FM_RUNNING;
    if (answer==FM_CANCEL) { j->cancel=1; return; }
    if (answer==FM_NO) {
        int r=cleanup(j); if (r) { fail(j,r); return; }
        j->skip_seen=1; j->skipped++; pop(j); return;
    }
    j->frames[j->depth-1].approved=1; if (answer==FM_YES_ALL) j->yes_all=1;
}
void fm_job_cancel(struct fm_job *j) { j->cancel=1; if (j->state==FM_REPLACE) j->state=FM_RUNNING; }
void fm_job_destroy(struct fm_job *j)
{
    if (j->fs.close) cleanup(j);
    for (unsigned i=0;i<j->depth;i++) free_frame(&j->frames[i]);
    for (unsigned i=0;i<j->count;i++) free(j->sources?j->sources[i]:NULL); free(j->sources);
    memset(j,0,sizeof(*j)); j->input=j->output=-1;
}
