/* f3-05 T0 "DOS audio" state parts: production SB16 DSP/mixer, 8237A, OPL3
 * and the F4 queues through dosvm_io/v86_io and dosvm_tick, with a fake output
 * clock. Migrates the audio cases of src/vm/test_guest_peripherals.cpp.
 * Included by dosvm_test.c after dosvm_devices_test.inc (device_in/out). */
#define SB 0x220u
static void a_out(struct dosvm *v, uint16_t p, uint8_t b) { device_out(v, p, b); }
static uint8_t a_in(struct dosvm *v, uint16_t p) { return device_in(v, p); }
static void dsp(struct dosvm *v, uint8_t b) { CHECK(!(a_in(v, SB + 0xC) & 0x80)); a_out(v, SB + 0xC, b); }
static uint8_t dsp_read(struct dosvm *v) { CHECK(a_in(v, SB + 0xE) & 0x80); return a_in(v, SB + 0xA); }
static void dsp_reset(struct dosvm *v)
{
    a_out(v, SB + 6, 1); CHECK(a_in(v, SB + 0xC) == 0xFF); a_out(v, SB + 6, 0);
    CHECK(dsp_read(v) == 0xAA); CHECK(!(a_in(v, SB + 0xE) & 0x80));
}
static void mixer_w(struct dosvm *v, uint8_t i, uint8_t x) { a_out(v, SB + 4, i); a_out(v, SB + 5, x); }
static uint8_t mixer_r(struct dosvm *v, uint8_t i) { a_out(v, SB + 4, i); return a_in(v, SB + 5); }
static void opl_w(struct dosvm *v, uint16_t base, unsigned bank, uint8_t reg, uint8_t x)
{ a_out(v, base + bank * 2, reg); a_out(v, base + bank * 2 + 1, x); }
static void ms(unsigned n) { for (unsigned i = 0; i < n; i++) { g_ticks++; dosvm_tick(); } }
static void dma8(struct dosvm *v, unsigned ch, uint32_t address, uint16_t count, bool autoinit)
{
    static const uint8_t page[4] = {0x87, 0x83, 0x81, 0x82};
    a_out(v, 0x0A, 4 | ch); a_out(v, 0x0C, 0);
    a_out(v, ch * 2, address); a_out(v, ch * 2, address >> 8);
    a_out(v, ch * 2 + 1, count); a_out(v, ch * 2 + 1, count >> 8);
    a_out(v, page[ch], address >> 16);
    a_out(v, 0x0B, 0x48 | ch | (autoinit ? 0x10 : 0)); a_out(v, 0x0A, ch);
}
static void dma16(struct dosvm *v, unsigned ch, uint32_t address, uint16_t words, bool autoinit)
{
    static const uint8_t page[4] = {0, 0x8B, 0x89, 0x8A};
    unsigned c = ch & 3;
    a_out(v, 0xD4, 4 | c); a_out(v, 0xD8, 0);
    a_out(v, 0xC0 + c * 4, address >> 1); a_out(v, 0xC0 + c * 4, address >> 9);
    a_out(v, 0xC2 + c * 4, words); a_out(v, 0xC2 + c * 4, words >> 8);
    a_out(v, page[c], (address >> 16) & 0xFE);
    a_out(v, 0xD6, 0x48 | c | (autoinit ? 0x10 : 0)); a_out(v, 0xD4, c);
}
static struct dosvm *audio_vm_create(void)
{
    struct dosvm *v = create(); CHECK(!dosvm_boot(v)); v->monitor.vif = false;
    device_pic_reset(v, 1); device_out(v, 0x21, 0x7F); device_out(v, 0xA1, 0xFF); /* IRQ7 only */
    v->monitor.pic[0].irr = 0;
    return v;
}
static struct dosvm_audio_info ainfo(struct dosvm *v) { struct dosvm_audio_info i; CHECK(dosvm_audio_info(dosvm_key(v), &i)); return i; }

/* Fake F4 consumer: drains segments in small fetches (partial segments kept). */
struct a_seg { struct dosvm_audio_block b; int16_t pcm[4096 * 2]; unsigned n, first; };
static struct a_seg segs[80];
static unsigned nsegs;
static void drain(struct dosvm *v, uint32_t token, unsigned quantum)
{
    for (;;) {
        struct dosvm_audio_block b; int16_t buf[1024]; CHECK(quantum <= 512);
        int n = dosvm_audio_fetch(dosvm_key(v), token, &b, buf, quantum);
        if (n == -EAGAIN) return;
        CHECK(n >= 0 && (unsigned)n <= quantum);
        struct a_seg *s = nsegs && segs[nsegs - 1].b.sequence == b.sequence && segs[nsegs - 1].b.start == b.start &&
                          !segs[nsegs - 1].b.closed ? &segs[nsegs - 1] : 0;
        if (!s) { CHECK(nsegs < ARRAY_SIZE(segs)); s = &segs[nsegs++]; memset(s, 0, sizeof(*s)); s->first = b.offset; }
        CHECK(b.offset == s->first + s->n && s->n + (unsigned)n <= 4096); /* first > 0: continued from an earlier drain */
        memcpy(s->pcm + s->n * 2, buf, (size_t)n * 4); s->n += (unsigned)n; s->b = b;
        if (!n && !b.closed) return; /* open segment, nothing new */
    }
}

