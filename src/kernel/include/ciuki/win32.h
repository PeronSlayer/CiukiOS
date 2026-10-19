/* Win32 kernel mechanisms. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_WIN32_H
#define CIUKI_WIN32_H
#include <ciuki/process.h>
struct w32_delivery {
    struct w32_delivery *next;
    uint32_t flags;
    bool emergency;
};
struct w32_section { uint32_t rva, bytes, offset, raw, flags; };
struct w32_image {
    uint32_t base, bytes, entry, headers, stack_reserve, stack_commit, count, flags;
    uint32_t directories[32];
    struct w32_section section[96];
    const char *failure;
};
uint32_t w32_status(int error);
uint32_t w32_last_error(uint32_t status);
uint32_t w32_record_size(uint32_t operation);
int w32_record_validate(const void *record, uint32_t bytes, bool win32);
int32_t w32_syscall(struct trap_frame *tf);
bool w32_is_pe(const struct ciuki_file *file);
int w32_pe_validate(const struct ciuki_file *file, struct w32_image *image);
int w32_bootstrap_entry(struct process *, const struct w32_image *, uint32_t *);
int w32_image_map(struct process *p, const struct ciuki_file *file, const struct w32_image *image, bool builtin);
uint32_t w32_loader_call(struct proc_thread *, struct ciuki_w32_loader_args *);
int w32_loader_snapshot(struct process *, const struct ciuki_file *, uint32_t *);
int w32_loader_open(struct process *, const char *, struct ciuki_file *);
int w32_loader_image(struct process *, const struct ciuki_file *, struct w32_image *, uint32_t *, uint32_t);
int w32_spawn(struct process *p, const struct ciuki_file *file, const struct proc_strings *strings, uint64_t mask);
int w32_thread_init(struct proc_thread *t, bool main);
void w32_thread_cleanup(struct proc_thread *t);
uint32_t w32_protection(uint32_t protect, uint32_t *native);
uint32_t w32_vm_alloc(struct uaddr *u, struct ciuki_w32_vm *a);
uint32_t w32_vm_free(struct uaddr *u, struct ciuki_w32_vm *a);
uint32_t w32_vm_protect(struct uaddr *u, struct ciuki_w32_vm *a);
uint32_t w32_vm_query(const struct uaddr *u, uint32_t address, struct ciuki_w32_memory_info *out);
int w32_map_system(struct uaddr *u, uint32_t bytes, uint32_t owner, uint32_t *base);
uint32_t w32_guard_fault(struct proc_thread *t, uint32_t address, bool *retry);
void w32_identity(struct ciuki_w32_identity *out);
static inline uint64_t w32_counter_sample(uint64_t ticks, uint16_t before, bool pending, uint16_t after)
{
    if (!before || before > W32_PIT_DIVISOR) before = W32_PIT_DIVISOR;
    if (!after || after > W32_PIT_DIVISOR) after = W32_PIT_DIVISOR;
    if (pending || after > before) ticks++;
    return ticks * W32_PIT_DIVISOR + W32_PIT_DIVISOR - after;
}
uint64_t arch_w32_counter(void);
void w32_shared_tick(void);
bool w32_fault(struct trap_frame *tf, uint32_t address);
uint32_t w32_exception_code(uint32_t vector, bool privileged, uint16_t control, uint16_t status, uint32_t mxcsr);
bool w32_sysenter(const uint8_t *code, unsigned bytes);
bool w32_privileged(const uint8_t *code, unsigned bytes);
bool w32_context_validate(struct ciuki_w32_context *c, const struct uaddr *u, bool extended, uint32_t mxcsr_mask);
void w32_context_capture(struct ciuki_w32_context *c, const struct trap_frame *tf);
void w32_context_restore(struct trap_frame *tf, const struct ciuki_w32_context *c);
void w32_deliver(struct proc_thread *t, struct trap_frame *tf, const struct ciuki_w32_exception *exception, const struct ciuki_w32_context *context);
bool w32_continue(struct proc_thread *t, struct trap_frame *tf, struct ciuki_w32_context *context);
void fpu_w32_export(struct task *t, struct ciuki_w32_context *context);
void fpu_w32_import(struct task *t, const struct ciuki_w32_context *context);
uint32_t fpu_mxcsr_mask(void);
#endif
