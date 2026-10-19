/* Immutable, handle-owned Toolhelp snapshots of copied records.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/cpu.h>
#include <ciuki/kernel.h>
#include <ciuki/w32_core.h>
#include <ciuki/w32_objects.h>
#define SNAP_MAX (CIUKI_PROCESS_MAX + CIUKI_THREAD_MAX + 128u)
struct snap {
  uint32_t count, cursor[3];
  struct ciuki_w32_snapshot_entry entries[SNAP_MAX];
};
void w32_snapshot_destroy(struct w32_object *o) {
  w32_free(o->u.section, sizeof(struct snap));
}
static bool module_name(struct process *p, uint32_t at, char *out,
                        unsigned cap) {
  uint8_t u[8];
  if (ua_read(p->memory, u, at, 8))
    return false;
  uint16_t length;
  uint32_t address;
  memcpy(&length, u, 2);
  memcpy(&address, u + 4, 4);
  if (length & 1 || length / 2 >= cap)
    return false;
  for (unsigned i = 0; i < length / 2; i++) {
    uint16_t c;
    if (ua_read(p->memory, &c, address + i * 2, 2))
      return false;
    if (c > 255)
      return false;
    out[i] = (char)c;
  }
  out[length / 2] = 0;
  return true;
}
static uint32_t modules(struct process *p, struct snap *s) {
  if (!p->w32_peb)
    return 0;
  uint32_t ldr, head, next;
  if (ua_read(p->memory, &ldr, p->w32_peb + 0x0c, 4) || !ldr)
    return 998;
  head = ldr + 0x0c;
  if (head < ldr || ua_read(p->memory, &next, head, 4))
    return 998;
  unsigned count = 0;
  while (next != head) {
    if (++count > 128 || s->count == SNAP_MAX)
      return 24;
    uint32_t row[13];
    if (ua_read(p->memory, row, next, sizeof(row)))
      return 998;
    struct ciuki_w32_snapshot_entry *e = &s->entries[s->count++];
    e->kind = 2;
    e->id = count;
    e->owner = p->pid;
    e->base = row[6];
    e->bytes = row[8];
    if (!module_name(p, next + 0x24, e->path, sizeof(e->path)) ||
        !module_name(p, next + 0x2c, e->name, sizeof(e->name)))
      return 1113;
    next = row[0];
  }
  return 0;
}
uint32_t w32_snapshot_call(struct proc_thread *t,
                           struct ciuki_w32_snapshot_args *a) {
  if (a->action == 1) {
    if (a->flags & ~0x8000001fu || a->flags & 1u) {
      a->error = 120;
      return 0;
    }
    struct w32_object *o = w32_object_new(W32_OBJ_SNAPSHOT);
    if (!o) {
      a->error = 8;
      return 0;
    }
    struct snap *s = w32_alloc(sizeof(*s));
    o->u.section = s;
    if (!s) {
      w32_release(o);
      a->error = 8;
      return 0;
    }
    uint32_t irq = irq_save();
    if (a->flags & 2)
      for (unsigned i = 0; i < CIUKI_PROCESS_MAX; i++) {
        struct process *p = proc_at_slot(i);
        if (!p || !p->published)
          continue;
        struct ciuki_w32_snapshot_entry *e = &s->entries[s->count++];
        e->kind = 0;
        e->id = p->pid;
        e->parent = p->ppid;
        e->threads = p->live_threads;
        e->priority = 8;
        memcpy(e->name, p->name, sizeof(p->name));
      }
    if (a->flags & 4)
      for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *v = proc_thread_slot(i);
        if (!v || !v->process->published || v->stopped)
          continue;
        struct ciuki_w32_snapshot_entry *e = &s->entries[s->count++];
        e->kind = 1;
        e->id = v->tid;
        e->owner = v->process->pid;
        e->priority = 8;
      }
    irq_restore(irq);
    if ((a->flags & (8 | 16)) && (!a->pid || a->pid == t->process->pid))
      a->error = modules(t->process, s);
    if (!a->error)
      a->error = w32_handle_install(t->process, o, 0,
                                    !!(a->flags & 0x80000000u), &a->result);
    w32_release(o);
    return 0;
  }
  struct w32_object *o = w32_lookup(t, a->handle, W32_OBJ_SNAPSHOT);
  if (!o) {
    a->error = 6;
    return 0;
  }
  w32_retain(o);
  struct snap *s = o->u.section;
  if (a->flags > 2 || a->bytes != sizeof(struct ciuki_w32_snapshot_entry))
    a->error = 24;
  else {
    uint32_t i = a->action == 2 ? 0 : s->cursor[a->flags];
    while (i < s->count && s->entries[i].kind != a->flags)
      i++;
    if (i == s->count)
      a->error = 18;
    else if (copy_to_user(a->buffer, &s->entries[i], sizeof(s->entries[i])))
      a->error = 998;
    else {
      s->cursor[a->flags] = i + 1;
      a->result = 1;
    }
  }
  w32_release(o);
  return 0;
}
