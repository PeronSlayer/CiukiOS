/* Private PCM voice/capture ownership. SPDX-License-Identifier: GPL-2.0-or-later
 * Voice packing adapted from Linux v6.12 sound/pci/trident/{trident.h,
 * trident_main.c}: Maintained by Jaroslav Kysela <perex@perex.cz>, originated
 * by audio@tridentmicro.com; and sound/pci/ali5451/ali5451.c:
 * Matt Wu <Matt_Wu@acersoftech.com.cn>. Ciuki adaptation: bounded kernel
 * transport, copied capture, generations, stop proof and retained quarantine.
 */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/audio_backend.h>

uint32_t audio_voice_mask(const struct audio_dma_device *d)
{ return d->kind==AUDIO_CMIPCI || d->kind==AUDIO_EMU10K2 ? UINT32_MAX :
    d->kind==AUDIO_ALI5451 || d->kind==AUDIO_EMU10K1 ? 0x7fffffffu : 0x3fffffffu; }

/* All phases, including posted-write readback, share one 100-ms budget.
 * register_lock serializes transactions; IRQ paths never use this deadline. */
uint64_t audio_voice_codec_begin(struct audio_dma_device *d)
{
    uint64_t saved = d->activation_deadline;
    d->activation_deadline = audio_hw_deadline(d, 100);
    return saved;
}
int audio_voice_codec_end(struct audio_dma_device *d, uint64_t saved, int error)
{ d->activation_deadline = saved; return error; }

int audio_voice_codec_verify(struct audio_dma_device *d, uint16_t reg, uint16_t value)
{
    uint16_t got;
    int e = d->voice_codec_read(d, reg, &got);
    unsigned mask = reg == 0x26 ? 0xff00 : reg == 2 || reg == 0x18 ? 0x9f1f : 0xffff;
    return e ? e : ((got ^ value) & mask) ? -EIO : 0;
}

void audio_voice_program(struct audio_dma_device *d, unsigned voice, uint32_t phys,
                         unsigned frames, bool muted, bool capture)
{
    uint32_t flags = irq_save();
    audio_hw_write(d, 0xa0, voice, 1); /* byte access preserves global IRQ bits */
    audio_hw_write(d, 0xe0, 0, 4); /* CSO=0, delta low byte=0 at 48 kHz */
    audio_hw_write(d, 0xe4, phys, 4);
    audio_hw_write(d, 0xe8, d->kind == AUDIO_TRIDENT_NX ?
                   0x10000000u | (frames - 1) : ((frames - 1) << 16) | 0x1000, 4);
    if (d->kind != AUDIO_ALI5451) audio_hw_write(d, 0xec, 0xffff, 4); /* FMC=3; rear/center muted */
    audio_hw_write(d, 0xf0, (capture ? 0 : 0x80000000u) | 0xf000 |
                   (muted ? 0x7fff0000u : 0), 4);
    if (d->kind == AUDIO_ALI5451) {
        audio_hw_write(d, 0xf4, 0x30000000, 4);
        audio_hw_write(d, 0xf8, 0x30000000, 4);
    }
    irq_restore(flags);
}

int audio_voice_cursor(struct audio_dma_device *d, unsigned voice, unsigned *frame)
{
    uint32_t flags = irq_save();
    audio_hw_write(d, 0xa0, voice, 1);
    uint32_t v = audio_hw_read(d, 0xe0, 4);
    irq_restore(flags);
    if (v == UINT32_MAX) return -EIO;
    *frame = d->kind == AUDIO_TRIDENT_NX ? v & 0xffffff : v >> 16;
    return *frame < AUDIO_HW_SLOTS * AUDIO_QUANTUM ? 0 : -EIO;
}

