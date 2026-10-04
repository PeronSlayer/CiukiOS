#include "guest_peripheral_scheduler.h"
#include "guest_opl_dbopl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    std::fprintf(stderr, "check %u failed at %s:%d: %s\n", checks, __FILE__, __LINE__, #x); \
    std::exit(1); } } while (0)

struct fixture {
    std::vector<uint8_t> memory;
    cvgp_dbopl *opl;
};

static uint8_t memory_read(void *opaque, uint32_t physical, int *valid)
{
    fixture *f = static_cast<fixture *>(opaque);
    *valid = physical < f->memory.size();
    return *valid ? f->memory[physical] : 0;
}

static void opl_reset(void *opaque, unsigned rate)
{
    fixture *f = static_cast<fixture *>(opaque);
    cvgp_dbopl_reset(f->opl, rate);
}

static void opl_write(void *opaque, uint16_t reg, uint8_t value)
{
    fixture *f = static_cast<fixture *>(opaque);
    cvgp_dbopl_write(f->opl, reg, value);
}

static int opl_generate(void *opaque, int16_t *stereo, unsigned frames)
{
    fixture *f = static_cast<fixture *>(opaque);
    return cvgp_dbopl_generate(f->opl, stereo, frames);
}

static void outb(cvgp_state &s, uint32_t owner, uint16_t port, uint8_t value)
{
    CHECK(cvgp_io_write(&s, owner, port, 1, 0, value) == CVGP_IO_OK);
}

static uint8_t inb(cvgp_state &s, uint32_t owner, uint16_t port)
{
    uint32_t value = 0;
    CHECK(cvgp_io_read(&s, owner, port, 1, 0, &value) == CVGP_IO_OK);
    return static_cast<uint8_t>(value);
}

static void eoi(cvgp_state &s, uint32_t owner, unsigned irq)
{
    if (irq >= 8) outb(s, owner, 0xa0, 0x20);
    outb(s, owner, 0x20, 0x20);
}

static std::vector<uint8_t> drain_kbc(cvgp_state &s, uint32_t owner)
{
    std::vector<uint8_t> result;
    while (inb(s, owner, 0x64) & 1) result.push_back(inb(s, owner, 0x60));
    return result;
}

static void dsp(cvgp_state &s, uint32_t owner, uint8_t value)
{
    outb(s, owner, static_cast<uint16_t>(s.sb.base + 0x0c), value);
}

static void test_ownership(cvgp_state &s, cvgp_backend &backend)
{
    cvgp_state missing;
    cvgp_init(&missing);
    CHECK(cvgp_begin(&missing, 1, CVGP_CAP_OPL3, 0, &backend) == CVGP_ERR_MISSING_DEVICE);
    CHECK(std::strstr(cvgp_error_message(missing.last_error), "unavailable") != 0);
    CHECK(cvgp_begin(&s, 0x11223344UL, 0x1f, 0x1f, &backend) == CVGP_OK);
    CHECK(cvgp_begin(&s, 9, CVGP_CAP_KEYBOARD, 0x1f, &backend) == CVGP_ERR_ACTIVE);
    CHECK(cvgp_key_event(&s, 9, 0x1d, 0, 1) == CVGP_ERR_OWNER);
    CHECK(cvgp_port_claimed(&s, 0x60));
    CHECK(cvgp_port_claimed(&s, 0x220));
    CHECK(cvgp_port_claimed(&s, 0x388));
    CHECK(!cvgp_port_claimed(&s, 0x3f8));
}

