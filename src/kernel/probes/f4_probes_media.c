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
static int digest_in(struct vfs_table *table, const char *path, uint64_t *size, char hex[65])
{
    int h=vfs_open(table,path,VFS_READ,VFS_DENY_NONE,0); if (h<0) return h;
    struct sha256_ctx ctx; sha256_init(&ctx); *size=0; uint8_t buf[512]; int e;
    for (;;) {
        size_t n; e=vfs_read(table,h,buf,sizeof(buf),&n);
        if (e || !n) break;
        sha256_update(&ctx,buf,n); *size+=n;
    }
    vfs_close(table,h); uint8_t hash[32]; sha256_final(&ctx,hash); sha256_hex(hash,hex); return e;
}
static int digest(const char *path, uint64_t *size, char hex[65]) { return digest_in(&optical_table,path,size,hex); }
struct walk_frame { struct walk_frame *parent; uint64_t directory; uint32_t cursor; unsigned depth; char prefix[FS_PATH_BYTES]; };
static int listing(struct fs_ro_volume *ro, uint64_t directory, const char *prefix, unsigned drive, unsigned cycle, unsigned depth)
{
    struct walk_frame *top=fs_alloc(sizeof(*top)); if (!top) return -FS_ENOMEM;
    memset(top,0,sizeof(*top)); top->directory=directory; top->depth=depth;
    memcpy(top->prefix,prefix,strlen(prefix)+1); int e=0;
    while (top) {
        struct fs_ro_node n;
        vfs_lock(optical_table.vfs,vfs_drive_mask(drive)); e=VFS_IO(optical_table.vfs,fs_ro_next(ro,top->directory,&top->cursor,&n)); vfs_unlock(optical_table.vfs);
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
        if (native>=0) { vfs_lock(&s->vfs,0); old_native=file_io_locked(file_fd(proc_supervisor(),native),data,1,false,false,0,&n); vfs_unlock(&s->vfs); }
        bool absent=!vfs_drive_present(&s->vfs,drive);
        char cwd[CIUKI_PATH_MAX]; vfs_lock(&s->vfs,0); int expired=px_getcwd_locked(proc_supervisor()->cwd,cwd); vfs_unlock(&s->vfs);
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
            if (native>=0) { vfs_lock(&s->vfs,0); old_native=file_io_locked(file_fd(proc_supervisor(),native),data,1,false,false,0,&n); vfs_unlock(&s->vfs); }
            rec_emit("optical","DATA","case=reinsert drive=%c generation=%u old=%d native_old=%d result=%d",'A'+new_drive,d->block.media_generation,old,old_native,e);
            pass &= !e && old==-FS_EIO && old_native==-EIO && peer_read(peer,hash,true);
            /* Keep the surprise-removal checks above, then exercise the
             * explicit action against this newly mounted generation. */
            if (pass) {
                char previous=first_file[0];first_file[0]=(char)('A'+new_drive);
                int opened=vfs_open(&optical_table,first_file,VFS_READ,VFS_DENY_NONE,0);
                first_file[0]=previous;
                int busy=opened<0 ? opened : optical_eject(s,new_drive,d->block.media_generation);
                if (opened>=0) vfs_close(&optical_table,opened);
                int removed=optical_eject(s,new_drive,d->block.media_generation);
                bool absent=!vfs_drive_present(&s->vfs,new_drive);
                rec_emit("optical","DATA","case=safe_remove busy=%d result=%d letter_absent=%u",busy,removed,absent);
                pass &= busy==-FS_EBUSY && !removed && absent;
            }
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

/* ---- f4:ntfs-ro (f4-04) ---------------------------------------------------
 * A declared NTFS fixture disk (MBR, partition type 07h) mounted at boot by
 * the production storage path: list and hash through the VFS, refuse every
 * mutation, unmount and remount, compare with the host's independent ntfs-3g
 * manifest (scripts/test/ntfs_fixtures.py verify_records). Every path is
 * recorded as its SHA-256 (the 240-byte record envelope cannot carry a
 * 255-unit name); sizes and hashes of files are separate records. */
static struct vfs_table ntfs_table;
static char ntfs_first[FS_PATH_BYTES], ntfs_target[FS_PATH_BYTES];
static unsigned ntfs_entries;
struct ntfs_frame { struct ntfs_frame *parent; uint64_t directory; uint32_t cursor; unsigned depth; char prefix[FS_PATH_BYTES]; };
static void path_digest(const char *path, char hex[65]) { uint8_t hash[32]; sha256(path,strlen(path),hash); sha256_hex(hash,hex); }
/* The walk is iterative and runs once at a time: its large scratch is static. */
static struct fs_ro_node ntfs_root, ntfs_node;
static char ntfs_path[FS_PATH_BYTES], ntfs_full[FS_PATH_BYTES+3];
static int ntfs_walk(struct storage_ntfs *n, unsigned cycle)
{
    struct fs_ro_volume *ro=&n->ro; struct fs_ro_node *node=&ntfs_node;
    int e=ro->ops->root(ro,&ntfs_root); if (e) return e;
    struct ntfs_frame *top=fs_alloc(sizeof(*top)); if (!top) return -FS_ENOMEM;
    memset(top,0,sizeof(*top)); top->directory=ntfs_root.id; ntfs_entries=0; e=0;
    while (top) {
        vfs_lock(ntfs_table.vfs,vfs_drive_mask(n->drive)); e=VFS_IO(ntfs_table.vfs,fs_ro_next(ro,top->directory,&top->cursor,node)); vfs_unlock(ntfs_table.vfs);
        if (e) {
            if (e!=-FS_ENOENT) {
                char hex[65]; path_digest(top->prefix[0] ? top->prefix : "/",hex);
                rec_emit("ntfs-ro","DATA","case=walkerr cycle=%u result=%d path_sha256=%s",cycle,e,hex);
            }
            struct ntfs_frame *old=top; top=top->parent; fs_free(old); e=0; continue;
        }
        if (++ntfs_entries>1024) { e=-FS_ENOSPC; break; }
        unsigned len=(unsigned)strlen(top->prefix), name=(unsigned)strlen(node->name);
        if (len+name+2>=sizeof(ntfs_path)) { e=-FS_ENAMETOOLONG; break; }
        memcpy(ntfs_path,top->prefix,len); ntfs_path[len++]='/'; memcpy(ntfs_path+len,node->name,name+1);
        char hex[65]; path_digest(ntfs_path,hex); unsigned id=ntfs_entries; bool directory=node->attr&FS_RO_DIRECTORY;
        rec_emit("ntfs-ro","DATA","case=entry cycle=%u id=%u kind=%s attr=%u status=%d plen=%u path_sha256=%s",
                 cycle,id,directory ? "directory" : "file",node->attr,node->status,(unsigned)strlen(ntfs_path),hex);
        ntfs_full[0]=(char)('A'+n->drive); ntfs_full[1]=':'; memcpy(ntfs_full+2,ntfs_path,strlen(ntfs_path)+1);
        if (directory) {
            if (top->depth>=7) { e=-FS_EIO; break; }
            struct ntfs_frame *child=fs_alloc(sizeof(*child)); if (!child) { e=-FS_ENOMEM; break; }
            memset(child,0,sizeof(*child)); child->parent=top; child->directory=node->id; child->depth=top->depth+1;
            memcpy(child->prefix,ntfs_path,strlen(ntfs_path)+1); top=child;
        } else if (node->status) {
            struct fat_entry st; int open=vfs_open(&ntfs_table,ntfs_full,VFS_READ,VFS_DENY_NONE,0), stat=vfs_stat(&ntfs_table,ntfs_full,&st);
            if (open>=0) vfs_close(&ntfs_table,open);
            rec_emit("ntfs-ro","DATA","case=status cycle=%u id=%u open=%d stat=%d",cycle,id,open,stat);
            if (!ntfs_target[0]) memcpy(ntfs_target,ntfs_full,strlen(ntfs_full)+1);
        } else if (node->size>UINT32_MAX) {
            int open=vfs_open(&ntfs_table,ntfs_full,VFS_READ,VFS_DENY_NONE,0); if (open>=0) vfs_close(&ntfs_table,open);
            rec_emit("ntfs-ro","DATA","case=large cycle=%u id=%u size=%llu open=%d",cycle,id,node->size,open);
        } else {
            uint64_t size; char digest[65]; e=digest_in(&ntfs_table,ntfs_full,&size,digest); if (e) break;
            rec_emit("ntfs-ro","DATA","case=file cycle=%u id=%u size=%llu sha256=%s",cycle,id,size,digest);
            if (!ntfs_first[0]) { memcpy(ntfs_first,ntfs_full,strlen(ntfs_full)+1); memcpy(ntfs_target,ntfs_full,strlen(ntfs_full)+1); }
        }
    }
    while (top) { struct ntfs_frame *old=top; top=top->parent; fs_free(old); }
    rec_emit("ntfs-ro","DATA","case=names cycle=%u omitted=%u",cycle,ro->names_omitted);
    return e;
}
static bool peer_after(struct ata_device *peer, const uint8_t expected[32])
{
    uint8_t data[512], hash[32];
    if (!peer || peer->block.read(&peer->block,0,1,data)) return false;
    sha256(data,sizeof(data),hash); return !memcmp(hash,expected,32);
}
static int probe_ntfs_ro(void)
{
    rec_emit("ntfs-ro","BEGIN",0);
    struct storage *s=storage_get(); unsigned slot=0;
    while (slot<STORAGE_NTFS && !s->ntfs[slot].present) slot++;
    if (slot==STORAGE_NTFS) {
        rec_emit("ntfs-ro","DATA","status=not_run reason=fixtures_absent");
        rec_emit("ntfs-ro","END","status=NOT_RUN reason=fixtures_absent"); return 0;
    }
    struct storage_ntfs *n=&s->ntfs[slot]; unsigned drive=n->drive;
    rec_emit("ntfs-ro","DATA","case=device slot=%u disk=%u part=%u sectors=%llu result=%d reason=%s drive=%c",
             slot,n->disk,n->partition,n->io.capacity,n->error,n->ro.diagnostic ? n->ro.diagnostic : "unavailable",
             n->error ? '-' : 'A'+drive);
    if (n->error) {
        /* A refused volume never attaches, holds no workspace and wrote nothing. */
        bool absent=!n->ro.mounted && !vfs_drive_present(&s->vfs,drive) && !n->ro.state;
        rec_emit("ntfs-ro","DATA","case=refusal slot=%u result=%d reason=%s letter_absent=%u writes=%llu",
                 slot,n->error,n->ro.diagnostic ? n->ro.diagnostic : "unavailable",absent,n->writes+n->flushes);
        bool refused=absent && !n->writes && !n->flushes && !s->cache.writes;
        rec_emit("ntfs-ro","END","status=%s",refused ? "PASS" : "FAIL"); return refused ? 0 : 1;
    }
    bool pass=true;
    /* An ATA peer (the declared FAT fixture, else the boot disk) before and after. */
    struct ata_device *peer=0; uint8_t data[512], hash[32];
    for (unsigned i=0; i<4 && !peer; i++) {
        struct ata_device *candidate=ata_device_get(i/2,i%2);
        if (candidate && !candidate->atapi && i!=n->disk) peer=candidate;
    }
    if (peer && !peer->block.read(&peer->block,0,1,data)) sha256(data,sizeof(data),hash); else peer=0;
    rec_emit("ntfs-ro","DATA","case=peer stage=before result=%u",!!peer); pass &= !!peer;
    int e=vfs_table_init(&s->vfs,&ntfs_table,2); if (e) pass=false;
    ntfs_first[0]=0; ntfs_target[0]=0;
    int stale=-1, native=-1; uint32_t generation=s->vfs.generation[drive];
    if (pass) { e=ntfs_walk(n,1); rec_emit("ntfs-ro","DATA","case=cycle cycle=1 entries=%u result=%d",ntfs_entries,e); pass=!e; }
    if (pass && ntfs_target[0]) {
        struct fat_times times={0}; size_t done=0; uint64_t position; char ads[FS_PATH_BYTES];
        const char *file=ntfs_target;
        int mutation[11]; unsigned count=0;
        mutation[count++]=vfs_open(&ntfs_table,file,VFS_WRITE,VFS_DENY_NONE,0);
        mutation[count++]=vfs_open(&ntfs_table,file,VFS_READ|VFS_CREATE,VFS_DENY_NONE,0);
        mutation[count++]=vfs_open(&ntfs_table,file,VFS_WRITE|VFS_TRUNCATE,VFS_DENY_NONE,0);
        if (ntfs_first[0]) {
            /* A regular file: open handles and the native namespace as well. */
            stale=vfs_open(&ntfs_table,ntfs_first,VFS_READ,VFS_DENY_NONE,0); pass &= stale>=0;
            char native_path[FS_PATH_BYTES]; ksnprintf(native_path,sizeof(native_path),"/mnt/%c%s",'a'+drive,ntfs_first+2);
            e=files_bootstrap(&s->vfs); if (!e) native=file_open(proc_supervisor(),native_path,O_RDONLY,0); pass &= !e && native>=0;
            if (pass) {
                pass &= px_chdir(proc_supervisor(),native_path)==-ENOTDIR;
                char *leaf=native_path; for (char *p=native_path; *p; p++) if (*p=='/') leaf=p;
                *leaf=0; pass &= !px_chdir(proc_supervisor(),native_path);
            }
            mutation[count++]=vfs_write(&ntfs_table,stale,"x",1,&done);
            mutation[count++]=vfs_truncate(&ntfs_table,stale,0);
        }
        mutation[count++]=vfs_attrib(&ntfs_table,file,0);
        mutation[count++]=vfs_times(&ntfs_table,file,&times);
        mutation[count++]=vfs_rename(&ntfs_table,file,file);
        mutation[count++]=vfs_delete(&ntfs_table,file);
        mutation[count++]=vfs_mkdir(&ntfs_table,file,0);
        mutation[count++]=vfs_rmdir(&ntfs_table,file);
        for (unsigned i=0; i<count; i++) { rec_emit("ntfs-ro","DATA","case=mutation operation=%u result=%d",i,mutation[i]); pass &= mutation[i]==-FS_EROFS; }
        if (stale>=0) pass &= !vfs_seek(&ntfs_table,stale,0,VFS_SEEK_SET,&position) && !vfs_commit(&ntfs_table,stale);
        ksnprintf(ads,sizeof(ads),"%s:stream",file);
        int stream=vfs_open(&ntfs_table,ads,VFS_READ,VFS_DENY_NONE,0); if (stream>=0) vfs_close(&ntfs_table,stream);
        rec_emit("ntfs-ro","DATA","case=ads result=%d",stream); pass &= stream==-FS_EOPNOTSUPP;
        if (stale>=0) {
            /* Open files hold the volume; the cwd pin does not. */
            int busy=storage_ntfs_unmount(s,slot);
            rec_emit("ntfs-ro","DATA","case=unmount stage=busy result=%d mounted=%u",busy,n->ro.mounted);
            pass &= busy==-FS_EBUSY && n->ro.mounted;
        }
    }
    bool same=peer_after(peer,hash);
    rec_emit("ntfs-ro","DATA","case=peer stage=after result=%u",same); pass &= same;
    if (stale>=0) vfs_close(&ntfs_table,stale);
    if (native>=0) file_close(proc_supervisor(),native);
    if (pass) {
        e=storage_ntfs_unmount(s,slot);
        bool gone=!vfs_drive_present(&s->vfs,drive) && !n->ro.mounted;
        char cwd[CIUKI_PATH_MAX]; int expired=-1;
        if (native>=0) { vfs_lock(&s->vfs,0); expired=px_getcwd_locked(proc_supervisor()->cwd,cwd); vfs_unlock(&s->vfs); }
        rec_emit("ntfs-ro","DATA","case=unmount stage=clean result=%d letter_absent=%u generation_before=%u generation_after=%u cwd_expired=%u",
                 e,gone,generation,s->vfs.generation[drive],native<0 || expired==-ENOENT);
        pass &= !e && gone && s->vfs.generation[drive]>generation && (native<0 || expired==-ENOENT);
        if (native>=0) px_chdir(proc_supervisor(),"/");
        struct fat_entry st; int old=vfs_stat(&ntfs_table,ntfs_target[0] ? ntfs_target : "D:/",&st);
        rec_emit("ntfs-ro","DATA","case=gone stat=%d",old); pass &= old<0;
        e=storage_ntfs_mount(s,slot);
        rec_emit("ntfs-ro","DATA","case=remount result=%d reason=%s generation=%u",e,n->ro.diagnostic ? n->ro.diagnostic : "unavailable",
                 e>=3 ? s->vfs.generation[e] : 0);
        pass &= e>=3;
        if (e>=3) {
            drive=(unsigned)e; ntfs_first[0]=0; ntfs_target[0]=0; e=ntfs_walk(n,2);
            rec_emit("ntfs-ro","DATA","case=cycle cycle=2 entries=%u result=%d",ntfs_entries,e); pass &= !e;
        }
    }
    if (native>=0) px_chdir(proc_supervisor(),"/");
    if (ntfs_table.vfs) vfs_table_destroy(&ntfs_table);
    rec_emit("ntfs-ro","DATA","case=counters reads=%llu view_writes=%llu view_flushes=%llu cache_writes=%llu",
             n->reads,n->writes,n->flushes,s->cache.writes);
    pass &= !n->writes && !n->flushes && !s->cache.writes;
    rec_emit("ntfs-ro","END","status=%s",pass ? "PASS" : "FAIL"); return pass ? 0 : 1;
}
CIUKI_F4_PROBE("ntfs-ro",probe_ntfs_ro);

/* F2-74: the same generation-qualified storage removal used by /dev/media.
 * Uses only the runner's expendable USB fixture, never the boot volume. */
#include <ciuki/usb.h>
static struct vfs_table removal_table;
static int probe_media_remove(void)
{
    rec_emit("media-remove","BEGIN",0);
    struct storage *s=storage_get(); unsigned drive=26;
    uint64_t deadline=deadline_after_ms(20000);
    while (deadline_after_ms(0)<deadline && drive==26) {
        vfs_lock(&s->vfs,0);
        for (unsigned d=4;d<26;d++) if (s->volumes[d].present && s->volumes[d].removable &&
            s->vfs.volumes[d]==&s->volumes[d].fat) { drive=d; break; }
        vfs_unlock(&s->vfs);
        if (drive==26) task_sleep_ms(100);
    }
    bool pass=drive<26;
    int held=-1,e=-FS_ENOENT,busy=-FS_ENOENT;
    struct usb_device *device=0;
    if (pass) {
        struct storage_volume *v=&s->volumes[drive];
        struct blkdev *block=v->part.parent; device=block->ctx;
        uint32_t generation=block->media_generation;
        /* Removable publication is read-only; this write/removal probe
         * explicitly requests the normal ownership-checked upgrade. */
        e=storage_enable_write(s,drive);
        if (!e) e=vfs_table_init(&s->vfs,&removal_table,2);
        pass=!e && !v->fat.readonly;
        char path[]="E:/SAFE.TXT";path[0]=(char)('A'+drive);
        if (pass) { held=vfs_open(&removal_table,path,VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0);pass=held>=0; }
        size_t written=0;
        if (pass) { e=vfs_write(&removal_table,held,"Ciuki safe removal\n",19,&written);pass=!e && written==19; }
        if (pass) {
            uint64_t writes=v->writes,flushes=v->flushes;
            busy=usb_storage_eject(s,drive,generation);
            bool unchanged=v->writes==writes && v->flushes==flushes && v->present;
            rec_emit("media-remove","DATA","case=busy result=%d unchanged=%u",busy,unchanged);
            pass=busy==-FS_EBUSY && unchanged;
        }
        if (held>=0) { e=vfs_close(&removal_table,held);held=-1;pass &= !e; }
        if (pass) {
            e=usb_storage_eject(s,drive,generation);
            bool absent=!vfs_drive_present(&s->vfs,drive);
            bool released=device->storage.ejected && !device->storage.online && !device->storage.published;
            rec_emit("media-remove","DATA","case=safe_remove result=%d absent=%u released=%u enumerated=%u",
                     e,absent,released,device->present && device->configured);
            pass=!e && absent && released && device->present && device->configured;
        }
        if (pass) {
            rec_emit("media-remove","ARM","action=usb_detach");
            deadline=deadline_after_ms(10000);
            while (device->present && deadline_after_ms(0)<deadline) task_sleep_ms(100);
            rec_emit("media-remove","DATA","case=detach absent=%u safe=%u",!device->present,v->safely_removed);
            pass=!device->present && v->safely_removed;
        }
    }
    if (held>=0) vfs_close(&removal_table,held);
    if (removal_table.vfs) vfs_table_destroy(&removal_table);
    rec_emit("media-remove","END","status=%s",pass ? "PASS" : "FAIL");return pass ? 0 : 1;
}
CIUKI_F4_PROBE("media-remove",probe_media_remove);
