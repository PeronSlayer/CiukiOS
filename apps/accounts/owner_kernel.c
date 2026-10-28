/* SPDX-License-Identifier: MIT
 * sec-own: production OWNER adapter of the sealed ACCOUNT service. It
 * replaces owner_interim.c in the image (main.c).
 *
 * Transport: the role-bound OWNER link (call 101 SERVICE_CONNECT, service 3)
 * whose server is the kernel supervisor, PID 1 with the OWNER role; CSC1
 * frames as for every security service. Batch operations 1..7 carry the
 * exact contract 3.5 codec (owner.c builds and checks them); the query
 * operations 8..14 carry the fixed 160-byte records documented in
 * src/kernel/fs/owner_system.h. Nothing here is an SDK ABI.
 *
 * Staging (contract 3.5 "OWNER-issued destination identity"): ACCOUNT builds
 * each binding's private tree below /System/Security/Staging/g<G>/p<uid> on
 * the destination volume, hashes it with the shared canonical tree digest and
 * returns the CIUKISTG manifest; store.c writes it durably and OWNER hashes
 * the same bytes independently before reserving anything. A fresh profile
 * receives the shell-folder skeleton the window server registers
 * (apps/w32srv profile_folders) plus Settings; a retained G32 root
 * (enrollment) is copied into staging from its proven old root.
 */
#include "production.h"
#include "accounts.h"
#include "kernel_sys.h"
#include "wire.h"
#include <ciuki/abi.h>
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSC1 0x31435343u
#define OWNER_SERVICE 3u
#define OWNER_ROLE 4u
#define QUERY_BYTES 160u
#define RPC_TIMEOUT_MS 5000u /* CIUKI_SECURITY_SERVICE_TIMEOUT_MS */
#define STAGING_ROOT "/System/Security/Staging"
#define SNAPSHOT_FILE "/System/Security/Owner/snapshot"
#define TREE_NODES 8192u
#define TREE_PATH 520u
enum { Q_IDENTITY=8, Q_GENERATION, Q_BINDING, Q_ROOT, Q_SNAPSHOT, Q_ORPHANS, Q_FLOOR };

/* ---- transport ---- */
static void link_close(struct acct_owner_link *l)
{
    if (l->fd>=0) (void)acct_sys_close(l->fd);
    l->fd=-1; l->endpoint=l->peer_pid=0;
}
static int link_connect(struct acct_owner_link *l)
{
    if (l->fd>=0) return 0;
    struct ciuki_security_link in, out; memset(&in,0,sizeof(in)); memset(&out,0,sizeof(out));
    in.version=1; in.size=64; in.service=OWNER_SERVICE;
    struct ciuki_security_args a; memset(&a,0,sizeof(a));
    a.version=1; a.size=64; a.operation=CIUKI_SECURITY_SERVICE_CONNECT;
    int e=acct_sys_security(&a,&in,64,&out,64);
    if (e) return e;
    /* The OWNER server is the kernel supervisor itself, never a user process. */
    if (out.endpoint_fd<0 || !out.endpoint_generation || out.service!=OWNER_SERVICE ||
        out.server_role!=OWNER_ROLE || out.peer_pid!=1) {
        if (out.endpoint_fd>=0) (void)acct_sys_close(out.endpoint_fd);
        return -EIO;
    }
    l->fd=out.endpoint_fd; l->endpoint=out.endpoint_generation; l->peer_pid=out.peer_pid;
    return 0;
}
/* One request/reply exchange. A transport failure closes the link (the next
 * call reconnects); a status reply leaves it usable. Durable OWNER STATUS
 * resolves a reply lost after admission (contract 3.5). */
