#include "guest_peripherals.h"

/* All code in this file is original CiukiOS device-model code.  Hardware
 * behavior is kept separate from the optional GPL DBOPL synthesis adapter.
 * No libc, allocator, DOS, BIOS, firmware or physical-port service is used.
 */

static void zero_bytes(void *at, uint32_t count)
{
    uint8_t *p = (uint8_t *)at;
    while (count--) *p++ = 0;
}

static int fail(cvgp_state *s, unsigned error, uint16_t port)
{
    s->last_error = (uint8_t)error;
    s->last_port = port;
    ++s->unsupported_count;
    return (int)error;
}

static int owns(cvgp_state *s, uint32_t generation)
{
    if (!s || !s->active || !generation || s->owner_generation != generation) {
        if (s) fail(s, CVGP_ERR_OWNER, 0);
        return 0;
    }
    return 1;
}

static unsigned first_bit(uint8_t value)
{
    unsigned bit;
    for (bit = 0; bit != 8; ++bit)
        if (value & (uint8_t)(1u << bit)) return bit;
    return 8;
}

static uint8_t pic_eligible(const cvgp_pic_unit *pic)
{
    uint8_t pending = pic->irr & (uint8_t)~pic->imr;
    unsigned in_service = first_bit(pic->isr);
    if (in_service < 8)
        pending &= (uint8_t)((1u << in_service) - 1u);
    return pending;
}

static void pic_update_cascade(cvgp_state *s)
{
    if (pic_eligible(&s->pic[1])) s->pic[0].irr |= 4;
    else s->pic[0].irr &= (uint8_t)~4u;
}

static void pic_raise(cvgp_state *s, unsigned irq)
{
    if (irq < 8) s->pic[0].irr |= (uint8_t)(1u << irq);
    else if (irq < 16) {
        s->pic[1].irr |= (uint8_t)(1u << (irq - 8));
        pic_update_cascade(s);
    }
}

static void pic_clear_request(cvgp_state *s, unsigned irq)
{
    if (irq < 8) s->pic[0].irr &= (uint8_t)~(1u << irq);
    else if (irq < 16) {
        s->pic[1].irr &= (uint8_t)~(1u << (irq - 8));
        pic_update_cascade(s);
    }
}

static void reset_pic(cvgp_state *s)
{
    zero_bytes(s->pic, (uint32_t)sizeof(s->pic));
    s->pic[0].vector_base = 0x08;
    s->pic[1].vector_base = 0x70;
}

static void reset_pit(cvgp_state *s)
{
    zero_bytes(s->pit, (uint32_t)sizeof(s->pit));
    s->pit[0].reload = 65536UL;
    s->pit[0].remaining = 65536UL;
    s->pit[0].access = 3;
    s->pit[0].mode = 3;
    s->pit[0].running = 1;
}

static void reset_dma(cvgp_state *s)
{
    unsigned i;
    zero_bytes(s->dma, (uint32_t)sizeof(s->dma));
    for (i = 0; i != 8; ++i) {
        s->dma[i].masked = 1;
        s->dma[i].base_count = s->dma[i].current_count = 0xffffu;
    }
    s->dma_flipflop[0] = s->dma_flipflop[1] = 0;
    s->dma_status[0] = s->dma_status[1] = 0;
}

static uint8_t irq_mixer_value(unsigned irq)
{
    if (irq == 2) return 1;
    if (irq == 5) return 2;
    if (irq == 7) return 4;
    if (irq == 10) return 8;
    return 0;
}

static uint8_t dma_mixer_value(unsigned low, unsigned high)
{
    uint8_t value = 0;
    if (low == 0 || low == 1 || low == 3) value |= (uint8_t)(1u << low);
    if (high == 5 || high == 6 || high == 7) value |= (uint8_t)(1u << high);
    return value;
}

static void reset_sb(cvgp_state *s)
{
    cvgp_sb_state *sb = &s->sb;
    zero_bytes(sb, (uint32_t)sizeof(*sb));
    sb->base = CVGP_DEFAULT_SB_BASE;
    sb->irq = CVGP_DEFAULT_SB_IRQ;
    sb->dma8 = CVGP_DEFAULT_SB_DMA8;
    sb->dma16 = CVGP_DEFAULT_SB_DMA16;
    sb->sample_rate = 22050UL;
    sb->block_units = 1;
    sb->mixer[0x04] = 0xcc;
    sb->mixer[0x22] = 0xcc;
    sb->mixer[0x26] = 0xcc;
    sb->mixer[0x80] = irq_mixer_value(sb->irq);
    sb->mixer[0x81] = dma_mixer_value(sb->dma8, sb->dma16);
}

static void reset_opl(cvgp_state *s)
{
    zero_bytes(&s->opl, (uint32_t)sizeof(s->opl));
    if ((s->capabilities & CVGP_CAP_OPL3) && s->backend.opl_reset)
        s->backend.opl_reset(s->backend.opaque, CVGP_AUDIO_RATE);
}

static void reset_kbc(cvgp_state *s)
{
    zero_bytes(s->key_down, (uint32_t)sizeof(s->key_down));
    s->kbc_read = s->kbc_write = s->kbc_count = 0;
    s->kbc_command = 3;               /* keyboard and auxiliary IRQ enabled */
    s->kbc_pending_command = 0;
    s->keyboard_scanning = 1;
    s->keyboard_enabled = 1;
    s->mouse_enabled = 1;
    s->mouse_stream = 0;
    s->mouse_scaling = 1;
    s->mouse_resolution = 2;
    s->mouse_rate = 100;
    s->mouse_buttons = 0;
    s->mouse_pending_command = 0;
}

void cvgp_init(cvgp_state *s)
{
    if (!s) return;
    zero_bytes(s, (uint32_t)sizeof(*s));
    reset_kbc(s);
    reset_pic(s);
    reset_pit(s);
    reset_dma(s);
    reset_sb(s);
}

int cvgp_begin(cvgp_state *s, uint32_t generation, uint32_t requested,
              uint32_t available, const cvgp_backend *backend)
{
    cvgp_backend copy;
    if (!s || !generation || !requested) return CVGP_ERR_ARGUMENT;
    if (s->active) return fail(s, CVGP_ERR_ACTIVE, 0);
    if (requested & ~available) return fail(s, CVGP_ERR_MISSING_DEVICE, 0);
    zero_bytes(&copy, (uint32_t)sizeof(copy));
    if (backend) copy = *backend;
    if ((requested & CVGP_CAP_DMA_SB) && !copy.memory_read8)
        return fail(s, CVGP_ERR_BACKEND, 0);
    if ((requested & CVGP_CAP_OPL3) &&
        (!copy.opl_write || !copy.opl_generate || !copy.opl_reset))
        return fail(s, CVGP_ERR_BACKEND, 0);

    cvgp_init(s);
    s->backend = copy;
    s->owner_generation = generation;
    s->capabilities = requested;
    s->available = available;
    s->active = 1;
    s->focused = 0;
    reset_opl(s);
    return CVGP_OK;
}