static void test_audio_detect(void)
{
    struct dosvm *v = audio_vm_create(), *w = create();
    dsp_reset(v);
    dsp(v, 0xE1); CHECK(dsp_read(v) == 4 && dsp_read(v) == 5);
    dsp(v, 0xE0); dsp(v, 0x55); CHECK(dsp_read(v) == 0xAA);
    dsp(v, 0xE4); dsp(v, 0x5A); dsp(v, 0xE8); CHECK(dsp_read(v) == 0x5A);
    dsp(v, 0xD8); CHECK(dsp_read(v) == 0x00);
    dsp(v, 0xD1); dsp(v, 0xD8); CHECK(dsp_read(v) == 0xFF);
    dsp(v, 0xD3); dsp(v, 0xD8); CHECK(dsp_read(v) == 0x00);
    dsp(v, 0xE3); { const char *c = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992."; while (*c) CHECK(dsp_read(v) == (uint8_t)*c++); CHECK(dsp_read(v) == 0); }
    CHECK(a_in(v, SB + 0xA) == 0); /* empty: last byte again */
    /* BLASTER=A220 I7 D1 H5 T6 (CIUKIDOS environment) agrees with 80h/81h. */
    CHECK(mixer_r(v, 0x80) == 0x04 && mixer_r(v, 0x81) == 0x22 && mixer_r(v, 0x82) == 0x20);
    CHECK(mixer_r(v, 0x30) == 0xF8 && mixer_r(v, 0x31) == 0xF8 && mixer_r(v, 0x22) == 0xFF && mixer_r(v, 0x04) == 0xFF);
    CHECK(mixer_r(v, 0x3C) == 0x1F && mixer_r(v, 0x3D) == 0x15 && mixer_r(v, 0x3E) == 0x0B && mixer_r(v, 0x44) == 0x80);
    CHECK(mixer_r(v, 0x36) == 0xF8 && mixer_r(v, 0x28) == 0xFF && mixer_r(v, 0x0A) == 0);
    mixer_w(v, 0x22, 0xA5); CHECK(mixer_r(v, 0x30) == 0xA8 && mixer_r(v, 0x31) == 0x50 && mixer_r(v, 0x22) == 0xA5);
    mixer_w(v, 0x32, 0xFF); CHECK(mixer_r(v, 0x32) == 0xF8 && (mixer_r(v, 0x04) >> 4) == 0xF);
    mixer_w(v, 0x3B, 0xFF); CHECK(mixer_r(v, 0x3B) == 0xC0);
    mixer_w(v, 0x0A, 5); CHECK(mixer_r(v, 0x0A) == 5);
    uint32_t word = 0xE834; CHECK(!v86_io(&v->monitor, SB + 4, 2, true, &word) && mixer_r(v, 0x34) == 0xE8); /* index/data word idiom */
    struct dosvm_audio_info i = ainfo(v); CHECK(i.master[0] == 0x15 && i.master[1] == 0x0A && i.voice[0] == 31 && i.fm[0] == 29);
    mixer_w(v, 0x00, 0); CHECK(mixer_r(v, 0x30) == 0xF8 && mixer_r(v, 0x34) == 0xF8 && mixer_r(v, 0x80) == 0x04);
    mixer_w(v, 0x22, 0); mixer_w(v, 0x04, 0); mixer_w(v, 0x26, 0); mixer_w(v, 0x28, 0);
    CHECK(!mixer_r(v,0x30) && !mixer_r(v,0x32) && !mixer_r(v,0x34) && !mixer_r(v,0x36));
    mixer_w(v,0x00,0);
    unsigned bad = ainfo(v).unsupported;
    mixer_w(v, 0x80, 0x02); CHECK(v->audio.sb.irq == 5 && mixer_r(v, 0x80) == 0x02);
    mixer_w(v, 0x80, 0x06); CHECK(v->audio.sb.irq == 5 && ainfo(v).unsupported == bad + 1);
    mixer_w(v, 0x80, 0x04);
    mixer_w(v, 0x81, 0x41); CHECK(v->audio.sb.dma8 == 0 && v->audio.sb.dma16 == 6 && mixer_r(v, 0x81) == 0x41);
    mixer_w(v, 0x81, 0x02); CHECK(v->audio.sb.dma8 == 1 && v->audio.sb.dma16 == 1 && mixer_r(v, 0x81) == 0x02);
    mixer_w(v, 0x81, 0x60); CHECK(v->audio.sb.dma8 == 1 && ainfo(v).unsupported == bad + 2);
    mixer_w(v, 0x81, 0x22);
    mixer_w(v, 0x82, 0xFF); CHECK(mixer_r(v, 0x82) == 0x20);
    /* Unclaimed SB offsets and wide reads are byte cycles of an 8-bit bus. */
    CHECK(a_in(v, SB + 7) == 0xFF && a_in(v, SB + 0xB) == 0xFF);
    uint32_t wide = 0; CHECK(!v86_io(&v->monitor, SB + 0xC, 2, false, &wide) && (wide & 0xFF) == 0x7F);
    /* The second VM's device is untouched by all of the above. */
    CHECK(w->audio.sb.irq == 7 && w->audio.sb.mixer[0x30] == 0xF8 && !w->audio.sb.out_count && w->audio.sb.mixer[0x34] == 0xF8);
    destroy(v, true); destroy(w, true); empty();
}

static void test_audio_dma8_single(void)
{
    /* Migrated cvgp test_sb_dma: 4 bytes, DSP 4.xx speaker off still plays. */
    struct dosvm *v = audio_vm_create(); struct dosvm_key key = dosvm_key(v); uint32_t token;
    const uint32_t physical = 0x12340; const uint8_t data[4] = {0, 64, 192, 255};
    CHECK(!dosvm_write(v, physical, data, 4));
    CHECK(!dosvm_audio_attach(key, &token) && token);
    dsp_reset(v);
    dma8(v, 1, physical, 3, false);
    dsp(v, 0xD3); dsp(v, 0x41); dsp(v, 0x1F); dsp(v, 0x40); /* 8000 Hz */
    dsp(v, 0x14); dsp(v, 3); dsp(v, 0);
    CHECK(v->audio.sb.x.active && ainfo(v).active && ainfo(v).bits == 8);
    ms(1);
    struct dosvm_audio_info i = ainfo(v);
    CHECK(!i.active && i.boundaries == 1 && i.irq_edges == 1 && i.units == 4 && i.causes == 1);
    CHECK((v->monitor.pic[0].irr & 0x80) && v86_irq_pending(&v->monitor) == 7);
    CHECK(a_in(v, 0x08) & 2); CHECK(!(a_in(v, 0x08) & 2)); /* TC bit clears on read */
    CHECK(v->audio.dma.ch[1].masked && v->audio.dma.ch[1].count == 0xFFFF);
    nsegs = 0; drain(v, token, 3);
    CHECK(nsegs == 1 && segs[0].n == 4 && segs[0].b.closed && segs[0].b.cause == 1 && segs[0].b.rate == 8000);
    CHECK(segs[0].b.flags == DOSVM_BLOCK_COMPLETE && segs[0].b.sequence == 1 && segs[0].b.generation == key.generation);
    CHECK(segs[0].b.format == 0 && segs[0].b.start == 6); /* first frame completes 125 us = 6 frames in */
    const int16_t want[4] = {-32768, -16384, 16384, 32512};
    for (unsigned k = 0; k < 4; k++) CHECK(segs[0].pcm[k * 2] == want[k] && segs[0].pcm[k * 2 + 1] == want[k]);
    CHECK(dosvm_pic_ack(&v->monitor) == 0x0F);
    CHECK(mixer_r(v, 0x82) == 0x21);
    CHECK(a_in(v, SB + 0xE) == 0x7F && mixer_r(v, 0x82) == 0x20 && ainfo(v).services == 1);
    device_out(v, 0x20, 0x20);
    dsp(v, 0xF3); CHECK(mixer_r(v, 0x82) == 0x22 && (v->monitor.pic[0].irr & 0x80) && ainfo(v).forced == 1);
    a_in(v, SB + 0xF); CHECK(mixer_r(v, 0x82) == 0x20 && (v->monitor.pic[0].irr & 0x80)); /* PIC keeps the edge */
    CHECK(dosvm_pic_ack(&v->monitor) == 0x0F); device_out(v, 0x20, 0x20);
    ms(5); CHECK(ainfo(v).frames == 4); /* a stopped DSP produces nothing */
    unsigned bad = ainfo(v).unsupported;
    dsp(v, 0x20); CHECK(dsp_read(v) == 0x80 && ainfo(v).unsupported == bad + 1); /* direct ADC midpoint only */
    dsp(v, 0x24); dsp(v, 3); dsp(v, 0); dsp(v, 0xCC); dsp(v, 0); dsp(v, 3); dsp(v, 0);
    ms(2); CHECK(!ainfo(v).active && ainfo(v).unsupported == bad + 3 && ainfo(v).boundaries == 1);
    /* An address outside the VM fails locally: no IRQ, DSP usable, VM alive. */
    CHECK(!dosvm_audio_detach(key, token) && dosvm_audio_fetch(key, token, &segs[0].b, segs[0].pcm, 1) == -EINVAL);
    dma8(v, 1, 0xFFFFF0, 3, false); dsp(v, 0xD1); dsp(v, 0x14); dsp(v, 3); dsp(v, 0);
    ms(2); i = ainfo(v);
    CHECK(!i.active && i.dma_faults == 1 && i.last_error == DOSVM_AUDIO_DMA_FAULT && i.boundaries == 1 && !(v->monitor.pic[0].irr & 0x80));
    CHECK(dosvm_valid(key) && i.frames == 4 && !i.discarded_frames);
    dsp_reset(v); CHECK(mixer_r(v, 0x82) == 0x20);
    /* A write-type channel (I/O to memory) is refused as a mode error. */
    dma8(v, 1, physical, 3, false); a_out(v, 0x0B, 0x45); dsp(v, 0x14); dsp(v, 3); dsp(v, 0); ms(1);
    CHECK(ainfo(v).last_error == DOSVM_AUDIO_DMA_MODE && ainfo(v).dma_faults == 2 && !ainfo(v).active);
    destroy(v, true); CHECK(dosvm_audio_attach(key, &token) == -EINVAL && !dosvm_audio_info(key, &i)); empty();
}

/* One complete transfer of every advertised PCM layout; checks decoding,
 * frame count, cause and nominal duration on the 48 kHz timeline. */
static void audio_mode(struct dosvm *v, uint32_t token, uint8_t cmd, uint8_t mode, unsigned units, const uint8_t *bytes, unsigned nbytes)
{
    bool bits16 = cmd < 0xC0, stereo = mode & 0x20, sign = mode & 0x10, autoinit = cmd & 4;
    uint32_t address = 0x20000;
    CHECK(!dosvm_write(v, address, bytes, nbytes));
    if (bits16) dma16(v, 5, address, (uint16_t)(nbytes / 2 - 1), autoinit); else dma8(v, 1, address, (uint16_t)(nbytes - 1), autoinit);
    uint32_t edges = ainfo(v).irq_edges, boundaries = ainfo(v).boundaries;
    dsp(v, 0x41); dsp(v, 0x56); dsp(v, 0x22); /* 22050 */
    dsp(v, cmd); dsp(v, mode); dsp(v, (uint8_t)(units - 1)); dsp(v, (uint8_t)((units - 1) >> 8));
    if (autoinit) {
        CHECK(ainfo(v).auto_init); dsp(v, bits16 ? 0xD9 : 0xDA); /* exit at the end of this block */
        for (unsigned t = 0; t < 40 && ainfo(v).active; t++) { ms(1); a_in(v, SB + (bits16 ? 0xF : 0xE)); }
    } else ms(20);
    struct dosvm_audio_info i = ainfo(v);
    CHECK(!i.active && i.irq_edges == edges + 1 && i.boundaries == boundaries + 1);
    a_in(v, SB + 0xE); a_in(v, SB + 0xF); v->monitor.pic[0].irr = 0;
    nsegs = 0; drain(v, token, 7);
    CHECK(nsegs >= 1);
    struct a_seg *s = &segs[0];
    unsigned frames = stereo ? units / 2 : units;
    CHECK(s->n == frames && s->b.closed && s->b.cause == (bits16 ? 2 : 1) && s->b.rate == 22050);
    CHECK(s->b.format == ((bits16 ? DOSVM_FORMAT_16BIT : 0) | (sign ? DOSVM_FORMAT_SIGNED : 0) | (stereo ? DOSVM_FORMAT_STEREO : 0)));
    for (unsigned f = 0; f < frames; f++) for (unsigned c = 0; c < 2; c++) {
        unsigned unit = stereo ? f * 2 + c : f;
        int32_t x;
        if (bits16) { uint16_t w = (uint16_t)(bytes[unit * 2] | (bytes[unit * 2 + 1] << 8)); x = sign ? (int16_t)w : (int32_t)w - 32768; }
        else x = sign ? (int8_t)bytes[unit] * 256 : ((int)bytes[unit] - 128) * 256;
        CHECK(s->pcm[f * 2 + c] == x);
    }
}
static void test_audio_modes(void)
{
    struct dosvm *v = audio_vm_create(); struct dosvm_key key = dosvm_key(v); uint32_t token;
    CHECK(!dosvm_audio_attach(key, &token)); dsp_reset(v);
    uint8_t bytes[64]; for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)(i * 37 + 11);
    for (unsigned bits16 = 0; bits16 < 2; bits16++) for (unsigned autoinit = 0; autoinit < 2; autoinit++)
        for (unsigned mode = 0; mode < 4; mode++) {
            uint8_t cmd = (uint8_t)((bits16 ? 0xB0 : 0xC0) | (autoinit ? 6 : 0));
            audio_mode(v, token, cmd, (uint8_t)(mode << 4), bits16 ? 16 : 32, bytes, bits16 ? 32 : 32);
        }
    /* 16-bit data on the 8-bit channel (81h without a high DMA). */
    mixer_w(v, 0x81, 0x02);
    dma8(v, 1, 0x20000, 15, false); CHECK(!dosvm_write(v, 0x20000, bytes, 16));
    dsp(v, 0xB0); dsp(v, 0x10); dsp(v, 7); dsp(v, 0); ms(2);
    nsegs = 0; drain(v, token, 64);
    CHECK(nsegs == 1 && segs[0].n == 8 && segs[0].b.cause == 2 && segs[0].pcm[2] == (int16_t)(bytes[2] | (bytes[3] << 8)));
    a_in(v, SB + 0xF); mixer_w(v, 0x81, 0x22);
    /* Legacy 14h with SB Pro stereo (mixer 0Eh) and a stereo time constant:
     * TC = 256 - 1000000/(2 x 11025) gives 11111 frames/s. */
    mixer_w(v, 0x0E, 0x02); dsp(v, 0x40); dsp(v, 211);
    dma8(v, 1, 0x20000, 7, false); dsp(v, 0x14); dsp(v, 7); dsp(v, 0); ms(2);
    nsegs = 0; drain(v, token, 64);
    CHECK(nsegs == 1 && segs[0].n == 4 && segs[0].b.rate == 11111 && segs[0].b.format == DOSVM_FORMAT_STEREO);
    CHECK(segs[0].pcm[0] == ((int)bytes[0] - 128) * 256 && segs[0].pcm[1] == ((int)bytes[1] - 128) * 256);
    mixer_w(v, 0x0E, 0); a_in(v, SB + 0xE);
    /* 80h silence: frames of zero, 8-bit cause, flagged SILENCE. */
    dsp(v, 0x41); dsp(v, 0x1F); dsp(v, 0x40); dsp(v, 0x80); dsp(v, 79); dsp(v, 0); ms(9);
    CHECK(ainfo(v).active); ms(2); CHECK(!ainfo(v).active && (ainfo(v).causes & 1));
    nsegs = 0; drain(v, token, 128);
    CHECK(nsegs == 1 && segs[0].n == 80 && (segs[0].b.flags & DOSVM_BLOCK_SILENCE) && segs[0].b.cause == 1 && !segs[0].pcm[10]);
    a_in(v, SB + 0xE);
    /* ADPCM keeps DMA, timing and IRQ (4-bit: 2 samples per byte), silent and flagged. */
    dma8(v, 1, 0x20000, 9, false); dsp(v, 0x74); dsp(v, 9); dsp(v, 0);
    ms(2); CHECK(ainfo(v).active); ms(1); CHECK(!ainfo(v).active && ainfo(v).adpcm_blocks == 1);
    nsegs = 0; drain(v, token, 128);
    CHECK(nsegs == 1 && segs[0].n == 20 && (segs[0].b.flags & DOSVM_BLOCK_UNSUPPORTED));
    a_in(v, SB + 0xE);
    /* Direct DAC: timestamped events, no PCM. */
    struct dosvm_audio_event e[4];
    while (dosvm_audio_events(key, token, e, 4) > 0) {}
    dsp(v, 0x10); dsp(v, 0x90); ms(1); dsp(v, 0x10); dsp(v, 0x70);
    CHECK(dosvm_audio_events(key, token, e, 4) == 2 && e[0].kind == DOSVM_EVENT_DAC && e[0].value == 0x90 && e[1].value == 0x70 &&
          e[1].clock == e[0].clock + 48 && e[0].generation == key.generation && ainfo(v).direct_dac == 2);
    /* High-speed 90h: commands ignored until reset; reset restores the rate. */
    dsp(v, 0x40); dsp(v, 233); dsp(v, 0x48); dsp(v, 63); dsp(v, 0);
    uint8_t ring[128]; memset(ring, 0x80, sizeof(ring)); CHECK(!dosvm_write(v, 0x20000, ring, 128));
    dma8(v, 1, 0x20000, 127, true); dsp(v, 0x90);
    CHECK(ainfo(v).highspeed && ainfo(v).active);
    unsigned ignored = ainfo(v).unsupported; dsp(v, 0xD0); dsp(v, 0xDA); CHECK(ainfo(v).unsupported == ignored + 2 && !ainfo(v).paused);
    ms(3); a_in(v, SB + 0xE);
    dsp_reset(v); CHECK(!ainfo(v).active && !ainfo(v).highspeed && v->audio.sb.rate == 1000000u / 23u && v->audio.sb.rate_from_tc);
    CHECK(mixer_r(v, 0x82) == 0x20);
    nsegs = 0; drain(v, token, 512); CHECK(nsegs >= 1);
    destroy(v, true); empty();
}

