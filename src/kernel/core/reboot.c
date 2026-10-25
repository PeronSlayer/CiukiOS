/* Reset fallback follows Linux x86 reboot.c and Intel SDM Vol.3 #DF/shutdown.
 * Source links and the QEMU -no-reboot policy: test-architecture.md (f1-28).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/reboot.h>
#ifndef REBOOT_HOST
#include <ciuki/cpu.h>
#include <ciuki/kernel.h>
#endif

void reboot_sequence(const struct reboot_ops *ops, void *ctx)
{
    ops->disable(ctx);
    for (unsigned n = 0; n < 10000; n++) {
        if (!(ops->status(ctx) & 2)) { ops->pulse(ctx); break; }
        ops->delay(ctx);
    }
    /* Give the 8042 time to assert reset before invalidating the IDT. */
    for (unsigned n = 0; n < 100; n++) ops->delay(ctx);
    if (ops->hard) { ops->hard(ctx); for (unsigned n = 0; n < 100; n++) ops->delay(ctx); }
    ops->triple(ctx);
}
#ifndef REBOOT_HOST
static void disable(void *ctx) { (void)ctx; __asm__ volatile("cli" ::: "memory"); }
static uint8_t status(void *ctx) { (void)ctx; return inb(0x64); }
static void pulse(void *ctx) { (void)ctx; outb(0x64, 0xfe); }
static void delay(void *ctx) { (void)ctx; outb(0x80, 0); }
static void triple(void *ctx)
{
    (void)ctx;
    const struct { uint16_t limit; uint32_t base; } __attribute__((packed)) empty = { 0, 0 };
    /* #GP delivering INT3, then #DF delivery outside the zero-limit IDT. */
    __asm__ volatile("lidt %0; int3" :: "m"(empty) : "memory");
}
/* Review 3, finding 3: the keyboard pulse and the triple fault are a CPU
 * INIT, not a PCI reset (ICH3 datasheet 82801CA, 5.11.1.2 and 9.7.5). An
 * Intel ISA/LPC bridge (PIIX4, ICH: class 0601h, vendor 8086h, bus 0) has
 * the reset control register RST_CNT at I/O 0CF9h: SYS_RST (bit 1) with
 * RST_CPU (bit 2) is a full reset of the whole platform, PCI included.
 * Written only when such a bridge is found; the keyboard pulse was tried
 * first, so a machine that already reset never reaches this. */
static bool intel_bridge(void)
{
    for (unsigned dev = 0; dev < 32; dev++) {
        uint32_t address = 0x80000000u | dev << 11;
        outl(0xcf8, address);
        uint32_t id = inl(0xcfc);
        if (id == 0xffffffffu || (id & 0xffffu) != 0x8086u) continue;
        outl(0xcf8, address | 8);
        if ((inl(0xcfc) >> 16) == 0x0601u) return true;
    }
    return false;
}
static void hard(void *ctx)
{
    (void)ctx;
    if (!intel_bridge()) return;
    outb(0xcf9, 0x02); delay(0); outb(0xcf9, 0x06);
}
__attribute__((noreturn)) void kernel_reboot(void)
{
    static const struct reboot_ops ops = { disable, status, pulse, delay, triple, hard };
    /* serial_write waits for THRE, not the shift register. Drain TEMT so the
     * last cut/completion record's CRLF survives the reset (PC16550D LSR.6).
     * No port access if the loader/kernel disabled the sink; finite polls. */
    if (serial_present()) for (unsigned n = 0; n < 10000; n++) {
        if (inb((uint16_t)g_boot.uart_base + 5) & 0x40) break;
        delay(0);
    }
    reboot_sequence(&ops, 0);
    for (;;) __asm__ volatile("cli; hlt");
}
#endif
