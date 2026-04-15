#include "splash.h"
#include "video.h"
#include "types.h"

#define SPLASH_MAX_LINES 1024U

extern unsigned char stage2_splash_ascii[];
extern unsigned int stage2_splash_ascii_len;

typedef struct splash_line {
    const char *ptr;
    u32 len;
} splash_line_t;

static splash_line_t g_lines[SPLASH_MAX_LINES];
static u32 g_line_count = 0;
static u32 g_max_line_len = 0;
static int g_index_ready = 0;

static void splash_index_lines(void) {
    const char *data = (const char *)stage2_splash_ascii;
    u32 len = (u32)stage2_splash_ascii_len;
    u32 line_start = 0;

    if (g_index_ready) {
        return;
    }

    g_line_count = 0;
    g_max_line_len = 0;

    for (u32 i = 0; i < len; i++) {
        if (data[i] != '\n') {
            continue;
        }

        if (g_line_count < SPLASH_MAX_LINES) {
            u32 line_len = i - line_start;
            while (line_len > 0 && data[line_start + line_len - 1] == '\r') {
                line_len--;
            }
            g_lines[g_line_count].ptr = data + line_start;
            g_lines[g_line_count].len = line_len;
            if (line_len > g_max_line_len) {
                g_max_line_len = line_len;
            }
            g_line_count++;
        }

        line_start = i + 1U;
    }

    if (line_start < len && g_line_count < SPLASH_MAX_LINES) {
        u32 line_len = len - line_start;
        while (line_len > 0 && data[line_start + line_len - 1] == '\r') {
            line_len--;
        }
        g_lines[g_line_count].ptr = data + line_start;
        g_lines[g_line_count].len = line_len;
        if (line_len > g_max_line_len) {
            g_max_line_len = line_len;
        }
        g_line_count++;
    }

    g_index_ready = 1;
}

unsigned int stage2_splash_source_cols(void) {
    splash_index_lines();
    return (unsigned int)g_max_line_len;
}

unsigned int stage2_splash_source_rows(void) {
    splash_index_lines();
    return (unsigned int)g_line_count;
}

void stage2_splash_show(void) {
    u32 dst_cols = video_columns();
    u32 dst_rows = video_text_rows();

    splash_index_lines();
    if (g_line_count == 0 || g_max_line_len == 0 || dst_cols == 0 || dst_rows == 0) {
        return;
    }

    if (dst_cols > 1U) {
        dst_cols -= 1U;
    }

    video_cls();

    for (u32 y = 0; y < dst_rows; y++) {
        u32 src_y = (y * g_line_count) / dst_rows;
        const splash_line_t *line = &g_lines[src_y];

        video_set_cursor(0, y);
        for (u32 x = 0; x < dst_cols; x++) {
            u32 src_x = (x * g_max_line_len) / dst_cols;
            char ch = ' ';

            if (src_x < line->len) {
                ch = line->ptr[src_x];
                if ((u8)ch < 0x20 || (u8)ch > 0x7EU) {
                    ch = ' ';
                }
            }

            video_putchar(ch);
        }
    }

    video_set_cursor(0, 0);
}
