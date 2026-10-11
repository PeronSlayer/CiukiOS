/* Optical gate on the canonical image. QMP actions are ARM synchronized.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/ata.h>
#include <ciuki/storage.h>
#include <ciuki/probe.h>
#include <ciuki/sha256.h>
#include <ciuki/files.h>
#include <ciuki/task.h>
#include <ciuki/cpu.h>
static struct vfs_table optical_table;
static char first_file[FS_PATH_BYTES];
static unsigned entries;
static int digest(const char *path, uint64_t *size, char hex[65])
{
    int h=vfs_open(&optical_table,path,VFS_READ,VFS_DENY_NONE,0); if (h<0) return h;
    struct sha256_ctx ctx; sha256_init(&ctx); *size=0; uint8_t buf[512]; int e;
    for (;;) {
        size_t n; e=vfs_read(&optical_table,h,buf,sizeof(buf),&n);
        if (e || !n) break;
        sha256_update(&ctx,buf,n); *size+=n;
    }
    vfs_close(&optical_table,h); uint8_t hash[32]; sha256_final(&ctx,hash); sha256_hex(hash,hex); return e;
}
struct walk_frame { struct walk_frame *parent; uint64_t directory; uint32_t cursor; unsigned depth; char prefix[FS_PATH_BYTES]; };
static int listing(struct fs_ro_volume *ro, uint64_t directory, const char *prefix, unsigned drive, unsigned cycle, unsigned depth)
{
    struct walk_frame *top=fs_alloc(sizeof(*top)); if (!top) return -FS_ENOMEM;
    memset(top,0,sizeof(*top)); top->directory=directory; top->depth=depth;
    memcpy(top->prefix,prefix,strlen(prefix)+1); int e=0;
    while (top) {
        struct fs_ro_node n;
        fs_lock_take(&optical_table.vfs->lock); e=fs_ro_next(ro,top->directory,&top->cursor,&n); fs_lock_drop(&optical_table.vfs->lock);
        if (e==-FS_ENOENT) { struct walk_frame *old=top; top=top->parent; fs_free(old); e=0; continue; }
        if (e) break;
        if (++entries>FS_RO_ENTRIES) { e=-FS_ENOSPC; break; }
        char path[FS_PATH_BYTES]; unsigned len=(unsigned)strlen(top->prefix), name=(unsigned)strlen(n.name);
        if (len+name+2>=sizeof(path)) { e=-FS_ENAMETOOLONG; break; }
        memcpy(path,top->prefix,len); path[len++]='/'; memcpy(path+len,n.name,name+1);
        /* Small declared fixtures: path and hash are separate bounded records. */
        if (strlen(path)>48) { e=-FS_ENAMETOOLONG; break; }
        char encoded[97]; for (unsigned i=0; path[i]; i++) { encoded[i*2]="0123456789abcdef"[(uint8_t)path[i]>>4]; encoded[i*2+1]="0123456789abcdef"[(uint8_t)path[i]&15]; } encoded[strlen(path)*2]=0;
        rec_emit("optical","DATA","case=entry cycle=%u id=%u path_hex=%s kind=%s",cycle,entries,encoded,n.attr&FS_RO_DIRECTORY ? "directory" : "file");
        if (n.attr&FS_RO_DIRECTORY) {
            if (top->depth>=15) { e=-FS_EIO; break; }
            struct walk_frame *child=fs_alloc(sizeof(*child)); if (!child) { e=-FS_ENOMEM; break; }
            memset(child,0,sizeof(*child)); child->parent=top; child->directory=n.id; child->depth=top->depth+1;
            memcpy(child->prefix,path,strlen(path)+1); top=child;
        } else {
            char full[FS_PATH_BYTES]; full[0]=(char)('A'+drive); full[1]=':'; memcpy(full+2,path,strlen(path)+1);
            if (!first_file[0]) memcpy(first_file,full,strlen(full)+1);
            uint64_t size; char hex[65]; e=digest(full,&size,hex); if (e) break;
            rec_emit("optical","DATA","case=file cycle=%u id=%u size=%llu sha256=%s",cycle,entries,size,hex);
        }
    }
    while (top) { struct walk_frame *old=top; top=top->parent; fs_free(old); }
    return e;
}
static bool peer_read(struct ata_device *d, const uint8_t expected[32], bool compare)
{
    uint8_t data[512], hash[32]; if (!d || d->atapi || d->block.read(&d->block,0,1,data)) return false;
    sha256(data,sizeof(data),hash); return !compare || !memcmp(hash,expected,32);
}
static int probe_optical(void)
{
    rec_emit("optical","BEGIN",0);
    struct ata_device *d=0; unsigned slot=0;
    for (; slot<4; slot++) { d=ata_device_get(slot/2,slot%2); if (d && d->atapi) break; }
    if (slot==4) { rec_emit("optical","DATA","status=not_run reason=device_absent"); rec_emit("optical","END","status=NOT_RUN reason=device_absent"); return 0; }
    struct storage *s=storage_get(); unsigned drive=26;
    for (unsigned i=3; i<26; i++) if (s->vfs.ro_volumes[i] && s->vfs.ro_volumes[i]->dev==&d->block) drive=i;
    rec_emit("optical","DATA","case=device slot=%u type=%u packet_bytes=%u block_size=%u generation=%u",slot,d->packet_type,d->packet_bytes,d->block.sector_size,d->block.media_generation);
    if (drive==26) {
        int e=atapi_ready(d); if (!e) e=storage_add_ro(s,slot,&d->block,d->session_lba);
        if (e<0) {
            bool refused=d->media==ATAPI_READY && s->optical[slot].diagnostic && !d->block.quarantined;
            rec_emit("optical","DATA","case=malformed refused=%u result=%d writes=%llu",refused,e,d->media_writes);
            rec_emit("optical","END","status=%s",refused ? "PASS" : "FAIL"); return refused ? 0 : 1;
        }
        drive=(unsigned)e;
    }
    bool pass=true;
    struct ata_device *peer=ata_device_get(slot/2,1-slot%2);
    /* The declared optical suite attaches an expendable FAT peer to the
     * other unit of this same channel. Physical absent peer is explicit. */
    uint8_t data[512], hash[32]; bool same=peer && !peer->atapi;
    if (!same) for (unsigned i=0; i<4; i++) { struct ata_device *candidate=ata_device_get(i/2,i%2); if (candidate && !candidate->atapi) { peer=candidate; break; } }
    if (peer && !peer->atapi && !peer->block.read(&peer->block,0,1,data)) sha256(data,512,hash); else pass=false;
    rec_emit("optical","DATA","case=peer stage=before same_channel=%u result=%u",same,pass);
    int e=vfs_table_init(&s->vfs,&optical_table,2); if (e) pass=false;
    struct fs_ro_volume *ro=s->vfs.ro_volumes[drive]; struct fs_ro_node root;
    first_file[0]=0; entries=0;
    if (pass) { e=ro->ops->root(ro,&root); if (!e) e=listing(ro,root.id,"",drive,1,0); pass=!e && first_file[0]; }
    int stale=-1; int native=-1;
    if (pass) {
        stale=vfs_open(&optical_table,first_file,VFS_READ,VFS_DENY_NONE,0); pass=stale>=0;
        char native_path[FS_PATH_BYTES]; ksnprintf(native_path,sizeof(native_path),"/mnt/%c%s",'a'+drive,first_file+2);
        e=files_bootstrap(&s->vfs); if (!e) native=file_open(proc_supervisor(),native_path,O_RDONLY,0); pass &= !e && native>=0;
        if (pass) {
            pass &= px_chdir(proc_supervisor(),native_path)==-ENOTDIR;
            char *leaf=native_path; for (char *p=native_path; *p; p++) if (*p=='/') leaf=p;
            *leaf=0; pass &= !px_chdir(proc_supervisor(),native_path);
        }
        /* Exercise mutation refusals on both existing and missing names. */
        struct fat_times times={0}; size_t done; uint64_t position;
        int results[] = {
            vfs_open(&optical_table,first_file,VFS_WRITE,VFS_DENY_NONE,0),
            vfs_open(&optical_table,first_file,VFS_READ|VFS_CREATE,VFS_DENY_NONE,0),
            vfs_open(&optical_table,first_file,VFS_WRITE|VFS_TRUNCATE,VFS_DENY_NONE,0),
            vfs_write(&optical_table,stale,"x",1,&done), vfs_truncate(&optical_table,stale,0),
            vfs_attrib(&optical_table,first_file,0), vfs_times(&optical_table,first_file,&times),
            vfs_rename(&optical_table,first_file,first_file), vfs_delete(&optical_table,first_file),
            vfs_mkdir(&optical_table,first_file,0), vfs_rmdir(&optical_table,first_file) };
        for (unsigned i=0; i<ARRAY_SIZE(results); i++) { rec_emit("optical","DATA","case=mutation operation=%u result=%d",i,results[i]); pass &= results[i]==-FS_EROFS; }
        pass &= !vfs_seek(&optical_table,stale,0,VFS_SEEK_SET,&position) && !vfs_commit(&optical_table,stale);
        pass &= peer_read(peer,hash,true);
        rec_emit("optical","DATA","case=peer stage=after same_channel=%u result=%u",same,pass);
        uint32_t gen=d->block.media_generation;
        e=atapi_eject(d); storage_refresh_ro(s);
        size_t n; int old=vfs_read(&optical_table,stale,data,1,&n), old_native=-1;
        if (native>=0) { fs_lock_take(&s->vfs.lock); old_native=file_io_locked(file_fd(proc_supervisor(),native),data,1,false,false,0,&n); fs_lock_drop(&s->vfs.lock); }
        bool absent=!vfs_drive_present(&s->vfs,drive);
        char cwd[CIUKI_PATH_MAX]; fs_lock_take(&s->vfs.lock); int expired=px_getcwd_locked(proc_supervisor()->cwd,cwd); fs_lock_drop(&s->vfs.lock);
        rec_emit("optical","DATA","case=cwd expired=%u",expired==-ENOENT); pass &= expired==-ENOENT;
        rec_emit("optical","DATA","case=eject result=%d old=%d native_old=%d letter_absent=%u generation_before=%u generation_after=%u",e,old,old_native,absent,gen,d->block.media_generation);
        pass &= !e && old==-FS_EIO && old_native==-EIO && absent && d->block.media_generation>gen;
        rec_emit("optical","ARM","action=media_reinsert device=ciuki-optical");
        /* QMP first ejects the already-empty tray, then changes its medium.
         * New readiness transactions are bounded independently; no failed
         * READ is repeated. The outer wait is also elapsed and preemptible. */
        uint64_t deadline=deadline_after_ms(20000);
        do { task_sleep_ms(100); e=atapi_ready(d); } while (e && !d->block.quarantined && deadline_after_ms(0)<deadline);
        if (!e) e=storage_add_ro(s,slot,&d->block,d->session_lba);
        if (e<0) pass=false;
        else {
            unsigned new_drive=(unsigned)e; ro=s->vfs.ro_volumes[new_drive]; entries=0;
            e=ro->ops->root(ro,&root); if (!e) e=listing(ro,root.id,"",new_drive,2,0);
            old=vfs_read(&optical_table,stale,data,1,&n);
            if (native>=0) { fs_lock_take(&s->vfs.lock); old_native=file_io_locked(file_fd(proc_supervisor(),native),data,1,false,false,0,&n); fs_lock_drop(&s->vfs.lock); }
            rec_emit("optical","DATA","case=reinsert drive=%c generation=%u old=%d native_old=%d result=%d",'A'+new_drive,d->block.media_generation,old,old_native,e);
            pass &= !e && old==-FS_EIO && old_native==-EIO && peer_read(peer,hash,true);
        }
    }
    if (stale>=0) vfs_close(&optical_table,stale);
    if (native>=0) { file_close(proc_supervisor(),native); px_chdir(proc_supervisor(),"/"); }
    if (optical_table.vfs) vfs_table_destroy(&optical_table);
    rec_emit("optical","DATA","case=counters writes=%llu max_packet_ms=%llu ordinary_deadline_ms=%u eject_deadline_ms=%u quarantined=%u",d->media_writes,d->max_packet_ms,ATAPI_COMMAND_MS,ATAPI_START_MS,d->block.quarantined);
    pass &= !d->media_writes && !d->block.quarantined && d->max_packet_ms<=ATAPI_START_MS;
    rec_emit("optical","END","status=%s",pass ? "PASS" : "FAIL"); return pass ? 0 : 1;
}
CIUKI_F4_PROBE("optical",probe_optical);