static void test_keyboard_focus(cvgp_state &s, uint32_t owner)
{
    uint8_t vector;
    std::vector<uint8_t> bytes;
    CHECK(cvgp_set_focus(&s, owner, 1) == CVGP_OK);
    CHECK(cvgp_key_event(&s, owner, 0x1d, 0, 1) == CVGP_OK);  /* left Ctrl */
    CHECK(cvgp_key_event(&s, owner, 0x39, 0, 1) == CVGP_OK);  /* Space */
    CHECK(cvgp_key_event(&s, owner, 0x1d, 1, 1) == CVGP_OK);  /* right Ctrl */
    CHECK(cvgp_key_event(&s, owner, 0x2a, 0, 1) == CVGP_OK);  /* left Shift */
    CHECK(cvgp_key_event(&s, owner, 0x38, 0, 1) == CVGP_OK);  /* left Alt */
    CHECK(cvgp_irq_acknowledge(&s, owner, 1, &vector) && vector == 9);
    eoi(s, owner, 1);
    bytes = drain_kbc(s, owner);
    CHECK(bytes.size() == 6);
    CHECK(bytes[0] == 0x1d && bytes[1] == 0x39 && bytes[2] == 0xe0 &&
          bytes[3] == 0x1d && bytes[4] == 0x2a && bytes[5] == 0x38);

    CHECK(cvgp_set_focus(&s, owner, 0) == CVGP_OK);
    bytes = drain_kbc(s, owner);
    CHECK(bytes.size() == 6);
    CHECK(bytes[0] == 0x9d && bytes[1] == 0xaa && bytes[2] == 0xb8 &&
          bytes[3] == 0xb9 && bytes[4] == 0xe0 && bytes[5] == 0x9d);
    for (unsigned i = 0; i != sizeof(s.key_down); ++i) CHECK(!s.key_down[i]);

    for (unsigned i = 0; i != 64; ++i) {
        CHECK(cvgp_set_focus(&s, owner, 1) == CVGP_OK);
        CHECK(cvgp_key_event(&s, owner, 0x1d, 0, 1) == CVGP_OK);
        (void)drain_kbc(s, owner);
        CHECK(cvgp_set_focus(&s, owner, 0) == CVGP_OK);
        bytes = drain_kbc(s, owner);
        CHECK(bytes.size() == 1 && bytes[0] == 0x9d);
    }

    /* A key typed while focused and not read yet stays the guest's. */
    CHECK(cvgp_set_focus(&s, owner, 1) == CVGP_OK);
    CHECK(cvgp_key_event(&s, owner, 0x01, 0, 1) == CVGP_OK);  /* Esc */
    CHECK(cvgp_key_event(&s, owner, 0x01, 0, 0) == CVGP_OK);
    CHECK(cvgp_key_event(&s, owner, 0x1d, 0, 1) == CVGP_OK);  /* held Ctrl */
    CHECK(cvgp_set_focus(&s, owner, 0) == CVGP_OK);
    bytes = drain_kbc(s, owner);
    CHECK(bytes.size() == 4);
    CHECK(bytes[0] == 0x01 && bytes[1] == 0x81 && bytes[2] == 0x1d &&
          bytes[3] == 0x9d);
}

