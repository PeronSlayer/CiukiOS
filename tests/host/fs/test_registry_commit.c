/* w32srv registry directory commit (apps/w32srv/directory.c save(),
 * apps/w32srv/storage.c wr_commit()) replayed on the production FAT32/VFS
 * code exactly as apps/w32srv/posix_io.c issues it:
 *   write_file : open O_WRONLY|O_CREAT|O_TRUNC, write whole record, close
 *   sync_file  : open O_RDWR, fsync, close
 *   patch_file : open O_WRONLY, lseek 32, write 4 bytes, close
 *   sync_file  : open O_RDWR, fsync, close
 * on SERVICE.CRS.<slot> alternating slots, in a /WINDOWS directory that
 * mtools populated like the image (SYSTEM.CRG.0, USER.CRG.0, SYSTEM, CRASH).
 * Two drivers: the legacy VFS API (vfs.c nodes are re-decoded at every open)
 * and fat.c called as src/kernel/proc/fdtable.c + posixpath.c call it (the
 * px node entry persists across opens, every open has an empty cursor); the
 * native syscall replay itself is tests/host/proc/files_test.c
 * test_registry_commit. The volume is mounted read-only like the guest's
 * storage_add_disk (src/kernel/fs/mount.c) and is first driven with the F1
 * write gate closed, the state of every QEMU test selector until its probe
 * calls storage_enable_write: the first commit must fail at write_file's
 * open with EROFS and write nothing, the w32-dialogs symptom on image
 * ec9f5854 ([win32] registry directory-commit-failed, C: mode=ro). Then
 * fat_enable_write opens the gate and every commit must succeed.
 * Every failing call is reported with its step and FS error.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include "fake.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define COMMITS 24u
#define MARKER 0x43524731u
#define DIR "C:/WINDOWS"
static struct fake f;
static struct block_cache cache;
static struct fat_volume vol;
static struct vfs space;
static struct vfs_table t;
static const char *expect_dir;
static uint32_t windows; /* /WINDOWS directory cluster */
static const char *fs_name(int e) {
    switch (-e) {
    case 0: return "ok"; case FS_EIO: return "EIO"; case FS_ENOENT: return "ENOENT"; case FS_EBADF: return "EBADF";
    case FS_ENOMEM: return "ENOMEM"; case FS_EACCES: return "EACCES"; case FS_EBUSY: return "EBUSY"; case FS_EEXIST: return "EEXIST";
    case FS_ENOTDIR: return "ENOTDIR"; case FS_EISDIR: return "EISDIR"; case FS_EINVAL: return "EINVAL"; case FS_EMFILE: return "EMFILE";
    case FS_EFBIG: return "EFBIG"; case FS_ENOSPC: return "ENOSPC"; case FS_EROFS: return "EROFS"; case FS_ENAMETOOLONG: return "ENAMETOOLONG";
    case FS_ENOTEMPTY: return "ENOTEMPTY"; case FS_ELOOP: return "ELOOP"; case FS_EILSEQ: return "EILSEQ"; case FS_EOPNOTSUPP: return "EOPNOTSUPP";
    case FS_EUCLEAN: return "EUCLEAN"; case FS_EQUARANTINED: return "EQUARANTINED"; case FS_EAGAIN: return "EAGAIN"; default: return "?";
    }
}
static void fail(const char *phase, unsigned commit, const char *name, const char *step, int e) {
    fprintf(stderr,"FAIL registry-commit phase=%s commit=%u file=%s step=%s error=%d (%s) readonly=%s reason=%s\n",
        phase,commit,name,step,e,fs_name(e),vol.readonly ? "yes" : "no",vol.readonly_reason ? vol.readonly_reason : "-");
    exit(1);
}
static void put32(uint8_t *p, uint32_t n) { for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(i*8)); }
/* directory.c save(): 56-byte header, then atoms (16 + name) and windows (28 +
 * title); the CRC fields hold a deterministic pattern here. The record grows
 * with every commit so the sizes cross the 512-byte sector within COMMITS. */
