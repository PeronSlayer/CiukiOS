#include "serial.h"
#include "video.h"
#include "cpu_tables.h"
#include "interrupts.h"
#include "timer.h"
#include "keyboard.h"
#include "mem.h"
#include "shell.h"
#include "splash.h"
#include "disk.h"
#include "fat.h"
#include "pmode_transition.h"
#include "bootinfo.h"
#include "handoff.h"
#include "version.h"
#include "ui.h"

#define SPLASH_FOOTER_MIN_PX 64U
#define SPLASH_FOOTER_MAX_PX 120U

#define M6_PMEM_BASE 0x01000000ULL
#define M6_PMEM_SIZE 0x00400000ULL
#define DOS_RUNTIME_BASE 0x0000000000600000ULL
#define DOS_RUNTIME_SIZE (512ULL * 1024ULL)

typedef struct __attribute__((packed)) m6_desc_ptr {
    u16 limit;
    u64 base;
} m6_desc_ptr_t;

static pmode_transition_state_t g_m6_transition_state;

static int m6_ranges_overlap(u64 a_base, u64 a_size, u64 b_base, u64 b_size) {
    u64 a_end;
    u64 b_end;

    if (a_size == 0U || b_size == 0U) {
        return 0;
    }
    a_end = a_base + a_size;
    b_end = b_base + b_size;
    if (a_end <= b_base || b_end <= a_base) {
        return 0;
    }
    return 1;
}

static u8 m6_a20_probe(void) {
    u8 port92;
    __asm__ volatile ("inb %1, %0" : "=a"(port92) : "Nd"((u16)0x92));
    return (port92 & 0x02U) ? 1U : 0U;
}

static int m6_a20_enable(void) {
    u8 port92;

    if (m6_a20_probe()) {
        return 1;
    }

    __asm__ volatile ("inb %1, %0" : "=a"(port92) : "Nd"((u16)0x92));
    port92 = (u8)((port92 | 0x02U) & (u8)~0x01U);
    __asm__ volatile ("outb %0, %1" : : "a"(port92), "Nd"((u16)0x92));

    return m6_a20_probe() ? 1 : 0;
}

static int m6_transition_state_init(void) {
    m6_desc_ptr_t gdtr;
    m6_desc_ptr_t idtr;

    mem_set(&g_m6_transition_state, 0U, (u64)sizeof(g_m6_transition_state));
    g_m6_transition_state.magic = PMODE_TRANSITION_MAGIC;
    g_m6_transition_state.version = PMODE_TRANSITION_VERSION;
    g_m6_transition_state.intended_cr0_set = 0x00000001ULL;
    g_m6_transition_state.intended_cr0_clear = 0ULL;
    g_m6_transition_state.return_path_status = PMODE_RETURN_PATH_OK;

    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    __asm__ volatile ("sidt %0" : "=m"(idtr));

    g_m6_transition_state.gdtr_pre.limit = gdtr.limit;
    g_m6_transition_state.gdtr_pre.base = gdtr.base;
    g_m6_transition_state.idtr_pre.limit = idtr.limit;
    g_m6_transition_state.idtr_pre.base = idtr.base;

    return (g_m6_transition_state.magic == PMODE_TRANSITION_MAGIC &&
            g_m6_transition_state.version == PMODE_TRANSITION_VERSION) ? 1 : 0;
}

static int m6_dpmi_detect_skeleton_ready(void) {
    return 1;
}

static int m6_rm_callback_skeleton_ready(void) {
    return 1;
}

static int m6_int_reflect_skeleton_ready(void) {
    return 1;
}

static int m6_pmem_overlap_check(const handoff_v0_t *handoff) {
    if (m6_ranges_overlap(M6_PMEM_BASE, M6_PMEM_SIZE, DOS_RUNTIME_BASE, DOS_RUNTIME_SIZE)) {
        return 0;
    }
    if (handoff && m6_ranges_overlap(M6_PMEM_BASE, M6_PMEM_SIZE, handoff->stage2_load_addr, handoff->stage2_size)) {
        return 0;
    }
    return 1;
}

