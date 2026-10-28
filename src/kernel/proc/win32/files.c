/* Drive-path file service. Native paths and descriptors retain their F2 ABI.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/files.h>
#include <ciuki/kernel.h>
#include <ciuki/w32_core.h>
#include <ciuki/w32_objects.h>
#include <ciuki/w32_fonts.h>
/* fs/spool_ring.c; absent from host fixtures that include this file. */
extern bool spool_publication(const char *) __attribute__((weak));
extern void spool_ring(struct vfs *) __attribute__((weak));
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
  case -VFS_ELOCK:
    return 33;
  case -VFS_ELOCKFULL:
    return 36;
  case -EBADF:
    return 6;
  case -ENXIO:
    return 21; /* fx-emptydrive: ERROR_NOT_READY, an empty CD-ROM/floppy drive */
  case -EINTR:
    return 995;
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
static bool windows_fonts(const struct px_node *n) {
  return n && n->drive==2 && n->parent && n->parent->parent &&
    n->parent->parent->mountpoint && path_equal(n->entry.name,"FONTS") &&
    path_equal(n->parent->entry.name,"WINDOWS");
}
static bool windows_root(const struct px_node *n) {
  return n && n->drive==2 && n->parent && n->parent->mountpoint &&
    path_equal(n->entry.name,"WINDOWS");
}
static bool optical_path(struct vfs *v,const char *full) {
  if(!full[0])return false;
  unsigned drive=(unsigned)(full[0]-'A');
  if(drive>=26 || !v->ro_volumes[drive])return false;
  const char *name=v->ro_volumes[drive]->ops->name;
  return path_equal(name,"CDFS") || path_equal(name,"ISO9660");
}
/* F5 W14: the builtin store C:\System\Win32 and the personality's system
 * fonts stay read-only to Win32 processes. K-Q4 (sec-k03): on a managed
 * system volume the common prefix policy (px_policy_check_locked) replaces
 * the former Win32-only ban on all of C:\System; an unmanaged (not yet
 * provisioned) volume keeps that ban until its policy is bound. */
