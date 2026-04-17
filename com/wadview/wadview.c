#include "services.h"

#define WADVIEW_IO_BUF_CAP     64U
#define WADVIEW_PATCH_BUF_CAP  32768U

static unsigned char g_io_buf[WADVIEW_IO_BUF_CAP];
static unsigned char g_patch_buf[WADVIEW_PATCH_BUF_CAP];
static char g_wad_path[64] = "DOOM1.WAD";
static char g_lump_name[9] = "M_DOOM";

static void regs_zero(ciuki_int21_regs_t *regs) {
    regs->ax = 0U;
    regs->bx = 0U;
    regs->cx = 0U;
    regs->dx = 0U;
    regs->si = 0U;
    regs->di = 0U;
    regs->ds = 0U;
    regs->es = 0U;
    regs->carry = 0U;
    regs->reserved[0] = 0U;
    regs->reserved[1] = 0U;
    regs->reserved[2] = 0U;
}

static unsigned int str_len(const char *s) {
    unsigned int n = 0U;
    while (s && s[n] != '\0') n++;
    return n;
}

static void mem_copy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n-- > 0U) *d++ = *s++;
}

static void mem_zero(void *dst, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    while (n-- > 0U) *d++ = 0U;
}

static int ptr_off(ciuki_dos_context_t *ctx, const void *ptr, unsigned short *off_out) {
    unsigned long long base;
    unsigned long long p;
    unsigned long long off;

    if (!ctx || !ptr || !off_out) return 0;
    base = ctx->image_linear;
    p = (unsigned long long)(const void *)ptr;
    if (p < base) return 0;
    off = p - base;
    if (off > 0xFFFFULL) return 0;
    *off_out = (unsigned short)off;
    return 1;
}

static int int21(ciuki_dos_context_t *ctx, ciuki_services_t *svc, ciuki_int21_regs_t *regs) {
    if (!ctx || !svc || !svc->int21 || !regs) return 0;
    svc->int21(ctx, regs);
    return 1;
}

static void print_line(ciuki_services_t *svc, const char *s) {
    if (svc && svc->print) svc->print(s);
}

static unsigned int read_le32(const unsigned char *p) {
    return (unsigned int)p[0]
         | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16)
         | ((unsigned int)p[3] << 24);
}

static int is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static char upper_ascii(char ch) {
    if (ch >= 'a' && ch <= 'z') return (char)(ch - 'a' + 'A');
    return ch;
}

static void parse_command_tail(ciuki_dos_context_t *ctx) {
    unsigned int i = 0U;
    unsigned int out = 0U;
    unsigned int token = 0U;

    if (!ctx) return;

    while (i < (unsigned int)ctx->command_tail_len) {
        while (i < (unsigned int)ctx->command_tail_len && is_space(ctx->command_tail[i])) i++;
        if (i >= (unsigned int)ctx->command_tail_len) break;
        out = 0U;
        if (token == 0U) {
            while (i < (unsigned int)ctx->command_tail_len && !is_space(ctx->command_tail[i])) {
                if (out + 1U < sizeof(g_wad_path)) g_wad_path[out++] = ctx->command_tail[i];
                i++;
            }
            g_wad_path[out] = '\0';
        } else if (token == 1U) {
            mem_zero(g_lump_name, (unsigned int)sizeof(g_lump_name));
            while (i < (unsigned int)ctx->command_tail_len && !is_space(ctx->command_tail[i]) && out < 8U) {
                g_lump_name[out++] = upper_ascii(ctx->command_tail[i]);
                i++;
            }
            g_lump_name[out] = '\0';
            break;
        } else {
            break;
        }
        token++;
    }
}

static int open_readonly(ciuki_dos_context_t *ctx, ciuki_services_t *svc, const char *path, unsigned short *h_out) {
    ciuki_int21_regs_t regs;
    unsigned short off;
    if (!ptr_off(ctx, path, &off)) return 0;
    regs_zero(&regs);
    regs.ax = 0x3D00U;
    regs.dx = off;
    if (!int21(ctx, svc, &regs)) return 0;
    if (regs.carry != 0U) return 0;
    *h_out = regs.ax;
    return 1;
}

static int close_handle(ciuki_dos_context_t *ctx, ciuki_services_t *svc, unsigned short h) {
    ciuki_int21_regs_t regs;
    regs_zero(&regs);
    regs.ax = 0x3E00U;
    regs.bx = h;
    if (!int21(ctx, svc, &regs)) return 0;
    return regs.carry == 0U ? 1 : 0;
}

static int read_handle(ciuki_dos_context_t *ctx, ciuki_services_t *svc,
                       unsigned short h, unsigned char *buf,
                       unsigned short len, unsigned short *got_out) {
    ciuki_int21_regs_t regs;
    unsigned short off;
    if (!ptr_off(ctx, buf, &off)) return 0;
    regs_zero(&regs);
    regs.ax = 0x3F00U;
    regs.bx = h;
    regs.cx = len;
    regs.dx = off;
    if (!int21(ctx, svc, &regs)) return 0;
    if (regs.carry != 0U) return 0;
    *got_out = regs.ax;
    return 1;
}

static int seek_abs(ciuki_dos_context_t *ctx, ciuki_services_t *svc,
                    unsigned short h, unsigned int pos) {
    ciuki_int21_regs_t regs;
    regs_zero(&regs);
    regs.ax = 0x4200U;
    regs.bx = h;
    regs.cx = (unsigned short)((pos >> 16) & 0xFFFFU);
    regs.dx = (unsigned short)(pos & 0xFFFFU);
    if (!int21(ctx, svc, &regs)) return 0;
    if (regs.carry != 0U) return 0;
    return (((unsigned int)regs.dx << 16) | regs.ax) == pos ? 1 : 0;
}