static size_t record(uint8_t *b, size_t cap, unsigned commit, uint32_t seq) {
    size_t n=56; unsigned atoms=commit, windows=commit/3;
    for (unsigned i=0;i<atoms;i++) n+=16+8+1;
    for (unsigned i=0;i<windows;i++) n+=28+12+1;
    CHECK(n<=cap); memset(b,0,n); memcpy(b,"CRS1",4); put32(b+4,1); put32(b+12,seq); put32(b+16,0); put32(b+20,(uint32_t)n-40);
    put32(b+24,0x5a5a0000u+commit); put32(b+28,0xa5a50000u+commit); /* marker at 32 stays zero until patch */
    put32(b+40,1+1024u*(commit+1)); put32(b+44,0xc000+atoms); put32(b+48,atoms); put32(b+52,windows);
    uint8_t *p=b+56;
    for (unsigned i=0;i<atoms;i++) { put32(p,0xc000+i); put32(p+4,5); put32(p+8,1); put32(p+12,9); snprintf((char *)p+16,9,"atom%04u",i); p+=25; }
    for (unsigned i=0;i<windows;i++) { for (unsigned j=0;j<24;j++) p[j]=(uint8_t)(j+i+commit); put32(p+24,13); snprintf((char *)p+28,13,"window%05u",i); p+=41; }
    CHECK((size_t)(p-b)==n); return n;
}
/* Hive record (storage.c): same sequence, larger payload like SYSTEM.CRG.* */
static size_t hive(uint8_t *b, size_t cap, unsigned commit) {
    size_t n=6381+commit*97; CHECK(n<=cap);
    for (size_t i=0;i<n;i++) b[i]=(uint8_t)(i*7u+commit*31u+(i>>9));
    memcpy(b,"CRG1",4); put32(b+32,0); return n;
}
static void save_expected(const char *name, const uint8_t *b, size_t n) {
    if (!expect_dir) return;
    char path[512]; snprintf(path,sizeof(path),"%s/%s",expect_dir,name);
    FILE *o=fopen(path,"wb"); CHECK(o && fwrite(b,1,n,o)==n && !fclose(o));
}

/* ---- driver 1: the legacy VFS API, posix_io.c call for call ---- */
static int v_write_file(const char *path, const uint8_t *b, size_t n, const char **step) {
    int h=vfs_open(&t,path,VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_NONE,0);
    if (h<0) { *step="write_file:open(O_WRONLY|O_CREAT|O_TRUNC)"; return h; }
    size_t at=0;
    while (at<n) {
        size_t done=0; int e=vfs_write(&t,h,b+at,n-at,&done);
        if (e || !done) { *step="write_file:write"; vfs_close(&t,h); return e ? e : -FS_EIO; }
        at+=done;
    }
    int e=vfs_close(&t,h); if (e) *step="write_file:close"; return e;
}
static int v_sync_file(const char *path, const char **step, const char *label) {
    static char text[64];
    int h=vfs_open(&t,path,VFS_READ|VFS_WRITE,VFS_DENY_NONE,0);
    if (h<0) { snprintf(text,sizeof(text),"%s:open(O_RDWR)",label); *step=text; return h; }
    int e=vfs_commit(&t,h);
    if (e) { snprintf(text,sizeof(text),"%s:fsync",label); *step=text; vfs_close(&t,h); return e; }
    e=vfs_close(&t,h); if (e) { snprintf(text,sizeof(text),"%s:close",label); *step=text; } return e;
}
static int v_patch_file(const char *path, const uint8_t *m, const char **step) {
    int h=vfs_open(&t,path,VFS_WRITE,VFS_DENY_NONE,0);
    if (h<0) { *step="patch_file:open(O_WRONLY)"; return h; }
    uint64_t position; int e=vfs_seek(&t,h,32,VFS_SEEK_SET,&position);
    if (e || position!=32) { *step="patch_file:lseek(32)"; vfs_close(&t,h); return e ? e : -FS_EINVAL; }
    size_t done=0; e=vfs_write(&t,h,m,4,&done);
    if (e || done!=4) { *step="patch_file:write(4)"; vfs_close(&t,h); return e ? e : -FS_EIO; }
    e=vfs_close(&t,h); if (e) *step="patch_file:close"; return e;
}
static int v_verify(const char *path, const uint8_t *b, size_t n, const char **step) {
    static uint8_t back[16384]; CHECK(n<=sizeof(back));
    int h=vfs_open(&t,path,VFS_READ,VFS_DENY_NONE,0);
    if (h<0) { *step="verify:open(O_RDONLY)"; return h; }
    struct fat_entry st; uint64_t position; int e=vfs_handle_stat(&t,h,&st,&position);
    if (e) { *step="verify:fstat"; vfs_close(&t,h); return e; }
    if (st.size!=n) { fprintf(stderr,"size on disk %u expected %zu\n",st.size,n); *step="verify:size"; vfs_close(&t,h); return -FS_EIO; }
    size_t done=0; e=vfs_read(&t,h,back,n,&done);
    if (e || done!=n) { *step="verify:read"; vfs_close(&t,h); return e ? e : -FS_EIO; }
    if (memcmp(back,b,n)) { *step="verify:bytes"; vfs_close(&t,h); return -FS_EIO; }
    return vfs_close(&t,h);
}
static void commit_vfs(const char *phase, unsigned commit, const char *name, uint8_t *b, size_t n) {
    char path[64]; snprintf(path,sizeof(path),DIR "/%s",name);
    uint8_t marker[4]; put32(marker,MARKER); const char *step="?"; int e;
    if ((e=v_write_file(path,b,n,&step))) fail(phase,commit,name,step,e);
    if ((e=v_sync_file(path,&step,"sync_file"))) fail(phase,commit,name,step,e);
    if ((e=v_patch_file(path,marker,&step))) fail(phase,commit,name,step,e);
    if ((e=v_sync_file(path,&step,"sync_file_2"))) fail(phase,commit,name,step,e);
    memcpy(b+32,marker,4);
    if ((e=v_verify(path,b,n,&step))) fail(phase,commit,name,step,e);
}

