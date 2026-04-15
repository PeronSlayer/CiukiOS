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

static u8 g_shell_file_buffer[SHELL_FILE_BUFFER_SIZE];

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

static void write_prompt(void) {
    video_write("A:\\> ");
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
    video_write("  dir      - list files in A:\\EFI\\CIUKIOS\n");
    video_write("  type X   - show text file from FAT\n");
    video_write("  ascii    - show custom ASCII art\n");
    video_write("  cls      - clear screen\n");
    video_write("  ver      - show OS version\n");
    video_write("  echo     - print text to screen\n");
    video_write("  ticks    - show PIT tick counter\n");
    video_write("  mem      - show boot memory info\n");
    video_write("  shutdown - power off the machine\n");
    video_write("  reboot   - reboot the machine\n");
    video_write("  run      - execute default COM (or INIT.COM)\n");
    video_write("  run X    - execute COM by name (e.g. run init)\n");
}

static void shell_cls(void) {
    video_cls();
}

static void shell_ver(void) {
    video_write("CiukiOS Stage2 v0.1 (Phase 0 / DOS bootstrap)\n");
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

static int build_ciukios_path(const char *name, char *out, u32 out_size) {
    static const char prefix[] = "/EFI/CIUKIOS/";
    u32 i = 0;
    u32 j = 0;

    while (prefix[i] != '\0') {
        if ((j + 1) >= out_size) {
            return 0;
        }
        out[j++] = prefix[i++];
    }

    i = 0;
    while (name[i] != '\0') {
        char ch = (char)to_upper_ascii((u8)name[i]);
        if (ch == '\\') {
            ch = '/';
        }
        if ((j + 1) >= out_size) {
            return 0;
        }
        out[j++] = ch;
        i++;
    }

    out[j] = '\0';
    return 1;
}

static int build_type_path(const char *args, char *out, u32 out_size) {
    char token[96];
    u32 i = 0;
    u32 j = 0;

    if (!extract_first_arg(args, token, (u32)sizeof(token))) {
        return 0;
    }

    if (token[0] == '/' || token[0] == '\\') {
        while (token[i] != '\0') {
            char ch = token[i++];
            if (ch == '\\') {
                ch = '/';
            }
            ch = (char)to_upper_ascii((u8)ch);
            if ((j + 1) >= out_size) {
                return 0;
            }
            out[j++] = ch;
        }
        out[j] = '\0';
        return 1;
    }

    return build_ciukios_path(token, out, out_size);
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

    video_write("  ");
    video_write(entry->name);
    if (entry->attr & FAT_ATTR_DIRECTORY) {
        video_write("  <DIR>  cluster=0x");
        video_write_hex64(entry->first_cluster);
    } else {
        video_write("  size=0x");
        video_write_hex64(entry->size);
        video_write("  cluster=0x");
        video_write_hex64(entry->first_cluster);
    }
    video_write("\n");
    ctx->entries++;
    return 1;
}

static int shell_dir_from_fat(void) {
    shell_dir_ctx_t ctx;
    ctx.entries = 0;

    if (!fat_ready()) {
        return 0;
    }

    video_write("Directory of A:\\EFI\\CIUKIOS\n");
    if (!fat_list_dir("/EFI/CIUKIOS", shell_dir_fat_cb, &ctx)) {
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

static void shell_dir(handoff_v0_t *handoff) {
    if (!shell_dir_from_fat()) {
        shell_dir_from_catalog(handoff);
    }
}

static void shell_run_entry(boot_info_t *boot_info, handoff_v0_t *handoff, u64 phys_base, const char *name) {
    if (phys_base == 0) {
        video_write("No COM loaded.\n");
        return;
    }

    ciuki_services_t svc;
    svc.print       = video_write;
    svc.print_hex64 = video_write_hex64;
    svc.cls         = video_cls;

    video_write("Executing ");
    if (name && name[0] != '\0') {
        video_write(name);
    } else {
        video_write("COM");
    }
    video_write(" @ 0x");
    video_write_hex64(phys_base);
    video_write("\n");

    com_entry_t entry = (com_entry_t)phys_base;
    entry(boot_info, handoff, &svc);
}

static int shell_run_from_fat(boot_info_t *boot_info, handoff_v0_t *handoff, const char *com_name) {
    char path[128];
    u32 com_size = 0;

    if (!fat_ready()) {
        return 0;
    }
    if (!build_ciukios_path(com_name, path, (u32)sizeof(path))) {
        return 0;
    }
    if (!fat_read_file(path, (void *)(u64)SHELL_RUNTIME_COM_ADDR, SHELL_RUNTIME_COM_MAX_SIZE, &com_size)) {
        return 0;
    }
    if (com_size == 0U) {
        return 0;
    }

    shell_run_entry(boot_info, handoff, SHELL_RUNTIME_COM_ADDR, com_name);
    return 1;
}

static void shell_run(boot_info_t *boot_info, handoff_v0_t *handoff, const char *args) {
    char target[HANDOFF_COM_NAME_MAX + 1];
    handoff_com_entry_t *entry;

    if (!normalize_com_name(args, target, (u32)sizeof(target))) {
        if (handoff->com_phys_base != 0) {
            shell_run_entry(boot_info, handoff, handoff->com_phys_base, "default");
            return;
        }
        if (shell_run_from_fat(boot_info, handoff, "INIT.COM")) {
            return;
        }
        video_write("Usage: run <name>\n");
        return;
    }

    entry = shell_find_com(handoff, target);
    if (entry) {
        shell_run_entry(boot_info, handoff, entry->phys_base, entry->name);
        return;
    }

    if (shell_run_from_fat(boot_info, handoff, target)) {
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
    if (!build_type_path(args, path, (u32)sizeof(path))) {
        video_write("Usage: type <file>\n");
        return;
    }
    if (!fat_find_file(path, &info) || (info.attr & FAT_ATTR_DIRECTORY)) {
        video_write("File not found: ");
        video_write(path);
        video_write("\n");
        return;
    }
    if (info.size > SHELL_FILE_BUFFER_SIZE) {
        video_write("File too large for buffer (max 0x");
        video_write_hex64(SHELL_FILE_BUFFER_SIZE);
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

    if (str_eq(cmd, "dir")) {
        shell_dir(handoff);
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