static int rpc(struct acct_owner_link *l, unsigned op, const void *in, size_t n, void *out, size_t cap, size_t *used)
{
    *used=0;
    if (!n || n>CIUKI_SECURITY_SERVICE_BODY_MAX) return -EINVAL;
    int e=link_connect(l); if (e) return e;
    if (l->serial==UINT64_MAX) { link_close(l); return -EOVERFLOW; }
    uint64_t id=++l->serial, deadline=acct_sys_monotonic_ms()+RPC_TIMEOUT_MS;
    struct ciuki_message m;
    for (size_t at=0;at<n;) {
        memset(&m,0,sizeof(m)); uint8_t *h=m.data; size_t take=n-at>192?192:n-at;
        w32(h,CSC1); w32(h+4,1); w32(h+8,64); w32(h+12,OWNER_SERVICE); w32(h+16,op); w32(h+20,1);
        w64(h+24,id); w32(h+32,l->endpoint); w32(h+36,(uint32_t)take); w32(h+52,(uint32_t)n); w32(h+56,(uint32_t)at);
        memcpy(h+64,(const uint8_t *)in+at,take); m.length=(uint32_t)(64+take);
        e=acct_sys_channel_send(l->fd,&m);
        if (e==-EAGAIN && acct_sys_monotonic_ms()<deadline) { (void)acct_sys_wait_readable(NULL,0,1); continue; }
        if (e) goto fail;
        at+=take;
    }
    size_t at=0, total=0; int first=1;
    while (first || at<total) {
        int got=acct_sys_channel_recv(l->fd,&m,0);
        if (got==-EAGAIN) {
            uint64_t now=acct_sys_monotonic_ms();
            if (now>=deadline) { e=-ETIMEDOUT; goto fail; }
            int fds[1]={l->fd}, w=acct_sys_wait_readable(fds,1,(unsigned)(deadline-now));
            if (w<0) { e=w; goto fail; }
            continue;
        }
        if (got!=1) { e=got<0?got:-ESRCH; goto fail; }
        const uint8_t *h=m.data; uint32_t count=a32(h+36), length=a32(h+52);
        if (m.fd_count || m.sender_pid!=l->peer_pid || m.length<64 || a32(h)!=CSC1 || a32(h+4)!=1 || a32(h+8)!=64 ||
            a32(h+12)!=OWNER_SERVICE || a32(h+16)!=op || a32(h+20)!=2 || a64(h+24)!=id || a32(h+32)!=l->endpoint ||
            count>192 || m.length!=64+count || a32(h+56)!=at || a32(h+60)) { e=-EIO; goto fail; }
        int32_t status=(int32_t)a32(h+48);
        if (status) {
            if (status>0 || at || length || count) { e=-EIO; goto fail; }
            return status;
        }
        if (first) { total=length; first=0; if (total>cap) { e=-EIO; goto fail; } }
        else if (total!=length) { e=-EIO; goto fail; }
        if (count>total-at || (!count && total)) { e=-EIO; goto fail; }
        memcpy((uint8_t *)out+at,h+64,count); at+=count;
    }
    *used=at; return 0;
fail:
    link_close(l); return e;
}
static int query(struct acct_owner_link *l, unsigned op, uint64_t volume, uint32_t uid, uint64_t transaction,
                 uint32_t uid_high, uint64_t accounts_generation, const uint8_t digest[32], const char *path,
                 uint8_t reply[QUERY_BYTES])
{
    uint8_t q[QUERY_BYTES]; memset(q,0,sizeof(q));
    w32(q,1); w32(q+4,QUERY_BYTES); w32(q+8,op); w32(q+12,uid); w64(q+16,volume); w64(q+24,transaction);
    w32(q+32,uid_high); w64(q+40,accounts_generation);
    if (digest) memcpy(q+48,digest,32);
    if (path) { size_t n=strlen(path); if (n>=64) return -ENAMETOOLONG; memcpy(q+80,path,n); }
    size_t used=0; int e=rpc(l,op,q,sizeof(q),reply,QUERY_BYTES,&used);
    if (!e && (used!=QUERY_BYTES || a32(reply)!=1 || a32(reply+4)!=QUERY_BYTES || a32(reply+8)!=op ||
               a32(reply+12)>2 || (op!=Q_IDENTITY && a64(reply+16)!=volume) || !azero(reply+136,24))) e=-EIO;
    return e;
}
int acct_owner_kernel_identity(struct acct_owner_link *l, uint64_t *volume, uint32_t *store_open)
{
    uint8_t r[QUERY_BYTES]; int e=query(l,Q_IDENTITY,0,0,0,0,0,NULL,NULL,r);
    if (!e && !a64(r+16)) e=-EIO;
    if (!e) { l->volume=a64(r+16); if (volume) *volume=l->volume; if (store_open) *store_open=a32(r+12); }
    return e;
}

