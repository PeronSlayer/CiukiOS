/* CreateProcess image search and private preparation/publication transaction.
 * The command line is copied as one string, never parsed by the kernel.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/files.h>
#include <ciuki/kernel.h>
#include <ciuki/w32_core.h>
#include <ciuki/w32_objects.h>
extern uint32_t w32_native_path(struct process *, const char *, char *, char *);
static int process_string(uint32_t va, char *out, unsigned cap) {
  if (!va)
    return -EINVAL;
  for (unsigned i = 0; i < cap; i++) {
    if (va > UINT32_MAX - i || copy_from_user(out + i, va + i, 1))
      return -EFAULT;
    if (!out[i])
      return 0;
  }
  return -ENAMETOOLONG;
}
static int joined(struct process *p, const char *dir, unsigned n,
                  const char *name, char *resolved, struct ciuki_file *file) {
  if (n + strlen(name) + 2 >= 260)
    return -ENAMETOOLONG;
  memcpy(resolved, dir, n);
  if (n && resolved[n - 1] != '/' && resolved[n - 1] != '\\')
    resolved[n++] = '\\';
  memcpy(resolved + n, name, strlen(name) + 1);
  return w32_loader_open(p, resolved, file);
}
static int executable(struct process *p, const char *image, const char *env,
                      char *resolved, struct ciuki_file *file) {
  char name[260];
  unsigned n = (unsigned)strlen(image);
  if (!n || n >= sizeof(name))
    return -ENOENT;
  memcpy(name, image, n + 1);
  bool explicit = false, ext = false;
  for (unsigned i = 0; i < n; i++) {
    if (name[i] == '/' || name[i] == '\\' || name[i] == ':') {
      explicit = true;
      ext = false;
    }
    if (name[i] == '.')
      ext = true;
  }
  if (!ext) {
    if (n + 4 >= sizeof(name))
      return -ENAMETOOLONG;
    memcpy(name + n, ".exe", 5);
  }
  if (explicit) {
    memcpy(resolved, name, strlen(name) + 1);
    return w32_loader_open(p, name, file);
  }
  const char *exe = p->w32_source_name;
  unsigned last = 0;
  for (unsigned i = 0; exe[i]; i++)
    if (exe[i] == '/' || exe[i] == '\\')
      last = i + 1;
  int e = -ENOENT;
  if (last)
    e = joined(p, exe, last, name, resolved, file);
  if (!e)
    return 0;
  if ((e = joined(p, "", 0, name, resolved, file)) != -ENOENT)
    return e;
  const char *dirs[] = {"C:\\WINDOWS\\SYSTEM", "C:\\WINDOWS"};
  for (unsigned i = 0; i < 2; i++) {
    e = joined(p, dirs[i], (unsigned)strlen(dirs[i]), name, resolved, file);
    if (e != -ENOENT)
      return e;
  }
  for (const char *v = env; *v; v += strlen(v) + 1)
    if ((v[0] == 'P' || v[0] == 'p') && (v[1] == 'A' || v[1] == 'a') &&
        (v[2] == 'T' || v[2] == 't') && (v[3] == 'H' || v[3] == 'h') &&
        v[4] == '=') {
      const char *q = v + 5;
      while (*q) {
        const char *start = q;
        while (*q && *q != ';')
          q++;
        e = joined(p, start, (unsigned)(q - start), name, resolved, file);
        if (e != -ENOENT)
          return e;
        if (*q)
          q++;
      }
      break;
    }
  return e;
}
uint32_t w32_process_call(struct proc_thread *t,
                          struct ciuki_w32_spawn_args *a) {
  uint32_t priority = a->flags & (0x20u | 0x40u | 0x80u | 0x100u);
  if ((a->flags & ~(4u | 0x10u | 0x20u | 0x40u | 0x80u | 0x100u)) ||
      (priority && (priority & (priority - 1)))) {
    a->error = 87;
    return 0;
  }
  if (priority == 0x100) {
    a->error = 5; /* same qualified F0-45 policy as SetPriorityClass */
    return 0;
  }
  if (!priority)
    priority = t->process->w32 && t->process->w32->priority_class == 0x40
                   ? 0x40
                   : 0x20;
  char *buffer = w32_alloc(32768 * 2 + FS_PATH_BYTES * 2 + CIUKI_PATH_MAX);
  if (!buffer) {
    a->error = 8;
    return 0;
  }
  char *command = buffer, *env = buffer + 32768, *image = env + 32768,
       *resolved = image + FS_PATH_BYTES, *directory = resolved + FS_PATH_BYTES;
  struct ciuki_w32_startup startup = {0};
  char title[260] = {0};
  int startup_error =
      a->startup ? copy_from_user(&startup, a->startup, sizeof(startup)) : 0;
  if (!startup_error && startup.title)
    startup_error = process_string(startup.title, title, sizeof(title));
  struct process *child = 0;
  struct proc_strings *strings = 0;
  struct ciuki_file file = {0};
  uint32_t ph = 0, th = 0;
  int err =
      startup_error ? startup_error : process_string(a->image, image, 260);
  if (!err)
    err = process_string(a->command, command, 32768);
  if (!err)
    err = copy_from_user(env, a->environment, a->environment_bytes);
  if (!err && (env[a->environment_bytes - 1] || env[a->environment_bytes - 2]))
    err = -EINVAL;
  if (!err)
    err = executable(t->process, image, env, resolved, &file);
  if (!err)
    err = proc_prepare(t->process, &child);
  if (!err)
    err = proc_inherit(child, t->process, 0, 0);
  if (!err && a->directory) {
    char *full = image;
    err = process_string(a->directory, image, FS_PATH_BYTES);
    if (!err) {
      uint32_t error = w32_native_path(t->process, image, directory, full);
      if (error) {
        a->error = error;
        err = -EINVAL;
      }
    }
    if (!err)
      err = px_chdir(child, directory);
  }
  if (!err) {
    strings = proc_strings_new();
    if (!strings)
      err = -ENOMEM;
  }
  if (!err)
    err = proc_strings_add(strings, resolved, (uint32_t)strlen(resolved) + 1,
                           false);
  bool pe = !err && w32_is_pe(&file);
  /* Native user-return does not yet have the Win32 suspension hook. */
  if (!err && !pe && (a->flags & 4)) {
    a->error = 120;
    err = -EINVAL;
  }
  if (!err && pe)
    err = proc_strings_add(strings, command, (uint32_t)strlen(command) + 1,
                           false);
  if (!err)
    for (unsigned at = 0; at < a->environment_bytes && env[at];) {
      unsigned n = (unsigned)strlen(env + at) + 1;
      err = proc_strings_add(strings, env + at, n, true);
      if (err)
        break;
      at += n;
    }
  if (!err) {
    proc_set_name(child, resolved);
    if (pe)
      err = w32_spawn(child, &file, strings, t->mask);
    else {
      struct elf_image im;
      err = elf_validate(&file, &im);
      if (!err)
        err = elf_load(&file, &im, child->memory);
      struct proc_thread *main = 0;
      if (!err)
        err = proc_thread_prepare(child, im.entry, 0, 0,
                                  CIUKI_THREAD_STACK_DEFAULT, 0, t->mask, true,
                                  &main);
      uint32_t esp = 0;
      if (!err)
        err =
            proc_stack_build(child->memory, strings, im.entry, main->tls, &esp);
      if (!err)
        task_native_frame(main->task, esp);
    }
  }
  if (!err && a->inherit && pe) {
    a->error = w32_handles_inherit(child, t->process);
    if (a->error)
      err = -ENOMEM;
  }
  if (!err && pe && a->startup) {
    struct ciuki_w32_parameters_v3 v = {.startup = startup};
    uint32_t prior = 0, address = 0;
    err = ua_read(child->memory, &prior, child->w32_peb + 0x10, 4);
    if (!err)
      err = ua_read(child->memory, &v.inherited, prior, sizeof(v.inherited));
    if (!err)
      err =
          w32_map_system(child->memory, sizeof(v) + sizeof(title), 0, &address);
    v.inherited.core.version = 3;
    v.inherited.core.size = sizeof(v);
    v.startup.title = title[0] ? address + sizeof(v) : 0;
    if (!err)
      err = ua_write(child->memory, address, &v, sizeof(v));
    if (!err && title[0])
      err = ua_write(child->memory, address + sizeof(v), title,
                     (uint32_t)strlen(title) + 1);
    if (!err)
      err = ua_write(child->memory, child->w32_peb + 0x10, &address, 4);
  }
  struct proc_thread *main = 0;
  for (unsigned i = 0; !err && i < CIUKI_THREAD_MAX; i++) {
    struct proc_thread *v = proc_thread_slot(i);
    if (v && v->process == child) {
      main = v;
      break;
    }
  }
  if (!err && (!main || t->process->state != PROC_LIVE))
    err = -EINTR;
  if (!err && w32_process_init(child))
    err = -ENOMEM;
  if (!err) {
    a->error = w32_handle_install(t->process, child->w32->identity, ~0u,
                                  !!(a->handle_flags & 1), &ph);
    if (a->error)
      err = -ENOMEM;
  }
  if (!err && !pe && w32_thread_objects_init(main))
    err = -ENOMEM;
  if (!err) {
    a->error = w32_handle_install(t->process, main->w32->identity, ~0u,
                                  !!(a->handle_flags & 2), &th);
    if (a->error)
      err = -ENOMEM;
  }
  if (!err) {
    child->w32->priority_class = priority;
    task_set_base(main->task, priority == 0x40 ? P_IDLE : P_NORMAL,
                  (unsigned)proc_priority_win32(priority, 0, false));
    if (pe && (a->flags & 4))
      main->w32->suspend = 1;
    proc_publish(child, 0);
    a->pid = child->pid;
    a->tid = main->tid;
    a->process_handle = ph;
    a->thread_handle = th;
    child = 0;
  }
  if (err) {
    if (th)
      w32_handle_close(t->process, th);
    if (ph)
      w32_handle_close(t->process, ph);
    if (!a->error)
      a->error = err == -ENOENT                                     ? 2
                 : err == -ENOTDIR                                  ? 3
                 : err == -EACCES || err == -EPERM || err == -EROFS ? 5
                 : err == -ENOMEM                                   ? 8
                 : err == -ENOEXEC                                  ? 193
                 : err == -EFAULT                                   ? 998
                 : err == -EINTR                                    ? 995
                 : err == -EIO                                      ? 1117
                 : err == -ENAMETOOLONG                             ? 206
                                                                    : 87;
  }
  if (child)
    proc_discard(child);
  if (file.close)
    file.close(file.cookie);
  proc_strings_free(strings);
  w32_free(buffer, 32768 * 2 + FS_PATH_BYTES * 2 + CIUKI_PATH_MAX);
  return 0;
}
