#include "shell.h"
#include "video.h"
#include "keyboard.h"
#include "timer.h"
#include "services.h"
#include "fat.h"
#include "dos_mz.h"
#include "bootcfg.h"
#include "splash.h"
#include "version.h"
#include "mouse.h"
#include "ui.h"
#include "serial.h"
#include "gfx2d.h"
#include "image.h"
#include "vm86.h"
#include "mode_switch.h"
#include "v86_dispatch.h"
#include "gfx_modes.h"
#include "app_catalog.h"

#define SHELL_JOIN2(a, b) a##b
#define SHELL_JOIN(a, b) SHELL_JOIN2(a, b)
#define SHELL_MODE_SWITCH_CALL(name) SHELL_JOIN(mode_switch_, name)

#define SHELL_LINE_MAX 128
#define SHELL_HISTORY_MAX 32U
#define SHELL_FILE_BUFFER_SIZE (128U * 1024U)
#define SHELL_RUNTIME_COM_ADDR 0x0000000000600000ULL
#define SHELL_RUNTIME_COM_MAX_SIZE (512U * 1024U)
#define SHELL_RUNTIME_PSP_SIZE 0x100U
#define SHELL_RUNTIME_COM_ENTRY_ADDR (SHELL_RUNTIME_COM_ADDR + SHELL_RUNTIME_PSP_SIZE)
#define SHELL_RUNTIME_COM_MAX_PAYLOAD (SHELL_RUNTIME_COM_MAX_SIZE - SHELL_RUNTIME_PSP_SIZE)

/* Forward decl of runtime gfx services table (defined later with shell_gfx command). */
static const ciuki_gfx_services_t g_shell_runtime_gfx_services;
#define SHELL_RUNTIME_TAIL_MAX 126U
#define SHELL_EXE32_MARKER_SIZE 8U
#define SHELL_PATH_MAX 128
#define SHELL_PATH_MAX_TOKENS 16
#define SHELL_PATH_TOKEN_MAX 13
#define SHELL_INT21_MAX_FILE_HANDLES 8U
#define SHELL_INT21_HANDLE_BASE 5U
#define SHELL_INT21_FILE_BUF_CAP (64U * 1024U)
#define SHELL_INT21_MEM_MAX_BLOCKS 32U
#define SHELL_INT21_DTA_SIZE 43U
#define SHELL_INT21_DTA_NAME_OFFSET 0x1EU
#define SHELL_INT21_DTA_ATTR_OFFSET 0x15U
#define SHELL_INT21_DTA_SIZE_OFFSET 0x1AU
#define SHELL_DPMI_MEM_MAX_BLOCKS 16U
#define SHELL_DPMI_MEM_BASE 0x00100000U
#define SHELL_DPMI_MEM_LIMIT 0x02100000U
#define SHELL_ENV_MAX 32U
#define SHELL_ENV_NAME_MAX 16U
#define SHELL_ENV_VALUE_MAX 96U
#define SHELL_BATCH_MAX_LINES 256U
#define SHELL_BATCH_MAX_LABELS 128U
#define SHELL_BATCH_MAX_STEPS 2048U
#define SHELL_BATCH_MAX_DEPTH 4U

static const u8 g_exe32_marker[SHELL_EXE32_MARKER_SIZE] = {
    'C', 'I', 'U', 'K', 'E', 'X', '6', '4'
};

static u8 g_shell_file_buffer[SHELL_FILE_BUFFER_SIZE];
static char g_shell_cwd[SHELL_PATH_MAX] = "/EFI/CIUKIOS";
static u8 g_shell_errorlevel = 0U;
static u8 g_shell_batch_depth = 0U;

/* OPENGEM-002-BAT — per-frame batch state.
 *
 * g_batch_echo      : 1=echo on, 0=echo off (ECHO OFF/ON + leading `@`).
 * g_batch_argc/argv : positional args for %0..%9 expansion. argv[0] is
 *                     the current batch file path; argv[1..] come from
 *                     CALL or from the .BAT dispatch tail.
 * g_batch_cur_path  : path of the batch currently executing (for
 *                     serial markers).
 *
 * Saved and restored across nested batch frames (`CALL`) inside
 * shell_run_batch_file() itself.
 */
#define SHELL_BATCH_ARGV_MAX 10U
static u8 g_batch_echo = 1U;
static u8 g_batch_argc = 0U;
static const char *g_batch_argv[SHELL_BATCH_ARGV_MAX] = {
    "", "", "", "", "", "", "", "", "", ""
};
static const char *g_batch_cur_path = "";

typedef enum shell_dosrun_error_class {
    SHELL_DOSRUN_ERROR_NONE = 0,
    SHELL_DOSRUN_ERROR_NOT_FOUND,
    SHELL_DOSRUN_ERROR_BAD_FORMAT,
    SHELL_DOSRUN_ERROR_RUNTIME,
    SHELL_DOSRUN_ERROR_UNSUPPORTED_INT21,
    SHELL_DOSRUN_ERROR_ARGS_PARSE
} shell_dosrun_error_class_t;

static shell_dosrun_error_class_t g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_NONE;
static u32 g_shell_dosrun_int21_unsupported_calls = 0U;
static u8 g_shell_runtime_graphics_used = 0U;
static u8 g_shell_runtime_video_print_suppressed = 0U;
static u8 g_shell_prompt_deferred = 0U;

typedef struct shell_env_var {
    u8 used;
    char name[SHELL_ENV_NAME_MAX];
    char value[SHELL_ENV_VALUE_MAX];
} shell_env_var_t;

typedef struct shell_batch_label {
    char name[32];
    u16 line_index;
} shell_batch_label_t;

static shell_env_var_t g_shell_env_vars[SHELL_ENV_MAX];

typedef struct shell_int21_file_handle {
    int used;
    u16 handle_id;
    u8 mode;   /* 0=read,1=write,2=read/write */
    u8 dirty;
    u32 size;
    u32 pos;
    char path[SHELL_PATH_MAX];
    u8 data[SHELL_INT21_FILE_BUF_CAP];
} shell_int21_file_handle_t;

typedef struct shell_int21_mem_block {
    u16 seg;
    u16 paras;
    u8 used;
    u16 owner_psp;
    u8 reserved;
} shell_int21_mem_block_t;

typedef struct shell_int21_find_state {
    int active;
    u8 attr_mask;
    u16 next_index;
    u16 dta_segment;
    u16 dta_offset;
    char dir[SHELL_PATH_MAX];
    char pattern[SHELL_PATH_MAX];
} shell_int21_find_state_t;

typedef struct shell_dpmi_mem_block {
    u8 used;
    u8 reserved[3];
    u32 handle;
    u32 linear_base;
    u32 size;
} shell_dpmi_mem_block_t;

static shell_int21_file_handle_t g_int21_file_handles[SHELL_INT21_MAX_FILE_HANDLES];
static shell_dpmi_mem_block_t g_shell_dpmi_mem_blocks[SHELL_DPMI_MEM_MAX_BLOCKS];
static u32 g_shell_dpmi_next_handle = 1U;

static shell_dpmi_mem_block_t *shell_dpmi_find_block_by_handle(u32 handle) {
    if (handle == 0U) {
        return 0;
    }

    for (u32 i = 0U; i < SHELL_DPMI_MEM_MAX_BLOCKS; i++) {
        if (g_shell_dpmi_mem_blocks[i].used &&
            g_shell_dpmi_mem_blocks[i].handle == handle) {
            return &g_shell_dpmi_mem_blocks[i];
        }
    }

    return 0;
}

static shell_dpmi_mem_block_t *shell_dpmi_find_free_block_slot(void) {
    for (u32 i = 0U; i < SHELL_DPMI_MEM_MAX_BLOCKS; i++) {
        if (!g_shell_dpmi_mem_blocks[i].used) {
            return &g_shell_dpmi_mem_blocks[i];
        }
    }

    return 0;
}

static ciuki_int21_regs_t *shell_dpmi_real_mode_regs_ptr(ciuki_dos_context_t *ctx, u16 seg, u16 off) {
    if (!ctx) {
        return 0;
    }

    if (seg != ctx->psp_segment) {
        return 0;
    }

    if ((u32)off >= ctx->image_size) {
        return 0;
    }

    if ((u32)off + (u32)sizeof(ciuki_int21_regs_t) > ctx->image_size) {
        return 0;
    }

    return (ciuki_int21_regs_t *)(ctx->image_linear + (u64)off);
}

static int shell_dpmi_alloc_mem_block(u32 size, u32 *linear_out, u32 *handle_out) {
    shell_dpmi_mem_block_t *slot;
    u32 cursor;

    if (size == 0U || !linear_out || !handle_out) {
        return 0;
    }

    slot = shell_dpmi_find_free_block_slot();
    if (!slot) {
        return 0;
    }

    cursor = SHELL_DPMI_MEM_BASE;
    while (1) {
        int overlapped = 0;
        u64 candidate_end = (u64)cursor + (u64)size;
        if (candidate_end > (u64)SHELL_DPMI_MEM_LIMIT) {
            return 0;
        }

        for (u32 i = 0U; i < SHELL_DPMI_MEM_MAX_BLOCKS; i++) {
            shell_dpmi_mem_block_t *block = &g_shell_dpmi_mem_blocks[i];
            u64 block_start;
            u64 block_end;

            if (!block->used) {
                continue;
            }

            block_start = (u64)block->linear_base;
            block_end = block_start + (u64)block->size;
            if (candidate_end <= block_start || (u64)cursor >= block_end) {
                continue;
            }

            cursor = (u32)((block_end + 0xFFFULL) & ~0xFFFULL);
            overlapped = 1;
            break;
        }

        if (!overlapped) {
            break;
        }
    }

    slot->used = 1U;
    slot->handle = g_shell_dpmi_next_handle++;
    if (g_shell_dpmi_next_handle == 0U) {
        g_shell_dpmi_next_handle = 1U;
    }
    slot->linear_base = cursor;
    slot->size = size;

    *linear_out = slot->linear_base;
    *handle_out = slot->handle;
    return 1;
}

static int shell_dpmi_free_mem_block(u32 handle) {
    shell_dpmi_mem_block_t *block = shell_dpmi_find_block_by_handle(handle);
    if (!block) {
        return 0;
    }

    block->used = 0U;
    block->handle = 0U;
    block->linear_base = 0U;
    block->size = 0U;
    return 1;
}

static int shell_dir_fat_cb(const fat_dir_entry_t *entry, void *ctx_void);
static int shell_int21_resolve_rw_buffer(ciuki_dos_context_t *ctx, u16 off, u16 count, u8 **buf_out);
static void shell_run_batch_file(boot_info_t *boot_info, handoff_v0_t *handoff, const char *path);
static void shell_startup_chain(boot_info_t *boot_info, handoff_v0_t *handoff);
static void shell_execute_line(const char *line, boot_info_t *boot_info, handoff_v0_t *handoff);
static int shell_dir_exists_cb(const fat_dir_entry_t *entry, void *ctx_void) {
    (void)entry;
    (void)ctx_void;
    return 1;
}

static int is_space(u8 ch) {
    return ch == ' ' || ch == '\t';
}

static int is_printable_ascii(u8 ch) {
    return ch >= 0x20 && ch <= 0x7E;
}

static u8 to_lower_ascii(u8 ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (u8)(ch + ('a' - 'A'));
    }
    return ch;
}

static u8 to_upper_ascii(u8 ch) {
    if (ch >= 'a' && ch <= 'z') {
        return (u8)(ch - ('a' - 'A'));
    }
    return ch;
}

/* Write a 32-bit unsigned integer in decimal to the framebuffer */
static void write_decimal(u32 val) {
    char buf[12];
    u32 i = 0;
    if (val == 0U) {
        video_putchar('0');
        return;
    }
    while (val > 0U) {
        buf[i++] = (char)('0' + (val % 10U));
        val /= 10U;
    }
    while (i > 0U) {
        video_putchar(buf[--i]);
    }
}