/* ---- acct_owner callbacks ---- */
static int k_authenticate(void *ctx) { return link_connect(ctx); }
static int k_generation(void *ctx, uint64_t *g)
{
    struct acct_owner_link *l=ctx; uint8_t r[QUERY_BYTES];
    int e=query(l,Q_GENERATION,l->volume,0,0,0,0,NULL,NULL,r);
    if (!e) *g=a64(r+24);
    return e;
}
static int k_binding(void *ctx, uint64_t volume, uint32_t uid, char root[64], struct acct_owner_binding *b)
{
    uint8_t r[QUERY_BYTES]; int e=query(ctx,Q_BINDING,volume,uid,0,0,0,NULL,NULL,r);
    if (e) return e;
    if (a32(r+64)!=uid || a32(r+68)!=1 || !a64(r+48) || !a64(r+56) || !apadded(r+72,64)) return -EIO;
    memset(b,0,sizeof(*b));
    b->uid=uid; b->action=ACCT_OWNER_CREATE; b->key=a64(r+48); b->generation=a64(r+56); b->volume=volume;
    memcpy(b->root,r+72,64); memcpy(root,r+72,64);
    return 0;
}
static int k_root_occupied(void *ctx, uint64_t volume, const char *root)
{
    uint8_t r[QUERY_BYTES]; int e=query(ctx,Q_ROOT,volume,0,0,0,0,NULL,root,r);
    return e?e:a32(r+44)?1:0;
}
static int k_recover_orphans(void *ctx, uint64_t volume, uint64_t active)
{ uint8_t r[QUERY_BYTES]; return query(ctx,Q_ORPHANS,volume,0,active,0,0,NULL,NULL,r); }
static int k_checkpoint_floor(void *ctx, uint64_t *floor, unsigned *receipts)
{
    struct acct_owner_link *l=ctx; uint8_t r[QUERY_BYTES];
    int e=query(l,Q_FLOOR,l->volume,0,0,0,0,NULL,NULL,r);
    if (!e) { *floor=a64(r+32); *receipts=a32(r+40); }
    return e;
}
static int k_call(void *ctx, const void *request, size_t n, void *reply, size_t cap, size_t *bytes)
{
    const uint8_t *p=request;
    if (!p || n<12) return -EINVAL;
    return rpc(ctx,a32(p+8),request,n,reply,cap,bytes);
}
static int read_whole(const struct acct_tree_io *t, const char *path, uint8_t *p, size_t want)
{
    for (size_t at=0;at<want;) {
        size_t got=0; int e=t->read(t->ctx,path,at,p+at,want-at,&got);
        if (e) return e;
        if (!got) return -EIO;
        at+=got;
    }
    return 0;
}
/* The candidate image goes through OWNER's SYSTEM-only snapshot file; the
 * reply's size and digest authenticate the bytes read back here. */
static int k_snapshot(void *ctx, const struct acct_db *d, uint64_t high, uint32_t uid_high, void *out, size_t cap,
                      size_t *bytes, uint64_t *floor)
{
    struct acct_owner_link *l=ctx;
    uint8_t *db=malloc(ACCT_FILE_MAX), digest[32], r[QUERY_BYTES]; size_t n=0;
    if (!db) return -ENOMEM;
    int e=acct_encode(d,0,db,ACCT_FILE_MAX,&n);
    if (!e) sha256(db,n,digest);
    acct_wipe(db,ACCT_FILE_MAX); free(db);
    if (!e) e=query(l,Q_SNAPSHOT,l->volume,0,high,uid_high,d->generation,digest,NULL,r);
    if (e) return e;
    uint64_t size=a64(r+72);
    if (size<160 || size>cap) return size>cap?-ENOSPC:-EIO;
    e=read_whole(l->tree,SNAPSHOT_FILE,out,(size_t)size);
    if (!e) { sha256(out,(size_t)size,digest); if (memcmp(digest,r+80,32)) e=-EIO; }
    if (!e) { *bytes=(size_t)size; *floor=a64(r+32); }
    return e;
}

