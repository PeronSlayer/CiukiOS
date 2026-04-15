#include "shell.h"
#include "video.h"
#include "keyboard.h"
#include "timer.h"
#include "services.h"
#include "fat.h"

#define SHELL_LINE_MAX 128
#define SHELL_FILE_BUFFER_SIZE (128U * 1024U)
#define SHELL_RUNTIME_COM_ADDR 0x0000000000600000ULL
#define SHELL_RUNTIME_COM_MAX_SIZE (512U * 1024U)
#define SHELL_RUNTIME_PSP_SIZE 0x100U
#define SHELL_RUNTIME_COM_ENTRY_ADDR (SHELL_RUNTIME_COM_ADDR + SHELL_RUNTIME_PSP_SIZE)
#define SHELL_RUNTIME_COM_MAX_PAYLOAD (SHELL_RUNTIME_COM_MAX_SIZE - SHELL_RUNTIME_PSP_SIZE)
#define SHELL_RUNTIME_TAIL_MAX 126U
#define SHELL_PATH_MAX 128
#define SHELL_PATH_MAX_TOKENS 16
#define SHELL_PATH_TOKEN_MAX 13

static u8 g_shell_file_buffer[SHELL_FILE_BUFFER_SIZE];
static char g_shell_cwd[SHELL_PATH_MAX] = "/EFI/CIUKIOS";

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
    video_write("  del X    - delete file from FAT cache\n");
    video_write("  ascii    - show custom ASCII art\n");
    video_write("  cls      - clear screen\n");
    video_write("  ver      - show OS version\n");
    video_write("  echo     - print text to screen\n");
    video_write("  ticks    - show PIT tick counter\n");
    video_write("  mem      - show boot memory info\n");
    video_write("  shutdown - power off the machine\n");
    video_write("  reboot   - reboot the machine\n");
    video_write("  run      - execute default COM (or INIT.COM)\n");
    video_write("  run X A  - execute COM with optional args (e.g. run init demo)\n");
}

static void shell_cls(void) {
    video_cls();
}

static void shell_ver(void) {
    video_write("CiukiOS Stage2 v0.2 (M1 DOS-like COM runtime)\n");
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

static void shell_prepare_psp(ciuki_dos_context_t *ctx, u32 image_size, const char *tail) {
    u8 *psp = (u8 *)(u64)SHELL_RUNTIME_COM_ADDR;
    u32 len = 0;
    u64 end_paragraph;

    local_memset(psp, 0U, SHELL_RUNTIME_PSP_SIZE);

    /* PSP:0000h = INT 20h */
    psp[0x00] = 0xCD;
    psp[0x01] = 0x20;

    end_paragraph = (SHELL_RUNTIME_COM_ENTRY_ADDR + (u64)image_size + 15ULL) >> 4;
    psp[0x02] = (u8)(end_paragraph & 0xFFU);
    psp[0x03] = (u8)((end_paragraph >> 8) & 0xFFU);

    if (tail) {
        while (tail[len] != '\0' && len < SHELL_RUNTIME_TAIL_MAX) {
            psp[0x81U + len] = (u8)tail[len];
            len++;
        }
    }
    psp[0x80] = (u8)len;
    psp[0x81U + len] = 0x0D;

    ctx->psp_segment = (u16)((SHELL_RUNTIME_COM_ADDR >> 4) & 0xFFFFU);
    ctx->psp_linear = SHELL_RUNTIME_COM_ADDR;
    ctx->image_linear = SHELL_RUNTIME_COM_ENTRY_ADDR;
    ctx->image_size = image_size;
    ctx->command_tail_len = (u8)len;
    ctx->command_tail[0] = '\0';
    for (u32 i = 0; i < len && i < (u32)sizeof(ctx->command_tail) - 1U; i++) {
        ctx->command_tail[i] = (char)psp[0x81U + i];
        ctx->command_tail[i + 1U] = '\0';
    }
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
    const u8 *image = (const u8 *)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR;
    com_entry_t entry = (com_entry_t)(u64)SHELL_RUNTIME_COM_ENTRY_ADDR;

    if (image_size == 0U || image_size > SHELL_RUNTIME_COM_MAX_PAYLOAD) {
        video_write("Invalid COM size.\n");
        return;
    }

    if (image_size >= 2U && image[0] == 'M' && image[1] == 'Z') {
        video_write("MZ executable detected. .EXE loader not implemented yet.\n");
        return;
    }

    local_memset(&ctx, 0U, (u32)sizeof(ctx));
    ctx.boot_info = boot_info;
    ctx.handoff = handoff;
    ctx.exit_reason = (u8)CIUKI_COM_EXIT_RETURN;
    ctx.exit_code = 0U;
    shell_prepare_psp(&ctx, image_size, tail);

    svc.print = video_write;
    svc.print_hex64 = video_write_hex64;
    svc.cls = video_cls;
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

    entry(&ctx, &svc);
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

    if (str_eq(cmd, "cls")) {
        shell_cls();
        return;
    }

    if (str_eq(cmd, "ver")) {
        shell_ver();
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

    video_write("Unknown command. Type 'help'.\n");
}

void stage2_shell_run(boot_info_t *boot_info, handoff_v0_t *handoff) {
    char line[SHELL_LINE_MAX];
    u32 line_len = 0;

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
