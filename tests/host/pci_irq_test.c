/* Production route qualification, fake PCI/ROM/ELCR only. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <ciuki/kernel.h>
#include <ciuki/registry.h>
#define CIUKI_CPU_H
static uint8_t bios[65536], elcr[2];
static uint32_t config[32][64], address, flags = 0x200;
static struct pci_func router;
static struct pci_func bridge;
static bool bridge_present;
struct ciuki_boot_info g_boot;
static uint32_t irq_save(void) { uint32_t f = flags; flags = 0; return f; }
static void irq_restore(uint32_t f) { flags = f; }
static void outl(uint16_t port, uint32_t value) { assert(port == 0xcf8 && !flags); address = value; }
static uint32_t inl(uint16_t port) { assert(port == 0xcfc && !flags); return config[(address >> 11) & 31][(address & 255) / 4]; }
static uint8_t inb(uint16_t port) { assert(port == 0x4d0 || port == 0x4d1); return elcr[port - 0x4d0]; }
unsigned pci_count(void) { return bridge_present ? 2 : 1; }
const struct pci_func *pci_get(unsigned i) { assert(i<pci_count()); return i ? &bridge : &router; }
#define P2V(p) ((void *)(bios + ((p) - 0xf0000)))
#include "../../src/kernel/core/pci_irq.c"
static void checksum(void)
{ bios[31] = 0; for (unsigned i = 0; i < 48; i++) if (i != 31) bios[31] -= bios[i]; }
int main(void)
{
    router = (struct pci_func){.dev=31,.vendor=0x8086,.device=0x248c,.class_code=6,.subclass=1};
    elcr[1] = 8; config[31][0x60 / 4] = 0x0b0b0b0b; config[31][0x68 / 4] = 11;
    struct pci_func p = {.dev=29,.vendor=0x8086,.device=0x2482,.class_code=12,.subclass=3,.irq_pin=1,.irq_line=11};
    const unsigned ids[] = {0x2482,0x2484,0x2487}, links[] = {0,3,2};
    for (unsigned i = 0; i < 3; i++) {
        p.fn=i; p.device=ids[i]; p.irq_pin=i+1;
        assert(pci_irq_routed(&p));
        config[31][0x60 / 4] |= 0x20u << (links[i] * 8);
        assert(pci_irq_routed(&p)); /* reserved bits do not change IRQ 11 */
        config[31][0x60 / 4] ^= 0x80u << (links[i] * 8);
        assert(!pci_irq_routed(&p));
        config[31][0x60 / 4] ^= 0x80u << (links[i] * 8);
    }
    bridge=(struct pci_func){.dev=30,.vendor=0x8086,.device=0x2448,.class_code=6,.subclass=4};
    bridge_present=true;config[30][0x18/4]=0x00020200;
    p = (struct pci_func){.bus=2,.dev=8,.vendor=0x8086,.device=0x1031,.class_code=2,.irq_pin=1,.irq_line=11};
    assert(pci_irq_routed(&p)); config[31][0x68/4] = 10; assert(!pci_irq_routed(&p)); config[31][0x68/4] = 11;
    config[31][0x68/4] = 0x7b; assert(pci_irq_routed(&p));
    config[31][0x68/4] = 0xfb; assert(!pci_irq_routed(&p));
    config[31][0x68/4] = 0x2b;
    /* Secondary-bus device: $PIR pin bitmap and live router, no slot guess. */
    bridge_present=false;
    memcpy(bios,"$PIR",4); bios[5]=1; bios[6]=48; bios[9]=31*8;
    bios[32]=2; bios[33]=8*8; bios[34]=0x68; bios[36]=8; checksum();
    assert(pci_irq_routed(&p)); config[31][0x68/4]=0xab;
    assert(!pci_irq_routed(&p)); config[31][0x68/4]=0x2b;
    assert(pci_irq_routed(&p)); bios[31]++; assert(!pci_irq_routed(&p)); checksum();
    bios[36]=4; checksum(); assert(!pci_irq_routed(&p)); bios[36]=8; checksum();
    elcr[1]=0; assert(!pci_irq_routed(&p)); elcr[1]=8;
    p.command=0x400; assert(!pci_irq_routed(&p)); p.command=0;
    p.irq_pin=0; assert(!pci_irq_routed(&p)); p.irq_pin=5; assert(!pci_irq_routed(&p));
    p.irq_pin=1; p.irq_line=0; assert(!pci_irq_routed(&p)); p.irq_line=16; assert(!pci_irq_routed(&p));
    p.irq_line=11; bios[6]=32; checksum(); assert(!pci_irq_routed(&p));
    memset(bios,0,sizeof(bios));
    router.dev=1; router.device=0x7000; config[1][0x60/4]=0x0b0b0b0b;
    p.bus=0; p.dev=3; g_boot.flags=CBI_F_SMBIOS_QEMU;
    for (unsigned i=0;i<2;i++) {
        router.device=i ? 0x7110:0x7000;
        assert(pci_irq_routed(&p));
        config[1][0x60/4]=0x2b2b2b2b; assert(!pci_irq_routed(&p));
        config[1][0x60/4]=0x8b8b8b8b; assert(!pci_irq_routed(&p));
        config[1][0x60/4]=0x10101010; assert(!pci_irq_routed(&p));
        config[1][0x60/4]=0x0b0b0b0b;
    }
    g_boot.flags=0; assert(!pci_irq_routed(&p));
    /* The firmware-table route must retain PIIX's strict byte decoding. */
    memset(bios,0,sizeof(bios));memcpy(bios,"$PIR",4);bios[5]=1;bios[6]=48;bios[9]=8;
    bios[32]=p.bus;bios[33]=p.dev*8;bios[34]=0x60;bios[36]=8;checksum();
    assert(pci_irq_routed(&p));config[1][0x60/4]=0x2b2b2b2b;assert(!pci_irq_routed(&p));
    assert(flags==0x200);
    puts("PCI INTx routes: ICH3 A/D/C and LAN E, reserved bits ignored, disabled routes refused, secondary-bus PIR, malformed/edge guards, strict QEMU PIIX3/4 bytes PASS");
}