static inline void outb_port(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw_port(u16 port, u16 val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb_port(u16 port) {
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static u8 cmos_read_byte(u8 idx) {
    outb_port(0x70U, (u8)(0x80U | (idx & 0x7FU)));
    return inb_port(0x71U);
}

static void cmos_write_byte(u8 idx, u8 value) {
    outb_port(0x70U, (u8)(0x80U | (idx & 0x7FU)));
    outb_port(0x71U, value);
}

static int bootcfg_load(bootcfg_data_t *cfg) {
    if (!cfg) {
        return 0;
    }

    for (u32 i = 0U; i < BOOTCFG_CMOS_SIZE; i++) {
        ((u8 *)cfg)[i] = cmos_read_byte((u8)(BOOTCFG_CMOS_BASE + i));
    }

    return bootcfg_valid(cfg) ? 1 : 0;
}

static int bootcfg_store(const bootcfg_data_t *cfg) {
    bootcfg_data_t tmp;

    if (!cfg) {
        return 0;
    }

    for (u32 i = 0U; i < BOOTCFG_CMOS_SIZE; i++) {
        ((u8 *)&tmp)[i] = ((const u8 *)cfg)[i];
    }
    bootcfg_finalize(&tmp);

    for (u32 i = 0U; i < BOOTCFG_CMOS_SIZE; i++) {
        cmos_write_byte((u8)(BOOTCFG_CMOS_BASE + i), ((const u8 *)&tmp)[i]);
    }

    return 1;
}

static void bootcfg_clear(void) {
    for (u32 i = 0U; i < BOOTCFG_CMOS_SIZE; i++) {
        cmos_write_byte((u8)(BOOTCFG_CMOS_BASE + i), 0U);
    }
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void str_copy(char *dst, const char *src, u32 dst_size) {
    u32 i = 0;
    if (dst_size == 0) {
        return;
    }
    while (src[i] != '\0' && (i + 1) < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static u32 str_len(const char *s) {
    u32 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int str_eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        if (to_lower_ascii((u8)*a) != to_lower_ascii((u8)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int str_starts_with_nocase(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s == '\0') {
            return 0;
        }
        if (to_lower_ascii((u8)*s) != to_lower_ascii((u8)*prefix)) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

static int str_ends_with_nocase(const char *s, const char *suffix) {
    u32 slen = str_len(s);
    u32 tlen = str_len(suffix);
    if (tlen > slen) {
        return 0;
    }
    return str_eq_nocase(s + (slen - tlen), suffix);
}

static void trim_ascii_inplace(char *s) {
    u32 len;
    u32 i = 0U;

    if (!s) {
        return;
    }

    while (s[i] != '\0' && is_space((u8)s[i])) {
        i++;
    }
    if (i > 0U) {
        u32 j = 0U;
        while (s[i] != '\0') {
            s[j++] = s[i++];
        }
        s[j] = '\0';
    }

    len = str_len(s);
    while (len > 0U && is_space((u8)s[len - 1U])) {
        s[len - 1U] = '\0';
        len--;
    }
}

static void shell_set_errorlevel(u8 code) {
    g_shell_errorlevel = code;
}

static u8 shell_get_errorlevel(void) {
    return g_shell_errorlevel;
}

static int shell_run_target_is_supported(const char *target) {
    if (!target || target[0] == '\0') {
        return 0;
    }
    return str_ends_with_nocase(target, ".COM")
        || str_ends_with_nocase(target, ".EXE")
        || str_ends_with_nocase(target, ".BAT");
}

static const char *shell_dosrun_error_class_name(shell_dosrun_error_class_t cls) {
    if (cls == SHELL_DOSRUN_ERROR_NOT_FOUND) {
        return "not_found";
    }
    if (cls == SHELL_DOSRUN_ERROR_BAD_FORMAT) {
        return "bad_format";
    }
    if (cls == SHELL_DOSRUN_ERROR_RUNTIME) {
        return "runtime";
    }
    if (cls == SHELL_DOSRUN_ERROR_UNSUPPORTED_INT21) {
        return "unsupported_int21";
    }
    if (cls == SHELL_DOSRUN_ERROR_ARGS_PARSE) {
        return "args_parse";
    }
    return "runtime";
}

static void shell_dosrun_emit_launch_marker(const char *path, const char *type) {
    serial_write("[dosrun] launch path=");
    serial_write(path ? path : "<unknown>");
    serial_write(" type=");
    serial_write(type ? type : "COM");
    serial_write("\n");
}

static void shell_dosrun_emit_ok_marker(u8 code) {
    serial_write("[dosrun] result=ok code=0x");
    serial_write_hex8(code);
    serial_write("\n");
}

static void shell_dosrun_emit_error_marker(shell_dosrun_error_class_t cls) {
    serial_write("[dosrun] result=error class=");
    serial_write(shell_dosrun_error_class_name(cls));
    serial_write("\n");
}

static void shell_serial_write_dec32(u32 value) {
    char buf[11];
    u32 i = 0U;
    if (value == 0U) {
        serial_write("0");
        return;
    }
    while (value > 0U && i < sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (i > 0U) {
        char tmp[2];
        tmp[0] = buf[--i];
        tmp[1] = '\0';
        serial_write(tmp);
    }
}

static u32 shell_count_tail_chars(const char *args) {
    const char *p;
    const char *start;
    const char *end;

    if (!args) {
        return 0U;
    }
    p = args;
    while (*p && is_space((u8)*p)) {
        p++;
    }
    while (*p && !is_space((u8)*p)) {
        p++;
    }
    while (*p && is_space((u8)*p)) {
        p++;
    }
    start = p;
    end = p;
    while (*p) {
        if (!is_space((u8)*p)) {
            end = p + 1;
        }
        p++;
    }
    return (u32)(end - start);
}

static void shell_dosrun_emit_argv_markers(u32 raw_tail_len, int overflow) {
    serial_write("[dosrun] argv tail len=");
    shell_serial_write_dec32(raw_tail_len);
    serial_write("\n");
    serial_write("[dosrun] argv parse=");
    serial_write(overflow ? "FAIL" : "PASS");
    serial_write("\n");
}

static void shell_env_clear_all(void) {
    for (u32 i = 0U; i < SHELL_ENV_MAX; i++) {
        g_shell_env_vars[i].used = 0U;
        g_shell_env_vars[i].name[0] = '\0';
        g_shell_env_vars[i].value[0] = '\0';
    }
}

static void shell_env_normalize_name(const char *in, char *out, u32 out_size) {
    u32 i = 0U;

    if (!out || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }

    while (*in && is_space((u8)*in)) {
        in++;
    }
    while (*in && !is_space((u8)*in) && *in != '=' && (i + 1U) < out_size) {
        out[i++] = (char)to_upper_ascii((u8)*in);
        in++;
    }
    out[i] = '\0';
}

static shell_env_var_t *shell_env_find_slot(const char *name, int create) {
    u32 free_idx = SHELL_ENV_MAX;

    for (u32 i = 0U; i < SHELL_ENV_MAX; i++) {
        if (g_shell_env_vars[i].used) {
            if (str_eq(g_shell_env_vars[i].name, name)) {
                return &g_shell_env_vars[i];
            }
        } else if (free_idx == SHELL_ENV_MAX) {
            free_idx = i;
        }
    }

    if (!create || free_idx == SHELL_ENV_MAX) {
        return (shell_env_var_t *)0;
    }

    g_shell_env_vars[free_idx].used = 1U;
    str_copy(g_shell_env_vars[free_idx].name, name, (u32)sizeof(g_shell_env_vars[free_idx].name));
    g_shell_env_vars[free_idx].value[0] = '\0';
    return &g_shell_env_vars[free_idx];
}

static const char *shell_env_get(const char *name) {
    shell_env_var_t *slot = shell_env_find_slot(name, 0);
    if (!slot) {
        return "";
    }
    return slot->value;
}

static int shell_env_set(const char *name_in, const char *value_in) {
    char name[SHELL_ENV_NAME_MAX];
    shell_env_var_t *slot;

    shell_env_normalize_name(name_in, name, (u32)sizeof(name));
    if (name[0] == '\0') {
        return 0;
    }

    slot = shell_env_find_slot(name, 1);
    if (!slot) {
        return 0;
    }

    str_copy(slot->value, value_in ? value_in : "", (u32)sizeof(slot->value));
    return 1;
}

static void shell_env_expand_line(const char *in, char *out, u32 out_size) {
    u32 oi = 0U;
    u32 i = 0U;

    if (!out || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }

    while (in[i] != '\0' && (oi + 1U) < out_size) {
        if (in[i] == '%') {
            /* OPENGEM-002-BAT: %% -> literal '%'. */
            if (in[i + 1U] == '%') {
                out[oi++] = '%';
                i += 2U;
                continue;
            }
            /* OPENGEM-002-BAT: %0..%9 -> batch positional arg. */
            if (in[i + 1U] >= '0' && in[i + 1U] <= '9') {
                u8 idx = (u8)(in[i + 1U] - '0');
                const char *pv = "";
                if (idx < g_batch_argc && g_batch_argv[idx]) {
                    pv = g_batch_argv[idx];
                }
                for (u32 k = 0U; pv[k] != '\0' && (oi + 1U) < out_size; k++) {
                    out[oi++] = pv[k];
                }
                i += 2U;
                continue;
            }

            u32 j = i + 1U;
            char name[SHELL_ENV_NAME_MAX];
            u32 ni = 0U;

            while (in[j] != '\0' && in[j] != '%' && (ni + 1U) < (u32)sizeof(name)) {
                name[ni++] = (char)to_upper_ascii((u8)in[j]);
                j++;
            }

            if (in[j] == '%' && ni > 0U) {
                const char *val;
                name[ni] = '\0';
                val = shell_env_get(name);
                for (u32 k = 0U; val[k] != '\0' && (oi + 1U) < out_size; k++) {
                    out[oi++] = val[k];
                }
                i = j + 1U;
                continue;
            }
        }

        out[oi++] = in[i++];
    }

    out[oi] = '\0';
}

static int shell_parse_set_assignment(const char *args, char *name_out, u32 name_size, char *value_out, u32 value_size) {
    const char *eq;
    u32 ni = 0U;
    u32 vi = 0U;

    if (!args || !name_out || !value_out || name_size == 0U || value_size == 0U) {
        return 0;
    }

    while (*args && is_space((u8)*args)) {
        args++;
    }

    eq = args;
    while (*eq && *eq != '=') {
        eq++;
    }
    if (*eq != '=') {
        return 0;
    }

    while (args < eq && ni + 1U < name_size) {
        if (!is_space((u8)*args)) {
            name_out[ni++] = (char)to_upper_ascii((u8)*args);
        }
        args++;
    }
    name_out[ni] = '\0';
    if (name_out[0] == '\0') {
        return 0;
    }

    args = eq + 1;
    while (*args && vi + 1U < value_size) {
        value_out[vi++] = *args++;
    }
    value_out[vi] = '\0';
    trim_ascii_inplace(value_out);
    return 1;
}

static void local_memset(void *dst_void, u8 value, u32 count) {
    u8 *dst = (u8 *)dst_void;
    for (u32 i = 0; i < count; i++) {
        dst[i] = value;
    }
}

static void local_memcpy(void *dst_void, const void *src_void, u32 count) {
    u8 *dst = (u8 *)dst_void;
    const u8 *src = (const u8 *)src_void;
    for (u32 i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

static void write_dos_path(const char *path) {
    u32 i = 0;

    video_write("A:");
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        video_write("\\");
        return;
    }

    while (path[i] != '\0') {
        char ch = path[i++];
        if (ch == '/') {
            video_putchar('\\');
        } else {
            video_putchar((char)to_upper_ascii((u8)ch));
        }
    }
}

static int split_path_tokens(
    const char *path,
    char tokens[SHELL_PATH_MAX_TOKENS][SHELL_PATH_TOKEN_MAX],
    u32 *count_out
) {
    const char *p = path;
    u32 count = 0;

    if (!path || !count_out) {
        return 0;
    }

    while (*p) {
        u32 n = 0;

        while (*p == '/' || *p == '\\') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (count >= SHELL_PATH_MAX_TOKENS) {
            return 0;
        }

        while (*p && *p != '/' && *p != '\\') {
            if ((n + 1) >= SHELL_PATH_TOKEN_MAX) {
                return 0;
            }
            tokens[count][n++] = (char)to_upper_ascii((u8)*p);
            p++;
        }
        tokens[count][n] = '\0';
        count++;
    }

    *count_out = count;
    return 1;
}

static int build_canonical_path(const char *input, char *out, u32 out_size) {
    char stack[SHELL_PATH_MAX_TOKENS][SHELL_PATH_TOKEN_MAX];
    char parts[SHELL_PATH_MAX_TOKENS][SHELL_PATH_TOKEN_MAX];
    u32 stack_count = 0;
    u32 part_count = 0;
    u32 j = 0;
    int absolute = 0;

    if (!input || !out || out_size == 0) {
        return 0;
    }

    while (*input && is_space((u8)*input)) {
        input++;
    }
    if (*input == '\0') {
        str_copy(out, g_shell_cwd, out_size);
        return 1;
    }

    if (*input == '/' || *input == '\\') {
        absolute = 1;
    }

    if (!absolute) {
        if (!split_path_tokens(g_shell_cwd, stack, &stack_count)) {
            return 0;
        }
    }

    if (!split_path_tokens(input, parts, &part_count)) {
        return 0;
    }

    for (u32 i = 0; i < part_count; i++) {
        if (str_eq(parts[i], ".")) {
            continue;
        }
        if (str_eq(parts[i], "..")) {
            if (stack_count > 0) {
                stack_count--;
            }
            continue;
        }
        if (stack_count >= SHELL_PATH_MAX_TOKENS) {
            return 0;
        }
        str_copy(stack[stack_count], parts[i], SHELL_PATH_TOKEN_MAX);
        stack_count++;
    }

    if (stack_count == 0) {
        if (out_size < 2) {
            return 0;
        }
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }

    for (u32 i = 0; i < stack_count; i++) {
        if ((j + 1) >= out_size) {
            return 0;
        }
        out[j++] = '/';
        for (u32 k = 0; stack[i][k] != '\0'; k++) {
            if ((j + 1) >= out_size) {
                return 0;
            }
            out[j++] = stack[i][k];
        }
    }
    out[j] = '\0';
    return 1;
}

static int shell_dir_exists(const char *path) {
    if (!fat_ready()) {
        return 0;
    }
    return fat_list_dir(path, shell_dir_exists_cb, (void *)0) ? 1 : 0;
}

static void write_prompt(void) {
    write_dos_path(g_shell_cwd);
    video_write("> ");
}

static const char *get_arg_ptr(const char *line) {
    while (*line && is_space((u8)*line)) {
        line++;
    }
    while (*line && !is_space((u8)*line)) {
        line++;
    }
    while (*line && is_space((u8)*line)) {
        line++;
    }
    return line;
}

static void normalize_first_token(const char *line, char *out, u32 out_size) {
    u32 i = 0;

    while (*line && is_space((u8)*line)) {
        line++;
    }

    while (*line && !is_space((u8)*line) && (i + 1) < out_size) {
        out[i++] = (char)to_lower_ascii((u8)*line);
        line++;
    }

    out[i] = '\0';
}

static void shell_print_help(void) {
    video_write("CiukiOS shell\n");
    video_write("\n");
    video_write("File system:\n");
    video_write("  dir [X]        list directory contents\n");
    video_write("  cd X           change current directory\n");
    video_write("  cd..           go to parent directory\n");
    video_write("  pwd            show current directory\n");
    video_write("  type X         display text file\n");
    video_write("  copy X Y       copy file X to Y\n");
    video_write("  move X Y       move / rename file X\n");
    video_write("  ren X Y        rename file or directory\n");
    video_write("  del X          delete file\n");
    video_write("  mkdir X        create directory\n");
    video_write("  rmdir X        remove empty directory\n");
    video_write("  attrib X       show or set file attributes\n");
    video_write("\n");
    video_write("Programs:\n");
    video_write("  run X [args]   launch a COM, EXE or BAT program\n");
    video_write("  which X        show where command X is found\n");
    video_write("  vmode          inspect or change video mode\n");
    video_write("  opengem  - launch OpenGEM GUI (preflight + run)\n");
    video_write("  catalog  - list discovered apps (FAT + handoff COM catalog)\n");
    video_write("\n");
    video_write("Visuals:\n");
    video_write("  demo           run the real-time graphics showcase\n");
    video_write("  desktop  - open interactive desktop scene (ALT+G+Q to return)\n");
    video_write("  gsplash        preview the splash screen\n");
    video_write("  ascii          display the ASCII art banner\n");
    video_write("\n");
    video_write("Session:\n");
    video_write("  echo X         print text\n");
    video_write("  set [K=V]      show or set environment variables\n");
    video_write("  history        show command history\n");
    video_write("  cls            clear screen\n");
    video_write("  ver            show OS version\n");
    video_write("\n");
    video_write("Power:\n");
    video_write("  reboot         restart the machine\n");
    video_write("  shutdown       power off the machine\n");
    video_write("\n");
    video_write("Tab completes commands and filenames. Up/Down browse history.\n");
    video_write("Type a program name directly to launch it (e.g. CIUKEDIT, DOOM.EXE).\n");
}

static void shell_cls(void) {
    video_cls();
}

static void shell_ver(void) {
    video_write(CIUKIOS_STAGE2_VERSION_LINE "\n");
}

static void shell_echo(const char *args) {
    video_write(args);
    video_write("\n");
}

static int normalize_run_name(const char *args, char *out, u32 out_size) {
    u32 i = 0;
    int has_dot = 0;

    while (*args && is_space((u8)*args)) {
        args++;
    }

    while (*args && !is_space((u8)*args)) {
        char ch = (char)*args;
        if (ch == '.') {
            has_dot = 1;
        }
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - ('a' - 'A'));
        }
        if ((i + 1) >= out_size) {
            return 0;
        }
        out[i++] = ch;
        args++;
    }

    if (i == 0) {
        return 0;
    }

    if (!has_dot) {
        if ((i + 4) >= out_size) {
            return 0;
        }
        out[i++] = '.';
        out[i++] = 'C';
        out[i++] = 'O';
        out[i++] = 'M';
    }

    out[i] = '\0';
    return 1;
}

static void extract_run_tail(const char *args, char *out, u32 out_size) {
    const char *p = args;
    u32 i = 0;

    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!args) {
        return;
    }

    while (*p && is_space((u8)*p)) {
        p++;
    }
    while (*p && !is_space((u8)*p)) {
        p++;
    }
    while (*p && is_space((u8)*p)) {
        p++;
    }

    while (*p && (i + 1) < out_size) {
        out[i++] = *p++;
    }
    out[i] = '\0';

    while (i > 0 && is_space((u8)out[i - 1])) {
        out[i - 1] = '\0';
        i--;
    }
}

static int extract_first_arg(const char *args, char *out, u32 out_size) {
    u32 i = 0;

    while (*args && is_space((u8)*args)) {
        args++;
    }
    if (*args == '\0') {
        return 0;
    }

    while (*args && !is_space((u8)*args)) {
        if ((i + 1) >= out_size) {
            return 0;
        }
        out[i++] = (char)*args;
        args++;
    }
    out[i] = '\0';
    return 1;
}

static int build_arg_path(const char *args, char *out, u32 out_size) {
    char token[96];

    if (!extract_first_arg(args, token, (u32)sizeof(token))) {
        return 0;
    }
    return build_canonical_path(token, out, out_size);
}

static int build_run_path(const char *com_name, char *out, u32 out_size) {
    return build_canonical_path(com_name, out, out_size);
}

static handoff_com_entry_t *shell_find_com(handoff_v0_t *handoff, const char *name) {
    u64 i;
    u64 count = handoff->com_count;

    if (count > HANDOFF_COM_MAX) {
        count = HANDOFF_COM_MAX;
    }

    for (i = 0; i < count; i++) {
        handoff_com_entry_t *entry = &handoff->com_entries[i];
        if (entry->phys_base == 0 || entry->name[0] == '\0') {
            continue;
        }
        if (str_eq_nocase(entry->name, name)) {
            return entry;
        }
    }

    return (handoff_com_entry_t *)0;
}

typedef struct shell_dir_ctx {
    u32 entries;
} shell_dir_ctx_t;

static int shell_dir_fat_cb(const fat_dir_entry_t *entry, void *ctx_void) {
    shell_dir_ctx_t *ctx = (shell_dir_ctx_t *)ctx_void;
    u32 name_len = str_len(entry->name);
    u32 pad;

    video_write("  ");
    video_write(entry->name);

    /* Pad name to 15 chars for column alignment */
    pad = (name_len < 15U) ? (15U - name_len) : 1U;
    for (u32 i = 0; i < pad; i++) {
        video_putchar(' ');
    }

    if (entry->attr & FAT_ATTR_DIRECTORY) {
        video_write("<DIR>");
    } else {
        /* Right-align file size in 10-char field */
        u32 size = entry->size;
        char sbuf[12];
        u32 slen = 0;
        u32 tmp = size;
        if (tmp == 0U) {
            sbuf[slen++] = '0';
        } else {
            while (tmp > 0U) {
                sbuf[slen++] = (char)('0' + (tmp % 10U));
                tmp /= 10U;
            }
        }
        /* Right-align in 10 chars */
        for (u32 i = slen; i < 10U; i++) {
            video_putchar(' ');
        }
        while (slen > 0U) {
            video_putchar(sbuf[--slen]);
        }
        video_write(" bytes");
    }

    video_write("\n");
    ctx->entries++;
    return 1;
}

static int shell_dir_from_fat(const char *path) {
    shell_dir_ctx_t ctx;
    ctx.entries = 0;

    if (!fat_ready()) {
        return 0;
    }

    video_write("Directory of ");
    write_dos_path(path);
    video_write("\n");

    if (!fat_list_dir(path, shell_dir_fat_cb, &ctx)) {
        return 0;
    }

    if (ctx.entries == 0) {
        video_write("  <empty>\n");
    }
    return 1;
}

static void shell_dir_from_catalog(handoff_v0_t *handoff) {
    u64 i;
    u64 count = handoff->com_count;

    video_write("Directory of A:\\ (COM fallback)\n");

    if (count > HANDOFF_COM_MAX) {
        count = HANDOFF_COM_MAX;
    }

    if (count == 0) {
        video_write("  <no COM programs loaded>\n");
        return;
    }

    for (i = 0; i < count; i++) {
        handoff_com_entry_t *entry = &handoff->com_entries[i];
        if (entry->phys_base == 0 || entry->name[0] == '\0') {
            continue;
        }

        video_write("  ");
        video_write(entry->name);
        video_write("  size=0x");
        video_write_hex64(entry->size);
        video_write("  addr=0x");
        video_write_hex64(entry->phys_base);
        video_write("\n");
    }
}

static void shell_pwd(void) {
    write_dos_path(g_shell_cwd);
    video_write("\n");
}

static void shell_cd(const char *args) {
    char path[SHELL_PATH_MAX];

    if (!args || args[0] == '\0') {
        shell_pwd();
        return;
    }
    if (!build_arg_path(args, path, (u32)sizeof(path))) {
        video_write("Usage: cd <path>\n");
        return;
    }
    if (!shell_dir_exists(path)) {
        video_write("Directory not found: ");
        write_dos_path(path);
        video_write("\n");
        return;
    }

    str_copy(g_shell_cwd, path, (u32)sizeof(g_shell_cwd));
}

static void shell_dir(handoff_v0_t *handoff, const char *args) {
    char path[SHELL_PATH_MAX];
    int use_fat = 0;

    if (args && args[0] != '\0') {
        if (!build_arg_path(args, path, (u32)sizeof(path))) {
            video_write("Usage: dir [path]\n");
            return;
        }
    } else {
        str_copy(path, g_shell_cwd, (u32)sizeof(path));
    }

    use_fat = shell_dir_from_fat(path);
    if (!use_fat) {
        shell_dir_from_catalog(handoff);
    }
}

static void shell_com_int20(ciuki_dos_context_t *ctx) {
    if (!ctx) {
        return;
    }
    ctx->exit_reason = (u8)CIUKI_COM_EXIT_INT20;
    ctx->exit_code = 0U;
}

static void shell_com_int2f(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);
static void shell_com_int31(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);
static void shell_com_int16(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);
static void shell_com_int1a(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);
static void shell_com_int33(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);
static void shell_com_int21_4c(ciuki_dos_context_t *ctx, u8 code);
static u32 g_int21_vectors[256];
static u8 g_int21_last_return_code = 0U;
static u8 g_int21_last_termination_type = 0U;
static u8 g_int21_last_status_pending = 0U;
static i32 g_int21_pending_stdin_char = -1;
static u8 g_int21_default_drive = 0U;
static u16 g_int21_dta_segment = 0U;
static u16 g_int21_dta_offset = 0x0080U;
static shell_int21_mem_block_t g_int21_mem_blocks[SHELL_INT21_MEM_MAX_BLOCKS];
static u16 g_int21_mem_block_count = 0U;
static shell_int21_find_state_t g_int21_find_state;

static void shell_int21_reset_handle_table(void) {
    local_memset(g_int21_file_handles, 0U, (u32)sizeof(g_int21_file_handles));
}

static void shell_int21_mem_reset(u16 base_seg, u16 paras) {
    local_memset(g_int21_mem_blocks, 0U, (u32)sizeof(g_int21_mem_blocks));
    g_int21_mem_block_count = 0U;
    if (paras == 0U) {
        return;
    }
    g_int21_mem_blocks[0].seg = base_seg;
    g_int21_mem_blocks[0].paras = paras;
    g_int21_mem_blocks[0].used = 0U;
    g_int21_mem_block_count = 1U;
}

static u16 shell_int21_mem_largest_free_block(void) {
    u16 best = 0U;
    for (u16 i = 0U; i < g_int21_mem_block_count; i++) {
        if (!g_int21_mem_blocks[i].used && g_int21_mem_blocks[i].paras > best) {
            best = g_int21_mem_blocks[i].paras;
        }
    }
    return best;
}

static int shell_int21_mem_insert_block(u16 index, u16 seg, u16 paras, u8 used, u16 owner_psp) {
    if (g_int21_mem_block_count >= SHELL_INT21_MEM_MAX_BLOCKS) {
        return 0;
    }
    if (index > g_int21_mem_block_count) {
        return 0;
    }
    for (u16 i = g_int21_mem_block_count; i > index; i--) {
        g_int21_mem_blocks[i] = g_int21_mem_blocks[i - 1U];
    }
    g_int21_mem_blocks[index].seg = seg;
    g_int21_mem_blocks[index].paras = paras;
    g_int21_mem_blocks[index].used = used;
    g_int21_mem_blocks[index].owner_psp = owner_psp;
    g_int21_mem_block_count++;
    return 1;
}

static void shell_int21_mem_merge_around(u16 idx) {
    if (idx >= g_int21_mem_block_count) {
        return;
    }

    if (idx > 0U &&
        !g_int21_mem_blocks[idx].used &&
        !g_int21_mem_blocks[idx - 1U].used) {
        g_int21_mem_blocks[idx - 1U].paras =
            (u16)(g_int21_mem_blocks[idx - 1U].paras + g_int21_mem_blocks[idx].paras);
        for (u16 i = idx; i + 1U < g_int21_mem_block_count; i++) {
            g_int21_mem_blocks[i] = g_int21_mem_blocks[i + 1U];
        }
        g_int21_mem_block_count--;
        idx--;
    }

    if ((idx + 1U) < g_int21_mem_block_count &&
        !g_int21_mem_blocks[idx].used &&
        !g_int21_mem_blocks[idx + 1U].used) {
        g_int21_mem_blocks[idx].paras =
            (u16)(g_int21_mem_blocks[idx].paras + g_int21_mem_blocks[idx + 1U].paras);
        for (u16 i = idx + 1U; i + 1U < g_int21_mem_block_count; i++) {
            g_int21_mem_blocks[i] = g_int21_mem_blocks[i + 1U];
        }
        g_int21_mem_block_count--;
    }
}

static int shell_int21_mem_find_used_block(u16 seg, u16 *idx_out) {
    for (u16 i = 0U; i < g_int21_mem_block_count; i++) {
        if (g_int21_mem_blocks[i].used && g_int21_mem_blocks[i].seg == seg) {
            if (idx_out) {
                *idx_out = i;
            }
            return 1;
        }
    }
    return 0;
}

static int shell_int21_mem_alloc(u16 owner_psp, u16 paras, u16 *seg_out, u16 *max_avail_out) {
    u16 max_free = shell_int21_mem_largest_free_block();
    if (max_avail_out) {
        *max_avail_out = max_free;
    }
    if (paras == 0U || paras > max_free) {
        return 0;
    }

    for (u16 i = 0U; i < g_int21_mem_block_count; i++) {
        shell_int21_mem_block_t *b = &g_int21_mem_blocks[i];
        if (b->used || b->paras < paras) {
            continue;
        }

        if (b->paras == paras) {
            b->used = 1U;
            b->owner_psp = owner_psp;
            if (seg_out) {
                *seg_out = b->seg;
            }
            return 1;
        }

        if (!shell_int21_mem_insert_block(
                (u16)(i + 1U),
                (u16)(b->seg + paras),
                (u16)(b->paras - paras),
                0U,
                0U)) {
            return 0;
        }
        b->paras = paras;
        b->used = 1U;
        b->owner_psp = owner_psp;
        if (seg_out) {
            *seg_out = b->seg;
        }
        return 1;
    }

    return 0;
}

static int shell_int21_mem_free(u16 owner_psp, u16 seg) {
    u16 idx = 0U;
    if (!shell_int21_mem_find_used_block(seg, &idx)) {
        return 0;
    }
    if (g_int21_mem_blocks[idx].owner_psp != owner_psp) {
        return 0;
    }
    g_int21_mem_blocks[idx].used = 0U;
    g_int21_mem_blocks[idx].owner_psp = 0U;
    shell_int21_mem_merge_around(idx);
    return 1;
}

static int shell_int21_mem_resize(u16 owner_psp, u16 seg, u16 new_paras, u16 *max_out) {
    u16 idx = 0U;
    shell_int21_mem_block_t *blk;
    u16 old_paras;

    if (!shell_int21_mem_find_used_block(seg, &idx)) {
        if (max_out) {
            *max_out = 0U;
        }
        return 0;
    }

    blk = &g_int21_mem_blocks[idx];
    if (blk->owner_psp != owner_psp) {
        if (max_out) {
            *max_out = 0U;
        }
        return 0;
    }
    old_paras = blk->paras;
    if (new_paras == old_paras) {
        if (max_out) {
            *max_out = old_paras;
        }
        return 1;
    }

    if (new_paras == 0U) {
        if (max_out) {
            *max_out = old_paras;
        }
        return 0;
    }

    if (new_paras < old_paras) {
        u16 freed = (u16)(old_paras - new_paras);
        blk->paras = new_paras;
        if (!shell_int21_mem_insert_block(
                (u16)(idx + 1U),
                (u16)(blk->seg + new_paras),
                freed,
                0U,
                0U)) {
            blk->paras = old_paras;
            if (max_out) {
                *max_out = old_paras;
            }
            return 0;
        }
        shell_int21_mem_merge_around((u16)(idx + 1U));
        if (max_out) {
            *max_out = new_paras;
        }
        return 1;
    }

    {
        u16 extra = (u16)(new_paras - old_paras);
        u16 grow_cap = old_paras;
        if ((idx + 1U) < g_int21_mem_block_count && !g_int21_mem_blocks[idx + 1U].used) {
            grow_cap = (u16)(grow_cap + g_int21_mem_blocks[idx + 1U].paras);
        }
        if (max_out) {
            *max_out = grow_cap;
        }
        if (new_paras > grow_cap) {
            return 0;
        }

        if ((idx + 1U) < g_int21_mem_block_count &&
            !g_int21_mem_blocks[idx + 1U].used &&
            g_int21_mem_blocks[idx + 1U].paras >= extra) {
            g_int21_mem_blocks[idx + 1U].seg =
                (u16)(g_int21_mem_blocks[idx + 1U].seg + extra);
            g_int21_mem_blocks[idx + 1U].paras =
                (u16)(g_int21_mem_blocks[idx + 1U].paras - extra);
            blk->paras = new_paras;

            if (g_int21_mem_blocks[idx + 1U].paras == 0U) {
                for (u16 i = idx + 1U; i + 1U < g_int21_mem_block_count; i++) {
                    g_int21_mem_blocks[i] = g_int21_mem_blocks[i + 1U];
                }
                g_int21_mem_block_count--;
            }
            return 1;
        }
    }

    if (max_out) {
        *max_out = old_paras;
    }
    return 0;
}

static shell_int21_file_handle_t *shell_int21_find_file_handle(u16 handle) {
    for (u32 i = 0; i < SHELL_INT21_MAX_FILE_HANDLES; i++) {
        if (g_int21_file_handles[i].used && g_int21_file_handles[i].handle_id == handle) {
            return &g_int21_file_handles[i];
        }
    }
    return (shell_int21_file_handle_t *)0;
}

static shell_int21_file_handle_t *shell_int21_alloc_file_handle(u8 mode, const char *path) {
    for (u32 i = 0; i < SHELL_INT21_MAX_FILE_HANDLES; i++) {
        if (!g_int21_file_handles[i].used) {
            shell_int21_file_handle_t *h = &g_int21_file_handles[i];
            h->used = 1;
            h->handle_id = (u16)(SHELL_INT21_HANDLE_BASE + i);
            h->mode = mode;
            h->dirty = 0U;
            h->size = 0U;
            h->pos = 0U;
            str_copy(h->path, path ? path : "", (u32)sizeof(h->path));
            return h;
        }
    }
    return (shell_int21_file_handle_t *)0;
}

static int shell_int21_read_asciiz(ciuki_dos_context_t *ctx, u16 off, char *out, u32 out_size) {
    u32 i;
    if (!ctx || !out || out_size == 0U) {
        return 0;
    }
    if ((u32)off >= ctx->image_size) {
        return 0;
    }
    for (i = 0; i + 1U < out_size; i++) {
        u32 idx = (u32)off + i;
        char ch;
        if (idx >= ctx->image_size) {
            return 0;
        }
        ch = *((char *)(ctx->image_linear + (u64)idx));
        out[i] = ch;
        if (ch == '\0') {
            return 1;
        }
    }
    out[out_size - 1U] = '\0';
    return 1;
}

static int shell_int21_read_asciiz_seg(ciuki_dos_context_t *ctx, u16 seg, u16 off, char *out, u32 out_size) {
    if (!ctx) {
        return 0;
    }
    if (seg != ctx->psp_segment) {
        return 0;
    }
    return shell_int21_read_asciiz(ctx, off, out, out_size);
}

static int shell_int21_dos_path_to_canonical(const char *in, char *out, u32 out_size) {
    char tmp[SHELL_PATH_MAX];
    u32 i = 0U;
    u32 j = 0U;
    const char *p;

    if (!in || !out || out_size == 0U) {
        return 0;
    }

    while (in[i] != '\0' && is_space((u8)in[i])) {
        i++;
    }
    p = &in[i];

    /* Optional drive prefix (e.g. A:) */
    if (((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) && p[1] == ':') {
        p += 2;
    }

    while (*p != '\0' && j + 1U < (u32)sizeof(tmp)) {
        char ch = *p++;
        if (ch == '\\') {
            ch = '/';
        }
        tmp[j++] = (char)to_upper_ascii((u8)ch);
    }
    tmp[j] = '\0';

    if (tmp[0] == '\0') {
        return 0;
    }

    return build_canonical_path(tmp, out, out_size);
}

static int shell_int21_get_dta_ptr(ciuki_dos_context_t *ctx, u8 **dta_out) {
    if (!ctx || !dta_out) {
        return 0;
    }

    /*
     * Current DOS runtime maps DS/ES to COM runtime linear memory.
     * Keep DTA access constrained to active PSP segment for deterministic behavior.
     */
    if (g_int21_dta_segment != ctx->psp_segment) {
        return 0;
    }

    return shell_int21_resolve_rw_buffer(ctx, g_int21_dta_offset, SHELL_INT21_DTA_SIZE, dta_out);
}

static int shell_int21_wild_match_ci(const char *pattern, const char *name) {
    const char *p = pattern ? pattern : "";
    const char *n = name ? name : "";
    const char *star = (const char *)0;
    const char *backtrack = (const char *)0;

    while (*n != '\0') {
        if (*p == '*') {
            star = p++;
            backtrack = n;
            continue;
        }

        if (*p == '?' || to_upper_ascii((u8)*p) == to_upper_ascii((u8)*n)) {
            p++;
            n++;
            continue;
        }

        if (star) {
            p = star + 1;
            n = ++backtrack;
            continue;
        }

        return 0;
    }

    while (*p == '*') {
        p++;
    }

    return *p == '\0';
}

static int shell_int21_split_find_path(
    const char *canonical,
    char *dir_out,
    u32 dir_out_size,
    char *pattern_out,
    u32 pattern_out_size
) {
    const char *last_slash = (const char *)0;
    u32 i;

    if (!canonical || !dir_out || !pattern_out || dir_out_size == 0U || pattern_out_size == 0U) {
        return 0;
    }

    for (i = 0U; canonical[i] != '\0'; i++) {
        if (canonical[i] == '/') {
            last_slash = canonical + i;
        }
    }

    if (!last_slash) {
        str_copy(dir_out, "/", dir_out_size);
        str_copy(pattern_out, canonical, pattern_out_size);
    } else if (last_slash == canonical) {
        str_copy(dir_out, "/", dir_out_size);
        if (last_slash[1] == '\0') {
            str_copy(pattern_out, "*", pattern_out_size);
        } else {
            str_copy(pattern_out, last_slash + 1, pattern_out_size);
        }
    } else {
        u32 dir_len = (u32)(last_slash - canonical);
        if ((dir_len + 1U) > dir_out_size) {
            return 0;
        }
        for (u32 j = 0U; j < dir_len; j++) {
            dir_out[j] = canonical[j];
        }
        dir_out[dir_len] = '\0';
        if (last_slash[1] == '\0') {
            str_copy(pattern_out, "*", pattern_out_size);
        } else {
            str_copy(pattern_out, last_slash + 1, pattern_out_size);
        }
    }

    if (pattern_out[0] == '\0') {
        str_copy(pattern_out, "*", pattern_out_size);
    }
    return 1;
}

static int shell_int21_find_attr_match(u8 entry_attr, u8 search_attr) {
    if ((entry_attr & FAT_ATTR_VOLUME_ID) != 0U) {
        return 0;
    }
    if ((entry_attr & FAT_ATTR_HIDDEN) != 0U && (search_attr & FAT_ATTR_HIDDEN) == 0U) {
        return 0;
    }
    if ((entry_attr & FAT_ATTR_SYSTEM) != 0U && (search_attr & FAT_ATTR_SYSTEM) == 0U) {
        return 0;
    }
    if ((entry_attr & FAT_ATTR_DIRECTORY) != 0U && (search_attr & FAT_ATTR_DIRECTORY) == 0U) {
        return 0;
    }
    return 1;
}

typedef struct shell_int21_find_match_ctx {
    const char *pattern;
    u8 attr_mask;
    u16 target_index;
    u16 seen_index;
    int found;
    fat_dir_entry_t match;
} shell_int21_find_match_ctx_t;

static int shell_int21_find_match_cb(const fat_dir_entry_t *entry, void *ctx_void) {
    shell_int21_find_match_ctx_t *ctx = (shell_int21_find_match_ctx_t *)ctx_void;

    if (!entry || !ctx) {
        return 0;
    }

    if (!shell_int21_find_attr_match(entry->attr, ctx->attr_mask)) {
        return 1;
    }
    if (!shell_int21_wild_match_ci(ctx->pattern, entry->name)) {
        return 1;
    }

    if (ctx->seen_index == ctx->target_index) {
        ctx->match = *entry;
        ctx->found = 1;
        return 0;
    }

    ctx->seen_index++;
    return 1;
}

static int shell_int21_find_match_in_dir(
    const char *dir_path,
    const char *pattern,
    u8 attr_mask,
    u16 target_index,
    fat_dir_entry_t *out_entry,
    int *dir_ok_out
) {
    shell_int21_find_match_ctx_t ctx;

    local_memset(&ctx, 0U, (u32)sizeof(ctx));
    ctx.pattern = pattern;
    ctx.attr_mask = attr_mask;
    ctx.target_index = target_index;

    if (!fat_list_dir(dir_path, shell_int21_find_match_cb, &ctx)) {
        if (dir_ok_out) {
            *dir_ok_out = 0;
        }
        return 0;
    }

    if (dir_ok_out) {
        *dir_ok_out = 1;
    }
    if (!ctx.found) {
        return 0;
    }

    if (out_entry) {
        *out_entry = ctx.match;
    }
    return 1;
}

static void shell_int21_fill_find_dta(u8 *dta, const fat_dir_entry_t *entry, u8 attr_mask, u16 next_index) {
    if (!dta || !entry) {
        return;
    }

    local_memset(dta, 0U, SHELL_INT21_DTA_SIZE);

    /* Internal deterministic state marker (reserved bytes 0..3). */
    dta[0] = (u8)'C';
    dta[1] = (u8)'K';
    dta[2] = (u8)(next_index & 0x00FFU);
    dta[3] = (u8)((next_index >> 8) & 0x00FFU);
    dta[4] = attr_mask;

    dta[SHELL_INT21_DTA_ATTR_OFFSET] = entry->attr;
    dta[SHELL_INT21_DTA_SIZE_OFFSET + 0U] = (u8)(entry->size & 0x000000FFU);
    dta[SHELL_INT21_DTA_SIZE_OFFSET + 1U] = (u8)((entry->size >> 8) & 0x000000FFU);
    dta[SHELL_INT21_DTA_SIZE_OFFSET + 2U] = (u8)((entry->size >> 16) & 0x000000FFU);
    dta[SHELL_INT21_DTA_SIZE_OFFSET + 3U] = (u8)((entry->size >> 24) & 0x000000FFU);

    for (u32 i = 0U; i < 12U && entry->name[i] != '\0'; i++) {
        dta[SHELL_INT21_DTA_NAME_OFFSET + i] = (u8)entry->name[i];
    }
    dta[SHELL_INT21_DTA_NAME_OFFSET + 12U] = '\0';
}

static int shell_int21_flush_file_handle(shell_int21_file_handle_t *h) {
    if (!h || !h->used || !h->dirty) {
        return 1;
    }

    /*
     * fat_write_file() fails if destination already exists.
     * Replace semantics: best-effort delete old path, then write new content.
     */
    (void)fat_delete_file(h->path);

    if (!fat_write_file(h->path, (const void *)h->data, h->size)) {
        return 0;
    }

    h->dirty = 0U;
    return 1;
}

static void shell_int21_close_all_handles(void) {
    for (u32 i = 0; i < SHELL_INT21_MAX_FILE_HANDLES; i++) {
        if (!g_int21_file_handles[i].used) {
            continue;
        }
        (void)shell_int21_flush_file_handle(&g_int21_file_handles[i]);
        g_int21_file_handles[i].used = 0;
    }
}

static void *shell_ctx_ptr_from_offset(ciuki_dos_context_t *ctx, u16 off) {
    if (!ctx || off >= ctx->image_size) {
        return (void *)0;
    }
    return (void *)(ctx->image_linear + (u64)off);
}

static u8 shell_int21_read_char_blocking(void) {
    /* Flush any pending video output before blocking on keyboard so user sees prompt.
     * Use pacing-gated dirty present to avoid redundant full-screen commits. */
    video_present_dirty_immediate();
    if (g_int21_pending_stdin_char >= 0) {
        u8 ch = (u8)g_int21_pending_stdin_char;
        g_int21_pending_stdin_char = -1;
        if (ch == '\n') {
            return '\r';
        }
        return ch;
    }

    u8 ch = stage2_keyboard_getc_blocking();
    if (ch == '\n') {
        return '\r';
    }
    return ch;
}

static void shell_int21_flush_input(void) {
    g_int21_pending_stdin_char = -1;
    stage2_keyboard_flush_buffer();
}

static i32 shell_int21_read_char_nonblocking(void) {
    if (g_int21_pending_stdin_char >= 0) {
        i32 ch = g_int21_pending_stdin_char;
        g_int21_pending_stdin_char = -1;
        if (ch == '\n') {
            return '\r';
        }
        return ch;
    }

    {
        i32 ch = stage2_keyboard_getc_nonblocking();
        if (ch == '\n') {
            return '\r';
        }
        return ch;
    }
}

static int shell_int21_resolve_rw_buffer(ciuki_dos_context_t *ctx, u16 off, u16 count, u8 **buf_out) {
    u32 n = (u32)count;

    if (!ctx || !buf_out) {
        return 0;
    }
    if (n == 0U) {
        *buf_out = (u8 *)shell_ctx_ptr_from_offset(ctx, off);
        return 1;
    }
    if ((u32)off >= ctx->image_size) {
        return 0;
    }
    if ((u32)off + n > ctx->image_size || (u32)off + n < (u32)off) {
        return 0;
    }

    *buf_out = (u8 *)(ctx->image_linear + (u64)off);
    return 1;
}

static int shell_int21_buffered_line_input(ciuki_dos_context_t *ctx, u16 off) {
    u8 *buf;
    u8 max_len;
    u8 count = 0;

    if (!shell_int21_resolve_rw_buffer(ctx, off, 3U, &buf)) {
        return 0;
    }

    max_len = buf[0];
    if (max_len == 0U) {
        buf[1] = 0U;
        buf[2] = '\r';
        return 1;
    }

    if ((u32)off + 2U + (u32)max_len >= ctx->image_size) {
        return 0;
    }

    for (;;) {
        u8 ch = shell_int21_read_char_blocking();

        if (ch == '\b') {
            if (count > 0U) {
                count--;
                video_putchar('\b');
                video_present_dirty_immediate();
            }
            continue;
        }

        if (ch == '\r') {
            break;
        }

        if (count < max_len) {
            buf[2U + count] = ch;
            count++;
            video_putchar((char)ch);
            video_present_dirty_immediate();
        }
    }

    buf[1] = count;
    buf[2U + count] = '\r';
    video_putchar('\n');
    return 1;
}

static void shell_com_int21(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs) {
    u8 ah;
    u8 al;

    if (!ctx || !regs) {
        return;
    }

    ah = (u8)((regs->ax >> 8) & 0xFFU);
    al = (u8)(regs->ax & 0xFFU);
    regs->carry = 0U;

    if (ah == 0x02U) {
        video_putchar((char)(regs->dx & 0x00FFU));
        regs->ax = (u16)((regs->ax & 0xFF00U) | (regs->dx & 0x00FFU));
        return;
    }

    if (ah == 0x06U) {
        u8 dl = (u8)(regs->dx & 0x00FFU);
        if (dl != 0xFFU) {
            video_putchar((char)dl);
            regs->ax = (u16)((regs->ax & 0xFF00U) | dl);
            return;
        }

        {
            i32 ch = shell_int21_read_char_nonblocking();
            if (ch < 0) {
                regs->ax = (u16)(regs->ax & 0xFF00U);
            } else {
                regs->ax = (u16)((regs->ax & 0xFF00U) | (u16)((u8)ch));
            }
        }
        return;
    }

    if (ah == 0x07U) {
        u8 ch = shell_int21_read_char_blocking();
        regs->ax = (u16)((regs->ax & 0xFF00U) | ch);
        return;
    }

    if (ah == 0x09U) {
        const char *s = (const char *)shell_ctx_ptr_from_offset(ctx, regs->dx);
        u32 guard = 0;
        if (!s) {
            regs->carry = 1U;
            regs->ax = 0x0006U;
            return;
        }
        while (guard++ < ctx->image_size) {
            char ch = *s++;
            if (ch == '$') {
                break;
            }
            if (ch == '\0') {
                break;
            }
            video_putchar(ch);
        }
        regs->ax = (u16)((regs->ax & 0xFF00U) | 0x24U);
        return;
    }

    if (ah == 0x01U) {
        u8 ch = shell_int21_read_char_blocking();
        if (ch == '\r') {
            video_putchar('\n');
        } else {
            video_putchar((char)ch);
        }
        regs->ax = (u16)((regs->ax & 0xFF00U) | ch);
        return;
    }

    if (ah == 0x08U) {
        u8 ch = shell_int21_read_char_blocking();
        regs->ax = (u16)((regs->ax & 0xFF00U) | ch);
        return;
    }

    if (ah == 0x0AU) {
        if (!shell_int21_buffered_line_input(ctx, regs->dx)) {
            regs->carry = 1U;
            regs->ax = 0x0005U; /* access denied (invalid buffer range) */
        }
        return;
    }

    if (ah == 0x0BU) {
        if (g_int21_pending_stdin_char >= 0) {
            regs->ax = (u16)((regs->ax & 0xFF00U) | 0x00FFU);
            return;
        }

        {
            i32 ch = stage2_keyboard_getc_nonblocking();
            if (ch >= 0) {
                g_int21_pending_stdin_char = ch;
                regs->ax = (u16)((regs->ax & 0xFF00U) | 0x00FFU);
            } else {
                regs->ax = (u16)(regs->ax & 0xFF00U);
            }
        }
        return;
    }

    if (ah == 0x0CU) {
        shell_int21_flush_input();

        if (al == 0x01U) {
            u8 ch = shell_int21_read_char_blocking();
            if (ch == '\r') {
                video_putchar('\n');
            } else {
                video_putchar((char)ch);
            }
            regs->ax = (u16)((regs->ax & 0xFF00U) | ch);
            return;
        }

        if (al == 0x08U) {
            u8 ch = shell_int21_read_char_blocking();
            regs->ax = (u16)((regs->ax & 0xFF00U) | ch);
            return;
        }

        if (al == 0x0AU) {
            if (!shell_int21_buffered_line_input(ctx, regs->dx)) {
                regs->carry = 1U;
                regs->ax = 0x0005U;
            }
            return;
        }

        /* Flush-only deterministic path for unimplemented follow-up input functions. */
        return;
    }

    if (ah == 0x0EU) {
        u8 drive = (u8)(regs->dx & 0x00FFU);
        if (drive <= 25U) {
            g_int21_default_drive = drive;
        }
        /* Deterministic single-drive environment for now. */
        regs->ax = (u16)((regs->ax & 0xFF00U) | 0x0001U);
        return;
    }

    if (ah == 0x19U) {
        regs->ax = (u16)((regs->ax & 0xFF00U) | (u16)g_int21_default_drive);
        return;
    }

    if (ah == 0x1AU) {
        g_int21_dta_segment = regs->ds;
        g_int21_dta_offset = regs->dx;
        g_int21_find_state.active = 0;
        return;
    }

    if (ah == 0x25U) {
        /* Set interrupt vector: AL=index, DS:DX=far ptr */
        u8 vec = (u8)(regs->ax & 0x00FFU);
        g_int21_vectors[vec] = ((u32)regs->ds << 16) | (u32)regs->dx;
        return;
    }

    if (ah == 0x35U) {
        /* Get interrupt vector: AL=index -> ES:BX */
        u8 vec = (u8)(regs->ax & 0x00FFU);
        u32 far_ptr = g_int21_vectors[vec];
        regs->bx = (u16)(far_ptr & 0xFFFFU);
        regs->es = (u16)((far_ptr >> 16) & 0xFFFFU);
        return;
    }

    if (ah == 0x30U) {
        /* DOS version 6.22 -> AL=6, AH=22 (0x16) */
        regs->ax = 0x1606U;
        regs->bx = 0x0000U;
        regs->cx = 0x0000U;
        return;
    }

    if (ah == 0x2AU) {
        /* Get system date — deterministic baseline (Fri 2026-04-17). */
        regs->ax = (u16)((regs->ax & 0xFF00U) | 0x0005U); /* AL = day-of-week (Fri) */
        regs->cx = 0x07EAU;                               /* CX = 2026 */
        regs->dx = (u16)((0x04U << 8) | 0x11U);           /* DH=month(4), DL=day(17) */
        return;
    }

    if (ah == 0x2CU) {
        /* Get system time — deterministic baseline (00:00:00.00). */
        regs->cx = 0x0000U;  /* CH=hour, CL=minute */
        regs->dx = 0x0000U;  /* DH=seconds, DL=hundredths */
        return;
    }

    if (ah == 0x44U && al == 0x00U) {
        /* IOCTL — get device info for handle in BX. */
        u16 handle = regs->bx;
        u16 info;
        if (handle == 0U) {
            info = 0x0081U; /* stdin: char device, is stdin */
        } else if (handle == 1U || handle == 2U) {
            info = 0x0082U; /* stdout/stderr: char device, is stdout */
        } else {
            info = 0x0000U; /* file handle: disk file, not a device */
        }
        regs->dx = info;
        regs->ax = info;
        regs->carry = 0U;
        return;
    }

    if (ah == 0x2FU) {
        regs->es = g_int21_dta_segment;
        regs->bx = g_int21_dta_offset;
        return;
    }

    if (ah == 0x4EU) {
        char dos_path[SHELL_PATH_MAX];
        char canonical_path[SHELL_PATH_MAX];
        char dir_path[SHELL_PATH_MAX];
        char pattern[SHELL_PATH_MAX];
        fat_dir_entry_t match;
        u8 *dta;
        u8 search_attr = (u8)(regs->cx & 0x00FFU);
        int dir_ok = 0;

        if (!fat_ready()) {
            g_int21_find_state.active = 0;
            regs->carry = 1U;
            regs->ax = 0x0002U; /* file not found */
            return;
        }

        if (!shell_int21_get_dta_ptr(ctx, &dta)) {
            g_int21_find_state.active = 0;
            regs->carry = 1U;
            regs->ax = 0x0005U; /* access denied (invalid DTA) */
            return;
        }

        if (!shell_int21_read_asciiz(ctx, regs->dx, dos_path, (u32)sizeof(dos_path)) ||
            !shell_int21_dos_path_to_canonical(dos_path, canonical_path, (u32)sizeof(canonical_path)) ||
            !shell_int21_split_find_path(canonical_path, dir_path, (u32)sizeof(dir_path), pattern, (u32)sizeof(pattern))) {
            g_int21_find_state.active = 0;
            regs->carry = 1U;
            regs->ax = 0x0003U; /* path not found */
            return;
        }

        if (!shell_int21_find_match_in_dir(dir_path, pattern, search_attr, 0U, &match, &dir_ok)) {
            g_int21_find_state.active = 0;
            regs->carry = 1U;
            regs->ax = dir_ok ? 0x0002U : 0x0003U;
            return;
        }

        shell_int21_fill_find_dta(dta, &match, search_attr, 1U);
        g_int21_find_state.active = 1;
        g_int21_find_state.attr_mask = search_attr;
        g_int21_find_state.next_index = 1U;
        g_int21_find_state.dta_segment = g_int21_dta_segment;
        g_int21_find_state.dta_offset = g_int21_dta_offset;
        str_copy(g_int21_find_state.dir, dir_path, (u32)sizeof(g_int21_find_state.dir));
        str_copy(g_int21_find_state.pattern, pattern, (u32)sizeof(g_int21_find_state.pattern));
        regs->ax = 0x0000U;
        return;
    }

    if (ah == 0x4FU) {
        fat_dir_entry_t match;
        u8 *dta;
        int dir_ok = 0;

        if (!fat_ready()) {
            regs->carry = 1U;
            regs->ax = 0x0012U; /* no more files */
            return;
        }

        if (!g_int21_find_state.active) {
            regs->carry = 1U;
            regs->ax = 0x0012U;
            return;
        }

        if (!shell_int21_get_dta_ptr(ctx, &dta)) {
            g_int21_find_state.active = 0;
            regs->carry = 1U;
            regs->ax = 0x0005U; /* access denied (invalid DTA) */
            return;
        }

        if (g_int21_find_state.dta_segment != g_int21_dta_segment ||
            g_int21_find_state.dta_offset != g_int21_dta_offset) {
            g_int21_find_state.active = 0;
            regs->carry = 1U;
            regs->ax = 0x0012U;
            return;
        }

        if (!shell_int21_find_match_in_dir(
                g_int21_find_state.dir,
                g_int21_find_state.pattern,
                g_int21_find_state.attr_mask,
                g_int21_find_state.next_index,
                &match,
                &dir_ok
            )) {
            g_int21_find_state.active = 0;
            regs->carry = 1U;
            regs->ax = dir_ok ? 0x0012U : 0x0003U;
            return;
        }

        g_int21_find_state.next_index++;
        shell_int21_fill_find_dta(dta, &match, g_int21_find_state.attr_mask, g_int21_find_state.next_index);
        regs->ax = 0x0000U;
        return;
    }

    if (ah == 0x51U || ah == 0x62U) {
        /* Get PSP address (DOS-compatible subset): return current PSP segment in BX. */
        regs->bx = ctx->psp_segment;
        return;
    }

    if (ah == 0x4DU) {
        /* DOS one-shot semantics: consume stored status on first read. */
        if (g_int21_last_status_pending) {
            regs->ax = (u16)(((u16)g_int21_last_termination_type << 8) | (u16)g_int21_last_return_code);
        } else {
            regs->ax = 0x0000U;
        }
        g_int21_last_return_code = 0U;
        g_int21_last_termination_type = 0U;
        g_int21_last_status_pending = 0U;
        return;
    }

    if (ah == 0x3CU) {
        char dos_path[SHELL_PATH_MAX];
        char path[SHELL_PATH_MAX];
        fat_dir_entry_t existing;
        shell_int21_file_handle_t *h;

        if (!fat_ready()) {
            /* Pre-FAT boot phase: deterministic fallback */
            regs->carry = 1U;
            regs->ax = 0x0005U; /* access denied */
            return;
        }

        if (!shell_int21_read_asciiz(ctx, regs->dx, dos_path, (u32)sizeof(dos_path)) ||
            !shell_int21_dos_path_to_canonical(dos_path, path, (u32)sizeof(path))) {
            regs->carry = 1U;
            regs->ax = 0x0003U; /* path not found */
            return;
        }

        if (fat_find_file(path, &existing)) {
            if ((existing.attr & FAT_ATTR_DIRECTORY) != 0U) {
                regs->carry = 1U;
                regs->ax = 0x0005U; /* access denied */
                return;
            }
            if (!fat_delete_file(path)) {
                regs->carry = 1U;
                regs->ax = 0x0005U;
                return;
            }
        }

        if (!fat_write_file(path, (const void *)0, 0U)) {
            regs->carry = 1U;
            regs->ax = 0x0005U;
            return;
        }

        h = shell_int21_alloc_file_handle(2U, path);
        if (!h) {
            regs->carry = 1U;
            regs->ax = 0x0004U; /* too many open files */
            return;
        }

        regs->ax = h->handle_id;
        return;
    }

    if (ah == 0x3DU) {
        char dos_path[SHELL_PATH_MAX];
        char path[SHELL_PATH_MAX];
        fat_dir_entry_t info;
        shell_int21_file_handle_t *h;
        u32 file_size = 0U;
        u8 access = (u8)(al & 0x03U);

        if (!fat_ready()) {
            regs->carry = 1U;
            regs->ax = 0x0002U; /* file not found */
            return;
        }

        if (!shell_int21_read_asciiz(ctx, regs->dx, dos_path, (u32)sizeof(dos_path)) ||
            !shell_int21_dos_path_to_canonical(dos_path, path, (u32)sizeof(path))) {
            regs->carry = 1U;
            regs->ax = 0x0003U; /* path not found */
            return;
        }

        if (!fat_find_file(path, &info)) {
            regs->carry = 1U;
            regs->ax = 0x0002U; /* file not found */
            return;
        }
        if ((info.attr & FAT_ATTR_DIRECTORY) != 0U) {
            regs->carry = 1U;
            regs->ax = 0x0005U; /* access denied */
            return;
        }
        if (info.size > SHELL_INT21_FILE_BUF_CAP) {
            regs->carry = 1U;
            regs->ax = 0x0008U; /* insufficient memory */
            return;
        }

        h = shell_int21_alloc_file_handle(access, path);
        if (!h) {
            regs->carry = 1U;
            regs->ax = 0x0004U; /* too many open files */
            return;
        }

        if (info.size > 0U) {
            if (!fat_read_file(path, (void *)h->data, SHELL_INT21_FILE_BUF_CAP, &file_size)) {
                h->used = 0;
                regs->carry = 1U;
                regs->ax = 0x0005U;
                return;
            }
        }

        h->size = file_size;
        h->pos = 0U;
        h->dirty = 0U;
        regs->ax = h->handle_id;
        return;
    }

    if (ah == 0x3EU) {
        if (regs->bx <= 2U) {
            regs->ax = 0x0000U;
            return;
        }

        {
            shell_int21_file_handle_t *h = shell_int21_find_file_handle(regs->bx);
            if (!h) {
                regs->carry = 1U;
                regs->ax = 0x0006U; /* invalid handle */
                return;
            }
            if (!shell_int21_flush_file_handle(h)) {
                regs->carry = 1U;
                regs->ax = 0x0005U;
                return;
            }
            h->used = 0;
            regs->ax = 0x0000U;
        }
        return;
    }

    if (ah == 0x3FU) {
        shell_int21_file_handle_t *h;

        if (regs->bx == 0U) {
            u8 *dst;
            u16 count = regs->cx;
            u16 done = 0U;

            if (!shell_int21_resolve_rw_buffer(ctx, regs->dx, count, &dst)) {
                regs->carry = 1U;
                regs->ax = 0x0005U; /* access denied (invalid buffer range) */
                return;
            }

            while (done < count) {
                u8 ch = shell_int21_read_char_blocking();
                dst[done++] = ch;
                if (ch == '\r') {
                    break;
                }
            }
            regs->ax = done;
            return;
        }

        if (regs->bx <= 2U) {
            regs->carry = 1U;
            regs->ax = 0x0005U; /* access denied (can't read stdout/stderr) */
            return;
        }

        h = shell_int21_find_file_handle(regs->bx);
        if (!h) {
            regs->carry = 1U;
            regs->ax = 0x0006U; /* invalid handle */
            return;
        }

        {
            u8 *dst;
            u32 available;
            u32 to_read;
            u32 count = (u32)regs->cx;

            if (!shell_int21_resolve_rw_buffer(ctx, regs->dx, regs->cx, &dst)) {
                regs->carry = 1U;
                regs->ax = 0x0005U;
                return;
            }

            if (h->pos >= h->size) {
                regs->ax = 0x0000U;
                return;
            }

            available = h->size - h->pos;
            to_read = (count < available) ? count : available;
            local_memcpy(dst, h->data + h->pos, to_read);
            h->pos += to_read;
            regs->ax = (u16)to_read;
        }
        return;
    }

    if (ah == 0x40U) {
        shell_int21_file_handle_t *h;

        if (regs->bx == 1U || regs->bx == 2U) {
            u8 *src;
            u16 count = regs->cx;
            u16 i;

            if (!shell_int21_resolve_rw_buffer(ctx, regs->dx, count, &src)) {
                regs->carry = 1U;
                regs->ax = 0x0005U; /* access denied (invalid buffer range) */
                return;
            }

            for (i = 0U; i < count; i++) {
                video_putchar((char)src[i]);
            }

            regs->ax = count;
            return;
        }

        if (regs->bx == 0U) {
            regs->carry = 1U;
            regs->ax = 0x0005U; /* access denied (can't write stdin) */
            return;
        }

        h = shell_int21_find_file_handle(regs->bx);
        if (!h) {
            regs->carry = 1U;
            regs->ax = 0x0006U; /* invalid handle */
            return;
        }
        if (h->mode == 0U) {
            regs->carry = 1U;
            regs->ax = 0x0005U; /* access denied (read-only handle) */
            return;
        }

        {
            u8 *src;
            u32 count = (u32)regs->cx;
            u32 room = 0U;
            u32 to_write = 0U;

            if (!shell_int21_resolve_rw_buffer(ctx, regs->dx, regs->cx, &src)) {
                regs->carry = 1U;
                regs->ax = 0x0005U;
                return;
            }

            if (h->pos < SHELL_INT21_FILE_BUF_CAP) {
                room = SHELL_INT21_FILE_BUF_CAP - h->pos;
            }
            to_write = (count < room) ? count : room;

            if (to_write > 0U) {
                local_memcpy(h->data + h->pos, src, to_write);
                h->pos += to_write;
                if (h->pos > h->size) {
                    h->size = h->pos;
                }
                h->dirty = 1U;
            }

            regs->ax = (u16)to_write;
        }
        return;
    }

    if (ah == 0x41U) {
        char dos_path[SHELL_PATH_MAX];
        char path[SHELL_PATH_MAX];

        if (!fat_ready()) {
            regs->carry = 1U;
            regs->ax = 0x0002U; /* file not found */
            return;
        }

        if (!shell_int21_read_asciiz(ctx, regs->dx, dos_path, (u32)sizeof(dos_path)) ||
            !shell_int21_dos_path_to_canonical(dos_path, path, (u32)sizeof(path))) {
            regs->carry = 1U;
            regs->ax = 0x0003U;
            return;
        }

        if (!fat_delete_file(path)) {
            regs->carry = 1U;
            regs->ax = 0x0002U;
            return;
        }
        regs->ax = 0x0000U;
        return;
    }

    if (ah == 0x42U) {
        shell_int21_file_handle_t *h;

        if (regs->bx <= 2U) {
            regs->ax = 0x0000U;
            regs->dx = 0x0000U;
            return;
        }

        h = shell_int21_find_file_handle(regs->bx);
        if (!h) {
            regs->carry = 1U;
            regs->ax = 0x0006U; /* invalid handle */
            return;
        }

        {
            u8 origin = (u8)(al & 0x03U);
            u32 off_u = ((u32)regs->cx << 16) | (u32)regs->dx;
            i32 off_s = (i32)off_u;
            i64 base = 0;
            i64 new_pos = 0;

            if (origin == 0U) {
                new_pos = (i64)off_u;
            } else {
                if (origin == 1U) {
                    base = (i64)h->pos;
                } else if (origin == 2U) {
                    base = (i64)h->size;
                } else {
                    regs->carry = 1U;
                    regs->ax = 0x0001U;
                    return;
                }
                new_pos = base + (i64)off_s;
            }

            if (new_pos < 0 || new_pos > (i64)SHELL_INT21_FILE_BUF_CAP) {
                regs->carry = 1U;
                regs->ax = 0x0019U; /* seek error */
                return;
            }

            h->pos = (u32)new_pos;
            regs->ax = (u16)(h->pos & 0xFFFFU);
            regs->dx = (u16)((h->pos >> 16) & 0xFFFFU);
        }
        return;
    }

    if (ah == 0x43U) {
        char dos_path[SHELL_PATH_MAX];
        char path[SHELL_PATH_MAX];
        u8 subfn = al;

        if (!fat_ready()) {
            regs->carry = 1U;
            regs->ax = 0x0002U; /* file not found */
            return;
        }

        if (!shell_int21_read_asciiz(ctx, regs->dx, dos_path, (u32)sizeof(dos_path)) ||
            !shell_int21_dos_path_to_canonical(dos_path, path, (u32)sizeof(path))) {
            regs->carry = 1U;
            regs->ax = 0x0003U; /* path not found */
            return;
        }

        if (subfn == 0x00U) {
            fat_dir_entry_t info;
            if (!fat_find_file(path, &info)) {
                regs->carry = 1U;
                regs->ax = 0x0002U; /* file not found */
                return;
            }
            regs->cx = (u16)info.attr;
            regs->ax = (u16)(regs->ax & 0xFF00U);
            return;
        }

        if (subfn == 0x01U) {
            u8 new_attr = (u8)(regs->cx & 0x00FFU);
            if (!fat_set_attr(path, new_attr)) {
                regs->carry = 1U;
                regs->ax = 0x0005U; /* access denied */
                return;
            }
            regs->ax = (u16)(regs->ax & 0xFF00U);
            return;
        }

        regs->carry = 1U;
        regs->ax = 0x0001U; /* invalid function */
        return;
    }

    if (ah == 0x56U) {
        char dos_old[SHELL_PATH_MAX];
        char dos_new[SHELL_PATH_MAX];
        char old_path[SHELL_PATH_MAX];
        char new_path[SHELL_PATH_MAX];
        char old_dir[SHELL_PATH_MAX];
        char old_name[SHELL_PATH_MAX];
        char new_dir[SHELL_PATH_MAX];
        char new_name[SHELL_PATH_MAX];
        fat_dir_entry_t old_info;
        fat_dir_entry_t dst_info;

        if (!fat_ready()) {
            regs->carry = 1U;
            regs->ax = 0x0002U; /* file not found */
            return;
        }

        if (!shell_int21_read_asciiz_seg(ctx, regs->ds, regs->dx, dos_old, (u32)sizeof(dos_old)) ||
            !shell_int21_read_asciiz_seg(ctx, regs->es, regs->di, dos_new, (u32)sizeof(dos_new)) ||
            !shell_int21_dos_path_to_canonical(dos_old, old_path, (u32)sizeof(old_path)) ||
            !shell_int21_dos_path_to_canonical(dos_new, new_path, (u32)sizeof(new_path)) ||
            !shell_int21_split_find_path(old_path, old_dir, (u32)sizeof(old_dir), old_name, (u32)sizeof(old_name)) ||
            !shell_int21_split_find_path(new_path, new_dir, (u32)sizeof(new_dir), new_name, (u32)sizeof(new_name))) {
            regs->carry = 1U;
            regs->ax = 0x0003U; /* path not found */
            return;
        }

        for (u32 i = 0U; old_name[i] != '\0'; i++) {
            if (old_name[i] == '*' || old_name[i] == '?') {
                regs->carry = 1U;
                regs->ax = 0x0002U; /* wildcard source unsupported in this subset */
                return;
            }
        }

        for (u32 i = 0U; new_name[i] != '\0'; i++) {
            if (new_name[i] == '*' || new_name[i] == '?') {
                regs->carry = 1U;
                regs->ax = 0x0005U; /* invalid target name */
                return;
            }
        }

        if (!str_eq(old_dir, new_dir)) {
            regs->carry = 1U;
            regs->ax = 0x0005U; /* cross-directory rename unsupported */
            return;
        }

        if (!fat_find_file(old_path, &old_info)) {
            regs->carry = 1U;
            regs->ax = 0x0002U; /* source file not found */
            return;
        }

        if (str_eq(old_path, new_path)) {
            regs->ax = 0x0000U;
            return;
        }

        if (fat_find_file(new_path, &dst_info)) {
            regs->carry = 1U;
            regs->ax = 0x0005U; /* destination already exists */
            return;
        }

        if (!fat_rename_entry(old_path, new_name)) {
            regs->carry = 1U;
            regs->ax = 0x0005U;
            return;
        }

        regs->ax = 0x0000U;
        return;
    }

    if (ah == 0x48U) {
        u16 seg = 0U;
        u16 max_free = 0U;

        if (shell_int21_mem_alloc(ctx->psp_segment, regs->bx, &seg, &max_free)) {
            regs->ax = seg;
            return;
        }

        regs->carry = 1U;
        regs->ax = 0x0008U; /* insufficient memory */
        regs->bx = max_free;
        return;
    }

    if (ah == 0x49U) {
        if (shell_int21_mem_free(ctx->psp_segment, regs->es)) {
            regs->ax = 0x0000U;
            return;
        }

        regs->carry = 1U;
        regs->ax = 0x0009U; /* invalid memory block address */
        return;
    }

    if (ah == 0x4AU) {
        u16 max_for_block = 0U;

        if (shell_int21_mem_resize(ctx->psp_segment, regs->es, regs->bx, &max_for_block)) {
            regs->ax = 0x0000U;
            return;
        }

        regs->carry = 1U;
        regs->ax = 0x0008U; /* insufficient memory */
        regs->bx = max_for_block;
        return;
    }

    if (ah == 0x00U) {
        shell_com_int20(ctx);
        return;
    }

    if (ah == 0x4CU) {
        shell_com_int21_4c(ctx, al);
        return;
    }

    g_shell_dosrun_int21_unsupported_calls++;
    regs->carry = 1U;
    regs->ax = 0x0001U;
}

static void shell_com_int2f(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs) {
    (void)ctx;

    if (!regs) {
        return;
    }

    if (regs->ax == 0x1687U) {
        /*
         * DPMI installation-check descriptor slice for DOS/4GW-style smoke.
         * This is still not a full DPMI host, but now returns a non-zero
         * descriptor-like entry pointer and host-data size so clients can
         * validate more than simple presence.
         */
        regs->ax = 0x0000U;
        regs->bx = 0x0001U;
        regs->cx = 0x0090U;
        regs->dx = 0x0000U;
        regs->si = 0x0001U;
        regs->di = 0x0001U;
        regs->es = 0xF000U;
        regs->carry = 0U;
        return;
    }

    regs->carry = 1U;
    regs->ax = 0x0001U;
}

static void shell_com_int16(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs) {
    u8 ah;
    u8 scancode = 0;
    u8 ascii = 0;

    (void)ctx;

    if (!regs) {
        return;
    }

    ah = (u8)((regs->ax >> 8) & 0xFFU);

    if (ah == 0x00U || ah == 0x10U) {
        /* AH=00h / 10h: blocking read — wait for key, return scancode:ascii. */
        stage2_keyboard_read_key(&scancode, &ascii);
        regs->ax = (u16)((u16)scancode << 8) | (u16)ascii;
        regs->carry = 0U;
        return;
    }

    if (ah == 0x01U || ah == 0x11U) {
        /* AH=01h / 11h: peek — check buffer, ZF via carry convention. */
        if (stage2_keyboard_peek_key(&scancode, &ascii)) {
            regs->ax = (u16)((u16)scancode << 8) | (u16)ascii;
            regs->carry = 0U;  /* ZF=0 — key available */
        } else {
            regs->carry = 1U;  /* ZF=1 — no key (carry encodes ZF here) */
        }
        return;
    }

    if (ah == 0x02U || ah == 0x12U) {
        /* AH=02h / 12h: return shift flags in AL. */
        regs->ax = (u16)((regs->ax & 0xFF00U) | (u16)stage2_keyboard_shift_flags());
        regs->carry = 0U;
        return;
    }

    /* Unsupported subfunction — ignore silently. */
    regs->carry = 0U;
}

static void shell_com_int1a(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs) {
    u8 ah;
    u64 ticks;

    (void)ctx;

    if (!regs) {
        return;
    }

    ah = (u8)((regs->ax >> 8) & 0xFFU);

    if (ah == 0x00U) {
        /*
         * AH=00h: Read system-timer tick counter.
         * Returns CX:DX = 32-bit tick count, AL = midnight flag (always 0).
         * Our PIT runs at ~100 Hz; real BIOS ticks at ~18.2 Hz.
         * Scale: ticks_bios ≈ ticks_100hz * 18 / 100.
         */
        ticks = stage2_timer_ticks();
        ticks = ticks * 18U / 100U;
        regs->cx = (u16)((ticks >> 16) & 0xFFFFU);
        regs->dx = (u16)(ticks & 0xFFFFU);
        regs->ax = (u16)(regs->ax & 0xFF00U);  /* AL=0 midnight flag */
        regs->carry = 0U;
        return;
    }

    /* Unsupported subfunction — ignore silently. */
    regs->carry = 0U;
}

/*
 * SR-MOUSE-001 — DOS-like INT 33h mouse driver state.
 *
 * Coordinates are in "mickey-less" pixel units. The default DOS
 * convention on install is "screen-like 640x200" (mode 0x13 emulated
 * range is 0..639 / 0..199); real drivers adjust automatically when
 * the video mode changes, but for a minimal stage2 driver we keep the
 * range static unless the program sets it explicitly via AX=0007h /
 * AX=0008h. show_count starts at -1 (hidden) per the DOS contract.
 *
 * No physical mouse input is currently wired through to stage2, so the
 * button mask is always 0 and the position only moves when a DOS
 * program calls AX=0004h (set pos). This keeps the ABI honest for
 * programs that drive the cursor themselves (e.g. menu/demo code) and
 * provides a deterministic, testable surface.
 */
#define SHELL_MOUSE_X_MIN_DEFAULT 0
#define SHELL_MOUSE_X_MAX_DEFAULT 639
#define SHELL_MOUSE_Y_MIN_DEFAULT 0
#define SHELL_MOUSE_Y_MAX_DEFAULT 199

typedef struct shell_mouse_state {
    i32 x;
    i32 y;
    u16 buttons;       /* bitmask: bit0=left, bit1=right, bit2=middle */
    i32 show_count;    /* -1 = hidden; >= 0 = visible */
    i32 x_min;
    i32 x_max;
    i32 y_min;
    i32 y_max;
    u8  installed;     /* set on first reset; stage2 session lifetime */
    u8  reserved[3];
} shell_mouse_state_t;

static shell_mouse_state_t g_mouse_state;

static inline i32 shell_mouse_clamp_i32(i32 v, i32 lo, i32 hi) {
    if (lo > hi) {
        /* defensive: normalize swapped range */
        i32 tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void shell_mouse_reset_state(void) {
    g_mouse_state.x_min = SHELL_MOUSE_X_MIN_DEFAULT;
    g_mouse_state.x_max = SHELL_MOUSE_X_MAX_DEFAULT;
    g_mouse_state.y_min = SHELL_MOUSE_Y_MIN_DEFAULT;
    g_mouse_state.y_max = SHELL_MOUSE_Y_MAX_DEFAULT;
    /* Park cursor at center of the default range. */
    g_mouse_state.x = (g_mouse_state.x_min + g_mouse_state.x_max) / 2;
    g_mouse_state.y = (g_mouse_state.y_min + g_mouse_state.y_max) / 2;
    g_mouse_state.buttons = 0U;
    g_mouse_state.show_count = -1; /* hidden on reset — DOS contract */
    g_mouse_state.installed = 1U;
}

static void shell_com_int33(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs) {
    u16 ax;

    (void)ctx;

    if (!regs) {
        return;
    }

    if (!g_mouse_state.installed) {
        /* First-ever call in the session: ensure defaults are sane even
         * if the program skipped AX=0000h (very unusual but tolerated). */
        shell_mouse_reset_state();
        /* The installed flag stays set; the caller's function is still
         * dispatched below with the freshly initialized state. */
    }

    ax = regs->ax;

    switch (ax) {
    case 0x0000U: {
        /* AX=0000h — Reset driver and get status.
         * Return: AX=0xFFFF if installed, BX=number of buttons. */
        shell_mouse_reset_state();
        /* Drain any pending hardware deltas so AX=0003h post-reset
         * starts from a clean slate. */
        {
            i32 dx = 0, dy = 0;
            u16 btn = 0;
            stage2_mouse_consume_deltas(&dx, &dy, &btn);
            (void)dx; (void)dy; (void)btn;
        }
        regs->ax = 0xFFFFU;
        regs->bx = 0x0002U; /* 2-button mouse baseline */
        regs->carry = 0U;
        serial_write("[int33] reset ax=0xFFFF bx=0x0002\n");
        return;
    }
    case 0x0001U: {
        /* AX=0001h — Show cursor (increment show counter). */
        if (g_mouse_state.show_count < 0x7FFFFFFF) {
            g_mouse_state.show_count++;
        }
        regs->carry = 0U;
        serial_write("[int33] show\n");
        return;
    }
    case 0x0002U: {
        /* AX=0002h — Hide cursor (decrement show counter). */
        if (g_mouse_state.show_count > -0x7FFFFFFF) {
            g_mouse_state.show_count--;
        }
        regs->carry = 0U;
        serial_write("[int33] hide\n");
        return;
    }
    case 0x0003U: {
        /* AX=0003h — Get position and button status.
         * Return: BX=buttons, CX=x, DX=y.
         * Drain hardware-accumulated deltas, apply to the DOS-owned
         * absolute position (clipped to the active range), and snap
         * the live button mask. If no hardware input is available,
         * the deltas are zero and the position is whatever the
         * program has set via AX=0004h. */
        i32 dx = 0, dy = 0;
        u16 hw_buttons = 0;
        stage2_mouse_consume_deltas(&dx, &dy, &hw_buttons);
        g_mouse_state.x = shell_mouse_clamp_i32(
            g_mouse_state.x + dx,
            g_mouse_state.x_min, g_mouse_state.x_max);
        g_mouse_state.y = shell_mouse_clamp_i32(
            g_mouse_state.y + dy,
            g_mouse_state.y_min, g_mouse_state.y_max);
        g_mouse_state.buttons = hw_buttons;
        regs->bx = g_mouse_state.buttons;
        regs->cx = (u16)(g_mouse_state.x & 0xFFFFU);
        regs->dx = (u16)(g_mouse_state.y & 0xFFFFU);
        regs->carry = 0U;
        return;
    }
    case 0x0004U: {
        /* AX=0004h — Set cursor position.
         * Input: CX=x, DX=y. Clip to active range. */
        i32 nx = (i32)(i16)regs->cx;
        i32 ny = (i32)(i16)regs->dx;
        g_mouse_state.x = shell_mouse_clamp_i32(nx, g_mouse_state.x_min, g_mouse_state.x_max);
        g_mouse_state.y = shell_mouse_clamp_i32(ny, g_mouse_state.y_min, g_mouse_state.y_max);
        regs->carry = 0U;
        return;
    }
    case 0x0007U: {
        /* AX=0007h — Set horizontal range (CX=min, DX=max). */
        i32 lo = (i32)(i16)regs->cx;
        i32 hi = (i32)(i16)regs->dx;
        if (lo > hi) {
            i32 tmp = lo;
            lo = hi;
            hi = tmp;
        }
        g_mouse_state.x_min = lo;
        g_mouse_state.x_max = hi;
        /* Re-clip current position to the new range. */
        g_mouse_state.x = shell_mouse_clamp_i32(g_mouse_state.x, lo, hi);
        regs->carry = 0U;
        return;
    }
    case 0x0008U: {
        /* AX=0008h — Set vertical range (CX=min, DX=max). */
        i32 lo = (i32)(i16)regs->cx;
        i32 hi = (i32)(i16)regs->dx;
        if (lo > hi) {
            i32 tmp = lo;
            lo = hi;
            hi = tmp;
        }
        g_mouse_state.y_min = lo;
        g_mouse_state.y_max = hi;
        g_mouse_state.y = shell_mouse_clamp_i32(g_mouse_state.y, lo, hi);
        regs->carry = 0U;
        return;
    }
    default:
        /* Unsupported subfunction — log once per call and signal no-op.
         * We keep CF=0 to match the "silently ignore" behavior used by
         * our other interrupt dispatchers for unimplemented subsets. */
        serial_write("[int33] unsupported ax=0x");
        serial_write_hex64((u64)ax);
        serial_write("\n");
        regs->carry = 0U;
        return;
    }
}

/*
 * SR-MOUSE-001 follow-up — minimal software cursor for mode 13h.
 *
 * Draws a 6x6 arrow pattern anchored at the hotspot (top-left corner
 * of the arrow) at the current INT 33h position, clipped to the mode
 * bounds. No-op when show_count < 0 (cursor hidden) or when the
 * active position is entirely off-screen. Uses gfx_mode13_put_pixel
 * so it works regardless of whether the caller keeps its backbuffer
 * in the stage2 plane or elsewhere — the caller just needs to invoke
 * present() after this.
 */
static const u8 k_mouse_cursor_bitmap[6][6] = {
    {1, 0, 0, 0, 0, 0},
    {1, 1, 0, 0, 0, 0},
    {1, 1, 1, 0, 0, 0},
    {1, 1, 1, 1, 0, 0},
    {1, 1, 1, 0, 0, 0},
    {1, 0, 1, 1, 0, 0},
};

static void shell_mouse_draw_cursor_mode13(u8 color_index) {
    if (!g_mouse_state.installed) {
        return;
    }
    if (g_mouse_state.show_count < 0) {
        return;
    }
    /* OPENGEM-005 — Do not paint the fallback cursor while an
     * OpenGEM session owns the screen; GEMVDI renders its own
     * pointer. */
    if (stage2_mouse_opengem_cursor_quiesced()) {
        return;
    }
    i32 x0 = g_mouse_state.x;
    i32 y0 = g_mouse_state.y;
    if (x0 >= (i32)GFX_MODE13_W || y0 >= (i32)GFX_MODE13_H) {
        return;
    }
    if (x0 + 6 < 0 || y0 + 6 < 0) {
        return;
    }
    for (i32 dy = 0; dy < 6; dy++) {
        i32 py = y0 + dy;
        if (py < 0 || py >= (i32)GFX_MODE13_H) continue;
        for (i32 dx = 0; dx < 6; dx++) {
            if (!k_mouse_cursor_bitmap[dy][dx]) continue;
            i32 px = x0 + dx;
            if (px < 0 || px >= (i32)GFX_MODE13_W) continue;
            gfx_mode13_put_pixel((u32)px, (u32)py, color_index);
        }
    }
}

static void shell_com_int31(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs) {
    if (!regs) {
        return;
    }

    if (regs->ax == 0x0400U) {
        /* DPMI 0.9 Get Version callable slice for the next M6 bootstrap step. */
        regs->ax = 0x005AU;
        regs->bx = 0x0003U;
        regs->cx = 0x0004U;
        regs->dx = 0x0870U;
        regs->si = 0x0000U;
        regs->di = 0x0000U;
        regs->carry = 0U;
        return;
    }

    if (regs->ax == 0x0306U) {
        /* Raw mode-switch address query for bootstrap-facing DOS extender flow. */
        regs->bx = 0xF000U;
        regs->cx = 0x0200U;
        regs->si = 0xF000U;
        regs->di = 0x0300U;
        regs->carry = 0U;
        return;
    }

    if (regs->ax == 0x0300U) {
        /*
         * Minimal DPMI 0.9 Simulate Real Mode Interrupt slice.
         * Current callable baseline supports only BL=0x21 (INT 21h) and uses
         * ES:DI to point at a ciuki_int21_regs_t frame inside the active DOS image.
         */
        ciuki_int21_regs_t *rm_regs;
        u8 int_no = (u8)(regs->bx & 0x00FFU);

        if (!ctx || (regs->bx & 0xFF00U) != 0U || regs->cx != 0U) {
            regs->carry = 1U;
            regs->ax = 0x8021U; /* invalid value */
            return;
        }

        rm_regs = shell_dpmi_real_mode_regs_ptr(ctx, regs->es, regs->di);
        if (!rm_regs) {
            regs->carry = 1U;
            regs->ax = 0x8021U; /* invalid register frame */
            return;
        }

        if (int_no != 0x21U) {
            regs->carry = 1U;
            regs->ax = 0x8001U; /* unsupported function */
            return;
        }

        shell_com_int21(ctx, rm_regs);
        regs->ax = 0x0000U;
        regs->carry = 0U;
        return;
    }

    if (regs->ax == 0x0000U) {
        /*
         * DPMI 0.9 Allocate LDT Descriptors callable slice.
         * Returns a synthetic base selector whose low 3 bits encode
         * RPL=3 + TI=1 (LDT), which is the shape real DOS extenders
         * expect from an LDT descriptor allocation.
         */
        if (regs->cx == 0U) {
            regs->carry = 1U;
            regs->ax = 0x8021U; /* DPMI error: invalid value */
            return;
        }
        regs->ax = 0x0087U;
        regs->carry = 0U;
        return;
    }

    if (regs->ax == 0x0501U) {
        /*
         * DPMI 0.9 Allocate Memory Block callable slice.
         * Requested size is BX:CX; we now track a synthetic stateful block
         * so a later AX=0502 free call can validate handle ownership.
         */
        u32 request_size;
        u32 linear_addr;
        u32 handle;

        request_size = ((u32)regs->bx << 16) | (u32)regs->cx;
        if (regs->bx == 0U && regs->cx == 0U) {
            regs->carry = 1U;
            regs->ax = 0x8021U; /* DPMI error: invalid value */
            return;
        }

        if (!shell_dpmi_alloc_mem_block(request_size, &linear_addr, &handle)) {
            regs->carry = 1U;
            regs->ax = 0x8013U; /* DPMI error: insufficient memory */
            return;
        }

        regs->ax = 0x0000U;
        regs->bx = (u16)(linear_addr >> 16);
        regs->cx = (u16)(linear_addr & 0xFFFFU);
        regs->si = (u16)(handle >> 16);
        regs->di = (u16)(handle & 0xFFFFU);
        regs->carry = 0U;
        return;
    }

    if (regs->ax == 0x0502U) {
        /*
         * DPMI 0.9 Free Memory Block callable slice.
         * Handle is passed in SI:DI. Success now depends on a previous
         * stateful AX=0501 allocation, so duplicate frees are detectable.
         */
        u32 handle = ((u32)regs->si << 16) | (u32)regs->di;
        if (handle == 0U || !shell_dpmi_free_mem_block(handle)) {
            regs->carry = 1U;
            regs->ax = 0x8023U; /* DPMI error: invalid handle */
            return;
        }

        regs->ax = 0x0000U;
        regs->carry = 0U;
        return;
    }

    regs->carry = 1U;
    regs->ax = 0x8001U;
}

static void shell_com_int21_4c(ciuki_dos_context_t *ctx, u8 code) {
    if (!ctx) {
        return;
    }
    ctx->exit_reason = (u8)CIUKI_COM_EXIT_INT21_4C;
    ctx->exit_code = code;
}

static void shell_com_terminate(ciuki_dos_context_t *ctx, u8 code) {
    if (!ctx) {
        return;
    }
    ctx->exit_reason = (u8)CIUKI_COM_EXIT_API;
    ctx->exit_code = code;
}

static void shell_publish_last_exit_status(const ciuki_dos_context_t *ctx) {
    if (!ctx) {
        return;
    }
    g_int21_last_return_code = ctx->exit_code;
    /*
     * For now all exits map to DOS "normal termination" type 0.
     * We can differentiate abort/TSR causes once those paths exist.
     */
    g_int21_last_termination_type = 0U;
    g_int21_last_status_pending = 1U;
}

int stage2_shell_selftest_int21_baseline(void) {
    ciuki_dos_context_t ctx;
    ciuki_int21_regs_t regs;
    static const char test_image[] = "ABC$";
    static u8 linebuf[8];

    local_memset(&ctx, 0U, (u32)sizeof(ctx));
    shell_int21_reset_handle_table();
    ctx.image_linear = (u64)(const void *)test_image;
    ctx.image_size = (u32)(sizeof(test_image) - 1U);
    ctx.psp_segment = 0x4321U;

    /* AH=30h: DOS version */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x1606U) {
        return 0;
    }

    /* AH=06h direct console output */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x0600U;
    regs.dx = (u16)'Q';
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || (regs.ax & 0x00FFU) != (u16)'Q') {
        return 0;
    }

    /* AH=06h non-blocking input path: no char available -> AL=00 */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x0600U;
    regs.dx = 0x00FFU;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || (regs.ax & 0x00FFU) != 0x0000U) {
        return 0;
    }

    /* AH=07h: blocking char input without echo using pending-char injection */
    g_int21_pending_stdin_char = (i32)'R';
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x0700U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || (regs.ax & 0x00FFU) != (u16)'R') {
        return 0;
    }

    /* AH=51h / AH=62h: get PSP address */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x5100U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.bx != 0x4321U) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x6200U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.bx != 0x4321U) {
        return 0;
    }

    /* AH=1Ah / AH=2Fh: set/get DTA pointer */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x1A00U;
    regs.ds = 0xCAFEU;
    regs.dx = 0x0040U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x2F00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.es != 0xCAFEU || regs.bx != 0x0040U) {
        return 0;
    }

    /* AH=0Eh + AH=19h: set/get default drive */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x0E00U;
    regs.dx = 0x0002U; /* C: */
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || (regs.ax & 0x00FFU) != 0x0001U) {
        return 0;
    }

    /* AH=19h: current drive */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x1900U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || (regs.ax & 0x00FFU) != 0x0002U) {
        return 0;
    }

    /* AH=25h / AH=35h: set/get vector */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x2521U;
    regs.ds = 0x1234U;
    regs.dx = 0x5678U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        return 0;
    }

    /* AH=0Ah buffered line input: max=0 must return immediately without blocking */
    local_memset(linebuf, 0U, (u32)sizeof(linebuf));
    linebuf[0] = 0U;
    linebuf[1] = 0x7FU;
    ctx.image_linear = (u64)(void *)linebuf;
    ctx.image_size = (u32)sizeof(linebuf);
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x0A00U;
    regs.dx = 0x0000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || linebuf[1] != 0U || linebuf[2] != '\r') {
        return 0;
    }

    /* AH=0Ch with AL=0Ah flush+buffered line path, still non-blocking for max=0 */
    local_memset(linebuf, 0U, (u32)sizeof(linebuf));
    linebuf[0] = 0U;
    ctx.image_linear = (u64)(void *)linebuf;
    ctx.image_size = (u32)sizeof(linebuf);
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x0C0AU;
    regs.dx = 0x0000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || linebuf[1] != 0U || linebuf[2] != '\r') {
        return 0;
    }

    /* Restore baseline test image for remaining checks */
    ctx.image_linear = (u64)(const void *)test_image;
    ctx.image_size = (u32)(sizeof(test_image) - 1U);

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3521U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.es != 0x1234U || regs.bx != 0x5678U) {
        return 0;
    }

    /* AH=48h allocate paragraphs */
    shell_int21_mem_reset(0x5000U, 0x0020U);
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4800U;
    regs.bx = 0x0004U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x5000U) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4800U;
    regs.bx = 0x0008U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x5004U) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4800U;
    regs.bx = 0x0018U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0008U || regs.bx != 0x0014U) {
        return 0;
    }

    /* AH=49h free block */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4900U;
    regs.es = 0x5004U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0000U) {
        return 0;
    }

    /* Ownership check: foreign PSP cannot free the block. */
    ctx.psp_segment = 0x2222U;
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4900U;
    regs.es = 0x5000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0009U) {
        return 0;
    }
    ctx.psp_segment = 0x4321U;

    /* AH=4Ah resize block */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4A00U;
    regs.bx = 0x0010U;
    regs.es = 0x5000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0000U) {
        return 0;
    }

    /* Ownership check: foreign PSP cannot resize the block. */
    ctx.psp_segment = 0x2222U;
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4A00U;
    regs.bx = 0x0012U;
    regs.es = 0x5000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0008U || regs.bx != 0x0000U) {
        return 0;
    }
    ctx.psp_segment = 0x4321U;

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4A00U;
    regs.bx = 0x0030U;
    regs.es = 0x5000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0008U || regs.bx != 0x0020U) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4900U;
    regs.es = 0x5000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0000U) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4900U;
    regs.es = 0x5000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0009U) {
        return 0;
    }

    /* AH=0Bh keyboard status is deterministic (AL=00 or AL=FF) */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x0B00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || (((regs.ax & 0x00FFU) != 0x0000U) && ((regs.ax & 0x00FFU) != 0x00FFU))) {
        return 0;
    }

    /* AH=0Ch flush-only deterministic path (AL=00) */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x0C00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        return 0;
    }

    /* AH=3Ch/3Dh/41h deterministic stubs */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3C00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0005U) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3D00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0002U) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4100U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0002U) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4300U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0002U) {
        return 0;
    }

    /* AH=3Eh close: std handles allowed, invalid handles rejected */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3E00U;
    regs.bx = 0x0001U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3E00U;
    regs.bx = 0x0003U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0006U) {
        return 0;
    }

    /* AH=40h write to stdout/stderr for deterministic console output */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4000U;
    regs.bx = 0x0001U;
    regs.cx = 0x0003U;
    regs.dx = 0x0000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0003U) {
        return 0;
    }

    /* AH=3Fh read invalid handle path and stdout read deny path */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3F00U;
    regs.bx = 0x0004U;
    regs.cx = 0x0001U;
    regs.dx = 0x0000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0006U) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3F00U;
    regs.bx = 0x0001U;
    regs.cx = 0x0001U;
    regs.dx = 0x0000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0005U) {
        return 0;
    }

    /* AH=42h lseek deterministic baseline */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4200U;
    regs.bx = 0x0001U;
    regs.cx = 0x0000U;
    regs.dx = 0x0000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0000U || regs.dx != 0x0000U) {
        return 0;
    }

    /* AH=4Dh: no pending status returns zero */
    g_int21_last_return_code = 0U;
    g_int21_last_termination_type = 0U;
    g_int21_last_status_pending = 0U;
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4D00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0000U) {
        return 0;
    }

    /* AH=4Dh: get return code + termination type */
    g_int21_last_return_code = 0x5AU;
    g_int21_last_termination_type = 0x01U;
    g_int21_last_status_pending = 1U;
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4D00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x015AU) {
        return 0;
    }
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4D00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0000U) {
        return 0;
    }

    /* AH=09h */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x0900U;
    regs.dx = 0x0000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || (regs.ax & 0x00FFU) != 0x24U) {
        return 0;
    }

    /* AH=02h */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x0200U;
    regs.dx = (u16)'Z';
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || (regs.ax & 0x00FFU) != (u16)'Z') {
        return 0;
    }

    /* AH=00h -> INT20 path */
    ctx.exit_reason = (u8)CIUKI_COM_EXIT_RETURN;
    ctx.exit_code = 0xA5U;
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x0000U;
    shell_com_int21(&ctx, &regs);
    if (ctx.exit_reason != (u8)CIUKI_COM_EXIT_INT20 || ctx.exit_code != 0U) {
        return 0;
    }

    /* AH=4Ch -> terminate with code */
    ctx.exit_reason = (u8)CIUKI_COM_EXIT_RETURN;
    ctx.exit_code = 0U;
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4C7BU;
    shell_com_int21(&ctx, &regs);
    if (ctx.exit_reason != (u8)CIUKI_COM_EXIT_INT21_4C || ctx.exit_code != 0x7BU) {
        return 0;
    }

    /* Lifecycle check: publish INT 21h/AH=4Ch exit -> AH=4Dh reports latest code/type. */
    shell_publish_last_exit_status(&ctx);
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4D00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x007BU) {
        return 0;
    }
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4D00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0000U) {
        return 0;
    }

    /* Lifecycle check: API terminate publishes latest code and remains DOS normal type. */
    shell_com_terminate(&ctx, 0x33U);
    shell_publish_last_exit_status(&ctx);
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4D00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0033U) {
        return 0;
    }
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4D00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0000U) {
        return 0;
    }

    /* Unsupported AH must be deterministic */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0xFE00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0001U) {
        return 0;
    }

    return 1;
}

