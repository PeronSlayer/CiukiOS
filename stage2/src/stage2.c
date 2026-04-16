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
#include "version.h"
#include "ui.h"

#define SPLASH_FOOTER_MIN_PX 64U
#define SPLASH_FOOTER_MAX_PX 120U

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static u32 splash_footer_height_px(void) {
    u32 h = video_height_px() / 7U;
    if (h < SPLASH_FOOTER_MIN_PX) {
        h = SPLASH_FOOTER_MIN_PX;
    }
    if (h > SPLASH_FOOTER_MAX_PX) {
        h = SPLASH_FOOTER_MAX_PX;
    }
    if (h >= video_height_px()) {
        h = video_height_px() / 4U;
    }
    return h;
}

static void draw_splash_footer(u32 footer_px, u32 progress_percent) {
    u32 fb_w = video_width_px();
    u32 fb_h = video_height_px();
    u32 y0;
    u32 bar_margin;
    u32 bar_x;
    u32 bar_y;
    u32 bar_w;
    u32 bar_h;
    u32 text_row;
    u32 loading_row;

    if (!video_ready() || footer_px == 0U || fb_w == 0U || fb_h == 0U) {
        return;
    }

    if (progress_percent > 100U) {
        progress_percent = 100U;
    }

    y0 = (footer_px >= fb_h) ? 0U : (fb_h - footer_px);
    ui_draw_panel(0U, y0, fb_w, footer_px, 0x00505050U, 0x00000000U);

    text_row = (y0 / 8U) + 1U;
    loading_row = text_row + 2U;
    video_set_colors(0x00FFFFFFU, 0x00000000U);
    ui_write_centered_row(text_row, "CiukiOS Stage2 " CIUKIOS_STAGE2_VERSION);
    ui_write_centered_row(loading_row, "Loading...");

    bar_margin = fb_w / 8U;
    if (bar_margin < 24U) {
        bar_margin = 24U;
    }
    if ((bar_margin * 2U) >= fb_w) {
        bar_margin = 8U;
    }

    bar_x = bar_margin;
    bar_w = fb_w - (bar_margin * 2U);
    bar_h = (footer_px >= 92U) ? 14U : 10U;
    if (bar_h >= footer_px) {
        bar_h = footer_px > 4U ? footer_px - 4U : 1U;
    }
    bar_y = y0 + footer_px - bar_h - 10U;
    if (bar_y < y0 + 4U) {
        bar_y = y0 + 4U;
    }

    ui_draw_progress_bar(
        bar_x, bar_y, bar_w, bar_h,
        progress_percent,
        0x00A0A0A0U,    /* border */
        0x00101010U,    /* bg */
        0x00E8E8E8U     /* fill */
    );
}

static void draw_title_bar(void) {
    ui_draw_top_bar("CiukiOS", 0x00FFFFFFU, 0x00000000U); /* white bar, black text */
    video_set_text_window(1);                   /* reserve top row for title bar */
}

static void show_boot_splash(void) {
    u64 start_ticks;
    const u64 max_wait_ticks = 200ULL; /* 2s @ 100Hz */
    u64 elapsed_ticks = 0ULL;
    u32 progress = 0U;
    u32 last_progress = 101U;
    u32 footer_px = 0U;
    int used_graphic = 0;
    int hud_drawn = 0;

    video_set_font_scale(1U, 1U);
    video_set_text_window(0);
    footer_px = splash_footer_height_px();
    used_graphic = stage2_splash_show_graphic_layout(footer_px);
    if (!used_graphic) {
        stage2_splash_show();
    } else {
        draw_splash_footer(footer_px, 0U);
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

    /* Draw boot HUD overlay if graphics available */
    if (used_graphic) {
        if (ui_draw_boot_hud(CIUKIOS_STAGE2_VERSION, "gfx", 0U)) {
            hud_drawn = 1;
            serial_write("[ ui ] boot hud active\n");
        }
    }

    start_ticks = stage2_timer_ticks();
    while ((stage2_timer_ticks() - start_ticks) < max_wait_ticks) {
        elapsed_ticks = stage2_timer_ticks() - start_ticks;
        progress = (u32)((elapsed_ticks * 100ULL) / max_wait_ticks);
        if (progress > 100U) {
            progress = 100U;
        }
        if (used_graphic && progress != last_progress) {
            draw_splash_footer(footer_px, progress);
            /* Update HUD progress if it was drawn */
            if (hud_drawn) {
                ui_draw_boot_hud(CIUKIOS_STAGE2_VERSION, "gfx", progress);
            }
            last_progress = progress;
        }

        if (stage2_keyboard_getc_nonblocking() >= 0) {
            break;
        }
        __asm__ volatile ("hlt");
    }

    if (used_graphic) {
        draw_splash_footer(footer_px, 100U);
        /* Final HUD update */
        if (hud_drawn) {
            ui_draw_boot_hud(CIUKIOS_STAGE2_VERSION, "gfx", 100U);
        }
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
    serial_write("[ compat ] INT10h baseline path ready (stage2 video text/gfx)\n");
    serial_write("[ compat ] INT16h baseline path ready (irq1 + key buffer)\n");
    serial_write("[ compat ] INT1Ah baseline path ready (pit tick source)\n");
    serial_write("[ compat ] INT21h PSP/status path ready (AH=51h/62h/4Dh)\n");
    serial_write("[ compat ] INT21h console/dta/drive ready (AH=06h/07h/0Ah/0Eh/1Ah/2Fh)\n");
    serial_write("[ compat ] INT21h io/handle baseline ready (AH=0Bh/0Ch/3Ch..42h)\n");
    if (stage2_shell_selftest_int21_baseline()) {
        serial_write("[ test ] int21 priority-a selftest: PASS\n");
    } else {
        serial_write("[ test ] int21 priority-a selftest: FAIL\n");
    }
    serial_write("[ ok ] stage2 mini shell ready (help/pwd/cd/dir/type/copy/ren/move/mkdir/rmdir/attrib/del/ascii/cls/ver/echo/ticks/mem/run/shutdown/reboot)\n");
    serial_write("[ ok ] desktop ui command available (type: desktop)\n");

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