static void test_mouse(cvgp_state &s, uint32_t owner)
{
    std::vector<uint8_t> bytes;
    uint8_t vector;
    CHECK(cvgp_set_focus(&s, owner, 1) == CVGP_OK);
    /* A forked DOS VM can inherit an already-initialized INT 33h driver;
     * the model must accept its explicit desktop event before F4 replay. */
    CHECK(cvgp_mouse_event(&s, owner, 3, -2, 0, 0) == CVGP_OK);
    bytes = drain_kbc(s, owner);
    CHECK(bytes.size() == 3 && bytes[1] == 3 &&
          bytes[2] == static_cast<uint8_t>(-2));
    outb(s, owner, 0x64, 0xd4);
    outb(s, owner, 0x60, 0xf4);
    bytes = drain_kbc(s, owner);
    CHECK(bytes.size() == 1 && bytes[0] == 0xfa);
    CHECK(cvgp_mouse_event(&s, owner, 17, -9, 3, 0) == CVGP_OK);
    CHECK(cvgp_irq_acknowledge(&s, owner, 1, &vector) && vector == 0x74);
    eoi(s, owner, 12);
    CHECK(inb(s, owner, 0x64) & 0x20);
    bytes = drain_kbc(s, owner);
    CHECK(bytes.size() == 3);
    CHECK((bytes[0] & 0x3f) == 0x2b);
    CHECK(bytes[1] == 17 && bytes[2] == static_cast<uint8_t>(-9));
    CHECK(cvgp_set_focus(&s, owner, 0) == CVGP_OK);
    bytes = drain_kbc(s, owner);
    CHECK(bytes.size() == 3 && (bytes[0] & 7) == 0);

    /* IntelliMouse negotiates with sample rates 200, 100, 80 and reports ID 3. */
    CHECK(cvgp_set_focus(&s, owner, 1) == CVGP_OK);
    const uint8_t rates[] = {200, 100, 80};
    for (unsigned i = 0; i < 3; ++i) {
        outb(s, owner, 0x64, 0xd4); outb(s, owner, 0x60, 0xf3);
        bytes = drain_kbc(s, owner); CHECK(bytes.size() == 1 && bytes[0] == 0xfa);
        outb(s, owner, 0x60, rates[i]);
        bytes = drain_kbc(s, owner); CHECK(bytes.size() == 1 && bytes[0] == 0xfa);
    }
    outb(s, owner, 0x64, 0xd4); outb(s, owner, 0x60, 0xf2);
    bytes = drain_kbc(s, owner);
    CHECK(bytes.size() == 2 && bytes[0] == 0xfa && bytes[1] == 3);
    CHECK(cvgp_mouse_event(&s, owner, 0, 0, 4, -2) == CVGP_OK);
    bytes = drain_kbc(s, owner);
    CHECK(bytes.size() == 4 && (bytes[0] & 7) == 4 && bytes[3] == 0xfe);

    /* Failed capacity preflight cannot leave a partial packet in the queue. */
    s.kbc_count = CVGP_KBC_QUEUE_BYTES - 2;
    CHECK(cvgp_mouse_event(&s, owner, 1, 1, 4, 1) == CVGP_ERR_QUEUE_FULL);
    CHECK(s.kbc_count == CVGP_KBC_QUEUE_BYTES - 2);
    s.kbc_count = 0;
    s.kbc_read = s.kbc_write = 0;

    outb(s, owner, 0x64, 0xd4); outb(s, owner, 0x60, 0xf6);
    bytes = drain_kbc(s, owner);
    CHECK(bytes.size() == 1 && bytes[0] == 0xfa && s.mouse_id == 0);
    CHECK(cvgp_mouse_event(&s, owner, 2, -1, 4, 1) == CVGP_OK);
    bytes = drain_kbc(s, owner);
    CHECK(bytes.size() == 3 && bytes[1] == 2 && bytes[2] == 0xff);
}

static void test_pic_pit(cvgp_state &s, uint32_t owner)
{
    uint8_t vector = 0;
    outb(s, owner, 0x43, 0x34);       /* channel 0, lo/hi, mode 2 */
    outb(s, owner, 0x40, 0xe8);
    outb(s, owner, 0x40, 0x03);       /* 1000 PIT clocks */
    CHECK(!(cvgp_advance(&s, owner, 999) & CVGP_SERVICE_IRQ));
    CHECK(cvgp_advance(&s, owner, 1) & CVGP_SERVICE_IRQ);
    CHECK(!cvgp_irq_acknowledge(&s, owner, 0, &vector));
    CHECK(cvgp_irq_acknowledge(&s, owner, 1, &vector) && vector == 8);
    CHECK(cvgp_set_focus(&s, owner, 1) == CVGP_OK);
    CHECK(cvgp_key_event(&s, owner, 0x1e, 0, 1) == CVGP_OK);
    CHECK(!cvgp_irq_acknowledge(&s, owner, 1, &vector)); /* IRQ0 in service */
    eoi(s, owner, 0);
    CHECK(cvgp_irq_acknowledge(&s, owner, 1, &vector) && vector == 9);
    CHECK(inb(s, owner, 0x60) == 0x1e);
    eoi(s, owner, 1);
    CHECK(cvgp_key_event(&s, owner, 0x1e, 0, 0) == CVGP_OK);
    CHECK(inb(s, owner, 0x60) == 0x9e);
    CHECK(cvgp_advance(&s, owner, 4000) & CVGP_SERVICE_IRQ);
    CHECK(cvgp_irq_acknowledge(&s, owner, 1, &vector) && vector == 8);
    eoi(s, owner, 0);

    CHECK(cvgp_io_write(&s, owner, 0x43, 1, 0, 0x38) == CVGP_IO_REJECTED); /* unsupported mode 4 */
    CHECK(s.last_error == CVGP_ERR_UNSUPPORTED_PIT);
    CHECK(std::strstr(cvgp_error_message(s.last_error), "timer") != 0);
}

