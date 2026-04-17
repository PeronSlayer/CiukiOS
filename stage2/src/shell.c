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
#include "ui.h"
#include "serial.h"
#include "gfx2d.h"
#include "image.h"
#include "gfx_modes.h"

#define SHELL_LINE_MAX 128
#define SHELL_FILE_BUFFER_SIZE (128U * 1024U)
#define SHELL_RUNTIME_COM_ADDR 0x0000000000600000ULL
#define SHELL_RUNTIME_COM_MAX_SIZE (512U * 1024U)
#define SHELL_RUNTIME_PSP_SIZE 0x100U
#define SHELL_RUNTIME_COM_ENTRY_ADDR (SHELL_RUNTIME_COM_ADDR + SHELL_RUNTIME_PSP_SIZE)
#define SHELL_RUNTIME_COM_MAX_PAYLOAD (SHELL_RUNTIME_COM_MAX_SIZE - SHELL_RUNTIME_PSP_SIZE)

/* Forward decl of gfx services table (defined later with shell_gfx command). */
static const ciuki_gfx_services_t g_gfx_services;
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

static shell_int21_file_handle_t g_int21_file_handles[SHELL_INT21_MAX_FILE_HANDLES];

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
    video_write("Commands:\n");
    video_write("  help     - show this help\n");
    video_write("  pwd      - show current directory\n");
    video_write("  cd X     - change current directory\n");
    video_write("  dir      - list files in current directory\n");
    video_write("  type X   - show text file from FAT\n");
    video_write("  copy X Y - copy file X to Y on FAT\n");
    video_write("  ren X Y  - rename file/dir X to Y (same dir)\n");
    video_write("  move X Y - move file X to Y or into dir Y\n");
    video_write("  mkdir X  - create directory X\n");
    video_write("  rmdir X  - remove empty directory X\n");
    video_write("  attrib X - show file attributes\n");
    video_write("  attrib +r|-r|+a|-a X - set/clear attribute\n");
    video_write("  del X    - delete file from FAT cache\n");
    video_write("  ascii    - show custom ASCII art\n");
    video_write("  gsplash  - show graphical splash preview\n");
    video_write("  desktop  - open interactive desktop scene (ALT+G+Q to return)\n");
    video_write("  cls      - clear screen\n");
    video_write("  ver      - show OS version\n");
    video_write("  echo     - print text to screen\n");
    video_write("  set      - show or set environment variables (set K=V)\n");
    video_write("  ticks    - show PIT tick counter\n");
    video_write("  mem      - show boot memory info\n");
    video_write("  shutdown - power off the machine\n");
    video_write("  reboot   - reboot the machine\n");
    video_write("  run      - execute default COM (or INIT.COM)\n");
    video_write("  run X A  - run COM/EXE/BAT with optional args\n");
    video_write("  pmode    - show protected-mode transition contract status\n");
    video_write("  opengem  - launch OpenGEM GUI (preflight + run)\n");
    video_write("  vmode    - video mode management (vmode help for details)\n");
    video_write("  vres     - alias for vmode\n");
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