int audio_voice_halt(struct audio_dma_device *d, unsigned start, unsigned stop,
                     unsigned enable, unsigned pending, uint32_t mask, unsigned voice)
{
    uint32_t enabled = audio_hw_read(d, enable, 4);
    if (enabled == UINT32_MAX) return -EIO;
    audio_hw_write(d, enable, enabled & ~mask, 4);
    audio_hw_write(d, stop, mask, 4);
    uint64_t end = audio_hw_deadline(d, 500);
    do {
        unsigned a, b;
        uint32_t status = audio_hw_read(d, start, 4);
        uint32_t timer = audio_hw_read(d, 0xc8, 4);
        if (status != UINT32_MAX && !(status & mask) && timer != UINT32_MAX &&
            !audio_voice_cursor(d, voice, &a)) {
            d->io.sleep(d->io.cookie, 2);
            status = audio_hw_read(d, start, 4);
            uint32_t after = audio_hw_read(d, 0xc8, 4);
            if (status != UINT32_MAX && !(status & mask) && after != UINT32_MAX && after != timer &&
                !audio_voice_cursor(d, voice, &b) && a == b) {
                audio_hw_write(d, pending, mask, 4);
                return 0;
            }
        }
        d->io.sleep(d->io.cookie, 1);
    } while ((int64_t)(d->io.now(d->io.cookie) - end) < 0);
    d->reason = "dma_stop_unproved";
    return -EIO;
}

static bool capture_idle_proof(int h, gen_t generation)
{
    return audio_dma.claims[6] == h && audio_dma.generations[6] == generation &&
           !audio_dma.capture.running && !audio_dma.state.quarantine;
}

static void capture_quarantine(struct audio_dma_device *d, const char *reason)
{
    d->capture.failed = d->state.quarantine = d->inhibited = true;
    d->state.ready = false;
    d->reason = reason; audio.reason = reason; audio.quarantined = true;
    d->ops->inhibit(d);
    for (unsigned i = 0; i < 8; i++) if (d->claims[i] >= 0)
        registry_quarantine(d->claims[i], d->generations[i]);
    audio_wake();
}

/* Caller holds register_lock. Failure is permanent; never free an uncertain
 * physical target or let playback restart around a failed capture stop. */
static int capture_close_locked(struct audio_dma_device *d)
{
    struct audio_capture *c = &d->capture;
    if (!c->phys) return 0;
    if (d->state.quarantine) return -EIO;
    int e = c->running ? d->capture_ops->halt(d) : 0;
    if (!e) c->running = false;
    if (!e && c->codec_changed) {
        e = d->voice_codec_write(d, 0x1c, c->gain);
        if (!e) e = d->voice_codec_write(d, 0x1a, c->select);
    }
    if (e) {
        capture_quarantine(d, c->running ? "capture_stop_unproved" : "capture_codec_restore_failed");
        return -EIO;
    }
    if (mm_unpin && c->descriptor_pin.ticket) {
        e = mm_unpin(&c->descriptor_pin, c->descriptor_pin.generation);
        if (e) { capture_quarantine(d, "capture_unpin_failed"); return -EIO; }
    }
    if (mm_unpin && c->pin.ticket) {
        e = mm_unpin(&c->pin, c->pin.generation);
        if (e) { capture_quarantine(d, "capture_unpin_failed"); return -EIO; }
    }
    if (d->claims[6] >= 0) {
        e = registry_quiesce(d->claims[6], d->generations[6], capture_idle_proof);
        if (!e) e = registry_release(d->claims[6], d->generations[6]);
        if (e) { capture_quarantine(d, "capture_release_failed"); return -EIO; }
        d->claims[6] = -1;
    }
    for (unsigned i = 0; i < c->pages; i++) {
        mm_free(c->phys + i * PAGE_SIZE, MM_DMA); audio.commitment_pages--;
    }
    memset(c, 0, sizeof(*c));
    return 0;
}

int audio_capture_shutdown(struct audio_dma_device *d)
{
    kmutex_lock(&d->register_lock);
    int e = capture_close_locked(d);
    kmutex_unlock(&d->register_lock);
    return e;
}