static bool system_managed(const struct px_node *n) {
  return n && n->drive < 26 && n->space->vfs->policy[n->drive];
}
static bool system_node(const struct px_node *n) {
  if(n && windows_fonts(n->parent) && w32_system_font_name(n->entry.name))return true;
  bool managed = system_managed(n);
  const struct px_node *child = 0;
  while (n && n->parent && n != n->parent) {
    if (n->drive == 2 && n->parent->mountpoint &&
        path_equal(n->entry.name, "System"))
      return !managed || (child && path_equal(child->entry.name, "Win32"));
    child = n;
    n = n->parent;
  }
  return false;
}
static bool lexical_prefix(const char *path, const char *prefix) {
  unsigned n = (unsigned)strlen(prefix);
  if (strlen(path) < n) return false;
  for (unsigned i = 0; i < n; i++)
    if (path_upper((uint8_t)path[i]) != (uint8_t)prefix[i]) return false;
  return !path[n] || path[n] == '/';
}
static bool protected_path(struct px_namespace *s, struct px_node *cwd,
                           const char *path, const struct security_subject *subject) {
  struct px_path r;
  /* sec-k03: classified through the caller's resolver; an object the caller
   * cannot reach is never read on its behalf (the open itself denies). */
  int e = px_resolve_as_locked(s, cwd, path, true, subject, &r);
  if (!e && system_node(r.node ? r.node : r.parent))
    return true;
  if(!e && !r.node && windows_fonts(r.parent) && w32_system_font_name(r.name))return true;
  bool managed = s->root && system_managed(s->root);
  if (!e && !r.node && managed && r.parent && r.parent->drive == 2 && r.parent->parent &&
      r.parent->parent->mountpoint && path_equal(r.parent->entry.name, "System") &&
      path_equal(r.name, "Win32"))
    return true;
  /* Guard missing children as well as aliases, including a missing System. */
  if (path[0] != '/')
    return false;
  return managed ? lexical_prefix(path, "/SYSTEM/WIN32") : lexical_prefix(path, "/SYSTEM");
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
/* sec-k03: the find table is attributed to the calling process's retained
 * subject, borrowed for this one synchronous call (the process outlives it). */
static struct vfs_table *temporary_table(struct vfs *v, struct process *p) {
  struct vfs_table *table = w32_alloc(sizeof(*table));
  if (table) {
    table->vfs = v;
    const struct security_subject *subject = px_process_subject(p);
    table->subject = subject == PX_KERNEL ? 0 : (struct security_subject *)subject;
    table->drive = 2;
    for (unsigned i = 0; i < 26; i++)
      memcpy(table->cwd[i], "/", 2);
  }
  return table;
}
void w32_file_handle_close(struct process *p,struct w32_object *o) {
  struct file_description *d=o->u.file.description;
  if (!d || !d->node) return;
  struct vfs *v=d->node->space->vfs;
  vfs_lock(v,0);
  for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) {
    struct vfs_range_lock *l=&v->range_locks[i];
    if (l->description==&d->lock_description && l->owner_type==VFS_OWNER_WIN32 && l->owner==p->pid)
      memset(l,0,sizeof(*l));
  }
  if (d->delete_on_close && !system_node(d->node)) d->node->delete_pending=true;
  fs_event_wake(&v->volume_changed);
  vfs_unlock(v);
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
      int e = px_resolve_as_locked(s, cwd, path, false, px_process_subject(p), &r);
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
    /* fx-emptydrive: GetLogicalDrives lists every drive letter, also a
     * CD-ROM or floppy drive without a medium (Windows 98 reserves one
     * letter per removable-media drive at startup). */
    for (unsigned i = 0; i < 26; i++)
      if (v->volumes[i] || v->ro_volumes[i] || vfs_drive_slot(v, i))
        info.drives |= 1u << i;
    if (d >= 26 || !(info.drives & (1u << d)))
      error = 15;
    else if (!v->volumes[d] && !v->ro_volumes[d]) {
      /* Empty drive: GetDriveType still answers from the copied record;
       * volume and space queries fail with ERROR_NOT_READY. */
      info.type = vfs_drive_slot(v, d) == VFS_SLOT_OPTICAL ? 5 : 2;
      info.max_component = 0;
      error = 21;
    } else {
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
        free = (uint64_t)fat_free_estimate(vol, 0) * info.cluster_bytes; /* fx-17b */
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
    if ((!error || error == 21) && (a->bytes != sizeof(info) ||
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
    struct vfs_table *table = temporary_table(s->vfs, p);
    struct fat_entry *entry = w32_alloc(sizeof(*entry));
    if (!table || !entry)
      error = 8;
    else if (!px_process_subject(p)) /* sec-k03: no unattributed find table */
      error = 5;
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
      a->action == W32_F_FLUSH || a->action == W32_F_LOCK || a->action == W32_F_UNLOCK ||
      ((a->action == W32_F_STAT || a->action == W32_F_TIMES) && !a->path)) {
    struct w32_object *o = w32_lookup(t, a->handle, W32_OBJ_FILE);
    if (!o) {
      error = 6;
      goto done;
    }
    w32_retain(o);
    struct file_description *d = o->u.file.description;
    if (a->action == W32_F_LOCK || a->action == W32_F_UNLOCK) {
      if (!(o->u.file.access & 0xc0000000u) || a->flags & ~3u) error=87;
      else {
        struct vfs *v=d->node->space->vfs;
        struct vfs_lock_owner owner={VFS_OWNER_WIN32,p->pid};
        uint64_t offset=((uint64_t)a->high<<32)|a->low;
        uint64_t length=((uint64_t)a->share<<32)|a->bytes;
        int e;
        do {
          vfs_lock(v,0);
          d->lock_node=(struct vfs_node){.volume=d->node->volume,.entry=d->node->entry,
            .generation=d->node->generation,.drive=d->node->drive,.identity=d->identity,
            .ro=d->node->ro,.ro_id=d->node->ro_id};
          d->lock_description.node=&d->lock_node;
          e=vfs_description_lock_locked(v,&d->lock_description,owner,offset,length,
              !!(a->flags&2),a->action==W32_F_UNLOCK);
          vfs_unlock(v);
          if (e!=-VFS_ELOCK || a->action==W32_F_UNLOCK || (a->flags&1)) break;
          if (p->state!=PROC_LIVE || (t->w32 && t->w32->terminate_requested)) { e=-EINTR; break; }
          task_sleep_ms(1);
        } while (true);
        error=dos_error(e);
      }
    } else if (a->action == W32_F_READ || a->action == W32_F_WRITE) {
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
      if (write && system_node(d->node)) e=-EACCES;
      do {
        uint32_t chunk = a->bytes - total;
        if (chunk > n)
          chunk = n;
        if (!e && write && chunk)
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
        e = system_node(d->node) ? -EACCES : file_truncate_locked(d, d->position);
      else if (a->action == W32_F_FLUSH)
        e = file_sync_locked(d);
      else if (a->action == W32_F_TIMES) {
        struct ciuki_w32_file_info f;
        if (!(o->u.file.access & 0x40000000u) || system_node(d->node))
          e = -EACCES;
        else if ((e = file_description_authorize_locked(d, POLICY_META))) { } /* sec-k03 */
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
          e = px_error(px_fat_transaction_locked(
              d->node, d->node->parent, 0,
              (struct fat_transaction_args){.op = FAT_TX_SET_METADATA,
                                            .entry = &d->node->entry,
                                            .attr = d->node->entry.attr,
                                            .times = &times}));
          if (!e)
            px_changed_locked(d->node);
        }
      } else {
        struct ciuki_w32_file_info f;
        file_info(&f, &d->node->entry,
                  d->node->ro ? d->node->ro_size : d->node->entry.size);
        /* Snapshot validity, mount/media generation and owning location under
         * the identity lock. Expired and non-FAT identities stay zero. */
        uint32_t volume_serial;
        f.file_id = vfs_identity_file_id(s->vfs, d->identity, &volume_serial);
        f.volume_serial = volume_serial;
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
                   ? (a->access & 0x40010000u) || (a->flags & 0x04000000u) || a->disposition != 3
                   : a->action == W32_F_DELETE || a->action == W32_F_MOVE ||
                         a->action == W32_F_MKDIR || a->action == W32_F_RMDIR ||
                         a->action == W32_F_ATTR || a->action == W32_F_TIMES;
  if (write && (protected_path(s, cwd, path, px_process_subject(p)) ||
                (a->path2 && protected_path(s, cwd, path2, px_process_subject(p)))))
    error = 5;
  if(write && (optical_path(s->vfs,full) ||
      (a->path2 && optical_path(s->vfs,full2))))error=19;
  /* A parent rename must not move the pinned files outside their protected
   * namespace. New installed faces remain ordinary writable files. */
  if(write && (a->action==W32_F_MOVE || a->action==W32_F_RMDIR)) {
    struct px_path r;
    if(!px_resolve_as_locked(s,cwd,path,false,px_process_subject(p),&r) &&
       (windows_fonts(r.node) || windows_root(r.node)))error=5;
  }
  if(write&&(driver_destination(full)||(a->path2&&driver_destination(full2)))){
    error=5;w32_refusal_queue(p,W32_REFUSE_DRIVER,a->path2?full2:full,5);
  }
  int e = 0;
  if (!error && a->action == W32_F_OPEN) {
    if (!a->path || a->disposition < 1 || a->disposition > 5 ||
        a->share & ~7u || a->access & ~0xc0010000u ||
        (a->disposition == 5 && !(a->access & 0x40000000u)))
      error = 87;
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
      bool exists = !px_resolve_as_locked(s, cwd, path, true, px_process_subject(p), &r) && r.node;
      if (exists && (r.node->kind == PX_DIRECTORY)) {
        if (!(a->flags & 0x02000000u) || a->disposition != 3 ||
            a->access & 0x40000000u || (a->flags&0x04000000u))
          error = 5;
        else
          flags |= O_DIRECTORY;
      }
      struct file_description *d = file_description_new(flags);
      if (!d)
        error = 8;
      else {
        d->win32=true;

        d->lock_owner=(struct vfs_lock_owner){VFS_OWNER_WIN32,p->pid};
        d->share_access=(a->access&0x80000000u ? 1u : 0) |
            (a->access&0x40000000u ? 2u : 0) | ((a->access&0x10000u) || (a->flags&0x04000000u) ? 4u : 0);
        d->deny = d->share_access ? (~a->share) & 7u : 0;
        if (exists && px_share_locked(r.node,
                                      d->share_access,
                                      d->deny))
          error = 32;
        if (!error && exists && (r.node->delete_pending ||
            (d->share_access&4 && ((r.node->entry.attr&1) || !r.node->volume || r.node->volume->readonly)))) error=5;
        if (!error && exists && a->disposition == 2 &&
            (r.node->entry.attr & 6) &&
            ((a->flags & 6) != (r.node->entry.attr & 6)))
          error = 5;
        if (!error)
          e = px_open_as_locked(s, cwd, path, flags, d, px_process_subject(p));
        if (!error && !e && d->node->kind==PX_FILE && d->node->volume)
          e=px_error(vfs_identity_get(s->vfs,d->node->volume,&d->node->entry,&d->identity));
        if (!error && !e && !exists && d->node->volume && (a->flags & 39)) {
          e = px_error(px_fat_transaction_locked(
              d->node, d->node->parent, 0,
              (struct fat_transaction_args){.op = FAT_TX_SET_METADATA,
                                            .entry = &d->node->entry,
                                            .attr = (uint8_t)(a->flags & 39)}));
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
            if (!error) d->delete_on_close=!!(a->flags&0x04000000u);
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
      e = px_resolve_as_locked(s, cwd, path, false, px_process_subject(p), &r);
      if (!e) {
        struct ciuki_w32_file_info f;
        file_info(&f, &r.node->entry,
                  r.node->ro ? r.node->ro_size : r.node->entry.size);
        if (a->bytes != sizeof(f) || copy_to_user(a->buffer, &f, sizeof(f)))
          e = -EFAULT;
      }
      break;
    case W32_F_DELETE: {
      /* f1-89 Amendment 1 item 2: the writer turn precedes the resolution,
       * so no other mutation can retire the resolved entry meanwhile. */
      uint32_t turn = 0;
      e = px_turn_take_path_locked(s, cwd, path, &turn);
      if (!e) e=px_resolve_as_locked(s,cwd,path,false,px_process_subject(p),&r);
      if (!e && r.node->kind!=PX_FILE) error=5;
      if (!e && !error && px_share_locked(r.node,4,0)) error=32;
      if (!e && !error && ((r.node->entry.attr&1) || r.node->delete_pending)) error=5;
      if (!e && !error) {
        /* sec-k03: REMOVE intent authorized now; deferred to the last
         * close of this identity only, never a later path lookup. */
        e=px_delete_as_locked(r.node,px_process_subject(p));
        if (e==-EBUSY) { e=0; error=32; }
      }
      vfs_turn_drop_locked(s->vfs, turn);
      break;
    }
    case W32_F_RMDIR:
      e = px_remove_as_locked(s, cwd, path, true, px_process_subject(p));
      break;
    case W32_F_MKDIR:
      e = px_mkdir_as_locked(s, cwd, path, 0777, px_process_subject(p));
      break;
    case W32_F_MOVE:
      e = px_resolve_as_locked(s, cwd, path2, true, px_process_subject(p), &r);
      if (!e) {
        if (r.node)
          error = 183;
        else {
          struct px_path from;
          e=px_resolve_as_locked(s,cwd,path,false,px_process_subject(p),&from);
          if (!e && px_share_locked(from.node,4,0)) error=32;
          if (!e && !error) e = px_rename_as_locked(s, cwd, path, path2, px_process_subject(p));
          if (e==-EBUSY) { e=0; error=32; }
        }
      }
      break;
    case W32_F_ATTR: {
      /* f1-89 Amendment 1 item 2: take the writer turn before resolving,
       * as px_mkdir/px_remove do; a delete can no longer retire the slot
       * between the resolution and the attribute transaction. */
      uint32_t turn = 0;
      e = px_turn_take_path_locked(s, cwd, path, &turn);
      if (!e) e = px_resolve_as_locked(s, cwd, path, false, px_process_subject(p), &r);
      if (!e) {
        if (!r.node->volume)
          e = -EROFS;
        else if ((e = px_policy_check_locked(r.node, px_process_subject(p), POLICY_META))) { } /* sec-k03 */
        else
          e = px_error(px_fat_transaction_locked(
              r.node, r.node->parent, 0,
              (struct fat_transaction_args){
                  .op = FAT_TX_SET_METADATA, .entry = &r.node->entry,
                  .attr = (uint8_t)((r.node->entry.attr & 16) | (a->flags & 39))}));
        if (!e)
          px_changed_locked(r.node);
      }
      vfs_turn_drop_locked(s->vfs, turn);
      break;
    }
    default:
      e = -ENOSYS;
      break;
    }
  }
  vfs_unlock(s->vfs);
  error = error ? error : dos_error(e);
  /* A published spool record (job, control, printer configuration) rings
   * the print worker, which sleeps until then (fs/spool_ring.c). */
  if (!error && a->action == W32_F_MOVE && spool_publication && spool_ring &&
      spool_publication(full2))
    spool_ring(s->vfs);
done:
  if (error)
    a->error = error;
  w32_free(names, FS_PATH_BYTES * 3 + CIUKI_PATH_MAX * 2);
  return 0;
}
