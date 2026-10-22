/* SPDX-License-Identifier: GPL-2.0-only
 * Execute the native PCI transaction with synthetic I/O, never real ports. */
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#define CIUKI_CPU_H
static uint32_t selected;
static unsigned writes, saves, restores;
static uint16_t command=0xffff, status=0xf900;
static bool identity=true, obey=true;
static uint32_t irq_save(void) { saves++; return 0x200; }
static void irq_restore(uint32_t f) { assert(f==0x200); restores++; }
static void outl(uint16_t port,uint32_t value) { assert(port==0xcf8); selected=value; }
static uint32_t inl(uint16_t port) {
    assert(port==0xcfc && selected==0x80021800); return identity?0x100e8086:UINT32_MAX;
}
static uint16_t cpu_inw(uint16_t port) { assert(port==0xcfc && selected==0x80021804); return command; }
static void cpu_outw(uint16_t port,uint16_t value) {
    assert(port==0xcfc && selected==0x80021804 && value==0xfff8);
    writes++; if(obey) command=value;
}
#include "src/kernel/ndis/fence_kernel.c"
int main(void) {
    struct pci_func p={.bus=2,.dev=3,.vendor=0x8086,.device=0x100e}; uint16_t before=0,after=0;
    assert(ndis_pci_disable(&p,&before,&after));
    assert(before==0xffff && after==0xfff8 && status==0xf900 && writes==1);
    identity=false; assert(!ndis_pci_disable(&p,&before,&after) && writes==1);
    identity=true; obey=false; command=7;
    /* For the failed-readback case, expected write value changes. */
    command=0xffff;
    assert(!ndis_pci_disable(&p,&before,&after) && writes==2 && command==0xffff);
    p.dev=32; assert(!ndis_pci_disable(&p,&before,&after));
    assert(saves==3 && restores==saves && status==0xf900);
    puts("PASS native PCI command word transaction identity decode BM INTx readback and W1C preservation");
    fflush(stdout);
}
