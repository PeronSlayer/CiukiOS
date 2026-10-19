/* Drive-path file service. Native paths and descriptors retain their F2 ABI.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/files.h>
#include <ciuki/kernel.h>
#include <ciuki/w32_core.h>
#include <ciuki/w32_objects.h>
static uint32_t dos_error(int e) {
  switch (e) {
  case 0:
    return 0;
  case -ENOENT:
    return 2;
  case -ENOTDIR:
    return 3;
  case -EMFILE:
    return 4;
  case -EACCES:
  case -EPERM:
  case -EROFS:
    return 5;
  case -EBADF:
    return 6;
  case -EFAULT:
    return 998;
  case -ENOMEM:
    return 8;
  case -EXDEV:
    return 17;
  case -EIO:
    return 1117;
  case -ENOSPC:
    return 112;
  case -EEXIST:
    return 80;
  case -ENOTEMPTY:
    return 145;
  case -ENAMETOOLONG:
    return 206;
  case -EFBIG:
  case -EOVERFLOW:
    return 223;
  case -ENOSYS:
    return 120;
  default:
    return 87;
  }
}
static int copy_path(uint32_t va, char out[FS_PATH_BYTES]) {
  if (!va)
    return -EINVAL;
  for (unsigned i = 0; i < FS_PATH_BYTES; i++) {
    if (va > UINT32_MAX - i || copy_from_user(out + i, va + i, 1))
      return -EFAULT;
    if (!out[i])
      return i ? 0 : -ENOENT;
  }
  return -ENAMETOOLONG;
}
static bool driver_destination(const char *full){
  static const char root[]="C:\\WINDOWS\\";
  for(unsigned i=0;i<sizeof(root)-1;i++)if(path_upper((uint8_t)full[i])!=(uint8_t)root[i])return false;
  unsigned n=(unsigned)strlen(full);if(n<4||full[n-4]!='.')return false;
  char ext[4];for(unsigned i=0;i<3;i++)ext[i]=(char)path_upper((uint8_t)full[n-3+i]);ext[3]=0;
  return !strncmp(ext,"VXD",4)||!strncmp(ext,"SYS",4)||!strncmp(ext,"DRV",4);
}
/* Trim each component before lexical normalization, so ".. " is a parent.
 * UNC and device prefixes are classified before slash conversion. */
