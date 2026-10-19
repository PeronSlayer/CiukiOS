/* Additive copied F5-16 records. SPDX-License-Identifier: MIT */
#ifndef CIUKI_W32_DIAGNOSTICS_H
#define CIUKI_W32_DIAGNOSTICS_H
#include <ciuki/abi.h>
#define W32_COMPAT 0x10du
#define W32_REPORT 0x10eu
#define W32_SHIM_COUNT 12u
enum w32_shim_id {
    W32_SH_VERSION, W32_SH_DDRAW_555, W32_SH_DDRAW_SCALE,
    W32_SH_DDRAW_WINDOWED, W32_SH_HEAP_ZERO, W32_SH_HEAP_PAD,
    W32_SH_MEMSTATUS_CAP, W32_SH_IO_PIT, W32_SH_IO_GAMEPORT,
    W32_SH_DLL_NATIVE, W32_SH_FPU_CW, W32_SH_W_STUBS
};
#define W32_SHIM_BIT(id) (1u << (id))
struct ciuki_w32_compat {
    uint32_t mask, version, heap_pad, memory_mib, fpu_cw, scale, width, height;
    char native_dlls[256], w_exports[512];
};
struct ciuki_w32_compat_args {
    struct ciuki_w32_header header;
    uint32_t action, settings, bytes, timestamp, image_size, file_size;
    uint32_t source, source_bytes;
};
enum w32_report_action { W32_REPORT_CRASH=1, W32_REPORT_REFUSE,
    W32_REPORT_NEXT, W32_REPORT_CLOSE, W32_REPORT_REASON };
enum w32_refusal { W32_REFUSE_MSI=1, W32_REFUSE_DRIVER, W32_REFUSE_RUNTIME,
    W32_REFUSE_MISSING_DLL, W32_REFUSE_EXPORT, W32_REFUSE_IMAGE, W32_REFUSE_SERVER };
enum w32_exit_reason { W32_EXIT_NORMAL, W32_EXIT_CRASH, W32_EXIT_KILLED, W32_EXIT_RESTART };
struct ciuki_w32_report_args {
    struct ciuki_w32_header header;
    uint32_t action, code, context, module, base, buffer, bytes, result;
};
/* No pointers, strings, file contents or general memory dump on the wire.
 * registers: eax ebx ecx edx esi edi ebp esp eflags. Unreadable stack words
 * are zero with their valid-mask bit clear; display them as ????????. */
struct ciuki_w32_fault {
    uint32_t kind, pid, tid, code, eip, offset, valid;
    uint32_t registers[9], stack[8];
    char exe[64], module[64];
};
_Static_assert(sizeof(struct ciuki_w32_fault)==224,"fault wire");
_Static_assert(sizeof(struct ciuki_w32_report_args)==48,"report wire");
_Static_assert(sizeof(struct ciuki_w32_compat)==800,"compat wire");
#endif