/* Simulated guest: deliver the SB IRQ when eligible, refill the half just
 * played with the next sequence number, acknowledge, EOI. */
struct a_guest { struct dosvm *v; uint32_t buffer; unsigned half, units, seq, serviced, stop_at; bool bits16, eligible; };
static unsigned a_refill_value(unsigned seq) { return 0x20 + seq; }
static void a_fill(struct a_guest *g, unsigned half, unsigned seq)
{
    uint8_t b[256]; unsigned n = g->units * (g->bits16 ? 2 : 1);
    for (unsigned i = 0; i < n; i++) b[i] = g->bits16 ? (i & 1 ? (uint8_t)seq : 0) : (uint8_t)a_refill_value(seq);
    CHECK(!dosvm_write(g->v, g->buffer + half * n, b, n));
}
static void a_service(struct a_guest *g)
{
    struct dosvm *v = g->v;
    for (unsigned guard = 0; guard < 64 && g->eligible && v86_irq_pending(&v->monitor) == 7; guard++) {
        CHECK(dosvm_pic_ack(&v->monitor) == 0x0F);
        uint8_t status = mixer_r(v, 0x82);
        CHECK(status & (g->bits16 ? 2 : 1));
        g->serviced++;
        a_fill(g, g->half, g->seq++); g->half ^= 1;
        if (g->serviced == g->stop_at) dsp(v, g->bits16 ? 0xD9 : 0xDA);
        a_in(v, SB + (g->bits16 ? 0xF : 0xE));
        device_out(v, 0x20, 0x20);
    }
}
static void test_audio_double_buffer(void)
{
    /* Timer-driven 64-unit double buffer: a responsive guest gets a service
     * opportunity every millisecond, before the DMA wraps onto that half. */
    struct dosvm *v = audio_vm_create(); struct dosvm_key key = dosvm_key(v); uint32_t token;
    CHECK(!dosvm_audio_attach(key, &token)); dsp_reset(v);
    struct a_guest g = { v, 0x30000, 0, 64, 0, 0, 31, false, true };
    a_fill(&g, 0, g.seq++); a_fill(&g, 1, g.seq++);
    dma8(v, 1, g.buffer, 127, true);
    dsp(v, 0x41); dsp(v, 0x56); dsp(v, 0x22); dsp(v, 0x48); dsp(v, 63); dsp(v, 0); dsp(v, 0x1C);
    nsegs = 0;
    for (unsigned step = 0; step < 100 && ainfo(v).active; step++) {
        ms(1); a_service(&g);
        drain(v, token, 50); /* output quantum smaller than a block: partial fetches */
    }
    a_service(&g); drain(v, token, 50);
    struct dosvm_audio_info i = ainfo(v);
    CHECK(!i.active && i.boundaries == 32 && g.serviced == 32 && i.services == 32 && !i.late && !i.stale_blocks);
    CHECK(i.units == 2048 && nsegs == 32);
    for (unsigned k = 0; k < 32; k++) {
        CHECK(segs[k].b.sequence == k + 1 && segs[k].n == 64 && segs[k].b.closed && segs[k].b.cause == 1 && segs[k].b.flags == DOSVM_BLOCK_COMPLETE);
        for (unsigned f = 0; f < 64; f++) CHECK(segs[k].pcm[f * 2] == ((int)a_refill_value(k) - 128) * 256);
        if (k) { uint64_t d = segs[k].b.start - segs[k - 1].b.start; CHECK(d == 139 || d == 140); }
    }
    CHECK(segs[31].b.start - segs[0].b.start == 4318 || segs[31].b.start - segs[0].b.start == 4319);
    /* The same program without service chances: guest
     * never eligible. Blocks keep nominal pace, late and stale are counted,
     * exactly one edge and one pending request, guest IF never forced. */
    dsp_reset(v); v->monitor.pic[0].irr = 0; nsegs = 0;
    g = (struct a_guest){ v, 0x30000, 0, 64, 0, 0, 0, false, false };
    a_fill(&g, 0, g.seq++); a_fill(&g, 1, g.seq++);
    dma8(v, 1, g.buffer, 127, true); dsp(v, 0x48); dsp(v, 63); dsp(v, 0); dsp(v, 0x1C);
    uint32_t edges = ainfo(v).irq_edges, before = ainfo(v).boundaries;
    for (unsigned t = 0; t < 100; t++) { ms(1); drain(v, token, 512); }
    i = ainfo(v);
    CHECK(i.boundaries - before >= 33 && i.boundaries - before <= 35); /* 100 ms / 2.902 ms */
    CHECK(i.irq_edges == edges + 1 && (v->monitor.pic[0].irr & 0x80) && !v->monitor.pic[0].isr && i.causes == 1 && !v->monitor.vif);
    CHECK(i.late >= 30 && i.stale_blocks >= 30 && (segs[3].b.flags & DOSVM_BLOCK_STALE) && (segs[3].b.flags & DOSVM_BLOCK_LATE));
    CHECK(segs[2].pcm[0] == ((int)a_refill_value(0) - 128) * 256); /* stale half replayed, as the card would */
    dsp_reset(v); v->monitor.pic[0].irr = 0;
    destroy(v, true); empty();
}