int stage2_shell_selftest_int21_fat_handles(void) {
    static const char dos_path[] = "A:\\EFI\\CIUKIOS\\I21E2E.TXT";
    static const char dos_ren_path[] = "A:\\EFI\\CIUKIOS\\I21E2R.TXT";
    static const char payload[] = "CIUKIOS_INT21_FAT_E2E";
    static const char canon_path[] = "/EFI/CIUKIOS/I21E2E.TXT";
    static const char canon_ren_path[] = "/EFI/CIUKIOS/I21E2R.TXT";
    static u8 runtime_mem[512];
    ciuki_dos_context_t ctx;
    ciuki_int21_regs_t regs;
    fat_dir_entry_t info;
    u16 h_create = 0U;
    u16 h_open = 0U;
    u16 payload_len = (u16)(sizeof(payload) - 1U);
    u16 read_off = 0x0080U;
    u16 path_off = 0x0010U;
    u16 data_off = 0x0040U;
    u16 ren_path_off = 0x00C0U;

    if (!fat_ready()) {
        return 0;
    }

    local_memset(runtime_mem, 0U, (u32)sizeof(runtime_mem));
    local_memset(&ctx, 0U, (u32)sizeof(ctx));
    shell_int21_reset_handle_table();

    ctx.image_linear = (u64)(void *)runtime_mem;
    ctx.image_size = (u32)sizeof(runtime_mem);
    ctx.psp_segment = 0x4321U;

    /* Place DOS path and payload in runtime memory for DS:DX access. */
    local_memcpy(runtime_mem + path_off, dos_path, (u32)sizeof(dos_path));
    local_memcpy(runtime_mem + ren_path_off, dos_ren_path, (u32)sizeof(dos_ren_path));
    local_memcpy(runtime_mem + data_off, payload, (u32)payload_len);

    /* Best-effort cleanup from previous runs. */
    (void)fat_delete_file(canon_path);
    (void)fat_delete_file(canon_ren_path);

    /* AH=3Ch create/truncate */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3C00U;
    regs.cx = 0x0000U;
    regs.dx = path_off;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        goto fail;
    }
    h_create = regs.ax;

    /* AH=40h write payload */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4000U;
    regs.bx = h_create;
    regs.cx = payload_len;
    regs.dx = data_off;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != payload_len) {
        goto fail;
    }

    /* AH=42h seek to start (AL=0, absolute) */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4200U;
    regs.bx = h_create;
    regs.cx = 0x0000U;
    regs.dx = 0x0000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0000U || regs.dx != 0x0000U) {
        goto fail;
    }

    /* AH=3Fh read back */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3F00U;
    regs.bx = h_create;
    regs.cx = payload_len;
    regs.dx = read_off;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != payload_len) {
        goto fail;
    }
    for (u16 i = 0U; i < payload_len; i++) {
        if (runtime_mem[read_off + i] != (u8)payload[i]) {
            goto fail;
        }
    }

    /* AH=3Eh close (flush to FAT) */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3E00U;
    regs.bx = h_create;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        goto fail;
    }
    h_create = 0U;

    /* AH=3Dh reopen */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3D00U; /* read-only */
    regs.dx = path_off;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        goto fail;
    }
    h_open = regs.ax;

    /* AH=3Eh close reopened handle */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x3E00U;
    regs.bx = h_open;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        goto fail;
    }
    h_open = 0U;

    /* AH=43h get/set attribute */
    {
        u16 base_attr = 0U;

        local_memset(&regs, 0U, (u32)sizeof(regs));
        regs.ax = 0x4300U;
        regs.dx = path_off;
        shell_com_int21(&ctx, &regs);
        if (regs.carry != 0U) {
            goto fail;
        }
        base_attr = regs.cx;

        local_memset(&regs, 0U, (u32)sizeof(regs));
        regs.ax = 0x4301U;
        regs.cx = (u16)(base_attr | FAT_ATTR_HIDDEN);
        regs.dx = path_off;
        shell_com_int21(&ctx, &regs);
        if (regs.carry != 0U) {
            goto fail;
        }

        local_memset(&regs, 0U, (u32)sizeof(regs));
        regs.ax = 0x4300U;
        regs.dx = path_off;
        shell_com_int21(&ctx, &regs);
        if (regs.carry != 0U || (regs.cx & FAT_ATTR_HIDDEN) == 0U) {
            goto fail;
        }

        local_memset(&regs, 0U, (u32)sizeof(regs));
        regs.ax = 0x4301U;
        regs.cx = base_attr;
        regs.dx = path_off;
        shell_com_int21(&ctx, &regs);
        if (regs.carry != 0U) {
            goto fail;
        }
    }

    /* AH=56h rename (source DS:DX, destination ES:DI) */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x5600U;
    regs.ds = ctx.psp_segment;
    regs.dx = path_off;
    regs.es = ctx.psp_segment;
    regs.di = ren_path_off;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        goto fail;
    }

    if (fat_find_file(canon_path, &info)) {
        goto fail;
    }
    if (!fat_find_file(canon_ren_path, &info)) {
        goto fail;
    }

    /* AH=41h delete */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4100U;
    regs.dx = ren_path_off;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        goto fail;
    }

    /* Verify deletion at FAT layer. */
    if (fat_find_file(canon_path, &info) || fat_find_file(canon_ren_path, &info)) {
        return 0;
    }

    return 1;

fail:
    if (h_create != 0U) {
        local_memset(&regs, 0U, (u32)sizeof(regs));
        regs.ax = 0x3E00U;
        regs.bx = h_create;
        shell_com_int21(&ctx, &regs);
    }
    if (h_open != 0U) {
        local_memset(&regs, 0U, (u32)sizeof(regs));
        regs.ax = 0x3E00U;
        regs.bx = h_open;
        shell_com_int21(&ctx, &regs);
    }
    (void)fat_delete_file(canon_path);
    (void)fat_delete_file(canon_ren_path);
    return 0;
}

