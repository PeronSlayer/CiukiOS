/* Independent synthetic DX/NX/M5451 register devices, never hardware evidence. */
struct voice_hw {
    uint32_t cfg[64], regs[64], voices[64][8], active[2], pending[2], address;
    uint16_t codec[64];
    uint64_t tick, codec_busy_until;
    unsigned selected, capture_frame, starts, writes, codec_writes, timer, command_delay;
    bool capture_active, stop_stuck, capture_stuck, codec_busy, reject_codec, no_progress, dead_timer;
};
static struct voice_hw vh;
static bool vh_ali(void) { return audio_dma.kind == AUDIO_ALI5451; }
static bool vh_nx(void) { return audio_dma.kind == AUDIO_TRIDENT_NX; }
static uint32_t voice_read(void *cookie, uint16_t port, unsigned width)
{
    struct voice_hw *h = cookie;
    if (port == 0xcfc) return h->cfg[(h->address & 255) / 4];
    if (port == 0x4d0 || port == 0x4d1) return 255;
    unsigned off = port - audio_dma.base;
    CHECK(off < 256);
    if (off == 0x80) return h->active[0];
    if (off == 0xb4 && !vh_ali()) return h->active[1];
    if (off == 0xc8) return h->dead_timer ? 0 : h->timer;
    if (off == 0xa0 && width == 1) return h->selected;
    if (off == 0x98) return h->pending[0];
    if (off == 0xd8) return h->pending[1];
    if (off == 0xb0) return (h->pending[0] || h->pending[1] ? 0x20 : 0) | h->regs[off / 4];
    if (off == 0xc0 && width == 2) return 16383 - h->capture_frame * 2;
    if (off >= 0xe0) return h->voices[h->selected][(off - 0xe0) / 4];
    if ((h->codec_busy || h->tick<h->codec_busy_until) && (off == 0x44 || off == (vh_nx() ? 0x48u : 0x40u)))
        return vh_nx() ? 0xc00 : 0x8000;
    uint32_t v = h->regs[off / 4] >> ((off & 3) * 8);
    return width == 1 ? v & 255 : width == 2 ? v & 65535 : v;
}
static void voice_write(void *cookie, uint16_t port, uint32_t value, unsigned width)
{
    struct voice_hw *h = cookie;
    if (port == 0xcf8) { h->address = value; return; }
    if (port >= 0xcfc && port <= 0xcff) {
        unsigned reg = (h->address & 255) + port - 0xcfc;
        uint32_t mask = width == 4 ? UINT32_MAX : ((1u << (width * 8)) - 1) << ((reg & 3) * 8);
        h->cfg[reg / 4] = (h->cfg[reg / 4] & ~mask) | value << ((reg & 3) * 8);
        return;
    }
    unsigned off = port - audio_dma.base;
    CHECK(off < 256); h->writes++;
    unsigned read_port = vh_ali() ? (audio_dma.pci.revision == 2 ? 0x40 : 0x44) : vh_nx() ? 0x48 : 0x44;
    unsigned write_port = vh_nx() ? 0x44 : 0x40;
    if (off == read_port || off == write_port) {
        h->codec_busy_until=h->tick+h->command_delay;
        unsigned reg = value & 0x7e;
        bool write = off == write_port && (off != read_port || (value & 0x100));
        if (write) {
            if (vh_ali() && audio_dma.pci.revision == 2) CHECK(value & 0x100);
            if (!h->reject_codec) h->codec[reg / 2] = value >> 16;
            h->codec_writes++;
            h->regs[off / 4] = reg;
        } else h->regs[off / 4] = (uint32_t)(h->codec[reg / 2] | (reg == 0x26 ? 15 : 0)) << 16 | reg;
        return;
    }
    if (off == 0xa0 && width == 1) { h->selected = value; CHECK(value < 64); return; }
    if (off >= 0xe0) { h->voices[h->selected][(off - 0xe0) / 4] = value; return; }
    if (off == 0x80 || (off == 0xb4 && !vh_ali())) {
        unsigned bank = off == 0xb4;
        h->active[bank] |= value; h->starts++;
        return;
    }
    if (off == 0x84 || off == 0xb8) {
        unsigned bank = off == 0xb8;
        uint32_t stuck = h->stop_stuck ? value : h->capture_stuck ? value & (1u << 31) : 0;
        h->active[bank] &= ~(value & ~stuck);
        return;
    }
    if (off == 0x98 || off == 0xd8) { h->pending[off == 0xd8] &= ~value; return; }
    if (off == 0xc4 && width == 1) {
        if (value) { h->capture_active = true; h->capture_frame = 0; }
        else if (!h->capture_stuck) h->capture_active = false;
        h->regs[off / 4] = h->capture_active ? 0xf9 : 0;
        return;
    }
    unsigned shift = (off & 3) * 8;
    uint32_t mask = width == 4 ? UINT32_MAX : ((1u << (width * 8)) - 1) << shift;
    h->regs[off / 4] = (h->regs[off / 4] & ~mask) | value << shift;
    if ((!vh_nx() && !vh_ali() && off == 0x48) || (vh_nx() && off == 0x40))
        h->regs[off / 4] |= vh_nx() ? 8 : 0x10;
}
static uint64_t voice_now(void *cookie) { return ((struct voice_hw *)cookie)->tick; }
static void voice_sleep(void *cookie, uint32_t ms)
{
    struct voice_hw *h = cookie;
    h->tick += ms; if (!h->dead_timer) h->timer += ms * 48;
    if (h->no_progress) return;
    for (unsigned ch = 0; ch < 64; ch++) if (h->active[ch / 32] & (1u << (ch % 32))) {
        uint32_t *v = h->voices[ch];
        unsigned end = vh_nx() ? (v[2] & 0xffffff) + 1 : (v[2] >> 16) + 1;
        unsigned old = vh_nx() ? v[0] & 0xffffff : v[0] >> 16;
        unsigned pos = (old + ms * 48) % end;
        v[0] = vh_nx() ? (v[0] & 0xff000000) | pos : (v[0] & 65535) | pos << 16;
    }
    bool capturing = vh_ali() ? (h->active[0] & (1u << 31)) != 0 : h->capture_active;
    if (capturing) {
        struct audio_capture *c = &audio_dma.capture;
        for (unsigned i = 0; i < ms * 48; i++) {
            unsigned at = h->capture_frame++ % 8192;
            CHECK(c->pcm); c->pcm[at] = (struct audio_frame){(int16_t)at, (int16_t)-(int)at};
        }
        h->capture_frame %= 8192;
    }
}
static void voice_setup(enum audio_backend_kind kind, unsigned revision)
{
    memset(&audio_dma, 0, sizeof(audio_dma)); memset(&vh, 0, sizeof(vh));
    struct audio_dma_device *d = &audio_dma;
    d->kind = kind; d->name = kind == AUDIO_ALI5451 ? "ali5451" : "trident";
    d->base = 0x1000; d->bytes = 256; d->command = 1; d->state.irq = 11;
    d->ops = kind == AUDIO_ALI5451 ? &ali5451_ops : &trident_ops;
    d->pci = (struct pci_func){.vendor=kind == AUDIO_ALI5451 ? 0x10b9 : 0x1023,
        .device=kind == AUDIO_ALI5451 ? 0x5451 : kind == AUDIO_TRIDENT_DX ? 0x2000 : 0x2001,
        .class_code=4,.subclass=1,.revision=revision,.irq_line=11,.irq_pin=1,.command=1,.bar={0x1001}};
    vh.cfg[1] = 1; vh.cfg[0x44 / 4] = kind == AUDIO_ALI5451 ? 0x800aa : 0;
    vh.regs[(kind == AUDIO_TRIDENT_NX ? 0x40 : 0x48) / 4] =
        kind == AUDIO_ALI5451 ? 1u << 24 : kind == AUDIO_TRIDENT_NX ? 8 : 0x10;
    vh.codec[0x7c / 2] = 0x4144; vh.codec[0x7e / 2] = 0x5348;
    vh.codec[0x26 / 2] = 15; vh.codec[2 / 2] = 0x8000; vh.codec[0x18 / 2] = 0x8808;
    vh.codec[0x1a / 2] = 0x0303; vh.codec[0x1c / 2] = 0x8000;
    d->io = (struct ac97_io){&vh, voice_read, voice_write, voice_now, voice_sleep};
    for (unsigned i = 0; i < 8; i++) d->claims[i] = -1;
    second_claim_next = 12; second_claim_failure = -1;
    dma_selected = true; audio.backend_name = d->name; audio.backend_pci = d->pci;
    audio.master = (struct ciuki_mixer_master){.left=20,.right=20};
    audio.error = 0; audio.quarantined = false; audio.ready = audio.running = false;
    CHECK(!voice_pins); voice_unpin_failure=false;
}
static void voice_dispose_quarantine(void)
{
    /* Test fixture teardown only. Production permanently retains these pages. */
    for (unsigned i = 0; i < 8; i++) pmm_free(audio_dma.phys + i * PAGE_SIZE);
    if (audio_dma.capture.phys)
        for (unsigned i = 0; i < 8; i++) pmm_free(audio_dma.capture.phys + i * PAGE_SIZE);
    audio.commitment_pages = 0;
    voice_pins=0; voice_unpin_failure=false; memset(backend_pins,0,sizeof(backend_pins));
}
static void test_ali_voice_routes(void)
{
    voice_setup(AUDIO_ALI5451,2);
    uint8_t table[64]={0}; memcpy(table,"$PIR",4); table[5]=1; table[6]=64;
    table[9]=8; table[33]=5*8; table[36]=8;
    struct pci_func card={.vendor=0x10b9,.device=0x5451,.dev=5,.irq_line=11,.irq_pin=1};
    second_pci_count=1;
    second_pci[0]=(struct pci_func){.dev=1,.class_code=6,.subclass=1,.vendor=0x10b9,.device=0x1533};
    for(unsigned bridge=0;bridge<2;bridge++) {
        second_pci[0].device=bridge ? 0x1563 : 0x1533;
        table[12]=0xb9; table[13]=0x10;
        table[14]=(uint8_t)second_pci[0].device; table[15]=second_pci[0].device>>8;
        for(unsigned link=0;link<=17;link++) {
            table[34]=link; table[31]=0;
            unsigned sum=0; for(unsigned i=0;i<64;i++) sum+=table[i]; table[31]=(uint8_t)-sum;
            for(unsigned code=0;code<16;code++) {
                memset(&vh.cfg[0x48/4],0,8);
                if(link && link<=16) {
                    unsigned index=link-1, reg=0x48+index/2;
                    vh.cfg[reg/4]=code<<((reg&3)*8+(index&1)*4);
                }
                CHECK(audio_pir_route(&audio_dma.io,&card,table,sizeof(table)) ==
                      (link>=1 && link<=16 && code==9));
            }
        }
    }
    second_pci_count=0;
    puts("ALi firmware routes: M1533/M1563, all 16 links/codes, zero/out-of-range refusal, readback only PASS");
}
static void test_voice_engines(void)
{
    unsigned baseline = pages_used;
    test_ali_voice_routes();
    static struct audio_frame marker[256], captured[512];
    for (unsigned i = 0; i < 256; i++) marker[i] = (struct audio_frame){1234, -2345};
    for (unsigned variant = 0; variant < 4; variant++) {
        enum audio_backend_kind kind = variant < 2 ? AUDIO_TRIDENT_DX + variant : AUDIO_ALI5451;
        voice_setup(kind, variant == 3 ? 2 : 1);
        if(kind==AUDIO_ALI5451) vh.regs[0xd4/4]=1u<<31; /* idle PCM-in routing is not active DMA */
        struct audio_dma_device *d = &audio_dma;
        enum audio_backend_kind match = 0;
        CHECK(kind == AUDIO_ALI5451 ? ali5451_match(&d->pci, &match) : trident_match(&d->pci, &match));
        CHECK(match == kind && audio_voice_mask(d) == (kind == AUDIO_ALI5451 ? 0x7fffffffu : 0x3fffffffu));
        CHECK(!backend_dma_init(d) && d->state.ready && !d->state.running && vh.starts == 1);
        CHECK(voice_pins==1 && backend_pins[0].direction==MM_PIN_TO_DEVICE);
        uint64_t began=vh.tick; uint16_t codec_value;
        vh.codec_busy_until=vh.tick+80; vh.command_delay=80;
        CHECK(d->voice_codec_read(d,0x7c,&codec_value)==-ETIMEDOUT && vh.tick-began<=101 && !d->activation_deadline);
        began=vh.tick; vh.codec_busy_until=vh.tick+80;
        CHECK(d->voice_codec_write(d,2,0x8000)==-ETIMEDOUT && vh.tick-began<=101 && !d->activation_deadline);
        vh.codec_busy_until=vh.command_delay=0;
        CHECK(audio.caps.backend == (unsigned)kind && audio.caps.output_rate == 48000 && audio.caps.codec_id == 0x41445348);
        CHECK(!audio_backend_master(&(struct ciuki_mixer_master){.mute=1}) && (vh.codec[1] & 0x8000));
        CHECK(!audio_backend_master(&(struct ciuki_mixer_master){0}) && !(vh.codec[1] & 0x8000));
        for (unsigned i = 0; i < 8; i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start());
        unsigned voice = kind == AUDIO_ALI5451 ? 0 : 32;
        CHECK(vh.voices[voice][1] == d->phys && vh.voices[voice][4] == 0x8000f000);
        CHECK(vh.voices[voice + 1][4] == 0xfffff000);
        CHECK(vh.voices[voice][2] == (kind == AUDIO_TRIDENT_NX ? 0x10001fff : 0x1fff1000));
        for (unsigned i = 0; i < 96; i++) {
            voice_sleep(&vh, 5); CHECK(!audio_backend_progress()); CHECK(!audio_backend_publish(marker));
        }
        CHECK(d->state.fetched == 96 * 240 && d->state.retired == 90);
        uint32_t token = 0;
        CHECK(!audio_capture_open(d, &token) && token && d->capture.running);
        CHECK(voice_pins==2 && d->capture.pin.direction==MM_PIN_FROM_DEVICE);
        CHECK(pages_used == baseline + 16 && audio.commitment_pages == 16);
        CHECK(audio_capture_open(d, &token) == -EBUSY && d->capture.running);
        CHECK(audio_capture_read(d, token + 1, captured, 1) == -EINVAL);
        CHECK(audio_capture_read(d, token, captured, 1) == -EAGAIN);
        voice_sleep(&vh, 10);
        CHECK(audio_capture_read(d, token, captured, 512) == 224);
        for (unsigned i = 0; i < 224; i++) CHECK(captured[i].left == (int)i && captured[i].right == -(int)i);
        CHECK(kind == AUDIO_ALI5451 ? vh.voices[31][1] == d->capture.phys && vh.voices[31][4] == 0xf000 :
            vh.regs[0] == d->capture.phys && (vh.regs[0xc4 / 4] & 255) == 0xf9);
        CHECK(!audio_backend_stop() && d->capture.running);
        voice_sleep(&vh, 10); CHECK(audio_capture_read(d, token, captured, 512) == 512);
        CHECK(!audio_capture_close(d, token) && pages_used == baseline + 8);
        CHECK(voice_pins==1);
        CHECK(vh.codec[0x1a / 2] == 0x0303 && vh.codec[0x1c / 2] == 0x8000);
        CHECK(audio_capture_close(d, token) == -EINVAL);
        CHECK(!audio_capture_open(d, &token));
        voice_sleep(&vh, 171);
        CHECK(audio_capture_read(d, token, captured, 1) == -EIO && !d->capture.running);
        CHECK(!audio_capture_close(d, token));
        alloc_fail=0; CHECK(audio_capture_open(d,&token)==-ENOMEM && pages_used==baseline+8); alloc_fail=-1;
        second_claim_failure=second_claim_next;
        CHECK(audio_capture_open(d,&token)==-EBUSY && pages_used==baseline+8 && !d->capture.generation);
        second_claim_failure=-1;
        /* G22: real fd/devctl boundary over each voice-family capture engine. */
        int fd=file_open(client,"/dev/audio",O_RDONLY,0); CHECK(fd>=0);
        struct ciuki_audio_capture caps={.version=1,.size=sizeof(caps)};
        CHECK(!ctl(fd,AUDIO_CAPTURE_CAPS,&caps,sizeof(caps)) && caps.available && caps.rate==48000);
        CHECK(!d->capture.generation); /* querying never starts recording */
        CHECK(call(CIUKI_SYS_WRITE,fd,0,4,0,0)==-EBADF); /* no playback stream on a capture fd */
        CHECK(ctl(fd,AUDIO_SET_GAIN,&(struct ciuki_audio_gain){.version=1,.size=24},24)==-EACCES);
        vh.capture_frame=0; /* fake ADC write cursor resets with its newly programmed ring */
        caps.source=CIUKI_CAPTURE_MIC; caps.gain=50;
        CHECK(!ctl(fd,AUDIO_CAPTURE_START,&caps,sizeof(caps)) && vh.codec[0x1a/2]==0);
        CHECK(ctl(fd,AUDIO_CAPTURE_START,&caps,sizeof(caps))==-EBUSY);
        int other=file_open(client,"/dev/audio",O_RDONLY,0); CHECK(other>=0);
        CHECK(ctl(other,AUDIO_CAPTURE_START,&caps,sizeof(caps))==-EBUSY);
        struct ciuki_audio_capture_data data={.version=1,.size=sizeof(data),.frames=128};
        CHECK(ctl(other,AUDIO_CAPTURE_READ,&data,sizeof(data))==-EINVAL);
        CHECK(ctl(fd,AUDIO_CAPTURE_READ,&data,sizeof(data))==-EAGAIN);
        voice_sleep(&vh,10);
        CHECK(!ctl(fd,AUDIO_CAPTURE_READ,&data,sizeof(data)) && data.captured==128);
        for (unsigned i=0;i<data.captured;i++) CHECK(data.pcm[i*2]==(int)i/2 && data.pcm[i*2+1]==-(int)i/2);
        data.frames=129; CHECK(ctl(fd,AUDIO_CAPTURE_READ,&data,sizeof(data))==-EINVAL);
        data.frames=128; data.reserved[0]=1; CHECK(ctl(fd,AUDIO_CAPTURE_READ,&data,sizeof(data))==-EINVAL);
        data.reserved[0]=0; data.version=2; CHECK(ctl(fd,AUDIO_CAPTURE_READ,&data,sizeof(data))==-EOPNOTSUPP);
        data.version=1;
        CHECK(!file_close(client,fd) && !d->capture.generation && pages_used==baseline+8);
        caps.source=CIUKI_CAPTURE_LINE; caps.gain=100;
        CHECK(!ctl(other,AUDIO_CAPTURE_START,&caps,sizeof(caps)) && vh.codec[0x1a/2]==0x0404);
        voice_sleep(&vh,171); CHECK(ctl(other,AUDIO_CAPTURE_READ,&data,sizeof(data))==-EIO);
        CHECK(!ctl(other,AUDIO_CAPTURE_STOP,&caps,sizeof(caps)) && !d->capture.generation);
        CHECK(!ctl(other,AUDIO_CAPTURE_STOP,&caps,sizeof(caps))); /* idempotent stop */
        caps.source=2; CHECK(ctl(other,AUDIO_CAPTURE_START,&caps,sizeof(caps))==-EINVAL);
        CHECK(!file_close(client,other));
        CHECK(!audio_capture_open(d,&token)); vh.no_progress=true;
        for(unsigned j=0;j<10;j++) { voice_sleep(&vh,10); CHECK(audio_capture_read(d,token,captured,1)==-EAGAIN); }
        voice_sleep(&vh,1); CHECK(audio_capture_read(d,token,captured,1)==-EIO);
        vh.no_progress=false; CHECK(!audio_capture_close(d,token));
        for (unsigned i = 0; i < 8; i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start());
        unsigned bank = kind == AUDIO_ALI5451 ? 0 : 1;
        vh.pending[bank] = 0x82;
        CHECK(backend_irq(11, d->name) && vh.pending[bank] == 0x80);
        CHECK(!backend_irq(11, d->name) && vh.pending[bank] == 0x80);
        audio_backend_shutdown(); CHECK(!audio.quarantined && pages_used == baseline && !audio.commitment_pages);
        CHECK(!voice_pins);
        CHECK(vh.codec[1] == 0x8000 && vh.codec[0x18 / 2] == 0x8808 && !(vh.cfg[1] & 4));

        voice_setup(kind, variant == 3 ? 2 : 1); vh.no_progress = true;
        CHECK(backend_dma_init(d) == -ETIMEDOUT && !d->state.ready && pages_used == baseline);
        voice_setup(kind, variant == 3 ? 2 : 1); vh.codec_busy = true;
        CHECK(backend_dma_init(d) == -ETIMEDOUT && !vh.starts && pages_used == baseline);
        voice_setup(kind, variant == 3 ? 2 : 1); vh.reject_codec = true;
        CHECK(backend_dma_init(d) == -EIO && !vh.starts && pages_used == baseline);
        voice_setup(kind, variant == 3 ? 2 : 1); vh.active[0] = 4;
        CHECK(backend_dma_init(d) == -EBUSY && !d->touched && !vh.writes && pages_used == baseline);
        voice_setup(kind, variant == 3 ? 2 : 1);
        CHECK(!backend_dma_init(d));
        for (unsigned i = 0; i < 8; i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start());
        vh.stop_stuck = true; unsigned held = pages_used;
        audio_backend_shutdown(); CHECK(audio.quarantined && d->state.quarantine && pages_used == held);
        CHECK(audio_backend_start() == -EIO && audio_backend_publish(marker) == -EAGAIN);
        CHECK(audio_backend_init()==-EIO && d->state.quarantine);
        for(unsigned j=0;j<8;j++) if(d->claims[j]>=0) CHECK(registry_get(d->claims[j])->state==RS_QUARANTINED);
        unsigned writes = vh.codec_writes;
        CHECK(audio_backend_master(&(struct ciuki_mixer_master){0})==-EIO && vh.codec_writes==writes);
        vh.stop_stuck = false; audio_backend_shutdown();
        CHECK(pages_used == held && d->state.quarantine && vh.codec_writes == writes);
        voice_dispose_quarantine();

        voice_setup(kind,variant==3 ? 2 : 1); CHECK(!backend_dma_init(d));
        held=pages_used; voice_unpin_failure=true;
        audio_backend_shutdown(); CHECK(audio.quarantined && pages_used==held && voice_pins==1);
        CHECK(backend_pins[0].ticket && registry_get(d->claims[0])->state==RS_QUARANTINED);
        voice_dispose_quarantine();

        voice_setup(kind, variant == 3 ? 2 : 1); CHECK(!backend_dma_init(d));
        CHECK(!audio_capture_open(d, &token)); vh.capture_stuck = true; held = pages_used;
        CHECK(audio_capture_close(d, token) == -EIO && d->state.quarantine && pages_used == held);
        audio_backend_shutdown(); CHECK(audio.quarantined && pages_used == held);
        CHECK(audio_capture_open(d, &token) == -ENODEV);
        voice_dispose_quarantine();
    }
    struct pci_func invalid = {.vendor=0x10b9,.device=0x5455,.class_code=4,.subclass=1};
    enum audio_backend_kind kind;
    CHECK(!ali5451_match(&invalid, &kind)); invalid.vendor = 0x1039; invalid.device = 0x7018;
    CHECK(!trident_match(&invalid, &kind));
    dma_selected = false; memset(&audio_dma, 0, sizeof(audio_dma)); memset(&ac97, 0, sizeof(ac97));
    audio.backend_pci = (struct pci_func){0}; audio.backend_name = 0; audio.backend_touched = false;
    audio.error = 0; audio.ready = audio.running = audio.quarantined = false; memset(&audio.caps, 0, sizeof(audio.caps));
    CHECK(pages_used == baseline && !audio.commitment_pages);
    puts("f1-120 DX/NX/M5451 rev1/rev2: voice packing, codec, PCM wrap, capture copy/generations, IRQ ownership, stop and permanent quarantine PASS");
}