/* Virtual IF clear, IMR mask or ISR in service for 100 ms (T0 DOS audio row). */
static void test_audio_if_clear(void)
{
    for (unsigned variant = 0; variant < 3; variant++) {
        struct dosvm *v = audio_vm_create(), *w = audio_vm_create();
        dsp_reset(v);
        uint8_t ring[160]; memset(ring, 0x90, sizeof(ring)); CHECK(!dosvm_write(v, 0x30000, ring, sizeof(ring)));
        dma8(v, 1, 0x30000, 159, true);
        dsp(v, 0x41); dsp(v, 0x1F); dsp(v, 0x40); dsp(v, 0x48); dsp(v, 79); dsp(v, 0); dsp(v, 0x1C); /* 10 ms blocks */
        ms(10); CHECK(ainfo(v).boundaries == 1 && v86_irq_pending(&v->monitor) == 7);
        if (variant == 1) device_out(v, 0x21, 0xFF);
        if (variant == 2) { CHECK(dosvm_pic_ack(&v->monitor) == 0x0F); a_in(v, SB + 0xE); } /* ISR busy, no EOI */
        struct dosvm_audio_info b = ainfo(v); uint64_t pit = v->pit.edges;
        ms(100);
        struct dosvm_audio_info a = ainfo(v);
        CHECK(a.boundaries - b.boundaries == 10 && a.units - b.units == 800 && a.dsp_clock + 480 >= a.clock);
        CHECK(a.late >= 9 && a.irq_edges == b.irq_edges + (variant == 2) && a.causes == 1 && (v->monitor.pic[0].irr & 0x80));
        CHECK(!v->monitor.vif && v->pit.edges > pit); /* time advances in the timer hook, IF stays clear */
        CHECK(!v->monitor.stats.delivered[7]);
        /* Eligibility: STI shadow defers, then exactly one delivery. */
        if (variant == 1) device_out(v, 0x21, 0x7F);
        if (variant == 2) { CHECK(v86_irq_pending(&v->monitor) < 0); device_out(v, 0x20, 0x20); }
        CHECK(v86_irq_pending(&v->monitor) == 7);
        struct v86_frame f = {0}; f.tf.cs = 0x200; f.tf.user_ss = 0x300; f.tf.user_esp = 0x100; f.tf.eflags = V86_VM | V86_IF | 2;
        v->monitor.vif = true; v->monitor.shadow = true;
        CHECK(v86_irq_deliver(&v->monitor, &f) == 0);
        v->monitor.shadow = false;
        CHECK(v86_irq_deliver(&v->monitor, &f) == 1 && v->monitor.stats.last_vector == 0x0F && f.tf.cs == 0xF000);
        CHECK(v86_irq_deliver(&v->monitor, &f) == 0 && v->monitor.stats.delivered[7] == 1); /* no inflation */
        a_in(v, SB + 0xE); device_out(v, 0x20, 0x20);
        CHECK(ainfo(v).services >= 1 && !(ainfo(v).causes & 1));
        CHECK(!ainfo(w).boundaries && !ainfo(w).frames && !(w->monitor.pic[0].irr & 0x80)); /* other VM untouched */
        dsp_reset(v); destroy(v, true); destroy(w, true); empty();
    }
}