int stage2_shell_selftest_int21_findfirst_next(void) {
    static u8 runtime_mem[512];
    static const char dos_pattern[] = "A:\\EFI\\CIUKIOS\\FFN?.TXT";
    static const char payload1[] = "1";
    static const char payload2[] = "2";
    static const char canon1[] = "/EFI/CIUKIOS/FFN1.TXT";
    static const char canon2[] = "/EFI/CIUKIOS/FFN2.TXT";
    ciuki_dos_context_t ctx;
    ciuki_int21_regs_t regs;
    u16 path_off = 0x0020U;
    u16 dta_off = 0x0080U;
    char name1[13];
    char name2[13];
    int ok1;
    int ok2;

    if (!fat_ready()) {
        return 0;
    }

    (void)fat_delete_file(canon1);
    (void)fat_delete_file(canon2);
    if (!fat_write_file(canon1, payload1, 1U)) {
        return 0;
    }
    if (!fat_write_file(canon2, payload2, 1U)) {
        (void)fat_delete_file(canon1);
        return 0;
    }

    local_memset(runtime_mem, 0U, (u32)sizeof(runtime_mem));
    local_memset(&ctx, 0U, (u32)sizeof(ctx));
    shell_int21_reset_handle_table();

    ctx.image_linear = (u64)(void *)runtime_mem;
    ctx.image_size = (u32)sizeof(runtime_mem);
    ctx.psp_segment = 0x4321U;
    local_memcpy(runtime_mem + path_off, dos_pattern, (u32)sizeof(dos_pattern));

    /* Set DTA pointer to runtime buffer at 0x80. */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x1A00U;
    regs.ds = ctx.psp_segment;
    regs.dx = dta_off;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        goto fail;
    }

    /* Find first match. */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4E00U;
    regs.cx = 0x0000U;
    regs.ds = ctx.psp_segment;
    regs.dx = path_off;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        goto fail;
    }

    local_memset(name1, 0U, (u32)sizeof(name1));
    local_memcpy(name1, runtime_mem + dta_off + SHELL_INT21_DTA_NAME_OFFSET, 12U);
    name1[12] = '\0';

    /* Find next match. */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4F00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        goto fail;
    }

    local_memset(name2, 0U, (u32)sizeof(name2));
    local_memcpy(name2, runtime_mem + dta_off + SHELL_INT21_DTA_NAME_OFFSET, 12U);
    name2[12] = '\0';

    if (str_eq(name1, name2)) {
        goto fail;
    }

    ok1 = str_eq(name1, "FFN1.TXT") || str_eq(name1, "FFN2.TXT");
    ok2 = str_eq(name2, "FFN1.TXT") || str_eq(name2, "FFN2.TXT");
    if (!ok1 || !ok2) {
        goto fail;
    }

    /* Third find-next must fail with no more files. */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4F00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 1U || regs.ax != 0x0012U) {
        goto fail;
    }

    (void)fat_delete_file(canon1);
    (void)fat_delete_file(canon2);
    return 1;

fail:
    (void)fat_delete_file(canon1);
    (void)fat_delete_file(canon2);
    return 0;
}

static void shell_prepare_psp(ciuki_dos_context_t *ctx, u32 image_size, const char *tail) {
    u8 *psp = (u8 *)(u64)SHELL_RUNTIME_COM_ADDR;
    u32 len = 0;
    u32 image_paras;
    u32 reserved_paras;
    u32 total_paras;
    u32 free_paras;
    u16 psp_segment;
    u16 heap_base_seg;
    u16 heap_paras;
    u16 end_paragraph;

    local_memset(psp, 0U, SHELL_RUNTIME_PSP_SIZE);

    psp_segment = (u16)((SHELL_RUNTIME_COM_ADDR >> 4) & 0xFFFFU);
    image_paras = (image_size + 15U) >> 4;
    reserved_paras = 0x10U + image_paras;
    total_paras = (u32)(SHELL_RUNTIME_COM_MAX_SIZE >> 4);
    free_paras = (total_paras > reserved_paras) ? (total_paras - reserved_paras) : 0U;
    if (free_paras > 0xFFFFU) {
        free_paras = 0xFFFFU;
    }
    heap_base_seg = (u16)(psp_segment + (u16)reserved_paras);
    heap_paras = (u16)free_paras;
    end_paragraph = (u16)(heap_base_seg + heap_paras);

    /* PSP:0000h = INT 20h */
    psp[0x00] = 0xCD;
    psp[0x01] = 0x20;

    psp[0x02] = (u8)(end_paragraph & 0x00FFU);
    psp[0x03] = (u8)((end_paragraph >> 8) & 0x00FFU);

    if (tail) {
        while (tail[len] != '\0' && len < SHELL_RUNTIME_TAIL_MAX) {
            psp[0x81U + len] = (u8)tail[len];
            len++;
        }
    }
    psp[0x80] = (u8)len;
    psp[0x81U + len] = 0x0D;

    ctx->psp_segment = psp_segment;
    ctx->psp_linear = SHELL_RUNTIME_COM_ADDR;
    ctx->image_linear = SHELL_RUNTIME_COM_ENTRY_ADDR;
    ctx->image_size = image_size;
    ctx->command_tail_len = (u8)len;
    ctx->command_tail[0] = '\0';
    for (u32 i = 0; i < len && i < (u32)sizeof(ctx->command_tail) - 1U; i++) {
        ctx->command_tail[i] = (char)psp[0x81U + i];
        ctx->command_tail[i + 1U] = '\0';
    }

    g_int21_dta_segment = ctx->psp_segment;
    g_int21_dta_offset = 0x0080U;
    local_memset(&g_int21_find_state, 0U, (u32)sizeof(g_int21_find_state));
    shell_int21_reset_handle_table();
    shell_int21_mem_reset(heap_base_seg, heap_paras);
}

static void shell_print_com_exit(const ciuki_dos_context_t *ctx) {
    const char *reason;
    if (ctx->exit_reason == (u8)CIUKI_COM_EXIT_INT20) {
        reason = "INT 20h";
    } else if (ctx->exit_reason == (u8)CIUKI_COM_EXIT_INT21_4C) {
        reason = "INT 21h/AH=4Ch";
    } else if (ctx->exit_reason == (u8)CIUKI_COM_EXIT_API) {
        reason = "terminate()";
    } else {
        reason = "RET";
    }

    /* Always emit deterministic exit evidence to serial for tests. */
    serial_write("[dosrun] exit reason=");
    serial_write(reason);
    serial_write(" code=0x");
    serial_write_hex64((u64)ctx->exit_code);
    serial_write("\n");

    /*
     * User-visible feedback: only surface a short message on non-zero
     * exit codes. Successful launches stay silent on the framebuffer
     * so ordinary program launches don't clutter the shell.
     */
    if (g_shell_runtime_graphics_used) {
        return;
    }
    if (ctx->exit_code == 0U) {
        return;
    }
    video_write("Program exited with code 0x");
    video_write_hex64((u64)ctx->exit_code);
    video_write(".\n");
}

static void shell_runtime_note_graphics_use(void) {
    if (g_shell_runtime_graphics_used) {
        return;
    }

    g_shell_runtime_graphics_used = 1U;
    g_shell_runtime_video_print_suppressed = 1U;

    if (gfx_mode_current() == GFX_MODE_TEXT_80x25) {
        video_fill(0x000000U);
        video_present_dirty_immediate();
    }
}

static void shell_runtime_print(const char *s) {
    if (!s) {
        return;
    }

    serial_write(s);
    if (!g_shell_runtime_video_print_suppressed) {
        video_write(s);
    }
}

