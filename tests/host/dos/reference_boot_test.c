/* Reproduce the FAT16 template in the exact compressed release SYS.COM.
 * Stop at the unpacker's first INT21 (memory resize); never execute SYS's
 * installer or any DOS/block services. No image, host ports or disk writes.
 * SPDX-License-Identifier: MIT */
#include <x86emu.h>
#include <stdio.h>
#include <stdbool.h>
static bool stopped;
static int interrupt(x86emu_t *e,u8 vector,unsigned type)
{
    (void)type;
    stopped=vector==0x21 && e->x86.R_AX==0x4a00 && e->x86.R_CS==0x1000 && e->x86.R_IP==0x197;
    x86emu_stop(e);return 1;
}
int main(int argc,char **argv)
{
    if (argc!=2) return 2;
    FILE *input=fopen(argv[1],"rb");if (!input) return 3;
    x86emu_t *e=x86emu_new(X86EMU_PERM_RWX,0);if (!e) { fclose(input);return 4; }
    unsigned at=0x10100;int byte;
    while ((byte=fgetc(input))!=EOF && at<0x20000) x86emu_write_byte(e,at++,byte);
    bool valid=feof(input);fclose(input);
    if (!valid) { x86emu_done(e);return 5; }
    for (unsigned i=0;i<6;i++) x86emu_set_seg_register(e,e->x86.seg+i,0x1000);
    e->x86.R_IP=0x100;e->x86.R_SP=0xfffe;e->x86.R_EFLG=0x202;e->max_instr=2000000;
    x86emu_set_intr_handler(e,interrupt);x86emu_run(e,0);
    if (stopped) for (unsigned i=0;i<512;i++) putchar(x86emu_read_byte(e,0x12fc8+i));
    x86emu_done(e);return stopped && !ferror(stdout) ? 0 : 6;
}
