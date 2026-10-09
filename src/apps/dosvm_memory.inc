/* Read-only parent DOS arena and INTDA evidence captured before VMFORK EXEC.
 * AH=52 is the sole interrupt used here.  All measurements are collected
 * before disk_log performs any file I/O.  The root fields describe the first
 * MCB in the chain; paragraph totals count block payloads, excluding headers.
 */
#define DOSVM_MEM_FIRST_MIN 0x0040
#define DOSVM_MEM_LIMIT     0xA000
#define DOSVM_MEM_MAX_BLOCKS 128

static void dosvm_memory_invalid(u16 code, u16 first, int have_first,
                                 u16 da_seg, u16 da_off)
{
    char detail[150], number[8];
    str_copy(detail, "DA=");
    fmt_hex4(number, da_seg); str_cat(detail, number);
    str_cat(detail, ":");
    fmt_hex4(number, da_off); str_cat(detail, number);
    str_cat(detail, " FIRST=");
    if (have_first) fmt_hex4(number, first);
    else str_copy(number, "----");
    str_cat(detail, number);
    str_cat(detail, " ROOT=INVALID FREE=INVALID MAX=INVALID USED=INVALID");
    disk_log("Parent memory", detail, code);
}

static void launch_memory_log(void)
{
    struct regs r;
    u16 list_seg, list_off, first, mcb, owner, paras, da_off, da_seg;
    u16 root_owner, root_paras, blocks;
    u8 type, da_bytes[16];
    u32 list_linear, next, free_paras, largest_free, used_paras;
    u32 da_linear, block_start, block_end, da_block_end = 0;
    u16 da_owner, da_paras, da_mcb, da_count, i;
    int valid, reason, have_first, da_found, da_in_header;
    char detail[150], number[12];

    /* Snapshot the original far pointer exactly as stored in the IVT. */
    da_off = peek16(0, 0x0368);
    da_seg = peek16(0, 0x036A);
    da_linear = ((u32)da_seg << 4) + da_off;

    mem_set(&r, 0, sizeof r);
    r.ax = 0x5200;
    r.ds = r.es = app_seg();
    if (intr(0x21, &r)) {
        dosvm_memory_invalid(0x5201, 0, 0, da_seg, da_off);
        return;
    }

    list_seg = r.es;
    list_off = r.bx;
    list_linear = ((u32)list_seg << 4) + list_off;
    if (list_seg >= DOSVM_MEM_LIMIT || list_off < 2 ||
        list_linear < 0x0402UL || list_linear >= 0x000A0000UL) {
        dosvm_memory_invalid(0x5202, 0, 0, da_seg, da_off);
        return;
    }

    first = peek16(list_seg, (u16)(list_off - 2));
    have_first = 1;
    if (first < DOSVM_MEM_FIRST_MIN || first >= DOSVM_MEM_LIMIT) {
        dosvm_memory_invalid(0x5203, first, have_first, da_seg, da_off);
        return;
    }

    free_paras = 0;
    largest_free = 0;
    used_paras = 0;
    root_owner = 0;
    root_paras = 0;
    blocks = 0;
    mcb = first;
    valid = 0;
    da_found = 0;
    da_owner = da_paras = da_mcb = da_count = 0;
    da_in_header = 0;
    reason = 0x5204;

    while (blocks < DOSVM_MEM_MAX_BLOCKS) {
        if (mcb < DOSVM_MEM_FIRST_MIN || mcb >= DOSVM_MEM_LIMIT) {
            reason = 0x5205;
            break;
        }
        type = peek8(mcb, 0);
        if (type != 'M' && type != 'Z') {
            reason = 0x5204;
            break;
        }
        owner = peek16(mcb, 1);
        paras = peek16(mcb, 3);
        block_start = (u32)mcb << 4;
        block_end = ((u32)mcb + (u32)paras + 1UL) << 4;
        if (da_linear >= block_start && da_linear < block_end) {
            da_found = 1;
            da_owner = owner;
            da_paras = paras;
            da_mcb = mcb;
            da_block_end = block_end;
            da_in_header = da_linear < (block_start + 16UL);
        }
        if (!blocks) {
            root_owner = owner;
            root_paras = paras;
        }

        if (owner == 0) {
            free_paras += (u32)paras;
            if ((u32)paras > largest_free) largest_free = (u32)paras;
        } else {
            used_paras += (u32)paras;
        }
        blocks++;

        next = (u32)mcb + (u32)paras + 1UL;
        if (next <= (u32)mcb || next > DOSVM_MEM_LIMIT ||
            (type == 'M' && next >= DOSVM_MEM_LIMIT)) {
            reason = 0x5205;
            break;
        }
        if (type == 'Z') {
            valid = 1;
            break;
        }
        mcb = (u16)next;
    }

    if (!valid) {
        if (blocks >= DOSVM_MEM_MAX_BLOCKS) reason = 0x5206;
        dosvm_memory_invalid((u16)reason, first, have_first, da_seg, da_off);
        return;
    }

    /* Capture the DA target only after the complete arena chain is valid.
     * The physical alias is resolved by linear address; bytes stop at the
     * containing MCB interval and at conventional-memory end. */
    if (da_found && da_linear >= 0x0400UL && da_linear < 0x000A0000UL) {
        u32 available = da_block_end - da_linear;
        if (available > 0x000A0000UL - da_linear)
            available = 0x000A0000UL - da_linear;
        da_count = (u16)(available > 16UL ? 16UL : available);
        for (i = 0; i < da_count; ++i)
            da_bytes[i] = peek8((u16)((da_linear + i) >> 4),
                                (u16)((da_linear + i) & 15UL));
    }

    /* Emit only after the complete chain is validated and totals are frozen. */
    str_copy(detail, "DA=");
    fmt_hex4(number, da_seg); str_cat(detail, number);
    str_cat(detail, ":");
    fmt_hex4(number, da_off); str_cat(detail, number);
    str_cat(detail, " FIRST=");
    fmt_hex4(number, first); str_cat(detail, number);
    str_cat(detail, " ROOT=");
    fmt_hex4(number, root_owner); str_cat(detail, number);
    str_cat(detail, "/");
    fmt_hex4(number, root_paras); str_cat(detail, number);
    str_cat(detail, " FREE=");
    fmt_u32(number, free_paras); str_cat(detail, number);
    str_cat(detail, " MAX=");
    fmt_u32(number, largest_free); str_cat(detail, number);
    str_cat(detail, " USED=");
    fmt_u32(number, used_paras); str_cat(detail, number);
    str_cat(detail, " BLOCKS=");
    fmt_u32(number, blocks); str_cat(detail, number);
    disk_log("Parent memory", detail, 0);

    /* Preserve the original statistics record; the detail record carries the
     * owner interval and a bounded byte sample without truncating either. */
    str_copy(detail, "DA=");
    fmt_hex4(number, da_seg); str_cat(detail, number);
    str_cat(detail, ":");
    fmt_hex4(number, da_off); str_cat(detail, number);
    if (da_found && da_count) {
        str_cat(detail, " MCB=");
        fmt_hex4(number, da_mcb); str_cat(detail, number);
        str_cat(detail, " OWNER=");
        fmt_hex4(number, da_owner); str_cat(detail, number);
        str_cat(detail, " PARAS=");
        fmt_hex4(number, da_paras); str_cat(detail, number);
        str_cat(detail, da_in_header ? " HDR" : " DATA");
        str_cat(detail, " N="); fmt_u32(number, da_count); str_cat(detail, number);
        str_cat(detail, " BYTES=");
        for (i = 0; i < da_count; ++i) {
            static const char hex[] = "0123456789ABCDEF";
            char byte[3];
            byte[0] = hex[da_bytes[i] >> 4];
            byte[1] = hex[da_bytes[i] & 15];
            byte[2] = 0;
            if (i) str_cat(detail, " ");
            str_cat(detail, byte);
        }
    } else {
        str_cat(detail, " MCB=INVALID N=0 BYTES=INVALID");
    }
    disk_log("Parent DA", detail, da_found && da_count ? 0 : 0x5207);
}

#undef DOSVM_MEM_FIRST_MIN
#undef DOSVM_MEM_LIMIT
#undef DOSVM_MEM_MAX_BLOCKS
