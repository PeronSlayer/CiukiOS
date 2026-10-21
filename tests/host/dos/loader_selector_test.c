/* Execute production selector_validate with libx86emu; no BIOS or VM boot.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86emu.h>
#include <ciuki/boot_info.h>
#include "../../../src/kernel/probes/selector.h"

static int result = -1;
static int interrupt(x86emu_t *cpu, u8 vector, unsigned type)
{
    (void)type;
    assert(vector == 0xe9);
    result = cpu->x86.R_AX;
    x86emu_stop(cpu);
    return 1;
}

void check_loader_selector(const char *binary, const char *request, unsigned source, int valid)
{
    x86emu_t *cpu = x86emu_new(X86EMU_PERM_RWX, 0);
    assert(cpu);
    FILE *stream = fopen(binary, "rb");
    assert(stream);
    unsigned at = 0x1000;
    int byte;
    while ((byte = fgetc(stream)) != EOF) x86emu_write_byte(cpu, at++, (u8)byte);
    assert(!ferror(stream));
    fclose(stream);
    /* The wrapper's fixed jump/header precedes the unchanged production code. */
    x86emu_write_word(cpu, 0x1003, (unsigned)strlen(request));
    x86emu_write_byte(cpu, 0x1005, source);
    for (unsigned i = 0; i < strlen(request); i++)
        x86emu_write_byte(cpu, 0x1006 + i, request[i]);
    for (unsigned i = 0; i < 6; i++) x86emu_set_seg_register(cpu, cpu->x86.seg + i, 0);
    cpu->x86.R_IP = 0x1000;
    cpu->x86.R_SP = 0xf000;
    cpu->max_instr = 20000;
    result = -1;
    x86emu_set_intr_handler(cpu, interrupt);
    x86emu_run(cpu, 0);
    assert(result == !valid);
    x86emu_done(cpu);
    struct probe_selection selection;
    uint32_t flags = source == 1 ? CBI_F_SMBIOS_QEMU | CBI_F_INPUT_FORCED : 0;
    assert(probes_parse_selector(request, strlen(request), flags, &selection) == valid);
}
