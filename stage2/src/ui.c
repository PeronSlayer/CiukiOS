#include "ui.h"
#include "video.h"
#include "serial.h"

static ui_scene_t current_scene = SCENE_BOOT_SPLASH;

static u32 local_strlen(const char *s) {
    u32 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* ===== Scene Management ===== */

ui_scene_t ui_get_scene(void) {
    return current_scene;
}

int ui_set_scene(ui_scene_t scene) {
    if (scene == current_scene) {
        return 0;
    }
    current_scene = scene;
    return 1;
}

static void ui_render_desktop_scene(void) {
    /* Desktop scene renderer */
    u32 fb_w, fb_h;
    u32 row;
    static int desktop_rendered = 0;

    if (!video_ready()) {
        return;
    }

    fb_w = video_width_px();
    fb_h = video_height_px();

    /* Draw desktop background - simple dark gradient/pattern */
    if (fb_w > 0 && fb_h > 0) {
        video_fill_rect(0U, 0U, fb_w, fb_h, 0x00101015U); /* very dark blue-gray */
    }

    /* Draw top bar */
    if (fb_h > 32U) {
        ui_draw_panel(0U, 0U, fb_w, 32U, 0x00404050U, 0x00202025U);

        /* Top bar text: "CiukiOS" centered */
        row = 1U;
        video_set_colors(0x0000FF00U, 0x00202025U); /* bright green on dark */
        ui_write_centered_row(row, "CiukiOS");
        video_set_colors(0x00C0C0C0U, 0x00000000U);
    }

    /* Draw bottom status bar */
    if (fb_h > 64U) {
        u32 status_y = fb_h - 24U;
        ui_draw_panel(0U, status_y, fb_w, 24U, 0x00404050U, 0x00202025U);

        /* Status text */
        row = ui_pixel_y_to_text_row(status_y + 2U);
        video_set_colors(0x00808080U, 0x00202025U); /* dim gray */
        video_set_cursor(2U, row);
        video_write("TAB: Focus | UP/DOWN or J/K | ENTER: Select | ESC: shell");
        video_set_colors(0x00C0C0C0U, 0x00000000U);
    }

    if (!desktop_rendered) {
        serial_write("[ ui ] desktop shell surface active\n");
        desktop_rendered = 1;
    }
}

int ui_render_scene(void) {
    if (!video_ready()) {
        return 0;
    }
    switch (current_scene) {
        case SCENE_BOOT_SPLASH:
            return 1;
        case SCENE_DESKTOP:
            ui_render_desktop_scene();
            return 1;
        default:
            return 0;
    }
}

int ui_enter_desktop_scene(void) {
    if (!ui_set_scene(SCENE_DESKTOP)) {
        return 0;
    }
    serial_write("[ ui ] scene=desktop\n");
    ui_render_scene();
    return 1;
}

void ui_write_centered_row(u32 row, const char *text) {
    u32 cols = video_columns();
    u32 len = local_strlen(text);
    u32 start_col = 0;

    if (cols > len) {
        start_col = (cols - len) / 2U;
    }

    video_set_cursor(start_col, row);
    video_write(text);
}

void ui_draw_progress_bar(
    u32 x_start,
    u32 y_start,
    u32 width,
    u32 height,
    u32 progress_percent,
    u32 border_color,
    u32 bg_color,
    u32 fill_color)
{
    u32 inner_w;
    u32 fill_w;

    if (!video_ready() || width == 0U || height == 0U) {
        return;
    }

    if (progress_percent > 100U) {
        progress_percent = 100U;
    }

    /* Draw border */
    video_fill_rect(x_start, y_start, width, height, border_color);

    /* Draw background */
    if (width > 2U && height > 2U) {
        inner_w = width - 2U;
        video_fill_rect(x_start + 1U, y_start + 1U, inner_w, height - 2U, bg_color);

        /* Draw fill */
        fill_w = (inner_w * progress_percent) / 100U;
        if (fill_w > 0U) {
            video_fill_rect(x_start + 1U, y_start + 1U, fill_w, height - 2U, fill_color);
        }
    }
}

void ui_draw_top_bar(const char *text, u32 bg_color, u32 fg_color) {
    u32 cols;
    u32 text_len;
    u32 start_col;

    if (!video_ready()) {
        return;
    }

    cols = video_columns();
    text_len = local_strlen(text);

    /* Fill entire row with background color (text mode) */
    video_set_colors(bg_color, fg_color);
    video_set_cursor(0, 0);
    for (u32 i = 0; i < cols; i++) {
        video_putchar(' ');
    }

    /* Write centered text */
    if (cols > text_len) {
        start_col = (cols - text_len) / 2U;
    } else {
        start_col = 0;
    }
    video_set_cursor(start_col, 0);
    video_write(text);

    /* Restore default colors */
    video_set_colors(0x00C0C0C0U, 0x00000000U);
}

void ui_draw_panel(
    u32 x_start,
    u32 y_start,
    u32 width,
    u32 height,
    u32 border_color,
    u32 bg_color)
{
    if (!video_ready() || width == 0U || height == 0U) {
        return;
    }

    /* Draw border frame */
    video_fill_rect(x_start, y_start, width, height, border_color);

    /* Fill interior */
    if (width > 2U && height > 2U) {
        video_fill_rect(x_start + 1U, y_start + 1U, width - 2U, height - 2U, bg_color);
    }
}

void ui_draw_separator_line(u32 y_pos, u32 color) {
    u32 fb_w;

    if (!video_ready()) {
        return;
    }

    fb_w = video_width_px();
    video_fill_rect(0U, y_pos, fb_w, 1U, color);
}

u32 ui_pixel_y_to_text_row(u32 y_pixel) {
    /* Assume 8-pixel character height in graphical mode */
    return y_pixel / 8U;
}

int ui_draw_boot_hud(
    const char *version_string,
    const char *mode_string,
    u32 progress_percent)
{
    u32 fb_w;
    u32 fb_h;
    u32 hud_h = 60U;
    u32 hud_margin = 12U;
    u32 text_row;
    u32 label_col;
    u32 i;
    u32 len;

    if (!video_ready()) {
        return 0;
    }

    fb_w = video_width_px();
    fb_h = video_height_px();

    if (fb_w < 320U || fb_h < 240U) {
        /* Screen too small for HUD */
        return 0;
    }

    /* Draw HUD panel in top-left corner */
    ui_draw_panel(
        hud_margin,
        hud_margin,
        fb_w - (hud_margin * 2U),
        hud_h,
        0x00505050U,    /* border gray */
        0x00101010U     /* bg dark */
    );

    /* Position text inside HUD panel */
    text_row = ui_pixel_y_to_text_row(hud_margin + 4U);
    label_col = 2U;

    /* Set colors for HUD text */
    video_set_colors(0x0000FF00U, 0x00101010U); /* bright green on dark */

    /* Line 1: CiukiOS Version */
    video_set_cursor(label_col, text_row);
    video_write("CiukiOS ");
    if (version_string) {
        video_write(version_string);
    }

    /* Line 2: Mode and Progress */
    text_row++;
    video_set_cursor(label_col, text_row);
    video_write("Mode: ");
    if (mode_string) {
        video_write(mode_string);
    }

    /* Line 3: Progress bar in text form */
    text_row++;
    video_set_cursor(label_col, text_row);
    video_write("Progress: ");

    if (progress_percent > 100U) {
        progress_percent = 100U;
    }

    /* Draw simple text progress indicator */
    len = (progress_percent / 5U); /* 0-20 characters */
    for (i = 0; i < 20U; i++) {
        if (i < len) {
            video_putchar('#');
        } else {
            video_putchar('-');
        }
    }

    /* Restore normal colors */
    video_set_colors(0x00C0C0C0U, 0x00000000U);

    return 1;
}

/* ===== Window Manager ===== */
#define UI_MAX_WINDOWS 3
static ui_window_t g_windows[UI_MAX_WINDOWS] = {
    {"System", 60, 60, 280, 120, 1},
    {"Shell", 60, 200, 280, 120, 0},
    {"Info", 360, 60, 200, 120, 0},
};
static int g_focused_idx = 0;
static int g_wm_initialized = 0;

void ui_cycle_window_focus(void) {
    g_windows[g_focused_idx].focused = 0;
    g_focused_idx = (g_focused_idx + 1) % UI_MAX_WINDOWS;
    g_windows[g_focused_idx].focused = 1;
    if (!g_wm_initialized) {
        serial_write("[ ui ] wm focus cycle ok\n");
        g_wm_initialized = 1;
    }
}

int ui_get_focused_window(void) {
    return g_focused_idx;
}

void ui_render_windows(void) {
    int i;
    u32 title_row, border_color, bg_color;
    if (!video_ready()) return;
    for (i = 0; i < UI_MAX_WINDOWS; i++) {
        ui_window_t *w = &g_windows[i];
        border_color = w->focused ? 0x0000FF00U : 0x00404050U;
        bg_color = w->focused ? 0x001a1a1aU : 0x00151515U;
        ui_draw_panel(w->x, w->y, w->w, w->h, border_color, bg_color);
        if (w->h > 16U) {
            title_row = ui_pixel_y_to_text_row(w->y + 2U);
            video_set_colors(border_color, bg_color);
            video_set_cursor(2U, title_row);
            video_write("[");
            video_write(w->title);
            video_write("]");
            video_set_colors(0x00C0C0C0U, 0x00000000U);
        }
    }
}

/* ===== Launcher ===== */
#define LAUNCHER_ITEMS 6
static const char *g_launcher_items[LAUNCHER_ITEMS] = {
    "DIR", "MEM", "CLS", "VER", "ASCII", "RUN INIT.COM"
};
static int g_launcher_focus = 0;
static int g_launcher_active = 0;

void ui_activate_launcher(void) {
    g_launcher_active = 1;
}

void ui_deactivate_launcher(void) {
    g_launcher_active = 0;
}

void ui_launcher_next(void) {
    g_launcher_focus = (g_launcher_focus + 1) % LAUNCHER_ITEMS;
}

void ui_launcher_prev(void) {
    if (g_launcher_focus == 0) {
        g_launcher_focus = LAUNCHER_ITEMS - 1;
    } else {
        g_launcher_focus--;
    }
}

const char *ui_get_launcher_item(void) {
    return (g_launcher_focus < LAUNCHER_ITEMS) ? g_launcher_items[g_launcher_focus] : "";
}

void ui_render_launcher(void) {
    int i;
    u32 launcher_x = 60U, launcher_y = 350U, item_height = 20U, item_y, item_row;
    if (!video_ready() || !g_launcher_active) return;
    ui_draw_panel(launcher_x, launcher_y, 300U, (LAUNCHER_ITEMS * item_height) + 10U, 0x00505050U, 0x00151515U);
    for (i = 0; i < LAUNCHER_ITEMS; i++) {
        item_y = launcher_y + 5U + (i * item_height);
        item_row = ui_pixel_y_to_text_row(item_y);
        video_set_colors(i == g_launcher_focus ? 0x0000FF00U : 0x00808080U, 0x00151515U);
        video_set_cursor(5U, item_row);
        video_write(i == g_launcher_focus ? "> " : "  ");
        video_write(g_launcher_items[i]);
        video_set_colors(0x00C0C0C0U, 0x00000000U);
    }
}
