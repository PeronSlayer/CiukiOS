/* Real Win32 drive service over production POSIX/VFS/FAT, fake block device.
 * Object-table lifetime is isolated here; the production table has its own
 * kernel_core_test. No PE execution or guest boot is claimed.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "../proc/files_test.c"
#include <ciuki/w32_core.h>
#include <ciuki/w32_objects.h>
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
  CHECK(path_request("C:\\System", W32_F_MKDIR) == 5);
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