/* ---- staging trees and the canonical tree digest ---- */
struct tree_node { char *relative; int directory; uint64_t size; uint8_t digest[32]; };
struct tree_walk {
    const struct acct_tree_io *t;
    struct tree_node *nodes;
    size_t count, capacity;
    const char *parent; /* relative path of the directory being listed */
    int error;
};
static int fold_compare(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned x=(unsigned char)*a++, y=(unsigned char)*b++;
        if (x>='A' && x<='Z') x+=32;
        if (y>='A' && y<='Z') y+=32;
        if (x!=y) return x<y?-1:1;
    }
    return (unsigned char)*a-(unsigned char)*b;
}
static int node_compare(const void *a, const void *b)
{ return fold_compare(((const struct tree_node *)a)->relative,((const struct tree_node *)b)->relative); }
static int join(char out[TREE_PATH], const char *base, const char *relative)
{
    int n=*relative?snprintf(out,TREE_PATH,"%s/%s",base,relative):snprintf(out,TREE_PATH,"%s",base);
    return n<0 || n>=(int)TREE_PATH?-ENAMETOOLONG:0;
}
static int walk_add(void *arg, const struct acct_tree_entry *entry)
{
    struct tree_walk *w=arg;
    for (const char *p=entry->name;*p;p++)
        if ((unsigned char)*p<32 || (unsigned char)*p>126 || *p=='/' || *p=='\\' || *p==':') return -EILSEQ;
    if (!entry->name[0]) return -EILSEQ;
    if (w->count==TREE_NODES) return -E2BIG;
    if (w->count==w->capacity) {
        size_t next=w->capacity?w->capacity*2:64;
        struct tree_node *grown=realloc(w->nodes,next*sizeof(*grown));
        if (!grown) return -ENOMEM;
        w->nodes=grown; w->capacity=next;
    }
    size_t pn=strlen(w->parent), nn=strlen(entry->name);
    if (pn+nn+2>260) return -ENAMETOOLONG;
    char *relative=malloc(pn+nn+2); if (!relative) return -ENOMEM;
    if (pn) { memcpy(relative,w->parent,pn); relative[pn]='/'; memcpy(relative+pn+1,entry->name,nn+1); }
    else memcpy(relative,entry->name,nn+1);
    struct tree_node *node=&w->nodes[w->count++]; memset(node,0,sizeof(*node));
    node->relative=relative; node->directory=entry->directory; node->size=entry->directory?0:entry->size;
    return 0;
}
static void walk_free(struct tree_walk *w)
{
    for (size_t i=0;i<w->count;i++) free(w->nodes[i].relative);
    free(w->nodes); memset(w,0,sizeof(*w));
}
static int file_digest(const struct acct_tree_io *t, const char *path, uint64_t size, uint8_t digest[32])
{
    uint8_t *chunk=malloc(4096); if (!chunk) return -ENOMEM;
    struct sha256_ctx h; sha256_init(&h); int e=0;
    for (uint64_t at=0;!e && at<size;) {
        size_t want=size-at>4096?4096:(size_t)(size-at), got=0;
        e=t->read(t->ctx,path,at,chunk,want,&got);
        if (!e && !got) e=-EIO;
        if (!e) { sha256_update(&h,chunk,got); at+=got; }
    }
    if (!e) {
        /* A file that grew while hashed is a concurrent change. */
        size_t extra=0; uint8_t probe; e=t->read(t->ctx,path,size,&probe,1,&extra);
        if (!e && extra) e=-EAGAIN;
    }
    if (!e) sha256_final(&h,digest);
    free(chunk); return e;
}
/* Canonical stream of contract 3.5: per node u32 path bytes (with NUL), the
 * path, u32 kind (1 directory, 2 file), u64 length, SHA-256 of the file (32
 * zero bytes for a directory); root first as the empty path; nodes sorted by
 * case-folded path. Identical to the kernel's owner_vfs_tree_digest. */