static void shell_com_int31(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs) {
    (void)ctx;

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
         * Requested size is BX:CX; we return a synthetic non-zero
         * linear address in BX:CX and a non-zero memory handle in SI:DI
         * so real extenders can observe the shape of a successful alloc.
         * A zero-size request surfaces as DPMI error 8021h with carry set.
         */
        if (regs->bx == 0U && regs->cx == 0U) {
            regs->carry = 1U;
            regs->ax = 0x8021U; /* DPMI error: invalid value */
            return;
        }
        regs->bx = 0x0010U; /* linear address high word (synthetic) */
        regs->cx = 0x0000U; /* linear address low word (synthetic) */
        regs->si = 0x0010U; /* memory handle high word (synthetic) */
        regs->di = 0x0000U; /* memory handle low word (synthetic) */
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
    video_write("COM exit: ");
    if (ctx->exit_reason == (u8)CIUKI_COM_EXIT_INT20) {
        video_write("INT 20h");
    } else if (ctx->exit_reason == (u8)CIUKI_COM_EXIT_INT21_4C) {
        video_write("INT 21h/AH=4Ch");
    } else if (ctx->exit_reason == (u8)CIUKI_COM_EXIT_API) {
        video_write("terminate()");
    } else {
        video_write("RET");
    }
    video_write(" code=0x");
    video_write_hex64((u64)ctx->exit_code);
    video_write("\n");
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

    shell_dosrun_emit_launch_marker(name && name[0] != '\0' ? name : "default", run_type);

    local_memset(&ctx, 0U, (u32)sizeof(ctx));
    ctx.boot_info = boot_info;
    ctx.handoff = handoff;
    ctx.exit_reason = (u8)CIUKI_COM_EXIT_RETURN;
    ctx.exit_code = 0U;
    shell_prepare_psp(&ctx, image_size, tail);
    if (is_mz) {
        ctx.image_linear = SHELL_RUNTIME_COM_ENTRY_ADDR + (u64)mz_info.entry_offset;
    }

    svc.print = video_write;
    svc.print_hex64 = video_write_hex64;
    svc.cls = video_cls;
    svc.int21 = shell_com_int21;
    svc.int2f = shell_com_int2f;
    svc.int31 = shell_com_int31;
    svc.int20 = shell_com_int20;
    svc.int21_4c = shell_com_int21_4c;
    svc.terminate = shell_com_terminate;
    svc.gfx = &g_gfx_services;

    video_write("Executing ");
    if (name && name[0] != '\0') {
        video_write(name);
    } else {
        video_write("COM");
    }
    video_write(" PSP=0x");
    video_write_hex64((u64)ctx.psp_segment);
    video_write(" entry=0x");
    video_write_hex64(ctx.image_linear);
    video_write("\n");

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

        video_write("MZ loaded: bytes=0x");
        video_write_hex64((u64)image_size);
        video_write(" reloc=0x");
        video_write_hex64((u64)reloc_applied);
        video_write(" load_seg=0x");
        video_write_hex64((u64)load_segment);
        video_write(" entry_off=0x");
        video_write_hex64((u64)mz_info.entry_offset);
        video_write(" stack_off=0x");
        video_write_hex64((u64)mz_info.stack_offset);
        video_write(" span=0x");
        video_write_hex64((u64)mz_info.runtime_required_bytes);
        video_write("\n");

        if (!marker_ok || image_size < 12U) {
            video_write("MZ runtime dispatch pending (16-bit execution path).\n");
            shell_set_errorlevel(1U);
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_RUNTIME;
            return;
        }

        stub_entry_off = (u32)module[8]
                       | ((u32)module[9] << 8)
                       | ((u32)module[10] << 16)
                       | ((u32)module[11] << 24);

        if (stub_entry_off >= image_size) {
            video_write("MZ dispatch marker invalid entry offset.\n");
            shell_set_errorlevel(1U);
            g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_RUNTIME;
            return;
        }

        ctx.image_linear = SHELL_RUNTIME_COM_ENTRY_ADDR + (u64)stub_entry_off;
        entry = (com_entry_t)ctx.image_linear;
        video_write("MZ dispatch (CIUKEX64): entry=0x");
        video_write_hex64(ctx.image_linear);
        video_write("\n");

        entry(&ctx, &svc);
        shell_int21_close_all_handles();
        shell_publish_last_exit_status(&ctx);
        shell_print_com_exit(&ctx);
        shell_set_errorlevel(ctx.exit_code);
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
    if (ctx.exit_code != 0U && g_shell_dosrun_int21_unsupported_calls > 0U) {
        g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_UNSUPPORTED_INT21;
    } else {
        g_shell_dosrun_error_class = SHELL_DOSRUN_ERROR_NONE;
    }
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
    const char *title = "CiukiOS";
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
            }

            if (chord_stage == 1 && (ch == 'q' || ch == 'Q')) {
                if (alt_held || now <= chord_deadline) {
                    serial_write("[ ui ] exit chord alt+g+q triggered\n");
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

    g_shell_batch_depth++;
    while (pc < line_count && steps < SHELL_BATCH_MAX_STEPS) {
        char line[SHELL_LINE_MAX];
        char expanded[SHELL_LINE_MAX];
        steps++;

        str_copy(line, lines[pc], (u32)sizeof(line));
        trim_ascii_inplace(line);
        pc++;

        if (line[0] == '\0' || line[0] == ':') {
            continue;
        }
        if (str_starts_with_nocase(line, "rem ") || str_eq_nocase(line, "rem")) {
            continue;
        }

        shell_env_expand_line(line, expanded, (u32)sizeof(expanded));

        if (str_starts_with_nocase(expanded, "goto ")) {
            u16 target_line = 0U;
            const char *label = expanded + 5;
            while (*label && is_space((u8)*label)) {
                label++;
            }
            if (shell_batch_find_label(labels, label_count, label, &target_line)) {
                pc = (u32)target_line + 1U;
                continue;
            }
            video_write("GOTO label not found: ");
            video_write(label);
            video_write("\n");
            shell_set_errorlevel(1U);
            break;
        }

        if (str_starts_with_nocase(expanded, "if errorlevel ")) {
            const char *p = expanded + 14;
            u32 threshold = 0U;
            while (*p && is_space((u8)*p)) {
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                threshold = (threshold * 10U) + (u32)(*p - '0');
                p++;
            }
            while (*p && is_space((u8)*p)) {
                p++;
            }
            if (str_starts_with_nocase(p, "goto ")) {
                if ((u32)shell_get_errorlevel() >= threshold) {
                    u16 target_line = 0U;
                    const char *label = p + 5;
                    while (*label && is_space((u8)*label)) {
                        label++;
                    }
                    if (shell_batch_find_label(labels, label_count, label, &target_line)) {
                        pc = (u32)target_line + 1U;
                    } else {
                        video_write("IF ERRORLEVEL target missing: ");
                        video_write(label);
                        video_write("\n");
                        shell_set_errorlevel(1U);
                        break;
                    }
                }
                continue;
            }
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
        shell_set_errorlevel(1U);
    }
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
    video_write("VGA mode 13h baseline v0 (compatibility scaffold):\n");
    video_write("  width=320 height=200 bpp=8 palette=256\n");
    video_write("  framebuffer: GOP-backed virtual linear buffer (no real ISA VGA yet)\n");
    video_write("  palette: default DOS 256-color table pending\n");
    video_write("  status: readiness marker active, draw path deferred to DOOM graphics step\n");
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

static const ciuki_gfx_services_t g_gfx_services = {
    .begin_frame = video_begin_frame,
    .end_frame = video_end_frame,
    .put_pixel = gfx2d_pixel,
    .fill_rect = gfx2d_fill_rect,
    .rect = gfx2d_rect,
    .line = gfx2d_line,
    .circle = gfx2d_circle,
    .fill_circle = gfx2d_fill_circle,
    .fill_tri = gfx2d_fill_tri,
    .blit = gfx2d_blit,
    .get_fb_info = shell_gfx_get_fb_info,
    .set_mode = gfx_mode_set,
    .get_mode = gfx_mode_current,
    .present = gfx_mode_present,
    .set_palette = gfx_palette_set,
    .mode13_plane = gfx_mode13_plane,
    .mode13_put_pixel = gfx_mode13_put_pixel,
    .int10 = gfx_int10_dispatch,
    .palette_fade = gfx_palette_fade,
    .mode13_fill = gfx_mode13_fill,
    .mode13_fill_rect = gfx_mode13_fill_rect,
    .mode13_blit_indexed = gfx_mode13_blit_indexed,
    .mode13_blit_indexed_clip = gfx_mode13_blit_indexed_clip,
    .mode13_draw_column = gfx_mode13_draw_column,
    .palette_get_raw = gfx_palette_get_raw,
    .mode13_blit_scaled = gfx_mode13_blit_scaled,
    .mode13_blit_scaled_clip = gfx_mode13_blit_scaled_clip,
    .mode13_draw_column_masked = gfx_mode13_draw_column_masked,
    .mode13_draw_column_sampled_masked = gfx_mode13_draw_column_sampled_masked,
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

    if (str_eq(cmd, "pwd")) {
        shell_pwd();
        return;
    }

    if (str_eq(cmd, "cd")) {
        shell_cd(get_arg_ptr(line));
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
        serial_write("[ app ] opengem launch requested\n");
        /* Preflight probe — search candidate entries under FREEDOS/OPENGEM */
        {
            fat_dir_entry_t probe;
            static const char *paths[] = {
                "/FREEDOS/OPENGEM/GEM.BAT",
                "/FREEDOS/OPENGEM/GEM.EXE",
                "/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/DESKTOP.APP",
                "/FREEDOS/OPENGEM/OPENGEM.BAT",
                "/FREEDOS/OPENGEM/OPENGEM.EXE",
            };
            const char *found_path = (const char *)0;
            int pi;
            int preflight_ok = 1;

            serial_write("[ app ] opengem preflight started\n");

            /* Check 1: find a runnable entry */
            for (pi = 0; pi < 5; pi++) {
                if (fat_find_file(paths[pi], &probe)) {
                    found_path = paths[pi];
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
                preflight_ok = 0;
            }

            /* Check 2: FAT filesystem ready */
            if (fat_ready()) {
                video_write("[preflight] FAT layer: ready\n");
                serial_write("[ app ] opengem preflight fat: ok\n");
            } else {
                video_write("[preflight] FAT layer: NOT READY\n");
                serial_write("[ app ] opengem preflight fat: fail\n");
                preflight_ok = 0;
            }

            serial_write("[ app ] opengem preflight complete\n");

            if (!preflight_ok) {
                video_write("[preflight] FAILED - cannot launch OpenGEM\n");
                video_write("Install: scripts/import_opengem.sh\n");
                serial_write("[ app ] opengem preflight failed\n");
            } else {
                video_write("[preflight] PASSED - launching OpenGEM\n");
                serial_write("[ app ] opengem preflight passed\n");
                shell_run(boot_info, handoff, found_path);
                serial_write("[ app ] opengem launch completed\n");
            }
        }
        return;
    }

    if (str_eq(cmd, "vmode") || str_eq(cmd, "vres")) {
        shell_vmode(get_arg_ptr(line), handoff);
        return;
    }

    video_write("Unknown command. Type 'help'.\n");
    shell_set_errorlevel(1U);
}

void stage2_shell_run(boot_info_t *boot_info, handoff_v0_t *handoff) {
    char line[SHELL_LINE_MAX];
    u32 line_len = 0;

    shell_startup_chain(boot_info, handoff);
    video_write("Tip: type 'desktop' to test GUI mode (ALT+G+Q to return).\n");
    write_prompt();
    video_present_dirty_immediate();

    for (;;) {
        i32 ch = stage2_keyboard_getc_nonblocking();
        if (ch < 0) {
            __asm__ volatile ("hlt");
            continue;
        }

        u8 ascii = (u8)ch;

        if (ascii == '\r') {
            ascii = '\n';
        }

        if (ascii == '\n') {
            video_putchar('\n');
            line[line_len] = '\0';
            shell_execute_line(line, boot_info, handoff);
            line_len = 0;
            write_prompt();
            video_present_dirty_immediate();
            continue;
        }

        if (ascii == '\b' || ascii == 0x7F) {
            if (line_len > 0) {
                line_len--;
                video_write("\b \b");
                video_present_dirty_immediate();
            }
            continue;
        }

        if (ascii == '\t') {
            ascii = ' ';
        }

        if (!is_printable_ascii(ascii)) {
            continue;
        }

        if ((line_len + 1) >= SHELL_LINE_MAX) {
            video_write("\n[ shell ] input too long\n");
            line_len = 0;
            write_prompt();
            continue;
        }

        line[line_len++] = (char)ascii;
        video_putchar((char)ascii);
        video_present_dirty_immediate();
    }
}
