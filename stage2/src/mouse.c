/*
 * SR-MOUSE-001 follow-up — PS/2 mouse driver (IRQ12) for CiukiOS stage2.
 *
 * Exposes a narrow delta+buttons interface consumed by the INT 33h
 * dispatcher in shell.c. The driver is deliberately small and
 * tolerant: if the 8042 controller does not acknowledge any of the
 * enable steps (which is normal under headless QEMU with no PS/2
 * mouse), init quietly leaves the driver in a zero-motion, zero-button
 * fallback. IRQ12 handling is idempotent with respect to that
 * fallback.
 */

#include "mouse.h"
#include "serial.h"

#define PIC1_CMD         0x20U
#define PIC1_DATA        0x21U
#define PIC2_CMD         0xA0U
#define PIC2_DATA        0xA1U
#define PIC_EOI          0x20U

#define PS2_DATA_PORT    0x60U
#define PS2_STATUS_PORT  0x64U
#define PS2_CMD_PORT     0x64U

#define PS2_STATUS_OBF   0x01U  /* output buffer full (data ready) */
#define PS2_STATUS_IBF   0x02U  /* input buffer full (do not write) */
#define PS2_STATUS_AUX   0x20U  /* byte is from aux (mouse) device */

#define PS2_CMD_ENABLE_AUX       0xA8U
#define PS2_CMD_WRITE_AUX_NEXT   0xD4U
#define PS2_CMD_READ_CONFIG      0x20U
#define PS2_CMD_WRITE_CONFIG     0x60U

#define PS2_CFG_INT2           0x02U  /* bit1: aux IRQ enabled */
#define PS2_CFG_DISABLE_AUX    0x20U  /* bit5: aux disabled */

#define PS2_AUX_SET_DEFAULTS   0xF6U
#define PS2_AUX_ENABLE_REPORT  0xF4U
#define PS2_AUX_ACK            0xFAU

#define WAIT_SPIN_LIMIT   100000U

static volatile i32 g_mouse_dx = 0;
static volatile i32 g_mouse_dy = 0;
static volatile u16 g_mouse_buttons = 0;
static volatile u64 g_mouse_irq_count = 0;
static volatile u8  g_mouse_present = 0;

/* 3-byte packet state machine. Index 0/1/2; resync if index==0 byte lacks bit3. */
static volatile u8  g_pkt_index = 0;
static volatile u8  g_pkt[3] = {0, 0, 0};

static inline void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline u64 irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(u64 flags) {
    if ((flags & (1ULL << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static int ps2_wait_write(void) {
    for (u32 i = 0; i < WAIT_SPIN_LIMIT; i++) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_IBF) == 0U) {
            return 1;
        }
    }
    return 0;
}

static int ps2_wait_read(void) {
    for (u32 i = 0; i < WAIT_SPIN_LIMIT; i++) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) != 0U) {
            return 1;
        }
    }
    return 0;
}

static int ps2_send_aux_cmd(u8 cmd) {
    if (!ps2_wait_write()) return 0;
    outb(PS2_CMD_PORT, PS2_CMD_WRITE_AUX_NEXT);
    if (!ps2_wait_write()) return 0;
    outb(PS2_DATA_PORT, cmd);
    if (!ps2_wait_read()) return 0;
    return (inb(PS2_DATA_PORT) == PS2_AUX_ACK) ? 1 : 0;
}

static void unmask_irq12(void) {
    /* Clear PIC2 mask bit4 (IRQ12) and PIC1 mask bit2 (IRQ2 cascade). */
    u8 m1 = inb(PIC1_DATA);
    u8 m2 = inb(PIC2_DATA);
    m1 = (u8)(m1 & ~0x04U);
    m2 = (u8)(m2 & ~0x10U);
    outb(PIC1_DATA, m1);
    outb(PIC2_DATA, m2);
}

