#include "shell.h"
#include "video.h"
#include "keyboard.h"
#include "timer.h"
#include "services.h"
#include "fat.h"
#include "dos_mz.h"
#include "splash.h"
#include "version.h"
#include "ui.h"
#include "serial.h"

#define SHELL_LINE_MAX 128
#define SHELL_FILE_BUFFER_SIZE (128U * 1024U)
#define SHELL_RUNTIME_COM_ADDR 0x0000000000600000ULL
#define SHELL_RUNTIME_COM_MAX_SIZE (512U * 1024U)
#define SHELL_RUNTIME_PSP_SIZE 0x100U
#define SHELL_RUNTIME_COM_ENTRY_ADDR (SHELL_RUNTIME_COM_ADDR + SHELL_RUNTIME_PSP_SIZE)
#define SHELL_RUNTIME_COM_MAX_PAYLOAD (SHELL_RUNTIME_COM_MAX_SIZE - SHELL_RUNTIME_PSP_SIZE)
#define SHELL_RUNTIME_TAIL_MAX 126U
#define SHELL_EXE32_MARKER_SIZE 8U
#define SHELL_PATH_MAX 128
#define SHELL_PATH_MAX_TOKENS 16
#define SHELL_PATH_TOKEN_MAX 13
#define SHELL_INT21_MAX_FILE_HANDLES 8U
#define SHELL_INT21_HANDLE_BASE 5U
#define SHELL_INT21_FILE_BUF_CAP (64U * 1024U)
#define SHELL_INT21_MEM_MAX_BLOCKS 32U

static const u8 g_exe32_marker[SHELL_EXE32_MARKER_SIZE] = {
    'C', 'I', 'U', 'K', 'E', 'X', '6', '4'
};

static u8 g_shell_file_buffer[SHELL_FILE_BUFFER_SIZE];
static char g_shell_cwd[SHELL_PATH_MAX] = "/EFI/CIUKIOS";

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
    u8 reserved[3];
} shell_int21_mem_block_t;

static shell_int21_file_handle_t g_int21_file_handles[SHELL_INT21_MAX_FILE_HANDLES];

static int shell_dir_fat_cb(const fat_dir_entry_t *entry, void *ctx_void);
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
    video_write("  ticks    - show PIT tick counter\n");
    video_write("  mem      - show boot memory info\n");
    video_write("  shutdown - power off the machine\n");
    video_write("  reboot   - reboot the machine\n");
    video_write("  run      - execute default COM (or INIT.COM)\n");
    video_write("  run X A  - run COM or load EXE with optional args\n");
    video_write("  ozone    - launch oZone GUI (if installed)\n");
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

static int normalize_com_name(const char *args, char *out, u32 out_size) {
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

static void shell_com_int21_4c(ciuki_dos_context_t *ctx, u8 code);
static u32 g_int21_vectors[256];
static u8 g_int21_last_return_code = 0U;
static u8 g_int21_last_termination_type = 0U;
static i32 g_int21_pending_stdin_char = -1;
static u8 g_int21_default_drive = 0U;
static u16 g_int21_dta_segment = 0U;
static u16 g_int21_dta_offset = 0x0080U;
static shell_int21_mem_block_t g_int21_mem_blocks[SHELL_INT21_MEM_MAX_BLOCKS];
static u16 g_int21_mem_block_count = 0U;

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

static int shell_int21_mem_insert_block(u16 index, u16 seg, u16 paras, u8 used) {
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

static int shell_int21_mem_alloc(u16 paras, u16 *seg_out, u16 *max_avail_out) {
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
            if (seg_out) {
                *seg_out = b->seg;
            }
            return 1;
        }

        if (!shell_int21_mem_insert_block(
                (u16)(i + 1U),
                (u16)(b->seg + paras),
                (u16)(b->paras - paras),
                0U)) {
            return 0;
        }
        b->paras = paras;
        b->used = 1U;
        if (seg_out) {
            *seg_out = b->seg;
        }
        return 1;
    }

    return 0;
}