int cvgp_end(cvgp_state *s, uint32_t generation)
{
    cvgp_backend backend;
    uint32_t service_generation;
    if (!owns(s, generation)) return CVGP_ERR_OWNER;
    backend = s->backend;
    service_generation = s->service_generation + 1;
    if ((s->capabilities & CVGP_CAP_OPL3) && backend.opl_reset)
        backend.opl_reset(backend.opaque, CVGP_AUDIO_RATE);
    cvgp_init(s);
    s->service_generation = service_generation;
    return CVGP_OK;
}

static void kbc_signal_head(cvgp_state *s)
{
    if (!s->kbc_count) return;
    if (s->kbc_aux[s->kbc_read]) {
        if (s->kbc_command & 2) pic_raise(s, 12);
    } else if (s->kbc_command & 1) pic_raise(s, 1);
}

static int kbc_push(cvgp_state *s, uint8_t value, int auxiliary)
{
    if (s->kbc_count == CVGP_KBC_QUEUE_BYTES)
        return fail(s, CVGP_ERR_QUEUE_FULL, auxiliary ? 0x60 : 0x64);
    s->kbc_data[s->kbc_write] = value;
    s->kbc_aux[s->kbc_write] = (uint8_t)!!auxiliary;
    if (++s->kbc_write == CVGP_KBC_QUEUE_BYTES) s->kbc_write = 0;
    ++s->kbc_count;
    kbc_signal_head(s);
    return CVGP_OK;
}

static uint8_t kbc_pop(cvgp_state *s)
{
    uint8_t value, auxiliary;
    if (!s->kbc_count) return 0xff;
    value = s->kbc_data[s->kbc_read];
    auxiliary = s->kbc_aux[s->kbc_read];
    pic_clear_request(s, auxiliary ? 12 : 1);
    if (++s->kbc_read == CVGP_KBC_QUEUE_BYTES) s->kbc_read = 0;
    --s->kbc_count;
    kbc_signal_head(s);
    return value;
}

static int key_bit(const cvgp_state *s, unsigned key)
{
    return !!(s->key_down[key >> 3] & (uint8_t)(1u << (key & 7)));
}

static void set_key_bit(cvgp_state *s, unsigned key, int down)
{
    uint8_t mask = (uint8_t)(1u << (key & 7));
    if (down) s->key_down[key >> 3] |= mask;
    else s->key_down[key >> 3] &= (uint8_t)~mask;
}

static int emit_key(cvgp_state *s, unsigned key, int pressed)
{
    int rc;
    if (key & 0x100u) {
        rc = kbc_push(s, 0xe0, 0);
        if (rc != CVGP_OK) return rc;
    }
    return kbc_push(s, (uint8_t)((key & 0x7f) | (pressed ? 0 : 0x80)), 0);
}

int cvgp_key_event(cvgp_state *s, uint32_t generation, uint8_t scan,
                  int extended, int pressed)
{
    unsigned key;
    if (!owns(s, generation)) return CVGP_ERR_OWNER;
    if (!(s->capabilities & CVGP_CAP_KEYBOARD))
        return fail(s, CVGP_ERR_MISSING_DEVICE, 0x60);
    if (!scan || scan > 0x7f) return fail(s, CVGP_ERR_ARGUMENT, 0x60);
    key = scan | (extended ? 0x100u : 0u);
    pressed = !!pressed;
    if (key_bit(s, key) == pressed) return CVGP_OK;
    set_key_bit(s, key, pressed);
    if (!s->focused || !s->keyboard_enabled || !s->keyboard_scanning)
        return CVGP_OK;
    return emit_key(s, key, pressed);
}

static int mouse_packet(cvgp_state *s, int dx, int dy, uint8_t buttons,
                        int force)
{
    uint8_t first;
    int rc;
    if (!force && (!s->focused || !s->mouse_enabled || !s->mouse_stream))
        return CVGP_OK;
    first = (uint8_t)(8 | (buttons & 7));
    if (dx < -255) { dx = -255; first |= 0x40; }
    if (dx > 255) { dx = 255; first |= 0x40; }
    if (dy < -255) { dy = -255; first |= 0x80; }
    if (dy > 255) { dy = 255; first |= 0x80; }
    if (dx < 0) first |= 0x10;
    if (dy < 0) first |= 0x20;
    rc = kbc_push(s, first, 1);
    if (rc == CVGP_OK) rc = kbc_push(s, (uint8_t)dx, 1);
    if (rc == CVGP_OK) rc = kbc_push(s, (uint8_t)dy, 1);
    if (rc == CVGP_OK) s->mouse_buttons = buttons & 7;
    return rc;
}

int cvgp_mouse_event(cvgp_state *s, uint32_t generation, int dx, int dy,
                    uint8_t buttons)
{
    if (!owns(s, generation)) return CVGP_ERR_OWNER;
    if (!(s->capabilities & CVGP_CAP_MOUSE))
        return fail(s, CVGP_ERR_MISSING_DEVICE, 0x60);
    return mouse_packet(s, dx, dy, buttons, 0);
}

int cvgp_key_raw(cvgp_state *s, uint32_t generation, uint8_t value)
{
    unsigned key;
    if (!owns(s, generation)) return CVGP_ERR_OWNER;
    if (!(s->capabilities & CVGP_CAP_KEYBOARD))
        return fail(s, CVGP_ERR_MISSING_DEVICE, 0x60);
    if (!s->focused) {
        s->raw_prefix = s->raw_skip = 0;
        return CVGP_OK;
    }
    if (s->raw_skip) --s->raw_skip;          /* inside an E1 Pause sequence */
    else if (value == 0xe0) s->raw_prefix = 1;
    else if (value == 0xe1) s->raw_skip = 5;
    else if (value == 0x00 || value == 0xaa || value == 0xee || value == 0xfa ||
             value == 0xfc || value == 0xfd || value == 0xfe || value == 0xff) {
        s->raw_prefix = 0;                   /* controller/keyboard response */
    } else {
        key = (value & 0x7fu) | (s->raw_prefix ? 0x100u : 0u);
        s->raw_prefix = 0;
        if (key != 0x12a && key != 0x136)    /* E0 2A/E0 36: fake shifts */
            set_key_bit(s, key, !(value & 0x80));
    }
    if (!s->keyboard_enabled || !s->keyboard_scanning) return CVGP_OK;
    return kbc_push(s, value, 0);
}

int cvgp_aux_raw(cvgp_state *s, uint32_t generation, uint8_t value)
{
    if (!owns(s, generation)) return CVGP_ERR_OWNER;
    if (!(s->capabilities & CVGP_CAP_MOUSE))
        return fail(s, CVGP_ERR_MISSING_DEVICE, 0x60);
    return kbc_push(s, value, 1);
}

/* Drop queued keyboard bytes but keep auxiliary bytes in order: those belong
 * to the host pointer driver, which must not lose packet synchronisation. */
