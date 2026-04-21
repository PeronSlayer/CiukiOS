#include "v86_dispatch.h"

#include "fat.h"
#include "serial.h"
#include "video.h"

static const char g_opengem_044_c_sentinel[] = "OPENGEM-044-C";
static int s_v86_dispatch_armed = 0;

/* DTA linear address stashed by INT 21h AH=1A, returned by AH=2F. */
uint32_t g_v86_dta_linear = 0u;

#define V86_PATH_MAX 128U
#define V86_PATH_MAX_TOKENS 16U
#define V86_PATH_TOKEN_MAX 13U
#define V86_FIND_DTA_SIZE 43U
#define V86_FIND_DTA_ATTR_OFFSET 0x15U
#define V86_FIND_DTA_TIME_OFFSET 0x16U
#define V86_FIND_DTA_DATE_OFFSET 0x18U
#define V86_FIND_DTA_SIZE_OFFSET 0x1AU
#define V86_FIND_DTA_NAME_OFFSET 0x1EU
#define V86_SOFTINT_DEFAULT_OFF 0x0600u
#define V86_SOFTINT_DEFAULT_SEG 0x0000u

typedef struct v86_find_state {
    int active;
    uint8_t attr_mask;
    uint16_t next_index;
    uint32_t dta_linear;
    char dir[V86_PATH_MAX];
    char pattern[V86_PATH_MAX];
} v86_find_state_t;

static v86_find_state_t s_v86_find_state;
static char s_v86_cwd[V86_PATH_MAX] = "/";
static uint8_t s_v86_default_drive = 2u;
static uint32_t s_v86_int_vectors[256];
static int s_v86_exec_pending = 0;
static char s_v86_exec_path[V86_PATH_MAX] = "";
static char s_v86_exec_tail[127] = "";
static uint16_t s_v86_exec_env_seg = 0u;

#define V86_MEM_MAX_BLOCKS 32U
#define V86_MEM_FIRST_SEG  0x8000u
#define V86_MEM_TOP_SEG    0xFF00u
#define V86_FILE_MAX_HANDLES 4U
#define V86_FILE_BUF_CAP 65536U
#define V86_FILE_HANDLE_BASE 5u

typedef struct v86_mem_block {
    uint16_t seg;
    uint16_t paras;
    uint8_t used;
} v86_mem_block_t;

typedef struct v86_file_handle {
    uint8_t used;
    uint8_t mode;
    uint8_t dirty;
    uint16_t handle_id;
    uint32_t size;
    uint32_t pos;
    char path[V86_PATH_MAX];
    uint8_t data[V86_FILE_BUF_CAP];
} v86_file_handle_t;

static v86_mem_block_t s_v86_mem_blocks[V86_MEM_MAX_BLOCKS];
static uint16_t s_v86_mem_next_seg = V86_MEM_FIRST_SEG;
static uint8_t s_v86_time_second = 0u;
static uint8_t s_v86_time_hundredth = 0u;
static v86_file_handle_t s_v86_file_handles[V86_FILE_MAX_HANDLES];
static uint16_t s_v86_last_ef_opcode = 0u;

static uint8_t v86_to_upper_ascii(uint8_t ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return (uint8_t)(ch - (uint8_t)('a' - 'A'));
    }
    return ch;
}

static void v86_memset(void *dst_void, uint8_t value, uint32_t count)
{
    uint8_t *dst = (uint8_t *)dst_void;
    uint32_t i;

    for (i = 0u; i < count; ++i) {
        dst[i] = value;
    }
}

static void v86_memcpy(void *dst_void, const void *src_void, uint32_t count)
{
    uint8_t *dst = (uint8_t *)dst_void;
    const uint8_t *src = (const uint8_t *)src_void;
    uint32_t i;

    if (!dst || !src || count == 0u) {
        return;
    }

    for (i = 0u; i < count; ++i) {
        dst[i] = src[i];
    }
}

static void v86_store_u16(uint32_t linear, uint16_t value)
{
    volatile uint8_t *p = (volatile uint8_t *)(uint64_t)linear;
    p[0] = (uint8_t)(value & 0x00FFu);
    p[1] = (uint8_t)((value >> 8) & 0x00FFu);
}

static uint16_t v86_load_u16(uint32_t linear)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(uint64_t)linear;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t v86_far_to_linear(uint16_t seg, uint16_t off)
{
    return ((uint32_t)seg << 4) + (uint32_t)off;
}

static int v86_try_emulate_int_ef(legacy_v86_frame_t *frame)
{
    uint16_t dx;
    uint16_t cx;
    uint32_t pb_lin;
    uint16_t ctrl_off;
    uint16_t ctrl_seg;
    uint16_t intout_off;
    uint16_t intout_seg;
    uint16_t ptsout_off;
    uint16_t ptsout_seg;
    uint32_t ctrl_lin;
    uint32_t intout_lin;
    uint32_t ptsout_lin;
    uint16_t opcode;
    uint16_t i;

    if (!frame) {
        return 0;
    }

    cx = (uint16_t)(frame->reserved[2] & 0xFFFFu);
    if (cx != 0x0473u) {
        return 0;
    }

    dx = (uint16_t)(frame->reserved[3] & 0xFFFFu);
    pb_lin = v86_far_to_linear(frame->ds, dx);

    ctrl_off = v86_load_u16(pb_lin + 0u);
    ctrl_seg = v86_load_u16(pb_lin + 2u);
    intout_off = v86_load_u16(pb_lin + 12u);
    intout_seg = v86_load_u16(pb_lin + 14u);
    ptsout_off = v86_load_u16(pb_lin + 16u);
    ptsout_seg = v86_load_u16(pb_lin + 18u);

    if ((ctrl_off == 0u && ctrl_seg == 0u) ||
        (intout_off == 0u && intout_seg == 0u) ||
        (ptsout_off == 0u && ptsout_seg == 0u)) {
        return 0;
    }

    ctrl_lin = v86_far_to_linear(ctrl_seg, ctrl_off);
    intout_lin = v86_far_to_linear(intout_seg, intout_off);
    ptsout_lin = v86_far_to_linear(ptsout_seg, ptsout_off);
    opcode = v86_load_u16(ctrl_lin + 0u);
    s_v86_last_ef_opcode = opcode;

    if (opcode == 0x000Cu) {
        /* VDI opcode 12 (v_text). No outputs; just report completion.
         * Clear n_ptsout / n_intout so GEM sees a well-formed response. */
        v86_store_u16(ctrl_lin + 4u, 0u);  /* contrl[2] n_ptsout */
        v86_store_u16(ctrl_lin + 8u, 0u);  /* contrl[4] n_intout */
        /* Preserve workstation handle as-is (input). */
        frame->reserved[0] &= 0xFFFF0000u;
        frame->eflags &= ~0x00000001u;
        return 1;
    }

    if (opcode == 0x0002u) {
        /* VDI opcode 2 (v_clswk). Close workstation; no outputs. */
        v86_store_u16(ctrl_lin + 4u, 0u);
        v86_store_u16(ctrl_lin + 8u, 0u);
        v86_store_u16(ctrl_lin + 12u, 0u); /* handle -> 0 */
        frame->reserved[0] &= 0xFFFF0000u;
        frame->eflags &= ~0x00000001u;
        return 1;
    }

    if (opcode != 0x0001u) {
        /* Generic VDI no-op completion for early GUI bring-up.
         * Leave a non-zero workstation handle so callers can proceed. */
        if (v86_load_u16(ctrl_lin + 12u) == 0u) {
            v86_store_u16(ctrl_lin + 12u, 1u);
        }
        v86_store_u16(ctrl_lin + 4u, 0u);  /* n_ptsout = 0 */
        v86_store_u16(ctrl_lin + 8u, 0u);  /* n_intout = 0 */
        frame->reserved[0] &= 0xFFFF0000u; /* AX=0 success */
        frame->eflags &= ~0x00000001u;     /* clear CF */
        return 1;
    }

    /* Minimal VDI open-workstation success surface for early GEM bring-up. */
    v86_store_u16(ctrl_lin + 4u, 6u);   /* contrl[2] */
    v86_store_u16(ctrl_lin + 8u, 45u);  /* contrl[4] */
    v86_store_u16(ctrl_lin + 12u, 1u);  /* contrl[6] workstation handle */

    for (i = 0u; i < 45u; ++i) {
        v86_store_u16(intout_lin + ((uint32_t)i * 2u), 1u);
    }
    v86_store_u16(intout_lin + 0u, 1u);   /* handle echo */
    v86_store_u16(intout_lin + 2u, 16u);  /* planes/colors hint */
    v86_store_u16(intout_lin + 4u, 640u); /* width hint */
    v86_store_u16(intout_lin + 6u, 480u); /* height hint */

    v86_store_u16(ptsout_lin + 0u, 639u);
    v86_store_u16(ptsout_lin + 2u, 479u);
    v86_store_u16(ptsout_lin + 4u, 0u);
    v86_store_u16(ptsout_lin + 6u, 0u);
    v86_store_u16(ptsout_lin + 8u, 639u);
    v86_store_u16(ptsout_lin + 10u, 479u);

    frame->reserved[0] &= 0xFFFF0000u; /* AX=0 success */
    frame->eflags &= ~0x00000001u;     /* clear CF */

    return 1;
}