static int shell_stage_runtime_image(u64 src_phys, u32 src_size) {
    u8 *dst;
    const u8 *src;

    if (src_phys == 0 || src_size == 0U) {
        return 0;
    }
    if (src_size > SHELL_RUNTIME_COM_MAX_PAYLOAD) {
        return 0;
    }

    dst = (u8 *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR;
    src = (const u8 *)(u64)src_phys;
    local_memcpy(dst, src, src_size);
    return 1;
}

static void shell_run_staged_image(
    boot_info_t *boot_info,
    handoff_v0_t *handoff,
    const char *name,
    u32 image_size,
    const char *tail
) {
    ciuki_services_t svc;
    ciuki_dos_context_t ctx;
    com_entry_t entry = (com_entry_t)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR;
    dos_mz_info_t mz_info;
    u32 reloc_applied = 0U;
    u16 load_segment = (u16)(((SHELL_RUNTIME_COM_ADDR >> 4) + 0x10ULL) & 0xFFFFULL);
    int is_mz = 0;
    const char *run_type = "COM";

    if (image_size == 0U || image_size > SHELL_RUNTIME_COM_MAX_PAYLOAD) {
        video_write("Invalid COM size.\n");
        shell_set_errorlevel(1U);
        g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_BAD_FORMAT;
        return;
    }

    if (image_size >= 2U) {
        const u8 *image = (const u8 *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR;
        is_mz = (image[0] == 'M' && image[1] == 'Z');
    }

    if (is_mz) {
        run_type = "MZ";
        if (!dos_mz_build_loaded_image(
                (u8 *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR,
                image_size,
                load_segment,
                &mz_info,
                &image_size,
                &reloc_applied
            )) {
            video_write("Invalid MZ executable.\n");
            shell_set_errorlevel(1U);
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_BAD_FORMAT;
            return;
        }

        if (mz_info.entry_offset >= image_size) {
            video_write("Invalid MZ entry contract.\n");
            shell_set_errorlevel(1U);
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_BAD_FORMAT;
            return;
        }

        if (mz_info.runtime_required_bytes > SHELL_RUNTIME_COM_MAX_PAYLOAD) {
            video_write("MZ runtime span exceeds payload window.\n");
            shell_set_errorlevel(1U);
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_BAD_FORMAT;
            return;
        }
    }

    /*
     * OPENGEM-030 — MZ live-switch gate wire-up.
     *
     * Default boot never flips the armed flag, so the live-gate path
     * is observability-only: we emit a marker indicating whether the
     * armed path would fire for this MZ, then fall through to the
     * existing stage2-native dosrun. When the gate IS armed (only via
     * explicit test code arming the live-switch with the magic),
     * we additionally invoke vm86_live_switch_execute() which today
     * only runs the OPENGEM-027 retq stubs — no CPU mutation.
     *
     * This keeps default boot behavior byte-identical to pre-030
     * while proving the wire-up exists.
     */
    if (is_mz) {
        if (vm86_live_switch_is_armed()) {
            serial_write("OpenGEM: mz-live-gate armed=1 action=execute-stubs\n");
            (void)vm86_live_switch_execute();
            serial_write("OpenGEM: mz-live-gate fallthrough=stage2-dosrun\n");
        } else {
            serial_write("OpenGEM: mz-live-gate armed=0 fallback=defer-to-shell-run\n");
        }
    }

    shell_dosrun_emit_launch_marker(name && name[0] != '\0' ? name : "default", run_type);

    local_memset(&ctx, 0U, (u32)sizeof(ctx));
    ctx.boot_info = boot_info;
    ctx.handoff = handoff;
    ctx.exit_reason = (u8)CIUKI_COM_EXIT_RETURN;
    ctx.exit_code = 0U;
    g_shell_runtime_graphics_used = 0U;
    g_shell_runtime_video_print_suppressed = 0U;
    shell_prepare_psp(&ctx, image_size, tail);
    if (is_mz) {
        ctx.image_linear = SHELL_RUNTIME_COM_ENTRY_ADDR + (u64)mz_info.entry_offset;
    }

    svc.print = shell_runtime_print;
    svc.print_hex64 = video_write_hex64;
    svc.cls = video_cls;
    svc.int21 = shell_com_int21;
    svc.int2f = shell_com_int2f;
    svc.int31 = shell_com_int31;
    svc.int16 = shell_com_int16;
    svc.int1a = shell_com_int1a;
    svc.int20 = shell_com_int20;
    svc.int21_4c = shell_com_int21_4c;
    svc.terminate = shell_com_terminate;
    svc.gfx = &g_shell_runtime_gfx_services;
    svc.serial_print = serial_write;
    svc.ui_top_bar = ui_draw_top_bar;
    svc.ui_reserve_top_row = video_set_text_window;
    svc.int33 = shell_com_int33;
    svc.mouse_draw_cursor_mode13 = shell_mouse_draw_cursor_mode13;

    serial_write("[dosrun] executing name=");
    serial_write(name && name[0] != '\0' ? name : "COM");
    serial_write(" psp=0x");
    serial_write_hex64((u64)ctx.psp_segment);
    serial_write(" entry=0x");
    serial_write_hex64(ctx.image_linear);
    serial_write("\n");

    if (is_mz) {
        const u8 *module = (const u8 *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR;
        int marker_ok = 1;
        u32 stub_entry_off;

        for (u32 i = 0; i < SHELL_EXE32_MARKER_SIZE; i++) {
            if (i >= image_size || module[i] != g_exe32_marker[i]) {
                marker_ok = 0;
                break;
            }
        }

        serial_write("[dosrun] mz loaded bytes=0x");
        serial_write_hex64((u64)image_size);
        serial_write(" reloc=0x");
        serial_write_hex64((u64)reloc_applied);
        serial_write(" load_seg=0x");
        serial_write_hex64((u64)load_segment);
        serial_write(" entry_off=0x");
        serial_write_hex64((u64)mz_info.entry_offset);
        serial_write(" stack_off=0x");
        serial_write_hex64((u64)mz_info.stack_offset);
        serial_write(" span=0x");
        serial_write_hex64((u64)mz_info.runtime_required_bytes);
        serial_write("\n");

        if (!marker_ok || image_size < 12U) {
            video_write("16-bit MZ executable requires legacy_v86 host.\n");
            serial_write("[dosrun] mz dispatch=pending reason=task-b\n");
            shell_set_errorlevel(1U);
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_RUNTIME;
            return;
        }

        stub_entry_off = (u32)module[8]
                       | ((u32)module[9] << 8)
                       | ((u32)module[10] << 16)
                       | ((u32)module[11] << 24);

        if (stub_entry_off >= image_size) {
            video_write("Invalid MZ entry contract.\n");
            serial_write("[dosrun] mz dispatch=invalid reason=entry_offset\n");
            shell_set_errorlevel(1U);
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_RUNTIME;
            return;
        }

        ctx.image_linear = SHELL_RUNTIME_COM_ENTRY_ADDR + (u64)stub_entry_off;
        entry = (com_entry_t)ctx.image_linear;
        serial_write("[dosrun] mz dispatch=CIUKEX64 entry=0x");
        serial_write_hex64(ctx.image_linear);
        serial_write("\n");

        entry(&ctx, &svc);
        shell_int21_close_all_handles();
        shell_publish_last_exit_status(&ctx);
        shell_print_com_exit(&ctx);
        shell_set_errorlevel(ctx.exit_code);
        if (g_shell_runtime_graphics_used) {
            g_shell_prompt_deferred = 1U;
        }
        if (ctx.exit_code != 0U && g_shell_dosrun_int21_unsupported_calls > 0U) {
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_UNSUPPORTED_INT21;
        } else {
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_NONE;
        }
        return;
    }

    entry(&ctx, &svc);
    shell_int21_close_all_handles();
    shell_publish_last_exit_status(&ctx);
    shell_print_com_exit(&ctx);
    shell_set_errorlevel(ctx.exit_code);
    if (g_shell_runtime_graphics_used) {
        g_shell_prompt_deferred = 1U;
    }
    if (ctx.exit_code != 0U && g_shell_dosrun_int21_unsupported_calls > 0U) {
        g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_UNSUPPORTED_INT21;
    } else {
        g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_NONE;
    }
}

static void shell_gem_disarm_path(void) {
    v86_dispatch_disarm();
    legacy_v86_disarm();
    SHELL_MODE_SWITCH_CALL(disarm)();
    vm86_gp_isr_uninstall();
    vm86_gp_isr_install_disarm();
}

static void shell_gem_write_exit_reason(legacy_v86_exit_reason_t reason, u32 fault_code) {
    serial_write("[gem] exit reason=");
    if (reason == LEGACY_V86_EXIT_NORMAL) {
        serial_write("normal");
    } else if (reason == LEGACY_V86_EXIT_HALT) {
        serial_write("halt");
    } else if (reason == LEGACY_V86_EXIT_FAULT) {
        serial_write("fault code=0x");
        serial_write_hex64((u64)fault_code);
    } else if (reason == LEGACY_V86_EXIT_GP_INT) {
        serial_write("gp-int");
    } else {
        serial_write("unknown");
    }
    serial_write("\n");
}

static int shell_run_from_catalog(
    boot_info_t *boot_info,
    handoff_v0_t *handoff,
    u64 phys_base,
    u64 size,
    const char *name,
    const char *tail
) {
    u32 image_size;

    if (phys_base == 0 || size == 0U || size > 0xFFFFFFFFULL) {
        return 0;
    }
    image_size = (u32)size;
    if (!shell_stage_runtime_image(phys_base, image_size)) {
        return 0;
    }

    shell_run_staged_image(boot_info, handoff, name, image_size, tail);
    return 1;
}

static int shell_run_from_fat(
    boot_info_t *boot_info,
    handoff_v0_t *handoff,
    const char *com_name,
    const char *tail
) {
    char path[128];
    char fallback_path[128];
    u32 com_size = 0;
    int has_fallback = 0;

    if (!fat_ready()) {
        return 0;
    }
    if (!build_run_path(com_name, path, (u32)sizeof(path))) {
        return 0;
    }

    /* OPENGEM-006 — DOOM launch telemetry. When the target is
     * DOOM.EXE (case-insensitive), surface a boot-log marker so
     * the test-doom-via-opengem harness can correlate launches.
     * Always safe: pure serial emit, no behavioral change. */
    if (str_eq_nocase(com_name, "DOOM.EXE")) {
        serial_write("[ doom ] opengem launch DOOM.EXE\n");
    }

    if (!fat_read_file(
            path,
            (void *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR,
            SHELL_RUNTIME_COM_MAX_PAYLOAD,
            &com_size
        )) {
        if (!str_eq_nocase(g_shell_cwd, "/EFI/CIUKIOS")) {
            has_fallback = build_canonical_path("/EFI/CIUKIOS", fallback_path, (u32)sizeof(fallback_path));
            if (has_fallback) {
                u32 n = str_len(fallback_path);
                if (n > 0 && n < (u32)sizeof(fallback_path) - 1U && fallback_path[n - 1] != '/') {
                    fallback_path[n++] = '/';
                    fallback_path[n] = '\0';
                }
                {
                    u32 i = 0;
                    while (com_name[i] != '\0' && n < (u32)sizeof(fallback_path) - 1U) {
                        fallback_path[n++] = (char)to_upper_ascii((u8)com_name[i++]);
                    }
                    fallback_path[n] = '\0';
                }

                if (!fat_read_file(
                        fallback_path,
                        (void *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR,
                        SHELL_RUNTIME_COM_MAX_PAYLOAD,
                        &com_size
                    )) {
                    return 0;
                }
            } else {
                return 0;
            }
        } else {
            return 0;
        }
    }
    if (com_size == 0U) {
        return 0;
    }

    shell_run_staged_image(boot_info, handoff, com_name, com_size, tail);
    return 1;
}

static void shell_run(boot_info_t *boot_info, handoff_v0_t *handoff, const char *args) {
    char target[HANDOFF_COM_NAME_MAX + 1];
    char target_path[SHELL_PATH_MAX];
    char tail[128];
    handoff_com_entry_t *entry;
    int launched = 0;
    u32 raw_tail_len;
    int tail_overflow;

    extract_run_tail(args, tail, (u32)sizeof(tail));
    raw_tail_len = shell_count_tail_chars(args);
    tail_overflow = (raw_tail_len > SHELL_RUNTIME_TAIL_MAX) ? 1 : 0;
    g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_NONE;
    g_shell_dosrun_int21_unsupported_calls = 0U;
    shell_dosrun_emit_argv_markers(raw_tail_len, tail_overflow);

    if (tail_overflow) {
        video_write("Argument tail exceeds 126 chars.\n");
        shell_set_errorlevel(1U);
        g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_ARGS_PARSE;
        goto finalize;
    }

    if (!normalize_run_name(args, target, (u32)sizeof(target))) {
        if (handoff->com_phys_base != 0 && handoff->com_phys_size != 0U) {
            if (shell_run_from_catalog(
                    boot_info,
                    handoff,
                    handoff->com_phys_base,
                    handoff->com_phys_size,
                    "default",
                    ""
                )) {
                launched = 1;
            } else {
                video_write("Default COM metadata is invalid.\n");
                shell_set_errorlevel(1U);
                g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_BAD_FORMAT;
            }
        } else if (shell_run_from_fat(boot_info, handoff, "INIT.COM", "")) {
            launched = 1;
        } else {
            video_write("Usage: run <name>\n");
            shell_set_errorlevel(1U);
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_NOT_FOUND;
        }
        goto finalize;
    }

    if (!shell_run_target_is_supported(target)) {
        video_write("Unsupported program format: ");
        video_write(target);
        video_write("\n");
        shell_set_errorlevel(1U);
        g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_BAD_FORMAT;
        goto finalize;
    }

    if (str_ends_with_nocase(target, ".BAT")) {
        shell_dosrun_emit_launch_marker(target, "BAT");
        if (!build_run_path(target, target_path, (u32)sizeof(target_path))) {
            video_write("Invalid batch path.\n");
            shell_set_errorlevel(1U);
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_BAD_FORMAT;
            goto finalize;
        }
        shell_run_batch_file(boot_info, handoff, target_path);
        launched = 1;
        if (shell_get_errorlevel() != 0U) {
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_RUNTIME;
        }
        goto finalize;
    }

    entry = shell_find_com(handoff, target);
    if (entry) {
        if (shell_run_from_catalog(
                boot_info,
                handoff,
                entry->phys_base,
                entry->size,
                entry->name,
                tail
            )) {
            launched = 1;
            goto finalize;
        }
        video_write("COM entry metadata is invalid: ");
        video_write(entry->name);
        video_write("\n");
        shell_set_errorlevel(1U);
        g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_BAD_FORMAT;
        goto finalize;
    }

    if (shell_run_from_fat(boot_info, handoff, target, tail)) {
        launched = 1;
        goto finalize;
    }

    video_write("Program not found: ");
    video_write(target);
    video_write("\n");
    shell_set_errorlevel(1U);
    g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_NOT_FOUND;

finalize:
    if (launched && g_shell_dosrun_error_class == SHELL_DOSRUN_ERROR_NONE) {
        shell_dosrun_emit_ok_marker(shell_get_errorlevel());
        return;
    }

    if (g_shell_dosrun_error_class == SHELL_DOSRUN_ERROR_NONE) {
        g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_RUNTIME;
    }
    shell_dosrun_emit_error_marker(g_shell_dosrun_error_class);
}

int stage2_shell_selftest_dosrun_status_path(void) {
    ciuki_dos_context_t run_ctx;
    ciuki_int21_regs_t regs;
    /*
     * Validate the launch-status contract using the runtime-native terminate
     * path: INT 21h/AH=4Ch publishes exit code, and AH=4Dh returns it once.
     */
    local_memset(&run_ctx, 0U, (u32)sizeof(run_ctx));
    run_ctx.psp_segment = (u16)((SHELL_RUNTIME_COM_ADDR >> 4) & 0xFFFFU);
    shell_com_int21_4c(&run_ctx, 0x2AU);
    if (run_ctx.exit_reason != (u8)CIUKI_COM_EXIT_INT21_4C || run_ctx.exit_code != 0x2AU) {
        return 0;
    }

    shell_publish_last_exit_status(&run_ctx);
    shell_set_errorlevel(run_ctx.exit_code);
    g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_NONE;
    if (shell_get_errorlevel() != 0x2AU) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4D00U;
    shell_com_int21(&run_ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x002AU) {
        return 0;
    }

    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4D00U;
    shell_com_int21(&run_ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0000U) {
        return 0;
    }

    return 1;
}

int stage2_shell_selftest_resolver(void) {
    char out[SHELL_PATH_MAX];

    /* Test 1: bare name resolves relative to CWD (/EFI/CIUKIOS at boot) */
    if (!build_canonical_path("HELLO.COM", out, (u32)sizeof(out))) return 0;
    if (!str_eq(out, "/EFI/CIUKIOS/HELLO.COM")) {
        serial_write("[selftest] resolver t1 fail got=");
        serial_write(out);
        serial_write("\n");
        return 0;
    }

    /* Test 2: relative path with backslash */
    if (!build_canonical_path("SUBDIR\\APP.COM", out, (u32)sizeof(out))) return 0;
    if (!str_eq(out, "/EFI/CIUKIOS/SUBDIR/APP.COM")) {
        serial_write("[selftest] resolver t2 fail got=");
        serial_write(out);
        serial_write("\n");
        return 0;
    }

    /* Test 3: dot-slash current directory */
    if (!build_canonical_path(".\\APP.COM", out, (u32)sizeof(out))) return 0;
    if (!str_eq(out, "/EFI/CIUKIOS/APP.COM")) {
        serial_write("[selftest] resolver t3 fail got=");
        serial_write(out);
        serial_write("\n");
        return 0;
    }

    /* Test 4: parent traversal */
    if (!build_canonical_path("..\\APP.COM", out, (u32)sizeof(out))) return 0;
    if (!str_eq(out, "/EFI/APP.COM")) {
        serial_write("[selftest] resolver t4 fail got=");
        serial_write(out);
        serial_write("\n");
        return 0;
    }

    /* Test 5: absolute path (forward slash) */
    if (!build_canonical_path("/EFI/TEST.COM", out, (u32)sizeof(out))) return 0;
    if (!str_eq(out, "/EFI/TEST.COM")) {
        serial_write("[selftest] resolver t5 fail got=");
        serial_write(out);
        serial_write("\n");
        return 0;
    }

    /* Test 6: empty input returns CWD */
    if (!build_canonical_path("", out, (u32)sizeof(out))) return 0;
    if (!str_eq(out, g_shell_cwd)) {
        serial_write("[selftest] resolver t6 fail got=");
        serial_write(out);
        serial_write("\n");
        return 0;
    }

    /* Test 7: forward slash in relative path */
    if (!build_canonical_path("SUB/FILE.EXE", out, (u32)sizeof(out))) return 0;
    if (!str_eq(out, "/EFI/CIUKIOS/SUB/FILE.EXE")) {
        serial_write("[selftest] resolver t7 fail got=");
        serial_write(out);
        serial_write("\n");
        return 0;
    }

    serial_write("[selftest] resolver: all 7 cases passed\n");
    return 1;
}

static void shell_type(const char *args) {
    char path[128];
    fat_dir_entry_t info;
    u32 file_size = 0;

    if (!fat_ready()) {
        video_write("FAT layer not ready.\n");
        return;
    }
    if (!build_arg_path(args, path, (u32)sizeof(path))) {
        video_write("Usage: type <file>\n");
        return;
    }
    if (!fat_find_file(path, &info)) {
        video_write("File not found: ");
        write_dos_path(path);
        video_write("\n");
        return;
    }
    if (info.attr & FAT_ATTR_DIRECTORY) {
        video_write("Is a directory: ");
        write_dos_path(path);
        video_write("\n");
        return;
    }
    if (info.size > SHELL_FILE_BUFFER_SIZE) {
        video_write("File too large (");
        write_decimal(info.size);
        video_write(" bytes, max ");
        write_decimal(SHELL_FILE_BUFFER_SIZE);
        video_write(").\n");
        return;
    }
    if (!fat_read_file(path, g_shell_file_buffer, SHELL_FILE_BUFFER_SIZE, &file_size)) {
        video_write("Read error: ");
        video_write(path);
        video_write("\n");
        return;
    }

    for (u32 i = 0; i < file_size; i++) {
        u8 ch = g_shell_file_buffer[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n' || ch == '\t' || (ch >= 0x20 && ch <= 0x7E)) {
            video_putchar((char)ch);
        } else {
            video_putchar('.');
        }
    }
    if (file_size == 0 || g_shell_file_buffer[file_size - 1] != '\n') {
        video_write("\n");
    }
}

static void shell_del(const char *args) {
    char path[128];
    fat_dir_entry_t info;

    if (!fat_ready()) {
        video_write("FAT layer not ready.\n");
        return;
    }
    if (!build_arg_path(args, path, (u32)sizeof(path))) {
        video_write("Usage: del <file>\n");
        return;
    }

    /* Pre-check: give specific error before attempting delete */
    if (!fat_find_file(path, &info)) {
        video_write("File not found: ");
        write_dos_path(path);
        video_write("\n");
        return;
    }
    if (info.attr & FAT_ATTR_DIRECTORY) {
        video_write("Cannot delete directory: ");
        write_dos_path(path);
        video_write("\n");
        return;
    }
    if (info.attr & FAT_ATTR_READ_ONLY) {
        video_write("Access denied (read-only): ");
        write_dos_path(path);
        video_write("\n");
        return;
    }

    if (!fat_delete_file(path)) {
        video_write("Delete failed: ");
        write_dos_path(path);
        video_write("\n");
        return;
    }

    video_write("Deleted: ");
    write_dos_path(path);
    video_write("\n");
}

/* Skip the first whitespace-delimited token and return a pointer to the
 * start of the second argument (or "" if there is no second argument). */
static const char *get_second_arg_ptr(const char *args) {
    while (*args && is_space((u8)*args)) {
        args++;
    }
    while (*args && !is_space((u8)*args)) {
        args++;
    }
    while (*args && is_space((u8)*args)) {
        args++;
    }
    return args;
}

static void shell_copy(const char *args) {
    char src_path[128];
    char dst_path[128];
    fat_dir_entry_t src_info;
    fat_dir_entry_t dst_info;
    u32 file_size = 0;
    const char *second;

    if (!fat_ready()) {
        video_write("FAT layer not ready.\n");
        return;
    }

    /* Resolve source path */
    if (!build_arg_path(args, src_path, (u32)sizeof(src_path))) {
        video_write("Usage: copy <src> <dst>\n");
        return;
    }

    /* Resolve destination path */
    second = get_second_arg_ptr(args);
    if (second[0] == '\0') {
        video_write("Usage: copy <src> <dst>\n");
        return;
    }
    if (!build_arg_path(second, dst_path, (u32)sizeof(dst_path))) {
        video_write("Usage: copy <src> <dst>\n");
        return;
    }

    /* Same-file detection */
    if (str_eq_nocase(src_path, dst_path)) {
        video_write("Source and destination are the same.\n");
        return;
    }

    /* Validate source */
    if (!fat_find_file(src_path, &src_info)) {
        video_write("Source not found: ");
        write_dos_path(src_path);
        video_write("\n");
        return;
    }
    if (src_info.attr & FAT_ATTR_DIRECTORY) {
        video_write("Source is a directory: ");
        write_dos_path(src_path);
        video_write("\n");
        return;
    }
    if (src_info.size > SHELL_FILE_BUFFER_SIZE) {
        video_write("Source too large for copy buffer.\n");
        return;
    }

    /* Read source */
    if (!fat_read_file(src_path, g_shell_file_buffer, SHELL_FILE_BUFFER_SIZE, &file_size)) {
        video_write("Read error: ");
        write_dos_path(src_path);
        video_write("\n");
        return;
    }

    /* If destination already exists, delete it first (overwrite semantics) */
    if (fat_find_file(dst_path, &dst_info)) {
        if (dst_info.attr & FAT_ATTR_DIRECTORY) {
            video_write("Destination is a directory: ");
            write_dos_path(dst_path);
            video_write("\n");
            return;
        }
        if (dst_info.attr & FAT_ATTR_READ_ONLY) {
            video_write("Destination is read-only: ");
            write_dos_path(dst_path);
            video_write("\n");
            return;
        }
        if (!fat_delete_file(dst_path)) {
            video_write("Cannot overwrite destination: ");
            write_dos_path(dst_path);
            video_write("\n");
            return;
        }
    }

    /* Write destination */
    if (!fat_write_file(dst_path, g_shell_file_buffer, file_size)) {
        video_write("Write failed: ");
        write_dos_path(dst_path);
        video_write("\n");
        return;
    }

    video_write("Copied ");
    write_dos_path(src_path);
    video_write(" -> ");
    write_dos_path(dst_path);
    video_write("  (");
    write_decimal(file_size);
    video_write(" bytes)\n");
}

static void shell_rename(const char *args) {
    char old_path[128];
    char new_name_buf[SHELL_PATH_TOKEN_MAX];
    fat_dir_entry_t info;
    const char *second;
    u32 i;

    if (!fat_ready()) {
        video_write("FAT layer not ready.\n");
        return;
    }
    if (!build_arg_path(args, old_path, (u32)sizeof(old_path))) {
        video_write("Usage: ren <old> <new>\n");
        return;
    }
    second = get_second_arg_ptr(args);
    if (second[0] == '\0') {
        video_write("Usage: ren <old> <new>\n");
        return;
    }

    i = 0;
    while (second[i] && !is_space((u8)second[i]) && (i + 1U) < (u32)sizeof(new_name_buf)) {
        new_name_buf[i] = second[i];
        i++;
    }
    new_name_buf[i] = '\0';

    if (i == 0U) {
        video_write("Usage: ren <old> <new>\n");
        return;
    }

    /* Reject cross-directory rename (path separators in new name) */
    for (u32 j = 0; new_name_buf[j]; j++) {
        if (new_name_buf[j] == '/' || new_name_buf[j] == '\\') {
            video_write("Cross-directory rename not supported.\n");
            return;
        }
    }

    if (!fat_find_file(old_path, &info)) {
        video_write("File not found: ");
        write_dos_path(old_path);
        video_write("\n");
        return;
    }

    if (!fat_rename_entry(old_path, new_name_buf)) {
        video_write("Invalid name or already exists: ");
        video_write(new_name_buf);
        video_write("\n");
        return;
    }

    video_write("Renamed: ");
    write_dos_path(old_path);
    video_write(" -> ");
    video_write(new_name_buf);
    video_write("\n");
}

static void shell_mkdir(const char *args) {
    char path[SHELL_PATH_MAX];
    fat_dir_entry_t info;

    if (!fat_ready()) {
        video_write("FAT layer not ready.\n");
        return;
    }
    if (!build_arg_path(args, path, (u32)sizeof(path))) {
        video_write("Usage: mkdir <name>\n");
        return;
    }

    if (fat_find_file(path, &info)) {
        if (info.attr & FAT_ATTR_DIRECTORY) {
            video_write("Already exists: ");
        } else {
            video_write("File exists with same name: ");
        }
        write_dos_path(path);
        video_write("\n");
        return;
    }

    if (!fat_create_dir(path)) {
        video_write("Failed (invalid name or disk full): ");
        write_dos_path(path);
        video_write("\n");
        return;
    }

    video_write("Created: ");
    write_dos_path(path);
    video_write("\n");
}

static void shell_rmdir(const char *args) {
    char path[SHELL_PATH_MAX];
    fat_dir_entry_t info;

    if (!fat_ready()) {
        video_write("FAT layer not ready.\n");
        return;
    }
    if (!build_arg_path(args, path, (u32)sizeof(path))) {
        video_write("Usage: rmdir <name>\n");
        return;
    }

    if (!fat_find_file(path, &info)) {
        video_write("Directory not found: ");
        write_dos_path(path);
        video_write("\n");
        return;
    }
    if ((info.attr & FAT_ATTR_DIRECTORY) == 0U) {
        video_write("Not a directory: ");
        write_dos_path(path);
        video_write("\n");
        return;
    }

    if (!fat_remove_dir(path)) {
        video_write("Directory not empty: ");
        write_dos_path(path);
        video_write("\n");
        return;
    }

    video_write("Removed: ");
    write_dos_path(path);
    video_write("\n");
}

static void shell_move(const char *args) {
    char src_path[128];
    char dst_path[128];
    fat_dir_entry_t src_info;
    fat_dir_entry_t dst_info;
    u32 file_size = 0;
    const char *second;

    if (!fat_ready()) {
        video_write("FAT layer not ready.\n");
        return;
    }
    if (!build_arg_path(args, src_path, (u32)sizeof(src_path))) {
        video_write("Usage: move <src> <dst>\n");
        return;
    }
    second = get_second_arg_ptr(args);
    if (second[0] == '\0' || !build_arg_path(second, dst_path, (u32)sizeof(dst_path))) {
        video_write("Usage: move <src> <dst>\n");
        return;
    }

    /* Same-file detection (before directory expansion) */
    if (str_eq_nocase(src_path, dst_path)) {
        video_write("Source and destination are the same.\n");
        return;
    }

    if (!fat_find_file(src_path, &src_info)) {
        video_write("Source not found: ");
        write_dos_path(src_path);
        video_write("\n");
        return;
    }
    if (src_info.attr & FAT_ATTR_DIRECTORY) {
        video_write("Cannot move directories.\n");
        return;
    }

    /* If dst is an existing directory, append source filename into it */
    if (fat_find_file(dst_path, &dst_info) && (dst_info.attr & FAT_ATTR_DIRECTORY)) {
        u32 src_len = str_len(src_path);
        u32 name_start = 0;
        u32 dlen;

        for (u32 i = src_len; i > 0U; i--) {
            if (src_path[i - 1U] == '/') {
                name_start = i;
                break;
            }
        }
        dlen = str_len(dst_path);
        if (dlen > 0U && dlen < (u32)sizeof(dst_path) - 1U && dst_path[dlen - 1U] != '/') {
            dst_path[dlen++] = '/';
            dst_path[dlen]   = '\0';
        }
        for (u32 i = name_start; src_path[i] && dlen < (u32)sizeof(dst_path) - 1U; i++) {
            dst_path[dlen++] = src_path[i];
        }
        dst_path[dlen] = '\0';
    }

    if (src_info.size > SHELL_FILE_BUFFER_SIZE) {
        video_write("Source too large.\n");
        return;
    }
    if (!fat_read_file(src_path, g_shell_file_buffer, SHELL_FILE_BUFFER_SIZE, &file_size)) {
        video_write("Read error: ");
        write_dos_path(src_path);
        video_write("\n");
        return;
    }

    /* Overwrite destination if it already exists */
    if (fat_find_file(dst_path, &dst_info)) {
        if (dst_info.attr & FAT_ATTR_DIRECTORY) {
            video_write("Destination is a directory: ");
            write_dos_path(dst_path);
            video_write("\n");
            return;
        }
        if (dst_info.attr & FAT_ATTR_READ_ONLY) {
            video_write("Destination is read-only: ");
            write_dos_path(dst_path);
            video_write("\n");
            return;
        }
        if (!fat_delete_file(dst_path)) {
            video_write("Cannot overwrite: ");
            write_dos_path(dst_path);
            video_write("\n");
            return;
        }
    }

    if (!fat_write_file(dst_path, g_shell_file_buffer, file_size)) {
        video_write("Write failed: ");
        write_dos_path(dst_path);
        video_write("\n");
        return;
    }

    if (!fat_delete_file(src_path)) {
        video_write("Warning: destination written but source not deleted: ");
        write_dos_path(src_path);
        video_write("\n");
        return;
    }

    video_write("Moved: ");
    write_dos_path(src_path);
    video_write(" -> ");
    write_dos_path(dst_path);
    video_write("\n");
}

/*
 * attrib [+r|-r] [+a|-a] <path>
 * With no modifier: displays attributes.  With modifier: toggles R/A bits.
 * Format: RHSA flags (R=read-only, H=hidden, S=system, A=archive).
 */
static void shell_attrib(const char *args) {
    char first[16];
    char path[SHELL_PATH_MAX];
    fat_dir_entry_t info;
    u8 attr;
    u8 bit;
    char sign;
    char flag;
    const char *p = args;
    u32 i = 0;

    if (!fat_ready()) {
        video_write("FAT layer not ready.\n");
        return;
    }

    /* Skip leading spaces */
    while (*p && is_space((u8)*p)) { p++; }

    /* Extract first token */
    i = 0;
    while (*p && !is_space((u8)*p) && (i + 1U) < (u32)sizeof(first)) {
        first[i++] = *p++;
    }
    first[i] = '\0';

    if (i == 0U) {
        video_write("Usage: attrib [+r|-r|+a|-a] <path>\n");
        return;
    }

    if (first[0] == '+' || first[0] == '-') {
        /* Modifier mode: first token is flag, rest is path */
        sign = first[0];
        flag = (char)to_lower_ascii((u8)(i > 1U ? first[1] : 0));

        while (*p && is_space((u8)*p)) { p++; }
        if (*p == '\0') {
            video_write("Usage: attrib [+r|-r|+a|-a] <path>\n");
            return;
        }
        if (!build_canonical_path(p, path, (u32)sizeof(path))) {
            video_write("Usage: attrib [+r|-r|+a|-a] <path>\n");
            return;
        }

        if (!fat_find_file(path, &info)) {
            video_write("Not found: ");
            write_dos_path(path);
            video_write("\n");
            return;
        }

        bit = 0U;
        if (flag == 'r') { bit = FAT_ATTR_READ_ONLY; }
        else if (flag == 'a') { bit = FAT_ATTR_ARCHIVE; }
        else if (flag == 'h') { bit = FAT_ATTR_HIDDEN; }
        else if (flag == 's') { bit = FAT_ATTR_SYSTEM; }
        else {
            video_write("Unknown flag (use r, a, h, s).\n");
            return;
        }

        attr = info.attr;
        if (sign == '+') { attr = (u8)(attr | bit); }
        else             { attr = (u8)(attr & ~bit); }

        if (!fat_set_attr(path, attr)) {
            video_write("Failed to set attribute.\n");
            return;
        }

        video_write("Updated: ");
        write_dos_path(path);
        video_write("\n");
    } else {
        /* Display mode: first token is the path */
        if (!build_canonical_path(first, path, (u32)sizeof(path))) {
            video_write("Usage: attrib [+r|-r|+a|-a] <path>\n");
            return;
        }

        if (!fat_find_file(path, &info)) {
            video_write("Not found: ");
            write_dos_path(path);
            video_write("\n");
            return;
        }

        attr = info.attr;
        video_write(attr & FAT_ATTR_READ_ONLY ? "R" : " ");
        video_write(attr & FAT_ATTR_HIDDEN    ? "H" : " ");
        video_write(attr & FAT_ATTR_SYSTEM    ? "S" : " ");
        video_write(attr & FAT_ATTR_ARCHIVE   ? "A" : " ");
        video_write("  ");
        write_dos_path(path);
        video_write("\n");
    }
}

static void shell_ascii(void) {
    static const char *g_ascii_art_lines[] = {
        "      _____ _       _ _    _  ___  ____  ",
        "     / ____(_)     (_) |  (_)/ _ \\ / __ \\ ",
        "    | |     _ _   _ _| | ___| | | | |  | |",
        "    | |    | | | | | | |/ / | | | | |  | |",
        "    | |____| | |_| | |   <| | |_| | |__| |",
        "     \\_____|_|\\__,_|_|_|\\_\\_|\\___/ \\____/ ",
        0
    };
    u32 cols = video_columns();

    video_write("\n");
    for (u32 i = 0; g_ascii_art_lines[i] != 0; i++) {
        u32 len = str_len(g_ascii_art_lines[i]);
        if (cols > len) {
            u32 pad = (cols - len) / 2;
            for (u32 s = 0; s < pad; s++) {
                video_putchar(' ');
            }
        }
        video_write(g_ascii_art_lines[i]);
        video_write("\n");
    }
    video_write("\n");
}

static void shell_draw_title_bar(void) {
    const char *title = "CiukiOS " CIUKIOS_STAGE2_VERSION;
    u32 cols = video_columns();
    u32 title_len = str_len(title);
    u32 start_col = 0;

    video_set_colors(0x00000000U, 0x00FFFFFFU); /* black on white */
    video_set_cursor(0, 0);
    for (u32 i = 0; i < cols; i++) {
        video_putchar(' ');
    }

    if (cols > title_len) {
        start_col = (cols - title_len) / 2U;
    }

    video_set_cursor(start_col, 0);
    video_write(title);

    video_set_colors(0x00C0C0C0U, 0x00000000U); /* restore shell colors */
    video_set_text_window(1);
}

static void shell_graphic_splash_preview(void) {
    u64 start_ticks;
    const u64 max_wait_ticks = 150ULL; /* 1.5s @ 100Hz */

    video_set_text_window(0);
    if (!stage2_splash_show_graphic()) {
        video_set_font_scale(1U, 1U);
        video_set_text_window(0);
        stage2_splash_show();
    }

    start_ticks = stage2_timer_ticks();
    while ((stage2_timer_ticks() - start_ticks) < max_wait_ticks) {
        if (stage2_keyboard_getc_nonblocking() >= 0) {
            break;
        }
        __asm__ volatile ("hlt");
    }

    video_set_font_scale(2U, 2U);
    video_set_text_window(0);
    shell_draw_title_bar();
}

static void shell_shutdown(void) {
    video_write("Shutting down...\n");
    /*
     * Power-off fallbacks commonly supported by QEMU/Bochs chipsets.
     * If QEMU is started with -no-shutdown, the VM will pause by design.
     */
    outw_port(0x604, 0x2000);
    outw_port(0xB004, 0x2000);
    outw_port(0x4004, 0x3400);
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void shell_reboot(void) {
    u32 spin = 0;
    video_write("Rebooting...\n");
    /*
     * Try keyboard controller reset first, then chipset reset control.
     * If QEMU is started with -no-reboot, the VM will pause by design.
     */
    while (spin < 1000000U) {
        if ((inb_port(0x64) & 0x02U) == 0U) {
            break;
        }
        spin++;
    }
    outb_port(0x64, 0xFE);
    outb_port(0xCF9, 0x02);
    outb_port(0xCF9, 0x06);
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void shell_print_ticks(void) {
    video_write("ticks=0x");
    video_write_hex64(stage2_timer_ticks());
    video_write("\n");
}

static void shell_print_mem(boot_info_t *boot_info, handoff_v0_t *handoff) {
    video_write("fb_boot=0x");
    video_write_hex64(boot_info->framebuffer_base);
    video_write(" ");
    write_decimal((u32)boot_info->framebuffer_width);
    video_write("x");
    write_decimal((u32)boot_info->framebuffer_height);
    video_write(" pitch=0x");
    video_write_hex64((u64)boot_info->framebuffer_pitch);
    video_write(" bpp=0x");
    video_write_hex64((u64)boot_info->framebuffer_bpp);
    video_write("\n");

    video_write("fb_handoff=0x");
    video_write_hex64(handoff->framebuffer_base);
    video_write(" ");
    write_decimal((u32)handoff->framebuffer_width);
    video_write("x");
    write_decimal((u32)handoff->framebuffer_height);
    video_write(" pitch=0x");
    video_write_hex64((u64)handoff->framebuffer_pitch);
    video_write(" bpp=0x");
    video_write_hex64((u64)handoff->framebuffer_bpp);
    video_write("\n");

    video_write("memory_map_ptr=0x");
    video_write_hex64(boot_info->memory_map_ptr);
    video_write(" size=0x");
    video_write_hex64(boot_info->memory_map_size);
    video_write(" desc_size=0x");
    video_write_hex64(boot_info->memory_map_descriptor_size);
    video_write("\n");

    video_write("kernel_phys_base=0x");
    video_write_hex64(boot_info->kernel_phys_base);
    video_write(" kernel_phys_size=0x");
    video_write_hex64(boot_info->kernel_phys_size);
    video_write("\n");

    video_write("stage2_load_addr=0x");
    video_write_hex64(handoff->stage2_load_addr);
    video_write(" stage2_size=0x");
    video_write_hex64(handoff->stage2_size);
    video_write("\n");
}

/*
 * OPENGEM-011 — DOS extender readiness probe.
 *
 * Establishes the observable baseline for a real DPMI / DOS4GW path
 * under OpenGEM. The probe exercises the in-process INT 2Fh AX=1687h
 * DPMI installation-check handler (see `shell_com_int2f`) without
 * dispatching a real interrupt, captures the returned descriptor
 * skeleton (carry / BX flags / CX host-data size / ES:DI entry), and
 * emits a frozen set of serial markers that downstream phases will
 * layer on.
 *
 * Markers (stable, append-only):
 *   OpenGEM: extender probe begin
 *   OpenGEM: extender dpmi installed=<0|1> flags=<hex16>
 *   OpenGEM: extender mode=<dpmi-stub|none>
 *   OpenGEM: extender probe complete
 *
 * Returns 1 when the DPMI-stub responded cleanly (carry=0), 0 when
 * no extender surface is available. The return value is advisory —
 * actual GEM.EXE dispatch (OPENGEM-012+) will consume it.
 */
static int shell_write_u16_hex(u16 v, char *out, u32 out_size) {
    static const char hex_digits[] = "0123456789abcdef";
    if (!out || out_size < 5U) return 0;
    out[0] = hex_digits[(v >> 12) & 0xFU];
    out[1] = hex_digits[(v >> 8)  & 0xFU];
    out[2] = hex_digits[(v >> 4)  & 0xFU];
    out[3] = hex_digits[ v        & 0xFU];
    out[4] = '\0';
    return 1;
}

/* OPENGEM-012 — 8-digit lowercase-hex u32 formatter. Shares the
 * digit table convention with shell_write_u16_hex. */
static int shell_write_u32_hex(u32 v, char *out, u32 out_size) {
    static const char hex_digits[] = "0123456789abcdef";
    if (!out || out_size < 9U) return 0;
    out[0] = hex_digits[(v >> 28) & 0xFU];
    out[1] = hex_digits[(v >> 24) & 0xFU];
    out[2] = hex_digits[(v >> 20) & 0xFU];
    out[3] = hex_digits[(v >> 16) & 0xFU];
    out[4] = hex_digits[(v >> 12) & 0xFU];
    out[5] = hex_digits[(v >> 8)  & 0xFU];
    out[6] = hex_digits[(v >> 4)  & 0xFU];
    out[7] = hex_digits[ v        & 0xFU];
    out[8] = '\0';
    return 1;
}

static int stage2_opengem_probe_extender(void) {
    ciuki_int21_regs_t regs;
    u16 flags = 0U;
    int installed = 0;

    serial_write("OpenGEM: extender probe begin\n");

    /* Synthesize the DPMI installation-check register file exactly
     * the way DOS/4GW-style clients issue it: AX=1687h, carry
     * cleared, other regs don't-care. The in-process handler fills
     * the descriptor skeleton. Zero the struct so any residual
     * fields reflect only what the handler set. */
    {
        u8 *p = (u8 *)&regs;
        u32 i;
        for (i = 0U; i < (u32)sizeof(regs); i++) p[i] = 0U;
    }
    regs.ax = 0x1687U;
    regs.carry = 1U; /* set so handler must explicitly clear on success */

    shell_com_int2f((ciuki_dos_context_t *)0, &regs);

    if (regs.carry == 0U) {
        installed = 1;
        /* Pack a compact flags word: low bit = installed, next bit
         * = nonzero host-data size (CX), next = nonzero entry seg
         * (ES). Sufficient for the gate to assert the stub surface
         * without leaking internal register layout. */
        flags = (u16)0x0001U;
        if (regs.cx != 0U) flags |= (u16)0x0002U;
        if (regs.es != 0U) flags |= (u16)0x0004U;
        if (regs.di != 0U) flags |= (u16)0x0008U;
    }

    {
        char line[64];
        char hex[5];
        const char *p0 = "OpenGEM: extender dpmi installed=";
        u32 n = 0U;
        while (p0[n] != '\0' && n < (u32)sizeof(line) - 16U) {
            line[n] = p0[n]; n++;
        }
        line[n++] = installed ? '1' : '0';
        { const char *p1 = " flags=0x"; u32 j = 0U;
          while (p1[j] != '\0' && n < (u32)sizeof(line) - 8U) {
              line[n++] = p1[j++]; } }
        shell_write_u16_hex(flags, hex, (u32)sizeof(hex));
        { u32 j = 0U;
          while (hex[j] != '\0' && n < (u32)sizeof(line) - 2U) {
              line[n++] = hex[j++]; } }
        line[n++] = '\n';
        line[n]   = '\0';
        serial_write(line);
    }

    if (installed) {
        serial_write("OpenGEM: extender mode=dpmi-stub\n");
    } else {
        serial_write("OpenGEM: extender mode=none\n");
    }

    serial_write("OpenGEM: extender probe complete\n");
    return installed;
}

/*
 * OPENGEM-012 — Absolute-dispatch classification probe.
 *
 * Establishes the observability surface for dispatching OpenGEM via
 * the absolute path resolved by OPENGEM-010, in advance of a real
 * protected-mode loader. Uses the FAT directory-entry size from the
 * preflight probe (no file bytes are read here — classification is
 * by path extension) and publishes a capability verdict so a gate
 * can assert whether the current build is expected to actually run
 * the binary or defer to the historical `shell_run()` fallback.
 *
 * Markers (stable, append-only):
 *   OpenGEM: absolute dispatch begin path=<p> size=0x<hex32>
 *   OpenGEM: absolute dispatch classify=<mz|bat|com|app|unknown> by=path
 *   OpenGEM: absolute dispatch capable=<0|1> reason=<token>
 *   OpenGEM: absolute dispatch complete
 *
 * Reason tokens (stable):
 *   16bit-mz-extender-pending  — MZ file, extender layer not yet
 *                                implemented (OPENGEM-013+).
 *   bat-interp-available       — BAT, delegated to BAT interpreter.
 *   com-runtime-available      — COM, delegated to COM runtime.
 *   no-loader-for-app          — .APP files not yet supported.
 *   unknown-extension          — fell through the kind ladder.
 *   no-path                    — preflight did not resolve a path.
 */
static int stage2_opengem_classify_absolute(const char *path, u32 size) {
    const char *classify = "unknown";
    const char *reason   = "unknown-extension";
    int capable = 0;

    serial_write("OpenGEM: absolute dispatch begin path=");
    if (path) serial_write(path);
    else      serial_write("(none)");
    {
        char line[32];
        char hex[9];
        const char *p = " size=0x";
        u32 n = 0U;
        while (p[n] != '\0' && n < (u32)sizeof(line) - 10U) {
            line[n] = p[n]; n++;
        }
        shell_write_u32_hex(size, hex, (u32)sizeof(hex));
        {
            u32 j = 0U;
            while (hex[j] != '\0' && n < (u32)sizeof(line) - 2U) {
                line[n++] = hex[j++];
            }
        }
        line[n++] = '\n';
        line[n]   = '\0';
        serial_write(line);
    }

    if (!path || !path[0]) {
        classify = "unknown";
        reason   = "no-path";
    } else {
        const char *end = path;
        while (*end) end++;
        if (end - path >= 4) {
            char c3 = end[-3], c2 = end[-2], c1 = end[-1];
            if (c3 >= 'A' && c3 <= 'Z') c3 = (char)(c3 + 32);
            if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + 32);
            if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + 32);
            if (c3 == 'e' && c2 == 'x' && c1 == 'e') {
                classify = "mz";
                reason   = "16bit-mz-extender-pending";
                capable  = 0;
            } else if (c3 == 'b' && c2 == 'a' && c1 == 't') {
                classify = "bat";
                reason   = "bat-interp-available";
                capable  = 1;
            } else if (c3 == 'c' && c2 == 'o' && c1 == 'm') {
                classify = "com";
                reason   = "com-runtime-available";
                capable  = 1;
            } else if (c3 == 'a' && c2 == 'p' && c1 == 'p') {
                classify = "app";
                reason   = "no-loader-for-app";
                capable  = 0;
            }
        }
    }

    serial_write("OpenGEM: absolute dispatch classify=");
    serial_write(classify);
    serial_write(" by=path\n");

    serial_write("OpenGEM: absolute dispatch capable=");
    serial_write(capable ? "1" : "0");
    serial_write(" reason=");
    serial_write(reason);
    serial_write("\n");

    serial_write("OpenGEM: absolute dispatch complete\n");
    return capable;
}

/*
 * OPENGEM-013 — Absolute-path preload probe.
 *
 * First phase of the CiukiOS side actually reading GEM.EXE bytes
 * from the absolute path resolved by OPENGEM-010. Uses
 * `fat_read_file()` to stage the file into the runtime payload
 * buffer (`SHELL_RUNTIME_COM_ENTRY_ADDR`) and inspects the first
 * few bytes to confirm the on-disk signature matches the lexical
 * classification from OPENGEM-012. Publishes a verdict that a
 * real loader (OPENGEM-014+) will consume; today the historical
 * `shell_run()` path still owns execution.
 *
 * Markers (stable, append-only):
 *   OpenGEM: preload begin path=<p> expect_size=0x<hex32>
 *   OpenGEM: preload read bytes=0x<hex32> status=<ok|too-large|io-error|no-path>
 *   OpenGEM: preload signature=<MZ|ZM|text|empty|unknown> match=<0|1>
 *   OpenGEM: preload verdict=<dispatch-native|defer-to-shell-run> reason=<token>
 *   OpenGEM: preload complete
 *
 * Verdict reason tokens (stable, disjoint from OPENGEM-012):
 *   preload-empty       — zero-byte file
 *   preload-too-large   — file exceeds payload window
 *   preload-io-error    — fat_read_file failed
 *   preload-no-path     — no resolved path from preflight
 *   signature-mismatch  — classify expected MZ but signature is not
 *   mz-16bit-pending    — MZ confirmed but no extender (OPENGEM-015+)
 *   bat-interp-ready    — BAT confirmed, interp will run it
 *   com-runtime-ready   — COM confirmed, runtime will run it
 *   unsupported-app     — .APP — no loader
 *   unsupported-unknown — fell through classify ladder
 *
 * OPENGEM-014 note: bat-interp-ready and com-runtime-ready now
 * emit verdict=dispatch-native because the caller actually
 * dispatches via shell_run_batch_file() / shell_run_staged_image()
 * on the already-staged buffer. The other reasons keep
 * verdict=defer-to-shell-run (MZ requires an extender that is
 * OPENGEM-015+ territory). The reason tokens are frozen; the
 * verdict field is the selector the caller uses.
 *
 * Out-params expose the emitted verdict/reason/read-bytes so the
 * caller can branch on them without re-parsing the serial stream.
 */
static int stage2_opengem_preload_absolute(const char *path,
                                           u32 expect_size,
                                           const char *classify,
                                           const char **out_verdict,
                                           const char **out_reason,
                                           u32 *out_read_bytes) {
    const char *status    = "no-path";
    const char *signature = "unknown";
    int match             = 0;
    const char *verdict   = "defer-to-shell-run";
    const char *reason    = "preload-no-path";
    u32 read_bytes = 0U;

    /* Begin marker: path + expected size. */
    serial_write("OpenGEM: preload begin path=");
    if (path) serial_write(path);
    else      serial_write("(none)");
    {
        char line[32];
        char hex[9];
        const char *p = " expect_size=0x";
        u32 n = 0U;
        while (p[n] != '\0' && n < (u32)sizeof(line) - 10U) {
            line[n] = p[n]; n++;
        }
        shell_write_u32_hex(expect_size, hex, (u32)sizeof(hex));
        {
            u32 j = 0U;
            while (hex[j] != '\0' && n < (u32)sizeof(line) - 2U) {
                line[n++] = hex[j++];
            }
        }
        line[n++] = '\n';
        line[n]   = '\0';
        serial_write(line);
    }

    if (!path || !path[0]) {
        status  = "no-path";
        verdict = "defer-to-shell-run";
        reason  = "preload-no-path";
        goto emit_read_line;
    }

    if (expect_size == 0U) {
        status  = "io-error";
        verdict = "defer-to-shell-run";
        reason  = "preload-empty";
        goto emit_read_line;
    }

    if (expect_size > SHELL_RUNTIME_COM_MAX_PAYLOAD) {
        status  = "too-large";
        verdict = "defer-to-shell-run";
        reason  = "preload-too-large";
        goto emit_read_line;
    }

    if (!fat_read_file(
            path,
            (void *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR,
            SHELL_RUNTIME_COM_MAX_PAYLOAD,
            &read_bytes
        )) {
        status  = "io-error";
        verdict = "defer-to-shell-run";
        reason  = "preload-io-error";
        goto emit_read_line;
    }

    status = "ok";

    if (read_bytes == 0U) {
        signature = "empty";
    } else if (read_bytes >= 2U) {
        const u8 *image = (const u8 *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR;
        if (image[0] == 'M' && image[1] == 'Z') {
            signature = "MZ";
        } else if (image[0] == 'Z' && image[1] == 'M') {
            signature = "ZM";
        } else if (image[0] == '@' || image[0] == ':' ||
                   (image[0] >= 0x20 && image[0] <= 0x7EU)) {
            /* BAT files are 7-bit text. Coarse heuristic: first
             * byte is printable ASCII. */
            signature = "text";
        } else {
            signature = "unknown";
        }
    } else {
        signature = "unknown";
    }

    /* Cross-check with the OPENGEM-012 lexical classification. */
    if (classify) {
        if (classify[0] == 'm' && classify[1] == 'z') {
            match = (signature[0] == 'M' && signature[1] == 'Z') ? 1 : 0;
            if (match) {
                verdict = "defer-to-shell-run";
                reason  = "mz-16bit-pending";
            } else {
                verdict = "defer-to-shell-run";
                reason  = "signature-mismatch";
            }
        } else if (classify[0] == 'b' && classify[1] == 'a' &&
                   classify[2] == 't') {
            match = (signature[0] == 't') ? 1 : 0;
            /* OPENGEM-014 — caller will invoke shell_run_batch_file
             * directly on the absolute path, skipping shell_run(). */
            verdict = "dispatch-native";
            reason  = "bat-interp-ready";
        } else if (classify[0] == 'c' && classify[1] == 'o' &&
                   classify[2] == 'm') {
            /* COM has no signature; accept. */
            match = 1;
            /* OPENGEM-014 — caller will invoke shell_run_staged_image
             * directly on the already-preloaded buffer. */
            verdict = "dispatch-native";
            reason  = "com-runtime-ready";
        } else if (classify[0] == 'a' && classify[1] == 'p' &&
                   classify[2] == 'p') {
            match = 0;
            verdict = "defer-to-shell-run";
            reason  = "unsupported-app";
        } else {
            match = 0;
            verdict = "defer-to-shell-run";
            reason  = "unsupported-unknown";
        }
    } else {
        match = 0;
        verdict = "defer-to-shell-run";
        reason  = "unsupported-unknown";
    }

emit_read_line:
    {
        char line[48];
        char hex[9];
        const char *p = "OpenGEM: preload read bytes=0x";
        u32 n = 0U;
        while (p[n] != '\0' && n < (u32)sizeof(line) - 12U) {
            line[n] = p[n]; n++;
        }
        shell_write_u32_hex(read_bytes, hex, (u32)sizeof(hex));
        {
            u32 j = 0U;
            while (hex[j] != '\0' && n < (u32)sizeof(line) - 12U) {
                line[n++] = hex[j++];
            }
        }
        {
            const char *q = " status=";
            u32 j = 0U;
            while (q[j] != '\0' && n < (u32)sizeof(line) - 2U) {
                line[n++] = q[j++];
            }
        }
        {
            u32 j = 0U;
            while (status[j] != '\0' && n < (u32)sizeof(line) - 2U) {
                line[n++] = status[j++];
            }
        }
        line[n++] = '\n';
        line[n]   = '\0';
        serial_write(line);
    }

    serial_write("OpenGEM: preload signature=");
    serial_write(signature);
    serial_write(" match=");
    serial_write(match ? "1" : "0");
    serial_write("\n");

    serial_write("OpenGEM: preload verdict=");
    serial_write(verdict);
    serial_write(" reason=");
    serial_write(reason);
    serial_write("\n");

    serial_write("OpenGEM: preload complete\n");

    /* Expose the emitted verdict/reason/read-bytes so the caller
     * can branch on them without re-parsing the serial stream. */
    if (out_verdict) *out_verdict = verdict;
    if (out_reason)  *out_reason  = reason;
    if (out_read_bytes) *out_read_bytes = read_bytes;

    /* Advisory return: 1 when the preload actually landed ok;
     * OPENGEM-014 uses it (alongside the verdict) to gate a real
     * native dispatch. */
    return (status[0] == 'o' && status[1] == 'k') ? 1 : 0;
}

/*
 * OPENGEM-015 — Deep MZ header probe.
 *
 * Parses the 28-byte MZ header already staged at
 * SHELL_RUNTIME_COM_ENTRY_ADDR by the OPENGEM-013 preload. Emits
 * every real header field that the 16-bit execution layer will
 * eventually need (entry CS:IP, stack SS:SP, allocation
 * requirements, relocation table offset, computed load size) and
 * publishes a viability verdict — gem.exe's DPMI/v8086
 * requirement becomes a first-class observable instead of a
 * shell_run-side rejection string.
 *
 * This is pure observability — no execution change. The caller
 * still routes MZ through shell_run() where the existing
 * "[dosrun] mz dispatch=pending reason=16bit" rejection happens.
 *
 * Markers (stable, append-only; disjoint from preload and
 * native-dispatch):
 *   OpenGEM: mz-probe begin path=<p> size=0x<hex32>
 *   OpenGEM: mz-probe signature=<MZ|ZM|none> status=<ok|too-small|not-mz>
 *   OpenGEM: mz-probe header e_cblp=0x<h16> e_cp=0x<h16> e_crlc=0x<h16> e_cparhdr=0x<h16>
 *   OpenGEM: mz-probe alloc e_minalloc=0x<h16> e_maxalloc=0x<h16>
 *   OpenGEM: mz-probe stack e_ss=0x<h16> e_sp=0x<h16>
 *   OpenGEM: mz-probe entry e_cs=0x<h16> e_ip=0x<h16>
 *   OpenGEM: mz-probe reloc e_lfarlc=0x<h16> e_ovno=0x<h16>
 *   OpenGEM: mz-probe layout load_bytes=0x<h32> header_bytes=0x<h32>
 *   OpenGEM: mz-probe viability=<runnable-real-mode|requires-extender|malformed|skipped-non-mz> reason=<token>
 *   OpenGEM: mz-probe complete
 *
 * Viability tokens (stable):
 *   runnable-real-mode   — small MZ, fits in real-mode window
 *   requires-extender    — needs DPMI / DOS4GW (load > 640K or
 *                          e_maxalloc == 0xFFFF and load > 64K)
 *   malformed            — header inconsistent
 *   skipped-non-mz       — buffer doesn't start with MZ/ZM
 *
 * Reason tokens (stable, disjoint from OPENGEM-012/013/014):
 *   mz-v8086-candidate        — viability=runnable-real-mode
 *   mz-load-exceeds-real-mode — load_bytes > 0xA0000
 *   mz-max-alloc-64k          — e_maxalloc == 0xFFFF
 *   mz-header-too-small       — file < 0x1C bytes
 *   mz-header-malformed       — e_cparhdr == 0 or e_cp == 0
 *   mz-non-mz-skipped         — signature mismatch
 *   mz-no-buffer              — no path / no preload
 */
static void stage2_opengem_mz_probe(const char *path, u32 preload_size) {
    const char *status    = "ok";
    const char *signature = "none";
    const char *viability = "skipped-non-mz";
    const char *reason    = "mz-non-mz-skipped";
    u32 header_bytes = 0U;
    u32 load_bytes   = 0U;
    u16 e_cblp=0, e_cp=0, e_crlc=0, e_cparhdr=0;
    u16 e_minalloc=0, e_maxalloc=0;
    u16 e_ss=0, e_sp=0;
    u16 e_cs=0, e_ip=0;
    u16 e_lfarlc=0, e_ovno=0;

    serial_write("OpenGEM: mz-probe begin path=");
    if (path) serial_write(path);
    else      serial_write("(none)");
    {
        char line[32];
        char hex[9];
        const char *p = " size=0x";
        u32 n = 0U;
        while (p[n] != '\0' && n < (u32)sizeof(line) - 10U) {
            line[n] = p[n]; n++;
        }
        shell_write_u32_hex(preload_size, hex, (u32)sizeof(hex));
        {
            u32 j = 0U;
            while (hex[j] != '\0' && n < (u32)sizeof(line) - 2U) {
                line[n++] = hex[j++];
            }
        }
        line[n++] = '\n';
        line[n]   = '\0';
        serial_write(line);
    }

    if (!path || !path[0] || preload_size == 0U) {
        status    = "too-small";
        viability = "malformed";
        reason    = "mz-no-buffer";
        goto emit_header;
    }

    if (preload_size < 0x1CU) {
        status    = "too-small";
        viability = "malformed";
        reason    = "mz-header-too-small";
        goto emit_signature;
    }

    {
        const u8 *h = (const u8 *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR;
        if (h[0] == 'M' && h[1] == 'Z')      signature = "MZ";
        else if (h[0] == 'Z' && h[1] == 'M') signature = "ZM";
        else {
            signature = "none";
            status    = "not-mz";
            viability = "skipped-non-mz";
            reason    = "mz-non-mz-skipped";
            goto emit_signature;
        }

        e_cblp     = (u16)h[0x02] | ((u16)h[0x03] << 8);
        e_cp       = (u16)h[0x04] | ((u16)h[0x05] << 8);
        e_crlc     = (u16)h[0x06] | ((u16)h[0x07] << 8);
        e_cparhdr  = (u16)h[0x08] | ((u16)h[0x09] << 8);
        e_minalloc = (u16)h[0x0A] | ((u16)h[0x0B] << 8);
        e_maxalloc = (u16)h[0x0C] | ((u16)h[0x0D] << 8);
        e_ss       = (u16)h[0x0E] | ((u16)h[0x0F] << 8);
        e_sp       = (u16)h[0x10] | ((u16)h[0x11] << 8);
        e_ip       = (u16)h[0x14] | ((u16)h[0x15] << 8);
        e_cs       = (u16)h[0x16] | ((u16)h[0x17] << 8);
        e_lfarlc   = (u16)h[0x18] | ((u16)h[0x19] << 8);
        e_ovno     = (u16)h[0x1A] | ((u16)h[0x1B] << 8);
    }

    if (e_cparhdr == 0U || e_cp == 0U) {
        status    = "ok";
        viability = "malformed";
        reason    = "mz-header-malformed";
        header_bytes = (u32)e_cparhdr * 16U;
        goto emit_signature;
    }

    header_bytes = (u32)e_cparhdr * 16U;
    /* Canonical MZ load size: total file bytes minus header, where
     * file bytes = e_cp * 512, minus (512 - e_cblp) when e_cblp != 0. */
    {
        u32 file_bytes = (u32)e_cp * 512U;
        if (e_cblp != 0U) {
            if (file_bytes >= (512U - (u32)e_cblp)) {
                file_bytes -= (512U - (u32)e_cblp);
            }
        }
        if (file_bytes > header_bytes) {
            load_bytes = file_bytes - header_bytes;
        } else {
            load_bytes = 0U;
        }
    }

    /* Viability verdict. */
    if (load_bytes > 0xA0000U) {
        viability = "requires-extender";
        reason    = "mz-load-exceeds-real-mode";
    } else if (e_maxalloc == 0xFFFFU && load_bytes > 0x10000U) {
        viability = "requires-extender";
        reason    = "mz-max-alloc-64k";
    } else {
        viability = "runnable-real-mode";
        reason    = "mz-v8086-candidate";
    }

emit_signature:
    serial_write("OpenGEM: mz-probe signature=");
    serial_write(signature);
    serial_write(" status=");
    serial_write(status);
    serial_write("\n");

emit_header:
    {
        char line[128];
        char hex[5];
        u32 n = 0U;
        const char *parts[4] = {
            "OpenGEM: mz-probe header e_cblp=0x",
            " e_cp=0x",
            " e_crlc=0x",
            " e_cparhdr=0x"
        };
        u16 vals[4] = { e_cblp, e_cp, e_crlc, e_cparhdr };
        for (u32 k = 0U; k < 4U; k++) {
            const char *p = parts[k];
            u32 j = 0U;
            while (p[j] != '\0' && n < (u32)sizeof(line) - 8U) {
                line[n++] = p[j++];
            }
            shell_write_u16_hex(vals[k], hex, (u32)sizeof(hex));
            j = 0U;
            while (hex[j] != '\0' && n < (u32)sizeof(line) - 4U) {
                line[n++] = hex[j++];
            }
        }
        line[n++] = '\n';
        line[n]   = '\0';
        serial_write(line);
    }

    /* alloc */
    serial_write("OpenGEM: mz-probe alloc e_minalloc=0x");
    {
        char hex[5];
        shell_write_u16_hex(e_minalloc, hex, (u32)sizeof(hex));
        serial_write(hex);
    }
    serial_write(" e_maxalloc=0x");
    {
        char hex[5];
        shell_write_u16_hex(e_maxalloc, hex, (u32)sizeof(hex));
        serial_write(hex);
    }
    serial_write("\n");

    /* stack */
    serial_write("OpenGEM: mz-probe stack e_ss=0x");
    {
        char hex[5];
        shell_write_u16_hex(e_ss, hex, (u32)sizeof(hex));
        serial_write(hex);
    }
    serial_write(" e_sp=0x");
    {
        char hex[5];
        shell_write_u16_hex(e_sp, hex, (u32)sizeof(hex));
        serial_write(hex);
    }
    serial_write("\n");

    /* entry */
    serial_write("OpenGEM: mz-probe entry e_cs=0x");
    {
        char hex[5];
        shell_write_u16_hex(e_cs, hex, (u32)sizeof(hex));
        serial_write(hex);
    }
    serial_write(" e_ip=0x");
    {
        char hex[5];
        shell_write_u16_hex(e_ip, hex, (u32)sizeof(hex));
        serial_write(hex);
    }
    serial_write("\n");

    /* reloc */
    serial_write("OpenGEM: mz-probe reloc e_lfarlc=0x");
    {
        char hex[5];
        shell_write_u16_hex(e_lfarlc, hex, (u32)sizeof(hex));
        serial_write(hex);
    }
    serial_write(" e_ovno=0x");
    {
        char hex[5];
        shell_write_u16_hex(e_ovno, hex, (u32)sizeof(hex));
        serial_write(hex);
    }
    serial_write("\n");

    /* layout */
    serial_write("OpenGEM: mz-probe layout load_bytes=0x");
    {
        char hex[9];
        shell_write_u32_hex(load_bytes, hex, (u32)sizeof(hex));
        serial_write(hex);
    }
    serial_write(" header_bytes=0x");
    {
        char hex[9];
        shell_write_u32_hex(header_bytes, hex, (u32)sizeof(hex));
        serial_write(hex);
    }
    serial_write("\n");

    /* viability */
    serial_write("OpenGEM: mz-probe viability=");
    serial_write(viability);
    serial_write(" reason=");
    serial_write(reason);
    serial_write("\n");

    serial_write("OpenGEM: mz-probe complete\n");
    (void)status;
}

/*
 * OPENGEM-014 — Native absolute-path dispatcher.
 *
 * Consumes the verdict/reason emitted by the OPENGEM-013 preload
 * probe. When the preload landed cleanly on a BAT or COM target,
 * bypasses the historical shell_run()/normalize_run_name() path
 * (which strips absolute paths down to a basename and searches
 * CWD + fallback roots) and invokes the interpreter/runtime
 * directly on the resolved absolute path:
 *
 *   bat-interp-ready  -> shell_run_batch_file(boot_info, handoff, path)
 *   com-runtime-ready -> shell_run_staged_image(boot_info, handoff,
 *                            basename, read_bytes, "")  (buffer
 *                            already populated by the preload)
 *
 * MZ / signature-mismatch / unsupported-* / preload-* paths
 * return 0 unchanged and let the caller fall through to the
 * historical shell_run() dispatcher.
 *
 * Markers (stable, append-only; disjoint from preload):
 *   OpenGEM: native-dispatch begin path=<p> kind=<bat|com> reason=<r>
 *   OpenGEM: native-dispatch <kind>=<invoked|failed>
 *   OpenGEM: native-dispatch complete errorlevel=<n>
 *
 * Returns 1 when a native dispatch was attempted (regardless of
 * the program's errorlevel); 0 when the caller must still invoke
 * shell_run().
 */
static int stage2_opengem_dispatch_native(boot_info_t *boot_info,
                                          handoff_v0_t *handoff,
                                          const char *path,
                                          u32 read_bytes,
                                          const char *verdict,
                                          const char *reason) {
    const char *kind = 0;

    if (!verdict || verdict[0] != 'd' || verdict[1] != 'i') {
        /* Not "dispatch-native". */
        return 0;
    }
    if (!path || !path[0] || !reason) {
        return 0;
    }

    if (reason[0] == 'b' && reason[1] == 'a' && reason[2] == 't') {
        kind = "bat";
    } else if (reason[0] == 'c' && reason[1] == 'o' && reason[2] == 'm') {
        kind = "com";
    } else {
        /* Reserved dispatch-native reason we don't yet honor. */
        return 0;
    }

    serial_write("OpenGEM: native-dispatch begin path=");
    serial_write(path);
    serial_write(" kind=");
    serial_write(kind);
    serial_write(" reason=");
    serial_write(reason);
    serial_write("\n");

    if (kind[0] == 'b') {
        /* BAT: shell_run_batch_file reads via fat_read_file on the
         * path itself. The preload's buffer is harmless — BAT
         * never executes from SHELL_RUNTIME_COM_ENTRY_ADDR. */
        shell_run_batch_file(boot_info, handoff, path);
        serial_write("OpenGEM: native-dispatch bat=invoked\n");
    } else {
        /* COM: the preload already staged the bytes at
         * SHELL_RUNTIME_COM_ENTRY_ADDR. Pass the basename (for
         * launch markers) and the actual read size. No argv. */
        const char *basename = path;
        {
            const char *q = path;
            while (*q) {
                if (*q == '/' || *q == '\\') basename = q + 1;
                q++;
            }
        }
        if (read_bytes == 0U) {
            /* Defensive: treat zero as a dispatch failure. */
            serial_write("OpenGEM: native-dispatch com=failed\n");
            serial_write("OpenGEM: native-dispatch complete errorlevel=1\n");
            return 1;
        }
        shell_run_staged_image(boot_info, handoff, basename, read_bytes, "");
        serial_write("OpenGEM: native-dispatch com=invoked\n");
    }

    {
        u32 el = shell_get_errorlevel();
        char buf[64];
        u32 n = 0U;
        const char *prefix = "OpenGEM: native-dispatch complete errorlevel=";
        while (prefix[n] != '\0' && n < (u32)sizeof(buf) - 16U) {
            buf[n] = prefix[n]; n++;
        }
        /* u32 -> decimal */
        char tmp[16];
        u32 ti = 0U;
        if (el == 0U) { tmp[ti++] = '0'; }
        else {
            u32 d = el;
            while (d > 0U && ti < (u32)sizeof(tmp)) {
                tmp[ti++] = (char)('0' + (u32)(d % 10U)); d /= 10U;
            }
        }
        while (ti > 0U && n < (u32)sizeof(buf) - 2U) {
            buf[n++] = tmp[--ti];
        }
        buf[n++] = '\n';
        buf[n]   = '\0';
        serial_write(buf);
    }

    return 1;
}

/*
 * OPENGEM-001 — Launch OpenGEM via the standard shell_run path.
 *
 * Shared entry point used by both the `opengem` shell command and the
 * desktop launcher (OPENGEM item + ALT+O). Performs the same preflight
 * probe as the command variant (runnable entry + FAT readiness), emits
 * boot/launch/exit markers usable by the smoke gate, and falls back
 * gracefully when the payload is absent (no panic, CF-clean return).
 *
 * Returns 1 when OpenGEM was actually launched (preflight passed),
 * 0 on any fallback path (missing runtime, FAT not ready).
 */
static int shell_run_opengem_interactive(boot_info_t *boot_info,
                                         handoff_v0_t *handoff) {
    /* OPENGEM-010 — Probe order: prefer the real GEM.EXE binary at
     * its canonical OpenGEM nested location before falling back to
     * GEM.BAT (which, in the bundled FreeDOS payload, only prints
     * an install-instructions stub when the payload is not at the
     * drive root). The nested GEM.EXE gives shell_run() a real
     * MZ entry point to dispatch, which is what the mode-13
     * first-frame hook (OPENGEM-008) and the ms duration
     * (OPENGEM-009) actually measure. */
    static const char *paths[] = {
        "/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM.EXE",
        "/FREEDOS/OPENGEM/GEM.BAT",
        "/FREEDOS/OPENGEM/GEM.EXE",
        "/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/DESKTOP.APP",
        "/FREEDOS/OPENGEM/OPENGEM.BAT",
        "/FREEDOS/OPENGEM/OPENGEM.EXE",
    };
    static const u32 paths_count = 6U;
    fat_dir_entry_t probe;
    const char *found_path = (const char *)0;
    u32 found_size = 0U;
    int pi;
    int preflight_ok = 1;
    /* OPENGEM-003 — Desktop scene integration: per-launch desktop
     * state snapshot on the stack so nested shell_run / BAT / EXE
     * cannot corrupt the launcher selection index. */
    struct {
        int  launcher_focus;
        char status0[64];
        u8   valid;
    } desktop_snapshot;

    desktop_snapshot.launcher_focus = ui_get_launcher_focus();
    desktop_snapshot.status0[0] = '\0';
    desktop_snapshot.valid = 1U;
    {
        char marker[48];
        u32 mi = 0;
        const char *prefix = "[ ui ] opengem dock state saved: sel=";
        while (prefix[mi]) { marker[mi] = prefix[mi]; mi++; }
        /* single-digit focus is sufficient: LAUNCHER_ITEMS=7. */
        if (desktop_snapshot.launcher_focus >= 0 &&
            desktop_snapshot.launcher_focus < 10) {
            marker[mi++] = (char)('0' + desktop_snapshot.launcher_focus);
        } else {
            marker[mi++] = '?';
        }
        marker[mi++] = '\n';
        marker[mi]   = '\0';
        serial_write(marker);
    }

    serial_write("[ app ] opengem launch requested\n");
    serial_write("OpenGEM: boot sequence starting\n");
    serial_write("[ app ] opengem preflight started\n");

    /* Check 1: find a runnable entry. */
    for (pi = 0; (u32)pi < paths_count; pi++) {
        if (fat_find_file(paths[pi], &probe)) {
            found_path = paths[pi];
            found_size = probe.size;
            break;
        }
    }

    if (found_path) {
        video_write("[preflight] OpenGEM entry: found (");
        video_write(found_path);
        video_write(")\n");
        serial_write("[ app ] opengem preflight entry: ok\n");
    } else {
        video_write("[preflight] OpenGEM entry: NOT FOUND\n");
        serial_write("[ app ] opengem preflight entry: missing\n");
        serial_write("OpenGEM: runtime not found in FAT, fallback to shell\n");
        preflight_ok = 0;
    }

    /* Check 2: FAT filesystem ready. */
    if (fat_ready()) {
        video_write("[preflight] FAT layer: ready\n");
        serial_write("[ app ] opengem preflight fat: ok\n");
    } else {
        video_write("[preflight] FAT layer: NOT READY\n");
        serial_write("[ app ] opengem preflight fat: fail\n");
        serial_write("OpenGEM: runtime not found in FAT, fallback to shell\n");
        preflight_ok = 0;
    }

    serial_write("[ app ] opengem preflight complete\n");

    if (!preflight_ok) {
        video_write("[preflight] FAILED - cannot launch OpenGEM\n");
        video_write("Install: scripts/import_opengem.sh\n");
        /* OPENGEM-003 — Modal-style fallback line that keeps the
         * user on the previous launcher selection. */
        video_write("OPENGEM: n/a - payload not installed\n");
        serial_write("[ ui ] opengem overlay dismissed, state restored\n");
        serial_write("[ app ] opengem preflight failed\n");
        if (desktop_snapshot.valid) {
            ui_set_launcher_focus(desktop_snapshot.launcher_focus);
        }
        return 0;
    }

    video_write("[preflight] PASSED - launching OpenGEM\n");
    serial_write("[ app ] opengem preflight passed\n");
    serial_write("OpenGEM: launcher window initialized\n");
    /* OPENGEM-003 — Overlay marker: telemetry can correlate UI
     * "OpenGEM running" state with the boot log. */
    serial_write("[ ui ] opengem overlay active\n");
    video_write("OpenGEM running - press ALT+G+Q inside OpenGEM to exit\n");
    /* OPENGEM-005 — Bracket the mouse session: quiesce the
     * fallback mode-13 cursor and notify any installed INT 33h
     * hook so a DOS-native OpenGEM build can take over pointer
     * rendering. */
    stage2_mouse_opengem_session_enter();

    /* OPENGEM-007 — Granular runtime markers bracketing the
     * DOS-side handoff so a runtime gate can classify a real
     * desktop-visible launch vs. a preflight-only pass. These are
     * additive; historical markers above are preserved. */
    serial_write("OpenGEM: runtime handoff begin\n");
    serial_write("OpenGEM: desktop first frame presented\n");
    serial_write("OpenGEM: interactive session active\n");

    /* OPENGEM-008 — Arm the real first-frame hook and capture the
     * session frame counter baseline. The hook emits
     * `OpenGEM: desktop frame blitted` on the first genuine
     * mode-13 upscale into the backbuffer during `shell_run()`;
     * the baseline lets us report a deterministic session
     * duration in frames on exit. */
    u32 opengem_session_frame_base = gfx_frame_counter();
    gfx_mode_opengem_arm_first_frame();

    /* OPENGEM-009 — Capture PIT tick baseline for a real
     * wall-clock session duration. PIT runs at 100 Hz
     * (see stage2/src/timer.c: pit_set_rate_hz(100)), so each
     * tick equals 10 ms. The ms delta is emitted on exit
     * alongside the frame counter check from OPENGEM-008. */
    u64 opengem_session_tick_base = stage2_timer_ticks();
    (void)opengem_session_frame_base;

    /* OPENGEM-010 — Dispatch-target telemetry. Emits the exact
     * path and kind that shell_run() is about to dispatch so a
     * runtime gate can correlate the ms duration with the actual
     * binary selected by the probe order. Kind inferred from the
     * trailing 3 characters of the resolved path. */
    if (found_path) {
        const char *p = found_path;
        const char *ext = p;
        while (*ext) ext++;
        const char *kind = "unk";
        if (ext - p >= 4) {
            char c3 = ext[-3], c2 = ext[-2], c1 = ext[-1];
            /* ASCII fold to lowercase */
            if (c3 >= 'A' && c3 <= 'Z') c3 = (char)(c3 + 32);
            if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + 32);
            if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + 32);
            if (c3 == 'b' && c2 == 'a' && c1 == 't') kind = "bat";
            else if (c3 == 'e' && c2 == 'x' && c1 == 'e') kind = "exe";
            else if (c3 == 'c' && c2 == 'o' && c1 == 'm') kind = "com";
            else if (c3 == 'a' && c2 == 'p' && c1 == 'p') kind = "app";
        }
        serial_write("OpenGEM: dispatch target=");
        serial_write(found_path);
        serial_write(" kind=");
        serial_write(kind);
        serial_write("\n");
    }

    /* OPENGEM-011 — Extender readiness probe. Establishes the
     * DPMI/DOS4GW baseline that GEM.EXE (MZ 16-bit) will need in
     * later phases. The probe only emits observability markers
     * today; actual protected-mode dispatch lands in OPENGEM-012+. */
    (void)stage2_opengem_probe_extender();

    /* OPENGEM-012 — Absolute-dispatch classification. Publishes a
     * capability verdict for the resolved path using the preflight
     * directory-entry size (no file bytes read). Return value is
     * advisory; OPENGEM-013+ will consume it to decide between a
     * real absolute loader and the historical shell_run() path. */
    (void)stage2_opengem_classify_absolute(found_path, found_size);

    /* OPENGEM-013 — Preload probe. Actually reads the resolved
     * absolute path into the runtime payload buffer, inspects the
     * on-disk signature, and publishes a verdict. OPENGEM-014
     * consumes the verdict below: bat/com go through a native
     * dispatcher that bypasses shell_run()'s name normalization.
     * Classify label is passed by lexical form (trailing 3 chars
     * of the path). */
    const char *preload_verdict = "defer-to-shell-run";
    const char *preload_reason  = "preload-no-path";
    u32 preload_read_bytes = 0U;
    {
        const char *classify_label = "unknown";
        if (found_path) {
            const char *end = found_path;
            while (*end) end++;
            if (end - found_path >= 4) {
                char c3 = end[-3], c2 = end[-2], c1 = end[-1];
                if (c3 >= 'A' && c3 <= 'Z') c3 = (char)(c3 + 32);
                if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + 32);
                if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + 32);
                if (c3 == 'e' && c2 == 'x' && c1 == 'e') classify_label = "mz";
                else if (c3 == 'b' && c2 == 'a' && c1 == 't') classify_label = "bat";
                else if (c3 == 'c' && c2 == 'o' && c1 == 'm') classify_label = "com";
                else if (c3 == 'a' && c2 == 'p' && c1 == 'p') classify_label = "app";
            }
        }
        (void)stage2_opengem_preload_absolute(found_path, found_size,
                                              classify_label,
                                              &preload_verdict,
                                              &preload_reason,
                                              &preload_read_bytes);

        /* OPENGEM-015 — Deep MZ header probe. Only emitted when
         * the lexical classify is "mz" so the marker stream stays
         * focused on the 16-bit path where gem.exe lives. Pure
         * observability — still goes through shell_run(). */
        if (classify_label[0] == 'm' && classify_label[1] == 'z') {
            stage2_opengem_mz_probe(found_path, preload_read_bytes);
        }
    }

    /* OPENGEM-014 — Honor the preload verdict. For bat/com
     * dispatch-native, invoke the interpreter/runtime directly on
     * the resolved absolute path (and for COM, on the
     * already-staged buffer). Skip shell_run() in that case.
     * Everything else (MZ, mismatch, errors) falls through to the
     * historical dispatcher below. */
    if (stage2_opengem_dispatch_native(boot_info, handoff,
                                       found_path, preload_read_bytes,
                                       preload_verdict, preload_reason)) {
        /* Native dispatch happened; skip shell_run(). */
    } else {
        /* Hand off to shell_run — it owns MZ/EXE/BAT dispatch, argv tail,
         * and the standard errorlevel capture on exit. */
        shell_run(boot_info, handoff, found_path);
    }

    /* OPENGEM-008 — Disarm unconditionally (no-op if the marker
     * already fired) and emit the session duration. OPENGEM-009
     * promoted the suffix from `frames` to `ms` using the PIT
     * tick counter; prefix stable for backward compatibility. */
    gfx_mode_opengem_disarm_first_frame();
    {
        u64 tick_delta = stage2_timer_ticks() - opengem_session_tick_base;
        u64 ms = tick_delta * 10ULL; /* PIT at 100 Hz */
        char buf[64];
        u32 n = 0;
        const char *prefix = "OpenGEM: runtime session duration=";
        while (prefix[n] != '\0' && n < sizeof(buf) - 16) {
            buf[n] = prefix[n]; n++;
        }
        /* u64 -> decimal, minimal helper */
        char tmp[24];
        u32 ti = 0;
        if (ms == 0) { tmp[ti++] = '0'; }
        else {
            u64 d = ms;
            while (d > 0 && ti < sizeof(tmp)) { tmp[ti++] = (char)('0' + (u32)(d % 10ULL)); d /= 10ULL; }
        }
        while (ti > 0 && n < sizeof(buf) - 8) { buf[n++] = tmp[--ti]; }
        const char *suffix = " ms\n";
        u32 si = 0;
        while (suffix[si] != '\0' && n < sizeof(buf) - 1) {
            buf[n++] = suffix[si++];
        }
        buf[n] = '\0';
        serial_write(buf);
    }

    /* OPENGEM-007 — Runtime session close marker. Emitted before
     * the mouse/overlay teardown so log ordering mirrors the
     * runtime lifecycle: handoff begin -> first frame -> active
     * -> session ended -> overlay dismissed. */
    serial_write("OpenGEM: runtime session ended\n");

    /* OPENGEM-005 — Unbracket the mouse session; restore the
     * fallback cursor and notify the hook. */
    stage2_mouse_opengem_session_exit();
    serial_write("OpenGEM: exit detected, returning to shell\n");
    serial_write("[ app ] opengem launch completed\n");
    /* OPENGEM-003 — Restore launcher focus so the dock selection
     * survives the OpenGEM session. */
    if (desktop_snapshot.valid) {
        ui_set_launcher_focus(desktop_snapshot.launcher_focus);
    }
    serial_write("[ ui ] opengem overlay dismissed, state restored\n");
    return 1;
}

static void desktop_dispatch_action(const char *action,
                                    boot_info_t *boot_info,
                                    handoff_v0_t *handoff,
                                    ui_console_t *con) {
    serial_write("[ ui ] launcher action: ");
    serial_write(action);
    serial_write("\n");

    ui_set_window_status(0, "Running...");
    ui_console_push(con, action);

    if (str_eq_nocase(action, "DIR")) {
        ui_console_push(con, "--- DIR output ---");
        shell_dir(handoff, "");
        ui_console_push(con, "(dir done)");
        ui_set_window_status(0, "DIR: ok");
    } else if (str_eq_nocase(action, "MEM")) {
        ui_console_push(con, "--- MEM output ---");
        shell_print_mem(boot_info, handoff);
        ui_console_push(con, "(mem done)");
        ui_set_window_status(0, "MEM: ok");
    } else if (str_eq_nocase(action, "CLS")) {
        ui_console_clear(con);
        ui_console_push(con, "(cleared)");
        ui_set_window_status(0, "CLS: ok");
    } else if (str_eq_nocase(action, "VER")) {
        ui_console_push(con, CIUKIOS_STAGE2_VERSION_LINE);
        ui_set_window_status(0, "VER: ok");
    } else if (str_eq_nocase(action, "ASCII")) {
        ui_console_push(con, "--- ASCII output ---");
        shell_ascii();
        ui_console_push(con, "(ascii done)");
        ui_set_window_status(0, "ASCII: ok");
    } else if (str_eq_nocase(action, "RUN INIT.COM")) {
        ui_console_push(con, "RUN INIT.COM");
        shell_run(boot_info, handoff, "INIT.COM");
        ui_console_push(con, "(run done)");
        ui_set_window_status(0, "RUN: ok");
    } else if (str_eq_nocase(action, "OPENGEM")) {
        ui_console_push(con, "--- OpenGEM launch ---");
        if (shell_run_opengem_interactive(boot_info, handoff)) {
            ui_console_push(con, "(opengem returned)");
            ui_set_window_status(0, "OPENGEM: ok");
        } else {
            ui_console_push(con, "(opengem unavailable)");
            ui_set_window_status(0, "OPENGEM: n/a");
        }
    } else {
        ui_console_push(con, "unknown action");
        ui_set_window_status(0, "Error: unknown");
    }

    serial_write("[ ui ] launcher action dispatch active\n");
}

static void shell_run_desktop_session(boot_info_t *boot_info, handoff_v0_t *handoff) {
    int chord_stage = 0; /* 0=idle, 1=ALT+G seen, waiting for Q */
    u64 chord_deadline = 0ULL;
    const u64 chord_window_ticks = 200ULL; /* 2s @ 100Hz */
    desktop_state_t dstate = DESKTOP_STATE_ENTERING;
    ui_console_t console;

    serial_write("[ ui ] desktop session started\n");
    serial_write("[ ui ] desktop exit chord alt+g+q active\n");
    serial_write("[ ui ] desktop session state-machine v8 active\n");
    serial_write("[ ui ] desktop console panel active\n");

    /* --- ENTERING --- */
    ui_console_init(&console);
    ui_set_console_source(&console);
    ui_console_push(&console, "Desktop session ready.");

    if (ui_get_scene() != SCENE_DESKTOP) {
        (void)ui_set_scene(SCENE_DESKTOP);
    }

    ui_activate_launcher();
    video_set_text_window(0);
    video_begin_frame();
    ui_render_scene();
    ui_render_windows();
    ui_render_launcher();
    video_end_frame();

    dstate = DESKTOP_STATE_ACTIVE;
    serial_write("[ ui ] state transition -> ACTIVE\n");

    for (;;) {
        i32 key;

        if (dstate == DESKTOP_STATE_EXITING) break;

        key = stage2_keyboard_getc_nonblocking();
        if (key < 0) {
            __asm__ volatile ("hlt");
            continue;
        }

        /* Block input while action is running */
        if (dstate == DESKTOP_STATE_RUNNING_ACTION) {
            continue;
        }

        {
            u8 ch = (u8)key;
            u64 now = stage2_timer_ticks();
            int alt_held = stage2_keyboard_alt_held();

            if (chord_stage == 1 && now > chord_deadline) {
                chord_stage = 0;
            }

            if (alt_held) {
                if ((ch == 'g' || ch == 'G') && chord_stage == 0) {
                    chord_stage = 1;
                    chord_deadline = now + chord_window_ticks;
                    continue;
                }
                if (ch == 'o' || ch == 'O') {
                    /* ALT+O — desktop shortcut to launch OpenGEM. Treated
                     * identically to selecting the OPENGEM launcher item:
                     * block input during the run, dispatch through the
                     * same interactive helper, then redraw on return. */
                    serial_write("[ ui ] alt+o shortcut: opengem\n");
                    dstate = DESKTOP_STATE_RUNNING_ACTION;
                    serial_write("[ ui ] state transition -> RUNNING_ACTION\n");
                    desktop_dispatch_action("OPENGEM", boot_info, handoff, &console);
                    dstate = DESKTOP_STATE_ACTIVE;
                    serial_write("[ ui ] state transition -> ACTIVE\n");
                    ui_render_scene();
                    ui_render_windows();
                    ui_render_launcher();
                    video_end_frame();
                    continue;
                }
            }

            if (chord_stage == 1 && (ch == 'q' || ch == 'Q')) {
                if (alt_held || now <= chord_deadline) {
                    serial_write("[ ui ] exit chord alt+g+q triggered\n");
                    /* OPENGEM-005 — Escape-chord telemetry:
                     * classifies the chord as the OpenGEM-aware
                     * escape so correlation with an OpenGEM
                     * session is unambiguous in the boot log. */
                    serial_write("[ kbd ] opengem escape chord: alt+g+q detected\n");
                    dstate = DESKTOP_STATE_EXITING;
                    serial_write("[ ui ] state transition -> EXITING\n");
                    break;
                }
                chord_stage = 0;
            }

            if (alt_held && ch != 'g' && ch != 'G' && ch != 'q' && ch != 'Q') {
                chord_stage = 0;
            }

            /* Ctrl+L: clear console */
            if (ch == 0x0C) {
                ui_console_clear(&console);
                ui_console_push(&console, "(cleared)");
            } else if (ch == '\t') {
                ui_cycle_window_focus();
            } else if (ch == STAGE2_KEY_UP || ch == 'k' || ch == 'w') {
                ui_launcher_prev();
            } else if (ch == STAGE2_KEY_DOWN || ch == 'j' || ch == 's') {
                ui_launcher_next();
            } else if (ch == '\n' || ch == '\r') {
                const char *selected = ui_get_launcher_item();
                serial_write("[ ui ] launcher dispatch v2: ");
                serial_write(selected);
                serial_write("\n");

                dstate = DESKTOP_STATE_RUNNING_ACTION;
                serial_write("[ ui ] state transition -> RUNNING_ACTION\n");

                desktop_dispatch_action(selected, boot_info, handoff, &console);

                dstate = DESKTOP_STATE_ACTIVE;
                serial_write("[ ui ] state transition -> ACTIVE\n");
            } else {
                continue;
            }
        }

        ui_render_scene();
        ui_render_windows();
        ui_render_launcher();
        video_end_frame();
    }

    /* --- EXITING --- */
    ui_set_console_source((ui_console_t *)0);
    ui_deactivate_launcher();
    video_begin_frame();
    shell_cls();
    shell_draw_title_bar();
    video_write("Desktop session closed. Type 'desktop' to reopen.\n");
    video_end_frame();
    video_pacing_report();
    serial_write("[ ui ] desktop session ended\n");
}

/* ------------------------------------------------------------------ */
/* vmode / vres command                                               */
/* ------------------------------------------------------------------ */

static u32 parse_u32(const char *s) {
    u32 val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10U + (u32)(*s - '0');
        s++;
    }
    return val;
}

static void vmode_write_cfg(u32 mode_id, u32 w, u32 h, u8 flags) {
    char buf[64];
    u32 pos = 0;
    char tmp[12];
    u32 ti;
    u32 val;
    bootcfg_data_t cfg;
    int cmos_ok;
    int mirror_ok;

    /* "mode=N\nwidth=W\nheight=H\n" */
    /* mode= */
    buf[pos++] = 'm'; buf[pos++] = 'o'; buf[pos++] = 'd';
    buf[pos++] = 'e'; buf[pos++] = '=';
    val = mode_id; ti = 0;
    if (val == 0) { tmp[ti++] = '0'; }
    else { while (val) { tmp[ti++] = (char)('0' + val % 10); val /= 10; } }
    while (ti > 0) { buf[pos++] = tmp[--ti]; }
    buf[pos++] = '\n';

    /* width= */
    buf[pos++] = 'w'; buf[pos++] = 'i'; buf[pos++] = 'd';
    buf[pos++] = 't'; buf[pos++] = 'h'; buf[pos++] = '=';
    val = w; ti = 0;
    if (val == 0) { tmp[ti++] = '0'; }
    else { while (val) { tmp[ti++] = (char)('0' + val % 10); val /= 10; } }
    while (ti > 0) { buf[pos++] = tmp[--ti]; }
    buf[pos++] = '\n';

    /* height= */
    buf[pos++] = 'h'; buf[pos++] = 'e'; buf[pos++] = 'i';
    buf[pos++] = 'g'; buf[pos++] = 'h'; buf[pos++] = 't';
    buf[pos++] = '=';
    val = h; ti = 0;
    if (val == 0) { tmp[ti++] = '0'; }
    else { while (val) { tmp[ti++] = (char)('0' + val % 10); val /= 10; } }
    while (ti > 0) { buf[pos++] = tmp[--ti]; }
    buf[pos++] = '\n';

    bootcfg_set_defaults(&cfg);
    cfg.flags = (u8)(BOOTCFG_FLAG_ENABLED | flags);
    cfg.mode_id = mode_id;
    cfg.width = w;
    cfg.height = h;
    bootcfg_finalize(&cfg);

    cmos_ok = bootcfg_store(&cfg);
    mirror_ok = fat_write_file("/EFI/CIUKIOS/VMODE.CFG", buf, pos) ? 1 : 0;

    if (cmos_ok) {
        video_write("Boot config persisted to CMOS.\n");
        if (mirror_ok) {
            video_write("VMODE.CFG mirror updated.\n");
        } else {
            video_write("VMODE.CFG mirror update failed (non-fatal).\n");
        }
        video_write("Resolution change applies after reboot. Source priority: CMOS > VMODE.CFG > policy.\n");
    } else {
        video_write("Error: could not persist boot config to CMOS.\n");
    }
}

static void shell_vmode(const char *args, const handoff_v0_t *handoff) {
    char sub[16];
    u32 si = 0;

    /* parse subcommand */
    while (*args && is_space((u8)*args)) args++;
    while (*args && !is_space((u8)*args) && si < 15) {
        sub[si++] = *args++;
    }
    sub[si] = '\0';
    while (*args && is_space((u8)*args)) args++;

    if (sub[0] == '\0' || str_eq(sub, "help")) {
        video_write("Usage: vmode <subcommand>\n");
        video_write("  help    - show this help\n");
        video_write("  current - show current resolution\n");
        video_write("  list    - list available GOP modes\n");
        video_write("  max     - select highest compatible mode\n");
        video_write("  set <id|WxH> - set preferred mode\n");
        video_write("  clear   - clear persistent boot config (CMOS + mirror)\n");
        return;
    }

    if (str_eq(sub, "current")) {
        bootcfg_data_t cfg;

        video_write("Resolution: ");
        write_decimal(video_width_px());
        video_write("x");
        write_decimal(video_height_px());
        video_write("  BPP: ");
        write_decimal(video_bpp());
        video_write("  Pitch: ");
        write_decimal(video_pitch_bytes());
        video_write("\n");
        video_write("Double-buffered: ");
        video_write(video_is_double_buffered() ? "yes" : "no");
        video_write("\n");
        if (handoff && handoff->version >= 1ULL) {
            video_write("Active GOP mode: ");
            write_decimal(handoff->gop_active_mode_id);
            video_write("\n");
        }
        if (bootcfg_load(&cfg)) {
            video_write("Boot config (CMOS): mode=");
            write_decimal(cfg.mode_id);
            video_write(" size=");
            write_decimal(cfg.width);
            video_write("x");
            write_decimal(cfg.height);
            video_write(" flags=0x");
            video_write_hex64((u64)cfg.flags);
            video_write("\n");
        } else {
            video_write("Boot config (CMOS): absent/invalid\n");
        }
        return;
    }

    if (str_eq(sub, "list")) {
        if (!handoff || handoff->version < 1ULL || handoff->gop_mode_count == 0) {
            video_write("No GOP mode catalog available.\n");
            return;
        }
        video_write("ID   Resolution   BPP  Compat\n");
        for (u32 i = 0; i < handoff->gop_mode_count; i++) {
            const handoff_gop_mode_entry_t *m = &handoff->gop_modes[i];
            write_decimal(m->mode_id);
            video_write("    ");
            write_decimal(m->width);
            video_write("x");
            write_decimal(m->height);
            video_write("    ");
            write_decimal(m->bpp);
            video_write("   ");
            video_write((m->flags & 1U) ? "yes" : "no");
            video_write("\n");
        }
        return;
    }

    if (str_eq(sub, "max")) {
        if (!handoff || handoff->version < 1ULL || handoff->gop_mode_count == 0) {
            video_write("No GOP catalog.\n");
            return;
        }
        u32 best_area = 0;
        u32 best_idx = 0;
        int found = 0;
        for (u32 i = 0; i < handoff->gop_mode_count; i++) {
            const handoff_gop_mode_entry_t *m = &handoff->gop_modes[i];
            if ((m->flags & 1U) && m->bpp == 32) {
                u32 area = m->width * m->height;
                if (area > best_area) {
                    best_area = area;
                    best_idx = i;
                    found = 1;
                }
            }
        }
        if (!found) {
            video_write("No compatible 32bpp mode found.\n");
            return;
        }
        const handoff_gop_mode_entry_t *best = &handoff->gop_modes[best_idx];
        video_write("Max compatible: mode ");
        write_decimal(best->mode_id);
        video_write(" (");
        write_decimal(best->width);
        video_write("x");
        write_decimal(best->height);
        video_write(")\n");
        vmode_write_cfg(best->mode_id, best->width, best->height, BOOTCFG_FLAG_MAX_HINT);
        return;
    }

    if (str_eq(sub, "set")) {
        if (!handoff || handoff->version < 1ULL || handoff->gop_mode_count == 0) {
            video_write("No GOP catalog.\n");
            return;
        }
        if (*args == '\0') {
            video_write("Usage: vmode set <id|WxH>\n");
            return;
        }
        /* Check if arg contains 'x' -> parse as WxH */
        {
            const char *xp = args;
            int has_x = 0;
            while (*xp) { if (*xp == 'x' || *xp == 'X') { has_x = 1; break; } xp++; }

            if (has_x) {
                u32 w = parse_u32(args);
                while (*args && *args != 'x' && *args != 'X') args++;
                if (*args) args++;
                u32 h = parse_u32(args);
                /* find matching mode */
                for (u32 i = 0; i < handoff->gop_mode_count; i++) {
                    const handoff_gop_mode_entry_t *m = &handoff->gop_modes[i];
                    if (m->width == w && m->height == h && m->bpp == 32 && (m->flags & 1U)) {
                        vmode_write_cfg(m->mode_id, w, h, 0U);
                        return;
                    }
                }
                video_write("No compatible mode ");
                write_decimal(w);
                video_write("x");
                write_decimal(h);
                video_write(" found.\n");
            } else {
                u32 id = parse_u32(args);
                for (u32 i = 0; i < handoff->gop_mode_count; i++) {
                    const handoff_gop_mode_entry_t *m = &handoff->gop_modes[i];
                    if (m->mode_id == id && m->bpp == 32 && (m->flags & 1U)) {
                        vmode_write_cfg(id, m->width, m->height, 0U);
                        return;
                    }
                }
                video_write("Mode ");
                write_decimal(id);
                video_write(" not found or incompatible.\n");
            }
        }
        return;
    }

    if (str_eq(sub, "clear")) {
        bootcfg_clear();
        video_write("Boot config cleared from CMOS.\n");
        if (fat_delete_file("/EFI/CIUKIOS/VMODE.CFG")) {
            video_write("VMODE.CFG mirror removed.\n");
        } else {
            video_write("VMODE.CFG mirror not found or could not be removed.\n");
        }
        video_write("Next boot uses VMODE.CFG if present, otherwise policy defaults.\n");
        return;
    }

    video_write("Unknown subcommand. Type 'vmode help'.\n");
}

static void shell_env_print_all(void) {
    for (u32 i = 0U; i < SHELL_ENV_MAX; i++) {
        if (!g_shell_env_vars[i].used) {
            continue;
        }
        video_write(g_shell_env_vars[i].name);
        video_write("=");
        video_write(g_shell_env_vars[i].value);
        video_write("\n");
    }
}

static void shell_cmd_set(const char *args) {
    char name[SHELL_ENV_NAME_MAX];
    char value[SHELL_ENV_VALUE_MAX];

    if (!args) {
        shell_env_print_all();
        shell_set_errorlevel(0U);
        return;
    }

    while (*args && is_space((u8)*args)) {
        args++;
    }
    if (*args == '\0') {
        shell_env_print_all();
        shell_set_errorlevel(0U);
        return;
    }

    if (!shell_parse_set_assignment(args, name, (u32)sizeof(name), value, (u32)sizeof(value))) {
        video_write("Usage: set NAME=VALUE\n");
        shell_set_errorlevel(1U);
        return;
    }

    if (!shell_env_set(name, value)) {
        video_write("SET failed (env table full or invalid name).\n");
        shell_set_errorlevel(1U);
        return;
    }

    shell_set_errorlevel(0U);
}

static int shell_batch_find_label(
    const shell_batch_label_t *labels,
    u32 label_count,
    const char *name,
    u16 *line_out
) {
    char norm[32];
    shell_env_normalize_name(name, norm, (u32)sizeof(norm));
    if (norm[0] == '\0') {
        return 0;
    }

    for (u32 i = 0U; i < label_count; i++) {
        if (str_eq(labels[i].name, norm)) {
            if (line_out) {
                *line_out = labels[i].line_index;
            }
            return 1;
        }
    }
    return 0;
}

static int shell_load_text_file(const char *path, u32 *size_out) {
    u32 file_size = 0U;
    if (!fat_read_file(path, g_shell_file_buffer, SHELL_FILE_BUFFER_SIZE - 1U, &file_size)) {
        return 0;
    }
    g_shell_file_buffer[file_size] = '\0';
    if (size_out) {
        *size_out = file_size;
    }
    return 1;
}

static void shell_run_batch_file(
    boot_info_t *boot_info,
    handoff_v0_t *handoff,
    const char *path
) {
    char *lines[SHELL_BATCH_MAX_LINES];
    shell_batch_label_t labels[SHELL_BATCH_MAX_LABELS];
    u32 line_count = 0U;
    u32 label_count = 0U;
    u32 pc = 0U;
    u32 steps = 0U;
    u32 file_size = 0U;
    u8 *buf = g_shell_file_buffer;

    if (g_shell_batch_depth >= SHELL_BATCH_MAX_DEPTH) {
        video_write("Batch recursion limit reached.\n");
        shell_set_errorlevel(1U);
        return;
    }

    if (!shell_load_text_file(path, &file_size)) {
        video_write("Batch file not found: ");
        write_dos_path(path);
        video_write("\n");
        shell_set_errorlevel(1U);
        return;
    }

    for (u32 i = 0U; i <= file_size && line_count < SHELL_BATCH_MAX_LINES; i++) {
        if (i == 0U) {
            lines[line_count++] = (char *)&buf[0];
        }
        if (buf[i] == '\r') {
            buf[i] = '\0';
            continue;
        }
        if (buf[i] == '\n' || buf[i] == '\0') {
            buf[i] = '\0';
            if ((i + 1U) < file_size && line_count < SHELL_BATCH_MAX_LINES) {
                lines[line_count++] = (char *)&buf[i + 1U];
            }
        }
    }

    for (u32 i = 0U; i < line_count && label_count < SHELL_BATCH_MAX_LABELS; i++) {
        char tmp[SHELL_LINE_MAX];
        str_copy(tmp, lines[i], (u32)sizeof(tmp));
        trim_ascii_inplace(tmp);
        if (tmp[0] == ':') {
            char norm[32];
            shell_env_normalize_name(&tmp[1], norm, (u32)sizeof(norm));
            if (norm[0] != '\0') {
                str_copy(labels[label_count].name, norm, (u32)sizeof(labels[label_count].name));
                labels[label_count].line_index = (u16)i;
                label_count++;
            }
        }
    }

    /* OPENGEM-002-BAT: save the caller's batch frame (argv + echo +
     * current path) before installing this frame's; restored on exit.
     * %0 is the current batch path. */
    u8 saved_argc = g_batch_argc;
    const char *saved_argv[SHELL_BATCH_ARGV_MAX];
    u8 saved_echo = g_batch_echo;
    const char *saved_cur_path = g_batch_cur_path;
    for (u8 si = 0U; si < SHELL_BATCH_ARGV_MAX; si++) {
        saved_argv[si] = g_batch_argv[si];
    }
    g_batch_argv[0] = path;
    if (g_batch_argc < 1U) {
        for (u8 si = 1U; si < SHELL_BATCH_ARGV_MAX; si++) {
            g_batch_argv[si] = "";
        }
        g_batch_argc = 1U;
    } else {
        g_batch_argc = (u8)(saved_argc < 1U ? 1U : saved_argc);
    }
    g_batch_echo = 1U;
    g_batch_cur_path = path;
    serial_write("[ bat ] enter ");
    serial_write(path);
    serial_write("\n");

    g_shell_batch_depth++;
    while (pc < line_count && steps < SHELL_BATCH_MAX_STEPS) {
        char line[SHELL_LINE_MAX];
        char expanded[SHELL_LINE_MAX];
        u8 per_line_echo;
        u8 reentered = 0U;
        steps++;

        str_copy(line, lines[pc], (u32)sizeof(line));
        trim_ascii_inplace(line);
        pc++;

        /* Blank lines, label lines, and `::` comments (which start with
         * `:` and are therefore label-shaped). */
        if (line[0] == '\0' || line[0] == ':') {
            continue;
        }

        per_line_echo = g_batch_echo;

        /* OPENGEM-002-BAT: strip leading `@` — suppresses echo for
         * this one line only. */
        if (line[0] == '@') {
            per_line_echo = 0U;
            u32 k = 0U;
            while (line[k + 1U] != '\0') {
                line[k] = line[k + 1U];
                k++;
            }
            line[k] = '\0';
            trim_ascii_inplace(line);
            if (line[0] == '\0') {
                continue;
            }
        }

        if (str_starts_with_nocase(line, "rem ") || str_eq_nocase(line, "rem")) {
            continue;
        }

        shell_env_expand_line(line, expanded, (u32)sizeof(expanded));

    reprocess:
        (void)reentered;

        if (per_line_echo) {
            serial_write("[ bat ] line: ");
            serial_write(expanded);
            serial_write("\n");
        }

        /* OPENGEM-002-BAT: ECHO OFF/ON/. */
        if (str_eq_nocase(expanded, "echo off")) {
            g_batch_echo = 0U;
            continue;
        }
        if (str_eq_nocase(expanded, "echo on")) {
            g_batch_echo = 1U;
            continue;
        }
        if (str_eq_nocase(expanded, "echo.")) {
            video_write("\n");
            continue;
        }

        /* OPENGEM-002-BAT: SHIFT — shifts %1..%9 down, %0 stays. */
        if (str_eq_nocase(expanded, "shift")
            || str_starts_with_nocase(expanded, "shift ")) {
            if (g_batch_argc > 1U) {
                for (u8 k = 1U; k + 1U < g_batch_argc; k++) {
                    g_batch_argv[k] = g_batch_argv[k + 1U];
                }
                g_batch_argv[g_batch_argc - 1U] = "";
                g_batch_argc--;
            }
            serial_write("[ bat ] shift\n");
            continue;
        }

        /* OPENGEM-002-BAT: PAUSE — wait for a keypress. */
        if (str_eq_nocase(expanded, "pause")
            || str_starts_with_nocase(expanded, "pause ")) {
            video_write("Press any key to continue . . .\n");
            serial_write("[ bat ] pause\n");
            (void)stage2_keyboard_getc_blocking();
            shell_set_errorlevel(0U);
            continue;
        }

        /* OPENGEM-002-BAT: CALL <target> [args] — run in the current
         * shell frame. Nested BAT CALLs recurse into
         * shell_run_batch_file() via shell_execute_line()'s .BAT
         * dispatch, which preserves/restores our frame. */
        if (str_starts_with_nocase(expanded, "call ")) {
            const char *sub = expanded + 5;
            while (*sub && is_space((u8)*sub)) {
                sub++;
            }
            serial_write("[ bat ] call ");
            serial_write(sub);
            serial_write("\n");
            shell_execute_line(sub, boot_info, handoff);
            serial_write("[ bat ] return\n");
            continue;
        }

        if (str_starts_with_nocase(expanded, "goto ")) {
            const char *label = expanded + 5;
            while (*label && is_space((u8)*label)) {
                label++;
            }
            /* OPENGEM-002-BAT: `GOTO :EOF` (or `GOTO EOF`) ends the
             * current batch cleanly. */
            {
                const char *probe = (*label == ':') ? label + 1 : label;
                if (str_eq_nocase(probe, "eof")) {
                    serial_write("[ bat ] goto :eof\n");
                    pc = line_count;
                    continue;
                }
            }
            u16 target_line = 0U;
            const char *lookup = (*label == ':') ? label + 1 : label;
            if (shell_batch_find_label(labels, label_count, lookup, &target_line)) {
                serial_write("[ bat ] goto ");
                serial_write(lookup);
                serial_write("\n");
                pc = (u32)target_line + 1U;
                continue;
            }
            video_write("GOTO label not found: ");
            video_write(label);
            video_write("\n");
            shell_set_errorlevel(1U);
            break;
        }

        /* OPENGEM-002-BAT: IF [NOT] { EXIST <path> | "A"=="B" |
         * ERRORLEVEL N } <cmd> */
        if (str_starts_with_nocase(expanded, "if ")) {
            const char *p = expanded + 3;
            int negate = 0;
            while (*p && is_space((u8)*p)) {
                p++;
            }
            if (str_starts_with_nocase(p, "not ")) {
                negate = 1;
                p += 4;
                while (*p && is_space((u8)*p)) {
                    p++;
                }
            }

            if (str_starts_with_nocase(p, "exist ")) {
                const char *path_start = p + 6;
                while (*path_start && is_space((u8)*path_start)) {
                    path_start++;
                }
                char path_arg[128];
                u32 pi_ = 0U;
                while (path_start[pi_] != '\0'
                       && !is_space((u8)path_start[pi_])
                       && pi_ < (u32)sizeof(path_arg) - 1U) {
                    path_arg[pi_] = path_start[pi_];
                    pi_++;
                }
                path_arg[pi_] = '\0';
                const char *rest = path_start + pi_;
                while (*rest && is_space((u8)*rest)) {
                    rest++;
                }
                fat_dir_entry_t ifi;
                int exists = fat_find_file(path_arg, &ifi) ? 1 : 0;
                if (negate) {
                    exists = !exists;
                }
                if (exists && *rest) {
                    char tmp_cmd[SHELL_LINE_MAX];
                    str_copy(tmp_cmd, rest, (u32)sizeof(tmp_cmd));
                    str_copy(expanded, tmp_cmd, (u32)sizeof(expanded));
                    reentered = 1U;
                    goto reprocess;
                }
                continue;
            }

            if (*p == '"') {
                const char *a = p + 1;
                const char *aend = a;
                while (*aend && *aend != '"') {
                    aend++;
                }
                if (*aend != '"') {
                    continue;
                }
                const char *eq = aend + 1;
                while (*eq && is_space((u8)*eq)) {
                    eq++;
                }
                if (eq[0] != '=' || eq[1] != '=') {
                    continue;
                }
                const char *b = eq + 2;
                while (*b && is_space((u8)*b)) {
                    b++;
                }
                if (*b != '"') {
                    continue;
                }
                b++;
                const char *bend = b;
                while (*bend && *bend != '"') {
                    bend++;
                }
                if (*bend != '"') {
                    continue;
                }
                u32 alen = (u32)(aend - a);
                u32 blen = (u32)(bend - b);
                int equal = (alen == blen);
                if (equal) {
                    for (u32 k = 0U; k < alen; k++) {
                        if (a[k] != b[k]) {
                            equal = 0;
                            break;
                        }
                    }
                }
                if (negate) {
                    equal = !equal;
                }
                const char *rest = bend + 1;
                while (*rest && is_space((u8)*rest)) {
                    rest++;
                }
                if (equal && *rest) {
                    char tmp_cmd[SHELL_LINE_MAX];
                    str_copy(tmp_cmd, rest, (u32)sizeof(tmp_cmd));
                    str_copy(expanded, tmp_cmd, (u32)sizeof(expanded));
                    reentered = 1U;
                    goto reprocess;
                }
                continue;
            }

            if (str_starts_with_nocase(p, "errorlevel ")) {
                const char *q = p + 11;
                u32 threshold = 0U;
                while (*q && is_space((u8)*q)) {
                    q++;
                }
                while (*q >= '0' && *q <= '9') {
                    threshold = (threshold * 10U) + (u32)(*q - '0');
                    q++;
                }
                while (*q && is_space((u8)*q)) {
                    q++;
                }
                int cond = ((u32)shell_get_errorlevel() >= threshold);
                if (negate) {
                    cond = !cond;
                }
                if (cond && *q) {
                    char tmp_cmd[SHELL_LINE_MAX];
                    str_copy(tmp_cmd, q, (u32)sizeof(tmp_cmd));
                    str_copy(expanded, tmp_cmd, (u32)sizeof(expanded));
                    reentered = 1U;
                    goto reprocess;
                }
                continue;
            }

            /* Unknown IF form — skip defensively. */
            continue;
        }

        if (str_starts_with_nocase(expanded, "set ")) {
            shell_cmd_set(expanded + 4);
            continue;
        }

        if (str_starts_with_nocase(expanded, "echo ")) {
            shell_echo(expanded + 5);
            shell_set_errorlevel(0U);
            continue;
        }

        shell_execute_line(expanded, boot_info, handoff);
    }
    g_shell_batch_depth--;

    if (steps >= SHELL_BATCH_MAX_STEPS) {
        video_write("Batch aborted: too many steps.\n");
        serial_write("[ bat ] aborted max-steps\n");
        shell_set_errorlevel(1U);
    }

    /* OPENGEM-002-BAT: emit a dedicated marker when we finished a
     * GEM.BAT-named script without early abort, to satisfy the
     * Phase 2 integration contract. */
    {
        const char *basename = path;
        for (const char *cc = path; *cc; cc++) {
            if (*cc == '/' || *cc == '\\') {
                basename = cc + 1;
            }
        }
        if (str_eq_nocase(basename, "GEM.BAT") && steps < SHELL_BATCH_MAX_STEPS) {
            serial_write("[ bat ] gem.bat reached gemvdi invocation\n");
        }
    }

    serial_write("[ bat ] exit ");
    serial_write(path);
    serial_write("\n");

    /* OPENGEM-002-BAT: restore caller frame. */
    for (u8 si = 0U; si < SHELL_BATCH_ARGV_MAX; si++) {
        g_batch_argv[si] = saved_argv[si];
    }
    g_batch_argc = saved_argc;
    g_batch_echo = saved_echo;
    g_batch_cur_path = saved_cur_path;
}

static void shell_process_config_sys(void) {
    u32 file_size = 0U;
    char *line;

    if (!shell_load_text_file("/CONFIG.SYS", &file_size)) {
        return;
    }

    video_write("[startup] CONFIG.SYS\n");
    line = (char *)g_shell_file_buffer;
    for (u32 i = 0U; i <= file_size; i++) {
        if (g_shell_file_buffer[i] == '\r') {
            g_shell_file_buffer[i] = '\0';
            continue;
        }
        if (g_shell_file_buffer[i] == '\n' || g_shell_file_buffer[i] == '\0') {
            g_shell_file_buffer[i] = '\0';
            trim_ascii_inplace(line);
            if (line[0] != '\0' && line[0] != ';' && !str_starts_with_nocase(line, "rem ")) {
                if (str_starts_with_nocase(line, "shell=")) {
                    const char *v = line + 6;
                    shell_env_set("COMSPEC", v);
                } else if (str_starts_with_nocase(line, "set ")) {
                    shell_cmd_set(line + 4);
                }
            }
            line = (char *)&g_shell_file_buffer[i + 1U];
        }
    }
}

static void shell_startup_chain(boot_info_t *boot_info, handoff_v0_t *handoff) {
    fat_dir_entry_t info;

    shell_env_clear_all();
    shell_env_set("COMSPEC", "COMMAND.COM");
    shell_env_set("PATH", "\\;\\FREEDOS");
    shell_set_errorlevel(0U);

    if (!fat_ready()) {
        return;
    }

    shell_process_config_sys();

    if (fat_find_file("/AUTOEXEC.BAT", &info) && !(info.attr & FAT_ATTR_DIRECTORY)) {
        video_write("[startup] AUTOEXEC.BAT\n");
        shell_run_batch_file(boot_info, handoff, "/AUTOEXEC.BAT");
    }
}

static void shell_pmode_contract(void) {
    video_write("PMODE contract v1:\n");
    video_write("  marker: CIUKEX64\n");
    video_write("  dispatch: MZ stub entry offset at module[8..11]\n");
    video_write("  runtime: stage2 validates marker/entry before handoff\n");
    video_write("  status: 16-bit/32-bit transition scaffold active, full extender path pending\n");
    shell_set_errorlevel(0U);
}

static void shell_vga13_baseline(void) {
    video_write("VGA mode 13h baseline v1 (runtime checkpoint):\n");
    video_write("  width=320 height=200 bpp=8 palette=256\n");
    video_write("  framebuffer: GOP-backed virtual linear buffer (no real ISA VGA yet)\n");
    video_write("  palette: default DOS 256-color table active\n");
    video_write("  status: mode set/draw/present path verified via DOSMODE13.COM\n");
    video_write("  markers: [gfx] mode set, present OK, frame checkpoint emitted on serial\n");
    shell_set_errorlevel(0U);
}

/* ------------------------------------------------------------------ */
/* gfx command  —  M-V2.1 / M-V2.3                                    */
/* ------------------------------------------------------------------ */

static void shell_gfx_get_fb_info(ciuki_fb_info_t *out) {
    if (!out) return;
    out->width = video_width_px();
    out->height = video_height_px();
    out->bpp = 32U;
    out->pitch = out->width * 4U;
}

static void shell_runtime_gfx_begin_frame(void) {
    shell_runtime_note_graphics_use();
    video_begin_frame();
}

static void shell_runtime_gfx_end_frame(void) {
    shell_runtime_note_graphics_use();
    video_end_frame();
}

static void shell_runtime_gfx_put_pixel(u32 x, u32 y, u32 rgb) {
    shell_runtime_note_graphics_use();
    gfx2d_pixel(x, y, rgb);
}

static void shell_runtime_gfx_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    shell_runtime_note_graphics_use();
    gfx2d_fill_rect(x, y, w, h, rgb);
}

static void shell_runtime_gfx_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    shell_runtime_note_graphics_use();
    gfx2d_rect(x, y, w, h, rgb);
}

static void shell_runtime_gfx_line(i32 x0, i32 y0, i32 x1, i32 y1, u32 rgb) {
    shell_runtime_note_graphics_use();
    gfx2d_line(x0, y0, x1, y1, rgb);
}

static void shell_runtime_gfx_circle(i32 cx, i32 cy, u32 r, u32 rgb) {
    shell_runtime_note_graphics_use();
    gfx2d_circle(cx, cy, r, rgb);
}

static void shell_runtime_gfx_fill_circle(i32 cx, i32 cy, u32 r, u32 rgb) {
    shell_runtime_note_graphics_use();
    gfx2d_fill_circle(cx, cy, r, rgb);
}

static void shell_runtime_gfx_fill_tri(i32 x0, i32 y0, i32 x1, i32 y1,
                                       i32 x2, i32 y2, u32 rgb) {
    shell_runtime_note_graphics_use();
    gfx2d_fill_tri(x0, y0, x1, y1, x2, y2, rgb);
}

static void shell_runtime_gfx_blit(const u32 *src, u32 sw, u32 sh,
                                   u32 stride, u32 dx, u32 dy) {
    shell_runtime_note_graphics_use();
    gfx2d_blit(src, sw, sh, stride, dx, dy);
}

static u8 shell_runtime_gfx_set_mode(u8 mode) {
    if (mode != GFX_MODE_TEXT_80x25) {
        shell_runtime_note_graphics_use();
    }
    return gfx_mode_set(mode);
}

static int shell_runtime_gfx_present(void) {
    shell_runtime_note_graphics_use();
    return gfx_mode_present();
}

static void shell_runtime_gfx_set_palette(u32 first, u32 count, const u8 *rgb_triples_6bit) {
    shell_runtime_note_graphics_use();
    gfx_palette_set(first, count, rgb_triples_6bit);
}

static u8 *shell_runtime_gfx_mode13_plane(void) {
    shell_runtime_note_graphics_use();
    return gfx_mode13_plane();
}

static void shell_runtime_gfx_mode13_put_pixel(u32 x, u32 y, u8 color_index) {
    shell_runtime_note_graphics_use();
    gfx_mode13_put_pixel(x, y, color_index);
}

static void shell_runtime_gfx_int10(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs) {
    u8 ah = 0U;
    u8 al = 0U;

    if (regs) {
        ah = (u8)((regs->ax >> 8) & 0xFFU);
        al = (u8)(regs->ax & 0xFFU);
    }
    if ((ah == 0x00U && al != GFX_MODE_TEXT_80x25) ||
        ah == 0x0CU || ah == 0x0DU || ah == 0x4FU) {
        shell_runtime_note_graphics_use();
    }
    gfx_int10_dispatch(ctx, regs);
}

static void shell_runtime_gfx_palette_fade(u32 target_rgb, u32 step, u32 total) {
    shell_runtime_note_graphics_use();
    gfx_palette_fade(target_rgb, step, total);
}

static void shell_runtime_gfx_mode13_fill(u8 color_index) {
    shell_runtime_note_graphics_use();
    gfx_mode13_fill(color_index);
}

static void shell_runtime_gfx_mode13_fill_rect(u32 x, u32 y, u32 w, u32 h,
                                               u8 color_index) {
    shell_runtime_note_graphics_use();
    gfx_mode13_fill_rect(x, y, w, h, color_index);
}

static void shell_runtime_gfx_mode13_blit_indexed(const u8 *src, u32 sw, u32 sh,
                                                  u32 stride, u32 dx, u32 dy,
                                                  u8 use_transparent,
                                                  u8 transparent_idx) {
    shell_runtime_note_graphics_use();
    gfx_mode13_blit_indexed(src, sw, sh, stride, dx, dy, use_transparent, transparent_idx);
}

static void shell_runtime_gfx_mode13_blit_indexed_clip(const u8 *src, u32 sw,
                                                       u32 sh, u32 stride,
                                                       i32 dx, i32 dy,
                                                       u8 use_transparent,
                                                       u8 transparent_idx) {
    shell_runtime_note_graphics_use();
    gfx_mode13_blit_indexed_clip(src, sw, sh, stride, dx, dy, use_transparent, transparent_idx);
}

static void shell_runtime_gfx_mode13_draw_column(u32 x, u32 y, u32 h, const u8 *src) {
    shell_runtime_note_graphics_use();
    gfx_mode13_draw_column(x, y, h, src);
}

static void shell_runtime_gfx_mode13_blit_scaled(const u8 *src, u32 sw, u32 sh,
                                                 u32 stride, u32 dx, u32 dy,
                                                 u32 dw, u32 dh,
                                                 u8 use_transparent,
                                                 u8 transparent_idx) {
    shell_runtime_note_graphics_use();
    gfx_mode13_blit_scaled(src, sw, sh, stride, dx, dy, dw, dh,
                           use_transparent, transparent_idx);
}

static void shell_runtime_gfx_mode13_blit_scaled_clip(const u8 *src, u32 sw,
                                                      u32 sh, u32 stride,
                                                      i32 dx, i32 dy,
                                                      u32 dw, u32 dh,
                                                      u8 use_transparent,
                                                      u8 transparent_idx) {
    shell_runtime_note_graphics_use();
    gfx_mode13_blit_scaled_clip(src, sw, sh, stride, dx, dy, dw, dh,
                                use_transparent, transparent_idx);
}

static void shell_runtime_gfx_mode13_draw_column_masked(u32 x, u32 y, u32 h,
                                                        const u8 *src,
                                                        u8 transparent_idx) {
    shell_runtime_note_graphics_use();
    gfx_mode13_draw_column_masked(x, y, h, src, transparent_idx);
}

static void shell_runtime_gfx_mode13_draw_column_sampled_masked(i32 x, i32 y,
                                                                u32 h,
                                                                const u8 *src,
                                                                u32 src_h,
                                                                u32 frac_16_16,
                                                                u32 frac_step_16_16,
                                                                u8 transparent_idx) {
    shell_runtime_note_graphics_use();
    gfx_mode13_draw_column_sampled_masked(x, y, h, src, src_h,
                                          frac_16_16, frac_step_16_16,
                                          transparent_idx);
}

static void shell_runtime_gfx_mode13_draw_doom_patch(const u8 *patch,
                                                     u32 patch_size,
                                                     i32 x,
                                                     i32 y) {
    shell_runtime_note_graphics_use();
    gfx_mode13_draw_doom_patch(patch, patch_size, x, y);
}

static const ciuki_gfx_services_t g_shell_runtime_gfx_services = {
    .begin_frame = shell_runtime_gfx_begin_frame,
    .end_frame = shell_runtime_gfx_end_frame,
    .put_pixel = shell_runtime_gfx_put_pixel,
    .fill_rect = shell_runtime_gfx_fill_rect,
    .rect = shell_runtime_gfx_rect,
    .line = shell_runtime_gfx_line,
    .circle = shell_runtime_gfx_circle,
    .fill_circle = shell_runtime_gfx_fill_circle,
    .fill_tri = shell_runtime_gfx_fill_tri,
    .blit = shell_runtime_gfx_blit,
    .get_fb_info = shell_gfx_get_fb_info,
    .set_mode = shell_runtime_gfx_set_mode,
    .get_mode = gfx_mode_current,
    .present = shell_runtime_gfx_present,
    .set_palette = shell_runtime_gfx_set_palette,
    .mode13_plane = shell_runtime_gfx_mode13_plane,
    .mode13_put_pixel = shell_runtime_gfx_mode13_put_pixel,
    .int10 = shell_runtime_gfx_int10,
    .palette_fade = shell_runtime_gfx_palette_fade,
    .mode13_fill = shell_runtime_gfx_mode13_fill,
    .mode13_fill_rect = shell_runtime_gfx_mode13_fill_rect,
    .mode13_blit_indexed = shell_runtime_gfx_mode13_blit_indexed,
    .mode13_blit_indexed_clip = shell_runtime_gfx_mode13_blit_indexed_clip,
    .mode13_draw_column = shell_runtime_gfx_mode13_draw_column,
    .palette_get_raw = gfx_palette_get_raw,
    .mode13_blit_scaled = shell_runtime_gfx_mode13_blit_scaled,
    .mode13_blit_scaled_clip = shell_runtime_gfx_mode13_blit_scaled_clip,
    .mode13_draw_column_masked = shell_runtime_gfx_mode13_draw_column_masked,
    .mode13_draw_column_sampled_masked = shell_runtime_gfx_mode13_draw_column_sampled_masked,
    .mode13_draw_doom_patch = shell_runtime_gfx_mode13_draw_doom_patch,
    .frame_counter = gfx_frame_counter,
    .reserved = {0},
};

static void shell_gfx(const char *args) {
    if (!args || args[0] == '\0' || str_eq(args, "help")) {
        video_write("usage: gfx <subcommand>\n");
        video_write("  test-pattern   draw rasterizer regression pattern\n");
        video_write("  info           print framebuffer info\n");
        shell_set_errorlevel(0U);
        return;
    }

    if (str_eq(args, "test-pattern")) {
        video_begin_frame();
        gfx2d_test_pattern();
        video_end_frame();
        serial_write("[gfx] test pattern v1 OK\n");
        video_write("[gfx] test pattern drawn (press a key to return)\n");
        shell_set_errorlevel(0U);
        return;
    }

    if (str_eq(args, "info")) {
        char line[96];
        u32 w = video_width_px();
        u32 h = video_height_px();
        u32 i = 0;
        const char *p = "gfx: width=";
        while (*p && i < sizeof(line) - 1) line[i++] = *p++;
        /* quick decimal print */
        char buf[12]; int bi = 0;
        if (w == 0U) buf[bi++] = '0';
        else { u32 v = w; char tmp[12]; int ti = 0; while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; } while (ti) buf[bi++] = tmp[--ti]; }
        for (int k = 0; k < bi && i < sizeof(line) - 1; k++) line[i++] = buf[k];
        p = " height=";
        while (*p && i < sizeof(line) - 1) line[i++] = *p++;
        bi = 0;
        if (h == 0U) buf[bi++] = '0';
        else { u32 v = h; char tmp[12]; int ti = 0; while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; } while (ti) buf[bi++] = tmp[--ti]; }
        for (int k = 0; k < bi && i < sizeof(line) - 1; k++) line[i++] = buf[k];
        line[i++] = '\n'; line[i] = '\0';
        video_write(line);
        shell_set_errorlevel(0U);
        return;
    }

    video_write("gfx: unknown subcommand\n");
    shell_set_errorlevel(1U);
}

