/* SPDX-License-Identifier: MIT. Included by transfer.c: same VFS publication discipline. */
#include "navigation.h"
#include "../desktop/profile.h"

struct fm_folder_view *fm_folder_view(struct fm_navigation *n, const char *path, int search)
{
    for (unsigned i=0;i<n->folders;i++)
        if (n->folder[i].search==!!search && fm_same_path(n->folder[i].path,path)) return &n->folder[i];
    if (n->folders==FM_FOLDER_VIEWS) {
        memmove(n->folder,n->folder+1,(FM_FOLDER_VIEWS-1)*sizeof(*n->folder)); n->folders--;
    }
    struct fm_folder_view *v=&n->folder[n->folders++]; memset(v,0,sizeof(*v));
    snprintf(v->path,sizeof(v->path),"%s",path); v->search=!!search; v->auto_arrange=1;
    return v;
}
void fm_address_visit(struct fm_navigation *n, const char *path)
{
    unsigned at=0;
    while (at<n->count && !fm_same_path(n->addresses[at],path)) at++;
    if (at==FM_ADDRESSES) at--;
    if (at==n->count && n->count<FM_ADDRESSES) n->count++;
    memmove(n->addresses+1,n->addresses,at*sizeof(*n->addresses));
    snprintf(n->addresses[0],FM_PATH,"%s",path);
}

/* Fixed-width little-endian records; no native padding/pointers enter storage.
 * The whole bounded record is validated before replacing the caller's state. */
