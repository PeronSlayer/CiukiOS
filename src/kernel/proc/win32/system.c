/* SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/w32_core.h>
#include <ciuki/w32_objects.h>
#include <ciuki/security.h>
#include <ciuki/security_device.h>
extern int joystick_control(struct ciuki_joystick_control *) __attribute__((weak));
extern int joystick_hid_query(uint32_t, void *) __attribute__((weak));
extern int joystick_settings_select(struct process *) __attribute__((weak));
#include <ciuki/systime.h>
#pragma weak timezone_call
#include <ciuki/power_source.h>
#include <ciuki/supervisor.h>
#pragma weak power_source_win32_status
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
  if (a->action == W32_S_JOYSTICK) {
    struct ciuki_joystick_control q;
    if(!security_process_live(t->process) ||
       (!security_device_session(t->process) && !security_device_system(t->process))) a->error=5;
    else if(a->value==1 || a->value==2) {
      union {struct ciuki_hid_caps caps;struct ciuki_hid_state state;} u;
      unsigned size=a->value==1 ? sizeof(u.caps) : sizeof(u.state);
      if(a->extra!=size || copy_from_user(&u,a->low,size)) a->error=998;
      else if(u.caps.version!=CIUKI_HID_VERSION || u.caps.size!=size) a->error=87;
      else {
        struct ciuki_hid_caps c={.version=CIUKI_HID_VERSION,.size=sizeof(c),
            .device=u.caps.device,.generation=u.caps.generation};
        int e=joystick_hid_query ? joystick_hid_query(INPUT_HID_GET_CAPS,&c) : -ENODEV;
        if(!e && !(c.classes&CIUKI_HID_JOYSTICK)) e=-EACCES;
        if(!e && a->value==1) u.caps=c;
        else if(!e) {
          e=joystick_hid_query(INPUT_HID_GET_STATE,&u.state);
          memset(u.state.keyboard,0,sizeof(u.state.keyboard));u.state.consumers=0;
        }
        if(e) a->error=e==-EACCES || e==-EPERM ? 5 : 1167;
        else if(copy_to_user(a->low,&u,size)) a->error=998;
        else a->result=1;
      }
    }
    else if(a->extra!=sizeof(q) || copy_from_user(&q,a->low,sizeof(q))) a->error=998;
    else if(q.version!=CIUKI_JOYSTICK_VERSION || q.size!=sizeof(q) ||
        (q.operation!=CIUKI_JOY_QUERY && q.operation!=CIUKI_JOY_NEXT)) a->error=87;
    else {
      int e=joystick_settings_select ? joystick_settings_select(t->process) : 0;
      if(!e) e=joystick_control ? joystick_control(&q) : -ENODEV;
      if(e) a->error=e==-ENODEV ? 1167 : e==-EACCES || e==-EPERM ? 5 : 87;
      else if(copy_to_user(a->low,&q,sizeof(q))) a->error=998;
      else a->result=1;
    }
  } else if (a->action == W32_S_LAUNCH_IDLE) {
    if (t->process->w32) t->process->w32->launch_feedback_until = 0;
    a->result = 1;
  } else if (a->action == W32_S_LAUNCH_FEEDBACK) {
    struct process *desktop = t->process;
    if (desktop->personality != PROC_NATIVE || desktop->state != PROC_LIVE ||
        desktop->pid != supervisor_desktop_pid() || !desktop->w32 ||
        desktop->w32->service != W32_SERVICE_DESKTOP) { a->error = 5; return 0; }
    uint64_t now = w32_now(), expiry = 0;
    a->result = 0;
    for (unsigned i = 0; i < CIUKI_PROCESS_MAX; i++) {
      struct process *p = proc_at_slot(i);
      if (!p || !p->published || p->state != PROC_LIVE || !p->w32 ||
          !security_same(desktop,p)) continue;
      if (p->pid == a->value) p->w32->launch_feedback_until = 0;
      uint64_t until = p->w32->launch_feedback_until;
      if (until > now) {
        a->result++;
        if (!expiry || until < expiry) expiry = until;
      }
    }
    a->low = (uint32_t)expiry; a->high = (uint32_t)(expiry >> 32);
  } else if (a->action == W32_S_READ_MEMORY || a->action == W32_S_WRITE_MEMORY) {
    struct w32_object *o=w32_lookup(t,a->handle,W32_OBJ_PROCESS);
    if (!o || o->signalled) { a->error=6; return 0; }
    if (o->id!=t->process->pid||!w32_handle_access(t,a->handle,
        a->action==W32_S_READ_MEMORY?0x10u:0x28u)) { a->error=5; return 0; }
    /* low = source, high = destination, extra = byte count, for both APIs.
     * Pin both complete spans before copying; failures publish no bytes. */
    struct ua_pin src,dst;
    int e=a->extra ? ua_pin(t->process->memory,&src,a->low,a->extra,false) : 0;
    if (e) { a->error=299; return 0; }
    e=a->extra ? ua_pin(t->process->memory,&dst,a->high,a->extra,true) : 0;
    if (e) { if (a->extra) ua_unpin(t->process->memory,&src); a->error=299; return 0; }
    uint32_t n=a->extra<PAGE_SIZE ? a->extra : PAGE_SIZE;
    void *buffer=n ? w32_alloc(n) : 0;
    if (n && !buffer) a->error=8;
    bool backwards=a->high>a->low && a->high-a->low<a->extra;
    for (uint32_t at=0;!a->error && at<a->extra;) {
      uint32_t take=a->extra-at; if (take>n) take=n;
      uint32_t offset=backwards ? a->extra-at-take : at;
      if (copy_from_user(buffer,a->low+offset,take) || copy_to_user(a->high+offset,buffer,take)) a->error=299;
      at+=take;
    }
    if (!a->error) a->result=a->extra;
    w32_free(buffer,n);
    if (a->extra) { ua_unpin(t->process->memory,&dst); ua_unpin(t->process->memory,&src); }
  } else if (a->action == W32_S_TIME) {
    uint64_t n = w32_system_time();
    a->low = (uint32_t)n;
    a->high = (uint32_t)(n >> 32);
  } else if (a->action == W32_S_TIME_ZONE) {
    struct ciuki_time_zone z;
    if (a->extra != sizeof(z) || copy_from_user(&z, a->low, sizeof(z)))
      a->error = 998;
    else {
      int e = timezone_call ? timezone_call(t->process, &z) : -ENOSYS;
      if (e == -EPERM) a->error = 1314;      /* ERROR_PRIVILEGE_NOT_HELD */
      else if (e == -EINVAL) a->error = 87;
      else if (e == -EBUSY) a->error = 170;  /* ERROR_BUSY */
      else if (e) a->error = 31;             /* ERROR_GEN_FAILURE */
      else if (copy_to_user(a->low, &z, sizeof(z))) a->error = 998;
    }
  } else if (a->action == W32_S_POWER) {
    struct power_source_win32 p = {255, 255, 255, UINT32_MAX};
    if (power_source_win32_status) power_source_win32_status(&p);
    a->low = p.ac; a->high = p.battery_flag; a->value = p.percent;
    a->extra = p.seconds; a->result = UINT32_MAX;
  } else if (a->action == W32_S_MEMORY) {
    a->low = pmm_total_usable();
    a->high = pmm_free_count();
    uint32_t reserved = 0;
    for (const struct ua_extent *e = t->process->memory->head; e; e = e->next) {
      uint32_t first=e->base<0x10000u ? 0x10000u : e->base;
      uint32_t end=e->end>0x7fff0000u ? 0x7fff0000u : e->end;
      if (end>first) reserved+=(end-first)/PAGE_SIZE;
    }
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
    else if(!w32_handle_access(t,a->handle,1)||!security_process_live(t->process)||
        (!security_same(t->process,p)&&(!security_admin(t->process)||security_system(p))))a->error=5;
    else {
      if(p->w32)p->w32->exit_reason=W32_EXIT_KILLED;
      proc_stop(p, (int)a->value, 0);
    }
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
    else if (!security_same(t->process,target->process) || !w32_handle_access(t,a->handle,1))
      a->error=5;
    else if (target->process->personality != PROC_WIN32)
      a->error = 120;
    else {
      target->w32->terminate_code = a->value;
      target->w32->terminate_requested = true;
      target->w32->suspend = 0;
      /* Same-process admission reports the victim TID so the caller's
       * in-process USER can release that thread's state; the victim exits at
       * its next user return and never executes user code again. */
      a->result = target->process == t->process ? target->tid : 0;
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
    else if (!security_same(t->process,target->process)) a->error=5;
    else if (a->action == W32_S_QUERY_PRIORITY ?
        (!w32_handle_access(t,a->handle,0x40u)&&!w32_handle_access(t,a->handle,0x800u)) :
        !w32_handle_access(t,a->handle,0x20u)) a->error=5;
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
    else if (!security_same(t->process,p)) a->error=5;
    else if (a->action == W32_S_QUERY_PRIORITY ?
        (!w32_handle_access(t,a->handle,0x400u)&&!w32_handle_access(t,a->handle,0x1000u)) :
        !w32_handle_access(t,a->handle,0x200u)) a->error=5;
    else if (a->action == W32_S_QUERY_PRIORITY)
      a->result = p->w32->priority_class;
    else if (a->value!=0x100 && proc_priority_win32(a->value, 0, false) < 0)
      a->error = 87;
    else {
      p->w32->priority_class = a->value==0x100 ? 0x80 : a->value;
      if (a->value==0x100) klog("[win32] start pid=%u prio=realtime→high",p->pid);
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