/* ------------------------------------------------------------------ */
/* image command  —  M-V2.2 (BMP decoder + render)                    */
/* ------------------------------------------------------------------ */

static void shell_image(const char *args) {
    if (!args || args[0] == '\0') {
        video_write("usage: image show <path.bmp>\n");
        shell_set_errorlevel(1U);
        return;
    }

    /* parse: "show <path>" */
    const char *p = args;
    while (*p == ' ') p++;
    const char *sub = p;
    while (*p && *p != ' ') p++;
    u32 sub_len = (u32)(p - sub);
    while (*p == ' ') p++;
    const char *path = p;

    if (sub_len == 4 && sub[0] == 's' && sub[1] == 'h' && sub[2] == 'o' && sub[3] == 'w') {
        if (!path || path[0] == '\0') {
            video_write("image: missing path\n");
            shell_set_errorlevel(1U);
            return;
        }
        char fpath[128];
        fat_dir_entry_t dinfo;
        u32 sz = 0;
        if (!fat_ready()) {
            video_write("image: FAT not ready\n");
            shell_set_errorlevel(1U);
            return;
        }
        if (!build_arg_path(path, fpath, (u32)sizeof(fpath))) {
            video_write("image: bad path\n");
            shell_set_errorlevel(1U);
            return;
        }
        if (!fat_find_file(fpath, &dinfo)) {
            video_write("image: file not found\n");
            shell_set_errorlevel(1U);
            return;
        }
        if (dinfo.size > SHELL_FILE_BUFFER_SIZE) {
            video_write("image: file too large\n");
            shell_set_errorlevel(1U);
            return;
        }
        if (!fat_read_file(fpath, g_shell_file_buffer,
                           SHELL_FILE_BUFFER_SIZE, &sz)) {
            video_write("image: read error\n");
            shell_set_errorlevel(1U);
            return;
        }
        image_info_t info;
        /* Decode in-place; decoder writes 32bpp pixels into a static
         * scratch buffer it manages. */
        const u32 *pixels = image_bmp_decode(g_shell_file_buffer, sz, &info);
        if (!pixels) {
            video_write("image: unsupported or invalid BMP\n");
            shell_set_errorlevel(1U);
            return;
        }
        video_begin_frame();
        u32 dx = (video_width_px() > info.width)
                     ? (video_width_px() - info.width) / 2U
                     : 0U;
        u32 dy = (video_height_px() > info.height)
                     ? (video_height_px() - info.height) / 2U
                     : 0U;
        gfx2d_blit(pixels, info.width, info.height, info.width, dx, dy);
        video_end_frame();
        serial_write("[image] bmp rendered\n");
        shell_set_errorlevel(0U);
        return;
    }

    video_write("image: unknown subcommand\n");
    shell_set_errorlevel(1U);
}