uint32_t w32_native_path(struct process *p, const char *name,
                         char out[CIUKI_PATH_MAX], char full[FS_PATH_BYTES]) {
  if ((name[0] == '\\' || name[0] == '/') &&
      (name[1] == '\\' || name[1] == '/'))
    return name[2] == '?' || name[2] == '.' ? 2 : 53;
  char clean[FS_PATH_BYTES], cwd[CIUKI_PATH_MAX], normal[FS_PATH_BYTES];
  unsigned at = 0;
  for (const char *q = name; *q;) {
    if (*q == '/' || *q == '\\') {
      clean[at++] = '/';
      q++;
      continue;
    }
    const char *s = q;
    while (*q && *q != '/' && *q != '\\')
      q++;
    unsigned n = (unsigned)(q - s);
    while (n && s[n - 1] == ' ')
      n--;
    if (!(n == 1 && s[0] == '.') && !(n == 2 && s[0] == '.' && s[1] == '.'))
      while (n && s[n - 1] == '.')
        n--;
    if (!n)
      return 123;
    if (at + n >= sizeof(clean))
      return 206;
    memcpy(clean + at, s, n);
    at += n;
  }
  clean[at] = 0;
  unsigned drive = p->cwd ? ((struct px_node *)p->cwd)->drive : 2;
  if (drive >= 26)
    drive = 2;
  unsigned desired = drive;
  if (clean[0] && clean[1] == ':') {
    desired = (unsigned)(path_upper((uint8_t)clean[0]) - 'A');
    if (desired >= 26)
      return 15;
  }
  struct px_node *saved = desired == drive ? p->cwd
                          : p->w32         ? p->w32->drive_cwd[desired]
                                           : 0;
  memcpy(cwd, "/", 2);
  if (saved) {
    vfs_lock(saved->space->vfs, 0);
    int e = px_getcwd_locked(saved, cwd);
    vfs_unlock(saved->space->vfs);
    if (e)
      return dos_error(e);
  }
  const char *relative = desired == 2                ? cwd
                         : !strncmp(cwd, "/mnt/", 5) ? cwd + 6
                                                     : "/";
  unsigned d = drive;
  int e = path_normalize(clean, drive, relative, &d, normal);
  if (e)
    return dos_error(px_error(e));
  unsigned n = (unsigned)strlen(normal);
  if (n + 2 >= FS_PATH_BYTES)
    return 206;
  full[0] = (char)('A' + d);
  full[1] = ':';
  for (unsigned i = 0; i <= n; i++)
    full[i + 2] = normal[i] == '/' ? '\\' : normal[i];
  if (d == 2) {
    if (n >= CIUKI_PATH_MAX)
      return 206;
    memcpy(out, normal, n + 1);
  } else {
    if (n + 6 >= CIUKI_PATH_MAX)
      return 206;
    memcpy(out, "/mnt/", 5);
    out[5] = (char)('A' + d);
    memcpy(out + 6, normal, n + 1);
  }
  return 0;
}
static bool system_node(const struct px_node *n) {
  while (n && n->parent && n != n->parent) {
    if (n->drive == 2 && n->parent->mountpoint &&
        path_equal(n->entry.name, "System"))
      return true;
    n = n->parent;
  }
  return false;
}
static bool protected_path(struct px_namespace *s, struct px_node *cwd,
                           const char *path) {
  struct px_path r;
  int e = px_resolve_locked(s, cwd, path, true, &r);
  if (!e && system_node(r.node ? r.node : r.parent))
    return true;
  /* Guard missing children as well as aliases, including a missing System. */
  if (path[0] != '/' || strlen(path) < 7)
    return false;
  const char *prefix = "/SYSTEM";
  for (unsigned i = 0; i < 7; i++)
    if (path_upper((uint8_t)path[i]) != (uint8_t)prefix[i])
      return false;
  return !path[7] || path[7] == '/';
}
static void file_info(struct ciuki_w32_file_info *f, const struct fat_entry *e,
                      uint64_t size) {
  memset(f, 0, sizeof(*f));
  f->attributes = e->attr ? e->attr : 128;
  f->size_low = (uint32_t)size;
  f->size_high = (uint32_t)(size >> 32);
  f->create_date = e->times.create_date;
  f->create_time = e->times.create_time;
  f->access_date = e->times.access_date;
  f->write_date = e->times.write_date;
  f->write_time = e->times.write_time;
  f->create_tenths = e->times.create_tenths;
  memcpy(f->name, e->name, sizeof(e->name));
  memcpy(f->alias, e->alias, sizeof(e->alias));
}
struct find_state {
  struct vfs_find search;
};
void w32_find_destroy(struct w32_object *o) {
  w32_free(o->u.section, sizeof(struct find_state));
}
static struct vfs_table *temporary_table(struct vfs *v) {
  struct vfs_table *table = w32_alloc(sizeof(*table));
  if (table) {
    table->vfs = v;
    table->drive = 2;
    for (unsigned i = 0; i < 26; i++)
      memcpy(table->cwd[i], "/", 2);
  }
  return table;
}
uint32_t w32_file_call(struct proc_thread *t, struct ciuki_w32_file_args *a) {
  struct process *p = t->process;
  struct px_node *cwd = p->cwd;
  if (!cwd) {
    a->error = 3;
    return 0;
  }
  struct px_namespace *s = cwd->space;
  char *names = w32_alloc(FS_PATH_BYTES * 3 + CIUKI_PATH_MAX * 2);
  if (!names) {
    a->error = 8;
    return 0;
  }
  char *input = names, *full = input + FS_PATH_BYTES,
       *full2 = full + FS_PATH_BYTES;
  char *path = full2 + FS_PATH_BYTES, *path2 = path + CIUKI_PATH_MAX;
  uint32_t error = 0;
  if (a->path) {
    int e = copy_path(a->path, input);
    error = e ? dos_error(e) : w32_native_path(p, input, path, full);
  } else
    path[0] = full[0] = 0;
  if (!error && a->path2) {
    int e = copy_path(a->path2, input);
    error = e ? dos_error(e) : w32_native_path(p, input, path2, full2);
  } else
    path2[0] = 0;
  if (error)
    goto done;
  if (a->action == W32_F_CWD) {
    error = w32_native_path(p, ".", path, full);
  }
  if (a->action == W32_F_FULLPATH || a->action == W32_F_CWD) {
    if (!error && a->action == W32_F_FULLPATH && (a->flags & 3)) {
      vfs_lock(s->vfs, 0);
      struct px_path r;
      int e = px_resolve_locked(s, cwd, path, false, &r);
      if (e)
        error = dos_error(e);
      else {
        unsigned used = FS_PATH_BYTES - 1;
        full2[used] = 0;
        struct px_node *node = r.node;
        while (node && node->parent && node != node->parent &&
               !node->mountpoint) {
          const char *part = (a->flags & 1) && node->entry.alias[0]
                                 ? node->entry.alias
                                 : node->entry.name;
          unsigned length = (unsigned)strlen(part);
          if (length + 1 > used) {
            error = 206;
            break;
          }
          used -= length;
          memcpy(full2 + used, part, length);
          full2[--used] = '\\';
          node = node->parent;
        }
        if (!error) {
          unsigned length = FS_PATH_BYTES - 1 - used;
          if (!length) {
            full2[--used] = '\\';
            length++;
          }
          if (length + 3 > FS_PATH_BYTES)
            error = 206;
          else {
            full[0] = (char)('A' + r.node->drive);
            full[1] = ':';
            memcpy(full + 2, full2 + used, length + 1);
          }
        }
      }
      vfs_unlock(s->vfs);
    }
    if (error)
      goto done;
    uint32_t n = (uint32_t)strlen(full) + 1;
    a->result = n;
    if (a->bytes < n)
      error = 122;
    else if (copy_to_user(a->buffer, full, n))
      error = 998;
    goto done;
  }
  if (a->action == W32_F_DISK) {
    struct ciuki_w32_disk_info info = {.sector_bytes = 512,
                                       .max_component = 255};
    unsigned d = full[0] ? full[0] - 'A' : cwd->drive;
    struct vfs *v = s->vfs;
    vfs_lock(v, 0);
    for (unsigned i = 0; i < 26; i++)
      if (v->volumes[i] || v->ro_volumes[i])
        info.drives |= 1u << i;
    if (d >= 26 || !(info.drives & (1u << d)))
      error = 15;
    else {
      vfs_reserve_locked(v, vfs_drive_mask(d));
      struct fat_volume *vol = v->volumes[d];
      struct fs_ro_volume *ro = v->ro_volumes[d];
      uint64_t free = 0, total = 0;
      if (vol) {
        int e = 0;
        if (e)
          error = dos_error(px_error(e));
        info.type = v->drive_flags[d] & VFS_DRIVE_REMOVABLE ? 2 : 3;
        info.serial = vol->volume_serial;
        info.cluster_bytes = vol->spc * 512;
        free = (uint64_t)vol->free_clusters * info.cluster_bytes;
        total = (uint64_t)vol->clusters * info.cluster_bytes;
        memcpy(info.label, vol->volume_label, 12);
        memcpy(info.filesystem, vol->type == 32 ? "FAT32" : "FAT",
               vol->type == 32 ? 6 : 4);
      } else {
        info.type = path_equal(ro->ops->name, "CDFS") ||
                            path_equal(ro->ops->name, "ISO9660")
                        ? 5
                        : 3;
        info.sector_bytes = ro->dev->sector_size;
        info.cluster_bytes = info.sector_bytes;
        total = ro->sectors * info.sector_bytes;
        info.serial = (uint32_t)ro->volume_identity;
        memcpy(info.label, ro->volume_label, 33);
        memcpy(info.filesystem, info.type == 5 ? "CDFS" : "NTFS", 5);
      }
      info.free_low = (uint32_t)free;
      info.free_high = (uint32_t)(free >> 32);
      info.total_low = (uint32_t)total;
      info.total_high = (uint32_t)(total >> 32);
    }
    vfs_unlock(v);
    if (!error && (a->bytes != sizeof(info) ||
                   copy_to_user(a->buffer, &info, sizeof(info))))
      error = 998;
    goto done;
  }
  if (a->action == W32_F_FIRST || a->action == W32_F_NEXT) {
    struct w32_object *o = 0;
    struct find_state *state = 0;
    if (a->action == W32_F_FIRST) {
      o = w32_object_new(W32_OBJ_FIND);
      if (o) {
        state = w32_alloc(sizeof(*state));
        o->u.section = state;
      }
    } else {
      o = w32_lookup(t, a->handle, W32_OBJ_FIND);
      if (o) {
        w32_retain(o);
        state = o->u.section;
      }
    }
    if (!o || !state) {
      error = o ? 8 : 6;
      w32_release(o);
      goto done;
    }
    struct vfs_table *table = temporary_table(s->vfs);
    struct fat_entry *entry = w32_alloc(sizeof(*entry));
    if (!table || !entry)
      error = 8;
    else {
      int e = a->action == W32_F_FIRST
                  ? vfs_find_first(table, full, 0x37, &state->search, entry)
                  : vfs_find_next(table, &state->search, entry);
      if (e)
        error = e == -FS_ENOENT ? (a->action == W32_F_FIRST ? 2 : 18)
                                : dos_error(px_error(e));
      else {
        struct ciuki_w32_file_info info;
        file_info(&info, entry, entry->size);
        if (a->bytes != sizeof(info) ||
            copy_to_user(a->buffer, &info, sizeof(info)))
          error = 998;
        else if (a->action == W32_F_FIRST)
          error = w32_handle_install(p, o, 0, false, &a->result);
      }
    }
    w32_free(table, sizeof(*table));
    w32_free(entry, sizeof(*entry));
    w32_release(o);
    goto done;
  }
  if (a->action == W32_F_CHDIR) {
    struct px_node *old = p->cwd;
    if (p->cwd_retain)
      p->cwd_retain(old);
    error = dos_error(px_chdir(p, path));
    if (!error && p->w32 && old->drive < 26) {
      struct px_node *previous = p->w32->drive_cwd[old->drive];
      unsigned current = ((struct px_node *)p->cwd)->drive;
      p->w32->drive_cwd[old->drive] = current == old->drive ? 0 : old;
      if (previous && p->cwd_release)
        p->cwd_release(previous);
      if (current == old->drive) {
        if (p->cwd_release)
          p->cwd_release(old);
      } else if (current < 26) {
        struct px_node *active = p->w32->drive_cwd[current];
        p->w32->drive_cwd[current] = 0;
        if (active && p->cwd_release)
          p->cwd_release(active);
      }
    } else if (p->cwd_release)
      p->cwd_release(old);
    goto done;
  }
  if (a->action == W32_F_READ || a->action == W32_F_WRITE ||
      a->action == W32_F_SEEK || a->action == W32_F_TRUNCATE ||
      a->action == W32_F_FLUSH ||
      ((a->action == W32_F_STAT || a->action == W32_F_TIMES) && !a->path)) {
    struct w32_object *o = w32_lookup(t, a->handle, W32_OBJ_FILE);
    if (!o) {
      error = 6;
      goto done;
    }
    w32_retain(o);
    struct file_description *d = o->u.file.description;
    if (a->action == W32_F_READ || a->action == W32_F_WRITE) {
      bool write = a->action == W32_F_WRITE;
      if (!(o->u.file.access & (write ? 0x40000000u : 0x80000000u))) {
        error = 5;
        goto release;
      }
      struct ua_pin pin;
      int e =
          a->bytes ? ua_pin(p->memory, &pin, a->buffer, a->bytes, !write) : 0;
      if (e) {
        error = dos_error(e);
        goto release;
      }
      uint32_t n = a->bytes < CIUKI_IO_MAX ? a->bytes : CIUKI_IO_MAX;
      void *buffer = n ? w32_alloc(n) : 0;
      if (n && !buffer) {
        error = 8;
        if (a->bytes)
          ua_unpin(p->memory, &pin);
        goto release;
      }
      uint32_t total = 0;
      /* Hold the description's VFS transaction across all relay chunks so a
       * shared synchronous file pointer cannot interleave mid-call. Pinning
       * the entire user span above validates it before the first mutation. */
      vfs_lock(s->vfs, vfs_drive_mask(d->node->drive));
      do {
        uint32_t chunk = a->bytes - total;
        if (chunk > n)
          chunk = n;
        if (write && chunk)
          e = copy_from_user(buffer, a->buffer + total, chunk);
        size_t copied = 0;
        if (!e)
          e = file_io_locked(d, buffer, chunk, write, !!(a->flags & 0x10000u),
                             (((uint64_t)a->high << 32) | a->low) + total,
                             &copied);
        if (!write && copied &&
            copy_to_user(a->buffer + total, buffer, (uint32_t)copied))
          e = -EFAULT;
        total += (uint32_t)copied;
        if (e || copied < chunk)
          break;
      } while (total < a->bytes);
      vfs_unlock(s->vfs);
      a->result = total;
      error = dos_error(e);
      w32_free(buffer, n);
      if (a->bytes)
        ua_unpin(p->memory, &pin);
    } else {
      vfs_lock(s->vfs, vfs_drive_mask(d->node->drive));
      int e = 0;
      if (a->action == W32_F_SEEK) {
        int64_t pos = 0;
        e = file_seek_locked(d, (int64_t)(((uint64_t)a->high << 32) | a->low),
                             a->flags, &pos);
        if (!e) {
          a->low = (uint32_t)pos;
          a->high = (uint32_t)((uint64_t)pos >> 32);
        }
      } else if (a->action == W32_F_TRUNCATE)
        e = file_truncate_locked(d, d->position);
      else if (a->action == W32_F_FLUSH)
        e = file_sync_locked(d);
      else if (a->action == W32_F_TIMES) {
        struct ciuki_w32_file_info f;
        if (!(o->u.file.access & 0x40000000u))
          e = -EACCES;
        else if (a->bytes != sizeof(f) ||
                 copy_from_user(&f, a->buffer, sizeof(f)))
          e = -EFAULT;
        else if (!d->node->volume)
          e = -EROFS;
        else {
          struct fat_times times = d->node->entry.times;
          if (a->flags & 1) {
            times.create_date = f.create_date;
            times.create_time = f.create_time;
            times.create_tenths = f.create_tenths;
          }
          if (a->flags & 2)
            times.access_date = f.access_date;
          if (a->flags & 4) {
            times.write_date = f.write_date;
            times.write_time = f.write_time;
          }
          e = px_error(
              VFS_IO(s->vfs, fat_set_metadata(d->node->volume, &d->node->entry,
                                              d->node->entry.attr, &times)));
          if (!e)
            px_changed_locked(d->node);
        }
      } else {
        struct ciuki_w32_file_info f;
        file_info(&f, &d->node->entry,
                  d->node->ro ? d->node->ro_size : d->node->entry.size);
        if (a->bytes != sizeof(f) || copy_to_user(a->buffer, &f, sizeof(f)))
          e = -EFAULT;
      }
      vfs_unlock(s->vfs);
      error = dos_error(e);
    }
  release:
    w32_release(o);
    goto done;
  }
  uint32_t drives = full[0] ? vfs_drive_mask((unsigned)(full[0] - 'A')) : 0;
  if (path2[0])
    drives |= vfs_drive_mask((unsigned)(full2[0] - 'A'));
  vfs_lock(s->vfs, drives);
  bool write = a->action == W32_F_OPEN
                   ? (a->access & 0x40000000u) || a->disposition != 3
                   : a->action == W32_F_DELETE || a->action == W32_F_MOVE ||
                         a->action == W32_F_MKDIR || a->action == W32_F_RMDIR ||
                         a->action == W32_F_ATTR || a->action == W32_F_TIMES;
  if (write && (protected_path(s, cwd, path) ||
                (a->path2 && protected_path(s, cwd, path2))))
    error = 5;
  if(write&&(driver_destination(full)||(a->path2&&driver_destination(full2)))){
    error=5;w32_refusal_queue(p,W32_REFUSE_DRIVER,a->path2?full2:full,5);
  }
  int e = 0;
  if (!error && a->action == W32_F_OPEN) {
    if (!a->path || a->disposition < 1 || a->disposition > 5 ||
        a->share & ~7u || a->access & ~0xc0000000u ||
        (a->disposition == 5 && !(a->access & 0x40000000u)))
      error = 87;
    else if (a->flags & 0x04000000u)
      error = 120; /* requires F1-84 delete ownership */
    else {
      uint32_t flags = (a->access & 0x40000000u)
                           ? (a->access & 0x80000000u) ? O_RDWR : O_WRONLY
                           : O_RDONLY;
      if (a->disposition == 1)
        flags |= O_CREAT | O_EXCL;
      if (a->disposition == 2)
        flags |= O_CREAT | O_TRUNC;
      if (a->disposition == 4)
        flags |= O_CREAT;
      if (a->disposition == 5)
        flags |= O_TRUNC;
      struct px_path r;
      bool exists = !px_resolve_locked(s, cwd, path, true, &r) && r.node;
      if (exists && (r.node->kind == PX_DIRECTORY)) {
        if (!(a->flags & 0x02000000u) || a->disposition != 3 ||
            a->access & 0x40000000u)
          error = 5;
        else
          flags |= O_DIRECTORY;
      }
      struct file_description *d = file_description_new(flags);
      if (!d)
        error = 8;
      else {
        d->deny = (~a->share) & 3u;
        if (exists && px_share_locked(r.node,
                                      (a->access & 0x80000000u ? 1 : 0) |
                                          (a->access & 0x40000000u ? 2 : 0),
                                      d->deny))
          error = 32;
        if (!error && exists && a->disposition == 2 &&
            (r.node->entry.attr & 6) &&
            ((a->flags & 6) != (r.node->entry.attr & 6)))
          error = 5;
        if (!error)
          e = px_open_locked(s, cwd, path, flags, d);
        if (!error && !e && !exists && d->node->volume && (a->flags & 39)) {
          e = px_error(
              VFS_IO(s->vfs, fat_set_metadata(d->node->volume, &d->node->entry,
                                              (uint8_t)(a->flags & 39), 0)));
          if (!e)
            px_changed_locked(d->node);
        }
        vfs_unlock(s->vfs);
        if (!error && !e) {
          struct w32_object *o = w32_object_new(W32_OBJ_FILE);
          if (!o)
            error = 8;
          else {
            o->u.file.description = d;
            d->object.retain(&d->object);
            o->u.file.access = a->access;
            o->u.file.share = a->share;
            o->signalled = true;
            error = w32_handle_install(p, o, a->access, !!(a->flags & 0x10000u),
                                       &a->result);
            w32_release(o);
            if (!error && (a->disposition == 2 || a->disposition == 4))
              a->error = exists ? 183 : 0;
          }
        }
        d->object.release(&d->object);
        error = error ? error : dos_error(e);
        goto done;
      }
    }
  } else if (!error) {
    struct px_path r;
    switch (a->action) {
    case W32_F_STAT:
      e = px_resolve_locked(s, cwd, path, false, &r);
      if (!e) {
        struct ciuki_w32_file_info f;
        file_info(&f, &r.node->entry,
                  r.node->ro ? r.node->ro_size : r.node->entry.size);
        if (a->bytes != sizeof(f) || copy_to_user(a->buffer, &f, sizeof(f)))
          e = -EFAULT;
      }
      break;
    case W32_F_DELETE:
      e = px_remove_locked(s, cwd, path, false);
      break;
    case W32_F_RMDIR:
      e = px_remove_locked(s, cwd, path, true);
      break;
    case W32_F_MKDIR:
      e = px_mkdir_locked(s, cwd, path, 0777);
      break;
    case W32_F_MOVE:
      e = px_resolve_locked(s, cwd, path2, true, &r);
      if (!e) {
        if (r.node)
          error = 183;
        else
          e = px_rename_locked(s, cwd, path, path2);
      }
      break;
    case W32_F_ATTR:
      e = px_resolve_locked(s, cwd, path, false, &r);
      if (!e) {
        if (!r.node->volume)
          e = -EROFS;
        else
          e = px_error(VFS_IO(
              s->vfs, fat_set_metadata(
                          r.node->volume, &r.node->entry,
                          (r.node->entry.attr & 16) | (a->flags & 39), 0)));
        if (!e)
          px_changed_locked(r.node);
      }
      break;
    default:
      e = -ENOSYS;
      break;
    }
  }
  vfs_unlock(s->vfs);
  error = error ? error : dos_error(e);
done:
  if (error)
    a->error = error;
  w32_free(names, FS_PATH_BYTES * 3 + CIUKI_PATH_MAX * 2);
  return 0;
}
