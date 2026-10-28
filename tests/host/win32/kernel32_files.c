/* Real Win32 drive service over production POSIX/VFS/FAT, fake block device.
 * Object-table lifetime is isolated here; the production table has its own
 * kernel_core_test. No PE execution or guest boot is claimed.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "../proc/files_test.c"
#include <ciuki/w32_core.h>
#include <ciuki/w32_objects.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
void w32_refusal_queue(struct process *p,uint32_t why,const char *name,uint32_t error){(void)p;(void)name;CHECK(why==W32_REFUSE_DRIVER&&error==5);}
static struct w32_object *handles[64];
static unsigned core_allocations;
void *w32_alloc(uint32_t n) {
  void *p = calloc(1, n);
  if (p)
    core_allocations++;
  return p;
}
void w32_free(void *p, uint32_t n) {
  (void)n;
  if (p) {
    core_allocations--;
    free(p);
  }
}
struct w32_object *w32_object_new(uint32_t type) {
  struct w32_object *o = w32_alloc(sizeof(*o));
  if (o) {
    o->type = type;
    o->refs = 1;
  }
  return o;
}
void w32_retain(struct w32_object *o) {
  if (o)
    o->refs++;
}
extern void w32_find_destroy(struct w32_object *);
void w32_release(struct w32_object *o) {
  if (o && !--o->refs) {
    if (o->type == W32_OBJ_FILE && o->u.file.description)
      o->u.file.description->object.release(&o->u.file.description->object);
    if (o->type == W32_OBJ_FIND)
      w32_find_destroy(o);
    w32_free(o, sizeof(*o));
  }
}
struct w32_object *w32_lookup(struct proc_thread *t, uint32_t h,
                              uint32_t type) {
  (void)t;
  struct w32_object *o = h && !(h & 3) && h / 4 < 64 ? handles[h / 4] : 0;
  return o && o->type == type ? o : 0;
}
uint32_t w32_handle_install(struct process *p, struct w32_object *o,
                            uint32_t access, bool inherit, uint32_t *h) {
  (void)p;
  (void)access;
  (void)inherit;
  for (unsigned i = 1; i < 64; i++)
    if (!handles[i]) {
      handles[i] = o;
      w32_retain(o);
      *h = i * 4;
      return 0;
    }
  return 4;
}
static void close_handle(uint32_t h) {
  CHECK(h / 4 < 64 && handles[h / 4]);
  struct w32_object *o = handles[h / 4];
  handles[h / 4] = 0;
  w32_release(o);
}
#include "../../../src/kernel/proc/win32/files.c"
static uint32_t path_request(const char *path, unsigned action) {
  put_path(USER, path);
  struct ciuki_w32_file_args a = {.action = action, .path = USER};
  CHECK(!w32_file_call(client_thread, &a));
  return a.error;
}
/* f1-89 Amendment 1 item 2: a DOS delete parked after taking the writer
 * turn but before publishing its tombstone, against Win32 SetFileAttributes
 * of the same path. The attribute call must find the file gone and must
 * never rewrite (resurrect) the deleted slot. */
int __real_cache_reserve(struct block_cache *, struct blkdev *, const struct cache_reserve_sector *,
                         unsigned, struct cache_reservation *);