static int tree_digest_at(const struct acct_tree_io *t, const char *root, uint8_t digest[32])
{
    int directory=0; uint64_t size=0;
    int e=t->inspect(t->ctx,root,&directory,&size);
    if (e) return e;
    if (!directory) return -ENOTDIR;
    struct tree_walk w; memset(&w,0,sizeof(w)); w.t=t;
    w.capacity=64; w.nodes=calloc(w.capacity,sizeof(*w.nodes));
    if (!w.nodes) return -ENOMEM;
    w.nodes[0].relative=calloc(1,1); w.nodes[0].directory=1; w.count=1;
    if (!w.nodes[0].relative) { walk_free(&w); return -ENOMEM; }
    char path[TREE_PATH];
    for (size_t i=0;!e && i<w.count;i++) {
        if (!w.nodes[i].directory) continue;
        e=join(path,root,w.nodes[i].relative);
        if (!e) { w.parent=w.nodes[i].relative; e=t->list(t->ctx,path,walk_add,&w); }
    }
    for (size_t i=0;!e && i<w.count;i++) {
        if (w.nodes[i].directory) continue;
        e=join(path,root,w.nodes[i].relative);
        if (!e) e=file_digest(t,path,w.nodes[i].size,w.nodes[i].digest);
    }
    if (!e) {
        qsort(w.nodes,w.count,sizeof(*w.nodes),node_compare);
        struct sha256_ctx h; sha256_init(&h);
        for (size_t i=0;!e && i<w.count;i++) {
            if (i && fold_compare(w.nodes[i-1].relative,w.nodes[i].relative)>=0) { e=-EIO; break; }
            uint8_t fields[12]; uint32_t length=(uint32_t)strlen(w.nodes[i].relative)+1;
            w32(fields,length); sha256_update(&h,fields,4); sha256_update(&h,w.nodes[i].relative,length);
            w32(fields,w.nodes[i].directory?1:2); w64(fields+4,w.nodes[i].size);
            sha256_update(&h,fields,12); sha256_update(&h,w.nodes[i].digest,32);
        }
        if (!e) sha256_final(&h,digest);
    }
    walk_free(&w);
    return e;
}
static int make_parents(const struct acct_tree_io *t, const char *path)
{
    char p[TREE_PATH]; size_t n=strlen(path); if (n>=TREE_PATH) return -ENAMETOOLONG;
    memcpy(p,path,n+1);
    for (char *s=p+1;(s=strchr(s,'/'));s++) {
        *s=0; int e=t->mkdir(t->ctx,p); *s='/';
        if (e && e!=-EEXIST) return e;
    }
    int e=t->mkdir(t->ctx,p);
    return e==-EEXIST?0:e;
}
struct child_list { struct acct_tree_entry *entries; size_t count, capacity; };
static int collect(void *arg, const struct acct_tree_entry *entry)
{
    struct child_list *c=arg;
    if (c->count==TREE_NODES) return -E2BIG;
    if (c->count==c->capacity) {
        size_t next=c->capacity?c->capacity*2:16;
        struct acct_tree_entry *grown=realloc(c->entries,next*sizeof(*grown));
        if (!grown) return -ENOMEM;
        c->entries=grown; c->capacity=next;
    }
    c->entries[c->count++]=*entry; return 0;
}
static int children(const struct acct_tree_io *t, const char *path, struct child_list *c)
{ memset(c,0,sizeof(*c)); int e=t->list(t->ctx,path,collect,c); if (e) { free(c->entries); memset(c,0,sizeof(*c)); } return e; }
/* Unexposed staging of an unreserved generation only (a crash before the
 * allocation watermark reuses G; OWNER's lease refuses a sealed tree). */