static void test_audio_pause_exit(void)
{
    struct dosvm *v = audio_vm_create(); struct dosvm_key key = dosvm_key(v); uint32_t token;
    CHECK(!dosvm_audio_attach(key, &token)); dsp_reset(v);
    uint8_t ring[160]; memset(ring, 0xA0, sizeof(ring)); CHECK(!dosvm_write(v, 0x30000, ring, sizeof(ring)));
    dma8(v, 1, 0x30000, 159, true);
    dsp(v, 0x41); dsp(v, 0x1F); dsp(v, 0x40); dsp(v, 0xC6); dsp(v, 0); dsp(v, 79); dsp(v, 0); /* Cxh auto 80 units */
    ms(4);
    uint32_t units = ainfo(v).units; a_out(v, 0x0C, 0);
    uint16_t count = a_in(v, 0x03); count |= (uint16_t)(a_in(v, 0x03) << 8);
    CHECK(units >= 31 && units <= 33 && count == 159 - units);
    dsp(v, 0xD5); CHECK(!ainfo(v).paused); /* 16-bit pause does not touch an 8-bit transfer */
    dsp(v, 0xD0); CHECK(ainfo(v).paused);
    ms(50); CHECK(ainfo(v).units == units && !ainfo(v).boundaries);
    a_out(v, 0x0C, 0); uint16_t frozen = a_in(v, 0x03); frozen |= (uint16_t)(a_in(v, 0x03) << 8); CHECK(frozen == count);
    dsp(v, 0xD4); CHECK(!ainfo(v).paused);
    ms(10); CHECK(ainfo(v).boundaries == 1); a_in(v, SB + 0xE);
    nsegs = 0; drain(v, token, 512);
    CHECK(nsegs >= 2 && segs[0].b.closed && !(segs[0].b.flags & DOSVM_BLOCK_COMPLETE) && (segs[1].b.flags & DOSVM_BLOCK_COMPLETE));
    CHECK(segs[0].b.sequence == segs[1].b.sequence && segs[1].b.start >= segs[0].b.start + 50 * 48);
    CHECK(segs[0].n + segs[1].n == 80);
    /* DAh then 45h: the exit is cancelled and auto-init continues. */
    dsp(v, 0xDA); dsp(v, 0x45); ms(10); a_in(v, SB + 0xE); CHECK(ainfo(v).active && ainfo(v).boundaries == 2);
    /* A new command during auto-init applies at the end of the current block
     * (Creative 3-6): a single-cycle 16-unit tail, then stop. */
    dsp(v, 0x14); dsp(v, 15); dsp(v, 0);
    CHECK(ainfo(v).active && ainfo(v).auto_init && v->audio.sb.next_valid);
    for (unsigned t = 0; t < 12; t++) { ms(1); a_in(v, SB + 0xE); }
    CHECK(!ainfo(v).active && ainfo(v).boundaries == 4 && !ainfo(v).late);
    nsegs = 0; drain(v, token, 512);
    CHECK(nsegs >= 1 && segs[nsegs - 1].b.sequence == 4 && segs[nsegs - 1].n == 16 && (segs[nsegs - 1].b.flags & DOSVM_BLOCK_COMPLETE));
    /* Reset inside a block ends its segment as ABORTED. */
    dma8(v, 1, 0x30000, 79, false); dsp(v, 0x14); dsp(v, 79); dsp(v, 0); ms(3);
    dsp_reset(v); nsegs = 0; drain(v, token, 512);
    CHECK(nsegs == 1 && segs[0].n == 24 && (segs[0].b.flags & DOSVM_BLOCK_ABORTED) && !(segs[0].b.flags & DOSVM_BLOCK_COMPLETE));
    dsp(v, 0x41); dsp(v, 0x1F); dsp(v, 0x40);
    /* 16-bit pause/continue and D9h exit. */
    uint8_t words[64]; memset(words, 0x11, sizeof(words)); CHECK(!dosvm_write(v, 0x40000, words, 64));
    dma16(v, 5, 0x40000, 31, true);
    dsp(v, 0xB6); dsp(v, 0x30); dsp(v, 15); dsp(v, 0); /* signed stereo, 16 samples = 8 frames */
    dsp(v, 0xD5); CHECK(ainfo(v).paused && ainfo(v).bits == 16 && ainfo(v).stereo); dsp(v, 0xD6);
    dsp(v, 0xD9); for (unsigned t = 0; t < 5; t++) { ms(1); a_in(v, SB + 0xF); }
    CHECK(!ainfo(v).active && (mixer_r(v, 0x82) & 2) == 0);
    destroy(v, true); empty();
}