static void kbc_drop_keyboard(cvgp_state *s)
{
    uint16_t i, index = s->kbc_read, kept = 0;
    uint8_t data[64];
    for (i = 0; i != s->kbc_count; ++i) {
        if (s->kbc_aux[index] && kept < sizeof(data)) data[kept++] = s->kbc_data[index];
        if (++index == CVGP_KBC_QUEUE_BYTES) index = 0;
    }
    s->kbc_read = s->kbc_write = s->kbc_count = 0;
    pic_clear_request(s, 1);
    pic_clear_request(s, 12);
    for (i = 0; i != kept; ++i) kbc_push(s, data[i], 1);
}

int cvgp_set_focus(cvgp_state *s, uint32_t generation, int focused)
{
    unsigned key;
    int rc = CVGP_OK;
    if (!owns(s, generation)) return CVGP_ERR_OWNER;
    focused = !!focused;
    if (s->focused == focused) return CVGP_OK;
    if (!focused) {
        /* Bytes already queued were typed while the guest had the focus and
         * stay its own (it may not have run since: another VM's turn); the
         * breaks follow them in order.  Only when there is no room for one
         * break for every tracked key are the pending bytes dropped. */
        unsigned need = 0;
        for (key = 0; key != 512; ++key)
            if (key_bit(s, key)) need += (key & 0x100u) ? 2u : 1u;
        if (CVGP_KBC_QUEUE_BYTES - s->kbc_count < need) kbc_drop_keyboard(s);
        s->raw_prefix = s->raw_skip = 0;
        for (key = 0; key != 512; ++key) {
            if (!key_bit(s, key)) continue;
            if (emit_key(s, key, 0) != CVGP_OK) rc = CVGP_ERR_QUEUE_FULL;
            set_key_bit(s, key, 0);
        }
        if (s->mouse_buttons && (s->capabilities & CVGP_CAP_MOUSE))
            if (mouse_packet(s, 0, 0, 0, 1) != CVGP_OK) rc = CVGP_ERR_QUEUE_FULL;
    }
    s->focused = (uint8_t)focused;
    return rc;
}

static void pic_eoi(cvgp_pic_unit *pic, uint8_t command)
{
    unsigned irq;
    if (!(command & 0x20)) return;
    irq = (command & 0x40) ? command & 7 : first_bit(pic->isr);
    if (irq < 8) pic->isr &= (uint8_t)~(1u << irq);
}

static void pic_write(cvgp_state *s, unsigned which, int data, uint8_t value)
{
    cvgp_pic_unit *pic = &s->pic[which];
    if (!data) {
        if (value & 0x10) {
            pic->irr = pic->isr = pic->imr = 0;
            pic->init_step = 1;
            pic->expect_icw4 = value & 1;
            pic->single = (value >> 1) & 1;
            pic->auto_eoi = 0;
            pic->read_isr = 0;
        } else if ((value & 0x18) == 0x08) {
            if (value & 2) pic->read_isr = value & 1;
        } else pic_eoi(pic, value);
    } else if (pic->init_step) {
        if (pic->init_step == 1) {
            pic->vector_base = value & 0xf8;
            pic->init_step = pic->single ? (pic->expect_icw4 ? 3 : 0) : 2;
        } else if (pic->init_step == 2) pic->init_step = pic->expect_icw4 ? 3 : 0;
        else {
            pic->auto_eoi = (value >> 1) & 1;
            pic->init_step = 0;
        }
    } else pic->imr = value;
    pic_update_cascade(s);
}

static uint8_t pic_read(cvgp_state *s, unsigned which, int data)
{
    cvgp_pic_unit *pic = &s->pic[which];
    if (data) return pic->imr;
    return pic->read_isr ? pic->isr : pic->irr;
}

int cvgp_irq_acknowledge(cvgp_state *s, uint32_t generation, int guest_if,
                        uint8_t *vector)
{
    cvgp_pic_unit *master, *slave;
    uint8_t pending;
    unsigned irq, sirq;
    if (!owns(s, generation) || !vector || !guest_if) return 0;
    master = &s->pic[0];
    slave = &s->pic[1];
    pic_update_cascade(s);
    pending = pic_eligible(master);
    irq = first_bit(pending);
    if (irq == 8) return 0;
    master->irr &= (uint8_t)~(1u << irq);
    if (!master->auto_eoi) master->isr |= (uint8_t)(1u << irq);
    if (irq != 2) {
        *vector = (uint8_t)(master->vector_base + irq);
        return 1;
    }
    pending = pic_eligible(slave);
    sirq = first_bit(pending);
    if (sirq == 8) {
        *vector = (uint8_t)(master->vector_base + 7); /* spurious cascade */
        return 1;
    }
    slave->irr &= (uint8_t)~(1u << sirq);
    if (!slave->auto_eoi) slave->isr |= (uint8_t)(1u << sirq);
    pic_update_cascade(s);
    *vector = (uint8_t)(slave->vector_base + sirq);
    return 1;
}

static uint16_t pit_count(const cvgp_pit_channel *pit)
{
    return (uint16_t)(pit->remaining == 65536UL ? 0 : pit->remaining);
}

static void pit_load(cvgp_pit_channel *pit, uint16_t value)
{
    pit->reload = value ? value : 65536UL;
    pit->remaining = pit->reload;
    pit->running = 1;
    pit->latched = 0;
}

static int pit_control(cvgp_state *s, uint8_t value)
{
    unsigned channel = value >> 6;
    unsigned access = (value >> 4) & 3;
    unsigned mode = (value >> 1) & 7;
    cvgp_pit_channel *pit;
    if (channel == 3) return fail(s, CVGP_ERR_UNSUPPORTED_PIT, 0x43);
    pit = &s->pit[channel];
    if (!access) {
        pit->latch = pit_count(pit);
        pit->latched = 1;
        pit->read_phase = 0;
        return CVGP_OK;
    }
    if (mode >= 6) mode -= 4;
    if (mode != 0 && mode != 2 && mode != 3)
        return fail(s, CVGP_ERR_UNSUPPORTED_PIT, 0x43);
    pit->access = (uint8_t)access;
    pit->mode = (uint8_t)mode;
    pit->write_phase = pit->read_phase = 0;
    return CVGP_OK;
}

static void pit_write(cvgp_pit_channel *pit, uint8_t value)
{
    uint16_t word;
    if (pit->access == 1) pit_load(pit, value);
    else if (pit->access == 2) pit_load(pit, (uint16_t)value << 8);
    else {
        if (!pit->write_phase) {
            pit->latch = value;
            pit->write_phase = 1;
        } else {
            word = (uint16_t)(pit->latch | ((uint16_t)value << 8));
            pit->write_phase = 0;
            pit_load(pit, word);
        }
    }
}