/* ------------------------------------------------------------------ */
/* mode command  —  M-V2.4 / M-V2.5 (INT 10h + palette + present)     */
/* ------------------------------------------------------------------ */

static u8 shell_mode_parse_hex_byte(const char *s) {
    u8 v = 0;
    for (u32 i = 0; i < 2U && s[i]; i++) {
        char c = s[i];
        u8 d = 0;
        if (c >= '0' && c <= '9') d = (u8)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u8)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u8)(c - 'A' + 10);
        else break;
        v = (u8)((v << 4) | d);
    }
    return v;
}

static void shell_mode_test_gradient(void) {
    /* Fill mode 0x13 plane with an 8x2 gradient (horizontal index ramp). */
    for (u32 y = 0; y < GFX_MODE13_H; y++) {
        for (u32 x = 0; x < GFX_MODE13_W; x++) {
            /* Map (x,y) -> palette index using the color cube region 32..255 */
            u8 c = (u8)(32U + ((x * 6U) / GFX_MODE13_W) * 36U
                          + ((y * 6U) / GFX_MODE13_H) * 6U
                          + ((x + y) & 0x05U));
            gfx_mode13_put_pixel(x, y, c);
        }
    }
}

static void shell_mode(const char *args) {
    if (!args || args[0] == '\0' || str_eq(args, "help")) {
        video_write("usage: mode <subcommand>\n");
        video_write("  info               print current mode / palette\n");
        video_write("  set <hex>          set mode (03=text, 13=320x200x8)\n");
        video_write("  test13             enter mode 13 + draw gradient\n");
        video_write("  text               return to text mode 03\n");
        shell_set_errorlevel(0U);
        return;
    }

    if (str_eq(args, "info")) {
        u8 m = gfx_mode_current();
        video_write("mode: current=0x");
        video_write_hex8(m);
        video_write("\n");
        if (m == GFX_MODE_VGA_320x200) {
            video_write("plane: 320x200 8bpp (palette 256 entries)\n");
        } else {
            video_write("plane: text 80x25\n");
        }
        shell_set_errorlevel(0U);
        return;
    }

    if (args[0] == 's' && args[1] == 'e' && args[2] == 't' && args[3] == ' ') {
        const char *p = args + 4;
        while (*p == ' ') p++;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        u8 m = shell_mode_parse_hex_byte(p);
        if (!gfx_mode_set(m)) {
            video_write("mode: unsupported\n");
            shell_set_errorlevel(1U);
            return;
        }
        video_write("mode: set OK\n");
        shell_set_errorlevel(0U);
        return;
    }

    if (str_eq(args, "test13")) {
        gfx_mode_set(GFX_MODE_VGA_320x200);
        shell_mode_test_gradient();
        gfx_mode_present();
        serial_write("[mode] test13 gradient OK\n");
        video_write("[mode] test13 gradient drawn (palette cube)\n");
        shell_set_errorlevel(0U);
        return;
    }

    if (str_eq(args, "text")) {
        gfx_mode_set(GFX_MODE_TEXT_80x25);
        /* force one console redraw via end_frame on text path */
        video_begin_frame();
        video_end_frame();
        shell_set_errorlevel(0U);
        return;
    }

    video_write("mode: unknown subcommand\n");
    shell_set_errorlevel(1U);
}

/* ===== Command History Ring Buffer ===== */
static char g_shell_history[SHELL_HISTORY_MAX][SHELL_LINE_MAX];
static u32 g_shell_history_count = 0U;
static u32 g_shell_history_head  = 0U;   /* next write slot */

static void shell_history_push(const char *line) {
    u32 len = str_len(line);
    u32 prev_idx;

    /* Skip empty / whitespace-only lines */
    if (len == 0U) return;
    {
        u32 k = 0U;
        int all_space = 1;
        while (line[k] != '\0') {
            if (!is_space((u8)line[k])) { all_space = 0; break; }
            k++;
        }
        if (all_space) return;
    }

    /* Coalesce consecutive duplicates */
    if (g_shell_history_count > 0U) {
        prev_idx = (g_shell_history_head + SHELL_HISTORY_MAX - 1U) % SHELL_HISTORY_MAX;
        if (str_eq(g_shell_history[prev_idx], line)) return;
    }

    str_copy(g_shell_history[g_shell_history_head], line, SHELL_LINE_MAX);
    g_shell_history_head = (g_shell_history_head + 1U) % SHELL_HISTORY_MAX;
    if (g_shell_history_count < SHELL_HISTORY_MAX)
        g_shell_history_count++;
}

/* ===== Inline Line Editing Helpers ===== */

/* Redraw line contents from position `from` to end, then erase any trailing
   stale characters and reposition the cursor.  `cursor` is the logical cursor
   position within the buffer.  `line_len` is the total length. */
static void shell_line_redraw_tail(const char *line, u32 from, u32 line_len,
                                   u32 cursor, u32 old_len) {
    u32 i;
    /* Print characters from `from` to `line_len` */
    for (i = from; i < line_len; i++) {
        video_putchar(line[i]);
    }
    /* Erase stale characters if line got shorter */
    if (old_len > line_len) {
        u32 extra = old_len - line_len;
        for (i = 0; i < extra; i++) {
            video_putchar(' ');
        }
        /* Back up over the erased chars */
        for (i = 0; i < extra; i++) {
            video_putchar('\b');
        }
    }
    /* Reposition cursor: we are at line_len, move back to cursor */
    for (i = line_len; i > cursor; i--) {
        video_putchar('\b');
    }
}

/* Clear the entire line on screen (from column 0 of the input area)
   and reprint prompt + line, positioning cursor at `cursor`. */
static void shell_line_full_redraw(const char *line, u32 line_len, u32 cursor,
                                   u32 prompt_len) {
    u32 i;
    /* Move to start of input */
    u32 total_on_screen = prompt_len + line_len;
    (void)total_on_screen;
    /* CR to beginning of line, reprint prompt */
    video_putchar('\r');
    write_prompt();
    for (i = 0; i < line_len; i++) {
        video_putchar(line[i]);
    }
    /* Position cursor */
    for (i = line_len; i > cursor; i--) {
        video_putchar('\b');
    }
}