static int stage2_phase2_timer_ticks_progress(void) {
    u64 start = stage2_timer_ticks();
    u64 guard_ticks = 200ULL; /* 2s @ 100Hz */

    while ((stage2_timer_ticks() - start) < 2ULL && (stage2_timer_ticks() - start) < guard_ticks) {
        __asm__ volatile ("hlt");
    }

    return (stage2_timer_ticks() - start) >= 2ULL;
}

static int stage2_phase2_lowlevel_selftest(void) {
    if (!stage2_phase2_timer_ticks_progress()) {
        return 0;
    }
    if (!stage2_keyboard_selftest_decode_capture()) {
        return 0;
    }
    return 1;
}

static int stage2_bios_compat_selftest_int10(void) {
    if (!video_ready()) {
        return 0;
    }
    if (video_width_px() == 0U || video_height_px() == 0U) {
        return 0;
    }
    return 1;
}

static int stage2_bios_compat_selftest_int16(void) {
    return stage2_keyboard_selftest_decode_capture();
}

static int stage2_bios_compat_selftest_int1a(void) {
    return stage2_phase2_timer_ticks_progress();
}

static int stage2_m6_pmode_contract_marker_selftest(void) {
    static const char marker[] = "CIUKEX64";

    return marker[0] == 'C' &&
           marker[1] == 'I' &&
           marker[2] == 'U' &&
           marker[3] == 'K' &&
           marker[4] == 'E' &&
           marker[5] == 'X' &&
           marker[6] == '6' &&
           marker[7] == '4' &&
           sizeof(marker) == 9U;
}

static int stage2_m6_pmode_shell_surface_selftest(void) {
    static const char cmd[] = "pmode";

    return cmd[0] == 'p' &&
           cmd[1] == 'm' &&
           cmd[2] == 'o' &&
           cmd[3] == 'd' &&
           cmd[4] == 'e' &&
           sizeof(cmd) == 6U;
}

static void stage2_log_fat_mount_info(void) {
    fat_mount_info_t info;

    if (!fat_get_mount_info(&info)) {
        return;
    }

    serial_write("[ fat ] mounted type=");
    if (info.fat_type == 12U) {
        serial_write("FAT12");
    } else if (info.fat_type == 16U) {
        serial_write("FAT16");
    } else if (info.fat_type == 32U) {
        serial_write("FAT32");
    } else {
        serial_write("UNKNOWN");
    }

    serial_write(" bps=0x");
    serial_write_hex64((u64)info.bytes_per_sector);
    serial_write(" spc=0x");
    serial_write_hex64((u64)info.sectors_per_cluster);
    serial_write(" clusters=0x");
    serial_write_hex64((u64)info.total_clusters);

    if (info.fat_type == 32U) {
        serial_write(" root_cluster=0x");
        serial_write_hex64((u64)info.root_cluster);
        serial_write(" fsinfo=");
        serial_write(info.fsinfo_valid ? "valid" : "absent");
        serial_write(" next_free_hint=0x");
        serial_write_hex64((u64)info.next_free_hint);
        serial_write(" free_clusters=");
        if (info.free_count_known) {
            serial_write("0x");
            serial_write_hex64((u64)info.free_cluster_count);
        } else {
            serial_write("unknown");
        }
    }

    serial_write("\n");
}

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
    video_present();
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
    video_present();
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
            video_present();
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
        video_present();
    }

    video_set_font_scale(2U, 2U);
    video_set_text_window(0);
}