static atomic_bool race_armed, race_parked, race_release;
static _Thread_local bool race_actor;
int __wrap_cache_reserve(struct block_cache *c, struct blkdev *d, const struct cache_reserve_sector *set,
                         unsigned n, struct cache_reservation *r) {
  if (race_actor && atomic_exchange(&race_armed, false)) {
    atomic_store(&race_parked, true);
    while (!atomic_load(&race_release)) sched_yield();
  }
  return __real_cache_reserve(c, d, set, n, r);
}
static int race_delete_error;
static void *race_delete(void *arg) {
  (void)arg; race_actor = true;
  struct vfs_table t; CHECK(!vfs_table_init(&vfs, &t, 2));
  race_delete_error = vfs_delete(&t, "C:/WINDOWS/RACE.TXT");
  vfs_table_destroy(&t); return 0;
}
static void *race_release_when_queued(void *arg) {
  (void)arg;
  for (bool queued = false; !queued; sched_yield()) {
    vfs_lock(&vfs, 0); queued = vfs.turn_waiters[2] != 0; vfs_unlock(&vfs);
  }
  atomic_store(&race_release, true); return 0;
}
static uint32_t path_request(const char *path, unsigned action);
static void delete_attr_race(void) {
  int fd = file_open(client, "/WINDOWS/RACE.TXT", O_CREAT | O_RDWR, 0666);
  CHECK(fd >= 0 && io(fd, "race", 4, true, false, 0) == 4 && !file_close(client, fd));
  atomic_store(&race_parked, false); atomic_store(&race_release, false); atomic_store(&race_armed, true);
  pthread_t deleter, releaser;
  CHECK(!pthread_create(&deleter, 0, race_delete, 0));
  while (!atomic_load(&race_parked)) sched_yield();
  CHECK(!pthread_create(&releaser, 0, race_release_when_queued, 0));
  uint32_t error = path_request("C:\\WINDOWS\\RACE.TXT", W32_F_ATTR);
  CHECK(!pthread_join(releaser, 0) && !pthread_join(deleter, 0));
  CHECK(!race_delete_error && error == 2);
  struct vfs_table t; struct fat_entry e; CHECK(!vfs_table_init(&vfs, &t, 2));
  CHECK(vfs_stat(&t, "C:/WINDOWS/RACE.TXT", &e) == -FS_ENOENT);
  vfs_table_destroy(&t);
  puts("PASS f1-89 delete versus SetFileAttributes: attribute call queued for the writer turn, "
       "ERROR_FILE_NOT_FOUND, deleted slot not resurrected");
}
/* fx-emptydrive: an empty CD-ROM drive (E:) and floppy drive (A:) keep their
 * letters. GetLogicalDrives lists them, GetDriveType answers DRIVE_CDROM /
 * DRIVE_REMOVABLE from the record copied with ERROR_NOT_READY, volume, space,
 * open, find, attribute and directory requests fail with ERROR_NOT_READY (21);
 * a letter nobody owns stays ERROR_INVALID_DRIVE (15) / not found. */