static uint8_t pit_read(cvgp_pit_channel *pit)
{
    uint16_t value = pit->latched ? pit->latch : pit_count(pit);
    uint8_t result;
    if (pit->access == 2) result = (uint8_t)(value >> 8);
    else if (pit->access == 3 && pit->read_phase) result = (uint8_t)(value >> 8);
    else result = (uint8_t)value;
    if (pit->access == 3) {
        pit->read_phase ^= 1;
        if (!pit->read_phase) pit->latched = 0;
    } else pit->latched = 0;
    return result;
}

static void pit_advance(cvgp_state *s, uint32_t clocks)
{
    cvgp_pit_channel *pit = &s->pit[0];
    uint32_t after;
    if (!pit->running || !clocks) return;
    if (clocks < pit->remaining) {
        pit->remaining -= clocks;
        return;
    }
    clocks -= pit->remaining;
    pic_raise(s, 0);
    if (pit->mode == 0) {
        pit->remaining = 0;
        pit->running = 0;
        return;
    }
    after = clocks % pit->reload;
    pit->remaining = after ? pit->reload - after : pit->reload;
}

static int page_channel(uint16_t port)
{
    static const int8_t map[16] = {-1,2,3,1,-1,-1,-1,0,-1,6,7,5,-1,-1,-1,4};
    return map[port - 0x80];
}

static int dma_address_port(uint16_t port, unsigned *channel, int *count)
{
    unsigned index;
    if (port <= 7) {
        *channel = port >> 1;
        *count = port & 1;
        return 1;
    }
    if (port >= 0xc0 && port <= 0xce && !(port & 1)) {
        index = (port - 0xc0) >> 1;
        *channel = 4 + (index >> 1);
        *count = index & 1;
        return 1;
    }
    return 0;
}

static int dma_control_port(uint16_t port, unsigned *controller, unsigned *index)
{
    if (port >= 8 && port <= 15) {
        *controller = 0;
        *index = port;
        return 1;
    }
    if (port >= 0xd0 && port <= 0xde && !(port & 1)) {
        *controller = 1;
        *index = 8 + ((port - 0xd0) >> 1);
        return 1;
    }
    return 0;
}

static uint8_t dma_read_port(cvgp_state *s, uint16_t port)
{
    unsigned channel, controller, index;
    int is_count, page;
    uint16_t value;
    uint8_t result;
    if (dma_address_port(port, &channel, &is_count)) {
        controller = channel >= 4;
        value = is_count ? s->dma[channel].current_count : s->dma[channel].current_address;
        result = s->dma_flipflop[controller] ? (uint8_t)(value >> 8) : (uint8_t)value;
        s->dma_flipflop[controller] ^= 1;
        return result;
    }
    if (port >= 0x80 && port <= 0x8f) {
        page = page_channel(port);
        return page >= 0 ? s->dma[page].page : 0xff;
    }
    if (dma_control_port(port, &controller, &index)) {
        if (index == 8) {
            result = s->dma_status[controller];
            s->dma_status[controller] &= 0xf0;
            return result;
        }
        return 0xff;
    }
    return 0xff;
}

static void dma_write_word_byte(cvgp_state *s, unsigned channel, int is_count,
                                uint8_t value)
{
    unsigned controller = channel >= 4;
    uint16_t *at = is_count ? &s->dma[channel].base_count : &s->dma[channel].base_address;
    if (!s->dma_flipflop[controller]) *at = (uint16_t)((*at & 0xff00) | value);
    else {
        *at = (uint16_t)((*at & 0x00ff) | ((uint16_t)value << 8));
        if (is_count) s->dma[channel].current_count = *at;
        else s->dma[channel].current_address = *at;
        s->dma[channel].terminal = 0;
    }
    s->dma_flipflop[controller] ^= 1;
}

static int dma_write_port(cvgp_state *s, uint16_t port, uint8_t value)
{
    unsigned channel, controller, index, base, i;
    int is_count, page;
    if (dma_address_port(port, &channel, &is_count)) {
        dma_write_word_byte(s, channel, is_count, value);
        return CVGP_OK;
    }
    if (port >= 0x80 && port <= 0x8f) {
        page = page_channel(port);
        if (page >= 0) s->dma[page].page = value;
        return CVGP_OK;
    }
    if (!dma_control_port(port, &controller, &index))
        return fail(s, CVGP_ERR_UNSUPPORTED_PORT, port);
    base = controller ? 4 : 0;
    switch (index) {
    case 8: return CVGP_OK;             /* command: no memory-to-memory support */
    case 9: return CVGP_OK;             /* software request is not a device DREQ */
    case 10:
        channel = base + (value & 3);
        s->dma[channel].masked = !!(value & 4);
        return CVGP_OK;
    case 11:
        channel = base + (value & 3);
        s->dma[channel].mode = value & 0xfc;
        return CVGP_OK;
    case 12: s->dma_flipflop[controller] = 0; return CVGP_OK;
    case 13:
        s->dma_flipflop[controller] = 0;
        s->dma_status[controller] = 0;
        for (i = base; i != base + 4; ++i) s->dma[i].masked = 1;
        return CVGP_OK;
    case 14:
        for (i = base; i != base + 4; ++i) s->dma[i].masked = 0;
        return CVGP_OK;
    case 15:
        for (i = 0; i != 4; ++i) s->dma[base + i].masked = (value >> i) & 1;
        return CVGP_OK;
    default: return fail(s, CVGP_ERR_UNSUPPORTED_DMA, port);
    }
}

static int dma_read_unit(cvgp_state *s, unsigned channel, uint16_t *sample)
{
    cvgp_dma_channel *dma;
    uint32_t physical;
    uint8_t low, high = 0;
    int valid = 0;
    unsigned controller;
    if (channel >= 8 || channel == 4) return fail(s, CVGP_ERR_UNSUPPORTED_DMA, 0);
    dma = &s->dma[channel];
    if (dma->masked || ((dma->mode >> 2) & 3) != 2 || (dma->mode >> 6) == 3)
        return fail(s, CVGP_ERR_UNSUPPORTED_DMA, 0);
    physical = ((uint32_t)dma->page << 16) | dma->current_address;
    if (channel >= 5) physical = ((uint32_t)dma->page << 16) |
                                  ((uint32_t)dma->current_address << 1);
    low = s->backend.memory_read8(s->backend.opaque, physical, &valid);
    if (!valid) return fail(s, CVGP_ERR_MEMORY, 0);
    if (channel >= 5) {
        high = s->backend.memory_read8(s->backend.opaque, physical + 1, &valid);
        if (!valid) return fail(s, CVGP_ERR_MEMORY, 0);
    }
    *sample = (uint16_t)(low | ((uint16_t)high << 8));
    if (dma->mode & 0x20) --dma->current_address;
    else ++dma->current_address;
    if (!dma->current_count) {
        controller = channel >= 4;
        s->dma_status[controller] |= (uint8_t)(1u << (channel & 3));
        dma->terminal = 1;
        if (dma->mode & 0x10) {
            dma->current_address = dma->base_address;
            dma->current_count = dma->base_count;
        } else {
            dma->current_count = 0xffffu;
            dma->masked = 1;
        }
    } else --dma->current_count;
    return CVGP_OK;
}

