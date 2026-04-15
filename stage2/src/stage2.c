#include "serial.h"
#include "cpu_tables.h"
#include "interrupts.h"
#include "timer.h"
#include "keyboard.h"
#include "bootinfo.h"
#include "handoff.h"

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static void idle_forever(void) {
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void stage2_main(boot_info_t *boot_info, handoff_v0_t *handoff) {
    serial_init();
    serial_write("\n[ stage2 ] scaffolding started\n");

    if (!boot_info) {
        serial_write("[ panic ] null boot_info\n");
        halt_forever();
    }

    if (boot_info->magic != BOOTINFO_MAGIC) {
        serial_write("[ panic ] invalid boot_info magic: 0x");
        serial_write_hex64(boot_info->magic);
        serial_write("\n");
        halt_forever();
    }

    serial_write("[ ok ] boot_info is valid\n");
    serial_write("[ info ] memory_map_ptr: 0x");
    serial_write_hex64(boot_info->memory_map_ptr);
    serial_write("\n");

    if (!handoff) {
        serial_write("[ panic ] null handoff\n");
        halt_forever();
    }

    if (handoff->magic != HANDOFF_V0_MAGIC) {
        serial_write("[ panic ] invalid handoff magic: 0x");
        serial_write_hex64(handoff->magic);
        serial_write("\n");
        halt_forever();
    }

    if (handoff->version != HANDOFF_V0_VERSION) {
        serial_write("[ panic ] invalid handoff version: 0x");
        serial_write_hex64(handoff->version);
        serial_write("\n");
        halt_forever();
    }

    serial_write("[ ok ] handoff v0 is valid\n");
    serial_write("[ info ] stage2_load_addr: 0x");
    serial_write_hex64(handoff->stage2_load_addr);
    serial_write("\n");
    serial_write("[ info ] stage2_size: 0x");
    serial_write_hex64(handoff->stage2_size);
    serial_write("\n");

    stage2_init_gdt_tss();
    serial_write("[ ok ] stage2 local gdt+tss is active\n");

    stage2_init_idt();
    serial_write("[ ok ] stage2 local idt is active\n");

    stage2_timer_init();
    serial_write("[ ok ] pic remapped and pit started\n");

    stage2_keyboard_init();
    serial_write("[ ok ] keyboard irq1 logger is ready\n");

    stage2_enable_interrupts();
    serial_write("[ ok ] interrupts enabled (timer irq0 + keyboard irq1)\n");

    serial_write("[ stage2 ] next step: handoff to DOS-like runtime\n");
    idle_forever();
}