static uint32_t pit_irq0_pending(cvgp_state &s, uint32_t owner)
{
    uint32_t count = 0;
    CHECK(cvgp_pit_irq0_pending(&s, owner, &count) == CVGP_OK);
    return count;
}

static void program_pit0(cvgp_state &s, uint32_t owner, uint8_t control,
                         uint16_t reload)
{
    outb(s, owner, 0x43, control);
    outb(s, owner, 0x40, static_cast<uint8_t>(reload));
    outb(s, owner, 0x40, static_cast<uint8_t>(reload >> 8));
}

static void test_pit_expiry_debt(void)
{
    cvgp_state s;
    const uint32_t owner = 0x50495431UL;
    uint32_t pending = 0;
    cvgp_init(&s);
    CHECK(cvgp_begin(&s, owner, CVGP_CAP_PIC_PIT, CVGP_CAP_PIC_PIT, 0) == CVGP_OK);
    CHECK(cvgp_pit_irq0_pending(&s, owner, &pending) == CVGP_OK && pending == 0);
    CHECK(cvgp_pit_irq0_consume(&s, owner) == 0); /* empty queue */

    /* Mode 0 expires once even when the host advance spans more clocks. */
    program_pit0(s, owner, 0x30, 1000);
    cvgp_advance(&s, owner, 999);
    CHECK(pit_irq0_pending(s, owner) == 0);
    cvgp_advance(&s, owner, 10001);
    CHECK(pit_irq0_pending(s, owner) == 1);
    CHECK(s.pit[0].running == 0 && s.pit[0].remaining == 0);
    CHECK(cvgp_pit_irq0_consume(&s, owner) == 1);
    CHECK(cvgp_pit_irq0_consume(&s, owner) == 0);

    /* Mode 2 counts each period crossed and keeps the exact residual phase. */
    program_pit0(s, owner, 0x34, 1000);
    cvgp_advance(&s, owner, 3500);
    CHECK(pit_irq0_pending(s, owner) == 3);
    CHECK(s.pit[0].remaining == 500);
    CHECK(cvgp_pit_irq0_consume(&s, owner) == 1);
    CHECK(cvgp_pit_irq0_consume(&s, owner) == 1);
    CHECK(pit_irq0_pending(s, owner) == 1);

    /* Reprogramming the counter does not withdraw already matured IRQs. */
    program_pit0(s, owner, 0x36, 7);
    CHECK(pit_irq0_pending(s, owner) == 1);
    cvgp_advance(&s, owner, 7);
    CHECK(pit_irq0_pending(s, owner) == 2);
    CHECK(cvgp_pit_irq0_consume(&s, owner) == 1);
    CHECK(cvgp_pit_irq0_consume(&s, owner) == 1);
    CHECK(cvgp_pit_irq0_consume(&s, owner) == 0);

    /* Mode 3 shares periodic terminal-count accounting and residual phase. */
    program_pit0(s, owner, 0x36, 10);
    cvgp_advance(&s, owner, 25);
    CHECK(pit_irq0_pending(s, owner) == 2);
    CHECK(s.pit[0].remaining == 5);
    CHECK(cvgp_pit_irq0_consume(&s, owner) == 1);
    CHECK(cvgp_pit_irq0_consume(&s, owner) == 1);
    CHECK(pit_irq0_pending(s, owner) == 0);

    /* A maximum 32-bit elapsed-clock batch is O(1), exact, and saturating. */
    program_pit0(s, owner, 0x34, 1);
    cvgp_advance(&s, owner, 0xffffffffUL);
    CHECK(pit_irq0_pending(s, owner) == 0xffffffffUL);
    cvgp_advance(&s, owner, 1);
    CHECK(pit_irq0_pending(s, owner) == 0xffffffffUL);
    CHECK(cvgp_pit_irq0_consume(&s, owner) == 1);
    CHECK(pit_irq0_pending(s, owner) == 0xfffffffeUL);

    CHECK(cvgp_pit_irq0_pending(&s, owner + 1, &pending) == CVGP_ERR_OWNER);
    CHECK(cvgp_pit_irq0_consume(&s, owner + 1) == CVGP_ERR_OWNER);
    CHECK(cvgp_pit_irq0_pending(&s, owner, 0) == CVGP_ERR_ARGUMENT);
    CHECK(cvgp_end(&s, owner) == CVGP_OK);
}

