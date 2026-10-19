/* SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/w32_core.h>
#include <ciuki/w32_objects.h>
static void priority_apply(struct proc_thread *t) {
  if (!t->w32 || !t->task)
    return;
  uint32_t cls = t->process->w32->priority_class;
  int level = proc_priority_win32(cls, t->w32->relative_priority, false);
  if (level < 0)
    return;
  enum task_prio kind = cls == 0x40 || t->w32->relative_priority == -15 ? P_IDLE
                        : t->w32->relative_priority == 15 ? P_INTERACTIVE
                                                          : P_NORMAL;
  task_set_base(t->task, kind, (unsigned)level);
}
uint32_t w32_system_call(struct proc_thread *t,
                         struct ciuki_w32_system_args *a) {
  if (a->action == W32_S_TIME) {
    uint64_t n = w32_system_time();
    a->low = (uint32_t)n;
    a->high = (uint32_t)(n >> 32);
  } else if (a->action == W32_S_MEMORY) {
    a->low = pmm_total_usable();
    a->high = pmm_free_count();
    uint32_t reserved = 0;
    for (const struct ua_extent *e = t->process->memory->head; e; e = e->next)
      if (e->base < CIUKI_W32_LIMIT)
        reserved += (e->end - e->base) / PAGE_SIZE;
    a->extra = reserved;
  } else if (a->action == W32_S_YIELD)
    task_yield();
  else if (a->action == W32_S_THREAD_EXIT)
    proc_thread_exit(a->value);
  else if (a->action == W32_S_TERMINATE_PROCESS) {
    struct w32_object *o = w32_lookup(t, a->handle, W32_OBJ_PROCESS);
    struct process *p = o ? proc_find(o->id) : 0;
    if (!p || o->signalled)
      a->error = 6;
    else
      proc_stop(p, (int)a->value, 0);
  } else if (a->action == W32_S_TERMINATE_THREAD) {
    struct w32_object *o = w32_lookup(t, a->handle, W32_OBJ_THREAD);
    struct proc_thread *target = 0;
    for (unsigned i = 0; (o || a->extra) && i < CIUKI_THREAD_MAX; i++) {
      struct proc_thread *candidate = proc_thread_slot(i);
      if (candidate && candidate->w32 &&
          (o ? candidate->w32->identity == o
             : candidate->process == t->process && candidate->tid == a->extra)) {
        target = candidate;
        break;
      }
    }
    if (!target || (o && o->signalled))
      a->error = 6;
    else if (target->process->personality != PROC_WIN32)
      a->error = 120;
    else {
      target->w32->terminate_code = a->value;
      target->w32->terminate_requested = true;
      target->w32->suspend = 0;
      proc_interrupt(target);
      w32_changed_wake();
    }
  } else if (a->action == W32_S_THREAD_PRIORITY ||
             (a->action == W32_S_QUERY_PRIORITY && a->value == 1)) {
    struct w32_object *o = w32_lookup(t, a->handle, W32_OBJ_THREAD);
    struct proc_thread *target = 0;
    for (unsigned i = 0; o && i < CIUKI_THREAD_MAX; i++) {
      struct proc_thread *v = proc_thread_slot(i);
      if (v && v->w32 && v->w32->identity == o) {
        target = v;
        break;
      }
    }
    if (!target)
      a->error = 6;
    else if (a->action == W32_S_QUERY_PRIORITY)
      a->result = (uint32_t)target->w32->relative_priority;
    else if (proc_priority_win32(target->process->w32->priority_class,
                                 (int32_t)a->value, false) < 0)
      a->error = 87;
    else {
      target->w32->relative_priority = (int32_t)a->value;
      priority_apply(target);
    }
  } else if (a->action == W32_S_PROCESS_PRIORITY ||
             a->action == W32_S_QUERY_PRIORITY) {
    struct w32_object *o = w32_lookup(t, a->handle, W32_OBJ_PROCESS);
    struct process *p = o ? proc_find(o->id) : 0;
    if (!p || !p->w32 || o->signalled)
      a->error = 6;
    else if (a->action == W32_S_QUERY_PRIORITY)
      a->result = p->w32->priority_class;
    else if (a->value == 0x100)
      a->error = 5; /* preserve the qualified F0-45 realtime policy */
    else if (proc_priority_win32(a->value, 0, false) < 0)
      a->error = 87;
    else {
      p->w32->priority_class = a->value;
      for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *v = proc_thread_slot(i);
        if (v && v->process == p)
          priority_apply(v);
      }
    }
  } else
    a->error = 120;
  return 0;
}