static void sb_response(cvgp_state *s, uint8_t value)
{
    cvgp_sb_state *sb = &s->sb;
    if (sb->response_count == sizeof(sb->response)) {
        fail(s, CVGP_ERR_QUEUE_FULL, (uint16_t)(sb->base + 0x0a));
        return;
    }
    sb->response[sb->response_write++] = value;
    sb->response_write &= (uint8_t)(sizeof(sb->response) - 1);
    ++sb->response_count;
}

static uint8_t sb_read_response(cvgp_state *s)
{
    cvgp_sb_state *sb = &s->sb;
    uint8_t value;
    if (!sb->response_count) return 0xff;
    value = sb->response[sb->response_read++];
    sb->response_read &= (uint8_t)(sizeof(sb->response) - 1);
    --sb->response_count;
    return value;
}

static void sb_stop(cvgp_state *s)
{
    s->sb.active = 0;
    s->sb.paused8 = s->sb.paused16 = 0;
    s->sb.auto_init = s->sb.exit_auto = 0;
    s->sb.units_left = 0;
    /* A stopped DSP outputs silence: the zero-order hold only bridges the
     * gaps between samples of a running transfer. */
    s->sb.held_left = s->sb.held_right = 0;
}

static void sb_reset_dsp(cvgp_state *s)
{
    unsigned base = s->sb.base, irq = s->sb.irq;
    unsigned dma8 = s->sb.dma8, dma16 = s->sb.dma16;
    uint8_t mixer[256];
    unsigned i;
    for (i = 0; i != 256; ++i) mixer[i] = s->sb.mixer[i];
    reset_sb(s);
    s->sb.base = (uint16_t)base;
    s->sb.irq = (uint8_t)irq;
    s->sb.dma8 = (uint8_t)dma8;
    s->sb.dma16 = (uint8_t)dma16;
    for (i = 0; i != 256; ++i) s->sb.mixer[i] = mixer[i];
    sb_response(s, 0xaa);
}

static unsigned sb_argument_need(uint8_t command)
{
    if ((command >= 0xb0 && command <= 0xcf)) return 3;
    switch (command) {
    case 0x10: case 0x38: case 0x40: case 0xe0: case 0xe2: case 0xe4: return 1;
    case 0x14: case 0x16: case 0x17: case 0x41: case 0x42: case 0x48:
    case 0x74: case 0x75: case 0x76: case 0x77: case 0x80: return 2;
    default: return 0;
    }
}

static void sb_start(cvgp_state *s, unsigned bits, int auto_init,
                     int is_signed, int stereo, uint32_t units, int silent)
{
    cvgp_sb_state *sb = &s->sb;
    sb->sample_bits = (uint8_t)bits;
    sb->sample_signed = (uint8_t)is_signed;
    sb->stereo = (uint8_t)stereo;
    sb->auto_init = (uint8_t)auto_init;
    sb->exit_auto = 0;
    sb->active = 1;
    sb->silent = (uint8_t)silent;
    sb->units_left = units ? units : 1;
    sb->block_units = sb->units_left;
    sb->resample_phase = 0;
}

static void sb_execute(cvgp_state *s)
{
    cvgp_sb_state *sb = &s->sb;
    uint8_t command = sb->command;
    uint32_t length;
    unsigned mode;
    if (command >= 0xb0 && command <= 0xcf) {
        if (command & 8) {
            fail(s, CVGP_ERR_UNSUPPORTED_SB, (uint16_t)(sb->base + 0x0c));
            return;                    /* ADC/capture is deliberately absent */
        }
        length = (uint32_t)sb->arguments[1] |
                 ((uint32_t)sb->arguments[2] << 8);
        mode = sb->arguments[0];
        sb_start(s, command < 0xc0 ? 16 : 8, !!(command & 4),
                 !!(mode & 0x10), !!(mode & 0x20), length + 1, 0);
        return;
    }
    switch (command) {
    case 0x10:
        sb->held_left = sb->held_right = (int16_t)(((int)sb->arguments[0] - 128) * 256);
        break;
    case 0x14: case 0x16: case 0x17: case 0x74: case 0x75: case 0x76: case 0x77:
        length = (uint32_t)sb->arguments[0] | ((uint32_t)sb->arguments[1] << 8);
        sb_start(s, 8, 0, 0, 0, length + 1, 0);
        break;
    case 0x1c: case 0x90:
        sb_start(s, 8, 1, 0, 0, sb->block_units, 0);
        break;
    case 0x91:
        sb_start(s, 8, 0, 0, 0, sb->block_units, 0);
        break;
    case 0x40:
        if (sb->arguments[0] != 255)
            sb->sample_rate = 1000000UL / (256u - sb->arguments[0]);
        break;
    case 0x41: case 0x42:
        length = ((uint32_t)sb->arguments[0] << 8) | sb->arguments[1];
        if (length >= 4000 && length <= 48000) sb->sample_rate = length;
        else fail(s, CVGP_ERR_UNSUPPORTED_SB, (uint16_t)(sb->base + 0x0c));
        break;
    case 0x48:
        sb->block_units = 1 + (uint32_t)sb->arguments[0] +
                          ((uint32_t)sb->arguments[1] << 8);
        break;
    case 0x80:
        length = (uint32_t)sb->arguments[0] | ((uint32_t)sb->arguments[1] << 8);
        sb_start(s, 8, 0, 0, 0, length + 1, 1);
        break;
    case 0xd0: sb->paused8 = 1; break;
    case 0xd1: sb->speaker = 1; break;
    case 0xd3: sb->speaker = 0; break;
    case 0xd4: sb->paused8 = 0; break;
    case 0xd5: sb->paused16 = 1; break;
    case 0xd6: sb->paused16 = 0; break;
    case 0xd8: sb_response(s, sb->speaker ? 0xff : 0); break;
    case 0xd9: if (sb->sample_bits == 16) sb->exit_auto = 1; break;
    case 0xda: if (sb->sample_bits == 8) sb->exit_auto = 1; break;
    case 0xe0: sb_response(s, (uint8_t)~sb->arguments[0]); break;
    case 0xe1: sb_response(s, 4); sb_response(s, 5); break;
    case 0xe4: sb->test_register = sb->arguments[0]; break;
    case 0xe8: sb_response(s, sb->test_register); break;
    case 0xf2: sb->irq_status |= 1; pic_raise(s, sb->irq); break;
    case 0xf3: sb->irq_status |= 2; pic_raise(s, sb->irq); break;
    case 0x20: case 0x30: case 0x31: case 0x34: case 0x35: case 0x36: case 0x37:
    case 0xe2:
        fail(s, CVGP_ERR_UNSUPPORTED_SB, (uint16_t)(sb->base + 0x0c));
        break;
    default:
        fail(s, CVGP_ERR_UNSUPPORTED_SB, (uint16_t)(sb->base + 0x0c));
        break;
    }
}