static struct ciuki_w32_file_args open_request(const char *path, unsigned access, unsigned share, unsigned disposition);
static uint32_t disk_request(const char *path, struct ciuki_w32_disk_info *info) {
  if (path) put_path(USER, path);
  struct ciuki_w32_file_args a = {.action = W32_F_DISK, .path = path ? USER : 0,
                                  .buffer = USER2, .bytes = sizeof(*info)};
  CHECK(!ua_write(client->memory, USER2, &(struct ciuki_w32_disk_info){0}, sizeof(*info)));
  CHECK(!w32_file_call(client_thread, &a));
  CHECK(!ua_read(client->memory, info, USER2, sizeof(*info)));
  return a.error;
}
static void empty_drives(void) {
  vfs_lock(&vfs, 0); vfs.slot_type[4] = VFS_SLOT_OPTICAL; vfs.slot_type[0] = VFS_SLOT_FLOPPY; vfs_unlock(&vfs);
  struct ciuki_w32_disk_info info;
  CHECK(!disk_request(0, &info) && info.drives == ((1u << 0) | (1u << 2) | (1u << 3) | (1u << 4)) && info.type == 3);
  CHECK(disk_request("E:\\", &info) == 21 && info.type == 5 && info.drives == 0x1du && !info.label[0] &&
        !info.filesystem[0] && !info.total_low && !info.free_low);
  CHECK(disk_request("A:\\", &info) == 21 && info.type == 2);
  CHECK(disk_request("D:\\", &info) == 0 && info.type == 3);
  CHECK(disk_request("F:\\", &info) == 15 && !info.type);
  struct ciuki_w32_file_args a = open_request("E:\\SETUP.EXE", 0x80000000u, 7, 3);
  CHECK(a.error == 21 && !a.result);
  CHECK(open_request("A:\\README.TXT", 0x80000000u, 7, 3).error == 21);
  CHECK(open_request("E:\\NEW.TXT", 0x40000000u, 0, 2).error == 21);
  CHECK(open_request("F:\\SETUP.EXE", 0x80000000u, 7, 3).error != 21);
  CHECK(path_request("E:\\GAMES", W32_F_MKDIR) == 21);
  put_path(USER, "E:\\*.*");
  struct ciuki_w32_file_info found;
  a = (struct ciuki_w32_file_args){.action = W32_F_FIRST, .path = USER, .buffer = USER2, .bytes = sizeof(found)};
  CHECK(!w32_file_call(client_thread, &a) && a.error == 21 && !a.result);
  put_path(USER, "E:\\SETUP.EXE");
  a = (struct ciuki_w32_file_args){.action = W32_F_STAT, .path = USER, .buffer = USER2, .bytes = sizeof(found)};
  CHECK(!w32_file_call(client_thread, &a) && a.error == 21);
  vfs_lock(&vfs, 0); vfs.slot_type[4] = 0; vfs.slot_type[0] = 0; vfs_unlock(&vfs);
  CHECK(disk_request("E:\\", &info) == 15);
  puts("PASS fx-emptydrive Win32 drives: empty CD-ROM E: and floppy A: in GetLogicalDrives, DRIVE_CDROM/DRIVE_REMOVABLE "
       "with ERROR_NOT_READY for volume/space, open/create/find/stat/mkdir ERROR_NOT_READY, unknown letter ERROR_INVALID_DRIVE");
}
static void empty_drives_by_identity(void) {
  struct ciuki_credential saved = client->subject->credential;
  for (unsigned admin = 0; admin < 2; admin++) {
    client->subject->credential = (struct ciuki_credential){
        .version = 1, .size = sizeof(struct ciuki_credential), .uid = 1000,
        .primary_gid = 545, .session_id = 1, .flags = CIUKI_CRED_INTERACTIVE,
        .group_count = admin ? 2 : 1, .groups = {admin ? 544 : 545, 545}};
    CHECK(security_admin(client) == (bool)admin);
    empty_drives();
  }
  client->subject->credential = saved;
  puts("PASS fx-emptydrive administrator and Standard user: no find handle without media");
}
static struct ciuki_w32_file_args open_request(const char *path,
                                               unsigned access, unsigned share,
                                               unsigned disposition) {
  put_path(USER, path);
  struct ciuki_w32_file_args a = {.action = W32_F_OPEN,
                                  .path = USER,
                                  .access = access,
                                  .share = share,
                                  .disposition = disposition};
  CHECK(!w32_file_call(client_thread, &a));
  return a;
}
static uint32_t lock_request(uint32_t h,unsigned op,uint64_t offset,uint64_t bytes,unsigned flags) {
  struct ciuki_w32_file_args a={.action=op,.handle=h,.low=(uint32_t)offset,
      .high=(uint32_t)(offset>>32),.bytes=(uint32_t)bytes,.share=(uint32_t)(bytes>>32),.flags=flags};
  CHECK(!w32_file_call(client_thread,&a));return a.error;
}
static struct ciuki_w32_file_info handle_info(uint32_t h) {
  struct ciuki_w32_file_args a = {.action = W32_F_STAT, .handle = h,
      .buffer = USER2, .bytes = sizeof(struct ciuki_w32_file_info)};
  struct ciuki_w32_file_info info;
  CHECK(!w32_file_call(client_thread, &a) && !a.error);
  CHECK(!ua_read(client->memory, &info, USER2, sizeof(info)));
  return info;
}
static void amendment_file_identity(void) {
  const char *long_name = "C:\\WINDOWS\\Identity File.wav";
  struct ciuki_w32_file_args a = open_request(long_name, 0xc0000000u, 7, 1);
  CHECK(!a.error);
  uint32_t h = a.result;
  struct ciuki_w32_file_info first = handle_info(h);
  CHECK(first.volume_serial == 0x00c1a001 && first.file_id);
  struct vfs_file_identity *identity = handles[h/4]->u.file.description->identity;
  uint64_t cache_serial = identity->serial;
  CHECK(first.volume_serial == volumes[0].volume_serial &&
        first.file_id == (((uint64_t)identity->parent << 32) | identity->index) + 1);
  char short_name[128];
  CHECK(first.alias[0] && strcmp(first.name, first.alias));
  snprintf(short_name, sizeof(short_name), "C:\\WINDOWS\\%s", first.alias);
  a = open_request(short_name, 0x80000000u, 7, 3);
  CHECK(!a.error);
  uint32_t alias = a.result;
  struct ciuki_w32_file_info second = handle_info(alias);
  CHECK(second.volume_serial == first.volume_serial && second.file_id == first.file_id);
  a = open_request("C:\\WINDOWS\\Identity File 2.wav", 0xc0000000u, 7, 1);
  CHECK(!a.error);
  uint32_t distinct = a.result;
  second = handle_info(distinct);
  CHECK(second.volume_serial == first.volume_serial && second.file_id &&
        second.file_id != first.file_id);
  close_handle(distinct);
  /* Reopen while the alias retains the identity, then after final close. */
  close_handle(h);
  a = open_request(long_name, 0x80000000u, 7, 3);
  CHECK(!a.error);
  h = a.result;
  second = handle_info(h);
  CHECK(second.file_id == first.file_id && second.volume_serial == first.volume_serial);
  close_handle(alias);
  close_handle(h);
  /* Force the unreferenced cache slot to serve an unrelated file before A
   * reopens: exporting its cache serial fails this regression. */
  a = open_request("C:\\WINDOWS\\Identity Churn.wav", 0xc0000000u, 7, 1);
  CHECK(!a.error);
  close_handle(a.result);
  for (unsigned order = 0; order < 2; order++) {
    a = open_request(order ? long_name : short_name, 0x80000000u, 7, 3);
    CHECK(!a.error);
    second = handle_info(a.result);
    CHECK(second.file_id == first.file_id && second.volume_serial == first.volume_serial);
    CHECK(handles[a.result/4]->u.file.description->identity->serial != cache_serial);
    close_handle(a.result);
  }
  CHECK(!path_request("C:\\WINDOWS\\IDMOVE", W32_F_MKDIR));
  a = open_request(long_name, 0x80000000u, 7, 3);
  CHECK(!a.error);
  h = a.result;
  put_path(USER, long_name);
  put_path(USER+512, "C:\\WINDOWS\\IDMOVE\\Renamed.wav");
  a = (struct ciuki_w32_file_args){.action=W32_F_MOVE, .path=USER, .path2=USER+512};
  CHECK(!w32_file_call(client_thread, &a) && !a.error);
  second = handle_info(h);
  CHECK(second.volume_serial == first.volume_serial && second.file_id &&
        second.file_id != first.file_id);
  first = second;
  a = open_request("C:\\WINDOWS\\IDMOVE\\Renamed.wav", 0x80000000u, 7, 3);
  CHECK(!a.error);
  second = handle_info(a.result);
  CHECK(second.file_id == first.file_id);
  close_handle(a.result);
  /* A retained handle cannot export an identity from an earlier medium, a
   * quarantined device or an explicit raw-change expiry. */
  identity = handles[h/4]->u.file.description->identity;
  identity->volume->dev->media_generation++;
  second = handle_info(h);
  CHECK(!second.volume_serial && !second.file_id);
  identity->volume->dev->media_generation--;
  identity->volume->dev->quarantined = true;
  second = handle_info(h);
  CHECK(!second.volume_serial && !second.file_id);
  identity->volume->dev->quarantined = false;
  second = handle_info(h);
  CHECK(second.file_id == first.file_id && second.volume_serial == first.volume_serial);
  vfs_lock(&vfs, vfs_drive_mask(2));
  vfs_identity_expire(&vfs, identity->volume);
  vfs_unlock(&vfs);
  CHECK(!identity->valid);
  second = handle_info(h);
  CHECK(!second.volume_serial && !second.file_id);
  close_handle(h);
  a = open_request("C:\\dev\\null", 0x80000000u, 7, 3);
  CHECK(!a.error);
  second = handle_info(a.result);
  CHECK(!second.volume_serial && !second.file_id);
  close_handle(a.result);
  puts("PASS file-info identity: FAT volume serial, long/short alias, distinct files, cache churn, rename, expiry, device zero");
}
static void amendment_files(void) {
  struct ciuki_w32_file_args a=open_request("C:\\WINDOWS\\locks.bin",0xc0000000u,7,1);
  CHECK(!a.error);uint32_t h=a.result;
  a=open_request("C:\\WINDOWS\\locks.bin",0xc0000000u,7,3);
  CHECK(!a.error);uint32_t second=a.result;
  CHECK(handles[h/4]->u.file.description->identity);
  CHECK(handles[h/4]->u.file.description->identity==handles[second/4]->u.file.description->identity);
  CHECK(!lock_request(h,W32_F_LOCK,0,4,3));
  CHECK(lock_request(second,W32_F_LOCK,0,4,3)==33);
  CHECK(lock_request(second,W32_F_UNLOCK,0,4,0)==33);
  int fd=file_open(client,"/WINDOWS/locks.bin",O_RDWR,0);CHECK(fd>=0);
  char byte='X';CHECK(io(fd,&byte,1,false,true,0)==-EACCES);
  struct vfs_table table;CHECK(!vfs_table_init(&vfs,&table,2));
  for (unsigned type=0;type<VFS_OWNER_TYPES;type++) {
    CHECK(!vfs_table_set_owner(&table,type,client->pid));
    int old=vfs_open(&table,"C:/WINDOWS/locks.bin",VFS_READ|VFS_WRITE,VFS_DENY_NONE,0);CHECK(old>=0);
    size_t got=99;CHECK(vfs_read(&table,old,&byte,1,&got)==-VFS_ELOCK&&!got);
    CHECK(vfs_lock_range(&table,old,0,1,false)==-VFS_ELOCK);
    CHECK(!vfs_close(&table,old));
  }
  CHECK(!lock_request(h,W32_F_UNLOCK,0,4,0));
  CHECK(!lock_request(h,W32_F_LOCK,0,4,1)); /* shared */
  CHECK(!lock_request(second,W32_F_LOCK,0,4,1));
  CHECK(io(fd,&byte,1,true,true,0)==-EACCES);
  size_t mapped=0;vfs_lock(&vfs,0);
  CHECK(!file_section_io_locked(file_fd(client,fd),&byte,1,true,true,0,&mapped)&&mapped==1);
  vfs_unlock(&vfs);
  CHECK(io(fd,&byte,1,false,true,0)==1); /* shared reads admitted */
  CHECK(!lock_request(second,W32_F_UNLOCK,0,4,0));
  CHECK(!lock_request(h,W32_F_UNLOCK,0,4,0));
  CHECK(!lock_request(h,W32_F_LOCK,UINT64_C(0x100000010),UINT64_C(0x100000000),3));
  CHECK(lock_request(second,W32_F_LOCK,UINT64_C(0x100000011),1,3)==33);
  close_handle(h); /* final close releases all this description's claims */
  CHECK(!lock_request(second,W32_F_LOCK,UINT64_C(0x100000011),1,3));
  close_handle(second);CHECK(!file_close(client,fd));vfs_table_destroy(&table);
  a=open_request("C:\\WINDOWS\\locks.bin",0x80000000u,3,3);CHECK(!a.error);h=a.result;
  CHECK(path_request("C:\\WINDOWS\\locks.bin",W32_F_DELETE)==32);close_handle(h);
  a=open_request("C:\\WINDOWS\\locks.bin",0x80000000u,7,3);CHECK(!a.error);h=a.result;
  CHECK(!path_request("C:\\WINDOWS\\locks.bin",W32_F_DELETE));
  a=open_request("C:\\WINDOWS\\locks.bin",0x80000000u,7,3);CHECK(a.error==5);
  close_handle(h);CHECK(open_request("C:\\WINDOWS\\locks.bin",0,7,3).error==2);
  put_path(USER,"C:\\WINDOWS\\delete.bin");
  a=(struct ciuki_w32_file_args){.action=W32_F_OPEN,.path=USER,.access=0xc0000000u,.share=7,.disposition=1,.flags=0x04000000};
  CHECK(!w32_file_call(client_thread,&a)&&!a.error);h=a.result;
  a=open_request("C:\\WINDOWS\\delete.bin",0x80000000u,7,3);CHECK(!a.error);second=a.result;
  close_handle(h);CHECK(open_request("C:\\WINDOWS\\delete.bin",0,7,3).error==5);
  close_handle(second);CHECK(open_request("C:\\WINDOWS\\delete.bin",0,7,3).error==2);
  CHECK(!make_dir("/System"));
  a=open_request("C:\\WINDOWS\\protect.bin",0xc0000000u,7,1);CHECK(!a.error);h=a.result;
  vfs_lock(&vfs,0);CHECK(!px_rename_locked(space,client->cwd,"/WINDOWS/protect.bin","/System/protect.bin"));vfs_unlock(&vfs);
  a=(struct ciuki_w32_file_args){.action=W32_F_WRITE,.handle=h,.buffer=USER2,.bytes=1};
  CHECK(!w32_file_call(client_thread,&a)&&a.error==5&&!a.result);
  a=(struct ciuki_w32_file_args){.action=W32_F_TRUNCATE,.handle=h};CHECK(!w32_file_call(client_thread,&a)&&a.error==5);
  close_handle(h);
  puts("PASS amendment common owner locks shared high ranges delete sharing pending-close protected-handle rename");
}
int main(int argc, char **argv) {
  CHECK(argc == 3);
  ram = calloc(HOST_PAGES, PAGE_SIZE);
  CHECK(ram);
  controller.state = T_RUNNING;
  g_current = &controller;
  proc_init();
  CHECK(!fake_open(&disk[0], argv[1]) && !fake_open(&disk[1], argv[2]));
  disk[0].undo_enabled = disk[1].undo_enabled = true;
  CHECK(!cache_init(&cache, 0));
  vfs_init(&vfs);
  for (unsigned i = 0; i < 2; i++) {
    CHECK(!fat_mount(&volumes[i], &cache, &disk[i].dev, 0, disk[i].dev.capacity,
                     true));
    CHECK(!vfs_attach(&vfs, i + 2, &volumes[i]));
  }
  CHECK(!files_attach(&vfs, &space));
  client_thread = make_process(proc_supervisor(), &client);
  g_current = client_thread->task;
  g_current->state = T_RUNNING;
  static struct w32_process w32_state;
  client->w32 = &w32_state;
  client->cwd = space->root;
  px_retain(client->cwd);
  client->cwd_retain = px_retain;
  client->cwd_release = px_release;
  CHECK(path_request("C:\\WINDOWS", W32_F_MKDIR) == 0);
  CHECK(!make_dir("/WINDOWS/FONTS"));
  int font_fd=file_open(client,"/WINDOWS/FONTS/LiberationSans-Regular.ttf",O_CREAT|O_RDWR,0666);
  CHECK(font_fd>=0 && !file_close(client,font_fd));
  struct ciuki_w32_file_args font_read=open_request("C:\\WINDOWS\\FONTS\\LIBERA~1.TTF",0x80000000u,3,3);
  CHECK(!font_read.error);close_handle(font_read.result);
  CHECK(open_request("C:\\WINDOWS\\FONTS\\LIBERA~1.TTF",0x40000000u,3,3).error==5);
  CHECK(path_request("C:\\WINDOWS\\FONTS\\LIBERA~1.TTF",W32_F_DELETE)==5);
  CHECK(path_request("C:\\WINDOWS\\FONTS\\LIBERA~1.TTF",W32_F_ATTR)==5);
  CHECK(open_request("C:\\WINDOWS\\FONTS\\LiberationMono-Italic.ttf",0x40000000u,3,2).error==5);
  struct ciuki_w32_file_args installed=open_request("C:\\WINDOWS\\FONTS\\User Face.ttf",0x40000000u,3,1);
  CHECK(!installed.error);close_handle(installed.result);
  CHECK(!path_request("C:\\WINDOWS\\FONTS\\User Face.ttf",W32_F_DELETE));
  put_path(USER,"C:\\WINDOWS\\FONTS");put_path(USER+512,"C:\\Moved Fonts");
  struct ciuki_w32_file_args font_move={.action=W32_F_MOVE,.path=USER,.path2=USER+512};
  CHECK(!w32_file_call(client_thread,&font_move) && font_move.error==5);
  for(unsigned i=0;i<3;i++){
    const char *drivers[]={"C:\\WINDOWS\\fixture.sys","C:\\windows\\fixture.VXD","C:\\WINDOWS\\fixture.drv"};
    struct ciuki_w32_file_args refused=open_request(drivers[i],0xc0000000u,3,2);
    CHECK(refused.error==5&&!refused.result);
  }
  CHECK(path_request("C:\\System", W32_F_MKDIR) == 5);
  /* q3exec4: Installer VISE creates a product tree one component at a
   * time with a trailing backslash (C:\Q3Ademo\, C:\Q3Ademo\demoq3\);
   * a path rooted on the current drive (\demoq3\) is a directory of C:. */
  CHECK(!path_request("C:\\Q3Ademo\\", W32_F_MKDIR) &&
        !path_request("C:\\Q3Ademo\\demoq3\\", W32_F_MKDIR) &&
        !path_request("\\demoq3\\", W32_F_MKDIR) && path_request("", W32_F_MKDIR));
  {
    struct ciuki_w32_file_args rooted = open_request("\\demoq3\\pak0.pk3", 0x40000000u, 0, 2);
    CHECK(!rooted.error);
    close_handle(rooted.result);
    struct vfs_table t; struct fat_entry e; CHECK(!vfs_table_init(&vfs, &t, 2));
    CHECK(!vfs_stat(&t, "C:/demoq3/pak0.pk3", &e) && !vfs_stat(&t, "C:/Q3Ademo/demoq3", &e));
    vfs_table_destroy(&t);
  }
  amendment_file_identity();
  empty_drives_by_identity();
  amendment_files();
  delete_attr_race();
  struct ciuki_w32_file_args a =
      open_request("C:\\WINDOWS\\Long File.txt", 0xc0000000u, 1, 1);
  CHECK(!a.error && a.result);
  uint32_t h = a.result;
  a = open_request("C:\\windows\\long file.TXT", 0x40000000u, 3, 3);
  CHECK(a.error == 32 && !a.result);
  a = (struct ciuki_w32_file_args){
      .action = W32_F_WRITE, .handle = h, .buffer = USER2, .bytes = 4};
  CHECK(!ua_write(client->memory, USER2, "test", 4));
  CHECK(!w32_file_call(client_thread, &a) && !a.error && a.result == 4);
  a = (struct ciuki_w32_file_args){.action = W32_F_SEEK, .handle = h};
  CHECK(!w32_file_call(client_thread, &a) && !a.error && !a.low);
  a = (struct ciuki_w32_file_args){
      .action = W32_F_READ, .handle = h, .buffer = USER2, .bytes = 8};
  CHECK(!w32_file_call(client_thread, &a) && !a.error && a.result == 4);
  char b[8];
  CHECK(!ua_read(client->memory, b, USER2, 4) && !memcmp(b, "test", 4));
  a = (struct ciuki_w32_file_args){.action = W32_F_WRITE, .handle = h};
  CHECK(!w32_file_call(client_thread, &a) && !a.error && !a.result);
  CHECK(!ua_write(client->memory, USER2, "XY", 2));
  a = (struct ciuki_w32_file_args){.action = W32_F_WRITE,
                                   .handle = h,
                                   .buffer = USER2,
                                   .bytes = 2,
                                   .low = 2,
                                   .flags = 0x10000};
  CHECK(!w32_file_call(client_thread, &a) && !a.error && a.result == 2);
  a = (struct ciuki_w32_file_args){
      .action = W32_F_SEEK, .handle = h, .flags = 1};
  CHECK(!w32_file_call(client_thread, &a) && !a.error && a.low == 4);
  a = (struct ciuki_w32_file_args){.action = W32_F_READ,
                                   .handle = h,
                                   .buffer = USER2,
                                   .bytes = 3,
                                   .low = 1,
                                   .flags = 0x10000};
  CHECK(!w32_file_call(client_thread, &a) && !a.error && a.result == 3);
  CHECK(!ua_read(client->memory, b, USER2, 3) && !memcmp(b, "eXY", 3));
  close_handle(h);
  /* One Win32 request may exceed the native relay's 1-MiB chunk size. */
  uint32_t bulk = CIUKI_MMAP_BASE + 16 * PAGE_SIZE,
           bulk_bytes = CIUKI_IO_MAX + 17;
  CHECK(!ua_map_at(client->memory, bulk, CIUKI_IO_MAX + PAGE_SIZE,
                   PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE, UA_ANON, 0));
  uint8_t pattern[4096];
  for (unsigned i = 0; i < sizeof(pattern); i++)
    pattern[i] = (uint8_t)(i * 13);
  for (uint32_t at = 0; at < bulk_bytes; at += sizeof(pattern)) {
    uint32_t count = bulk_bytes - at;
    if (count > sizeof(pattern))
      count = sizeof(pattern);
    CHECK(!ua_write(client->memory, bulk + at, pattern, count));
  }
  a = open_request("C:\\WINDOWS\\Bulk.bin", 0xc0000000u, 0, 1);
  CHECK(!a.error);
  h = a.result;
  a = (struct ciuki_w32_file_args){
      .action = W32_F_WRITE, .handle = h, .buffer = bulk, .bytes = bulk_bytes};
  CHECK(!w32_file_call(client_thread, &a) && !a.error &&
        a.result == bulk_bytes);
  uint8_t zero[4096] = {0};
  for (uint32_t at = 0; at < bulk_bytes; at += sizeof(zero)) {
    uint32_t count = bulk_bytes - at;
    if (count > sizeof(zero))
      count = sizeof(zero);
    CHECK(!ua_write(client->memory, bulk + at, zero, count));
  }
  a = (struct ciuki_w32_file_args){.action = W32_F_READ,
                                   .handle = h,
                                   .buffer = bulk,
                                   .bytes = bulk_bytes,
                                   .flags = 0x10000};
  CHECK(!w32_file_call(client_thread, &a) && !a.error &&
        a.result == bulk_bytes);
  uint8_t edge[32];
  CHECK(!ua_read(client->memory, edge, bulk + CIUKI_IO_MAX - 15, sizeof(edge)));
  for (unsigned i = 0; i < sizeof(edge); i++)
    CHECK(edge[i] == (uint8_t)(((CIUKI_IO_MAX - 15 + i) % 4096) * 13));
  close_handle(h);
  CHECK(!ua_munmap(client->memory, bulk, CIUKI_IO_MAX + PAGE_SIZE));
  a = open_request("C:\\WINDOWS\\LONGFI~1.TXT", 0x80000000u, 3, 3);
  CHECK(!a.error);
  close_handle(a.result);
  CHECK(path_request("\\\\server\\share\\x", W32_F_STAT) == 53);
  CHECK(path_request("\\\\?\\C:\\x", W32_F_STAT) == 2);
  put_path(USER, "C:\\WINDOWS\\LONGFI~1.TXT");
  a = (struct ciuki_w32_file_args){.action = W32_F_STAT,
                                   .path = USER,
                                   .buffer = USER2,
                                   .bytes = sizeof(struct ciuki_w32_file_info)};
  CHECK(!w32_file_call(client_thread, &a) && !a.error);
  struct ciuki_w32_file_info info;
  CHECK(!ua_read(client->memory, &info, USER2, sizeof(info)) &&
        !strcmp(info.alias, "LONGFI~1.TXT") &&
        !strcmp(info.name, "Long File.txt"));
  put_path(USER, "C:\\WINDOWS\\*.*");
  a = (struct ciuki_w32_file_args){.action = W32_F_FIRST,
                                   .path = USER,
                                   .buffer = USER2,
                                   .bytes = sizeof(info)};
  CHECK(!w32_file_call(client_thread, &a) && !a.error && a.result);
  h = a.result;
  close_handle(h);
  CHECK(!path_request("C:\\WINDOWS\\subdir", W32_F_MKDIR));
  CHECK(!path_request("C:\\WINDOWS\\subdir", W32_F_CHDIR));
  put_path(USER, ".. \\Long File.txt. ");
  a = (struct ciuki_w32_file_args){.action = W32_F_STAT,
                                   .path = USER,
                                   .buffer = USER2,
                                   .bytes = sizeof(info)};
  CHECK(!w32_file_call(client_thread, &a) && !a.error);
  a = (struct ciuki_w32_file_args){
      .action = W32_F_CWD, .buffer = USER2, .bytes = 260};
  CHECK(!w32_file_call(client_thread, &a) && !a.error);
  CHECK(!ua_read(client->memory, b, USER2, 2) && !memcmp(b, "C:", 2));
  CHECK(!path_request("C:\\WINDOWS", W32_F_CHDIR));
  CHECK(!path_request("C:\\WINDOWS\\subdir", W32_F_RMDIR));
  unsigned writes = disk[0].writes;
  CHECK(path_request("C:\\System\\x", W32_F_DELETE) == 5 &&
        disk[0].writes == writes);
  CHECK(!path_request("D:\\Fixture dir", W32_F_CHDIR));
  put_path(USER, "C:Long File.txt");
  a = (struct ciuki_w32_file_args){.action = W32_F_STAT,
                                   .path = USER,
                                   .buffer = USER2,
                                   .bytes = sizeof(info)};
  CHECK(!w32_file_call(client_thread, &a) && !a.error);
  CHECK(!path_request("C:\\WINDOWS", W32_F_CHDIR));
  put_path(USER, "C:\\WINDOWS\\Long File.txt");
  a = (struct ciuki_w32_file_args){.action = W32_F_FULLPATH,
                                   .flags = 1,
                                   .path = USER,
                                   .buffer = USER2,
                                   .bytes = 260};
  CHECK(!w32_file_call(client_thread, &a) && !a.error);
  char short_name[260];
  CHECK(!ua_read(client->memory, short_name, USER2, a.result) &&
        !strcmp(short_name, "C:\\WINDOWS\\LONGFI~1.TXT"));
  for (unsigned d = 0; d < 26; d++)
    if (w32_state.drive_cwd[d])
      px_release(w32_state.drive_cwd[d]);
  client->w32 = 0;
  CHECK(!core_allocations);
  all_closed();
  stop_collect(client);
  for (unsigned i = 0; i < 2; i++)
    CHECK(!vfs_detach(&vfs, i + 2));
  vfs_destroy(&vfs);
  cache_destroy(&cache);
  for (unsigned i = 0; i < 2; i++) {
    fake_reset(&disk[i]);
    fake_close(&disk[i]);
  }
  CHECK(!pages_used);
  free(ram);
  puts("PASS kernel32 production VFS drive paths sharing aliases files "
       "directories find");
  return 0;
}