int stage2_mouse_init(void) {
    g_mouse_dx = 0;
    g_mouse_dy = 0;
    g_mouse_buttons = 0;
    g_mouse_irq_count = 0;
    g_mouse_present = 0;
    g_pkt_index = 0;

    /* Flush any stale data from OBF. */
    for (u32 i = 0; i < 16U; i++) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) == 0U) break;
        (void)inb(PS2_DATA_PORT);
    }

    /* Enable auxiliary device. */
    if (!ps2_wait_write()) {
        serial_write("[mouse] init fail step=enable_aux phase=ibf\n");
        return 0;
    }
    outb(PS2_CMD_PORT, PS2_CMD_ENABLE_AUX);

    /* Read config byte. */
    if (!ps2_wait_write()) {
        serial_write("[mouse] init fail step=read_cfg phase=ibf\n");
        return 0;
    }
    outb(PS2_CMD_PORT, PS2_CMD_READ_CONFIG);
    if (!ps2_wait_read()) {
        serial_write("[mouse] init fail step=read_cfg phase=obf\n");
        return 0;
    }
    u8 cfg = inb(PS2_DATA_PORT);
    cfg = (u8)(cfg | PS2_CFG_INT2);       /* enable IRQ12 */
    cfg = (u8)(cfg & ~PS2_CFG_DISABLE_AUX); /* clear aux-disabled */

    /* Write config back. */
    if (!ps2_wait_write()) {
        serial_write("[mouse] init fail step=write_cfg phase=ibf\n");
        return 0;
    }
    outb(PS2_CMD_PORT, PS2_CMD_WRITE_CONFIG);
    if (!ps2_wait_write()) {
        serial_write("[mouse] init fail step=write_cfg phase=data\n");
        return 0;
    }
    outb(PS2_DATA_PORT, cfg);

    /* Reset mouse to defaults, then enable data reporting. */
    if (!ps2_send_aux_cmd(PS2_AUX_SET_DEFAULTS)) {
        serial_write("[mouse] init fail step=set_defaults\n");
        return 0;
    }
    if (!ps2_send_aux_cmd(PS2_AUX_ENABLE_REPORT)) {
        serial_write("[mouse] init fail step=enable_report\n");
        return 0;
    }

    unmask_irq12();
    g_mouse_present = 1;
    serial_write("[mouse] init ok ps2 aux enabled irq12 unmasked\n");
    return 1;
}

void stage2_mouse_on_irq12(void) {
    /*
     * Acknowledge regardless of packet completion so we never leave
     * PIC2/PIC1 latched on a malformed stream.
     */
    u8 status = inb(PS2_STATUS_PORT);
    u8 byte;

    g_mouse_irq_count++;

    if ((status & PS2_STATUS_OBF) == 0U) {
        /* No data — spurious IRQ, still EOI both PICs. */
        outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
        return;
    }

    byte = inb(PS2_DATA_PORT);

    /*
     * The byte is only ours if bit5 of status was set. If it's keyboard
     * data that slipped through, drop it and re-sync on next IRQ.
     */
    if ((status & PS2_STATUS_AUX) == 0U) {
        outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
        return;
    }

    if (g_pkt_index == 0U) {
        /*
         * First byte must have bit3 set (always-1 in the standard
         * packet). If not, drop and retry to re-sync.
         */
        if ((byte & 0x08U) == 0U) {
            outb(PIC2_CMD, PIC_EOI);
            outb(PIC1_CMD, PIC_EOI);
            return;
        }
    }

    g_pkt[g_pkt_index] = byte;
    g_pkt_index = (u8)(g_pkt_index + 1U);

    if (g_pkt_index >= 3U) {
        u8 b0 = g_pkt[0];
        u8 b1 = g_pkt[1];
        u8 b2 = g_pkt[2];
        i32 dx = (i32)b1;
        i32 dy = (i32)b2;

        /* Sign-extend via the X/Y sign bits in byte 0 (bits 4 and 5). */
        if (b0 & 0x10U) dx -= 256;
        if (b0 & 0x20U) dy -= 256;

        /* PS/2 Y is positive = up; DOS/screen Y is positive = down.
         * Invert so accumulated motion matches screen-space semantics. */
        dy = -dy;

        /*
         * If an axis overflow bit is set (bit 6 / 7 of byte 0), the
         * packet is unreliable: drop it rather than propagating
         * garbage. Level button state is still updated so the UI
         * does not get stuck with a phantom press.
         */
        int overflow = (b0 & 0xC0U) ? 1 : 0;

        u16 buttons = 0U;
        if (b0 & 0x01U) buttons |= STAGE2_MOUSE_BTN_LEFT;
        if (b0 & 0x02U) buttons |= STAGE2_MOUSE_BTN_RIGHT;
        if (b0 & 0x04U) buttons |= STAGE2_MOUSE_BTN_MIDDLE;
        g_mouse_buttons = buttons;

        if (!overflow) {
            g_mouse_dx += dx;
            g_mouse_dy += dy;
        }

        g_pkt_index = 0U;

        if (g_mouse_irq_count <= 3ULL || (g_mouse_irq_count % 256ULL) == 0ULL) {
            serial_write("[mouse] irq12 pkt ok irq#");
            serial_write_hex64(g_mouse_irq_count);
            serial_write("\n");
        }
    }

    outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void stage2_mouse_consume_deltas(i32 *out_dx, i32 *out_dy, u16 *out_buttons) {
    u64 flags = irq_save();
    i32 dx = g_mouse_dx;
    i32 dy = g_mouse_dy;
    u16 btn = g_mouse_buttons;
    g_mouse_dx = 0;
    g_mouse_dy = 0;
    irq_restore(flags);

    if (out_dx)      *out_dx = dx;
    if (out_dy)      *out_dy = dy;
    if (out_buttons) *out_buttons = btn;
}

u64 stage2_mouse_irq_count(void) {
    return g_mouse_irq_count;
}

int stage2_mouse_is_present(void) {
    return g_mouse_present ? 1 : 0;
}