int audio_capture_close(struct audio_dma_device *d, uint32_t generation)
{
    if (!generation || generation != d->capture.generation) return -EINVAL;
    kmutex_lock(&d->register_lock);
    int e = generation != d->capture.generation ? -EINVAL : capture_close_locked(d);
    kmutex_unlock(&d->register_lock);
    return e;
}

int audio_capture_open(struct audio_dma_device *d, uint32_t *generation)
{
    if (!generation || d != &audio_dma) return -EINVAL;
    if (!d->capture_ops) return -EOPNOTSUPP;
    kmutex_lock(&d->register_lock);
    struct audio_capture *c = &d->capture;
    int e = !d->state.ready || d->state.quarantine ? -ENODEV :
            c->generation ? -EBUSY : 0;
    if (!e && d->kind==AUDIO_CMIPCI && d->cmi.running) e=-EBUSY;
    if (!e && (d->kind==AUDIO_EMU10K1 || d->kind==AUDIO_EMU10K2) && d->emu.capture_running) e=-EBUSY;
    if (e) { kmutex_unlock(&d->register_lock); return e; }
    if (!e && !c->phys) {
        unsigned pages = AUDIO_HW_BYTES / PAGE_SIZE + (d->kind == AUDIO_ICH ? 1u : 0u);
        uint32_t phys = pmm_alloc_dma_run(pages, 1);
        if (!phys) e = -ENOMEM;
        else {
            c->phys = phys; c->pcm = P2V(phys); c->pages = pages;
            mm_claim(phys, pages, MM_DMA);
            audio.commitment_pages += pages;
            if (phys > audio_voice_mask(d) - (pages * PAGE_SIZE - 1)) e = -EIO;
        }
        if (!e) {
            int h = registry_claim(RES_MMIO, phys, phys + pages * PAGE_SIZE, d->name, false);
            if (h < 0) e = h;
            else {
                d->claims[6] = h; d->generations[6] = registry_get(h)->generation;
                e = registry_activate(h, d->generations[6]);
            }
        }
    }
    if (!e) {
        c->generation = gen_alloc();
        if (!c->generation) e = -ENOMEM;
    }
    if (!e && mm_pin) {
        c->pin = (struct mm_pin){.generation=c->generation,.first_pfn=c->phys/PAGE_SIZE,
            .pages=AUDIO_HW_BYTES/PAGE_SIZE,.address_mask=audio_voice_mask(d),
            .alignment=(d->kind==AUDIO_EMU10K1 || d->kind==AUDIO_EMU10K2) ? PAGE_SIZE : 4,
            .direction=MM_PIN_FROM_DEVICE};
        e = mm_pin(&c->pin);
        if (!e && d->kind == AUDIO_ICH) {
            c->descriptor_pin=(struct mm_pin){.generation=c->generation,
                .first_pfn=(c->phys+AUDIO_HW_BYTES)/PAGE_SIZE,.pages=1,
                .address_mask=audio_voice_mask(d),.alignment=8,.direction=MM_PIN_TO_DEVICE};
            e=mm_pin(&c->descriptor_pin);
        }
    }
    if (!e) e = d->voice_codec_read(d, 0x1a, &c->select);
    if (!e) e = d->voice_codec_read(d, 0x1c, &c->gain);
    if (!e) {
        c->codec_changed = true;
        e = d->capture_source ? d->capture_source(d, CIUKI_CAPTURE_LINE) :
            d->voice_codec_write(d, 0x1a, 0x0404); /* line-in L/R */
        if (!e) e = d->voice_codec_write(d, 0x1c, 0); /* unmuted 0 dB */
    }
    if (!e) {
        memset(c->pcm, 0, AUDIO_HW_BYTES);
        c->produced = c->consumed = c->last_frame = 0;
        c->failed = false; c->running = true;
        e = d->capture_ops->start(d);
        c->observed = c->progressed = d->io.now(d->io.cookie);
    }
    if (e && c->phys && !d->state.quarantine) (void)capture_close_locked(d);
    if (!e) *generation = c->generation;
    kmutex_unlock(&d->register_lock);
    return e;
}