static int read_exact(ciuki_dos_context_t *ctx, ciuki_services_t *svc,
                      unsigned short h, unsigned char *buf,
                      unsigned int len) {
    unsigned int done = 0U;
    while (done < len) {
        unsigned short got = 0U;
        unsigned short want = (unsigned short)((len - done) > 0xFFF0U ? 0xFFF0U : (len - done));
        if (!read_handle(ctx, svc, h, buf + done, want, &got)) return 0;
        if (got == 0U) return 0;
        done += (unsigned int)got;
    }
    return 1;
}

static int lump_name_eq(const unsigned char *name8, const char *query) {
    unsigned int i;
    for (i = 0U; i < 8U; i++) {
        char q = query[i];
        char n = (char)name8[i];
        if (q == '\0') q = '\0';
        if (n == '\0') n = '\0';
        if (q == '\0') {
            if (n != '\0' && n != ' ') return 0;
        } else if (upper_ascii(n) != upper_ascii(q)) {
            return 0;
        }
    }
    return 1;
}

static int find_lump(ciuki_dos_context_t *ctx, ciuki_services_t *svc,
                     unsigned short h, unsigned int dir_ofs,
                     unsigned int lump_count,
                     unsigned int *filepos_out,
                     unsigned int *size_out) {
    unsigned int i;
    unsigned char ent[16];
    for (i = 0U; i < lump_count; i++) {
        if (!seek_abs(ctx, svc, h, dir_ofs + i * 16U)) return 0;
        if (!read_exact(ctx, svc, h, ent, 16U)) return 0;
        if (lump_name_eq(ent + 8U, g_lump_name)) {
            *filepos_out = read_le32(ent + 0U);
            *size_out = read_le32(ent + 4U);
            return 1;
        }
    }
    return 0;
}

static void u32_to_dec(unsigned int v, char *out) {
    char buf[16];
    int n = 0;
    int j = 0;
    if (v == 0U) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v > 0U && n < 15) {
        buf[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (n > 0) out[j++] = buf[--n];
    out[j] = '\0';
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    const ciuki_gfx_services_t *gfx = svc->gfx;
    ciuki_int21_regs_t regs;
    unsigned short h = 0U;
    unsigned int lump_ofs = 0U;
    unsigned int lump_size = 0U;
    unsigned int lump_count = 0U;
    unsigned int dir_ofs = 0U;
    char dec[16];
    int opened = 0;

    if (!gfx || !gfx->set_mode || !gfx->mode13_fill ||
        !gfx->mode13_draw_doom_patch || !gfx->present) {
        print_line(svc, "[wadview] FAIL: gfx services incomplete\n");
        goto exit;
    }

    parse_command_tail(ctx);

    if (!open_readonly(ctx, svc, g_wad_path, &h)) {
        if (str_len(g_wad_path) == 9U && g_wad_path[4] == '1') {
            mem_copy(g_wad_path, "DOOM.WAD", 9U);
            if (!open_readonly(ctx, svc, g_wad_path, &h)) {
                print_line(svc, "[wadview] FAIL: open WAD\n");
                goto exit;
            }
        } else {
            print_line(svc, "[wadview] FAIL: open WAD\n");
            goto exit;
        }
    }
    opened = 1;

    if (!read_exact(ctx, svc, h, g_io_buf, 12U)) {
        print_line(svc, "[wadview] FAIL: read header\n");
        goto exit;
    }
    if (!((g_io_buf[0] == 'I' && g_io_buf[1] == 'W' && g_io_buf[2] == 'A' && g_io_buf[3] == 'D') ||
          (g_io_buf[0] == 'P' && g_io_buf[1] == 'W' && g_io_buf[2] == 'A' && g_io_buf[3] == 'D'))) {
        print_line(svc, "[wadview] FAIL: bad WAD sig\n");
        goto exit;
    }
    lump_count = read_le32(g_io_buf + 4U);
    dir_ofs = read_le32(g_io_buf + 8U);

    if (!find_lump(ctx, svc, h, dir_ofs, lump_count, &lump_ofs, &lump_size)) {
        print_line(svc, "[wadview] FAIL: lump not found\n");
        goto exit;
    }
    if (lump_size == 0U || lump_size > WADVIEW_PATCH_BUF_CAP) {
        print_line(svc, "[wadview] FAIL: lump too large\n");
        goto exit;
    }
    if (!seek_abs(ctx, svc, h, lump_ofs) || !read_exact(ctx, svc, h, g_patch_buf, lump_size)) {
        print_line(svc, "[wadview] FAIL: read lump\n");
        goto exit;
    }

    if (!gfx->set_mode(0x13U)) {
        print_line(svc, "[wadview] FAIL: set_mode 0x13\n");
        goto exit;
    }

    gfx->mode13_fill(0U);
    gfx->mode13_draw_doom_patch(g_patch_buf, lump_size, 160, 100);
    gfx->mode13_draw_doom_patch(g_patch_buf, lump_size, 24, 24);
    gfx->present();

    print_line(svc, "[wadview] wad=");
    print_line(svc, g_wad_path);
    print_line(svc, " lump=");
    print_line(svc, g_lump_name);
    print_line(svc, " size=");
    u32_to_dec(lump_size, dec);
    print_line(svc, dec);
    print_line(svc, "\n[wadview] OK\n");

exit:
    if (opened) (void)close_handle(ctx, svc, h);
    regs_zero(&regs);
    regs.ax = 0x4C00U;
    if (svc->int21) {
        svc->int21(ctx, &regs);
    } else if (svc->int21_4c) {
        svc->int21_4c(ctx, 0x00);
    } else {
        svc->terminate(ctx, 0x00);
    }
}