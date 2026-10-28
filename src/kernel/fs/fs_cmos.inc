/* Sole native CMOS accessor, shared by calendar, EC and APM.
 * Included by fs_port.c; host firmware tests replace only CPU/port access.
 * See docs/validation/fx-biosvm2.md. SPDX-License-Identifier: GPL-2.0-only */
static uint32_t rtc_flags;
/* Port 70h is not a readable index register. The UP native policy starts
 * with NMI enabled; NMI diagnostics never touch CMOS. */
static uint8_t rtc_index_shadow, rtc_saved_index;
static void rtc_select(uint8_t index)
{
    outb(0x70, index);
    rtc_index_shadow = index;
}
static void rtc_begin(void *ctx)
{
    (void)ctx;
    rtc_flags = irq_save();
    rtc_saved_index = rtc_index_shadow;
}
static void rtc_end(void *ctx)
{
    (void)ctx;
    rtc_select(rtc_saved_index);
    irq_restore(rtc_flags);
}
static uint8_t rtc_read(void *ctx, uint8_t reg)
{
    (void)ctx;
    rtc_select(reg & 0x7F);
    return inb(0x71);
}
static void rtc_write(void *ctx, uint8_t reg, uint8_t value)
{
    (void)ctx;
    rtc_select(reg & 0x7F);
    outb(0x71, value);
}
uint8_t fs_cmos_read(uint8_t reg)
{
    rtc_begin(0);
    uint8_t value = rtc_read(0, reg);
    rtc_end(0);
    return value;
}
uint8_t fs_floppy_cmos_types(void) { return fs_cmos_read(0x10); }
uint8_t fs_cmos_bios_data(uint8_t index, bool write, uint8_t value)
{
    rtc_begin(0);
    rtc_select(index); /* preserve firmware's NMI-disable bit during the cycle */
    if (write) outb(0x71, value);
    else value = inb(0x71);
    rtc_end(0);
    return value;
}
