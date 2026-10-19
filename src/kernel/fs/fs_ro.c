/* Host VFS read-only name views; no filesystem-private cache.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/fs_ro.h>
struct ro_page { struct ro_page *next; unsigned count; struct fs_ro_node nodes[4]; };
struct fs_ro_view { struct fs_ro_view *next; struct ro_page *pages; uint64_t directory; unsigned count; };
_Static_assert(sizeof(struct ro_page) <= 4096, "RO view page");
static struct fs_ro_node *at(struct fs_ro_view *v, unsigned i)
{
    for (struct ro_page *p = v->pages; p; p = p->next) {
        if (i < p->count) return &p->nodes[i];
        i -= p->count;
    }
    return 0;
}
static void view_free(struct fs_ro_view *v)
{
    while (v->pages) { fs_service(); struct ro_page *p = v->pages; v->pages = p->next; fs_page_free(p); }
    fs_free(v);
}
int fs_ro_mount(struct fs_ro_volume *v, const struct fs_ro_ops *ops, struct block_cache *c,
                struct blkdev *d, uint64_t start, uint64_t sectors)
{
    if (!v || !ops || !ops->probe || !ops->mount || !ops->unmount || !ops->root || !ops->lookup ||
        !ops->readdir || !ops->read || !ops->metadata || !ops->generation || !c || !d || !sectors || start >= d->capacity || sectors > d->capacity - start) return -FS_EINVAL;
    memset(v, 0, sizeof(*v)); v->ops = ops; v->cache = c; v->dev = d; v->start = start; v->sectors = sectors;
    uint32_t gen = ops->generation(v);
    int e = gen==UINT32_MAX ? -FS_EIO : ops->probe(c, d, start);
    if (!e) e = ops->mount(v);
    if (!e && gen != ops->generation(v)) e = -FS_EIO;
    if (e) { ops->unmount(v); return e; }
    v->mount_generation = gen; v->mounted = true; return 0;
}
void fs_ro_unmount(struct fs_ro_volume *v)
{
    while (v->views) { struct fs_ro_view *p = v->views; v->views = p->next; view_free(p); }
    v->view_count = 0;
    if (v->ops) v->ops->unmount(v);
    cache_invalidate(v->cache, v->dev, true); v->mounted = false;
}
int fs_ro_check(struct fs_ro_volume *v)
{
    return v && v->mounted && !v->dev->quarantined &&
        v->ops->generation(v) == v->mount_generation ? 0 : -FS_EIO;
}
/* Build a collision-checked complete directory before publishing any alias.
 * Names accepted by path_validate_name are UCS-2/UTF-8, case preserving.
 * Valid supplied DOS aliases are preserved. Generated tails reserve every
 * long name and supplied alias, including entries enumerated later. */
static bool conflict(struct fs_ro_view *v, const char *name, unsigned self)
{
    for (unsigned i = 0; i < v->count; i++) if (i != self) {
        /* Four UTF-8 comparisons/page walks: <= 1 ms at 250 us each. */
        if (!(i&3)) { fs_service(); if (fs_request_check()) return true; }
        struct fs_ro_node *n = at(v, i);
        if (path_equal(name, n->name) || (n->alias[0] && path_equal(name, n->alias))) return true;
    }
    return false;
}
static uint16_t upper437(uint16_t ch) {
    uint16_t upper=path_upper(ch);
    return path_cp437(upper)>=0 ? upper : path_cp437(ch)>=0 ? ch : '_';
}
static bool short_character(uint16_t ch) {
    if (ch<33 || ch==127 || path_cp437(ch)<0) return false;
    const char *bad="\"*+,/:;<=>?[\\]|";
    for (;*bad;bad++) if (ch==(uint8_t)*bad) return false;
    return true;
}
bool fs_ro_short_name(const char *s,char out[40]) {
    uint16_t units[12]; unsigned a=0,b=0,n=0; bool dot=false;
    while (*s) {
        uint16_t ch; if (path_decode(&s,&ch) || path_cp437(ch)<0) return false;
        ch=upper437(ch);
        if (ch=='.') { if (dot || !a) return false; dot=true; }
        else {
            if (!short_character(ch)) return false;
            if (dot) { if (++b>3) return false; } else if (++a>8) return false;
        }
        if (n==12) return false; units[n++]=ch;
    }
    return a && (!dot || b) && !path_from_ucs2(units,n,out,40);
}
static int byte_order(const struct fs_ro_node *a,const struct fs_ro_node *b) {
    const uint8_t *x=(const uint8_t *)a->name,*y=(const uint8_t *)b->name;
    while (*x && *x==*y) { x++; y++; }
    if (*x!=*y) return *x<*y ? -1 : 1;
    return a->id<b->id ? -1 : a->id>b->id;
}
static int sort_view(struct fs_ro_view *v) {
    /* Once per directory generation, before aliases are assigned. */
    for (unsigned i=1;i<v->count;i++) {
        fs_service(); int e=fs_request_check(); if (e) return e;
        struct fs_ro_node node=*at(v,i); unsigned j=i;
        while (j && byte_order(&node,at(v,j-1))<0) {
            if (!(j&1)) { fs_service(); if ((e=fs_request_check())) return e; }
            *at(v,j)=*at(v,j-1); j--;
        }
        *at(v,j)=node;
    }
    return 0;
}