static void test_pit_readback(void)
{
    cvgp_state s;
    cvgp_state interleaved;
    const uint32_t owner = 0x50495432UL;
    cvgp_init(&s);
    CHECK(cvgp_begin(&s, owner, CVGP_CAP_PIC_PIT, CVGP_CAP_PIC_PIT, 0) == CVGP_OK);

    program_pit0(s, owner, 0x34, 1000);
    cvgp_advance(&s, owner, 123);
    outb(s, owner, 0x43, 0xc2);       /* read-back count + status, channel 0 */
    cvgp_advance(&s, owner, 10);
    outb(s, owner, 0x43, 0xc2);       /* unread latches must not be overwritten */
    CHECK(inb(s, owner, 0x40) == 0xb4); /* status is returned before count */
    CHECK(inb(s, owner, 0x40) == 0x6d); /* captured 877, not live 867 */
    CHECK(inb(s, owner, 0x40) == 0x03);
    CHECK(inb(s, owner, 0x40) == 0x63); /* live count resumes after latch read */
    CHECK(inb(s, owner, 0x40) == 0x03);

    /* Null Count remains set until both bytes of a word count arrive. */
    outb(s, owner, 0x43, 0x34);
    outb(s, owner, 0x43, 0xe2);       /* status-only read-back */
    CHECK(inb(s, owner, 0x40) == 0xf4); /* OUT=1, NULL=1, RW=3, mode=2 */
    outb(s, owner, 0x40, 0xe8);
    outb(s, owner, 0x43, 0xe2);
    CHECK(inb(s, owner, 0x40) == 0xf4);
    outb(s, owner, 0x40, 0x03);
    outb(s, owner, 0x43, 0xe2);
    CHECK(inb(s, owner, 0x40) == 0xb4); /* count load clears NULL */

    /* Read-back can select counters 0, 1 and 2 together. */
    outb(s, owner, 0x43, 0x74);       /* channel 1, mode 2 */
    outb(s, owner, 0x41, 20);
    outb(s, owner, 0x41, 0);
    outb(s, owner, 0x43, 0xb6);       /* channel 2, mode 3 */
    outb(s, owner, 0x42, 10);
    outb(s, owner, 0x42, 0);
    cvgp_advance(&s, owner, 3);
    outb(s, owner, 0x43, 0xce);       /* count + status, all counters */
    CHECK(inb(s, owner, 0x40) == 0xb4);
    CHECK(inb(s, owner, 0x40) == 0xe5); /* channel 0 elapsed three clocks */
    CHECK(inb(s, owner, 0x40) == 0x03);
    CHECK(inb(s, owner, 0x41) == 0xb4);
    CHECK(inb(s, owner, 0x41) == 17);
    CHECK(inb(s, owner, 0x41) == 0);
    CHECK(inb(s, owner, 0x42) == 0xb6);
    CHECK(inb(s, owner, 0x42) == 4);  /* mode 3: N - (2 * elapsed mod N) */
    CHECK(inb(s, owner, 0x42) == 0);

    /* Mode 3 status reflects its low half-cycle; its count follows QEMU's
     * documented formula (including QEMU's odd-reload approximation). */
    program_pit0(s, owner, 0x36, 10);
    cvgp_advance(&s, owner, 1);
    outb(s, owner, 0x43, 0x00);       /* counter-latch command, channel 0 */
    CHECK(inb(s, owner, 0x40) == 8);
    CHECK(inb(s, owner, 0x40) == 0);
    cvgp_advance(&s, owner, 4);
    outb(s, owner, 0x43, 0xe2);
    CHECK(inb(s, owner, 0x40) == 0x36); /* OUT=0, NULL=0, RW=3, mode=3 */

    CHECK(cvgp_end(&s, owner) == CVGP_OK);

    /* The count-read latch and partial count-write latch are independent.
     * Loading the new count must also leave the earlier read snapshot intact. */
    {
        const uint32_t interleaved_owner = 0x50495433UL;
        cvgp_init(&interleaved);
        CHECK(cvgp_begin(&interleaved, interleaved_owner,
                         CVGP_CAP_PIC_PIT, CVGP_CAP_PIC_PIT, 0) == CVGP_OK);
        outb(interleaved, interleaved_owner, 0x43, 0x34); /* mode 2, word */
        outb(interleaved, interleaved_owner, 0x40, 0x34); /* partial write */
        outb(interleaved, interleaved_owner, 0x43, 0x00); /* latch old count */
        outb(interleaved, interleaved_owner, 0x40, 0x12); /* finish new count */
        CHECK(inb(interleaved, interleaved_owner, 0x40) == 0x00);
        CHECK(inb(interleaved, interleaved_owner, 0x40) == 0x00);
        CHECK(inb(interleaved, interleaved_owner, 0x40) == 0x34);
        CHECK(inb(interleaved, interleaved_owner, 0x40) == 0x12);
        CHECK(cvgp_end(&interleaved, interleaved_owner) == CVGP_OK);
    }
}