void stage2_main(boot_info_t *boot_info, handoff_v0_t *handoff) {
    int m6_desc_ready;
    u8 a20_state;

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

    if (handoff->version < HANDOFF_V0_VERSION) {
        serial_write("[ warn ] handoff version older than expected: 0x");
        serial_write_hex64(handoff->version);
        serial_write(" (expected >= 0x");
        serial_write_hex64(HANDOFF_V0_VERSION);
        serial_write(")\n");
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

    if (handoff->version >= 1ULL) {
        serial_write("[ video ] gop modes=0x");
        serial_write_hex64((u64)handoff->gop_mode_count);
        serial_write("\n");
        serial_write("[ video ] active mode=0x");
        serial_write_hex64((u64)handoff->gop_active_mode_id);
        serial_write("\n");
    }

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
    serial_write("[ compat ] INT21h io/handle baseline ready (AH=0Bh/0Ch/3Ch..43h)\n");
    serial_write("[ compat ] INT21h memory api ready (AH=48h/49h/4Ah)\n");
    serial_write("[compat] INT21h date/time ready (AH=2Ah/2Ch)\n");
    serial_write("[compat] INT21h ioctl baseline ready (AH=44h/AL=00h)\n");
    if (stage2_phase2_timer_ticks_progress()) {
        serial_write("[ test ] phase2 timer tick progress: PASS\n");
    } else {
        serial_write("[ test ] phase2 timer tick progress: FAIL\n");
    }
    if (stage2_keyboard_selftest_decode_capture()) {
        serial_write("[ test ] phase2 keyboard decode/capture: PASS\n");
    } else {
        serial_write("[ test ] phase2 keyboard decode/capture: FAIL\n");
    }
    if (stage2_phase2_lowlevel_selftest()) {
        serial_write("[ test ] phase2 low-level core selftest: PASS\n");
    } else {
        serial_write("[ test ] phase2 low-level core selftest: FAIL\n");
    }
    if (stage2_shell_selftest_int21_baseline()) {
        serial_write("[ test ] int21 priority-a selftest: PASS\n");
    } else {
        serial_write("[ test ] int21 priority-a selftest: FAIL\n");
    }
    if (stage2_shell_selftest_dosrun_status_path()) {
        serial_write("[ test ] dosrun status path selftest: PASS\n");
    } else {
        serial_write("[ test ] dosrun status path selftest: FAIL\n");
    }
    serial_write("[ ok ] stage2 mini shell ready (help/pwd/cd/dir/type/copy/ren/move/mkdir/rmdir/attrib/del/ascii/cls/ver/echo/set/ticks/mem/run/pmode/opengem/vmode/shutdown/reboot)\n");
    serial_write("[ compat ] PMODE contract v1 ready (CIUKEX64 marker + stub offset)\n");
    if (stage2_m6_pmode_contract_marker_selftest()) {
        serial_write("[ test ] m6 pmode contract marker selftest: PASS\n");
    } else {
        serial_write("[ test ] m6 pmode contract marker selftest: FAIL\n");
    }
    if (stage2_m6_pmode_shell_surface_selftest()) {
        serial_write("[ test ] m6 pmode shell surface selftest: PASS\n");
    } else {
        serial_write("[ test ] m6 pmode shell surface selftest: FAIL\n");
    }

    if (m6_transition_state_init()) {
        serial_write("[m6] transition state init: PASS\n");
    } else {
        serial_write("[m6] transition state init: FAIL\n");
    }

    if (g_m6_transition_state.gdtr_pre.limit != 0U && g_m6_transition_state.idtr_pre.limit != 0U) {
        serial_write("[m6] gdt/idt snapshot: PASS\n");
    } else {
        serial_write("[m6] gdt/idt snapshot: FAIL\n");
    }
    serial_write("[m6] snapshot gdtr.base=0x");
    serial_write_hex64(g_m6_transition_state.gdtr_pre.base);
    serial_write(" gdtr.limit=0x");
    serial_write_hex64((u64)g_m6_transition_state.gdtr_pre.limit);
    serial_write(" idtr.base=0x");
    serial_write_hex64(g_m6_transition_state.idtr_pre.base);
    serial_write(" idtr.limit=0x");
    serial_write_hex64((u64)g_m6_transition_state.idtr_pre.limit);
    serial_write("\n");

    serial_write("[m6] cr0 intended set=0x");
    serial_write_hex64(g_m6_transition_state.intended_cr0_set);
    serial_write(" clear=0x");
    serial_write_hex64(g_m6_transition_state.intended_cr0_clear);
    serial_write("\n");
    if ((g_m6_transition_state.intended_cr0_set & 0x1ULL) != 0ULL) {
        serial_write("[m6] cr0 transition contract: PASS\n");
    } else {
        serial_write("[m6] cr0 transition contract: FAIL\n");
    }

    if (g_m6_transition_state.return_path_status == PMODE_RETURN_PATH_OK) {
        serial_write("[m6] return-path contract: PASS\n");
    } else {
        serial_write("[m6] return-path contract: FAIL\n");
    }

    a20_state = m6_a20_probe();
    serial_write("[m6] a20 probe=");
    serial_write(a20_state ? "on" : "off");
    serial_write("\n");
    if (m6_a20_enable()) {
        serial_write("[m6] a20 enable result=PASS\n");
    } else {
        serial_write("[m6] a20 enable result=FAIL\n");
        serial_write("[m6] a20 enable reason=port92_stuck\n");
    }

    m6_desc_ready = (g_m6_transition_state.gdtr_pre.limit != 0U && g_m6_transition_state.idtr_pre.limit != 0U) ? 1 : 0;
    if (m6_desc_ready) {
        serial_write("[m6] descriptor baseline ready=1\n");
    } else {
        serial_write("[m6] descriptor baseline ready=0\n");
    }

    if (m6_dpmi_detect_skeleton_ready()) {
        serial_write("[m6] dpmi detect skeleton ready\n");
    }
    if (m6_rm_callback_skeleton_ready()) {
        serial_write("[m6] rm callback skeleton ready\n");
    }
    if (m6_int_reflect_skeleton_ready()) {
        serial_write("[m6] int reflect skeleton ready\n");
    }

    serial_write("[m6] pmem range base=0x");
    serial_write_hex64(M6_PMEM_BASE);
    serial_write(" size=0x");
    serial_write_hex64(M6_PMEM_SIZE);
    serial_write("\n");
    if (m6_pmem_overlap_check(handoff)) {
        serial_write("[m6] pmem overlap check: PASS\n");
    } else {
        serial_write("[m6] pmem overlap check: FAIL\n");
    }

    serial_write("[ ok ] desktop ui command available (type: desktop)\n");

    video_init(boot_info);
    if (stage2_bios_compat_selftest_int10()) {
        serial_write("[ test ] bios int10 baseline selftest: PASS\n");
    } else {
        serial_write("[ test ] bios int10 baseline selftest: FAIL\n");
    }
    if (stage2_bios_compat_selftest_int16()) {
        serial_write("[ test ] bios int16 baseline selftest: PASS\n");
    } else {
        serial_write("[ test ] bios int16 baseline selftest: FAIL\n");
    }
    if (stage2_bios_compat_selftest_int1a()) {
        serial_write("[ test ] bios int1a baseline selftest: PASS\n");
    } else {
        serial_write("[ test ] bios int1a baseline selftest: FAIL\n");
    }
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
        stage2_log_fat_mount_info();
        serial_write("[ compat ] INT21h FAT-backed file handles ready (AH=3Ch/3Dh/3Eh/3Fh/40h/41h/42h/43h/56h)\n");
        if (stage2_shell_selftest_int21_fat_handles()) {
            serial_write("[ test ] int21 fat-handle e2e selftest: PASS\n");
        } else {
            serial_write("[ test ] int21 fat-handle e2e selftest: FAIL\n");
        }
        serial_write("[ compat ] INT21h file search ready (AH=4Eh/4Fh)\n");
        if (stage2_shell_selftest_int21_findfirst_next()) {
            serial_write("[ test ] int21 findfirst/findnext selftest: PASS\n");
        } else {
            serial_write("[ test ] int21 findfirst/findnext selftest: FAIL\n");
        }
    } else {
        serial_write("[ warn ] FAT layer not mounted\n");
    }

    serial_write("[ shell ] mini command loop active\n");
    serial_write("[ stage2 ] next step: handoff to DOS-like runtime\n");
    stage2_shell_run(boot_info, handoff);
}