static void sb_command_byte(cvgp_state *s, uint8_t value)
{
    cvgp_sb_state *sb = &s->sb;
    if (!sb->argument_need) {
        sb->command = value;
        sb->argument_count = 0;
        sb->argument_need = (uint8_t)sb_argument_need(value);
        if (!sb->argument_need) sb_execute(s);
    } else {
        if (sb->argument_count < sizeof(sb->arguments))
            sb->arguments[sb->argument_count] = value;
        ++sb->argument_count;
        if (sb->argument_count == sb->argument_need) {
            sb->argument_need = 0;
            sb_execute(s);
        }
    }
}

static uint8_t sb_read_mixer(cvgp_state *s)
{
    cvgp_sb_state *sb = &s->sb;
    if (sb->mixer_index == 0x80) return irq_mixer_value(sb->irq);
    if (sb->mixer_index == 0x81) return dma_mixer_value(sb->dma8, sb->dma16);
    /* SB16 interrupt status: which DSP interrupt is pending. SB16-aware
     * drivers (Apogee Sound System) read it first and chain the IRQ away
     * when neither DMA bit is set. */
    if (sb->mixer_index == 0x82) return sb->irq_status;
    return sb->mixer[sb->mixer_index];
}

static void sb_write_mixer(cvgp_state *s, uint8_t value)
{
    cvgp_sb_state *sb = &s->sb;
    unsigned i;
    if (sb->mixer_index == 0) {
        for (i = 0; i != 256; ++i) sb->mixer[i] = 0;
        sb->mixer[0x80] = irq_mixer_value(sb->irq);
        sb->mixer[0x81] = dma_mixer_value(sb->dma8, sb->dma16);
    } else if (sb->mixer_index == 0x80) {
        if (value == 1) sb->irq = 2;
        else if (value == 2) sb->irq = 5;
        else if (value == 4) sb->irq = 7;
        else if (value == 8) sb->irq = 10;
        else { fail(s, CVGP_ERR_UNSUPPORTED_SB, (uint16_t)(sb->base + 5)); return; }
    } else if (sb->mixer_index == 0x81) {
        if (value & 1) sb->dma8 = 0;
        else if (value & 2) sb->dma8 = 1;
        else if (value & 8) sb->dma8 = 3;
        else { fail(s, CVGP_ERR_UNSUPPORTED_SB, (uint16_t)(sb->base + 5)); return; }
        if (value & 0x20) sb->dma16 = 5;
        else if (value & 0x40) sb->dma16 = 6;
        else if (value & 0x80) sb->dma16 = 7;
        else { fail(s, CVGP_ERR_UNSUPPORTED_SB, (uint16_t)(sb->base + 5)); return; }
    } else sb->mixer[sb->mixer_index] = value;
}

static uint8_t sb_read_port(cvgp_state *s, uint16_t port)
{
    cvgp_sb_state *sb = &s->sb;
    unsigned offset = port - sb->base;
    switch (offset) {
    case 4: return sb->mixer_index;
    case 5: return sb_read_mixer(s);
    case 0x0a: return sb_read_response(s);
    case 0x0c: return 0;               /* DSP write buffer ready */
    case 0x0e:
        sb->irq_status &= (uint8_t)~1u;
        pic_clear_request(s, sb->irq);
        return sb->response_count ? 0x80 : 0;
    case 0x0f:
        sb->irq_status &= (uint8_t)~2u;
        pic_clear_request(s, sb->irq);
        return 0xff;
    default: return 0xff;
    }
}

static int sb_write_port(cvgp_state *s, uint16_t port, uint8_t value)
{
    cvgp_sb_state *sb = &s->sb;
    unsigned offset = port - sb->base;
    switch (offset) {
    case 4: sb->mixer_index = value; return CVGP_OK;
    case 5: sb_write_mixer(s, value); return CVGP_OK;
    case 6:
        if (value & 1) sb->reset_latch = 1;
        else if (sb->reset_latch) { sb->reset_latch = 0; sb_reset_dsp(s); }
        return CVGP_OK;
    case 0x0c: sb_command_byte(s, value); return CVGP_OK;
    default: return fail(s, CVGP_ERR_UNSUPPORTED_PORT, port);
    }
}

static void opl_write_data(cvgp_state *s, unsigned bank, uint8_t value)
{
    unsigned reg = s->opl.index[bank] & 0x1ff;
    s->opl.regs[reg] = value;
    if (!bank && reg == 4) {
        if (value & 0x80) s->opl.status = 0;
        s->opl.timer_control = value;
    }
    s->backend.opl_write(s->backend.opaque, (uint16_t)reg, value);
}

static void opl_write_port(cvgp_state *s, uint16_t port, uint8_t value)
{
    unsigned bank = (port & 2) ? 1 : 0;
    if (!(port & 1)) s->opl.index[bank] = (uint16_t)(value | (bank ? 0x100 : 0));
    else opl_write_data(s, bank, value);
}

static uint8_t opl_read_port(cvgp_state *s, uint16_t port)
{
    (void)port;
    return s->opl.status;
}

static void opl_advance(cvgp_state *s, uint32_t clocks)
{
    uint32_t limit;
    unsigned timer;
    uint8_t start, mask, status;
    if (!(s->capabilities & CVGP_CAP_OPL3)) return;
    for (timer = 0; timer != 2; ++timer) {
        start = (uint8_t)(1u << timer);
        mask = timer ? 0x20 : 0x40;
        status = timer ? 0x20 : 0x40;
        if (!(s->opl.timer_control & start)) continue;
        limit = (256u - s->opl.regs[2 + timer]) * (timer ? 382u : 95u);
        if (!limit) limit = 1;
        s->opl.timer_accum[timer] += clocks;
        if (s->opl.timer_accum[timer] >= limit) {
            s->opl.timer_accum[timer] %= limit;
            if (!(s->opl.timer_control & mask)) s->opl.status |= (uint8_t)(0x80 | status);
        }
    }
}