static int remove_tree(const struct acct_tree_io *t, const char *path, unsigned depth)
{
    if (depth>32) return -ENAMETOOLONG;
    struct child_list c; int e=children(t,path,&c);
    for (size_t i=0;!e && i<c.count;i++) {
        char child[TREE_PATH]; e=join(child,path,c.entries[i].name);
        if (!e) e=c.entries[i].directory?remove_tree(t,child,depth+1):t->remove(t->ctx,child);
    }
    free(c.entries);
    return e?e:t->rmdir(t->ctx,path);
}
static int copy_tree(const struct acct_tree_io *t, const char *from, const char *to, unsigned depth)
{
    if (depth>32) return -ENAMETOOLONG;
    int e=t->mkdir(t->ctx,to); if (e && e!=-EEXIST) return e;
    struct child_list c; e=children(t,from,&c);
    uint8_t *chunk=e?NULL:malloc(4096); if (!e && !chunk) e=-ENOMEM;
    for (size_t i=0;!e && i<c.count;i++) {
        char a[TREE_PATH], b[TREE_PATH]; e=join(a,from,c.entries[i].name); if (!e) e=join(b,to,c.entries[i].name);
        if (e) break;
        if (c.entries[i].directory) { e=copy_tree(t,a,b,depth+1); continue; }
        uint64_t size=c.entries[i].size;
        if (!size) e=t->write(t->ctx,b,0,"",0);
        for (uint64_t at=0;!e && at<size;) {
            size_t want=size-at>4096?4096:(size_t)(size-at), got=0;
            e=t->read(t->ctx,a,at,chunk,want,&got);
            if (!e && !got) e=-EIO;
            if (!e) e=t->write(t->ctx,b,at,chunk,got);
            at+=got;
        }
        if (!e) e=t->sync(t->ctx,b);
    }
    free(chunk); free(c.entries);
    return e?e:t->sync(t->ctx,to);
}
/* The window server's profile shell folders (apps/w32srv/storage.c
 * profile_folders) and the protected Settings anchor (k06 personal hive,
 * w32srv Settings/Win32, k05 Settings/DOS). */
static const char *const skeleton[]={ "Application Data", "Documents", "Documents/Desktop", "Favorites", "Recent",
    "SendTo", "Settings", "Start Menu", "Start Menu/Programs", "Start Menu/Programs/StartUp", "Templates" };
static int stage_fresh(const struct acct_tree_io *t, const char *staged)
{
    int e=0;
    for (unsigned i=0;!e && i<sizeof(skeleton)/sizeof(*skeleton);i++) {
        char path[TREE_PATH]; e=join(path,staged,skeleton[i]);
        if (!e) { e=t->mkdir(t->ctx,path); if (e==-EEXIST) e=-EIO; }
    }
    for (unsigned i=sizeof(skeleton)/sizeof(*skeleton);!e && i;i--) {
        char path[TREE_PATH]; e=join(path,staged,skeleton[i-1]); if (!e) e=t->sync(t->ctx,path);
    }
    return e;
}
/* Finished generations leave only empty staging directories (KEEP/RETIRE
 * trees, published roots' parents): remove those, best effort. A generation
 * still holding any file (an in-flight manifest, a checkpoint image) or
 * sealed by OWNER's lease is kept untouched. */
