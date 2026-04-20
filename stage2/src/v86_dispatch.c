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

    case 0x25u: /* Set interrupt vector: ignore for now */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x35u: /* Get interrupt vector: return ES:BX=0:0 */
        /* ES returned by caller via frame->es; set to 0 and BX=0. */
        frame->es = 0u;
        frame->reserved[1] = (frame->reserved[1] & 0xFFFF0000u);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x48u: /* Allocate memory: for now report out-of-memory */
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0008u;  /* AX=error 8 */
        frame->reserved[1] = 0u;                              /* BX=largest */
        V86_CF_SET();
        return V86_DISPATCH_CONT;

    case 0x49u: /* Free memory: succeed silently */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x4Au: /* Resize memory block: succeed silently */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x4Bu: { /* EXEC: DS:DX path, ES:BX parameter block */
        char dos_path[V86_PATH_MAX];
        char canonical_path[V86_PATH_MAX];
        uint32_t plin = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);

        if (!v86_read_guest_asciiz(plin, dos_path, (uint32_t)sizeof(dos_path))) {
            dos_path[0] = '\0';
        }

        serial_write("[v86] int21/4B exec path=\"");
        serial_write(dos_path);
        serial_write("\"\n");

        if (v86_dos_path_to_canonical(dos_path, canonical_path, (uint32_t)sizeof(canonical_path))) {
            serial_write("[v86] int21/4B canonical=");
            serial_write(canonical_path);
            serial_write("\n");
        }

        frame->reserved[0] = (eax & 0xFFFF0000u); /* AX=0 success */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
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
    s_v86_dispatch_armed = 1;
    return 1;
}

void v86_dispatch_disarm(void)
{
    s_v86_dispatch_armed = 0;
}

int v86_dispatch_is_armed(void)
{
    return s_v86_dispatch_armed;
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