/* ---- driver 2: fat.c as fdtable.c/posixpath.c call it ---- */
struct px { char name[16]; struct fat_entry entry; bool live; };
static struct px nodes[8];
struct desc { struct px *node; uint64_t position; struct fat_cursor cursor; };
static struct px *px_node(const struct fat_entry *e) {
    for (unsigned i=0;i<8;i++) if (nodes[i].live && nodes[i].entry.parent==e->parent && nodes[i].entry.index==e->index) return &nodes[i];
    for (unsigned i=0;i<8;i++) if (!nodes[i].live) { nodes[i].live=true; nodes[i].entry=*e; return &nodes[i]; }
    CHECK(false); return 0;
}
/* px_open_locked: lookup; O_CREAT creates (fat_create into the new node's
 * entry); O_TRUNC truncates the node entry in place. */
static int p_open(const char *name, bool creat, bool trunc, struct desc *d, const char **step) {
    struct fat_entry e; int r=fat_lookup(&vol,windows,name,&e); struct px *n;
    if (r==-FS_ENOENT) {
        if (!creat) { *step="open:lookup"; return r; }
        n=0; for (unsigned i=0;i<8;i++) if (!nodes[i].live) { n=&nodes[i]; break; }
        CHECK(n); memset(n,0,sizeof(*n)); snprintf(n->name,sizeof(n->name),"%s",name);
        if ((r=fat_create(&vol,windows,name,0,&n->entry))) { *step="open:fat_create"; return r; }
        n->live=true;
    } else if (r) { *step="open:fat_lookup"; return r; }
    else { n=px_node(&e); snprintf(n->name,sizeof(n->name),"%s",name); }
    if (trunc && (r=fat_truncate(&vol,&n->entry,0))) { *step="open:fat_truncate(0)"; return r; }
    *d=(struct desc){.node=n,.position=0,.cursor={0}}; return 0;
}
static int p_write(struct desc *d, const uint8_t *b, size_t n, const char **step) {
    size_t at=0;
    while (at<n) {
        size_t done=0; int e=fat_write_cursor(&vol,&d->node->entry,&d->cursor,d->position,b+at,n-at,&done);
        if (e || !done) { *step="write:fat_write_cursor"; return e ? e : -FS_EIO; }
        d->position+=done; at+=done;
    }
    return 0;
}
static void commit_px(const char *phase, unsigned commit, const char *name, uint8_t *b, size_t n) {
    uint8_t marker[4]; put32(marker,MARKER); const char *step="?"; int e; struct desc d;
    if ((e=p_open(name,true,true,&d,&step))) fail(phase,commit,name,step,e);
    if ((e=p_write(&d,b,n,&step))) fail(phase,commit,name,step,e);
    if ((e=p_open(name,false,false,&d,&step))) fail(phase,commit,name,step,e);
    if ((e=fat_commit(&vol))) fail(phase,commit,name,"sync_file:fsync",e);
    if ((e=p_open(name,false,false,&d,&step))) fail(phase,commit,name,step,e);
    d.position=32;
    if ((e=p_write(&d,marker,4,&step))) fail(phase,commit,name,"patch_file:write(4)",e);
    if ((e=p_open(name,false,false,&d,&step))) fail(phase,commit,name,step,e);
    if ((e=fat_commit(&vol))) fail(phase,commit,name,"sync_file_2:fsync",e);
    memcpy(b+32,marker,4);
    if ((e=p_open(name,false,false,&d,&step))) fail(phase,commit,name,step,e);
    if (d.node->entry.size!=n) { fprintf(stderr,"size %u expected %zu\n",d.node->entry.size,n); fail(phase,commit,name,"verify:size",-FS_EIO); }
    static uint8_t back[16384]; size_t done=0;
    if ((e=fat_read_cursor(&vol,&d.node->entry,&d.cursor,0,back,n,&done)) || done!=n) fail(phase,commit,name,"verify:read",e ? e : -FS_EIO);
    if (memcmp(back,b,n)) fail(phase,commit,name,"verify:bytes",-FS_EIO);
    /* A reader that opened the file later (fresh decode) sees the same bytes. */
    struct fat_entry fresh; struct fat_cursor none={0};
    if ((e=fat_lookup(&vol,windows,name,&fresh))) fail(phase,commit,name,"verify:fresh-lookup",e);
    if (fresh.size!=n || (e=fat_read_cursor(&vol,&fresh,&none,0,back,n,&done)) || done!=n || memcmp(back,b,n)) fail(phase,commit,name,"verify:fresh-read",e ? e : -FS_EIO);
}

