/* Private PE/kernel result protocol. SPDX-License-Identifier: MIT */
#ifndef CIUKI_W32_KERNEL32_CONTROLLER_H
#define CIUKI_W32_KERNEL32_CONTROLLER_H
#include <stdint.h>
#define W32_KERNEL32_RESULT_VA 0x10030000u
#define W32_KERNEL32_PATH "/System/Win32/Fixtures/kernel32-exercise.exe"
#define W32_KERNEL32_MAGIC 0x32334b57u
enum { W32_K32_FILES, W32_K32_MEMORY, W32_K32_THREADS, W32_K32_PROFILE, W32_K32_TEARDOWN, W32_K32_CASES };
struct w32_kernel32_result {
    uint32_t magic, version, done, release;
    uint32_t checked[W32_K32_CASES], errors[W32_K32_CASES], first_error;
};
_Static_assert(sizeof(struct w32_kernel32_result)==60,"kernel32 result layout");
static inline int w32_kernel32_valid(const struct w32_kernel32_result *r)
{
    if (r->magic!=W32_KERNEL32_MAGIC || r->version!=1 || r->done!=1) return 0;
    for (unsigned i=0;i<W32_K32_CASES;i++) if (!r->checked[i]) return 0;
    return 1;
}
static inline int w32_kernel32_pass(const struct w32_kernel32_result *r, uint32_t exit_code)
{
    if (!w32_kernel32_valid(r) || exit_code) return 0;
    for (unsigned i=0;i<W32_K32_CASES;i++) if (r->errors[i]) return 0;
    return 1;
}
static inline void w32_kernel32_publish(const struct w32_kernel32_result *r,
    void (*emit)(const char *,const char *,const char *,...))
{
    static const char *const cases[]={"files","memory","threads","profile","teardown"};
    for (unsigned i=0;i<W32_K32_CASES;i++)
        emit("w32-kernel32","DATA","case=%s errors=%u checked=%u first_error=%u",
             cases[i],r->errors[i],r->checked[i],r->first_error);
}
#endif
