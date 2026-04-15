#include "serial.h"
#include "video.h"
#include "cpu_tables.h"
#include "interrupts.h"
#include "timer.h"
#include "keyboard.h"
#include "shell.h"
#include "splash.h"
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

static void show_boot_splash(void) {
    u64 start_ticks;
    const u64 max_wait_ticks = 200ULL; /* 2s @ 100Hz */
    int used_graphic = 0;

    video_set_text_window(0);
    used_graphic = stage2_splash_show_graphic();
    if (!used_graphic) {
        video_set_font_scale(1U, 1U);
        video_set_text_window(0);
        stage2_splash_show();
    }
    serial_write("[ ok ] splashscreen rendered src=0x");
    serial_write_hex64((u64)stage2_splash_source_cols());
    serial_write("x0x");
    serial_write_hex64((u64)stage2_splash_source_rows());
    serial_write(" mode=");
    serial_write(used_graphic ? "gfx" : "ascii");
    serial_write(" bpp=0x");
    serial_write_hex64((u64)video_bpp());
    serial_write("\n");

    start_ticks = stage2_timer_ticks();
    while ((stage2_timer_ticks() - start_ticks) < max_wait_ticks) {
        if (stage2_keyboard_getc_nonblocking() >= 0) {
            break;
        }
        __asm__ volatile ("hlt");
    }

    video_set_font_scale(2U, 2U);
    video_set_text_window(0);
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
    serial_write("[ info ] handoff framebuffer: base=0x");
    serial_write_hex64(handoff->framebuffer_base);
    serial_write(" pitch=0x");
    serial_write_hex64((u64)handoff->framebuffer_pitch);
    serial_write(" bpp=0x");
    serial_write_hex64((u64)handoff->framebuffer_bpp);
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
    serial_write("[ ok ] stage2 mini shell ready (help/pwd/cd/dir/type/copy/ren/move/mkdir/rmdir/attrib/del/ascii/cls/ver/echo/ticks/mem/run/shutdown/reboot)\n");

    video_init(boot_info);
    show_boot_splash();
    draw_title_bar();

    stage2_disk_init(handoff);
    if (stage2_disk_ready()) {
        serial_write("[ ok ] disk cache layer is available\n");
    } else {
        serial_write("[ warn ] disk cache layer unavailable\n");
    }

    if (fat_init()) {
        serial_write("[ ok ] FAT layer mounted (rw cache)\n");
    } else {
        serial_write("[ warn ] FAT layer not mounted\n");
    }

    serial_write("[ shell ] mini command loop active\n");
    serial_write("[ stage2 ] next step: handoff to DOS-like runtime\n");
    stage2_shell_run(boot_info, handoff);
}
