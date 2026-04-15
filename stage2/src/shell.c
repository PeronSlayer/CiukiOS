#include "shell.h"
#include "serial.h"
#include "keyboard.h"
#include "timer.h"

#define SHELL_LINE_MAX 128

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

static void write_prompt(void) {
    serial_write("A:\\> ");
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
    serial_write("Commands:\n");
    serial_write("  help  - show this help\n");
    serial_write("  ticks - show PIT tick counter\n");
    serial_write("  mem   - show boot memory info\n");
}

static void shell_print_ticks(void) {
    serial_write("ticks=0x");
    serial_write_hex64(stage2_timer_ticks());
    serial_write("\n");
}

static void shell_print_mem(boot_info_t *boot_info, handoff_v0_t *handoff) {
    serial_write("memory_map_ptr=0x");
    serial_write_hex64(boot_info->memory_map_ptr);
    serial_write(" size=0x");
    serial_write_hex64(boot_info->memory_map_size);
    serial_write(" desc_size=0x");
    serial_write_hex64(boot_info->memory_map_descriptor_size);
    serial_write("\n");

    serial_write("kernel_phys_base=0x");
    serial_write_hex64(boot_info->kernel_phys_base);
    serial_write(" kernel_phys_size=0x");
    serial_write_hex64(boot_info->kernel_phys_size);
    serial_write("\n");

    serial_write("stage2_load_addr=0x");
    serial_write_hex64(handoff->stage2_load_addr);
    serial_write(" stage2_size=0x");
    serial_write_hex64(handoff->stage2_size);
    serial_write("\n");
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

    if (str_eq(cmd, "ticks")) {
        shell_print_ticks();
        return;
    }

    if (str_eq(cmd, "mem")) {
        shell_print_mem(boot_info, handoff);
        return;
    }

    serial_write("Unknown command. Type 'help'.\n");
}

void stage2_shell_run(boot_info_t *boot_info, handoff_v0_t *handoff) {
    char line[SHELL_LINE_MAX];
    u32 line_len = 0;

    serial_write("[ shell ] mini command loop active\n");
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
            serial_write("\n");
            line[line_len] = '\0';
            shell_execute_line(line, boot_info, handoff);
            line_len = 0;
            write_prompt();
            continue;
        }

        if (ascii == '\b' || ascii == 0x7F) {
            if (line_len > 0) {
                line_len--;
                serial_write("\b \b");
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
            serial_write("\n[ shell ] input too long\n");
            line_len = 0;
            write_prompt();
            continue;
        }

        line[line_len++] = (char)ascii;
        serial_write_char((char)ascii);
    }
}
