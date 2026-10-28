/* Real destination reads, barriers, tree hashing and profile installation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "owner_internal.h"
#include "owner_vfs.h"

static int absolute(struct owner_vfs *b,const char *path,char out[FS_PATH_BYTES])
{
    size_t n=strlen(path); if(path[0]!='/' || n+3>=FS_PATH_BYTES) return -ENAMETOOLONG;
    out[0]=(char)('A'+b->drive); out[1]=':'; memcpy(out+2,path,n+1); return 0;
}
static int read_path(struct owner_vfs *b,const char *name,void *p,size_t cap,size_t *bytes)
{
    char path[FS_PATH_BYTES]; int e=absolute(b,name,path); if(e) return e;
    int fd=vfs_open(b->table,path,VFS_READ,VFS_DENY_ALL,0); if(fd<0) return fd;
    struct fat_entry entry; e=vfs_handle_stat(b->table,fd,&entry,0);
    if(!e && (entry.attr&FAT_ATTR_DIR)) e=-EISDIR;
    if(!e && entry.size>cap) e=-ENOSPC;
    size_t got=0;
    while(!e && got<entry.size) {
        size_t n=0; e=vfs_read(b->table,fd,(uint8_t *)p+got,entry.size-got,&n);
        if(!e && !n) e=-EIO; got+=n;
    }
    int close=vfs_close(b->table,fd); if(!e) e=close;
    if(!e) *bytes=got; return e;
}
static const char *const store_names[5]={
    "/System/Security/Owner/store.0", "/System/Security/Owner/store.1",
    "/System/Security/Owner/commit.0", "/System/Security/Owner/commit.1",
    "/System/Security/Owner/checkpoint"
};
static int backend_read(void *ctx,unsigned file,void *p,size_t cap,size_t *bytes)
{ if(file>=5) return -EINVAL; return read_path(ctx,store_names[file],p,cap,bytes); }
static int backend_write(void *ctx,unsigned file,const void *p,size_t bytes)
{
    struct owner_vfs *b=ctx; if(file>=5) return -EINVAL;
    char path[FS_PATH_BYTES]; int e=absolute(b,store_names[file],path); if(e) return e;
    int fd=vfs_open(b->table,path,VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_ALL,FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM);
    if(fd<0) return fd;
    size_t wrote=0; e=vfs_write(b->table,fd,p,bytes,&wrote); if(!e && wrote!=bytes) e=-EIO;
    int close=vfs_close(b->table,fd); return e?e:close;
}
static int backend_flush(void *ctx,unsigned file)
{
    struct owner_vfs *b=ctx; if(file>=5) return -EINVAL;
    char path[FS_PATH_BYTES]; int e=absolute(b,store_names[file],path); if(e) return e;
    int fd=vfs_open(b->table,path,VFS_READ,VFS_DENY_ALL,0); if(fd<0) return fd;
    e=vfs_commit(b->table,fd); int close=vfs_close(b->table,fd); return e?e:close;
}
static int backend_barrier(void *ctx)
{ struct owner_vfs *b=ctx; return vfs_flush_drive(b->table,b->drive); }
static void hex64(char *p,uint64_t g,unsigned digits)
{ const char hex[]="0123456789abcdef"; for(unsigned i=0;i<digits;i++) p[i]=hex[(g>>(4*(digits-i-1)))&15]; }
static void stage_name(char out[96],uint64_t g,uint32_t uid,bool checkpoint)
{
    const char prefix[]="/System/Security/Staging/g"; memset(out,0,96); memcpy(out,prefix,sizeof(prefix)-1);
    size_t n=sizeof(prefix)-1; hex64(out+n,g,16); n+=16; out[n++]='/';
    if(checkpoint) memcpy(out+n,"checkpoint",11);
    else { out[n++]='p'; hex64(out+n,uid,8); }
}
static int backend_manifest(void *ctx,uint64_t g,bool checkpoint,void *p,size_t cap,size_t *bytes)
{
    char path[96]; stage_name(path,g,0,true);
    if(!checkpoint) { size_t n=strlen(path)-10; memcpy(path+n,"manifest",9); }
    return read_path(ctx,path,p,cap,bytes);
}
static int ascii_compare(const char *a,const char *b)
{
    while(*a && *b) { unsigned x=(uint8_t)*a++, y=(uint8_t)*b++; if(x>='A' && x<='Z') x+=32; if(y>='A' && y<='Z') y+=32; if(x!=y) return x<y?-1:1; }
    return (uint8_t)*a-(uint8_t)*b;
}
struct tree_node {
    struct tree_node *next;
    char relative[FS_PATH_BYTES], alias[40];
    uint8_t digest[32]; uint64_t bytes;
    unsigned kind;
};
static struct tree_node *tree_sort(struct tree_node *list)
{
    if(!list || !list->next) return list;
    struct tree_node *slow=list,*fast=list->next;
    while(fast && fast->next) { slow=slow->next; fast=fast->next->next; }
    struct tree_node *right=slow->next; slow->next=0; list=tree_sort(list); right=tree_sort(right);
    struct tree_node *head=0,**tail=&head;
    while(list || right) {
        struct tree_node **take=!right || (list && ascii_compare(list->relative,right->relative)<0)?&list:&right;
        *tail=*take; *take=(*take)->next; tail=&(*tail)->next;
    }
    return head;
}
static int file_digest(struct vfs_table *t,const char *path,uint64_t expected,uint8_t digest[32])
{
    int fd=vfs_open(t,path,VFS_READ,VFS_DENY_ALL,0); if(fd<0) return fd;
    uint8_t *data=fs_page_alloc(); int e=data?0:-ENOMEM; struct sha256_ctx hash; sha256_init(&hash); uint64_t read=0;
    while(!e && read<expected) {
        size_t n=0,want=expected-read>4096?4096:(size_t)(expected-read);
        e=vfs_read(t,fd,data,want,&n); if(!e && !n) e=-EIO;
        if(!e) { sha256_update(&hash,data,n); read+=n; }
    }
    if(!e) sha256_final(&hash,digest);
    fs_page_free(data); int close=vfs_close(t,fd); return e?e:close;
}
int owner_vfs_tree_digest(struct vfs_table *t,const char *root,uint8_t digest[32])
{
    if(!t || !root || !digest) return -EINVAL;
    unsigned drive; char pattern[FS_PATH_BYTES];
    int e=path_normalize(root,t->drive,"/",&drive,pattern); if(e) return e;
    struct fat_volume *volume=t->vfs->volumes[drive]; if(!volume) return -EIO;
    uint64_t sequence=volume->mutation_sequence; uint32_t generation=t->vfs->generation[drive];
    struct fat_entry root_entry; e=vfs_stat(t,root,&root_entry);
    if(e) return e; if(!(root_entry.attr&FAT_ATTR_DIR)) return -ENOTDIR;
    struct tree_node *head=oalloc(sizeof(*head)); if(!head) return -ENOMEM;
    memset(head,0,sizeof(*head)); head->kind=1; struct tree_node *tail=head;
    for(struct tree_node *directory=head;!e && directory;directory=directory->next) {
        if(directory->kind!=1) continue;
        size_t rn=strlen(root), dn=strlen(directory->relative);
        if(rn+dn+4>=sizeof(pattern)) { e=-ENAMETOOLONG; break; }
        memcpy(pattern,root,rn); size_t at=rn;
        if(dn) { pattern[at++]='/'; memcpy(pattern+at,directory->relative,dn); at+=dn; }
        pattern[at++]='/'; pattern[at++]='*'; pattern[at]=0;
        struct vfs_find find; struct fat_entry entry;
        int found=vfs_find_first(t,pattern,FAT_ATTR_DIR|FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM,&find,&entry);
        while(!e && !found) {
            if(path_equal(entry.name,".") || path_equal(entry.name,"..")) { found=vfs_find_next(t,&find,&entry); continue; }
            for(const char *p=entry.name;*p;p++) if((uint8_t)*p<32 || (uint8_t)*p>126 || *p=='/' || *p=='\\' || *p==':') { e=-EILSEQ; break; }
            if(e) break;
            size_t len=strlen(entry.name), parent=dn?dn+1:0;
            if(parent+len+1>FS_PATH_BYTES) { e=-ENAMETOOLONG; break; }
            struct tree_node *node=oalloc(sizeof(*node)); if(!node) { e=-ENOMEM; break; }
            memset(node,0,sizeof(*node)); if(dn) { memcpy(node->relative,directory->relative,dn); node->relative[dn]='/'; }
            memcpy(node->relative+parent,entry.name,len+1); memcpy(node->alias,entry.alias,sizeof(node->alias));
            node->kind=entry.attr&FAT_ATTR_DIR?1:2; node->bytes=node->kind==1?0:entry.size;
            /* A long/short collision in the same directory is ambiguous even
             * if the raw long names sort differently. */
            for(struct tree_node *old=head;old;old=old->next) {
                const char *leaf=old->relative; for(const char *p=leaf;*p;p++) if(*p=='/') leaf=p+1;
                size_t old_parent=(size_t)(leaf-old->relative);
                if(old_parent==parent && !memcmp(old->relative,node->relative,parent) &&
                    (path_equal(leaf,entry.name) || path_equal(leaf,entry.alias) || path_equal(old->alias,entry.name) || path_equal(old->alias,entry.alias))) { e=-EIO; break; }
            }
            if(e) { fs_free(node); break; }
            tail->next=node; tail=node;
            if(node->kind==2) {
                char *path=pattern; size_t n=strlen(node->relative);
                if(rn+n+2>FS_PATH_BYTES) { e=-ENAMETOOLONG; break; }
                memcpy(path,root,rn); path[rn]='/'; memcpy(path+rn+1,node->relative,n+1);
                e=file_digest(t,path,node->bytes,node->digest);
            }
            found=vfs_find_next(t,&find,&entry);
        }
        if(!e && found!=-ENOENT) e=found;
    }
    if(!e && (sequence!=volume->mutation_sequence || generation!=t->vfs->generation[drive])) e=-EAGAIN;
    if(!e) {
        head=tree_sort(head); struct sha256_ctx hash; sha256_init(&hash);
        const char *prior=0;
        for(struct tree_node *n=head;n;n=n->next) {
            if(prior && ascii_compare(prior,n->relative)>=0) { e=-EIO; break; }
            uint8_t fields[12]; uint32_t length=(uint32_t)strlen(n->relative)+1;
            fs_wr32(fields,length); sha256_update(&hash,fields,4); sha256_update(&hash,n->relative,length);
            fs_wr32(fields,n->kind); op64(fields+4,n->bytes); sha256_update(&hash,fields,12); sha256_update(&hash,n->digest,32); prior=n->relative;
        }
        if(!e) sha256_final(&hash,digest);
    }
    while(head) { struct tree_node *next=head->next; fs_free(head); head=next; }
    return e;
}
static int backend_tree(void *ctx,uint64_t g,uint32_t uid,uint8_t digest[32])
{
    struct owner_vfs *b=ctx; char name[96], path[FS_PATH_BYTES]; stage_name(name,g,uid,false);
    int e=absolute(b,name,path); if(!e) e=owner_vfs_tree_digest(b->table,path,digest);
    if(e==-ENOENT && b->sealed_g==g && b->account.intent(b->account.ctx,&b->request)==1) {
        for(unsigned i=0;i<b->request.count;i++) if(b->request.bindings[i].uid==uid) {
            e=absolute(b,b->request.bindings[i].path,path); if(!e) e=owner_vfs_tree_digest(b->table,path,digest); break;
        }
    }
    return e;
}
static int entry_proof(struct owner_vfs *b,const char *name,uint8_t proof[32])
{
    char path[FS_PATH_BYTES]; int e=absolute(b,name,path); if(e) return e;
    struct fat_entry entry; e=vfs_stat(b->table,path,&entry); if(e) return e;
    struct fat_volume *v=b->table->vfs->volumes[b->drive]; if(!v) return -EIO;
    owner_object_proof(v,&entry,proof); return 0;
}
static int backend_reserve(void *ctx,const struct owner_request *q,struct owner_record *records,bool recheck)
{
    struct owner_vfs *b=ctx;
    if(q->volume!=b->volume || (b->sealed_g && (b->sealed_g!=q->transaction || memcmp(b->sealed_digest,q->digest,32)))) return -EIO;
    b->sealed_g=q->transaction; memcpy(b->sealed_digest,q->digest,32);
    b->request=*q;
    memset(b->prior_roots,0,sizeof(b->prior_roots));
    uint8_t *manifest=oalloc(OWNER_MANIFEST_MAX); if(!manifest) return -ENOMEM;
    size_t bytes=0; int result=backend_manifest(b,q->transaction,false,manifest,OWNER_MANIFEST_MAX,&bytes);
    if(result || bytes!=64+256*q->count) { ofree(manifest,OWNER_MANIFEST_MAX); return result?result:-EIO; }
    for(unsigned i=0;i<q->count;i++) {
        uint8_t proof[32]; char stage[96]; stage_name(stage,q->transaction,q->bindings[i].uid,false);
        const uint8_t *entry=manifest+64+256*i;
        if(fs_rd32(entry+12)&2) { memcpy(b->prior_roots[i],entry+80,64); b->prior_roots[i][63]=0; }
        bool retained=(fs_rd32(entry+12)&2) && !(fs_rd32(entry+12)&4);
        const char *name=q->bindings[i].action==OWNER_CREATE && !retained?stage:q->bindings[i].path;
        int e=entry_proof(b,name,proof);
        if(e==-ENOENT && recheck && q->bindings[i].action==OWNER_CREATE) e=entry_proof(b,q->bindings[i].path,proof);
        if(e) { result=e; break; }
        if(recheck && memcmp(proof,records[i].proof,32)) { result=-EIO; break; }
        if(!recheck) {
            if(q->bindings[i].action!=OWNER_CREATE && memcmp(proof,records[i].proof,32)) { result=-EIO; break; }
            memcpy(records[i].proof,proof,32);
            if(q->bindings[i].action==OWNER_CREATE) {
                uint8_t existing[32]; int found=entry_proof(b,q->bindings[i].path,existing);
                if(retained) {
                    uint8_t tree[32]; char path[FS_PATH_BYTES];
                    e=absolute(b,q->bindings[i].path,path); if(!e) e=owner_vfs_tree_digest(b->table,path,tree);
                    if(!e && memcmp(tree,entry+208,32)) e=-EIO;
                    if(e) { result=e; break; }
                } else if(found!=-ENOENT) { result=found?found:-EEXIST; break; }
            }
        }
    }
    ofree(manifest,OWNER_MANIFEST_MAX); return result;
}
static int backend_publish(void *ctx,const struct owner_request *q,struct owner_record *records)
{
    struct owner_vfs *b=ctx;
    if(b->sealed_g!=q->transaction || memcmp(b->sealed_digest,q->digest,32)) return -EIO;
    for(unsigned i=0;i<q->count;i++) if(q->bindings[i].action==OWNER_CREATE) {
        char stage[96],from[FS_PATH_BYTES],to[FS_PATH_BYTES]; stage_name(stage,q->transaction,q->bindings[i].uid,false);
        int e=absolute(b,stage,from); if(!e) e=absolute(b,q->bindings[i].path,to); if(e) return e;
        uint8_t proof[32]; e=entry_proof(b,q->bindings[i].path,proof);
        if(!e) { if(memcmp(proof,records[i].proof,32)) return -EIO; continue; }
        if(e!=-ENOENT) return e;
        /* This API combines validation and mutation errors: a failed rename
         * is uncertain, unlike either read-only proof lookup around it. */
        e=vfs_rename(b->table,from,to); if(e) return e==-EAGAIN?-EIO:e;
        e=entry_proof(b,q->bindings[i].path,proof);
        if(e) return e;
        if(memcmp(proof,records[i].proof,32)) return -EIO;
    }
    return 0;
}
static int backend_release(void *ctx,const struct owner_request *q,bool committed)
{
    struct owner_vfs *b=ctx; (void)committed;
    if(b->sealed_g && b->sealed_g!=q->transaction) return -EIO;
    b->sealed_g=0; memset(b->sealed_digest,0,32); return 0;
}
static int backend_intent(void *ctx,const struct owner_request *q)
{ struct owner_vfs *b=ctx; return b->account.intent(b->account.ctx,q); }
static int backend_current(void *ctx,uint64_t *g,uint8_t digest[32])
{ struct owner_vfs *b=ctx; return b->account.current(b->account.ctx,g,digest); }
static int backend_checkpoint(void *ctx,const struct owner_request *q,uint64_t high,uint32_t uid)
{ struct owner_vfs *b=ctx; return b->account.checkpoint(b->account.ctx,q,high,uid); }
static int backend_import(void *ctx,uint64_t g,uint32_t old,uint32_t uid,const char *prior,const char *destination)
{ struct owner_vfs *b=ctx; return b->account.import_map(b->account.ctx,g,old,uid,prior,destination); }
static int backend_occupied(void *ctx,const char *root)
{
    struct owner_vfs *b=ctx; char path[FS_PATH_BYTES]; int e=absolute(b,root,path); if(e) return e;
    struct fat_entry entry; e=vfs_stat(b->table,path,&entry); return e==-ENOENT?0:e?e:1;
}
static int backend_verify(void *ctx,const struct owner_record *r)
{
    uint8_t proof[32]; int e=entry_proof(ctx,r->binding.path,proof);
    if(e==-ENOENT && r->retired && !owner_profile_path(r->binding.path)) return 0;
    return e?e:memcmp(proof,r->proof,32)?-EIO:0;
}
int owner_vfs_backend(struct owner_vfs *b,struct vfs_table *t,uint64_t volume,const struct owner_account_evidence *a,struct owner_backend *out)
{
    if(!b || !t || !volume || !a || !out || !a->intent || !a->current || !a->checkpoint || !a->import_map ||
       t->drive>=26 || !t->vfs->volumes[t->drive]) return -EINVAL;
    memset(b,0,sizeof(*b)); b->table=t; b->drive=t->drive; b->volume=volume; b->account=*a; b->initialized=true;
    t->vfs->volumes[t->drive]->strict_names=true;
    *out=(struct owner_backend){b,backend_read,backend_write,backend_flush,backend_barrier,backend_manifest,backend_tree,
        backend_reserve,backend_publish,backend_release,backend_intent,backend_current,backend_checkpoint,backend_import,backend_occupied,backend_verify};
    return 0;
}
bool owner_vfs_reserved(const struct owner_vfs *b,const char *path)
{
    if(!b || !b->sealed_g) return false;
    if(!path) return true;
    uint16_t requested[FS_PATH_CHARS+1]; unsigned length;
    if(path[0]!='/' || path_to_ucs2(path,requested,FS_PATH_CHARS+1,&length)) return true;
    char staging[96]; stage_name(staging,b->sealed_g,0,true); staging[strlen(staging)-11]=0;
    /* Protect full object/child paths, plus ancestors against rename/delete.
     * Sibling ACCOUNT journal/DB paths stay writable during the protocol. k03
     * passes resolved within-volume names, not input aliases or ADD's parent. */
    for(unsigned i=0;i<1+2*b->request.count;i++) {
        const char *protected=i==0?staging:i<=b->request.count?b->request.bindings[i-1].path:b->prior_roots[i-b->request.count-1];
        if(!*protected) continue;
        uint16_t target[FS_PATH_CHARS+1]; unsigned n;
        if(path_to_ucs2(protected,target,FS_PATH_CHARS+1,&n)) return true;
        unsigned at=0,common=length<n?length:n;
        while(at<common && path_upper(requested[at])==path_upper(target[at])) at++;
        if(at!=common) continue;
        if(length==n || (length<n && (requested[length-1]=='/' || target[length]=='/')) ||
            (n<length && (target[n-1]=='/' || requested[n]=='/'))) return true;
    }
    return false;
}