static int has_files(const struct acct_tree_io *t, const char *path, unsigned depth)
{
    if (depth>8) return 1;
    struct child_list c; if (children(t,path,&c)) return 1;
    int found=0;
    for (size_t i=0;!found && i<c.count;i++) {
        char child[TREE_PATH];
        found=!c.entries[i].directory || join(child,path,c.entries[i].name) || has_files(t,child,depth+1);
    }
    free(c.entries); return found;
}
static void prune_staging(const struct acct_tree_io *t, const char *current)
{
    struct child_list c; if (children(t,STAGING_ROOT,&c)) return;
    for (size_t i=0;i<c.count;i++) {
        char path[TREE_PATH];
        if (!c.entries[i].directory || c.entries[i].name[0]!='g' || join(path,STAGING_ROOT,c.entries[i].name) ||
            !strcmp(path,current) || has_files(t,path,0)) continue;
        (void)remove_tree(t,path,0);
    }
    free(c.entries);
}
static void staged_path(char out[64], uint64_t g, uint32_t uid)
{ snprintf(out,64,STAGING_ROOT "/g%016llx/p%08x",(unsigned long long)g,(unsigned)uid); }
static int parse_old(const char *root, uint32_t *old)
{
    static const char prefix[]="/Windows/Profiles/u";
    if (strncmp(root,prefix,sizeof(prefix)-1) || strlen(root)!=sizeof(prefix)-1+8) return -EINVAL;
    uint32_t v=0;
    for (const char *p=root+sizeof(prefix)-1;*p;p++) {
        unsigned d=*p>='0' && *p<='9'?(unsigned)(*p-'0'):*p>='a' && *p<='f'?(unsigned)(*p-'a'+10):16u;
        if (d==16) return -EINVAL;
        v=v<<4|d;
    }
    *old=v; return v?0:-EINVAL;
}
static int k_tree_digest(void *ctx, uint64_t transaction, uint32_t uid, uint8_t digest[32]);
static int k_stage(void *ctx, const struct acct_owner_request *q, void *manifest, size_t capacity, size_t *bytes)
{
    struct acct_owner_link *l=ctx; const struct acct_tree_io *t=l->tree;
    size_t n=64+256*(size_t)q->count;
    if (!q->count || q->count>32) return -EINVAL;
    if (capacity<n) return -ENOSPC;
    uint8_t *p=manifest; memset(p,0,n);
    memcpy(p,"CIUKISTG",8); w32(p+8,1); w32(p+12,64); w32(p+16,q->count);
    for (unsigned i=0;i<3;i++) w32(p+24+4*i,q->sid[i]);
    w64(p+40,q->volume); w64(p+48,q->transaction);
    char generation[64]; snprintf(generation,sizeof(generation),STAGING_ROOT "/g%016llx",(unsigned long long)q->transaction);
    int directory=0; uint64_t size=0;
    int e=t->inspect(t->ctx,generation,&directory,&size);
    if (!e) e=directory?remove_tree(t,generation,0):-EIO;
    else if (e==-ENOENT) e=0;
    if (!e) e=make_parents(t,generation);
    if (!e) prune_staging(t,generation);
    for (unsigned i=0;!e && i<q->count;i++) {
        const struct acct_owner_binding *b=&q->bindings[i]; uint8_t *r=p+64+256*i;
        char staged[64], fresh[64]; staged_path(staged,q->transaction,b->uid);
        snprintf(fresh,sizeof(fresh),"/Windows/Profiles/u%08x",(unsigned)b->uid);
        uint32_t old=0, flags=1;
        if (b->action==ACCT_OWNER_CREATE && strcmp(b->root,fresh)) {
            /* Enrollment retains the G32 root: its proven old tree is staged. */
            e=parse_old(b->root,&old); flags=2;
            if (!e) e=copy_tree(t,b->root,staged,0);
        } else {
            e=t->mkdir(t->ctx,staged);
            if (!e && b->action==ACCT_OWNER_CREATE) e=stage_fresh(t,staged);
            if (!e) e=t->sync(t->ctx,staged);
        }
        w32(r,b->uid); w32(r+4,old); w32(r+8,b->action); w32(r+12,flags);
        memcpy(r+16,b->root,strlen(b->root));
        if (old) memcpy(r+80,b->root,strlen(b->root));
        memcpy(r+144,staged,strlen(staged));
        w64(r+240,b->action==ACCT_OWNER_CREATE?0:b->key);
        if (!e) e=k_tree_digest(l,q->transaction,b->uid,r+208);
    }
    if (!e) e=t->sync(t->ctx,generation);
    if (!e) e=t->sync(t->ctx,STAGING_ROOT);
    if (!e) *bytes=n;
    return e;
}
static int read_manifest(const struct acct_tree_io *t, uint64_t g, uint8_t *p, size_t *n)
{
    char path[96]; snprintf(path,sizeof(path),STAGING_ROOT "/g%016llx/manifest",(unsigned long long)g);
    int directory=0; uint64_t size=0; int e=t->inspect(t->ctx,path,&directory,&size);
    if (!e && (directory || size<64 || size>ACCT_MANIFEST_MAX)) e=-EIO;
    if (!e) e=read_whole(t,path,p,(size_t)size);
    if (!e) *n=(size_t)size;
    return e;
}
/* Staging of a published binding was renamed into its canonical root by
 * OWNER COMMIT; recovery then hashes the root the manifest names, as the
 * kernel backend does once ACCOUNT holds COMMIT_REQUIRED. */