static void program_dma1(cvgp_state &s, uint32_t owner, uint32_t address,
                         uint16_t count_minus_one, int auto_init)
{
    outb(s, owner, 0x0c, 0);
    outb(s, owner, 0x02, static_cast<uint8_t>(address));
    outb(s, owner, 0x02, static_cast<uint8_t>(address >> 8));
    outb(s, owner, 0x03, static_cast<uint8_t>(count_minus_one));
    outb(s, owner, 0x03, static_cast<uint8_t>(count_minus_one >> 8));
    outb(s, owner, 0x83, static_cast<uint8_t>(address >> 16));
    outb(s, owner, 0x0b, static_cast<uint8_t>(0x49 | (auto_init ? 0x10 : 0)));
    outb(s, owner, 0x0a, 1);
}

static void test_sb_dma(cvgp_state &s, uint32_t owner, fixture &f,
                        std::vector<int16_t> &capture)
{
    const uint32_t physical = 0x12340;
    uint8_t vector;
    f.memory[physical] = 0;
    f.memory[physical + 1] = 64;
    f.memory[physical + 2] = 192;
    f.memory[physical + 3] = 255;
    outb(s, owner, 0x226, 1);
    outb(s, owner, 0x226, 0);
    CHECK(inb(s, owner, 0x22e) & 0x80);
    CHECK(inb(s, owner, 0x22a) == 0xaa);
    dsp(s, owner, 0xe1);
    CHECK(inb(s, owner, 0x22a) == 4 && inb(s, owner, 0x22a) == 5);
    program_dma1(s, owner, physical, 3, 0);
    dsp(s, owner, 0xd3);              /* SB16: speaker off does not gate the DAC */
    dsp(s, owner, 0x41); dsp(s, owner, 0x1f); dsp(s, owner, 0x40); /* 8000 Hz */
    dsp(s, owner, 0x14); dsp(s, owner, 3); dsp(s, owner, 0);
    capture.assign(128 * 2, 0);
    CHECK(cvgp_render_audio(&s, owner, &capture[0], 128, CVGP_AUDIO_RATE) == CVGP_OK);
    CHECK(!s.sb.active);
    CHECK(s.dma[1].terminal && s.dma[1].masked);
    CHECK(cvgp_irq_acknowledge(&s, owner, 1, &vector) && vector == 0x0f);
    outb(s, owner, 0x224, 0x82);      /* SB16 interrupt status: 8-bit DMA */
    CHECK(inb(s, owner, 0x225) == 1);
    CHECK(inb(s, owner, 0x22e) == 0);  /* device IRQ acknowledgement */
    CHECK(inb(s, owner, 0x225) == 0);
    eoi(s, owner, 7);
    dsp(s, owner, 0xf3);              /* 16-bit interrupt request */
    CHECK(inb(s, owner, 0x225) == 2);
    inb(s, owner, 0x22f);             /* DSP line drops; PIC keeps edge */
    CHECK(inb(s, owner, 0x225) == 0);
    CHECK(cvgp_irq_acknowledge(&s, owner, 1, &vector) && vector == 0x0f);
    eoi(s, owner, 7);
    {                                 /* a stopped DSP outputs silence */
        std::vector<int16_t> after(64 * 2, 1);
        CHECK(cvgp_render_audio(&s, owner, &after[0], 64, CVGP_AUDIO_RATE) == CVGP_OK);
        for (unsigned i = 0; i != after.size(); ++i) CHECK(after[i] == 0);
    }
    int peak = 0;
    unsigned changes = 0;
    for (unsigned i = 0; i != capture.size(); ++i) {
        int value = capture[i] < 0 ? -capture[i] : capture[i];
        if (value > peak) peak = value;
        if (i && capture[i] != capture[i - 1]) ++changes;
    }
    CHECK(peak >= 32000);
    CHECK(changes >= 3);

    dsp(s, owner, 0x20);              /* ADC is explicit, never physical passthrough */
    CHECK(s.last_error == CVGP_ERR_UNSUPPORTED_SB);
    CHECK(std::strstr(cvgp_error_message(s.last_error), "Sound Blaster") != 0);
}

