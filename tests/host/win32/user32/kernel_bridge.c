/* Production f5-04 queue/wait code; deterministic scheduling and RAM adapter.
 * SPDX-License-Identifier: GPL-2.0-only */
#define W32_KERNEL_BRIDGE
#define w32_pe_validate w32_kernel_pe_validate
#include "../kernel_core_test.c"
/* f5-16 added [win32] exit/crash/stub records through klog in the Win32
 * kernel sources this fixture links; the core fixture omits klog under
 * W32_KERNEL_BRIDGE, so the bridge forwards the record to the trace log. */
#include <stdarg.h>
void klog(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
}
#undef main
#include <stdarg.h>
/* The shared kernel harness leaves bridge logging to each adapter. Preserve
 * f5-16 exit diagnostics when the USER fixture tears down its processes. */
void klog(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
  putchar('\n');
}
static struct process *ui_processes[2];
static struct proc_thread *ui_threads[3];
static uint32_t buffers[3];
static unsigned current_thread;
static void (*dispatch)(void);
static void interleave(void) {
  if (dispatch)
    dispatch();
}
void ui_kernel_init(void) {
  ram = calloc(HOST_PAGES, PAGE_SIZE);
  CHECK(ram);
  g_current = &controller;
  controller.state = T_RUNNING;
  proc_init();
  ui_threads[0] = object_process(proc_supervisor(), &ui_processes[0]);
  CHECK(!proc_thread_prepare(ui_processes[0], 0x401000, 0, 0, 0, 0, 0, true,
                             &ui_threads[1]));
  ui_threads[2] = object_process(proc_supervisor(), &ui_processes[1]);
  for (unsigned i = 0; i < 3; i++) {
    struct ciuki_w32_vm a = {.bytes = 65536,
                             .flags = W32_MEM_RESERVE | W32_MEM_COMMIT,
                             .protect = W32_PAGE_READWRITE};
    CHECK(!w32_vm_alloc(ui_threads[i]->process->memory, &a));
    buffers[i] = a.address;
  }
  on_schedule = interleave;
  g_current = ui_threads[0]->task;
  g_current->state = T_RUNNING;
}
void ui_kernel_switch(unsigned i) {
  CHECK(i < 3);
  current_thread = i;
  g_current = ui_threads[i]->task;
  g_current->state = T_RUNNING;
}
unsigned ui_kernel_current(void) { return current_thread; }
uint32_t ui_kernel_tid(void) { return ui_threads[current_thread]->tid; }
uint32_t ui_kernel_pid(void) {
  return ui_threads[current_thread]->process->pid;
}
uint32_t ui_kernel_ticks(void) { return (uint32_t)g_ticks; }
void ui_kernel_schedule(void (*fn)(void)) { dispatch = fn; }
void ui_kernel_pause(uint32_t n) {
  g_ticks += n;
  if (dispatch)
    dispatch();
}
int32_t ui_kernel_call(void *record, uint32_t bytes) {
  struct ciuki_w32_header *header = record;
  CHECK(bytes == header->size);
  CHECK(!w32_record_validate(record, bytes, true));
  struct proc_thread *thread = ui_threads[current_thread];
  if (header->operation == W32_MESSAGE) {
    struct ciuki_w32_message_args *r = record, a = *r;
    uint32_t original = a.buffer, capacity = a.bytes;
    if (capacity) {
      CHECK(capacity <= 65536);
      CHECK(!ua_write(thread->process->memory, buffers[current_thread],
                      (void *)(uintptr_t)original, capacity));
      a.buffer = buffers[current_thread];
    }
    CHECK(!w32_message_call(thread, &a));
    if (capacity && !a.error &&
        (a.action == W32_MSG_GET || a.action == W32_MSG_SEND ||
         a.action == W32_MSG_POLL_REPLY || a.action == W32_MSG_SELECT)) {
      CHECK(!ua_read(thread->process->memory, (void *)(uintptr_t)original,
                     buffers[current_thread], capacity));
    }
    if (a.token && a.message.lparam == a.buffer)
      a.message.lparam = original;
    a.buffer = original;
    *r = a;
    return 0;
  }
  if (header->operation == W32_WAIT) {
    struct ciuki_w32_wait_args *r = record, a = *r;
    uint32_t original = a.handles;
    if (a.count) {
      CHECK(!ua_write(thread->process->memory, buffers[current_thread],
                      (void *)(uintptr_t)original, a.count * 4));
      a.handles = buffers[current_thread];
    }
    CHECK(!w32_wait_call(thread, &a));
    a.handles = original;
    *r = a;
    return 0;
  }
  if (header->operation == W32_SERVICE) {
    struct ciuki_w32_service_args *r = record;
    r->endpoint = 7;
    r->result = 1;
    return 0;
  }
  CHECK(false);
  return -1;
}
void ui_kernel_finish(void) {
  dispatch = 0;
  on_schedule = 0;
  g_current = &controller;
  proc_discard(ui_processes[1]);
  proc_discard(ui_processes[0]);
  CHECK(!sends && !w32_accounts.messages && !w32_accounts.objects &&
        !w32_accounts.handles);
  free(ram);
  puts("PASS USER production kernel queues and teardown");
}