static int k_tree_digest(void *ctx, uint64_t transaction, uint32_t uid, uint8_t digest[32])
{
    struct acct_owner_link *l=ctx; char staged[64]; staged_path(staged,transaction,uid);
    int e=tree_digest_at(l->tree,staged,digest);
    if (e!=-ENOENT) return e;
    uint8_t *p=malloc(ACCT_MANIFEST_MAX); size_t n=0; if (!p) return -ENOMEM;
    int m=read_manifest(l->tree,transaction,p,&n);
    for (unsigned i=0;!m && 64+256*(size_t)i<n;i++) {
        const uint8_t *r=p+64+256*i;
        if (a32(r)!=uid || a32(r+8)!=ACCT_OWNER_CREATE || !apadded(r+16,64)) continue;
        char root[64]; memcpy(root,r+16,64);
        e=tree_digest_at(l->tree,root,digest); break;
    }
    free(p); return e;
}
static int k_validate_stage(void *ctx, const struct acct_owner_request *q)
{
    struct acct_owner_link *l=ctx;
    uint8_t *p=malloc(ACCT_MANIFEST_MAX), sum[32]; size_t n=0; if (!p) return -ENOMEM;
    int e=read_manifest(l->tree,q->transaction,p,&n);
    if (!e) e=acct_manifest_validate(q,p,n)?-EIO:0;
    if (!e) { sha256(p,n,sum); if (memcmp(sum,q->manifest_digest,32)) e=-EIO; }
    for (unsigned i=0;!e && i<q->count;i++) {
        uint8_t tree[32]; e=k_tree_digest(l,q->transaction,q->bindings[i].uid,tree);
        if (!e && memcmp(tree,p+64+256*i+208,32)) e=-EIO;
    }
    free(p); return e;
}
int acct_owner_kernel(struct acct_owner_link *l, const struct acct_tree_io *tree, struct acct_owner *out)
{
    if (!l || !tree || !out || !tree->inspect || !tree->mkdir || !tree->list || !tree->read || !tree->write ||
        !tree->rename || !tree->remove || !tree->rmdir || !tree->sync) return -EINVAL;
    memset(l,0,sizeof(*l)); l->fd=-1; l->tree=tree;
    *out=(struct acct_owner){ .ctx=l, .real=1, .authenticate=k_authenticate, .generation=k_generation,
        .binding=k_binding, .root_occupied=k_root_occupied, .stage=k_stage, .tree_digest=k_tree_digest,
        .validate_stage=k_validate_stage, .snapshot=k_snapshot, .recover_orphans=k_recover_orphans,
        .checkpoint_floor=k_checkpoint_floor, .call=k_call };
    return 0;
}

/* ---- DOS saves migration (sec-k05 legacy machine store) ---- */
#define DOS_LEGACY_STORE "/System/DOS/SAVES"
int acct_dos_saves_migrate(const struct acct_tree_io *t, const char *profile_root, unsigned *moved, unsigned *kept)
{
    unsigned done=0, left=0;
    if (moved) *moved=0;
    if (kept) *kept=0;
    if (!t || !profile_root || strncmp(profile_root,"/Windows/Profiles/",18)) return -EINVAL;
    int directory=0; uint64_t size=0;
    int e=t->inspect(t->ctx,DOS_LEGACY_STORE,&directory,&size);
    if (e==-ENOENT) return 0;
    if (!e && !directory) return 0;
    struct child_list c; if (!e) e=children(t,DOS_LEGACY_STORE,&c);
    if (e) return e;
    char destination[TREE_PATH];
    int files=0; for (size_t i=0;i<c.count;i++) files|=!c.entries[i].directory;
    if (files) {
        e=join(destination,profile_root,"Settings/DOS");
        if (!e) e=make_parents(t,destination);
    }
    for (size_t i=0;!e && i<c.count;i++) {
        if (c.entries[i].directory) { left++; continue; }
        char from[TREE_PATH], to[TREE_PATH];
        e=join(from,DOS_LEGACY_STORE,c.entries[i].name); if (!e) e=join(to,destination,c.entries[i].name);
        if (e) break;
        int r=t->rename(t->ctx,from,to);
        /* Present at the destination or still open by a running VM: keep it. */
        if (r==-EEXIST || r==-EBUSY || r==-EACCES) { left++; continue; }
        if (r) { e=r; break; }
        done++;
    }
    free(c.entries);
    if (!e && done) e=t->sync(t->ctx,destination);
    if (!e && done) e=t->sync(t->ctx,DOS_LEGACY_STORE);
    if (moved) *moved=done;
    if (kept) *kept=left;
    return e;
}