int audio_capture_source(struct audio_dma_device *d, uint32_t generation, unsigned source)
{
    if (!generation || source>CIUKI_CAPTURE_MIX) return -EINVAL;
    if (!audio_capture_has_source(d,source)) return -EOPNOTSUPP;
    kmutex_lock(&d->register_lock);
    int e=generation!=d->capture.generation || !d->capture.running ? -EINVAL :
        d->state.quarantine ? -EIO :
        d->capture_source ? d->capture_source(d,source) :
        d->voice_codec_write(d,0x1a,source==CIUKI_CAPTURE_MIC ? 0x0000 :
            source==CIUKI_CAPTURE_LINE ? 0x0404 : source==CIUKI_CAPTURE_CD ? 0x0101 : 0x0505);
    kmutex_unlock(&d->register_lock);
    return e;
}
int audio_capture_read(struct audio_dma_device *d, uint32_t generation,
                       struct audio_frame *out, unsigned frames)
{
    if (!generation || generation != d->capture.generation || (frames && !out) ||
        frames > AUDIO_HW_SLOTS * AUDIO_QUANTUM) return -EINVAL;
    kmutex_lock(&d->register_lock);
    struct audio_capture *c = &d->capture;
    int e = generation != c->generation ? -EINVAL :
            !c->running || c->failed || d->state.quarantine ? -EIO : 0;
    if (!e && frames) {
        uint64_t now = d->io.now(d->io.cookie);
        unsigned pos;
        if (now - c->observed >= AUDIO_HW_SLOTS * AUDIO_QUANTUM * 1000u / AUDIO_RATE)
            e = -EIO;
        else e = d->capture_ops->cursor(d, &pos);
        if (!e) {
            unsigned delta = (pos + AUDIO_HW_SLOTS * AUDIO_QUANTUM - c->last_frame) %
                             (AUDIO_HW_SLOTS * AUDIO_QUANTUM);
            if (delta) c->progressed = now;
            c->produced += delta;
            c->last_frame = pos; c->observed = now;
            if (now - c->progressed > 100 ||
                c->produced - c->consumed >= (AUDIO_HW_SLOTS - 2) * AUDIO_QUANTUM) e = -EIO;
        }
        if (!e) {
            uint64_t safe = c->produced > AUDIO_QUANTUM ? c->produced - AUDIO_QUANTUM : 0;
            unsigned available = safe > c->consumed ? (unsigned)(safe - c->consumed) : 0;
            unsigned count = frames < available ? frames : available;
            for (unsigned i = 0; i < count; i++) out[i] = c->pcm[(c->consumed + i) % (AUDIO_HW_SLOTS * AUDIO_QUANTUM)];
            /* An interrupt may delay the copy. Revalidate ownership before
             * returning data; ambiguous/overwritten output is never accepted. */
            unsigned after;
            if (d->io.now(d->io.cookie) - c->observed >= AUDIO_HW_SLOTS * AUDIO_QUANTUM * 1000u / AUDIO_RATE ||
                d->capture_ops->cursor(d, &after) ||
                c->produced + (after + AUDIO_HW_SLOTS * AUDIO_QUANTUM - pos) %
                    (AUDIO_HW_SLOTS * AUDIO_QUANTUM) - c->consumed >=
                    (AUDIO_HW_SLOTS - 1) * AUDIO_QUANTUM) e = -EIO;
            else { c->consumed += count; e = count ? (int)count : -EAGAIN; }
        }
        if (e == -EIO) {
            c->failed = true;
            if (d->capture_ops->halt(d)) {
                capture_quarantine(d, "capture_stop_unproved");
            }
            else c->running = false;
        }
    }
    kmutex_unlock(&d->register_lock);
    return e;
}