static void test_audio_dma_controller(void)
{
    struct dosvm *v = audio_vm_create();
    struct dosvm_dma *d = &v->audio.dma;
    /* Flip-flop, base/current, readback. */
    a_out(v, 0x0C, 0); a_out(v, 0x04, 0x34); a_out(v, 0x04, 0x12);
    CHECK(d->ch[2].base_address == 0x1234 && d->ch[2].address == 0x1234);
    a_out(v, 0x0C, 0); CHECK(a_in(v, 0x04) == 0x34 && a_in(v, 0x04) == 0x12);
    a_out(v, 0x81, 0x56); CHECK(a_in(v, 0x81) == 0x56 && a_in(v, 0x80) == 0 && d->page[1] == 0x56);
    a_out(v, 0x80, 0x99); CHECK(a_in(v, 0x80) == 0x99); /* POST/extra page: storage */
    /* Request bits read back as status bits 4-7; temporary register; illegal reads. */
    a_out(v, 0x09, 0x06); CHECK((a_in(v, 0x08) & 0xF0) == 0x40); a_out(v, 0x09, 0x02);
    CHECK(a_in(v, 0x0D) == 0 && a_in(v, 0x0A) == 0xFF && a_in(v, 0xC1) == 0xFF);
    /* Mask commands, master clear. */
    a_out(v, 0x0E, 0); for (unsigned i = 0; i < 4; i++) CHECK(!d->ch[i].masked);
    a_out(v, 0x0F, 0x05); CHECK(d->ch[0].masked && !d->ch[1].masked && d->ch[2].masked && !d->ch[3].masked);
    a_out(v, 0x08, 0x04); a_out(v, 0x0D, 0); CHECK(!d->command[0] && d->ch[1].masked && d->ch[3].masked && !d->flipflop[0]);
    a_out(v, 0xDA, 0); CHECK(d->ch[4].masked && d->ch[7].masked);
    /* 8-bit page wrap: the address counter wraps inside its 64 KiB page. */
    uint8_t buf[8], tail[4] = {1, 2, 3, 4}, head[4] = {5, 6, 7, 8};
    CHECK(!dosvm_write(v, 0x2FFFE, tail, 2) && !dosvm_write(v, 0x20000, head, 4));
    dma8(v, 3, 0x2FFFE, 3, false); bool tc;
    CHECK(dosvm_dma_read(v, 3, buf, 8, &tc) == 4 && tc && buf[0] == 1 && buf[1] == 2 && buf[2] == 5 && buf[3] == 6);
    CHECK(d->ch[3].masked && d->ch[3].address == 2 && (a_in(v, 0x08) & 8));
    CHECK(dosvm_dma_read(v, 3, buf, 1, &tc) == 0); /* masked after TC: DSP would stall */
    /* Auto-init reloads at TC and stays unmasked. */
    dma8(v, 3, 0x20000, 1, true);
    CHECK(dosvm_dma_read(v, 3, buf, 3, &tc) == 2 && tc && !d->ch[3].masked && d->ch[3].address == 0);
    CHECK(dosvm_dma_read(v, 3, buf, 3, &tc) == 2 && buf[0] == 5 && buf[1] == 6);
    /* Decrement mode. */
    dma8(v, 3, 0x20003, 3, false); a_out(v, 0x0B, 0x6B);
    CHECK(dosvm_dma_read(v, 3, buf, 4, &tc) == 4 && buf[0] == 8 && buf[3] == 5 && tc);
    /* Verify transfers read nothing (open bus), cascade and disabled controllers stall. */
    dma8(v, 3, 0x20000, 1, false); a_out(v, 0x0B, 0x43);
    CHECK(dosvm_dma_read(v, 3, buf, 2, &tc) == 2 && buf[0] == 0xFF);
    dma8(v, 3, 0x20000, 1, false); a_out(v, 0x0B, 0xCB); CHECK(dosvm_dma_read(v, 3, buf, 1, &tc) == 0);
    dma8(v, 3, 0x20000, 1, false); a_out(v, 0x08, 0x04); CHECK(dosvm_dma_read(v, 3, buf, 1, &tc) == 0); a_out(v, 0x08, 0);
    /* 16-bit channel: word address and count, page bit 0 ignored, 128 KiB wrap. */
    uint8_t w[6] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15}; CHECK(!dosvm_write(v, 0x3FFFE, w, 2) && !dosvm_write(v, 0x20000, w + 2, 4));
    dma16(v, 7, 0x3FFFE, 2, false); a_out(v, 0x8A, 0x03);
    CHECK(dosvm_dma_read(v, 7, buf, 3, &tc) == 3 && tc && buf[0] == 0x10 && buf[1] == 0x11 && buf[2] == 0x12 && buf[5] == 0x15);
    CHECK((a_in(v, 0xD0) & 8) && d->ch[7].masked);
    CHECK(dosvm_dma_read(v, 4, buf, 1, &tc) == -EINVAL);
    /* Outside the VM: local failure, counted. */
    uint32_t faults = d->faults;
    dma8(v, 3, 0x900000, 1, false); CHECK(dosvm_dma_read(v, 3, buf, 2, &tc) == -EFAULT && d->faults == faults + 1);
    destroy(v, true); empty();
}