static void opl_reg(cvgp_state &s, uint32_t owner, uint8_t reg, uint8_t value)
{
    outb(s, owner, 0x388, reg);
    outb(s, owner, 0x389, value);
}

static void test_opl(cvgp_state &s, uint32_t owner, std::vector<int16_t> &capture)
{
    opl_reg(s, owner, 0x20, 0x01);
    opl_reg(s, owner, 0x23, 0x01);
    opl_reg(s, owner, 0x40, 0x10);
    opl_reg(s, owner, 0x43, 0x00);
    opl_reg(s, owner, 0x60, 0xf0);
    opl_reg(s, owner, 0x63, 0xf0);
    opl_reg(s, owner, 0x80, 0x77);
    opl_reg(s, owner, 0x83, 0x77);
    opl_reg(s, owner, 0xa0, 0x98);
    opl_reg(s, owner, 0xb0, 0x31);
    capture.assign(4096 * 2, 0);
    CHECK(cvgp_render_audio(&s, owner, &capture[0], 4096, 44100) == CVGP_OK);
    int peak = 0;
    unsigned nonzero = 0, changes = 0;
    for (unsigned i = 0; i != capture.size(); ++i) {
        int value = capture[i] < 0 ? -capture[i] : capture[i];
        if (value > peak) peak = value;
        if (capture[i]) ++nonzero;
        if (i && capture[i] != capture[i - 1]) ++changes;
    }
    CHECK(peak > 100);
    CHECK(nonzero > 4000);
    CHECK(changes > 1000);

    opl_reg(s, owner, 2, 0);
    opl_reg(s, owner, 4, 1);
    cvgp_advance(&s, owner, 256u * 95u);
    CHECK((inb(s, owner, 0x388) & 0xc0) == 0xc0);
    opl_reg(s, owner, 4, 0x80);
    CHECK(inb(s, owner, 0x388) == 0);
}

