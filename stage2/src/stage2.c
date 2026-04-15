#include "serial.h"
#include "video.h"
#include "cpu_tables.h"
#include "interrupts.h"
#include "timer.h"
#include "keyboard.h"
#include "shell.h"
#include "disk.h"
#include "fat.h"
#include "bootinfo.h"
#include "handoff.h"

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static u32 local_strlen(const char *s) {
    u32 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void draw_title_bar(void) {
    const char *title = "CiukiOS";
    u32 cols = video_columns();
    u32 title_len = local_strlen(title);
    u32 start_col = 0;

    video_set_colors(0x00000000U, 0x00FFFFFFU); /* black on white */
    video_set_cursor(0, 0);
    for (u32 i = 0; i < cols; i++) {
        video_putchar(' ');
    }

    if (cols > title_len) {
        start_col = (cols - title_len) / 2;
    }
    video_set_cursor(start_col, 0);
    video_write(title);

    video_set_colors(0x00C0C0C0U, 0x00000000U); /* restore shell colors */
    video_set_text_window(1);                   /* reserve top row for title bar */
}

void stage2_main(boot_info_t *boot_info, handoff_v0_t *handoff) {
    serial_init();
    serial_write("\n[ stage2 ] scaffolding started\n");

    if (!boot_info) {
        serial_write("[ panic ] null boot_info\n");
        halt_forever();
    }

    if (boot_info->magic != BOOTINFO_MAGIC) {
        serial_write("[ panic ] invalid boot_info magic: 0x");
        serial_write_hex64(boot_info->magic);
        serial_write("\n");
        halt_forever();
    }

    serial_write("[ ok ] boot_info is valid\n");
    serial_write("[ info ] memory_map_ptr: 0x");
    serial_write_hex64(boot_info->memory_map_ptr);
    serial_write("\n");

    if (!handoff) {
        serial_write("[ panic ] null handoff\n");
        halt_forever();
    }

    if (handoff->magic != HANDOFF_V0_MAGIC) {
        serial_write("[ panic ] invalid handoff magic: 0x");
        serial_write_hex64(handoff->magic);
        serial_write("\n");
        halt_forever();
    }

    if (handoff->version != HANDOFF_V0_VERSION) {
        serial_write("[ panic ] invalid handoff version: 0x");
        serial_write_hex64(handoff->version);
        serial_write("\n");
        halt_forever();
    }

    serial_write("[ ok ] handoff v0 is valid\n");
    serial_write("[ info ] stage2_load_addr: 0x");
    serial_write_hex64(handoff->stage2_load_addr);
    serial_write("\n");
    serial_write("[ info ] stage2_size: 0x");
    serial_write_hex64(handoff->stage2_size);
    serial_write("\n");

    stage2_init_gdt_tss();
    serial_write("[ ok ] stage2 local gdt+tss is active\n");

    stage2_init_idt();
    serial_write("[ ok ] stage2 local idt is active\n");

    stage2_timer_init();
    serial_write("[ ok ] pic remapped and pit started\n");

    stage2_keyboard_init();
    serial_write("[ ok ] keyboard ring buffer + set1 decoder ready\n");

    stage2_enable_interrupts();
    serial_write("[ ok ] interrupts enabled (timer irq0 + keyboard irq1)\n");
    serial_write("[ ok ] stage2 mini shell ready (help/dir/type/ascii/cls/ver/echo/ticks/mem/run/shutdown/reboot)\n");

    video_init(boot_info);
    draw_title_bar();

    stage2_disk_init(handoff);
    if (stage2_disk_ready()) {
        serial_write("[ ok ] disk cache layer is available\n");
    } else {
        serial_write("[ warn ] disk cache layer unavailable\n");
    }

    if (fat_init()) {
        serial_write("[ ok ] FAT readonly layer mounted\n");
    } else {
        serial_write("[ warn ] FAT readonly layer not mounted\n");
    }

    serial_write("[ shell ] mini command loop active\n");
    serial_write("[ stage2 ] next step: handoff to DOS-like runtime\n");
    stage2_shell_run(boot_info, handoff);
}