static void test_audio_opl(void)
{
    struct dosvm *v = audio_vm_create(), *w = audio_vm_create(); struct dosvm_key key = dosvm_key(v); uint32_t token;
    /* No consumer: register file still authoritative, events counted as discarded. */
    opl_w(v, 0x388, 0, 0x20, 0x01); CHECK(ainfo(v).discarded_events == 1 && v->audio.opl.regs[0x20] == 1);
    CHECK(!dosvm_audio_attach(key, &token));
    /* AdLib/Linux detection: reset, status 00h; T1=FFh started, 80 us later C0h. */
    opl_w(v, 0x388, 0, 4, 0x60); opl_w(v, 0x388, 0, 4, 0x80);
    uint8_t s1 = a_in(v, 0x388);
    CHECK((s1 & 0xE0) == 0 && !(s1 & 6)); /* OPL3: low bits 0, not 06h */
    opl_w(v, 0x388, 0, 2, 0xFF); opl_w(v, 0x388, 0, 4, 0x21);
    uint8_t s2 = 0; for (unsigned i = 0; i < 100; i++) s2 = a_in(v, 0x388); /* ISA-time delay loop */
    CHECK((s2 & 0xE0) == 0xC0);
    opl_w(v, 0x388, 0, 4, 0x60); opl_w(v, 0x388, 0, 4, 0x80); CHECK(a_in(v, 0x388) == 0);
    CHECK(a_in(v, 0x389) == 0xFF && a_in(v, 0x38A) == 0 && a_in(v, 0x22B) == 0xFF);
    /* Migrated cvgp timer case: T1 count 0 = 256 x 80 us; RST clears. */
    ms(1);
    opl_w(v, 0x388, 0, 2, 0); opl_w(v, 0x388, 0, 3, 0); opl_w(v, 0x388, 0, 4, 0x03);
    struct dosvm_audio_info i = ainfo(v);
    CHECK(i.opl_period_us[0] == 20480 && i.opl_period_us[1] == 81920);
    ms(20); CHECK(!(a_in(v, 0x388) & 0x40));
    ms(1); CHECK(a_in(v, 0x388) == 0xC0);
    ms(61); CHECK(a_in(v, 0x388) == 0xE0);
    i = ainfo(v);
    CHECK(i.opl_flag_us[0] - i.opl_start_us[0] <= 20480 && i.opl_flag_us[0] - i.opl_start_us[0] > 20400);
    CHECK(i.opl_flag_us[1] - i.opl_start_us[1] <= 81920 && i.opl_flag_us[1] - i.opl_start_us[1] > 81600);
    opl_w(v, 0x388, 0, 4, 0x43); CHECK(a_in(v, 0x388) == 0xA0); /* masking T1 clears its flag */
    opl_w(v, 0x388, 0, 4, 0x80); CHECK(a_in(v, 0x388) == 0x00);
    opl_w(v, 0x388, 0, 4, 0x00);
    /* OPL2 compatibility: with NEW clear only 105h of array 1 is reachable. */
    opl_w(v, 0x388, 1, 0xC0, 0x31); CHECK(v->audio.opl.regs[0xC0] == 0x31 && !v->audio.opl.regs[0x1C0]);
    opl_w(v, 0x388, 1, 0x05, 0x01); CHECK(ainfo(v).opl_new == 1);
    opl_w(v, 0x388, 1, 0x04, 0x3F); opl_w(v, 0x388, 1, 0xC0, 0xF1); opl_w(v, 0x388, 0, 0xBD, 0x3F);
    CHECK(ainfo(v).opl_pairs == 0x3F && v->audio.opl.regs[0x1C0] == 0xF1 && v->audio.opl.regs[0xBD] == 0x3F);
    /* SB16 mirrors: 2x0h-2x3h both arrays, 2x8h/2x9h array 0. */
    opl_w(v, SB, 1, 0xB0, 0x31); opl_w(v, SB, 0, 0xA0, 0x98); a_out(v, SB + 8, 0x40); a_out(v, SB + 9, 0x10);
    CHECK(v->audio.opl.regs[0x1B0] == 0x31 && v->audio.opl.regs[0xA0] == 0x98 && v->audio.opl.regs[0x40] == 0x10);
    CHECK(a_in(v, SB + 8) == a_in(v, 0x388) && a_in(v, SB) == 0);
    struct dosvm_audio_event e[64]; int n = dosvm_audio_events(key, token, e, 64);
    static const uint16_t regs[] = {4, 4, 2, 4, 4, 4, 2, 3, 4, 4, 4, 4, 0xC0, 0x105, 0x104, 0x1C0, 0xBD, 0x1B0, 0xA0, 0x40};
    static const uint8_t vals[] = {0x60, 0x80, 0xFF, 0x21, 0x60, 0x80, 0, 0, 3, 0x43, 0x80, 0, 0x31, 1, 0x3F, 0xF1, 0x3F, 0x31, 0x98, 0x10};
    CHECK(n == (int)ARRAY_SIZE(regs));
    for (int k = 0; k < n; k++) {
        CHECK(e[k].kind == DOSVM_EVENT_OPL && e[k].reg == regs[k] && e[k].value == vals[k] && e[k].generation == key.generation);
        if (k) CHECK(e[k].clock >= e[k - 1].clock);
    }
    CHECK(e[3].clock > e[2].clock || e[4].clock > e[3].clock); /* I/O time inside the millisecond */
    CHECK(e[n - 1].clock >= 83 * 48);
    uint8_t snapshot[512]; uint64_t clock;
    CHECK(dosvm_audio_opl_registers(key, snapshot, &clock) && snapshot[0x105] == 1 && snapshot[0x104] == 0x3F && clock == dosvm_audio_now(v));
    /* Overflow: a GAP precedes the first event after loss; the register file resyncs. */
    for (unsigned k = 0; k < DOSVM_EVENTS + 10; k++) opl_w(v, 0x388, 0, 0x60, (uint8_t)k);
    CHECK(ainfo(v).lost_events == 10 && ainfo(v).events_queued == DOSVM_EVENTS);
    struct dosvm_audio_event big[DOSVM_EVENTS];
    CHECK(dosvm_audio_events(key, token, big, DOSVM_EVENTS) == (int)DOSVM_EVENTS);
    opl_w(v, 0x388, 0, 0x61, 0x77);
    CHECK(dosvm_audio_events(key, token, e, 4) == 2 && e[0].kind == DOSVM_EVENT_GAP && e[1].reg == 0x61 && e[1].value == 0x77);
    CHECK(v->audio.opl.regs[0x60] == (uint8_t)(DOSVM_EVENTS + 9));
    /* Two chips never share state. */
    CHECK(!w->audio.opl.regs[0x105] && !w->audio.opl.regs[0xBD] && !ainfo(w).events && !(a_in(w, 0x388) & 0xE0));
    CHECK(dosvm_audio_events(dosvm_key(w), token, e, 1) == -EINVAL); /* foreign token */
    destroy(v, true); destroy(w, true); empty();
}

