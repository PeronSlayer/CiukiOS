#include "keyboard.h"
#include "serial.h"

#define PIC1_CMD       0x20
#define PIC_EOI        0x20
#define KBD_DATA_PORT  0x60
#define KBD_STAT_PORT  0x64
#define KBD_OBF_MASK   0x01

#define SHIFT_LEFT_BIT   0x01
#define SHIFT_RIGHT_BIT  0x02

#define KEYBUF_SIZE 128
#define KEYBUF_MASK (KEYBUF_SIZE - 1)

static volatile u64 g_keyboard_irq_count = 0;
static volatile u8 g_shift_state = 0;
static volatile u8 g_extended_prefix = 0;
static volatile u8 g_keybuf[KEYBUF_SIZE];
static volatile u16 g_keybuf_head = 0;
static volatile u16 g_keybuf_tail = 0;

static const u8 k_set1_ascii[128] = {
    [0x01] = 0x1B,
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x39] = ' ',
};

static const u8 k_set1_ascii_shift[128] = {
    [0x01] = 0x1B,
    [0x02] = '!',
    [0x03] = '@',
    [0x04] = '#',
    [0x05] = '$',
    [0x06] = '%',
    [0x07] = '^',
    [0x08] = '&',
    [0x09] = '*',
    [0x0A] = '(',
    [0x0B] = ')',
    [0x0C] = '_',
    [0x0D] = '+',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'Q',
    [0x11] = 'W',
    [0x12] = 'E',
    [0x13] = 'R',
    [0x14] = 'T',
    [0x15] = 'Y',
    [0x16] = 'U',
    [0x17] = 'I',
    [0x18] = 'O',
    [0x19] = 'P',
    [0x1A] = '{',
    [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A',
    [0x1F] = 'S',
    [0x20] = 'D',
    [0x21] = 'F',
    [0x22] = 'G',
    [0x23] = 'H',
    [0x24] = 'J',
    [0x25] = 'K',
    [0x26] = 'L',
    [0x27] = ':',
    [0x28] = '"',
    [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z',
    [0x2D] = 'X',
    [0x2E] = 'C',
    [0x2F] = 'V',
    [0x30] = 'B',
    [0x31] = 'N',
    [0x32] = 'M',
    [0x33] = '<',
    [0x34] = '>',
    [0x35] = '?',
    [0x39] = ' ',
};

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

static void keybuf_push(u8 ch) {
    u16 head = g_keybuf_head;
    u16 tail = g_keybuf_tail;
    u16 next = (u16)((head + 1) & KEYBUF_MASK);

    if (next == tail) {
        tail = (u16)((tail + 1) & KEYBUF_MASK);
        g_keybuf_tail = tail;
    }

    g_keybuf[head] = ch;
    g_keybuf_head = next;
}

static i32 keybuf_pop(void) {
    u16 head = g_keybuf_head;
    u16 tail = g_keybuf_tail;

    if (head == tail) {
        return -1;
    }

    u8 ch = g_keybuf[tail];
    g_keybuf_tail = (u16)((tail + 1) & KEYBUF_MASK);
    return (i32)ch;
}

static u8 set1_decode_to_ascii(u8 scancode) {
    u8 code = 0;
    u8 is_break = 0;
    u8 shift_active = 0;

    if (scancode == 0xE0) {
        g_extended_prefix = 1;
        return 0;
    }

    is_break = (u8)((scancode & 0x80U) != 0);
    code = (u8)(scancode & 0x7FU);

    if (code == 0x2A) {
        if (is_break) {
            g_shift_state = (u8)(g_shift_state & ~SHIFT_LEFT_BIT);
        } else {
            g_shift_state = (u8)(g_shift_state | SHIFT_LEFT_BIT);
        }
        g_extended_prefix = 0;
        return 0;
    }

    if (code == 0x36) {
        if (is_break) {
            g_shift_state = (u8)(g_shift_state & ~SHIFT_RIGHT_BIT);
        } else {
            g_shift_state = (u8)(g_shift_state | SHIFT_RIGHT_BIT);
        }
        g_extended_prefix = 0;
        return 0;
    }

    if (is_break) {
        g_extended_prefix = 0;
        return 0;
    }

    if (g_extended_prefix) {
        g_extended_prefix = 0;

        if (code == 0x48) {
            return STAGE2_KEY_UP;
        }

        if (code == 0x50) {
            return STAGE2_KEY_DOWN;
        }

        if (code == 0x4B) {
            return STAGE2_KEY_LEFT;
        }

        if (code == 0x4D) {
            return STAGE2_KEY_RIGHT;
        }

        if (code == 0x1C) {
            return '\n';
        }

        if (code == 0x35) {
            return '/';
        }

        return 0;
    }

    shift_active = (u8)(g_shift_state != 0);
    if (shift_active) {
        return k_set1_ascii_shift[code];
    }

    return k_set1_ascii[code];
}

void stage2_keyboard_init(void) {
    g_keyboard_irq_count = 0;
    g_shift_state = 0;
    g_extended_prefix = 0;
    g_keybuf_head = 0;
    g_keybuf_tail = 0;

    for (u8 i = 0; i < 32; i++) {
        u8 status = inb(KBD_STAT_PORT);
        if ((status & KBD_OBF_MASK) == 0) {
            break;
        }
        (void)inb(KBD_DATA_PORT);
    }
}

void stage2_keyboard_on_irq1(void) {
    u8 scancode = inb(KBD_DATA_PORT);
    u8 ascii = 0;

    g_keyboard_irq_count++;
    ascii = set1_decode_to_ascii(scancode);
    if (ascii != 0) {
        keybuf_push(ascii);
    }

    if (g_keyboard_irq_count <= 4 || (g_keyboard_irq_count % 32ULL) == 0) {
        serial_write("[ key ] scancode=0x");
        serial_write_hex8(scancode);
        if (ascii != 0) {
            serial_write(" ascii=0x");
            serial_write_hex8(ascii);
        }
        serial_write(" irq1#");
        serial_write_hex64(g_keyboard_irq_count);
        serial_write("\n");
    }

    outb(PIC1_CMD, PIC_EOI);
}

u64 stage2_keyboard_irq_count(void) {
    return g_keyboard_irq_count;
}

i32 stage2_keyboard_getc_nonblocking(void) {
    u64 flags = irq_save();
    i32 ch = keybuf_pop();
    irq_restore(flags);
    return ch;
}

u8 stage2_keyboard_getc_blocking(void) {
    for (;;) {
        i32 ch = stage2_keyboard_getc_nonblocking();
        if (ch >= 0) {
            return (u8)ch;
        }
        __asm__ volatile ("hlt");
    }
}

void stage2_keyboard_flush_buffer(void) {
    u64 flags = irq_save();
    g_keybuf_head = 0;
    g_keybuf_tail = 0;
    irq_restore(flags);
}