static void write_pcm(const char *path, const std::vector<int16_t> &pcm)
{
    std::FILE *file = std::fopen(path, "wb");
    CHECK(file != 0);
    CHECK(std::fwrite(&pcm[0], sizeof(pcm[0]), pcm.size(), file) == pcm.size());
    CHECK(std::fclose(file) == 0);
}

int main(int argc, char **argv)
{
    fixture f;
    cvgp_backend backend;
    cvgp_state state;
    const uint32_t owner = 0x11223344UL;
    std::vector<int16_t> sb_capture, opl_capture, error_capture;
    uint32_t value = 0;
    CHECK(argc == 3);
    CHECK(CVGP_SCHED_EVENTS_PER_SERVICE == 128u);
    f.memory.resize(2 * 1024 * 1024);
    f.opl = cvgp_dbopl_create(44100);
    CHECK(f.opl != 0);
    std::memset(&backend, 0, sizeof(backend));
    backend.opaque = &f;
    backend.memory_read8 = memory_read;
    backend.opl_reset = opl_reset;
    backend.opl_write = opl_write;
    backend.opl_generate = opl_generate;
    cvgp_init(&state);

    test_ownership(state, backend);
    test_keyboard_focus(state, owner);
    test_mouse(state, owner);
    test_pic_pit(state, owner);
    test_pit_expiry_debt();
    test_pit_readback();
    test_sb_dma(state, owner, f, sb_capture);
    test_opl(state, owner, opl_capture);

    CHECK(cvgp_io_read(&state, owner, 0x388, 2, 0, &value) == CVGP_IO_REJECTED);
    CHECK(state.last_error == CVGP_ERR_IO_WIDTH);
    CHECK(cvgp_io_write(&state, owner, 0x220, 1, 1, 0) == CVGP_IO_REJECTED);
    CHECK(state.last_error == CVGP_ERR_STRING_IO);
    CHECK(cvgp_io_read(&state, owner, 0x3f8, 1, 0, &value) == CVGP_IO_NOT_CLAIMED);
    CHECK(cvgp_end(&state, owner) == CVGP_OK);
    CHECK(!state.active && !state.owner_generation && !state.sb.active);
    CHECK(cvgp_end(&state, owner) == CVGP_ERR_OWNER);

    CHECK(cvgp_begin(&state, owner + 1, CVGP_CAP_DMA_SB, CVGP_CAP_DMA_SB, &backend) == CVGP_OK);
    program_dma1(state, owner + 1, 0xfffff0UL, 3, 0);
    dsp(state, owner + 1, 0xd1);
    dsp(state, owner + 1, 0x14); dsp(state, owner + 1, 3); dsp(state, owner + 1, 0);
    error_capture.assign(16 * 2, 0);
    CHECK(cvgp_render_audio(&state, owner + 1, &error_capture[0], 16, 22050) == CVGP_ERR_MEMORY);
    CHECK(!state.sb.active);
    CHECK(cvgp_end(&state, owner + 1) == CVGP_OK);

    write_pcm(argv[1], sb_capture);
    write_pcm(argv[2], opl_capture);
    cvgp_dbopl_destroy(f.opl);
    std::printf("guest peripherals: %u assertions; ownership, focus release, PS/2, PIC/PIT expiry and read-back, DMA/SB and DBOPL passed\n", checks);
    return 0;
}