static int16_t saturate(int32_t value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

static void sb_complete_unit(cvgp_state *s)
{
    cvgp_sb_state *sb = &s->sb;
    if (sb->units_left) --sb->units_left;
    if (sb->units_left) return;
    sb->irq_status |= sb->sample_bits == 16 ? 2 : 1;
    pic_raise(s, sb->irq);
    if (sb->auto_init && !sb->exit_auto) sb->units_left = sb->block_units;
    else sb_stop(s);
}

static int sb_sample(cvgp_state *s, int16_t *sample)
{
    cvgp_sb_state *sb = &s->sb;
    uint16_t raw;
    unsigned channel = sb->sample_bits == 16 ? sb->dma16 : sb->dma8;
    int rc;
    if (sb->silent) {
        *sample = 0;
        sb_complete_unit(s);
        return CVGP_OK;
    }
    rc = dma_read_unit(s, channel, &raw);
    if (rc != CVGP_OK) { sb_stop(s); return rc; }
    if (sb->sample_bits == 16) {
        *sample = sb->sample_signed ? (int16_t)raw : (int16_t)(raw ^ 0x8000u);
    } else {
        int value = (int)(raw & 0xff);
        if (sb->sample_signed) value = (int)(int8_t)value;
        else value -= 128;
        *sample = (int16_t)(value * 256);
    }
    sb_complete_unit(s);
    return CVGP_OK;
}

static int sb_next_frame(cvgp_state *s)
{
    cvgp_sb_state *sb = &s->sb;
    int16_t left = 0, right = 0;
    int rc;
    if (!sb->active || (sb->sample_bits == 16 ? sb->paused16 : sb->paused8)) return CVGP_OK;
    rc = sb_sample(s, &left);
    if (rc != CVGP_OK) return rc;
    if (sb->stereo && sb->active) {
        rc = sb_sample(s, &right);
        if (rc != CVGP_OK) return rc;
    } else right = left;
    sb->held_left = left;
    sb->held_right = right;
    return CVGP_OK;
}

int cvgp_render_audio(cvgp_state *s, uint32_t generation, int16_t *stereo,
                     unsigned frames, unsigned output_rate)
{
    cvgp_sb_state *sb;
    unsigned i;
    int opl_ok = 1, rc;
    if (!owns(s, generation)) return CVGP_ERR_OWNER;
    if (!stereo || !output_rate || output_rate > 192000u)
        return fail(s, CVGP_ERR_ARGUMENT, 0);
    if ((s->capabilities & CVGP_CAP_OPL3) && output_rate != CVGP_AUDIO_RATE)
        return fail(s, CVGP_ERR_BACKEND, 0x388);
    for (i = 0; i != frames * 2u; ++i) stereo[i] = 0;
    if ((s->capabilities & CVGP_CAP_OPL3) && frames) {
        opl_ok = s->backend.opl_generate(s->backend.opaque, stereo, frames);
        if (!opl_ok) return fail(s, CVGP_ERR_BACKEND, 0x388);
    }
    if (!(s->capabilities & CVGP_CAP_DMA_SB)) return CVGP_OK;
    sb = &s->sb;
    for (i = 0; i != frames; ++i) {
        sb->resample_phase += sb->sample_rate;
        while (sb->active && sb->resample_phase >= output_rate) {
            sb->resample_phase -= output_rate;
            rc = sb_next_frame(s);
            if (rc != CVGP_OK) return rc;
        }
        /* DSP 4.xx (SB16): D1h/D3h only change the D8h status; the DAC is
         * never gated. DMX (original DOOM) plays without ever sending D1h.
         * A paused transfer outputs silence. */
        if (sb->active && !sb->silent &&
            !(sb->sample_bits == 16 ? sb->paused16 : sb->paused8)) {
            stereo[i * 2] = saturate((int32_t)stereo[i * 2] + sb->held_left);
            stereo[i * 2 + 1] = saturate((int32_t)stereo[i * 2 + 1] + sb->held_right);
        }
    }
    return CVGP_OK;
}

static int keyboard_command(cvgp_state *s, uint8_t value)
{
    if (s->kbc_pending_command == 0x60) {
        s->kbc_command = value;
        s->kbc_pending_command = 0;
        return CVGP_OK;
    }
    if (s->mouse_pending_command) {
        if (s->mouse_pending_command == 0xe8) s->mouse_resolution = value & 3;
        else s->mouse_rate = value;
        s->mouse_pending_command = 0;
        return kbc_push(s, 0xfa, 1);
    }
    if (s->kbc_pending_command == 0xd4) {
        s->kbc_pending_command = 0;
        switch (value) {
        case 0xe6: s->mouse_scaling = 1; return kbc_push(s, 0xfa, 1);
        case 0xe7: s->mouse_scaling = 2; return kbc_push(s, 0xfa, 1);
        case 0xe8: s->mouse_pending_command = 0xe8; return kbc_push(s, 0xfa, 1);
        case 0xf2: kbc_push(s, 0xfa, 1); return kbc_push(s, 0, 1);
        case 0xf3: s->mouse_pending_command = 0xf3; return kbc_push(s, 0xfa, 1);
        case 0xf4: s->mouse_stream = 1; return kbc_push(s, 0xfa, 1);
        case 0xf5: s->mouse_stream = 0; return kbc_push(s, 0xfa, 1);
        case 0xf6:
            s->mouse_stream = 0; s->mouse_scaling = 1;
            s->mouse_resolution = 2; s->mouse_rate = 100;
            return kbc_push(s, 0xfa, 1);
        case 0xff:
            s->mouse_stream = 0;
            kbc_push(s, 0xfa, 1); kbc_push(s, 0xaa, 1); return kbc_push(s, 0, 1);
        default: return fail(s, CVGP_ERR_UNSUPPORTED_PORT, 0x60);
        }
    }
    switch (value) {
    case 0xed: case 0xf3:
        s->kbc_pending_command = value;
        return kbc_push(s, 0xfa, 0);
    case 0xee: return kbc_push(s, 0xee, 0);
    case 0xf2: kbc_push(s, 0xfa, 0); kbc_push(s, 0xab, 0); return kbc_push(s, 0x83, 0);
    case 0xf4: s->keyboard_scanning = 1; return kbc_push(s, 0xfa, 0);
    case 0xf5: s->keyboard_scanning = 0; return kbc_push(s, 0xfa, 0);
    case 0xf6: s->keyboard_scanning = 1; return kbc_push(s, 0xfa, 0);
    case 0xff: s->keyboard_scanning = 1; kbc_push(s, 0xfa, 0); return kbc_push(s, 0xaa, 0);
    default:
        if (s->kbc_pending_command == 0xed || s->kbc_pending_command == 0xf3) {
            s->kbc_pending_command = 0;
            return kbc_push(s, 0xfa, 0);
        }
        return fail(s, CVGP_ERR_UNSUPPORTED_PORT, 0x60);
    }
}

static int kbc_command(cvgp_state *s, uint8_t value)
{
    switch (value) {
    case 0x20: return kbc_push(s, s->kbc_command, 0);
    case 0x60: s->kbc_pending_command = 0x60; return CVGP_OK;
    case 0xa7: s->mouse_enabled = 0; return CVGP_OK;
    case 0xa8: s->mouse_enabled = 1; return CVGP_OK;
    case 0xaa: return kbc_push(s, 0x55, 0);
    case 0xab: return kbc_push(s, 0, 0);
    case 0xad: s->keyboard_enabled = 0; return CVGP_OK;
    case 0xae: s->keyboard_enabled = 1; return CVGP_OK;
    case 0xd4: s->kbc_pending_command = 0xd4; return CVGP_OK;
    default: return fail(s, CVGP_ERR_UNSUPPORTED_PORT, 0x64);
    }
}

int cvgp_port_claimed(const cvgp_state *s, uint16_t port)
{
    uint32_t caps;
    if (!s || !s->active) return 0;
    caps = s->capabilities;
    if ((caps & (CVGP_CAP_KEYBOARD | CVGP_CAP_MOUSE)) && (port == 0x60 || port == 0x64)) return 1;
    if ((caps & CVGP_CAP_PIC_PIT) &&
        ((port >= 0x40 && port <= 0x43) || port == 0x20 || port == 0x21 ||
         port == 0xa0 || port == 0xa1)) return 1;
    if (caps & CVGP_CAP_DMA_SB) {
        if (port <= 0x0f || (port >= 0x80 && port <= 0x8f) ||
            (port >= 0xc0 && port <= 0xdf) ||
            (port >= s->sb.base && port < s->sb.base + 16)) return 1;
    }
    if (caps & CVGP_CAP_OPL3) {
        if (port >= 0x388 && port <= 0x38b) return 1;
        if (port == s->sb.base || port == s->sb.base + 1 ||
            port == s->sb.base + 8 || port == s->sb.base + 9) return 1;
    }
    return 0;
}

static int io_check(cvgp_state *s, uint32_t generation, uint16_t port,
                    unsigned width, int string_io)
{
    if (!owns(s, generation)) return CVGP_IO_REJECTED;
    if (!cvgp_port_claimed(s, port)) return CVGP_IO_NOT_CLAIMED;
    if (string_io) { fail(s, CVGP_ERR_STRING_IO, port); return CVGP_IO_REJECTED; }
    if (width != 1) { fail(s, CVGP_ERR_IO_WIDTH, port); return CVGP_IO_REJECTED; }
    return CVGP_IO_OK;
}

int cvgp_io_read(cvgp_state *s, uint32_t generation, uint16_t port,
                unsigned width, int string_io, uint32_t *value)
{
    int check;
    uint8_t byte = 0xff;
    if (!value) return CVGP_IO_REJECTED;
    check = io_check(s, generation, port, width, string_io);
    if (check != CVGP_IO_OK) return check;
    if (port == 0x60) byte = kbc_pop(s);
    else if (port == 0x64) {
        byte = 4;
        if (s->kbc_count) {
            byte |= 1;
            if (s->kbc_aux[s->kbc_read]) byte |= 0x20;
        }
    } else if (port == 0x20 || port == 0x21) byte = pic_read(s, 0, port & 1);
    else if (port == 0xa0 || port == 0xa1) byte = pic_read(s, 1, port & 1);
    else if (port >= 0x40 && port <= 0x42) byte = pit_read(&s->pit[port - 0x40]);
    else if (port == 0x43) byte = 0xff;
    else if ((s->capabilities & CVGP_CAP_OPL3) &&
             (port >= 0x388 && port <= 0x38b)) byte = opl_read_port(s, port);
    else if ((s->capabilities & CVGP_CAP_OPL3) &&
             (port == s->sb.base || port == s->sb.base + 1 ||
              port == s->sb.base + 8 || port == s->sb.base + 9))
        byte = opl_read_port(s, port);
    else if (port >= s->sb.base && port < s->sb.base + 16) byte = sb_read_port(s, port);
    else byte = dma_read_port(s, port);
    *value = byte;
    return CVGP_IO_OK;
}

int cvgp_io_write(cvgp_state *s, uint32_t generation, uint16_t port,
                 unsigned width, int string_io, uint32_t value)
{
    int check;
    uint8_t byte = (uint8_t)value;
    check = io_check(s, generation, port, width, string_io);
    if (check != CVGP_IO_OK) return check;
    if (port == 0x60) keyboard_command(s, byte);
    else if (port == 0x64) kbc_command(s, byte);
    else if (port == 0x20 || port == 0x21) pic_write(s, 0, port & 1, byte);
    else if (port == 0xa0 || port == 0xa1) pic_write(s, 1, port & 1, byte);
    else if (port >= 0x40 && port <= 0x42) pit_write(&s->pit[port - 0x40], byte);
    else if (port == 0x43) pit_control(s, byte);
    else if ((s->capabilities & CVGP_CAP_OPL3) &&
             (port >= 0x388 && port <= 0x38b)) opl_write_port(s, port, byte);
    else if ((s->capabilities & CVGP_CAP_OPL3) &&
             (port == s->sb.base || port == s->sb.base + 1 ||
              port == s->sb.base + 8 || port == s->sb.base + 9)) {
        uint16_t mapped = (uint16_t)(0x388 + ((port - s->sb.base) & 3));
        opl_write_port(s, mapped, byte);
    } else if (port >= s->sb.base && port < s->sb.base + 16)
        sb_write_port(s, port, byte);
    else dma_write_port(s, port, byte);
    return CVGP_IO_OK;
}

unsigned cvgp_advance(cvgp_state *s, uint32_t generation, uint32_t pit_clocks)
{
    unsigned result = CVGP_SERVICE_NONE;
    if (!owns(s, generation)) return CVGP_SERVICE_DIAGNOSTIC;
    pit_advance(s, pit_clocks);
    opl_advance(s, pit_clocks);
    ++s->service_generation;
    pic_update_cascade(s);
    if (pic_eligible(&s->pic[0]) || pic_eligible(&s->pic[1])) result |= CVGP_SERVICE_IRQ;
    if (s->sb.active || (s->capabilities & CVGP_CAP_OPL3)) result |= CVGP_SERVICE_AUDIO;
    if (s->last_error) result |= CVGP_SERVICE_DIAGNOSTIC;
    return result;
}

const char *cvgp_error_message(unsigned error)
{
    switch (error) {
    case CVGP_OK: return "The guest device session is ready.";
    case CVGP_ERR_ARGUMENT: return "The guest device request is invalid.";
    case CVGP_ERR_ACTIVE: return "Another session already owns these guest devices.";
    case CVGP_ERR_OWNER: return "The guest device owner is no longer current.";
    case CVGP_ERR_MISSING_DEVICE: return "A required guest input or sound device is unavailable.";
    case CVGP_ERR_BACKEND: return "The guest audio backend is unavailable.";
    case CVGP_ERR_QUEUE_FULL: return "The guest input queue is full; the session must close.";
    case CVGP_ERR_IO_WIDTH: return "This guest device supports byte I/O only.";
    case CVGP_ERR_STRING_IO: return "String I/O is not supported by this guest device.";
    case CVGP_ERR_UNSUPPORTED_PORT: return "The guest used an unsupported device port operation.";
    case CVGP_ERR_UNSUPPORTED_PIT: return "The requested timer mode is not supported.";
    case CVGP_ERR_UNSUPPORTED_DMA: return "The requested ISA DMA mode is not supported.";
    case CVGP_ERR_UNSUPPORTED_SB: return "The requested Sound Blaster operation is not supported.";
    case CVGP_ERR_MEMORY: return "The guest DMA address is outside owned guest memory.";
    default: return "The guest device reported an unknown error.";
    }
}