#define NAV_BYTES (16u + FM_ADDRESSES*FM_PATH + FM_FOLDER_VIEWS*(FM_PATH+8+4*FM_VIEW_COLUMNS))
static void nav_put(unsigned char *p,unsigned v)
{ for (unsigned k=0;k<4;k++) p[k]=(unsigned char)(v>>(8*k)); }
static unsigned nav_get(const unsigned char *p)
{ return (unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24; }
static int nav_path(char *out,size_t cap)
{
    const char *root=profile_root_path();
    int n=root ? snprintf(out,cap,"%s/Application Data/CiukiOS/Files/navigation.dat",root) :
        snprintf(out,cap,"/system/settings/files.navigation");
    return n<0 || (size_t)n>=cap ? -ENAMETOOLONG : 0;
}
static int nav_string(const unsigned char *p)
{ return memchr(p,0,FM_PATH) && (!p[0] || p[0]=='/' || !strcmp((const char *)p,FM_RECYCLE)); }
int fm_navigation_load(const struct fm_vfs *f,struct fm_navigation *n)
{
    char path[FM_PATH]; int r=nav_path(path,sizeof(path));
    if (r || !f->open || !f->read || !f->close || !f->stat_path) return r?r:-ENOSYS;
    struct fm_meta meta; r=f->stat_path(f->ctx,path,&meta);
    if (r) return r;
    if (meta.type!=FM_FILE || meta.size>NAV_BYTES) return -EINVAL;
    int fd=f->open(f->ctx,path,0); if (fd<0) return fd;
    unsigned char *p=calloc(1,NAV_BYTES+1); unsigned used=0;
    if (!p) { f->close(f->ctx,fd); return -ENOMEM; }
    while (used<NAV_BYTES+1) {
        int got=f->read(f->ctx,fd,p+used,NAV_BYTES+1-used);
        if (got==-EINTR) continue;
        if (got<0) { r=got; break; } if (!got) break;
        if ((unsigned)got>NAV_BYTES+1-used) { r=-EIO; break; } used+=(unsigned)got;
    }
    int closed=f->close(f->ctx,fd); if (!r) r=closed;
    unsigned count=used>=16?nav_get(p+8):0,folders=used>=16?nav_get(p+12):0;
    if (!r && (used<16 || memcmp(p,"CIUKNAV1",8) || count>FM_ADDRESSES || folders>FM_FOLDER_VIEWS ||
        used!=16+count*FM_PATH+folders*(FM_PATH+8+4*FM_VIEW_COLUMNS))) r=-EINVAL;
    unsigned at=16;
    for (unsigned i=0;!r && i<count;i++,at+=FM_PATH) if (!nav_string(p+at)) r=-EINVAL;
    for (unsigned i=0;!r && i<folders;i++) {
        if (!nav_string(p+at) || nav_get(p+at+FM_PATH)>1 || nav_get(p+at+FM_PATH+4)>1) { r=-EINVAL; break; }
        at+=FM_PATH+8;
        for (unsigned k=0;k<FM_VIEW_COLUMNS;k++,at+=4) {
            unsigned w=nav_get(p+at); if (w && (w<24 || w>16384)) r=-EINVAL;
        }
    }
    if (!r) {
        memset(n,0,sizeof(*n)); n->count=count; n->folders=folders; at=16;
        for (unsigned i=0;i<count;i++,at+=FM_PATH) memcpy(n->addresses[i],p+at,FM_PATH);
        for (unsigned i=0;i<folders;i++) {
            struct fm_folder_view *v=&n->folder[i]; memcpy(v->path,p+at,FM_PATH); at+=FM_PATH;
            v->search=nav_get(p+at); v->auto_arrange=nav_get(p+at+4); at+=8;
            for (unsigned k=0;k<FM_VIEW_COLUMNS;k++,at+=4) v->widths[k]=(int)nav_get(p+at);
        }
    }
    free(p); return r;
}
int fm_navigation_save(const struct fm_vfs *f,const struct fm_navigation *n)
{
    if (n->count>FM_ADDRESSES || n->folders>FM_FOLDER_VIEWS) return -EINVAL;
    if (!f->mkdir || !f->open || !f->write || !f->sync || !f->close || !f->rename || !f->unlink) return -ENOSYS;
    char path[FM_PATH],temp[FM_PATH+24]; int r=nav_path(path,sizeof(path)); if (r) return r;
    char parent[FM_PATH]; strcpy(parent,path);
    for (char *p=parent+1;*p;p++) if (*p=='/') {
        *p=0; r=f->mkdir(f->ctx,parent); *p='/'; if (r && r!=-EEXIST) return r;
    }
    unsigned size=16+n->count*FM_PATH+n->folders*(FM_PATH+8+4*FM_VIEW_COLUMNS);
    unsigned char *p=calloc(1,size); if (!p) return -ENOMEM;
    memcpy(p,"CIUKNAV1",8); nav_put(p+8,n->count); nav_put(p+12,n->folders); unsigned at=16;
    for (unsigned i=0;i<n->count;i++,at+=FM_PATH) memcpy(p+at,n->addresses[i],strlen(n->addresses[i])+1);
    for (unsigned i=0;i<n->folders;i++) {
        const struct fm_folder_view *v=&n->folder[i]; memcpy(p+at,v->path,strlen(v->path)+1); at+=FM_PATH;
        nav_put(p+at,v->search); nav_put(p+at+4,v->auto_arrange); at+=8;
        for (unsigned k=0;k<FM_VIEW_COLUMNS;k++,at+=4) nav_put(p+at,(unsigned)v->widths[k]);
    }
    int fd=-EEXIST;
    for (unsigned i=0;i<1000 && fd==-EEXIST;i++) {
        snprintf(temp,sizeof(temp),"%s.%u.new",path,i); fd=f->open(f->ctx,temp,1);
    }
    if (fd<0) r=fd;
    else {
        r=write_all(f,fd,p,size); if (!r) r=f->sync(f->ctx,fd);
        int closed=f->close(f->ctx,fd); if (!r) r=closed;
        if (!r) r=f->rename(f->ctx,temp,path);
        if (r) f->unlink(f->ctx,temp);
    }
    free(p); return r;
}
static unsigned nav_lower(unsigned c) { return c>='A' && c<='Z'?c+32:c; }
static int nav_prefix(const char *name,const char *prefix)
{
    while (*prefix) { if (nav_lower((unsigned char)*name++)!=nav_lower((unsigned char)*prefix++)) return 0; }
    return 1;
}
struct nav_completion { const struct fm_model *model; struct fm_completions *out; char parent[FM_PATH], prefix[FM_NAME]; };
static int nav_emit(const char *name,const struct fm_meta *meta,void *arg)
{
    struct nav_completion *c=arg;
    if (!strcmp(name,".") || !strcmp(name,"..") || fm_hidden(c->parent,name) || !nav_prefix(name,c->prefix) ||
        (!c->model->show_hidden && (meta->attributes&2)) || (c->model->hide_system && (meta->attributes&4))) return 0;
    if (meta->type!=FM_DIR && meta->type!=FM_FILE) return 0;
    char path[FM_PATH],display[FM_PATH]; if (fm_join(path,c->parent,name) || fm_display_path(display,path)) return 0;
    struct fm_completions *out=c->out;
    unsigned at=0; while (at<out->count && strcmp(out->paths[at],display)<0) at++;
    if (at==FM_ADDRESSES) return 0;
    if (out->count<FM_ADDRESSES) out->count++;
    memmove(out->paths+at+1,out->paths+at,(out->count-at-1)*sizeof(*out->paths));
    strcpy(out->paths[at],display); return 0;
}
int fm_complete(const struct fm_model *m,const char *text,struct fm_completions *out)
{
    out->count=0; char path[FM_PATH]; if (!text[0] || fm_posix_path(path,text)) return 0;
    size_t len=strlen(text);
    int trailing=len && (text[len-1]=='/' || text[len-1]=='\\' || text[len-1]==':');
    char *slash=strrchr(path,'/'); if (!slash) return 0;
    struct nav_completion c={.model=m,.out=out};
    if (strlen(slash+1)>=FM_NAME) return 0;
    if (trailing) strcpy(c.parent,path);
    else { strcpy(c.prefix,slash+1); if (slash==path) strcpy(c.parent,"/"); else { *slash=0; strcpy(c.parent,path); } }
    char canonical[FM_PATH]; int r=fm_canonical(&m->fs,c.parent,canonical); if (r) return r;
    strcpy(c.parent,canonical);
    return m->fs.list(m->fs.ctx,c.parent,FM_LIST_NAMES_ONLY,nav_emit,&c);
}
struct nav_target { const char *wanted; char name[FM_NAME]; int directory, found; };
static int nav_target_emit(const char *name,const struct fm_meta *meta,void *arg)
{
    struct nav_target *t=arg;
    if (strlen(name)==strlen(t->wanted) && nav_prefix(name,t->wanted)) {
        if (strlen(name)>=sizeof(t->name)) return -ENAMETOOLONG;
        strcpy(t->name,name); t->directory=meta->type==FM_DIR;
        t->found=meta->type==FM_DIR || meta->type==FM_FILE;
    }
    return 0;
}
int fm_address_resolve(const struct fm_model *m,const char *text,char *out,int *directory)
{
    char path[FM_PATH]; int r=fm_posix_path(path,text); if (r) return r;
    if (!path[0] || fm_is_recycle(path) || fm_drive_root(path)) { strcpy(out,path); *directory=1; return 0; }
    char parent[FM_PATH],canonical[FM_PATH]; r=fm_parent(parent,path);
    if (!r) r=fm_canonical(&m->fs,parent,canonical);
    if (r) return r;
    struct nav_target target={.wanted=fm_basename(path)};
    r=m->fs.list(m->fs.ctx,canonical,FM_LIST_NAMES_ONLY,nav_target_emit,&target);
    if (r) return r;
    if (!target.found) return -ENOENT;
    *directory=target.directory;
    return fm_join(out,canonical,target.name);
}
int fm_extension_changes(const struct fm_model *m,unsigned i,const char *name)
{
    if (i>=m->count || m->entries[i].meta.type!=FM_FILE || fm_valid_name(name)) return 0;
    const struct fm_entry *e=&m->entries[i];
    if (m->hide_extensions && fm_kind(e)!=FM_KIND_FILE) return 0;
    const char *a=strrchr(e->name,'.'),*b=strrchr(name,'.');
    if (!a || a==e->name) a="";
    if (!b || b==name) b="";
    while (*a && *b && nav_lower((unsigned char)*a)==nav_lower((unsigned char)*b)) { a++; b++; }
    return *a!=*b;
}