static void test_audio_queue_limits(void)
{
    struct dosvm *v = audio_vm_create(), *w = audio_vm_create(); struct dosvm_key key = dosvm_key(v); uint32_t token, old;
    CHECK(!dosvm_audio_attach(key, &old) && !dosvm_audio_attach(key, &token) && token != old);
    struct dosvm_audio_block b; int16_t pcm[2];
    CHECK(dosvm_audio_fetch(key, old, &b, pcm, 1) == -EINVAL && dosvm_audio_fetch(key, token, &b, pcm, 1) == -EAGAIN);
    CHECK(dosvm_audio_fetch(key, token, &b, 0, 1) == -EINVAL && dosvm_audio_detach(key, old) == -EINVAL);
    dsp_reset(v);
    /* PCM ring overrun: 45 kHz stereo 16-bit for 200 ms without draining. */
    uint8_t ring[4096]; memset(ring, 0x40, sizeof(ring)); CHECK(!dosvm_write(v, 0x40000, ring, sizeof(ring)));
    dma16(v, 5, 0x40000, 2047, true);
    dsp(v, 0x41); dsp(v, 0xAF); dsp(v, 0xC8); /* 45000 */
    dsp(v, 0xB6); dsp(v, 0x30); dsp(v, 0xFF); dsp(v, 0x07); /* 2048 samples = 1024 frames, 22.7 ms */
    for (unsigned t = 0; t < 200; t++) { ms(1); a_in(v, SB + 0xF); }
    struct dosvm_audio_info i = ainfo(v);
    CHECK(i.pcm_queued == DOSVM_PCM_FRAMES && i.overrun_frames > 0 && i.frames == i.pcm_queued + i.overrun_frames);
    CHECK(i.boundaries >= 8 && !i.late && i.max_lag <= 3 * 48);
    nsegs = 0; drain(v, token, 512);
    CHECK(nsegs >= 4 && (segs[nsegs - 1].b.flags & DOSVM_BLOCK_OVERRUN));
    /* Long timer gaps: device work per tick is bounded, the timeline is not;
     * backlog beyond one second is dropped and counted. */
    uint32_t boundaries = ainfo(v).boundaries;
    g_ticks += 3000; dosvm_tick();
    i = ainfo(v);
    CHECK(i.dropped_frames >= 2000u * 48u && i.dropped_frames <= 2001u * 48u && i.boundaries <= boundaries + 3 && i.clock - i.dsp_clock <= 1000u * 48u);
    for (unsigned t = 0; t < 40; t++) { a_in(v, SB + 0xF); ms(1); }
    CHECK(ainfo(v).clock - ainfo(v).dsp_clock < 48 * 30); /* serviced blocks drain the backlog */
    dsp_reset(v);
    /* Segment ring: 64 descriptors; further short single-cycle blocks are dropped. */
    CHECK(!dosvm_audio_detach(key, token) && !dosvm_audio_attach(key, &token));
    dsp(v, 0x41); dsp(v, 0xAF); dsp(v, 0xC8);
    for (unsigned k = 0; k < DOSVM_SEGMENTS + 4; k++) {
        dma8(v, 1, 0x40000, 1, false); dsp(v, 0x14); dsp(v, 1); dsp(v, 0); ms(1); a_in(v, SB + 0xE);
    }
    i = ainfo(v); CHECK(i.blocks_queued == DOSVM_SEGMENTS && i.overrun_frames >= 4 && i.boundaries >= DOSVM_SEGMENTS + 4);
    /* A VM stop revokes the consumer; destroy frees every queue page. */
    CHECK(!dosvm_stop(key, DOSVM_TERMINATE) && dosvm_audio_fetch(key, token, &b, pcm, 1) == -EINVAL);
    CHECK(!ainfo(w).frames && !ainfo(w).consumer);
    destroy(v, true); destroy(w, true); empty();
}

static void test_audio_tiny_auto_init(void)
{
    struct dosvm *v = audio_vm_create(); dsp_reset(v);
    dma16(v, 5, 0x30000, 511, true);
    dsp(v, 0x41); dsp(v, 0xAF); dsp(v, 0xC8); /* 45000 Hz */
    dsp(v, 0xB6); dsp(v, 0x30); dsp(v, 1); dsp(v, 0); /* one stereo frame per block */
    ms(1);
    CHECK(v->audio.sb.units == 90 && v->audio.sb.boundaries == 45 && v->audio.sb.irq_merged == 44);
    CHECK(v->audio.sb.pos == v->audio.clock && v->audio.sb.x.waiting);
    a_in(v, SB + 0xE); CHECK(v->audio.sb.causes == 2 && v->audio.sb.units == 90);
    a_in(v, SB + 0xF); CHECK(!v->audio.sb.causes && v->audio.sb.units == 90 && !v->audio.sb.x.waiting);
    g_ticks += 40; dosvm_tick(); /* a delayed physical callback catches up through every boundary */
    CHECK(v->audio.sb.units == 3690 && v->audio.sb.boundaries == 1845 && v->audio.sb.irq_edges == 2);
    CHECK(v->audio.sb.irq_merged == 1843 && v->audio.sb.pos == v->audio.clock);
    destroy(v, true); empty();
    puts("f3-40 tiny auto-init: 45 blocks/tick and 1800-block timer catch-up, one edge while asserted, acknowledgements never clock DMA PASS");
}

static void test_audio(void)
{
    test_audio_tiny_auto_init();
    test_audio_detect(); test_audio_dma8_single(); test_audio_modes(); test_audio_double_buffer();
    test_audio_if_clear(); test_audio_pause_exit(); test_audio_dma_controller(); test_audio_opl(); test_audio_queue_limits();
    puts("DOS audio: PASS (SB16 DSP 4.05 detection/mixer/80h-82h, every PCM mode, 32 fresh blocks with a fake output clock, IF/mask/ISR 100 ms, pause/exit/continue, 8237A, OPL3 timers/banks/events, queue limits)");
}