/* Negative control: the probe's state before the write gate opened. */
static void gate_closed(void) {
    static uint8_t b[16384]; size_t n=record(b,sizeof(b),0,1);
    unsigned writes=f.writes; const char *step="?";
    int e=v_write_file(DIR "/SERVICE.CRS.0",b,n,&step);
    if (e!=-FS_EROFS || strcmp(step,"write_file:open(O_WRONLY|O_CREAT|O_TRUNC)")) {
        fprintf(stderr,"FAIL registry-commit phase=gate-closed step=%s error=%d (%s), expected EROFS at open\n",e ? step : "none",e,fs_name(e));
        exit(1);
    }
    struct desc d; e=p_open("SERVICE.CRS.0",true,true,&d,&step);
    CHECK(e==-FS_EROFS && !strcmp(step,"open:fat_create"));
    struct fat_entry absent; CHECK(fat_lookup(&vol,windows,"SERVICE.CRS.0",&absent)==-FS_ENOENT);
    CHECK(f.writes==writes);
    printf("PASS registry-commit phase=gate-closed write_file:open error=%d (EROFS) both drivers, no entry, writes=0\n",e);
    CHECK(!fat_enable_write(&vol) && !vol.readonly);
}
static void run(const char *phase, void (*commit)(const char *, unsigned, const char *, uint8_t *, size_t)) {
    static uint8_t b[16384]; unsigned slot=0; char name[24];
    for (unsigned c=0;c<COMMITS;c++) {
        slot=c ? 1-slot : 0; snprintf(name,sizeof(name),"SERVICE.CRS.%u",slot);
        size_t n=record(b,sizeof(b),c,c+1); commit(phase,c,name,b,n);
        if (c+2>=COMMITS) save_expected(name,b,n);
        if (c%6==5) { /* the hive flush of wr_tick, interleaved like the guest */
            snprintf(name,sizeof(name),"%s.CRG.%u",c%12==5 ? "SYSTEM" : "USER",1-(c/6)%2);
            n=hive(b,sizeof(b),c); commit(phase,c,name,b,n);
        }
    }
    printf("PASS registry-commit phase=%s commits=%u (write/sync/patch/sync on SERVICE.CRS.0/1 plus hive flushes)\n",phase,COMMITS);
}
int main(int argc, char **argv) {
    CHECK(argc==2 || argc==3); expect_dir=argc==3 ? argv[2] : 0;
    CHECK(!fake_open(&f,argv[1]));
    CHECK(!cache_init(&cache,4u*1024u*1024u));
    vfs_init(&space);
    CHECK(!fat_mount(&vol,&cache,&f.dev,0,f.dev.capacity,false) && vol.readonly && vol.type==32);
    CHECK(!vfs_attach(&space,2,&vol));
    CHECK(!vfs_table_init(&space,&t,2));
    struct fat_entry dir; CHECK(!fat_lookup(&vol,vol.root,"WINDOWS",&dir) && (dir.attr&FAT_ATTR_DIR)); windows=dir.first;
    struct fat_entry seeded; CHECK(!fat_lookup(&vol,windows,"SYSTEM.CRG.0",&seeded) && !fat_lookup(&vol,windows,"USER.CRG.0",&seeded));
    CHECK(fat_lookup(&vol,windows,"SERVICE.CRS.0",&seeded)==-FS_ENOENT);
    gate_closed();
    run("vfs",commit_vfs);
    /* Second driver on the files the first one left: existing slots, like a
     * w32srv restart on the same image. */
    run("px-fat",commit_px);
    vfs_table_destroy(&t);
    CHECK(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache); fake_close(&f);
    puts("RESULT registry directory commit replay FAT32 failures=0");
    return 0;
}