static int view(struct fs_ro_volume *vol, uint64_t dir, struct fs_ro_view **out)
{
    int e = fs_ro_check(vol); if (e) return e;
    for (struct fs_ro_view *v = vol->views; v; v = v->next) if (v->directory == dir) { *out = v; return 0; }
    if (vol->view_count == FS_RO_VIEWS) return -FS_ENOSPC;
    struct fs_ro_view *v = fs_alloc(sizeof(*v)); if (!v) return -FS_ENOMEM;
    memset(v, 0, sizeof(*v)); v->directory = dir;
    struct fs_ro_cursor cursor = { {0} }; struct ro_page **tail = &v->pages;
    for (;;) {
        fs_service(); if ((e=fs_request_check())) break;
        struct fs_ro_node n;
        struct fs_ro_cursor before = cursor;
        e = vol->ops->readdir(vol, dir, &cursor, &n);
        if (e == -FS_ENOENT) { e = 0; break; }
        if (e) break;
        if (!memcmp(&cursor, &before, sizeof(cursor))) { e = -FS_EIO; break; }
        uint16_t units[256]; unsigned length;
        if ((e = path_validate_name(n.name, units, &length))) break;
        if (v->count == FS_RO_ENTRIES) { e = -FS_ENOSPC; break; }
        for (unsigned i = 0; i < v->count; i++) {
            if (!(i&3)) fs_service();
            if (path_equal(n.name, at(v, i)->name)) { e = -FS_EIO; break; }
        }
        if (e) break;
        if (!*tail || (*tail)->count == 4) {
            if (*tail) tail = &(*tail)->next;
            *tail = fs_page_alloc(); if (!*tail) { e = -FS_ENOMEM; break; }
        }
        (*tail)->nodes[(*tail)->count++] = n; v->count++;
    }
    if (!e) e=sort_view(v);
    for (unsigned i = 0; !e && i < v->count; i++) {
        fs_service(); if ((e=fs_request_check())) break;
        struct fs_ro_node *n = at(v, i); char alias[40];
        if (n->alias[0]) {
            if (!fs_ro_short_name(n->alias, alias) || conflict(v, alias, i)) { e = -FS_EIO; break; }
            memcpy(n->alias, alias, strlen(alias) + 1);
        }
    }
    for (unsigned i = 0; !e && i < v->count; i++) {
        fs_service(); if ((e=fs_request_check())) break;
        struct fs_ro_node *n = at(v, i); if (n->alias[0]) continue;
        if (fs_ro_short_name(n->name, n->alias) && !conflict(v, n->alias, i)) continue;
        n->alias[0] = 0;
        const char *text=n->name,*dot=0;
        for (const char *p=text;*p;p++) if (*p=='.') dot=p;
        uint16_t base[8],ext[3]; unsigned nb=0,ne=0;
        while (*text && text!=dot) {
            uint16_t ch; path_decode(&text,&ch); ch=upper437(ch);
            if (ch==' ' || ch=='.') continue;
            if (nb<8) base[nb++]=short_character(ch) ? ch : '_';
        }
        if (!nb) base[nb++]='_';
        text=dot ? dot+1 : "";
        while (*text && ne<3) {
            uint16_t ch; path_decode(&text,&ch); ch=upper437(ch);
            ext[ne++]=short_character(ch) ? ch : '_';
        }
        for (unsigned serial=1;serial<=999999;serial++) {
            fs_service(); if ((e=fs_request_check())) break;
            uint16_t result[12]; char digits[6]; unsigned nd=0,x=serial;
            do { digits[nd++]=(char)('0'+x%10); x/=10; } while (x);
            unsigned keep=nb<7-nd ? nb : 7-nd,used=0;
            for (unsigned k=0;k<keep;k++) result[used++]=base[k];
            result[used++]='~'; while (nd) result[used++]=(uint8_t)digits[--nd];
            if (ne) { result[used++]='.'; for (unsigned k=0;k<ne;k++) result[used++]=ext[k]; }
            if (path_from_ucs2(result,used,n->alias,sizeof(n->alias))) { e=-FS_EILSEQ; break; }
            if (!conflict(v,n->alias,i)) break;
            n->alias[0]=0;
        }
        if (!n->alias[0]) e = -FS_ENOSPC;
    }
    if (!e) e = fs_ro_check(vol);
    if (e) { view_free(v); return e; }
    v->next = vol->views; vol->views = v; vol->view_count++; *out = v; return 0;
}
int fs_ro_lookup(struct fs_ro_volume *v, uint64_t dir, const char *name, struct fs_ro_node *out)
{
    if (!name || !out) return -FS_EINVAL;
    if (v && v->ops && (v->ops->flags & FS_RO_STREAM_SYNTAX)) {
        for (const char *p = name; *p; p++) if (*p == ':') return -FS_EOPNOTSUPP;
    }
    struct fs_ro_view *names; int e = view(v, dir, &names); if (e) return e;
    struct fs_ro_node *hit = 0;
    for (unsigned i = 0; i < names->count && !hit; i++) {
        if (!(i&3)) { fs_service(); if ((e=fs_request_check())) return e; }
        struct fs_ro_node *n = at(names, i);
        if (path_equal(name, n->name) || path_equal(name, n->alias)) hit = n;
    }
    if (!hit && (v->ops->flags & FS_RO_DRIVER_COLLATION)) {
        /* The volume's own folding (NTFS $UpCase) may equate names the VFS
         * does not. The driver answers with an id; the view entry carries
         * the names, alias and status. Valid only while the media is live. */
        struct fs_ro_node found;
        e = v->ops->lookup(v, dir, name, &found);
        if (e) return e;
        for (unsigned i = 0; i < names->count && !hit; i++) {
            if (!(i&3)) { fs_service(); if ((e=fs_request_check())) return e; }
            if (at(names, i)->id == found.id) hit = at(names, i);
        }
        if (!hit) return -FS_ENOENT;
    }
    if (!hit) return -FS_ENOENT;
    if (hit->status) return hit->status;
    *out = *hit; return 0;
}
int fs_ro_next(struct fs_ro_volume *v, uint64_t dir, uint32_t *cursor, struct fs_ro_node *out)
{
    struct fs_ro_view *names; int e = view(v, dir, &names); if (e) return e;
    if (*cursor >= names->count) return -FS_ENOENT;
    *out = *at(names, (*cursor)++); return 0;
}
int fs_ro_read(struct fs_ro_volume *v, uint64_t id, uint64_t pos, void *buf, size_t n, size_t *done)
{
    if (!done || (n && !buf)) return -FS_EINVAL;
    *done = 0; int e = fs_ro_check(v);
    if (!e) e = v->ops->read(v, id, pos, buf, n, done);
    if (!e) e = fs_ro_check(v);
    if (e) *done = 0;
    return e;
}
