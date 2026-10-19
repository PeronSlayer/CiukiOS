/* Additive F5-06 transport; native F2 syscall numbers are unchanged.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_W32_CORE_H
#define CIUKI_W32_CORE_H
#include <ciuki/w32_transport.h>
#define W32_FILE 0x109u
#define W32_SYSTEM 0x10au
#define W32_PROCESS 0x10bu
#define W32_SNAPSHOT 0x10cu
enum w32_file_action {
  W32_F_OPEN = 1,
  W32_F_READ,
  W32_F_WRITE,
  W32_F_SEEK,
  W32_F_TRUNCATE,
  W32_F_FLUSH,
  W32_F_STAT,
  W32_F_ATTR,
  W32_F_TIMES,
  W32_F_DELETE,
  W32_F_MOVE,
  W32_F_MKDIR,
  W32_F_RMDIR,
  W32_F_CHDIR,
  W32_F_CWD,
  W32_F_FULLPATH,
  W32_F_FIRST,
  W32_F_NEXT,
  W32_F_DISK
};
struct ciuki_w32_file_args {
  struct ciuki_w32_header header;
  uint32_t action, handle, path, path2, buffer, bytes;
  uint32_t access, share, disposition, flags, low, high, result, error;
};
/* FAT local timestamps remain packed here; kernel32 owns FILETIME conversion.
 */
struct ciuki_w32_file_info {
  uint32_t attributes, size_low, size_high;
  uint16_t create_date, create_time, access_date, write_date, write_time;
  uint8_t create_tenths, reserved;
  char name[768], alias[40]; /* VFS UTF-8 names */
};
struct ciuki_w32_disk_info {
  uint32_t drives, type, serial, cluster_bytes, sector_bytes, max_component;
  uint32_t free_low, free_high, total_low, total_high;
  char label[64], filesystem[16];
};
enum w32_system_action {
  W32_S_TIME = 1,
  W32_S_MEMORY,
  W32_S_TERMINATE_PROCESS,
  W32_S_TERMINATE_THREAD,
  W32_S_THREAD_EXIT,
  W32_S_YIELD,
  W32_S_THREAD_PRIORITY,
  W32_S_PROCESS_PRIORITY,
  W32_S_QUERY_PRIORITY
};
struct ciuki_w32_system_args {
  struct ciuki_w32_header header;
  uint32_t action, handle, value, low, high, extra, result, error;
};
_Static_assert(sizeof(struct ciuki_w32_file_args) == 72, "file wire");
_Static_assert(sizeof(struct ciuki_w32_file_info) == 832, "file info wire");
_Static_assert(sizeof(struct ciuki_w32_disk_info) == 120, "disk wire");
_Static_assert(sizeof(struct ciuki_w32_system_args) == 48, "system wire");
struct ciuki_w32_startup {
  uint32_t x, y, width, height, chars_x, chars_y, fill, flags, show, input,
      output, error, title;
};
struct ciuki_w32_parameters_v3 {
  struct ciuki_w32_parameters_v2 inherited;
  struct ciuki_w32_startup startup;
};
_Static_assert(sizeof(struct ciuki_w32_startup) == 52, "startup wire");
_Static_assert(sizeof(struct ciuki_w32_parameters_v3) == 84,
               "parameters v3 wire");
struct ciuki_w32_spawn_args {
  struct ciuki_w32_header header;
  uint32_t image, command, directory, environment, environment_bytes, inherit,
      flags;
  uint32_t process_handle, thread_handle, pid, tid, error;
  uint32_t startup, handle_flags;
};
_Static_assert(sizeof(struct ciuki_w32_spawn_args) == 72, "spawn wire");
struct ciuki_w32_snapshot_args {
  struct ciuki_w32_header header;
  uint32_t action, handle, flags, pid, buffer, bytes, result, error;
};
struct ciuki_w32_snapshot_entry {
  uint32_t kind, id, parent, threads, priority, owner, base, bytes;
  char name[260], path[260];
};
_Static_assert(sizeof(struct ciuki_w32_snapshot_args) == 48, "snapshot wire");
_Static_assert(sizeof(struct ciuki_w32_snapshot_entry) == 552,
               "snapshot entry wire");
#endif