static int shell_int21_mem_free(u16 seg) {
    u16 idx = 0U;
    if (!shell_int21_mem_find_used_block(seg, &idx)) {
        return 0;
    }
    g_int21_mem_blocks[idx].used = 0U;
    shell_int21_mem_merge_around(idx);
    return 1;
}

static int shell_int21_mem_resize(u16 seg, u16 new_paras, u16 *max_out) {
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

    if (ah == 0x2FU) {
        regs->es = g_int21_dta_segment;
        regs->bx = g_int21_dta_offset;
        return;
    }

    if (ah == 0x51U || ah == 0x62U) {
        /* Get PSP address (DOS-compatible subset): return current PSP segment in BX. */
        regs->bx = ctx->psp_segment;
        return;
    }

    if (ah == 0x4DU) {
        /* Get return code (DOS-compatible subset). AH=termination type, AL=code. */
        regs->ax = (u16)(((u16)g_int21_last_termination_type << 8) | (u16)g_int21_last_return_code);
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

    if (ah == 0x48U) {
        u16 seg = 0U;
        u16 max_free = 0U;

        if (shell_int21_mem_alloc(regs->bx, &seg, &max_free)) {
            regs->ax = seg;
            return;
        }

        regs->carry = 1U;
        regs->ax = 0x0008U; /* insufficient memory */
        regs->bx = max_free;
        return;
    }

    if (ah == 0x49U) {
        if (shell_int21_mem_free(regs->es)) {
            regs->ax = 0x0000U;
            return;
        }

        regs->carry = 1U;
        regs->ax = 0x0009U; /* invalid memory block address */
        return;
    }

    if (ah == 0x4AU) {
        u16 max_for_block = 0U;

        if (shell_int21_mem_resize(regs->es, regs->bx, &max_for_block)) {
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

    regs->carry = 1U;
    regs->ax = 0x0001U;
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

    /* AH=4Ah resize block */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4A00U;
    regs.bx = 0x0010U;
    regs.es = 0x5000U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x0000U) {
        return 0;
    }

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

    /* AH=4Dh: get return code + termination type */
    g_int21_last_return_code = 0x5AU;
    g_int21_last_termination_type = 0x01U;
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4D00U;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U || regs.ax != 0x015AU) {
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
    static const char payload[] = "CIUKIOS_INT21_FAT_E2E";
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
    local_memcpy(runtime_mem + data_off, payload, (u32)payload_len);

    /* Best-effort cleanup from previous runs. */
    (void)fat_delete_file("/EFI/CIUKIOS/I21E2E.TXT");

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

    /* AH=41h delete */
    local_memset(&regs, 0U, (u32)sizeof(regs));
    regs.ax = 0x4100U;
    regs.dx = path_off;
    shell_com_int21(&ctx, &regs);
    if (regs.carry != 0U) {
        goto fail;
    }

    /* Verify deletion at FAT layer. */
    if (fat_find_file("/EFI/CIUKIOS/I21E2E.TXT", &info)) {
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
    (void)fat_delete_file("/EFI/CIUKIOS/I21E2E.TXT");
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

    if (image_size == 0U || image_size > SHELL_RUNTIME_COM_MAX_PAYLOAD) {
        video_write("Invalid COM size.\n");
        return;
    }

    if (image_size >= 2U) {
        const u8 *image = (const u8 *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR;
        is_mz = (image[0] == 'M' && image[1] == 'Z');
    }

    if (is_mz) {
        if (!dos_mz_build_loaded_image(
                (u8 *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR,
                image_size,
                load_segment,
                &mz_info,
                &image_size,
                &reloc_applied
            )) {
            video_write("Invalid MZ executable.\n");
            return;
        }
    }

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
    svc.int20 = shell_com_int20;
    svc.int21_4c = shell_com_int21_4c;
    svc.terminate = shell_com_terminate;

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
        video_write("\n");

        if (!marker_ok || image_size < 12U) {
            video_write("MZ runtime dispatch pending (16-bit execution path).\n");
            return;
        }

        stub_entry_off = (u32)module[8]
                       | ((u32)module[9] << 8)
                       | ((u32)module[10] << 16)
                       | ((u32)module[11] << 24);

        if (stub_entry_off >= image_size) {
            video_write("MZ dispatch marker invalid entry offset.\n");
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
        return;
    }

    entry(&ctx, &svc);
    shell_int21_close_all_handles();
    shell_publish_last_exit_status(&ctx);
    shell_print_com_exit(&ctx);
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
    char tail[128];
    handoff_com_entry_t *entry;

    extract_run_tail(args, tail, (u32)sizeof(tail));

    if (!normalize_com_name(args, target, (u32)sizeof(target))) {
        if (handoff->com_phys_base != 0 && handoff->com_phys_size != 0U) {
            if (shell_run_from_catalog(
                    boot_info,
                    handoff,
                    handoff->com_phys_base,
                    handoff->com_phys_size,
                    "default",
                    ""
                )) {
                return;
            }
            video_write("Default COM metadata is invalid.\n");
            return;
        }
        if (shell_run_from_fat(boot_info, handoff, "INIT.COM", "")) {
            return;
        }
        video_write("Usage: run <name>\n");
        return;
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
            return;
        }
        video_write("COM entry metadata is invalid: ");
        video_write(entry->name);
        video_write("\n");
    }

    if (shell_run_from_fat(boot_info, handoff, target, tail)) {
        return;
    }

    video_write("COM not found: ");
    video_write(target);
    video_write("\n");
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
    /* ACPI S5 soft-off: QEMU PIIX4 PM1a_CNT at 0x604, SLP_EN (bit 13) */
    outw_port(0x604, 0x2000);
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void shell_reboot(void) {
    video_write("Rebooting...\n");
    /* Keyboard controller system reset pulse via bit 0 of port 0x64 */
    outb_port(0x64, 0xFE);
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
    ui_render_scene();
    ui_render_windows();
    ui_render_launcher();

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
    }

    /* --- EXITING --- */
    ui_set_console_source((ui_console_t *)0);
    ui_deactivate_launcher();
    shell_cls();
    shell_draw_title_bar();
    video_write("Desktop session closed. Type 'desktop' to reopen.\n");
    serial_write("[ ui ] desktop session ended\n");
}

static void shell_execute_line(const char *line, boot_info_t *boot_info, handoff_v0_t *handoff) {
    char cmd[16];

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

    if (str_eq(cmd, "ozone")) {
        serial_write("[ app ] ozone launch requested\n");

        /* Try known locations for OZONE.EXE */
        {
            fat_dir_entry_t probe;
            const char *paths[] = {
                "/FREEDOS/OZONE/OZONE.EXE",
                "/EFI/CIUKIOS/OZONE.EXE",
                "/OZONE.EXE",
            };
            const char *found_path = (const char *)0;
            int pi;

            for (pi = 0; pi < 3; pi++) {
                if (fat_find_file(paths[pi], &probe)) {
                    found_path = paths[pi];
                    break;
                }
            }

            if (found_path) {
                video_write("Launching oZone GUI: ");
                video_write(found_path);
                video_write("\n");
                serial_write("[ app ] ozone found: ");
                serial_write(found_path);
                serial_write("\n");
                shell_run(boot_info, handoff, "OZONE.EXE");
                serial_write("[ app ] ozone launch completed\n");
            } else {
                video_write("oZone GUI not found.\n");
                video_write("Install: scripts/import_ozonegui.sh --source <dir>\n");
                serial_write("[ app ] ozone not found on disk\n");
            }
        }
        return;
    }

    video_write("Unknown command. Type 'help'.\n");
}

void stage2_shell_run(boot_info_t *boot_info, handoff_v0_t *handoff) {
    char line[SHELL_LINE_MAX];
    u32 line_len = 0;

    video_write("Tip: type 'desktop' to test GUI mode (ALT+G+Q to return).\n");
    write_prompt();

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
            continue;
        }

        if (ascii == '\b' || ascii == 0x7F) {
            if (line_len > 0) {
                line_len--;
                video_write("\b \b");
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
    }
}