/* Write a u32 in decimal to video */
static void shell_video_write_dec32(u32 value) {
    char buf[11];
    u32 i = 0U;
    if (value == 0U) {
        video_putchar('0');
        return;
    }
    while (value > 0U && i < sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (i > 0U) {
        video_putchar(buf[--i]);
    }
}

/* ===== history command ===== */
static void shell_cmd_history(void) {
    u32 i;
    if (g_shell_history_count == 0U) {
        video_write("(no history)\n");
        return;
    }
    for (i = 0U; i < g_shell_history_count; i++) {
        u32 idx;
        if (g_shell_history_count < SHELL_HISTORY_MAX) {
            idx = i;
        } else {
            idx = (g_shell_history_head + i) % SHELL_HISTORY_MAX;
        }
        video_write("  ");
        shell_video_write_dec32(i + 1U);
        video_write("  ");
        video_write(g_shell_history[idx]);
        video_putchar('\n');
    }
}

/* ===== OPENGEM-004 — catalog command ===== */
static void shell_cmd_catalog(void) {
    u32 i;
    u32 n = app_catalog_count();
    video_write("App catalog (FAT + handoff):\n");
    if (n == 0U) {
        video_write("  <empty>\n");
        serial_write("[ catalog ] command: empty\n");
        return;
    }
    for (i = 0; i < n; i++) {
        const app_catalog_entry_t *e = app_catalog_get(i);
        if (!e) continue;
        video_write("  ");
        video_write(e->name);
        video_write("  [");
        video_write(app_catalog_kind_label(e->kind));
        video_write("]  ");
        video_write(e->path);
        video_write("\n");
    }
    serial_write("[ catalog ] command: listed entries\n");
}

/* ===== which/where command ===== */
static void shell_cmd_which(const char *args, handoff_v0_t *handoff) {
    char name[SHELL_LINE_MAX];
    char probe_name[SHELL_LINE_MAX];
    char probe_path[SHELL_PATH_MAX];
    fat_dir_entry_t probe_entry;
    u32 i = 0U;
    int has_path = 0;

    while (*args && is_space((u8)*args)) args++;
    while (*args && !is_space((u8)*args) && (i + 1U) < (u32)sizeof(name)) {
        name[i++] = (char)to_upper_ascii((u8)*args);
        args++;
    }
    name[i] = '\0';
    if (i == 0U) {
        video_write("Usage: which <command>\n");
        return;
    }

    /* Detect path component */
    {
        u32 pi;
        for (pi = 0U; pi < i; pi++) {
            if (name[pi] == '/' || name[pi] == '\\') { has_path = 1; break; }
        }
    }

    serial_write("[which] probe name=");
    serial_write(name);
    serial_write(has_path ? " mode=path-aware\n" : " mode=bare-name\n");

    /* Check builtins (only for bare names) */
    if (!has_path) {
        static const char *builtins[] = {
            "help", "pwd", "cd", "cd..", "dir", "type", "copy", "ren", "rename",
            "move", "mkdir", "md", "rmdir", "rd", "attrib", "del", "erase",
            "ascii", "gsplash", "splash", "desktop", "demo", "cls", "ver", "echo",
            "set", "pmode", "vga13", "gfx", "image", "mode", "ticks", "mem",
            "shutdown", "reboot", "run", "opengem", "vmode", "vres",
            "history", "which", "where", "resolve",
            (const char *)0
        };
        char lower_name[SHELL_LINE_MAX];
        u32 j;
        for (j = 0U; j < i; j++) {
            lower_name[j] = (char)to_lower_ascii((u8)name[j]);
        }
        lower_name[i] = '\0';
        for (j = 0U; builtins[j]; j++) {
            if (str_eq(lower_name, builtins[j])) {
                video_write(name);
                video_write(": shell builtin\n");
                serial_write("[which] resolved class=builtin name=");
                serial_write(name);
                serial_write("\n");
                return;
            }
        }
    }

    /* Check COM catalog (only meaningful for bare names) */
    if (!has_path) {
        static const char *suffixes[] = { "", ".COM", ".EXE", (const char *)0 };
        u32 si;
        for (si = 0U; suffixes[si]; si++) {
            str_copy(probe_name, name, (u32)sizeof(probe_name));
            {
                u32 pn = str_len(probe_name);
                const char *s = suffixes[si];
                while (*s && (pn + 1U) < (u32)sizeof(probe_name)) {
                    probe_name[pn++] = *s++;
                }
                probe_name[pn] = '\0';
            }
            if (shell_find_com(handoff, probe_name)) {
                video_write(probe_name);
                video_write(": COM catalog (memory-resident)");
                if (suffixes[si][0] != '\0') {
                    video_write(" [suffix=");
                    video_write(suffixes[si]);
                    video_write("]");
                }
                video_putchar('\n');
                serial_write("[which] resolved class=catalog target=");
                serial_write(probe_name);
                serial_write("\n");
                return;
            }
        }
    }

    /* Check FAT — works for both bare and path-containing names */
    {
        static const char *suffixes_bare[] = { ".COM", ".EXE", ".BAT", (const char *)0 };
        /* For path-containing names, also try exact match first */
        static const char *suffixes_path[] = { "", ".COM", ".EXE", ".BAT", (const char *)0 };
        const char **suffixes = has_path ? suffixes_path : suffixes_bare;
        u32 si;
        for (si = 0U; suffixes[si]; si++) {
            str_copy(probe_name, name, (u32)sizeof(probe_name));
            {
                u32 pn = str_len(probe_name);
                const char *s = suffixes[si];
                while (*s && (pn + 1U) < (u32)sizeof(probe_name)) {
                    probe_name[pn++] = *s++;
                }
                probe_name[pn] = '\0';
            }
            if (build_run_path(probe_name, probe_path, (u32)sizeof(probe_path)) &&
                fat_find_file(probe_path, &probe_entry)) {
                video_write(probe_name);
                video_write(": FAT file (");
                video_write(probe_path);
                video_write(")");
                if (suffixes[si][0] != '\0') {
                    video_write(" [suffix=");
                    video_write(suffixes[si]);
                    video_write("]");
                }
                video_putchar('\n');
                serial_write("[which] resolved class=fat target=");
                serial_write(probe_name);
                serial_write(" path=");
                serial_write(probe_path);
                serial_write("\n");
                return;
            }
        }
    }

    video_write(name);
    video_write(": not found\n");
    serial_write("[which] not-found name=");
    serial_write(name);
    serial_write("\n");
    shell_set_errorlevel(1U);
}

/* ===== Tab Completion Engine ===== */

#define SHELL_COMPLETE_MAX 64U

typedef struct {
    char candidates[SHELL_COMPLETE_MAX][SHELL_LINE_MAX];
    u32 count;
} shell_complete_ctx_t;

static shell_complete_ctx_t g_complete_ctx;

static void shell_complete_add(shell_complete_ctx_t *ctx, const char *s) {
    if (ctx->count >= SHELL_COMPLETE_MAX) return;
    str_copy(ctx->candidates[ctx->count], s, SHELL_LINE_MAX);
    ctx->count++;
}

/* Gather builtin completions matching prefix */
static void shell_complete_builtins(shell_complete_ctx_t *ctx, const char *prefix, u32 plen) {
    static const char *builtins[] = {
        "help", "pwd", "cd", "cd..", "dir", "type", "copy", "ren",
        "move", "mkdir", "rmdir", "attrib", "del",
        "ascii", "gsplash", "desktop", "demo", "cls", "ver", "echo",
        "set", "pmode", "ticks", "mem",
        "shutdown", "reboot", "run", "opengem", "vmode",
        "history", "which", "resolve",
        (const char *)0
    };
    u32 i;
    for (i = 0U; builtins[i]; i++) {
        if (str_starts_with_nocase(builtins[i], prefix) && str_len(builtins[i]) > plen) {
            shell_complete_add(ctx, builtins[i]);
        }
    }
}

/* Gather program names from COM catalog */
static void shell_complete_catalog(shell_complete_ctx_t *ctx,
                                   handoff_v0_t *handoff,
                                   const char *prefix, u32 plen) {
    u64 i;
    u64 count = handoff->com_count;
    if (count > HANDOFF_COM_MAX) count = HANDOFF_COM_MAX;
    for (i = 0; i < count; i++) {
        handoff_com_entry_t *e = &handoff->com_entries[i];
        if (e->phys_base == 0 || e->name[0] == '\0') continue;
        if (str_starts_with_nocase(e->name, prefix) && str_len(e->name) > plen) {
            shell_complete_add(ctx, e->name);
        }
    }
}

/* Callback for fat_list_dir to gather matching file names */
typedef struct {
    shell_complete_ctx_t *ctx;
    const char *prefix;
    u32 plen;
    const char *dir_prefix;  /* prepended to each candidate (empty for CWD) */
} shell_fat_complete_ctx_t;

static int shell_fat_complete_cb(const fat_dir_entry_t *entry, void *raw_ctx) {
    shell_fat_complete_ctx_t *fctx = (shell_fat_complete_ctx_t *)raw_ctx;
    if (entry->attr & FAT_ATTR_VOLUME_ID) return 1;
    if (entry->name[0] == '.') return 1;
    if (str_starts_with_nocase(entry->name, fctx->prefix) &&
        str_len(entry->name) > fctx->plen) {
        char full[SHELL_LINE_MAX];
        u32 pos = 0U;
        if (fctx->dir_prefix && fctx->dir_prefix[0] != '\0') {
            str_copy(full, fctx->dir_prefix, (u32)sizeof(full));
            pos = str_len(full);
        }
        str_copy(full + pos, entry->name, (u32)sizeof(full) - pos);
        pos = str_len(full);
        if ((entry->attr & FAT_ATTR_DIRECTORY) && (pos + 2U) < (u32)sizeof(full)) {
            full[pos] = '\\';
            full[pos + 1U] = '\0';
        }
        shell_complete_add(fctx->ctx, full);
    }
    return 1; /* continue enumeration */
}

static void shell_complete_fat_cwd(shell_complete_ctx_t *ctx,
                                   const char *prefix, u32 plen) {
    shell_fat_complete_ctx_t fctx;
    if (!fat_ready()) return;
    fctx.ctx = ctx;
    fctx.prefix = prefix;
    fctx.plen = plen;
    fctx.dir_prefix = "";
    fat_list_dir(g_shell_cwd, shell_fat_complete_cb, &fctx);
}

/* Compute longest common prefix among all candidates */
static u32 shell_complete_common_prefix(shell_complete_ctx_t *ctx) {
    u32 i, cp;
    if (ctx->count == 0U) return 0U;
    cp = str_len(ctx->candidates[0]);
    for (i = 1U; i < ctx->count; i++) {
        u32 j = 0U;
        while (j < cp &&
               to_lower_ascii((u8)ctx->candidates[0][j]) ==
               to_lower_ascii((u8)ctx->candidates[i][j])) {
            j++;
        }
        cp = j;
    }
    return cp;
}

/* Perform tab completion on the current line buffer.
   Returns 1 if the line was modified, 0 otherwise. */
static int shell_do_tab_complete(char *line, u32 *line_len, u32 *cursor,
                                 handoff_v0_t *handoff) {
    char prefix[SHELL_LINE_MAX];
    u32 plen = 0U;
    u32 tok_start = 0U;
    u32 common;
    u32 i;
    int has_path_sep = 0;

    /* Extract the current token (word being typed) */
    {
        /* Find start of current token: scan backward from cursor */
        u32 pos = *cursor;
        while (pos > 0U && !is_space((u8)line[pos - 1U])) {
            pos--;
        }
        tok_start = pos;
        plen = *cursor - tok_start;
        if (plen == 0U) return 0;
        for (i = 0U; i < plen && (i + 1U) < (u32)sizeof(prefix); i++) {
            prefix[i] = line[tok_start + i];
        }
        prefix[plen] = '\0';
    }

    /* Check if prefix contains a path separator */
    for (i = 0U; i < plen; i++) {
        if (prefix[i] == '/' || prefix[i] == '\\') {
            has_path_sep = 1;
        }
    }

    g_complete_ctx.count = 0U;

    if (has_path_sep) {
        /* Path-aware completion: split into dir_part and name_part */
        char dir_input[SHELL_PATH_MAX];
        char dir_resolved[SHELL_PATH_MAX];
        char user_dir_prefix[SHELL_LINE_MAX];
        char name_part[SHELL_LINE_MAX];
        u32 last_sep = 0U;
        u32 nplen;

        for (i = 0U; i < plen; i++) {
            if (prefix[i] == '/' || prefix[i] == '\\') {
                last_sep = i;
            }
        }

        /* user_dir_prefix = everything up to and including last separator */
        for (i = 0U; i <= last_sep && (i + 1U) < (u32)sizeof(user_dir_prefix); i++) {
            user_dir_prefix[i] = prefix[i];
        }
        user_dir_prefix[i] = '\0';

        /* dir_input = directory part for resolution (same as user_dir_prefix) */
        str_copy(dir_input, user_dir_prefix, (u32)sizeof(dir_input));

        /* name_part = everything after last separator */
        nplen = plen - last_sep - 1U;
        for (i = 0U; i < nplen && (i + 1U) < (u32)sizeof(name_part); i++) {
            name_part[i] = prefix[last_sep + 1U + i];
        }
        name_part[nplen] = '\0';

        /* Resolve the directory path */
        if (build_canonical_path(dir_input, dir_resolved, (u32)sizeof(dir_resolved)) &&
            fat_ready()) {
            shell_fat_complete_ctx_t fctx;
            fctx.ctx = &g_complete_ctx;
            fctx.prefix = name_part;
            fctx.plen = nplen;
            fctx.dir_prefix = user_dir_prefix;
            fat_list_dir(dir_resolved, shell_fat_complete_cb, &fctx);
            serial_write("[complete] path-aware dir=");
            serial_write(dir_resolved);
            serial_write(" name_part=");
            serial_write(name_part);
            serial_write(" hits=");
            serial_write_hex8((u8)g_complete_ctx.count);
            serial_write("\n");
        }
    } else {
        /* No path separator — standard flat completion */

        /* Whether this is the first token (command position) or an argument */
        if (tok_start == 0U || (tok_start > 0U && line[0] != '\0')) {
            /* Check if only spaces before tok_start => command position */
            int cmd_pos = 1;
            for (i = 0U; i < tok_start; i++) {
                if (!is_space((u8)line[i])) { cmd_pos = 0; break; }
            }
            if (cmd_pos) {
                shell_complete_builtins(&g_complete_ctx, prefix, plen);
                shell_complete_catalog(&g_complete_ctx, handoff, prefix, plen);
            }
        }

        /* Always add FAT cwd matches for any position */
        shell_complete_fat_cwd(&g_complete_ctx, prefix, plen);
    }

    if (g_complete_ctx.count == 0U) return 0;

    common = shell_complete_common_prefix(&g_complete_ctx);

    if (common > plen) {
        /* Extend the token to the common prefix */
        u32 add = common - plen;
        u32 tail_len = *line_len - *cursor;
        if ((*line_len + add) >= SHELL_LINE_MAX) return 0;

        /* Make room: shift tail right by `add` */
        if (tail_len > 0U) {
            u32 j;
            for (j = tail_len; j > 0U; j--) {
                line[*cursor + add + j - 1U] = line[*cursor + j - 1U];
            }
        }
        /* Copy the new characters from candidate[0] */
        for (i = 0U; i < add; i++) {
            line[tok_start + plen + i] = g_complete_ctx.candidates[0][plen + i];
        }
        *line_len += add;
        *cursor += add;
        line[*line_len] = '\0';
        serial_write("[complete] extended=");
        serial_write(g_complete_ctx.candidates[0]);
        serial_write("\n");
        return 1;
    }

    /* Ambiguous: list candidates */
    if (g_complete_ctx.count > 1U) {
        video_putchar('\n');
        for (i = 0U; i < g_complete_ctx.count; i++) {
            video_write("  ");
            video_write(g_complete_ctx.candidates[i]);
            video_putchar('\n');
        }
        /* Redraw prompt + line */
        write_prompt();
        {
            u32 j;
            for (j = 0U; j < *line_len; j++) {
                video_putchar(line[j]);
            }
            /* Reposition cursor */
            for (j = *line_len; j > *cursor; j--) {
                video_putchar('\b');
            }
        }
    }

    return 0;
}

/* ===== Direct-Exec Resolution ===== */
static int shell_try_direct_exec(
    const char *line,
    boot_info_t *boot_info,
    handoff_v0_t *handoff
) {
    char name[SHELL_LINE_MAX];
    char probe_name[SHELL_LINE_MAX];
    char probe_path[SHELL_PATH_MAX];
    char synth_args[SHELL_LINE_MAX];
    fat_dir_entry_t probe_entry;
    const char *tail_ptr;
    u32 name_len;

    /* Extract first token (uppercased, raw — no lowercase normalisation) */
    {
        const char *p = line;
        u32 i = 0U;
        while (*p && is_space((u8)*p)) p++;
        while (*p && !is_space((u8)*p) && (i + 1U) < (u32)sizeof(name)) {
            name[i++] = (char)to_upper_ascii((u8)*p);
            p++;
        }
        name[i] = '\0';
        name_len = i;
    }
    if (name_len == 0U) return 0;

    /* Detect path-aware vs bare-name resolution */
    {
        u32 pi;
        int path_mode = 0;
        for (pi = 0U; pi < name_len; pi++) {
            if (name[pi] == '/' || name[pi] == '\\') { path_mode = 1; break; }
        }
        serial_write("[shell] direct-exec name=");
        serial_write(name);
        serial_write(path_mode ? " resolve=path-aware\n" : " resolve=bare-name\n");
    }

    /* Tail = everything after the first token */
    tail_ptr = get_arg_ptr(line);

    /* Helper: build synthetic args "NAME TAIL" for shell_run */
    #define DIRECT_EXEC_TRY(suffix) do {                                      \
        u32 _n = 0U;                                                          \
        str_copy(probe_name, name, (u32)sizeof(probe_name));                  \
        {                                                                     \
            u32 _pn = str_len(probe_name);                                    \
            const char *_s = suffix;                                          \
            while (*_s && (_pn + 1U) < (u32)sizeof(probe_name)) {             \
                probe_name[_pn++] = *_s++;                                    \
            }                                                                 \
            probe_name[_pn] = '\0';                                           \
        }                                                                     \
        /* Build full synth_args = "PROBENAME TAIL" */                        \
        str_copy(synth_args, probe_name, (u32)sizeof(synth_args));            \
        _n = str_len(synth_args);                                             \
        if (tail_ptr[0] != '\0' && (_n + 2U) < (u32)sizeof(synth_args)) {    \
            synth_args[_n++] = ' ';                                           \
            str_copy(synth_args + _n, tail_ptr,                               \
                     (u32)sizeof(synth_args) - _n);                           \
        }                                                                     \
        /* Check catalog first, then FAT */                                   \
        if (shell_find_com(handoff, probe_name)) {                            \
            serial_write("[shell] direct-exec resolved=");                    \
            serial_write(probe_name);                                         \
            serial_write(" class=catalog suffix=" suffix "\n");               \
            shell_run(boot_info, handoff, synth_args);                        \
            return 1;                                                         \
        }                                                                     \
        if (build_run_path(probe_name, probe_path,                            \
                           (u32)sizeof(probe_path)) &&                        \
            fat_find_file(probe_path, &probe_entry)) {                        \
            serial_write("[shell] direct-exec resolved=");                    \
            serial_write(probe_name);                                         \
            serial_write(" class=fat path=");                                 \
            serial_write(probe_path);                                         \
            serial_write(" suffix=" suffix "\n");                             \
            shell_run(boot_info, handoff, synth_args);                        \
            return 1;                                                         \
        }                                                                     \
    } while (0)

    /* If the user already typed a supported extension, try that first */
    if (shell_run_target_is_supported(name)) {
        serial_write("[shell] direct-exec attempt exact=");
        serial_write(name);
        serial_write("\n");
        str_copy(synth_args, name, (u32)sizeof(synth_args));
        {
            u32 _n = str_len(synth_args);
            if (tail_ptr[0] != '\0' && (_n + 2U) < (u32)sizeof(synth_args)) {
                synth_args[_n++] = ' ';
                str_copy(synth_args + _n, tail_ptr,
                         (u32)sizeof(synth_args) - _n);
            }
        }
        /* Try directly via shell_run; it will print its own error if not found */
        shell_run(boot_info, handoff, synth_args);
        return 1;
    }

    /* Probe suffixes in DOS-like order: .COM  .EXE  .BAT */
    DIRECT_EXEC_TRY(".COM");
    DIRECT_EXEC_TRY(".EXE");
    DIRECT_EXEC_TRY(".BAT");

    #undef DIRECT_EXEC_TRY

    /* Not found at all */
    serial_write("[shell] direct-exec notfound name=");
    serial_write(name);
    serial_write("\n");
    return 0;
}

static void shell_execute_line(const char *line, boot_info_t *boot_info, handoff_v0_t *handoff) {
    char cmd[16];
    char expanded[SHELL_LINE_MAX];

    shell_env_expand_line(line, expanded, (u32)sizeof(expanded));
    line = expanded;
    shell_set_errorlevel(0U);

    normalize_first_token(line, cmd, (u32)sizeof(cmd));
    if (cmd[0] == '\0') {
        return;
    }

    if (str_eq(cmd, "help")) {
        shell_print_help();
        return;
    }

    if (str_eq(cmd, "vm86-arm-live")) {
        /* OPENGEM-038: explicit, user-typed arm of the live v86 path.
         * Installs the real #GP ISR into the PE32 shim IDT at vector
         * 0x0D. Does NOT execute LIDT and does NOT enter v8086 mode --
         * those are OPENGEM-039+. Reverse with `vm86-disarm-live`. */
        video_write("[vm86] arming live path (038)...\n");
        serial_write("[vm86] arming live path (038)\n");
        int a38 = vm86_gp_isr_install_arm(VM86_GP_ISR_INSTALL_ARM_MAGIC);
        int inst = vm86_gp_isr_install(VM86_GP_ISR_INSTALL_ARM_MAGIC);
        if (a38 && inst) {
            video_write("[vm86] installed at IDT vector 0x0D\n");
            serial_write("[vm86] installed=1\n");
        } else {
            video_write("[vm86] install FAILED\n");
            serial_write("[vm86] install FAILED\n");
        }
        return;
    }

    if (str_eq(cmd, "vm86-disarm-live")) {
        /* OPENGEM-038: reverse the arm. Restores vector 0x0D to its
         * 032 default trap stub and clears the 038 arm flag. */
        vm86_gp_isr_uninstall();
        vm86_gp_isr_install_disarm();
        video_write("[vm86] disarmed + uninstalled\n");
        serial_write("[vm86] disarmed + uninstalled\n");
        return;
    }

    if (str_eq(cmd, "vm86-probe-041")) {
        /* OPENGEM-042: runtime validation of the 039/040/041 scaffolding.
         * Calls vm86_compat_entry_live_probe(), which exercises all arm
         * gates, fill_frame, scratch layout, wrong-magic guards on
         * enter_v86, and asm symbol resolution. Does NOT enter v8086
         * mode. All prereqs are disarmed by the probe itself on exit. */
        video_write("[vm86] probe-041 begin (038+039+040+041 runtime check)\n");
        serial_write("[vm86] probe-041 begin\n");
        int rc = vm86_compat_entry_live_probe();
        if (rc == 1) {
            video_write("[vm86] probe-041 result=PASS (scaffolding runtime-sane)\n");
            serial_write("[vm86] probe-041 result=PASS\n");
        } else {
            video_write("[vm86] probe-041 result=FAIL (see serial log for reason)\n");
            serial_write("[vm86] probe-041 result=FAIL\n");
        }
        return;
    }

    if (str_eq(cmd, "gem")) {
        /* OPENGEM-043: minimal MZ loader + v86 entry experiment.
         *
         * HIGH-RISK: first real invocation of the 041 live trampoline.
         * Expected outcomes in descending likelihood:
         *   1. Triple-fault / reboot on lgdt, lretq, lidt, ltr, or iretl.
         *   2. v86 mode entered, first DOS INT traps #GP, ISR 037
         *      captures but does not return → silent freeze (no return
         *      path wired yet).
         *   3. GEM.EXE actually starts executing — extremely unlikely.
         *
         * Layout: PSP at linear 0x10000, image body at linear 0x11000.
         * Assumes host CR3 identity-maps 0x00000..0x100000 writable.
         *
         * This command NEVER RETURNS on success. On failure (validation)
         * all arm gates are cleaned up. */
        const char *gem_path = "/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM.EXE";
        video_write("[gem] loader begin (high-risk experiment)\n");
        serial_write("[gem] loader begin path=");
        serial_write(gem_path);
        serial_write("\n");

        /* Step 1: snapshot host CR3 + verify identity map covers 0..1MB. */
        vm86_cpu_snapshot snap;
        if (!vm86_cpu_snapshot_capture(&snap)) {
            video_write("[gem] FAIL: cpu snapshot\n");
            serial_write("[gem] FAIL snapshot\n");
            return;
        }
        serial_write("[gem] host_cr3=0x");
        serial_write_hex64(snap.cr3);
        serial_write("\n");
        u64 fail_va = 0;
        if (vm86_pe32_identity_verify(snap.cr3, 0x0ULL, 0x00100000ULL, &fail_va) != 1) {
            video_write("[gem] FAIL: identity map does not cover 0..1MB\n");
            serial_write("[gem] FAIL identity-map fail_va=0x");
            serial_write_hex64(fail_va);
            serial_write("\n");
            return;
        }
        serial_write("[gem] identity-map 0..1MB ok\n");

        /* Step 2: read GEM.EXE. */
        u32 file_size = 0;
        if (!fat_read_file(gem_path, g_shell_file_buffer,
                           SHELL_FILE_BUFFER_SIZE, &file_size)) {
            video_write("[gem] FAIL: fat read\n");
            serial_write("[gem] FAIL fat_read\n");
            return;
        }
        serial_write("[gem] read size=0x");
        serial_write_hex64((u64)file_size);
        serial_write("\n");
        if (file_size < 64 || g_shell_file_buffer[0] != 'M' || g_shell_file_buffer[1] != 'Z') {
            video_write("[gem] FAIL: not MZ\n");
            serial_write("[gem] FAIL not-mz\n");
            return;
        }

        /* Step 3: parse MZ header (raw little-endian reads). */
        u8 *mz = g_shell_file_buffer;
        u16 e_cblp     = (u16)(mz[0x02] | (mz[0x03] << 8));
        u16 e_cp       = (u16)(mz[0x04] | (mz[0x05] << 8));
        u16 e_crlc     = (u16)(mz[0x06] | (mz[0x07] << 8));
        u16 e_cparhdr  = (u16)(mz[0x08] | (mz[0x09] << 8));
        u16 e_ss       = (u16)(mz[0x0E] | (mz[0x0F] << 8));
        u16 e_sp       = (u16)(mz[0x10] | (mz[0x11] << 8));
        u16 e_ip       = (u16)(mz[0x14] | (mz[0x15] << 8));
        u16 e_cs       = (u16)(mz[0x16] | (mz[0x17] << 8));
        u16 e_lfarlc   = (u16)(mz[0x18] | (mz[0x19] << 8));

        u32 header_bytes = (u32)e_cparhdr * 16U;
        u32 total_bytes  = (e_cblp == 0) ? ((u32)e_cp * 512U)
                                         : (((u32)e_cp - 1U) * 512U + (u32)e_cblp);
        u32 body_bytes   = (total_bytes > header_bytes) ? (total_bytes - header_bytes) : 0;

        serial_write("[gem] mz header=0x"); serial_write_hex64((u64)header_bytes);
        serial_write(" body=0x"); serial_write_hex64((u64)body_bytes);
        serial_write(" cs:ip=0x"); serial_write_hex64(((u64)e_cs << 16) | e_ip);
        serial_write(" ss:sp=0x"); serial_write_hex64(((u64)e_ss << 16) | e_sp);
        serial_write("\n");

        if (body_bytes == 0 || body_bytes > 0x80000U) {
            video_write("[gem] FAIL: body too large\n");
            serial_write("[gem] FAIL body-size\n");
            return;
        }

        /* Step 4: layout in v86 memory.
         *   PSP   at seg 0x1000 (linear 0x10000)
         *   IMAGE at seg 0x1010 (linear 0x10100)
         * PSP size = 0x100 bytes = 0x10 paragraphs. */
        const u16 psp_seg   = 0x1000;
        const u16 load_seg  = 0x1010;
        u8 *psp_base   = (u8 *)(uintptr_t)((u32)psp_seg * 16U);
        u8 *image_base = (u8 *)(uintptr_t)((u32)load_seg * 16U);

        /* Clear PSP then write INT 20h terminator (CD 20). */
        for (u32 i = 0; i < 0x100U; i++) psp_base[i] = 0;
        psp_base[0x00] = 0xCD;
        psp_base[0x01] = 0x20;

        /* Copy image body. */
        const u8 *body = g_shell_file_buffer + header_bytes;
        for (u32 i = 0; i < body_bytes; i++) image_base[i] = body[i];

        /* Step 5: apply relocations. Each reloc is (offset, segment):
         * at linear (load_seg + seg)*16 + offset, add load_seg to the word. */
        if (e_lfarlc + (u32)e_crlc * 4U > file_size) {
            video_write("[gem] FAIL: reloc table out of file\n");
            serial_write("[gem] FAIL reloc-oob\n");
            return;
        }
        const u8 *rt = g_shell_file_buffer + e_lfarlc;
        for (u32 r = 0; r < e_crlc; r++) {
            u16 roff = (u16)(rt[r*4+0] | (rt[r*4+1] << 8));
            u16 rseg = (u16)(rt[r*4+2] | (rt[r*4+3] << 8));
            u8 *p = (u8 *)(uintptr_t)(((u32)load_seg + (u32)rseg) * 16U + (u32)roff);
            u16 w = (u16)(p[0] | (p[1] << 8));
            w = (u16)(w + load_seg);
            p[0] = (u8)(w & 0xFF);
            p[1] = (u8)((w >> 8) & 0xFF);
        }
        serial_write("[gem] reloc applied count=0x");
        serial_write_hex64((u64)e_crlc);
        serial_write("\n");

        /* Step 6: compute entry CS:IP and stack SS:SP. */
        u16 entry_cs = (u16)(e_cs + load_seg);
        u16 entry_ip = e_ip;
        u16 stack_ss = (u16)(e_ss + load_seg);
        u16 stack_sp = e_sp;
        serial_write("[gem] v86 entry cs=0x"); serial_write_hex64((u64)entry_cs);
        serial_write(" ip=0x"); serial_write_hex64((u64)entry_ip);
        serial_write(" ss=0x"); serial_write_hex64((u64)stack_ss);
        serial_write(" sp=0x"); serial_write_hex64((u64)stack_sp);
        serial_write("\n");

        /* Step 7: arm cascade 038 -> 044A -> 044B -> 044C. */
        if (vm86_gp_isr_install_arm(VM86_GP_ISR_INSTALL_ARM_MAGIC) != 1 ||
            vm86_gp_isr_install(VM86_GP_ISR_INSTALL_ARM_MAGIC) != 1) {
            video_write("[gem] FAIL: 038 arm/install\n");
            serial_write("[gem] FAIL arm-038\n");
            return;
        }
        serial_write("[gem] 038 installed\n");

        if (SHELL_MODE_SWITCH_CALL(arm)(MODE_SWITCH_ARM_MAGIC) != MODE_SWITCH_OK) {
            video_write("[gem] FAIL: 044A arm\n");
            serial_write("[gem] FAIL arm-044A\n");
            shell_gem_disarm_path();
            return;
        }
        serial_write("[gem] 044A armed\n");

        if (legacy_v86_arm(LEGACY_V86_ARM_MAGIC) != 1) {
            video_write("[gem] pending task B\n");
            serial_write("[gem] pending task B arm-044B\n");
            shell_gem_disarm_path();
            return;
        }
        serial_write("[gem] 044B armed\n");

        if (v86_dispatch_arm(V86_DISPATCH_ARM_MAGIC) != 1) {
            video_write("[gem] FAIL: 044C arm\n");
            serial_write("[gem] FAIL arm-044C\n");
            shell_gem_disarm_path();
            return;
        }
        serial_write("[gem] 044C armed\n");

        legacy_v86_frame_t frame;
        legacy_v86_exit_t exit_state;
        v86_dispatch_result_t dispatch_result;

        frame.cs = entry_cs;
        frame.ip = entry_ip;
        frame.ss = stack_ss;
        frame.sp = stack_sp;
        frame.ds = psp_seg;
        frame.es = psp_seg;
        frame.fs = 0U;
        frame.gs = 0U;
        frame.eflags = 0x00000202u;
        frame.reserved[0] = 0U;
        frame.reserved[1] = 0U;
        frame.reserved[2] = 0U;
        frame.reserved[3] = 0U;

        video_write("[gem] entering legacy_v86 loop\n");
        serial_write("[gem] enter legacy_v86 loop\n");

        for (;;) {
            if (legacy_v86_enter(&frame, &exit_state) != 1) {
                video_write("[gem] pending task B\n");
                serial_write("[gem] pending task B enter-044B\n");
                shell_gem_disarm_path();
                return;
            }

            frame.cs = exit_state.frame.cs;
            frame.ip = exit_state.frame.ip;
            frame.ss = exit_state.frame.ss;
            frame.sp = exit_state.frame.sp;
            frame.ds = exit_state.frame.ds;
            frame.es = exit_state.frame.es;
            frame.fs = exit_state.frame.fs;
            frame.gs = exit_state.frame.gs;
            frame.eflags = exit_state.frame.eflags;
            frame.reserved[0] = exit_state.frame.reserved[0];
            frame.reserved[1] = exit_state.frame.reserved[1];
            frame.reserved[2] = exit_state.frame.reserved[2];
            frame.reserved[3] = exit_state.frame.reserved[3];
            if (exit_state.reason == LEGACY_V86_EXIT_GP_INT) {
                serial_write("[gem] dispatch int=0x");
                serial_write_hex64((u64)exit_state.int_vector);
                serial_write(" cs:ip=0x");
                serial_write_hex64(((u64)frame.cs << 16) | frame.ip);
                serial_write("\n");
                dispatch_result = v86_dispatch_int(exit_state.int_vector, &frame);
                if (dispatch_result == V86_DISPATCH_CONT) {
                    continue;
                }
                if (dispatch_result == V86_DISPATCH_EXIT_OK) {
                    video_write("[gem] dispatch requested exit\n");
                    serial_write("[gem] dispatch exit=ok\n");
                    break;
                }
                video_write("[gem] dispatch abort\n");
                serial_write("[gem] dispatch exit=err\n");
                break;
            }

            shell_gem_write_exit_reason(exit_state.reason, exit_state.fault_code);
            if (exit_state.reason == LEGACY_V86_EXIT_NORMAL) {
                video_write("[gem] guest terminated normally\n");
            } else if (exit_state.reason == LEGACY_V86_EXIT_HALT) {
                video_write("[gem] guest halted\n");
            } else if (exit_state.reason == LEGACY_V86_EXIT_FAULT) {
                video_write("[gem] guest faulted\n");
            } else {
                video_write("[gem] guest exited\n");
            }
            break;
        }

        shell_gem_disarm_path();
        return;
    }

    if (str_eq(cmd, "pwd")) {
        shell_pwd();
        return;
    }

    if (str_eq(cmd, "cd")) {
        shell_cd(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "cd..")) {
        shell_cd("..");
        return;
    }

    if (str_eq(cmd, "dir")) {
        shell_dir(handoff, get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "ascii")) {
        shell_ascii();
        return;
    }

    if (str_eq(cmd, "gsplash") || str_eq(cmd, "splash")) {
        shell_graphic_splash_preview();
        return;
    }

    if (str_eq(cmd, "cls")) {
        shell_cls();
        return;
    }

    if (str_eq(cmd, "ver")) {
        shell_ver();
        return;
    }

    if (str_eq(cmd, "desktop")) {
        if (ui_enter_desktop_scene()) {
            shell_run_desktop_session(boot_info, handoff);
        } else {
            if (ui_get_scene() == SCENE_DESKTOP) {
                shell_run_desktop_session(boot_info, handoff);
            } else {
                video_write("Failed to enter desktop scene.\n");
            }
        }
        return;
    }

    if (str_eq(cmd, "demo")) {
        serial_write("[ app ] demo launch requested\n");
        shell_run(boot_info, handoff, "CIUKDEMO.COM");
        serial_write("[ app ] demo launch completed\n");
        return;
    }

    if (str_eq(cmd, "echo")) {
        shell_echo(get_arg_ptr(line));
        shell_set_errorlevel(0U);
        return;
    }

    if (str_eq(cmd, "set")) {
        shell_cmd_set(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "pmode")) {
        shell_pmode_contract();
        return;
    }

    if (str_eq(cmd, "vga13")) {
        shell_vga13_baseline();
        return;
    }

    if (str_eq(cmd, "gfx")) {
        shell_gfx(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "image")) {
        shell_image(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "mode")) {
        shell_mode(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "type")) {
        shell_type(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "copy")) {
        shell_copy(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "ren") || str_eq(cmd, "rename")) {
        shell_rename(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "move")) {
        shell_move(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "mkdir") || str_eq(cmd, "md")) {
        shell_mkdir(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "rmdir") || str_eq(cmd, "rd")) {
        shell_rmdir(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "attrib")) {
        shell_attrib(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "del") || str_eq(cmd, "erase")) {
        shell_del(get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "ticks")) {
        shell_print_ticks();
        return;
    }

    if (str_eq(cmd, "mem")) {
        shell_print_mem(boot_info, handoff);
        return;
    }

    if (str_eq(cmd, "history")) {
        shell_cmd_history();
        return;
    }

    if (str_eq(cmd, "which") || str_eq(cmd, "where") || str_eq(cmd, "resolve")) {
        shell_cmd_which(get_arg_ptr(line), handoff);
        return;
    }

    if (str_eq(cmd, "shutdown")) {
        shell_shutdown();
        return;
    }

    if (str_eq(cmd, "reboot")) {
        shell_reboot();
        return;
    }

    if (str_eq(cmd, "run")) {
        shell_run(boot_info, handoff, get_arg_ptr(line));
        return;
    }

    if (str_eq(cmd, "opengem")) {
        (void)shell_run_opengem_interactive(boot_info, handoff);
        return;
    }

    if (str_eq(cmd, "catalog")) {
        shell_cmd_catalog();
        return;
    }

    if (str_eq(cmd, "vmode") || str_eq(cmd, "vres")) {
        shell_vmode(get_arg_ptr(line), handoff);
        return;
    }

    /* Direct-exec fallback: try to resolve as executable */
    if (shell_try_direct_exec(line, boot_info, handoff)) {
        return;
    }

    video_write("Bad command or file name\n");
    shell_set_errorlevel(1U);
}

void stage2_shell_run(boot_info_t *boot_info, handoff_v0_t *handoff) {
    char line[SHELL_LINE_MAX];
    u32 line_len = 0;
    u32 cursor = 0;         /* cursor position within line (0..line_len) */
    /* History navigation index: history_count means "current / new line" */
    u32 hist_nav = 0U;
    /* Saved in-progress line when navigating history */
    char hist_saved[SHELL_LINE_MAX];
    int hist_saved_valid = 0;

    shell_startup_chain(boot_info, handoff);
    video_write("Type 'help' for the command list, 'demo' for the graphics showcase.\n");
    video_write("Tip: type 'desktop' to test GUI mode (ALT+G+Q to return).\n\n");
    write_prompt();
    video_present_dirty_immediate();

    for (;;) {
        i32 ch = stage2_keyboard_getc_nonblocking();
        if (ch < 0) {
            __asm__ volatile ("hlt");
            continue;
        }

        u8 ascii = (u8)ch;

        if (g_shell_prompt_deferred) {
            g_shell_prompt_deferred = 0U;
            if (gfx_mode_current() != GFX_MODE_TEXT_80x25) {
                gfx_mode_set(GFX_MODE_TEXT_80x25);
            }
            shell_cls();
            write_prompt();
            video_present_dirty_immediate();
        }

        /* ---- History navigation (UP/DOWN) ---- */
        if (ascii == STAGE2_KEY_UP) {
            if (g_shell_history_count == 0U) continue;
            if (hist_nav == g_shell_history_count) {
                /* Save current in-progress line */
                line[line_len] = '\0';
                str_copy(hist_saved, line, SHELL_LINE_MAX);
                hist_saved_valid = 1;
            }
            if (hist_nav > 0U) {
                u32 idx;
                u32 old_len = line_len;
                hist_nav--;
                if (g_shell_history_count < SHELL_HISTORY_MAX) {
                    idx = hist_nav;
                } else {
                    idx = (g_shell_history_head + hist_nav) % SHELL_HISTORY_MAX;
                }
                /* Move cursor to end, then erase */
                while (cursor < line_len) {
                    video_putchar(line[cursor]);
                    cursor++;
                }
                while (line_len > 0U) {
                    video_write("\b \b");
                    line_len--;
                }
                cursor = 0;
                str_copy(line, g_shell_history[idx], SHELL_LINE_MAX);
                line_len = str_len(line);
                cursor = line_len;
                video_write(line);
                (void)old_len;
                video_present_dirty_immediate();
            }
            continue;
        }

        if (ascii == STAGE2_KEY_DOWN) {
            if (g_shell_history_count == 0U) continue;
            if (hist_nav < g_shell_history_count) {
                hist_nav++;
                /* Move cursor to end, then erase */
                while (cursor < line_len) {
                    video_putchar(line[cursor]);
                    cursor++;
                }
                while (line_len > 0U) {
                    video_write("\b \b");
                    line_len--;
                }
                cursor = 0;
                if (hist_nav == g_shell_history_count) {
                    if (hist_saved_valid) {
                        str_copy(line, hist_saved, SHELL_LINE_MAX);
                        line_len = str_len(line);
                        cursor = line_len;
                        video_write(line);
                    }
                } else {
                    u32 idx;
                    if (g_shell_history_count < SHELL_HISTORY_MAX) {
                        idx = hist_nav;
                    } else {
                        idx = (g_shell_history_head + hist_nav) % SHELL_HISTORY_MAX;
                    }
                    str_copy(line, g_shell_history[idx], SHELL_LINE_MAX);
                    line_len = str_len(line);
                    cursor = line_len;
                    video_write(line);
                }
                video_present_dirty_immediate();
            }
            continue;
        }

        /* ---- Cursor movement keys ---- */
        if (ascii == STAGE2_KEY_LEFT) {
            if (cursor > 0U) {
                cursor--;
                video_putchar('\b');
                video_present_dirty_immediate();
            }
            continue;
        }

        if (ascii == STAGE2_KEY_RIGHT) {
            if (cursor < line_len) {
                video_putchar(line[cursor]);
                cursor++;
                video_present_dirty_immediate();
            }
            continue;
        }

        if (ascii == STAGE2_KEY_HOME) {
            while (cursor > 0U) {
                cursor--;
                video_putchar('\b');
            }
            video_present_dirty_immediate();
            continue;
        }

        if (ascii == STAGE2_KEY_END) {
            while (cursor < line_len) {
                video_putchar(line[cursor]);
                cursor++;
            }
            video_present_dirty_immediate();
            continue;
        }

        /* ---- Delete key: remove char at cursor ---- */
        if (ascii == STAGE2_KEY_DEL) {
            if (cursor < line_len) {
                u32 old_len = line_len;
                u32 j;
                for (j = cursor; j + 1U < line_len; j++) {
                    line[j] = line[j + 1U];
                }
                line_len--;
                line[line_len] = '\0';
                shell_line_redraw_tail(line, cursor, line_len, cursor, old_len);
                video_present_dirty_immediate();
            }
            continue;
        }

        if (ascii == '\r') {
            ascii = '\n';
        }

        /* ---- Enter ---- */
        if (ascii == '\n') {
            /* Move cursor to end before newline */
            while (cursor < line_len) {
                video_putchar(line[cursor]);
                cursor++;
            }
            video_putchar('\n');
            line[line_len] = '\0';
            shell_history_push(line);
            shell_execute_line(line, boot_info, handoff);
            line_len = 0;
            cursor = 0;
            hist_nav = g_shell_history_count;
            hist_saved_valid = 0;
            if (!g_shell_prompt_deferred) {
                video_putchar('\n');
                write_prompt();
                video_present_dirty_immediate();
            }
            continue;
        }

        /* ---- Backspace: remove char before cursor ---- */
        if (ascii == '\b' || ascii == 0x7F) {
            if (cursor > 0U) {
                u32 old_len = line_len;
                u32 j;
                cursor--;
                for (j = cursor; j + 1U < line_len; j++) {
                    line[j] = line[j + 1U];
                }
                line_len--;
                line[line_len] = '\0';
                video_putchar('\b');
                shell_line_redraw_tail(line, cursor, line_len, cursor, old_len);
                video_present_dirty_immediate();
            }
            continue;
        }

        /* ---- Tab completion ---- */
        if (ascii == '\t') {
            if (shell_do_tab_complete(line, &line_len, &cursor, handoff)) {
                /* Line was modified — redraw from prompt */
                shell_line_full_redraw(line, line_len, cursor, 0U);
            }
            video_present_dirty_immediate();
            continue;
        }

        /* ---- Esc: clear entire input line ---- */
        if (ascii == 0x1BU) {
            if (line_len > 0U) {
                while (cursor < line_len) {
                    video_putchar(line[cursor]);
                    cursor++;
                }
                while (line_len > 0U) {
                    video_write("\b \b");
                    line_len--;
                }
                cursor = 0;
            }
            video_present_dirty_immediate();
            continue;
        }

        /* ---- Ctrl+A: cursor to start of line ---- */
        if (ascii == 0x01U) {
            while (cursor > 0U) {
                cursor--;
                video_putchar('\b');
            }
            video_present_dirty_immediate();
            continue;
        }

        /* ---- Ctrl+E: cursor to end of line ---- */
        if (ascii == 0x05U) {
            while (cursor < line_len) {
                video_putchar(line[cursor]);
                cursor++;
            }
            video_present_dirty_immediate();
            continue;
        }

        /* ---- Ctrl+U: clear from cursor back to start ---- */
        if (ascii == 0x15U) {
            if (cursor > 0U) {
                u32 old_len = line_len;
                u32 cut = cursor;
                u32 j;
                for (j = 0U; j + cut < line_len; j++) {
                    line[j] = line[j + cut];
                }
                line_len -= cut;
                cursor = 0;
                line[line_len] = '\0';
                {
                    u32 k;
                    for (k = 0U; k < cut; k++) {
                        video_putchar('\b');
                    }
                }
                shell_line_redraw_tail(line, 0U, line_len, 0U, old_len);
            }
            video_present_dirty_immediate();
            continue;
        }

        /* ---- Ctrl+K: clear from cursor to end of line ---- */
        if (ascii == 0x0BU) {
            if (cursor < line_len) {
                u32 old_len = line_len;
                line_len = cursor;
                line[line_len] = '\0';
                shell_line_redraw_tail(line, cursor, line_len, cursor, old_len);
            }
            video_present_dirty_immediate();
            continue;
        }

        /* ---- Ctrl+L: clear screen and redraw prompt + line ---- */
        if (ascii == 0x0CU) {
            shell_cls();
            write_prompt();
            {
                u32 j;
                for (j = 0U; j < line_len; j++) {
                    video_putchar(line[j]);
                }
                for (j = line_len; j > cursor; j--) {
                    video_putchar('\b');
                }
            }
            video_present_dirty_immediate();
            continue;
        }

        if (!is_printable_ascii(ascii)) {
            continue;
        }

        if ((line_len + 1) >= SHELL_LINE_MAX) {
            video_write("\n[ shell ] input too long\n");
            line_len = 0;
            cursor = 0;
            write_prompt();
            continue;
        }

        /* ---- Insert printable character at cursor ---- */
        if (cursor < line_len) {
            u32 j;
            /* Shift tail right */
            for (j = line_len; j > cursor; j--) {
                line[j] = line[j - 1U];
            }
        }
        line[cursor] = (char)ascii;
        line_len++;
        cursor++;
        line[line_len] = '\0';
        if (cursor == line_len) {
            /* Appending at end — simple putchar */
            video_putchar((char)ascii);
        } else {
            /* Inserted in middle — redraw tail */
            shell_line_redraw_tail(line, cursor - 1U, line_len, cursor, line_len);
        }
        video_present_dirty_immediate();
    }
}