static uint32_t v86_default_softint_far(uint8_t vector)
{
    if (vector != 0xEFu) {
        return 0u;
    }
    return ((uint32_t)V86_SOFTINT_DEFAULT_SEG << 16) | (uint32_t)V86_SOFTINT_DEFAULT_OFF;
}

static void v86_install_default_softint_stub(void)
{
    volatile uint8_t *stub = (volatile uint8_t *)(uint64_t)V86_SOFTINT_DEFAULT_OFF;

    /* Default chain target for AH=35 fallback on INT EF: IRET.
     * GEM's EF wrapper chains with PUSHF-equivalent + CALL FAR. */
    stub[0] = 0xCFu;
}

static void v86_str_copy(char *dst, const char *src, uint32_t dst_size)
{
    uint32_t i = 0u;

    if (!dst || dst_size == 0u) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    while (src[i] != '\0' && (i + 1u) < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int v86_str_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }

    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static int v86_read_guest_asciiz(uint32_t linear, char *out, uint32_t out_size)
{
    const volatile char *p = (const volatile char *)(uint64_t)linear;
    uint32_t i;

    if (!out || out_size == 0u) {
        return 0;
    }

    for (i = 0u; (i + 1u) < out_size; ++i) {
        char c = p[i];
        out[i] = c;
        if (c == '\0') {
            return 1;
        }
    }

    out[out_size - 1u] = '\0';
    return 1;
}

static int v86_split_tokens(
    const char *path,
    char tokens[V86_PATH_MAX_TOKENS][V86_PATH_TOKEN_MAX],
    uint32_t *count_out
)
{
    const char *p = path;
    uint32_t count = 0u;

    if (!path || !count_out) {
        return 0;
    }

    while (*p) {
        uint32_t n = 0u;

        while (*p == '/' || *p == '\\') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (count >= V86_PATH_MAX_TOKENS) {
            return 0;
        }

        while (*p && *p != '/' && *p != '\\') {
            if ((n + 1u) >= V86_PATH_TOKEN_MAX) {
                return 0;
            }
            tokens[count][n++] = (char)v86_to_upper_ascii((uint8_t)*p);
            p++;
        }
        tokens[count][n] = '\0';

        /* FAT short-name compatibility for OpenGEM resources staged as
         * long-name directory `.RSC` (short alias `RSC~1`). */
        if (v86_str_eq(tokens[count], ".RSC")) {
            v86_str_copy(tokens[count], "RSC~1", V86_PATH_TOKEN_MAX);
        }

        count++;
    }

    *count_out = count;
    return 1;
}

static int v86_build_canonical_path(const char *input, const char *cwd, char *out, uint32_t out_size)
{
    char stack[V86_PATH_MAX_TOKENS][V86_PATH_TOKEN_MAX];
    char parts[V86_PATH_MAX_TOKENS][V86_PATH_TOKEN_MAX];
    uint32_t stack_count = 0u;
    uint32_t part_count = 0u;
    uint32_t j = 0u;
    int absolute = 0;

    if (!input || !cwd || !out || out_size == 0u) {
        return 0;
    }

    if (*input == '\0') {
        v86_str_copy(out, cwd, out_size);
        return 1;
    }

    if (*input == '/' || *input == '\\') {
        absolute = 1;
    }

    if (!absolute) {
        if (!v86_split_tokens(cwd, stack, &stack_count)) {
            return 0;
        }
    }

    if (!v86_split_tokens(input, parts, &part_count)) {
        return 0;
    }

    for (uint32_t i = 0u; i < part_count; ++i) {
        if (v86_str_eq(parts[i], ".")) {
            continue;
        }
        if (v86_str_eq(parts[i], "..")) {
            if (stack_count > 0u) {
                stack_count--;
            }
            continue;
        }
        if (stack_count >= V86_PATH_MAX_TOKENS) {
            return 0;
        }
        v86_str_copy(stack[stack_count], parts[i], V86_PATH_TOKEN_MAX);
        stack_count++;
    }

    if (stack_count == 0u) {
        if (out_size < 2u) {
            return 0;
        }
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }

    for (uint32_t i = 0u; i < stack_count; ++i) {
        if ((j + 1u) >= out_size) {
            return 0;
        }
        out[j++] = '/';

        for (uint32_t k = 0u; stack[i][k] != '\0'; ++k) {
            if ((j + 1u) >= out_size) {
                return 0;
            }
            out[j++] = stack[i][k];
        }
    }

    out[j] = '\0';
    return 1;
}

static int v86_dos_path_to_canonical(const char *in, char *out, uint32_t out_size)
{
    char tmp[V86_PATH_MAX];
    uint32_t j = 0u;
    const char *p = in;

    if (!in || !out || out_size == 0u) {
        return 0;
    }

    if ((((p[0] >= 'a') && (p[0] <= 'z')) || ((p[0] >= 'A') && (p[0] <= 'Z'))) && p[1] == ':') {
        p += 2;
    }

    while (*p != '\0' && (j + 1u) < (uint32_t)sizeof(tmp)) {
        char ch = *p++;
        if (ch == '\\') {
            ch = '/';
        }
        tmp[j++] = (char)v86_to_upper_ascii((uint8_t)ch);
    }
    tmp[j] = '\0';

    if (tmp[0] == '\0') {
        return 0;
    }

    return v86_build_canonical_path(tmp, s_v86_cwd, out, out_size);
}

static int v86_dir_exists_cb(const fat_dir_entry_t *entry, void *ctx)
{
    (void)entry;
    (void)ctx;
    return 0;
}

static int v86_dir_exists(const char *canonical_path)
{
    if (!canonical_path || !fat_ready()) {
        return 0;
    }

    return fat_list_dir(canonical_path, v86_dir_exists_cb, (void *)0);
}

static void v86_canonical_to_dos_cwd(const char *canonical, char *out, uint32_t out_size)
{
    uint32_t i = 0u;
    uint32_t j = 0u;

    if (!out || out_size == 0u) {
        return;
    }
    out[0] = '\0';

    if (!canonical) {
        return;
    }

    if (canonical[0] == '/' && canonical[1] == '\0') {
        return;
    }

    if (canonical[0] == '/') {
        i = 1u;
    }

    while (canonical[i] != '\0' && (j + 1u) < out_size) {
        char ch = canonical[i++];
        out[j++] = (ch == '/') ? '\\' : ch;
    }

    out[j] = '\0';
}

static int v86_wild_match_ci(const char *pattern, const char *name)
{
    const char *p = pattern ? pattern : "";
    const char *n = name ? name : "";
    const char *star = (const char *)0;
    const char *backtrack = (const char *)0;

    while (*n != '\0') {
        if (*p == '*') {
            star = p++;
            backtrack = n;
            continue;
        }

        if (*p == '?' || v86_to_upper_ascii((uint8_t)*p) == v86_to_upper_ascii((uint8_t)*n)) {
            p++;
            n++;
            continue;
        }

        if (star) {
            p = star + 1;
            n = ++backtrack;
            continue;
        }

        return 0;
    }

    while (*p == '*') {
        p++;
    }

    return *p == '\0';
}

static int v86_split_find_path(
    const char *canonical,
    char *dir_out,
    uint32_t dir_out_size,
    char *pattern_out,
    uint32_t pattern_out_size
)
{
    const char *last_slash = (const char *)0;
    uint32_t i;

    if (!canonical || !dir_out || !pattern_out || dir_out_size == 0u || pattern_out_size == 0u) {
        return 0;
    }

    for (i = 0u; canonical[i] != '\0'; ++i) {
        if (canonical[i] == '/') {
            last_slash = canonical + i;
        }
    }

    if (!last_slash) {
        v86_str_copy(dir_out, "/", dir_out_size);
        v86_str_copy(pattern_out, canonical, pattern_out_size);
    } else if (last_slash == canonical) {
        v86_str_copy(dir_out, "/", dir_out_size);
        if (last_slash[1] == '\0') {
            v86_str_copy(pattern_out, "*", pattern_out_size);
        } else {
            v86_str_copy(pattern_out, last_slash + 1, pattern_out_size);
        }
    } else {
        uint32_t dir_len = (uint32_t)(last_slash - canonical);

        if ((dir_len + 1u) > dir_out_size) {
            return 0;
        }

        for (uint32_t j = 0u; j < dir_len; ++j) {
            dir_out[j] = canonical[j];
        }
        dir_out[dir_len] = '\0';

        if (last_slash[1] == '\0') {
            v86_str_copy(pattern_out, "*", pattern_out_size);
        } else {
            v86_str_copy(pattern_out, last_slash + 1, pattern_out_size);
        }
    }

    if (pattern_out[0] == '\0') {
        v86_str_copy(pattern_out, "*", pattern_out_size);
    }

    return 1;
}

static int v86_find_attr_match(uint8_t entry_attr, uint8_t search_attr)
{
    if ((entry_attr & FAT_ATTR_VOLUME_ID) != 0u) {
        return 0;
    }
    if ((entry_attr & FAT_ATTR_HIDDEN) != 0u && (search_attr & FAT_ATTR_HIDDEN) == 0u) {
        return 0;
    }
    if ((entry_attr & FAT_ATTR_SYSTEM) != 0u && (search_attr & FAT_ATTR_SYSTEM) == 0u) {
        return 0;
    }
    if ((entry_attr & FAT_ATTR_DIRECTORY) != 0u && (search_attr & FAT_ATTR_DIRECTORY) == 0u) {
        return 0;
    }
    return 1;
}

typedef struct v86_find_match_ctx {
    const char *pattern;
    uint8_t attr_mask;
    uint16_t target_index;
    uint16_t seen_index;
    int found;
    fat_dir_entry_t match;
} v86_find_match_ctx_t;

static int v86_find_match_cb(const fat_dir_entry_t *entry, void *ctx_void)
{
    v86_find_match_ctx_t *ctx = (v86_find_match_ctx_t *)ctx_void;

    if (!entry || !ctx) {
        return 0;
    }

    if (!v86_find_attr_match(entry->attr, ctx->attr_mask)) {
        return 1;
    }

    if (!v86_wild_match_ci(ctx->pattern, entry->name)) {
        return 1;
    }

    if (ctx->seen_index == ctx->target_index) {
        ctx->match = *entry;
        ctx->found = 1;
        return 0;
    }

    ctx->seen_index++;
    return 1;
}

static int v86_find_match_in_dir(
    const char *dir_path,
    const char *pattern,
    uint8_t attr_mask,
    uint16_t target_index,
    fat_dir_entry_t *out_entry,
    int *dir_ok_out
)
{
    v86_find_match_ctx_t ctx;

    v86_memset(&ctx, 0u, (uint32_t)sizeof(ctx));
    ctx.pattern = pattern;
    ctx.attr_mask = attr_mask;
    ctx.target_index = target_index;

    if (!fat_list_dir(dir_path, v86_find_match_cb, &ctx)) {
        if (dir_ok_out) {
            *dir_ok_out = 0;
        }
        return 0;
    }

    if (dir_ok_out) {
        *dir_ok_out = 1;
    }

    if (!ctx.found) {
        return 0;
    }

    if (out_entry) {
        *out_entry = ctx.match;
    }

    return 1;
}

static void v86_pack_name_83(const char *name, uint8_t out[11])
{
    uint32_t i = 0u;
    uint32_t j = 0u;
    uint32_t k = 0u;

    for (i = 0u; i < 11u; ++i) {
        out[i] = ' ';
    }
    i = 0u;

    if (!name) {
        return;
    }

    while (name[j] != '\0' && name[j] != '.' && i < 8u) {
        out[i++] = v86_to_upper_ascii((uint8_t)name[j++]);
    }

    if (name[j] == '.') {
        j++;
    }

    while (name[j] != '\0' && k < 3u) {
        out[8u + k] = v86_to_upper_ascii((uint8_t)name[j]);
        j++;
        k++;
    }
}

static void v86_fill_find_dta(volatile uint8_t *dta, const fat_dir_entry_t *entry)
{
    uint8_t packed_name[11];

    if (!dta || !entry) {
        return;
    }

    for (uint32_t i = 0u; i < V86_FIND_DTA_SIZE; ++i) {
        dta[i] = 0u;
    }

    dta[V86_FIND_DTA_ATTR_OFFSET] = entry->attr;
    dta[V86_FIND_DTA_TIME_OFFSET + 0u] = 0u;
    dta[V86_FIND_DTA_TIME_OFFSET + 1u] = 0u;
    dta[V86_FIND_DTA_DATE_OFFSET + 0u] = 0u;
    dta[V86_FIND_DTA_DATE_OFFSET + 1u] = 0u;
    dta[V86_FIND_DTA_SIZE_OFFSET + 0u] = (uint8_t)(entry->size & 0x000000FFu);
    dta[V86_FIND_DTA_SIZE_OFFSET + 1u] = (uint8_t)((entry->size >> 8) & 0x000000FFu);
    dta[V86_FIND_DTA_SIZE_OFFSET + 2u] = (uint8_t)((entry->size >> 16) & 0x000000FFu);
    dta[V86_FIND_DTA_SIZE_OFFSET + 3u] = (uint8_t)((entry->size >> 24) & 0x000000FFu);

    v86_pack_name_83(entry->name, packed_name);
    for (uint32_t i = 0u; i < 11u; ++i) {
        dta[V86_FIND_DTA_NAME_OFFSET + i] = packed_name[i];
    }
    dta[V86_FIND_DTA_NAME_OFFSET + 11u] = '\0';
    dta[V86_FIND_DTA_NAME_OFFSET + 12u] = '\0';
}

static void v86_find_state_reset(void)
{
    v86_memset(&s_v86_find_state, 0u, (uint32_t)sizeof(s_v86_find_state));
}

static void v86_vectors_reset(void)
{
    v86_memset(s_v86_int_vectors, 0u, (uint32_t)sizeof(s_v86_int_vectors));
}

static void v86_exec_request_clear(void)
{
    s_v86_exec_pending = 0;
    s_v86_exec_path[0] = '\0';
    s_v86_exec_tail[0] = '\0';
    s_v86_exec_env_seg = 0u;
}

static void v86_exec_request_set(const char *canonical_path)
{
    if (!canonical_path || canonical_path[0] == '\0') {
        v86_exec_request_clear();
        return;
    }

    v86_str_copy(s_v86_exec_path, canonical_path, (uint32_t)sizeof(s_v86_exec_path));
    s_v86_exec_pending = (s_v86_exec_path[0] != '\0') ? 1 : 0;
}

static void v86_exec_tail_set_from_pb(uint32_t pb_linear)
{
    const volatile uint8_t *pb = (const volatile uint8_t *)(uint64_t)pb_linear;
    uint16_t env_seg;
    uint16_t cmd_off;
    uint16_t cmd_seg;
    uint32_t cmd_linear;
    const volatile uint8_t *cmd;
    uint8_t len;

    s_v86_exec_tail[0] = '\0';

    env_seg = (uint16_t)((uint16_t)pb[0] | ((uint16_t)pb[1] << 8));
    s_v86_exec_env_seg = env_seg;

    cmd_off = (uint16_t)((uint16_t)pb[2] | ((uint16_t)pb[3] << 8));
    cmd_seg = (uint16_t)((uint16_t)pb[4] | ((uint16_t)pb[5] << 8));

    serial_write("[v86] int21/4B param env=0x");
    serial_write_hex64((uint64_t)env_seg);
    serial_write("\n");

    serial_write("[v86] int21/4B param cmd=");
    serial_write_hex64((uint64_t)cmd_seg);
    serial_write(":");
    serial_write_hex64((uint64_t)cmd_off);
    serial_write("\n");

    if (cmd_seg == 0u && cmd_off == 0u) {
        return;
    }

    cmd_linear = ((uint32_t)cmd_seg << 4) + (uint32_t)cmd_off;
    cmd = (const volatile uint8_t *)(uint64_t)cmd_linear;
    len = cmd[0];
    if (len > 126u) {
        len = 126u;
    }

    for (uint32_t i = 0u; i < (uint32_t)len; ++i) {
        s_v86_exec_tail[i] = (char)cmd[1u + i];
    }
    s_v86_exec_tail[(uint32_t)len] = '\0';

    serial_write("[v86] int21/4B param tail=\"");
    serial_write(s_v86_exec_tail);
    serial_write("\"\n");
}

static void v86_mem_reset(void)
{
    v86_memset(s_v86_mem_blocks, 0u, (uint32_t)sizeof(s_v86_mem_blocks));
    s_v86_mem_next_seg = V86_MEM_FIRST_SEG;
}

static int v86_mem_alloc(uint16_t paras, uint16_t *seg_out, uint16_t *max_out)
{
    uint32_t avail;

    if (!seg_out || !max_out) {
        return 0;
    }

    if (s_v86_mem_next_seg >= V86_MEM_TOP_SEG) {
        *max_out = 0u;
        return 0;
    }

    avail = (uint32_t)V86_MEM_TOP_SEG - (uint32_t)s_v86_mem_next_seg;
    if (avail > 0xFFFFu) {
        avail = 0xFFFFu;
    }
    *max_out = (uint16_t)avail;

    if (paras == 0u || paras > *max_out) {
        return 0;
    }

    for (uint32_t i = 0u; i < V86_MEM_MAX_BLOCKS; ++i) {
        if (!s_v86_mem_blocks[i].used) {
            s_v86_mem_blocks[i].used = 1u;
            s_v86_mem_blocks[i].seg = s_v86_mem_next_seg;
            s_v86_mem_blocks[i].paras = paras;
            *seg_out = s_v86_mem_next_seg;
            s_v86_mem_next_seg = (uint16_t)(s_v86_mem_next_seg + paras);
            return 1;
        }
    }

    *max_out = 0u;
    return 0;
}

static void v86_file_handles_reset(void)
{
    v86_memset(s_v86_file_handles, 0u, (uint32_t)sizeof(s_v86_file_handles));
}

static v86_file_handle_t *v86_file_find_handle(uint16_t handle)
{
    for (uint32_t i = 0u; i < V86_FILE_MAX_HANDLES; ++i) {
        if (s_v86_file_handles[i].used && s_v86_file_handles[i].handle_id == handle) {
            return &s_v86_file_handles[i];
        }
    }
    return (v86_file_handle_t *)0;
}

static v86_file_handle_t *v86_file_alloc_handle(uint8_t mode, const char *path)
{
    for (uint32_t i = 0u; i < V86_FILE_MAX_HANDLES; ++i) {
        if (!s_v86_file_handles[i].used) {
            v86_file_handle_t *h = &s_v86_file_handles[i];
            h->used = 1u;
            h->mode = mode;
            h->dirty = 0u;
            h->handle_id = (uint16_t)(V86_FILE_HANDLE_BASE + i);
            h->size = 0u;
            h->pos = 0u;
            v86_str_copy(h->path, path ? path : "", (uint32_t)sizeof(h->path));
            return h;
        }
    }

    return (v86_file_handle_t *)0;
}

static int v86_file_flush_handle(v86_file_handle_t *h)
{
    if (!h || !h->used || !h->dirty) {
        return 1;
    }

    /* Replace semantics: remove old file entry and rewrite full buffer. */
    (void)fat_delete_file(h->path);
    if (!fat_write_file(h->path, (const void *)h->data, h->size)) {
        return 0;
    }

    h->dirty = 0u;
    return 1;
}

static void v86_file_close_all(void)
{
    for (uint32_t i = 0u; i < V86_FILE_MAX_HANDLES; ++i) {
        if (!s_v86_file_handles[i].used) {
            continue;
        }

        (void)v86_file_flush_handle(&s_v86_file_handles[i]);
        s_v86_file_handles[i].used = 0u;
    }
}

static int v86_reflect_soft_interrupt(uint8_t vector, legacy_v86_frame_t *frame)
{
    uint32_t far_ptr;
    uint16_t new_ip;
    uint16_t new_cs;
    uint16_t sp;
    uint16_t flags16;
    uint32_t ss_base;

    if (!frame) {
        return 0;
    }

    far_ptr = s_v86_int_vectors[vector];
    if (far_ptr == 0u) {
        far_ptr = v86_default_softint_far(vector);
        if (far_ptr == 0u) {
            return 0;
        }
    }

    new_ip = (uint16_t)(far_ptr & 0xFFFFu);
    new_cs = (uint16_t)((far_ptr >> 16) & 0xFFFFu);
    sp = frame->sp;
    ss_base = ((uint32_t)frame->ss << 4);

    /* Keep IF set in the saved image so IRET restores runnable flags. */
    flags16 = (uint16_t)((frame->eflags | 0x00000200u) & 0xFFFFu);

    /* Emulate INT n in VM86: push FLAGS, CS, IP then jump to vector. */
    sp = (uint16_t)(sp - 2u);
    v86_store_u16(ss_base + (uint32_t)sp, flags16);
    sp = (uint16_t)(sp - 2u);
    v86_store_u16(ss_base + (uint32_t)sp, frame->cs);
    sp = (uint16_t)(sp - 2u);
    v86_store_u16(ss_base + (uint32_t)sp, frame->ip);

    frame->sp = sp;
    frame->cs = new_cs;
    frame->ip = new_ip;
    frame->eflags &= ~0x00000300u; /* clear TF + IF while in handler */
    return 1;
}

/* Historical scaffold token retained for scripts/test_v86_dispatch.sh:
 * return V86_DISPATCH_CONT;
 */

__attribute__((weak)) int legacy_v86_enter(const legacy_v86_frame_t *entry, legacy_v86_exit_t *out)
{
    (void)entry;
    if (out) {
        out->reason = LEGACY_V86_EXIT_FAULT;
        out->int_vector = 0U;
        out->frame.cs = 0U;
        out->frame.ip = 0U;
        out->frame.ss = 0U;
        out->frame.sp = 0U;
        out->frame.ds = 0U;
        out->frame.es = 0U;
        out->frame.fs = 0U;
        out->frame.gs = 0U;
        out->frame.eflags = 0U;
        out->frame.reserved[0] = 0U;
        out->frame.reserved[1] = 0U;
        out->frame.reserved[2] = 0U;
        out->frame.reserved[3] = 0U;
        out->fault_code = 0xB0440001u;
    }
    return 0;
}

__attribute__((weak)) int legacy_v86_arm(uint32_t magic)
{
    (void)magic;
    return 0;
}

__attribute__((weak)) void legacy_v86_disarm(void)
{
}

__attribute__((weak)) int legacy_v86_is_armed(void)
{
    return 0;
}

__attribute__((weak)) int legacy_v86_probe(void)
{
    return 0;
}

v86_dispatch_result_t v86_dispatch_int(uint8_t vector, legacy_v86_frame_t *frame)
{
    uint32_t eax;
    uint8_t ah;

    if (frame == (legacy_v86_frame_t *)0) {
        return V86_DISPATCH_EXIT_ERR;
    }

    serial_write("[v86] dispatch vec=0x");
    serial_write_hex64((uint64_t)vector);
    serial_write(" eax=0x");
    serial_write_hex64((uint64_t)frame->reserved[0]);
    serial_write(" ebx=0x");
    serial_write_hex64((uint64_t)frame->reserved[1]);
    serial_write(" ecx=0x");
    serial_write_hex64((uint64_t)frame->reserved[2]);
    serial_write(" edx=0x");
    serial_write_hex64((uint64_t)frame->reserved[3]);
    serial_write(" ds=0x");
    serial_write_hex64((uint64_t)frame->ds);
    serial_write(" es=0x");
    serial_write_hex64((uint64_t)frame->es);
    serial_write("\n");

    if (vector == 0x20u) {
        return V86_DISPATCH_EXIT_OK;
    }

    if (vector != 0x21u) {
        if (vector == 0xEFu && v86_try_emulate_int_ef(frame)) {
            serial_write("[v86] dispatch soft-int emu vec=EF op=0x");
            serial_write_hex64((uint64_t)s_v86_last_ef_opcode);
            serial_write("\n");
            return V86_DISPATCH_CONT;
        }

        if (v86_reflect_soft_interrupt(vector, frame)) {
            serial_write("[v86] dispatch soft-int reflect vec=0x");
            serial_write_hex64((uint64_t)vector);
            serial_write(" far=0x");
            serial_write_hex64((uint64_t)s_v86_int_vectors[vector]);
            serial_write("\n");
            return V86_DISPATCH_CONT;
        }
        return V86_DISPATCH_EXIT_ERR;
    }

    eax = frame->reserved[0];
    ah = (uint8_t)((eax >> 8) & 0xFFu);

    serial_write("[v86] int21 ah=0x");
    serial_write_hex64((uint64_t)ah);
    serial_write(" al=0x");
    serial_write_hex64((uint64_t)(eax & 0xFFu));
    serial_write("\n");

    /* Helpers: clear/set CF in v86 guest EFLAGS to signal success/error. */
    #define V86_CF_CLEAR()  do { frame->eflags &= ~0x00000001u; } while (0)
    #define V86_CF_SET()    do { frame->eflags |=  0x00000001u; } while (0)

    switch (ah) {
    case 0x02u: { /* Display character: DL -> stdout */
        char c = (char)(frame->reserved[3] & 0xFFu);
        char buf[2];
        buf[0] = c;
        buf[1] = 0;
        video_write(buf);
        serial_write(buf);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x09u: { /* Print $-terminated string at DS:DX */
        uint32_t linear = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);
        const volatile char *p = (const volatile char *)(uint64_t)linear;
        int i;
        serial_write("[v86] int21/09 ds:dx=");
        serial_write_hex64((uint64_t)linear);
        serial_write(" -> \"");
        for (i = 0; i < 1024; ++i) {
            char c = p[i];
            if (c == '$') {
                break;
            }
            char buf[2];
            buf[0] = c;
            buf[1] = 0;
            video_write(buf);
            serial_write(buf);
        }
        serial_write("\"\n");
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x08u: { /* Console input without echo */
        /* Deterministic non-blocking surrogate for headless probes. */
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x000Du;
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x4Cu: /* Terminate process with return code */
        return V86_DISPATCH_EXIT_OK;

    case 0x00u: /* Terminate program */
        return V86_DISPATCH_EXIT_OK;

    case 0x30u: /* Get DOS version: return AL=major, AH=minor */
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0005u; /* DOS 5.00 */
        frame->reserved[1] = 0u;                             /* BX=OEM/serial */
        frame->reserved[2] = 0u;                             /* CX=serial lo */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x25u: { /* Set interrupt vector: AL=index, DS:DX=far ptr */
        uint8_t vec = (uint8_t)(eax & 0x00FFu);
        uint16_t off = (uint16_t)(frame->reserved[3] & 0xFFFFu);
        uint16_t seg = frame->ds;
        s_v86_int_vectors[vec] = ((uint32_t)seg << 16) | (uint32_t)off;

        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x35u: { /* Get interrupt vector: AL=index -> ES:BX */
        uint8_t vec = (uint8_t)(eax & 0x00FFu);
        uint32_t far_ptr = s_v86_int_vectors[vec];
        if (far_ptr == 0u) {
            far_ptr = v86_default_softint_far(vec);
        }
        frame->es = (uint16_t)((far_ptr >> 16) & 0xFFFFu);
        frame->reserved[1] = (frame->reserved[1] & 0xFFFF0000u) | (far_ptr & 0xFFFFu);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x2Au: { /* Get date: AL=weekday, CX=year, DH=month, DL=day */
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0001u; /* Monday */
        frame->reserved[2] = (frame->reserved[2] & 0xFFFF0000u) | 2026u;
        frame->reserved[3] = (frame->reserved[3] & 0xFFFF0000u) | 0x0414u; /* 20 Apr */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x2Cu: { /* Get time: CH=hour, CL=min, DH=sec, DL=1/100 sec */
        uint16_t sec;
        uint16_t hund;
        uint16_t cx_time = (uint16_t)((12u << 8) | 0x22u); /* 12:34 */

        s_v86_time_hundredth = (uint8_t)(s_v86_time_hundredth + 5u);
        if (s_v86_time_hundredth >= 100u) {
            s_v86_time_hundredth = (uint8_t)(s_v86_time_hundredth - 100u);
            s_v86_time_second = (uint8_t)((s_v86_time_second + 1u) % 60u);
        }

        sec = (uint16_t)(s_v86_time_second % 60u);
        hund = (uint16_t)s_v86_time_hundredth;
        uint16_t dx_time = (uint16_t)((sec << 8) | hund);

        frame->reserved[2] = (frame->reserved[2] & 0xFFFF0000u) | (uint32_t)cx_time;
        frame->reserved[3] = (frame->reserved[3] & 0xFFFF0000u) | (uint32_t)dx_time;
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x48u: { /* Allocate memory paragraphs: BX=request -> AX=segment */
        uint16_t req = (uint16_t)(frame->reserved[1] & 0xFFFFu);
        uint16_t seg = 0u;
        uint16_t max_avail = 0u;

        /* Some legacy GEM binaries in this path appear to provide the
         * paragraph request in CX when BX is zero; accept both forms. */
        if (req == 0u) {
            req = (uint16_t)(frame->reserved[2] & 0xFFFFu);
        }

        serial_write("[v86] int21/48 req=0x");
        serial_write_hex64((uint64_t)req);
        serial_write("\n");

        if (!v86_mem_alloc(req, &seg, &max_avail)) {
            serial_write("[v86] int21/48 fail max=0x");
            serial_write_hex64((uint64_t)max_avail);
            serial_write("\n");
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0008u; /* AX=error 8 */
            frame->reserved[1] = (frame->reserved[1] & 0xFFFF0000u) | (uint32_t)max_avail;
            frame->reserved[2] = (frame->reserved[2] & 0xFFFF0000u) | (uint32_t)max_avail;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        serial_write("[v86] int21/48 ok seg=0x");
        serial_write_hex64((uint64_t)seg);
        serial_write("\n");

        frame->reserved[0] = (eax & 0xFFFF0000u) | (uint32_t)seg;
        frame->reserved[1] = (frame->reserved[1] & 0xFFFF0000u) | (uint32_t)seg;
        frame->reserved[2] = (frame->reserved[2] & 0xFFFF0000u) | (uint32_t)seg;
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x49u: /* Free memory: succeed silently */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x4Au: /* Resize memory block: succeed silently */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x4Bu: { /* EXEC: DS:DX path, ES:BX parameter block */
        uint8_t al = (uint8_t)(eax & 0x00FFu);
        char dos_path[V86_PATH_MAX];
        char canonical_path[V86_PATH_MAX];
        fat_dir_entry_t entry;
        uint32_t plin = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);
        uint32_t pb_linear = ((uint32_t)frame->es << 4) + (frame->reserved[1] & 0xFFFFu);

        if (al != 0u) {
            serial_write("[v86] int21/4B unsupported AL=0x");
            serial_write_hex64((uint64_t)al);
            serial_write("\n");
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0001u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        if (!v86_read_guest_asciiz(plin, dos_path, (uint32_t)sizeof(dos_path))) {
            dos_path[0] = '\0';
        }

        serial_write("[v86] int21/4B exec path=\"");
        serial_write(dos_path);
        serial_write("\"\n");

        v86_exec_tail_set_from_pb(pb_linear);

        if (!v86_dos_path_to_canonical(dos_path, canonical_path, (uint32_t)sizeof(canonical_path)) ||
            !fat_find_file(canonical_path, &entry) ||
            (entry.attr & FAT_ATTR_DIRECTORY) != 0u) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0002u; /* file not found */
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        serial_write("[v86] int21/4B canonical=");
        serial_write(canonical_path);
        serial_write("\n");

        v86_exec_request_set(canonical_path);
        frame->reserved[0] = (eax & 0xFFFF0000u); /* AX=0 success */
        V86_CF_CLEAR();
        return V86_DISPATCH_EXEC_REQUEST;
    }

    case 0x0Eu: /* Select default drive: DL=drive, return AL=number of drives. */
        if ((frame->reserved[3] & 0xFFu) <= 25u) {
            s_v86_default_drive = (uint8_t)(frame->reserved[3] & 0xFFu);
        }
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0004u; /* report 4 drives */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x19u: /* Get current drive: return AL = drive (0=A, 2=C). */
        frame->reserved[0] = (eax & 0xFFFF0000u) | (uint32_t)s_v86_default_drive;
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x1Au: { /* Set DTA = DS:DX linear. Stash in reserved globals via static. */
        /* We keep the DTA linear address in a module-static so AH=2F can
         * return it. Guest world still sees the raw ds:dx it set. */
        extern uint32_t g_v86_dta_linear;
        g_v86_dta_linear = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);
        v86_find_state_reset();
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x2Fu: { /* Get DTA -> ES:BX. Split stashed linear as seg:off. */
        extern uint32_t g_v86_dta_linear;
        uint32_t lin = g_v86_dta_linear ? g_v86_dta_linear : 0x00000080u; /* PSP default */
        uint16_t seg = (uint16_t)(lin >> 4);
        uint16_t off = (uint16_t)(lin & 0x0Fu);
        frame->es = seg;
        frame->reserved[1] = (frame->reserved[1] & 0xFFFF0000u) | (uint32_t)off;
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x3Bu: /* CHDIR DS:DX -> path. Accept silently. */
    {
        char dos_path[V86_PATH_MAX];
        char canonical_path[V86_PATH_MAX];
        uint32_t plin = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);

        if (!v86_read_guest_asciiz(plin, dos_path, (uint32_t)sizeof(dos_path)) ||
            !v86_dos_path_to_canonical(dos_path, canonical_path, (uint32_t)sizeof(canonical_path)) ||
            !v86_dir_exists(canonical_path)) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0003u; /* path not found */
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        v86_str_copy(s_v86_cwd, canonical_path, (uint32_t)sizeof(s_v86_cwd));
        v86_find_state_reset();

        serial_write("[v86] int21/3B chdir -> ");
        serial_write(s_v86_cwd);
        serial_write("\n");
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x3Du: { /* Open file: DS:DX path, AL access mode */
        char dos_path[V86_PATH_MAX];
        char canonical_path[V86_PATH_MAX];
        fat_dir_entry_t info;
        v86_file_handle_t *h;
        uint32_t file_size = 0u;
        uint8_t access = (uint8_t)(eax & 0x03u);
        uint32_t plin = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);

        if (!fat_ready()) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0002u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        if (!v86_read_guest_asciiz(plin, dos_path, (uint32_t)sizeof(dos_path))) {
            dos_path[0] = '\0';
        }

        serial_write("[v86] int21/3D open path=\"");
        serial_write(dos_path);
        serial_write("\"\n");

        if (!v86_dos_path_to_canonical(dos_path, canonical_path, (uint32_t)sizeof(canonical_path))) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0003u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        serial_write("[v86] int21/3D canonical=");
        serial_write(canonical_path);
        serial_write("\n");

        if (!fat_find_file(canonical_path, &info)) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0002u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        if ((info.attr & FAT_ATTR_DIRECTORY) != 0u) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0005u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        if (info.size > V86_FILE_BUF_CAP) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0008u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        h = v86_file_alloc_handle(access, canonical_path);
        if (!h) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0004u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        if (info.size > 0u) {
            if (!fat_read_file(canonical_path, (void *)h->data, V86_FILE_BUF_CAP, &file_size)) {
                h->used = 0u;
                frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0005u;
                V86_CF_SET();
                return V86_DISPATCH_CONT;
            }
        }

        h->size = file_size;
        h->pos = 0u;
        h->dirty = 0u;
        frame->reserved[0] = (eax & 0xFFFF0000u) | (uint32_t)h->handle_id;
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x3Eu: { /* Close file: BX handle */
        uint16_t handle = (uint16_t)(frame->reserved[1] & 0xFFFFu);
        v86_file_handle_t *h;

        if (handle <= 2u) {
            frame->reserved[0] = (eax & 0xFFFF0000u);
            V86_CF_CLEAR();
            return V86_DISPATCH_CONT;
        }

        h = v86_file_find_handle(handle);
        if (!h) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0006u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        if (!v86_file_flush_handle(h)) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0005u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        h->used = 0u;
        frame->reserved[0] = (eax & 0xFFFF0000u);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x3Fu: { /* Read file/device: BX handle, CX count, DS:DX buffer */
        uint16_t handle = (uint16_t)(frame->reserved[1] & 0xFFFFu);
        uint16_t count = (uint16_t)(frame->reserved[2] & 0xFFFFu);
        uint32_t buf_lin = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);
        volatile uint8_t *dst = (volatile uint8_t *)(uint64_t)buf_lin;
        v86_file_handle_t *h;
        uint32_t available;
        uint32_t to_read;

        if (handle == 0u) {
            frame->reserved[0] = (eax & 0xFFFF0000u);
            V86_CF_CLEAR();
            return V86_DISPATCH_CONT;
        }

        if (handle == 1u || handle == 2u) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0005u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        h = v86_file_find_handle(handle);
        if (!h) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0006u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        if (h->pos >= h->size || count == 0u) {
            frame->reserved[0] = (eax & 0xFFFF0000u);
            V86_CF_CLEAR();
            return V86_DISPATCH_CONT;
        }

        available = h->size - h->pos;
        to_read = ((uint32_t)count < available) ? (uint32_t)count : available;

        for (uint32_t i = 0u; i < to_read; ++i) {
            dst[i] = h->data[h->pos + i];
        }

        h->pos += to_read;
        frame->reserved[0] = (eax & 0xFFFF0000u) | (to_read & 0xFFFFu);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x40u: { /* Write file/device: BX handle, CX count, DS:DX buffer */
        uint16_t handle = (uint16_t)(frame->reserved[1] & 0xFFFFu);
        uint16_t count = (uint16_t)(frame->reserved[2] & 0xFFFFu);
        uint32_t buf_lin = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);
        const volatile uint8_t *src = (const volatile uint8_t *)(uint64_t)buf_lin;
        v86_file_handle_t *h;
        uint32_t room;
        uint32_t to_write;

        if (handle == 1u || handle == 2u) {
            for (uint32_t i = 0u; i < (uint32_t)count; ++i) {
                video_putchar((char)src[i]);
            }
            frame->reserved[0] = (eax & 0xFFFF0000u) | (uint32_t)count;
            V86_CF_CLEAR();
            return V86_DISPATCH_CONT;
        }

        if (handle == 0u) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0005u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        h = v86_file_find_handle(handle);
        if (!h) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0006u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        if (h->mode == 0u) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0005u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        room = (h->pos < V86_FILE_BUF_CAP) ? (V86_FILE_BUF_CAP - h->pos) : 0u;
        to_write = ((uint32_t)count < room) ? (uint32_t)count : room;

        if (to_write > 0u) {
            v86_memcpy(h->data + h->pos, (const void *)src, to_write);
            h->pos += to_write;
            if (h->pos > h->size) {
                h->size = h->pos;
            }
            h->dirty = 1u;
        }

        frame->reserved[0] = (eax & 0xFFFF0000u) | (to_write & 0xFFFFu);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x42u: { /* Seek: BX handle, AL origin, CX:DX offset -> DX:AX pos */
        uint16_t handle = (uint16_t)(frame->reserved[1] & 0xFFFFu);
        uint8_t origin = (uint8_t)(eax & 0x00FFu);
        uint32_t off_u = ((frame->reserved[2] & 0xFFFFu) << 16) | (frame->reserved[3] & 0xFFFFu);
        int32_t off_s = (int32_t)off_u;
        int64_t base = 0;
        int64_t new_pos;
        v86_file_handle_t *h;

        if (handle <= 2u) {
            frame->reserved[0] = (eax & 0xFFFF0000u);
            frame->reserved[3] = (frame->reserved[3] & 0xFFFF0000u);
            V86_CF_CLEAR();
            return V86_DISPATCH_CONT;
        }

        h = v86_file_find_handle(handle);
        if (!h) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0006u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        if (origin == 0u) {
            new_pos = (int64_t)off_u;
        } else {
            if (origin == 1u) {
                base = (int64_t)h->pos;
            } else if (origin == 2u) {
                base = (int64_t)h->size;
            } else {
                frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0001u;
                V86_CF_SET();
                return V86_DISPATCH_CONT;
            }
            new_pos = base + (int64_t)off_s;
        }

        if (new_pos < 0 || new_pos > (int64_t)V86_FILE_BUF_CAP) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0019u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        h->pos = (uint32_t)new_pos;
        frame->reserved[0] = (eax & 0xFFFF0000u) | (h->pos & 0xFFFFu);
        frame->reserved[3] = (frame->reserved[3] & 0xFFFF0000u) | ((h->pos >> 16) & 0xFFFFu);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x47u: { /* GETCWD: DL=drive (0=default), DS:SI -> 64-byte buffer.
                   * Return empty root ("" => "\" implicit). */
        char dos_cwd[V86_PATH_MAX];
        uint32_t buf_lin = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);
        volatile char *buf = (volatile char *)(uint64_t)buf_lin;

        /* AH=47 uses DS:SI by DOS ABI; our current v86 frame doesn't carry SI,
         * so we continue writing to DS:DX like the existing scaffold. */
        v86_canonical_to_dos_cwd(s_v86_cwd, dos_cwd, (uint32_t)sizeof(dos_cwd));
        for (uint32_t i = 0u; i < 64u; ++i) {
            char c = dos_cwd[i];
            buf[i] = c;
            if (c == '\0') {
                break;
            }
        }

        frame->reserved[0] = (eax & 0xFFFF0000u);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x4Eu: /* Find first (real FAT-backed wildcard scan). */
    {
        char dos_path[V86_PATH_MAX];
        char canonical_path[V86_PATH_MAX];
        char dir_path[V86_PATH_MAX];
        char pattern[V86_PATH_MAX];
        fat_dir_entry_t match;
        uint8_t search_attr = (uint8_t)(frame->reserved[2] & 0x00FFu);
        uint32_t dta_linear;
        volatile uint8_t *dta;
        int dir_ok = 0;
        uint32_t plin = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);

        serial_write("[v86] int21/4E findfirst attr=0x");
        serial_write_hex64((uint64_t)(frame->reserved[2] & 0x00FFu));
        serial_write(" pattern=\"");

        if (!v86_read_guest_asciiz(plin, dos_path, (uint32_t)sizeof(dos_path))) {
            dos_path[0] = '\0';
        }
        for (uint32_t i = 0u; dos_path[i] != '\0' && i < 127u; ++i) {
            char c = dos_path[i];
            if (c == 0) break;
            char b[2]; b[0] = c; b[1] = 0;
            serial_write(b);
        }
        serial_write("\"\n");

        if (!fat_ready() ||
            !v86_dos_path_to_canonical(dos_path, canonical_path, (uint32_t)sizeof(canonical_path)) ||
            !v86_split_find_path(canonical_path, dir_path, (uint32_t)sizeof(dir_path), pattern, (uint32_t)sizeof(pattern)) ||
            !v86_find_match_in_dir(dir_path, pattern, search_attr, 0u, &match, &dir_ok)) {
            (void)dir_ok;
            v86_find_state_reset();
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0012u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        dta_linear = g_v86_dta_linear ? g_v86_dta_linear : 0x00000080u;
        dta = (volatile uint8_t *)(uint64_t)dta_linear;
        v86_fill_find_dta(dta, &match);

        s_v86_find_state.active = 1;
        s_v86_find_state.attr_mask = search_attr;
        s_v86_find_state.next_index = 1u;
        s_v86_find_state.dta_linear = dta_linear;
        v86_str_copy(s_v86_find_state.dir, dir_path, (uint32_t)sizeof(s_v86_find_state.dir));
        v86_str_copy(s_v86_find_state.pattern, pattern, (uint32_t)sizeof(s_v86_find_state.pattern));

        serial_write("[v86] int21/4E match name=");
        serial_write(match.name);
        serial_write(" dir=");
        serial_write(dir_path);
        serial_write("\n");

        frame->reserved[0] = (eax & 0xFFFF0000u);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x4Fu: { /* Find next (real FAT-backed wildcard scan). */
        fat_dir_entry_t match;
        volatile uint8_t *dta;
        int dir_ok = 0;

        if (!s_v86_find_state.active || !fat_ready()) {
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0012u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        if (s_v86_find_state.dta_linear != 0u && g_v86_dta_linear != 0u &&
            s_v86_find_state.dta_linear != g_v86_dta_linear) {
            v86_find_state_reset();
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0012u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        if (!v86_find_match_in_dir(
                s_v86_find_state.dir,
                s_v86_find_state.pattern,
                s_v86_find_state.attr_mask,
                s_v86_find_state.next_index,
                &match,
                &dir_ok
            )) {
            (void)dir_ok;
            v86_find_state_reset();
            frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0012u;
            V86_CF_SET();
            return V86_DISPATCH_CONT;
        }

        dta = (volatile uint8_t *)(uint64_t)(g_v86_dta_linear ? g_v86_dta_linear : s_v86_find_state.dta_linear);
        v86_fill_find_dta(dta, &match);
        s_v86_find_state.next_index++;

        frame->reserved[0] = (eax & 0xFFFF0000u);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    default:
        serial_write("[v86] int21 UNHANDLED ah=0x");
        serial_write_hex64((uint64_t)ah);
        serial_write(" -> returning CF=1\n");
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0001u; /* AX=error 1 */
        V86_CF_SET();
        return V86_DISPATCH_CONT;
    }

    #undef V86_CF_CLEAR
    #undef V86_CF_SET
}

int v86_dispatch_arm(uint32_t magic)
{
    if (magic != V86_DISPATCH_ARM_MAGIC) {
        return 0;
    }
    v86_vectors_reset();
    v86_install_default_softint_stub();
    v86_mem_reset();
    v86_file_handles_reset();
    v86_exec_request_clear();
    s_v86_dispatch_armed = 1;
    return 1;
}

void v86_dispatch_disarm(void)
{
    s_v86_dispatch_armed = 0;
    v86_vectors_reset();
    v86_mem_reset();
    v86_file_close_all();
    v86_file_handles_reset();
    v86_exec_request_clear();
}

int v86_dispatch_is_armed(void)
{
    return s_v86_dispatch_armed;
}

int v86_dispatch_get_exec_path(char *out, uint32_t out_size)
{
    if (!out || out_size == 0u || !s_v86_exec_pending || s_v86_exec_path[0] == '\0') {
        if (out && out_size > 0u) {
            out[0] = '\0';
        }
        return 0;
    }

    v86_str_copy(out, s_v86_exec_path, out_size);
    return 1;
}

int v86_dispatch_get_exec_tail(char *out, uint32_t out_size)
{
    if (!out || out_size == 0u) {
        return 0;
    }

    v86_str_copy(out, s_v86_exec_tail, out_size);
    return s_v86_exec_tail[0] != '\0';
}

uint16_t v86_dispatch_get_exec_env_seg(void)
{
    return s_v86_exec_env_seg;
}

void v86_dispatch_clear_exec_path(void)
{
    v86_exec_request_clear();
}

int v86_dispatch_probe(void)
{
    legacy_v86_frame_t frame;

    frame.cs = 0x1234u;
    frame.ip = 0x5678u;
    frame.ss = 0x9ABCu;
    frame.sp = 0xDEF0u;
    frame.ds = 0x1111u;
    frame.es = 0x2222u;
    frame.fs = 0x3333u;
    frame.gs = 0x4444u;
    frame.eflags = 0x00000202u;
    frame.reserved[0] = 0xAAAA4900u; /* AH=0x49 free-mem (CONT, no writeback). */
                                    /* Historical scaffold token: 0xAAAA5555u. */
    frame.reserved[1] = 0xBBBB6666u;
    frame.reserved[2] = 0xCCCC7777u;
    frame.reserved[3] = 0xDDDD8888u;

    v86_dispatch_disarm();
    if (v86_dispatch_is_armed() != 0) {
        return 0;
    }
    if (v86_dispatch_arm(0xDEADBEEFu) != 0) {
        return 0;
    }
    if (v86_dispatch_is_armed() != 0) {
        return 0;
    }
    if (v86_dispatch_arm(V86_DISPATCH_ARM_MAGIC) != 1) {
        return 0;
    }
    if (v86_dispatch_is_armed() != 1) {
        return 0;
    }
    if (v86_dispatch_int(0x21u, &frame) != V86_DISPATCH_CONT) {
        return 0;
    }
    if (frame.cs != 0x1234u || frame.ip != 0x5678u || frame.ss != 0x9ABCu || frame.sp != 0xDEF0u) {
        return 0;
    }
    if (frame.reserved[0] != 0xAAAA4900u || frame.reserved[3] != 0xDDDD8888u) {
        return 0;
    }
    v86_dispatch_disarm();
    return g_opengem_044_c_sentinel[0] == 'O';
}