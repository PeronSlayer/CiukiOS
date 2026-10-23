/* Backend selection and cyclic DMA ownership.
 * References: docs/design/f4-audio-second-set.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/arch.h>
#include <ciuki/timing.h>
#include <ciuki/audio_backend.h>
#include <ciuki/maestro.h>
#include <ciuki/emu10k1.h>
static struct mm_pin backend_pins[3];
struct audio_dma_device audio_dma;
static bool dma_selected;
/* One selected cyclic engine; IRQ and worker serialize these with irq_save. */
static bool dma_cursor_retrying;
static uint64_t dma_cursor_retry_since;
static uint32_t backend_read_io(void *c, uint16_t p, unsigned w)
{
    (void)c; if (w == 1) return inb(p); if (w == 4) return inl(p);
    uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v;
}
static void backend_write_io(void *c, uint16_t p, uint32_t v, unsigned w)
{
    (void)c; if (w == 1) outb(p, v); else if (w == 4) outl(p, v);
    else __asm__ volatile("outw %0,%1" :: "a"((uint16_t)v), "Nd"(p));
}
static uint64_t backend_now(void *c) { (void)c; return g_ticks; }
static void backend_sleep(void *c, uint32_t ms) { (void)c; task_sleep_ms(ms); }
uint32_t audio_hw_read(struct audio_dma_device *d, unsigned off, unsigned width)
{
    if (d->mmio) {
        if (d->mmio_read) return d->mmio_read(d->io.cookie, off, width);
        if (width == 1) return *(volatile uint8_t *)(d->mmio + off);
        if (width == 2) return *(volatile uint16_t *)(d->mmio + off);
        return *(volatile uint32_t *)(d->mmio + off);
    }
    return d->io.read(d->io.cookie, d->base + off, width);
}
void audio_hw_write(struct audio_dma_device *d, unsigned off, uint32_t v, unsigned width)
{
    if (d->mmio) {
        if (d->mmio_write) { d->mmio_write(d->io.cookie, off, v, width); return; }
        if (width == 1) *(volatile uint8_t *)(d->mmio + off) = v;
        else if (width == 2) *(volatile uint16_t *)(d->mmio + off) = v;
        else *(volatile uint32_t *)(d->mmio + off) = v;
        return;
    }
    d->io.write(d->io.cookie, d->base + off, v, width);
}
uint64_t audio_hw_deadline(struct audio_dma_device *d, uint32_t ms)
{
    uint64_t end=d->io.now(d->io.cookie)+ms;
    return d->activation_deadline && (int64_t)(end-d->activation_deadline)>0 ? d->activation_deadline : end;
}
int audio_hw_wait(struct audio_dma_device *d, unsigned off, uint32_t mask, uint32_t value, uint32_t ms)
{
    uint64_t end = audio_hw_deadline(d,ms);
    do {
        uint32_t got = audio_hw_read(d, off, 4);
        if (got == UINT32_MAX) return -EIO;
        if ((got & mask) == value) return 0;
        d->io.sleep(d->io.cookie, 1);
    } while ((int64_t)(d->io.now(d->io.cookie) - end) < 0);
    return -ETIMEDOUT;
}
/* Shared AC'97 codec policy for the two non-ICH register interfaces. Every
 * posted write is followed by the transport's completion/readback protocol. */
int audio_dma_codec_mute(struct audio_dma_device *d, bool mute, int (*write)(struct audio_dma_device *,uint16_t,uint16_t))
{
    const struct ac97_codec_profile *p=d->codec.profile ? d->codec.profile : ac97_codec_lookup(d->state.codec_id);
    uint16_t bits=mute ? (p->master_mask & 0x8080) : 0;
    int e = p->master_mask ? write(d, 2, bits) : 0;
    if (!e && p->pcm_mask) e = write(d, 0x18, bits | 0x0808);
    if (!e && (d->codec.caps & 0x10) && p->headphone_mask) e=write(d,4,bits+p->headphone_unity);
    if (!e) { d->state.master = mute ? 0x8000 : 0; d->state.pcm_volume = (mute ? 0x8000 : 0) | 0x0808; }
    return e;
}
static int dma_policy_read(void *cookie, uint16_t reg, uint16_t *value)
{
    struct audio_dma_device *d=cookie;
    return d->codec_read(d,reg,value);
}
static int dma_policy_update(void *cookie, uint16_t reg, uint16_t value, uint16_t mask)
{
    struct audio_dma_device *d=cookie;
    (void)mask; /* value already preserves the non-quirk fields */
    return d->codec_write(d,reg,value);
}
static struct ac97_codec_ops dma_policy_ops(struct audio_dma_device *d)
{ return (struct ac97_codec_ops){d,dma_policy_read,dma_policy_update}; }
int audio_dma_codec_prepare(struct audio_dma_device *d, int (*read)(struct audio_dma_device *,uint16_t,uint16_t *),
                            int (*write)(struct audio_dma_device *,uint16_t,uint16_t))
{
    static const uint16_t regs[] = {2,0x18,0x26,0x2a};
    d->codec_read=read; d->codec_write=write;
    uint16_t hi, lo, caps, ext;
    int e = read(d, 0x7c, &hi); if (!e) e = read(d, 0x7e, &lo);
    if (e || !hi || hi == 0xffff || lo == 0xffff) return e ? e : -EIO;
    d->state.codec_id = (uint32_t)hi << 16 | lo;
    e=read(d,0,&caps); if (!e) e=read(d,0x28,&ext);
    if (e) { d->reason="codec_capabilities"; return e; }
    ac97_codec_policy_init(&d->codec,&d->pci,d->state.codec_id,caps,ext);
    for (unsigned i = 0; !e && i < 4; i++) {
        e = read(d, regs[i], &d->codec_saved[i]);
        if (!e) d->codec_saved_count++;
    }
    if (e) return e;
    if ((caps & 0x10) && d->codec.profile->headphone_mask) {
        e=read(d,4,&d->codec_saved[4]); if (e) return e;
        d->codec_saved_count++;
    }
    d->codec_changed = true;
    e = write(d, 0x26, d->codec.eapd); /* AC-link/DAC powered; qualified amp polarity */
    uint64_t limit = audio_hw_deadline(d,1000);
    while (!e) {
        e = read(d, 0x26, &d->state.power);
        if (e || (d->state.power & 0xf) == 0xf) break;
        if ((int64_t)(d->io.now(d->io.cookie) - limit) >= 0) return -ETIMEDOUT;
        d->io.sleep(d->io.cookie, 1);
    }
    struct ac97_codec_ops io=dma_policy_ops(d);
    if (!e) e=ac97_codec_quirks(&d->codec,&d->pci,&io);
    if (e) { d->reason=e==-EOPNOTSUPP ? "codec_route_unsupported" : "codec_quirks"; return e; }
    if (d->codec.profile->master_mask) {
        if (!e) e = write(d, 2, 0x8101); /* reject stale zero / mute */
        if (!e) e = write(d, 2, d->codec.profile->master_mask & 0x8080);
    }
    if (!e) e = write(d, 0x2a, d->codec_saved[3] & ~0xfu); /* fixed 48 kHz, analog */
    if (!e) e = audio_dma_codec_mute(d, audio.master.mute, write);
    return e;
}
static int backend_dma_progress_locked(struct audio_dma_device *d)
{
    struct audio_transport *s = &d->state;
    s->tail_drained = false;
    if (s->quarantine || d->inhibited) return -EIO; /* thread must prove halt/reset before reuse */
    if (!s->running) return d->inhibited ? -EIO : 0;
    uint64_t now = d->io.now(d->io.cookie);
    /* Modulo cursors cannot distinguish a whole missed revolution. Fail
     * before using an ambiguous observation; never fabricate retirement. */
    unsigned rate = s->rate ? s->rate : AUDIO_RATE;
    if (now - d->last_observation >= (AUDIO_HW_SLOTS * AUDIO_QUANTUM * 1000u) / rate) {
        s->deadline_misses++; return -EIO;
    }
    unsigned frame;
    int e = d->ops->cursor(d, &frame);
    if (e == -EAGAIN) {
        if (!dma_cursor_retrying) { dma_cursor_retrying = true; dma_cursor_retry_since = now; }
        if (now - dma_cursor_retry_since <= 2 && now - d->last_progress <= 100) return -EAGAIN;
        s->deadline_misses++; return -EIO;
    }
    if (e || frame >= AUDIO_HW_SLOTS * AUDIO_QUANTUM) return -EIO;
    dma_cursor_retrying = false;
    unsigned delta = (frame + AUDIO_HW_SLOTS * AUDIO_QUANTUM - d->last_frame) % (AUDIO_HW_SLOTS * AUDIO_QUANTUM);
    uint64_t next = s->generation_fetched + delta;
    if (next >= s->published * AUDIO_QUANTUM) {
        if (!s->idle_tail) s->dma_halts++;
        /* Cyclic engines have no LVI; even a drained tail is not normal if
         * the cursor has crossed the prepared frontier. Inhibit and fail. */
        d->inhibited = true; return -EIO;
    }
    if (delta) d->last_progress = now;
    if (now - d->last_progress > 100) { s->deadline_misses++; return -EIO; }
    s->fetched += delta; s->generation_fetched = next;
    s->retired = next / AUDIO_QUANTUM; s->slot = frame / AUDIO_QUANTUM;
    s->remaining = AUDIO_QUANTUM - frame % AUDIO_QUANTUM;
    d->last_frame = frame; d->last_observation = now;
    return 0;
}
int audio_dma_progress(struct audio_dma_device *d)
{
    uint32_t flags=irq_save(); int e=backend_dma_progress_locked(d); irq_restore(flags); return e;
}
int audio_dma_publish(struct audio_dma_device *d, const struct audio_frame *pcm)
{
    struct audio_transport *s = &d->state;
    if (s->retired > s->published) return -EIO;
    if (s->quarantine || d->inhibited || s->published - s->retired >= 30) return -EAGAIN;
    unsigned slot = s->published % AUDIO_HW_SLOTS;
    if (s->running) {
        uint32_t flags = irq_save();
        int e = backend_dma_progress_locked(d);
        if (e && e != -EAGAIN) { irq_restore(flags); return e; }
        /* A cyclic engine cannot publish LVI. When its cursor is uncertain,
         * protect every frame it could have reached since the last coherent
         * observation, plus its following prefetched period. This bound is
         * only an ownership guard: never commit it as fetched/retired. */
        unsigned rate = s->rate ? s->rate : AUDIO_RATE;
        /* progress already rejects an entire ring interval (<190 ms at
         * supported rates), so this product fits 32 bits with IF clear. */
        unsigned elapsed = (unsigned)(d->io.now(d->io.cookie) - d->last_observation);
        unsigned advance = e ? (elapsed * rate + 999) / 1000 : 0;
        unsigned distance = (slot * AUDIO_QUANTUM + AUDIO_HW_SLOTS * AUDIO_QUANTUM - d->last_frame) %
            (AUDIO_HW_SLOTS * AUDIO_QUANTUM);
        bool owned = slot == d->last_frame / AUDIO_QUANTUM || distance <= advance + AUDIO_QUANTUM;
        irq_restore(flags);
        if (owned) return -EAGAIN;
    }
    memcpy(d->pcm + slot * AUDIO_QUANTUM, pcm, AUDIO_QUANTUM * sizeof(*pcm));
    __asm__ volatile("" ::: "memory");
    uint32_t flags = irq_save(); s->published++; irq_restore(flags);
    return 0;
}
int audio_dma_start(struct audio_dma_device *d)
{
    if (!d->state.ready || d->state.quarantine || d->inhibited || d->state.published < 2 ||
        d->state.published > AUDIO_MAX_LEAD) return -EIO;
    kmutex_lock(&d->register_lock);
    if (!d->state.ready || d->state.quarantine || audio.error) { kmutex_unlock(&d->register_lock); return -EIO; }
    d->state.running = true;
    int e = d->ops->start(d, false);
    d->last_observation = d->last_progress = d->io.now(d->io.cookie);
    dma_cursor_retrying = false;
    kmutex_unlock(&d->register_lock); return e;
}
int audio_dma_stop(struct audio_dma_device *d)
{
    kmutex_lock(&d->register_lock);
    if (d->state.quarantine) {
        kmutex_unlock(&d->register_lock); return -EIO;
    }
    int e = d->ops->halt(d);
    if (e || d->state.quarantine) { d->state.quarantine = true; kmutex_unlock(&d->register_lock); return -EIO; }
    d->state.running = false; d->inhibited = false;
    d->state.published = d->state.retired = d->state.generation_fetched = 0;
    d->last_frame = 0;
    dma_cursor_retrying = false;
    if (d->pcm) memset(d->pcm, 0, AUDIO_HW_BYTES);
    kmutex_unlock(&d->register_lock); return 0;
}
int audio_dma_prove(struct audio_dma_device *d)
{
    if (d->state.quarantine) return -EIO;
    memset(d->pcm, 0, AUDIO_HW_BYTES);
    d->proving = d->state.running = true;
    int e = d->ops->start(d, true);
    uint64_t deadline = audio_hw_deadline(d,500);
    bool moved = false;
    while (!e && (int64_t)(d->io.now(d->io.cookie) - deadline) < 0) {
        unsigned frame;
        e = d->ops->cursor(d, &frame);
        if (e==-EAGAIN) { e=0; d->io.sleep(d->io.cookie,1); continue; }
        if (e) break;
        if (frame && frame < AUDIO_HW_SLOTS * AUDIO_QUANTUM) { moved = true; break; }
        d->io.sleep(d->io.cookie, 1);
    }
    int stop = audio_dma_stop(d);
    d->proving = false;
    if (stop) return stop;
    if (e) return e;
    return moved ? 0 : -ETIMEDOUT;
}
static bool backend_irq(unsigned irq, const char *owner)
{
    (void)irq; (void)owner;
    struct audio_dma_device *d = &audio_dma;
    uint64_t begin = ktime_cycles();
    /* DS-XG may finish its last bank while shutdown sleeps for busy-clear.
     * Its callback owns the mapped device until detach and must acknowledge
     * that source even after READY is revoked, without rearming MODE. */
    bool stopping_ymf = d->kind == AUDIO_YMFPCI && d->irq_attached;
    if ((!d->state.ready && !d->proving && !stopping_ymf) || !d->ops->ack(d)) return false;
    d->state.irqs++;
    /* IRQ cannot sleep. An exhausted cyclic buffer is inhibited using only
     * the immediate register operations in ack(); service then proves stop. */
    if (!d->proving && d->state.running) {
        int e=audio_dma_progress(d);
        if (e && e!=-EAGAIN) { d->inhibited = true; d->ops->inhibit(d); }
    }
    audio_wake();
    uint64_t us = g_tsc_per_ms ? (ktime_cycles() - begin) * 1000 / g_tsc_per_ms : 0;
    if (us > audio.max_irq_us) audio.max_irq_us = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
    return true;
}
static void backend_isa_irq(struct trap_frame *tf) { (void)tf; backend_irq(5, "sb16"); }
static int backend_claim(struct audio_dma_device *d, unsigned slot, enum res_type type, uint32_t start, uint32_t end, bool share)
{
    int h = registry_claim(type, start, end, d->name, share);
    if (h < 0) return h;
    d->claims[slot] = h; d->generations[slot] = registry_get(h)->generation;
    return registry_activate(h, d->generations[slot]);
}
static bool backend_idle_proof(int h, gen_t g)
{ (void)h; (void)g; return !audio_dma.state.running && !audio_dma.irq_attached && !audio_dma.state.quarantine; }
static int backend_codec_restore(struct audio_dma_device *d)
{
    if (maestro_kind(d->kind)) return maestro_restore(d);
    if (emu10k1_kind(d->kind)) return emu10k1_restore(d);
    if (!d->codec_changed) return 0;
    if (d->kind == AUDIO_CMIPCI) return cmipci_restore(d);
    if (d->kind == AUDIO_SB16) return sb16_restore(d);
    if (d->kind == AUDIO_ES1370) return d->ops->mute(d, true); /* write-only AK4531: safe mute */
    static const uint16_t regs[] = {2,0x18,0x26,0x2a};
    int (*write)(struct audio_dma_device *,uint16_t,uint16_t) = d->voice_codec_write ? d->voice_codec_write :
        d->kind == AUDIO_ES1371 ? es137x_codec_write :
        d->kind == AUDIO_YMFPCI ? ymfpci_codec_write : via82xx_codec_write;
    /* Restore rate mode while still powered, power last. */
    static const unsigned order[] = {3,1,0,2};
    struct ac97_codec_ops io=dma_policy_ops(d);
    int e=ac97_codec_unpatch(&d->codec,&io); if (e) return e;
    if (d->codec_saved_count==5) { e=write(d,4,d->codec_saved[4]); if (e) return e; }
    for (unsigned i = 0; i < 4; i++) { unsigned at = order[i]; e = write(d, regs[at], d->codec_saved[at]); if (e) return e; }
    d->codec_changed = false; return 0;
}
void audio_backend_shutdown(void)
{
    if (!dma_selected) {
        ac97_shutdown();
        if (audio.quarantined)
            klog("[audio] failure stage=shutdown backend=ac97 reason=stop_unproved fallback=quarantine");
        return;
    }
    struct audio_dma_device *d = &audio_dma; d->state.ready = false;
    bool unsafe = d->state.quarantine;
    uint64_t previous_deadline=d->activation_deadline;
    if (d->kind==AUDIO_CMIPCI) d->activation_deadline=audio_hw_deadline(d,500);
    if (!unsafe && d->capture.phys && audio_capture_shutdown(d)) unsafe = true;
    if (d->touched) {
        if (d->state.quarantine) d->ops->inhibit(d); /* no reset after permanent quarantine */
        else if (audio_dma_stop(d)) unsafe = true;
        if (d->kind == AUDIO_CMIPCI && cmipci_capture_stop(d)) unsafe = true;
    }
    d->activation_deadline=previous_deadline;
    if (d->irq_attached) {
        /* sb_halt gates DMA5, cancels DSP auto-init and acknowledges the
         * 16-bit source. Clean detach needs no 8259 mask transaction. */
        if (d->kind == AUDIO_SB16) { irq_set_handler(5, 0); d->irq_attached = false; }
        else if (irq_chain_remove(d->state.irq, backend_irq, d->name)) unsafe = true;
        else d->irq_attached = false;
    }
    if (!unsafe && backend_codec_restore(d)) unsafe = true;
    if (!unsafe && d->kind == AUDIO_YMFPCI && ymfpci_release(d)) unsafe = true;
    if (!unsafe && d->touched && d->voice_restore && d->voice_restore(d)) unsafe = true;
    if (!unsafe && d->touched && d->kind != AUDIO_SB16) {
        if (d->kind != AUDIO_YMFPCI && d->link_changed) audio_pci_write(&d->io, &d->pci, 0x41, d->saved_link, 1);
        if (d->kind != AUDIO_YMFPCI && d->legacy_changed) audio_pci_write(&d->io, &d->pci, 0x42, d->saved_legacy, 1);
        uint32_t config=audio_pci_read(&d->io,&d->pci,0x40);
        if (d->kind != AUDIO_YMFPCI && ((d->link_changed && ((config>>8)&255)!=d->saved_link) ||
            (d->legacy_changed && ((config>>16)&255)!=d->saved_legacy))) unsafe=true;
        audio_pci_write(&d->io, &d->pci, 4, d->command & ~4u, 2);
        if ((audio_pci_read(&d->io, &d->pci, 4) & 0xffff) != (d->command & ~4u)) unsafe = true;
    }
    /* Retire pins before releasing their physical leases. A rejected unpin
     * retains the buffer and claims; never free a still-accounted DMA page. */
    if (!unsafe && mm_unpin) for (unsigned i=0;i<ARRAY_SIZE(backend_pins);i++) if (backend_pins[i].ticket) {
        if (mm_unpin(&backend_pins[i],backend_pins[i].generation)) { unsafe=true; break; }
        backend_pins[i].ticket=0;
    }
    d->state.quarantine = unsafe;
    for (unsigned i = 0; i < 8; i++) if (d->claims[i] >= 0) {
        int e = unsafe ? -EIO : registry_quiesce(d->claims[i], d->generations[i], backend_idle_proof);
        if (!e) e = registry_release(d->claims[i], d->generations[i]);
        if (e) { unsafe = d->state.quarantine = true; registry_quarantine(d->claims[i], d->generations[i]); }
        else d->claims[i] = -1;
    }
    if (unsafe) {
        if (d->kind == AUDIO_SB16) pic_mask(5); /* uncertain source: quarantine only */
        klog("[audio] failure stage=shutdown backend=%s reason=stop_unproved irq=%u fallback=quarantine",
             d->name,d->state.irq);
        audio.quarantined = true; audio.reason = "stop_unproved"; return;
    }
    if (d->mmio) { vmm_revoke_mmio((void *)d->mmio, d->bytes); d->mmio = 0; }
    if (d->phys) {
        unsigned pages=d->dma_pages ? d->dma_pages : AUDIO_HW_BYTES/PAGE_SIZE;
        for (unsigned i = 0; i < pages; i++) { mm_free(d->phys + i * PAGE_SIZE, MM_DMA); audio.commitment_pages--; }
        d->phys = 0; d->pcm = 0;
    }
    if (d->desc_phys) {
        for (unsigned i=0;i<(d->desc_pages ? d->desc_pages : 1);i++) {
            mm_free(d->desc_phys+i*PAGE_SIZE, MM_DMA); audio.commitment_pages--;
        }
        d->desc_phys = 0; d->desc = 0; d->desc_pages = 0;
    }
    if (d->cmi.phys) {
        for (unsigned i=0;i<AUDIO_HW_BYTES/PAGE_SIZE;i++) { mm_free(d->cmi.phys+i*PAGE_SIZE,MM_DMA); audio.commitment_pages--; }
        d->cmi.phys=0; d->cmi.pcm=0;
    }
    if (d->kind == AUDIO_YMFPCI) d->touched = false; /* MMIO is revoked; clean teardown is idempotent. */
}
static int backend_dma_init(struct audio_dma_device *d)
{
    if (d->state.quarantine) return -EIO;
    kmutex_init(&d->register_lock);
    int e = d->kind == AUDIO_YMFPCI ?
        backend_claim(d, 0, RES_MMIO, d->mmio_phys, d->mmio_phys + d->bytes, false) :
        backend_claim(d, 0, RES_PORT, d->base, d->base + d->bytes, false);
    if (!e && d->kind == AUDIO_YMFPCI) {
        d->mmio = vmm_map_mmio(d->mmio_phys, d->bytes, true);
        if (!d->mmio) { d->reason = "mmio_mapping_failed"; e = -ENOMEM; }
    }
    if (!e && d->kind == AUDIO_SB16) {
        /* Assignment registers are read before reset or DMA writes. */
        audio_hw_write(d, 4, 0x80, 1); unsigned irq = audio_hw_read(d, 5, 1);
        audio_hw_write(d, 4, 0x81, 1); unsigned dma = audio_hw_read(d, 5, 1);
        if (irq != 2 || dma != 0x22 || (d->io.read(d->io.cookie, 0x4d0, 1) & 0x20)) {
            d->reason = "isa_resources_unqualified"; e = -ENODEV;
        }
        if (!e) e = backend_claim(d, 2, RES_DMA, 1, 2, false);
        if (!e) e = backend_claim(d, 3, RES_DMA, 5, 6, false);
    }
    if (!e) e = backend_claim(d, 1, RES_IRQ, d->state.irq, d->state.irq + 1u, d->kind != AUDIO_SB16);
    if (!e) {
        d->dma_pages=(maestro_kind(d->kind) ? MAESTRO_ARENA_BYTES : AUDIO_HW_BYTES)/PAGE_SIZE;
        d->phys = d->kind == AUDIO_SB16 || maestro_kind(d->kind) || d->kind >= AUDIO_TRIDENT_DX ? pmm_alloc_dma_run(d->dma_pages,d->kind >= AUDIO_TRIDENT_DX ? 1 : d->dma_pages) : pmm_alloc_contig(d->dma_pages);
        if (!d->phys) e = -ENOMEM;
        else {
            mm_claim(d->phys,d->dma_pages,MM_DMA); memset(P2V(d->phys),0,d->dma_pages*PAGE_SIZE);
            d->pcm = P2V(d->phys+(maestro_kind(d->kind) ? MAESTRO_PLAY_OFFSET : 0));
            audio.commitment_pages += d->dma_pages;
        }
    }
    if (!e && d->kind >= AUDIO_TRIDENT_DX && d->phys > audio_voice_mask(d) - (AUDIO_HW_BYTES - 1))
        e = -ENOMEM;
    if (!e) e = backend_claim(d, 4, RES_MMIO, d->phys, d->phys + d->dma_pages*PAGE_SIZE, false);
    if (!e && (d->kind == AUDIO_VIA686 || d->kind == AUDIO_VIA8233 || emu10k1_kind(d->kind) || d->kind == AUDIO_YMFPCI)) {
        d->desc_pages = emu10k1_kind(d->kind) ? emu10k1_aux_pages(d->kind) : 1;
        d->desc_phys = d->desc_pages == 1 ? pmm_alloc() : pmm_alloc_contig(d->desc_pages);
        if (!d->desc_phys) e = -ENOMEM;
        else { mm_claim(d->desc_phys,d->desc_pages,MM_DMA); d->desc = P2V(d->desc_phys);
               memset(d->desc, 0, d->desc_pages*PAGE_SIZE); audio.commitment_pages+=d->desc_pages;
               e = backend_claim(d, 5, RES_MMIO, d->desc_phys, d->desc_phys + d->desc_pages*PAGE_SIZE, false); }
    }
    uint32_t dma_mask = d->kind==AUDIO_SB16 ? 0xffffffu :
                        emu10k1_kind(d->kind) ? emu10k1_dma_mask(d->kind) :
                        d->kind >= AUDIO_TRIDENT_DX ? audio_voice_mask(d) : UINT32_MAX;
    if (!e && (d->phys>dma_mask-(AUDIO_HW_BYTES-1) ||
        (d->desc_phys && d->desc_phys>dma_mask-(d->desc_pages*PAGE_SIZE-1)))) e=-ENOMEM;
    if (!e && d->kind == AUDIO_CMIPCI) {
        d->cmi.phys=pmm_alloc_contig(AUDIO_HW_BYTES/PAGE_SIZE);
        if (!d->cmi.phys) e=-ENOMEM;
        else {
            mm_claim(d->cmi.phys,AUDIO_HW_BYTES/PAGE_SIZE,MM_DMA);
            d->cmi.pcm=P2V(d->cmi.phys); memset(d->cmi.pcm,0,AUDIO_HW_BYTES);
            audio.commitment_pages+=AUDIO_HW_BYTES/PAGE_SIZE;
            e=backend_claim(d,5,RES_MMIO,d->cmi.phys,d->cmi.phys+AUDIO_HW_BYTES,false);
        }
    }
    if (!e && mm_pin) {
        memset(backend_pins,0,sizeof(backend_pins));
        uint32_t generation=d->generations[1];
        backend_pins[0]=(struct mm_pin){.generation=generation,.first_pfn=d->phys/PAGE_SIZE,
            .pages=d->dma_pages,.address_mask=maestro_kind(d->kind) ? 0x0fffffffu : dma_mask,
            .alignment=maestro_kind(d->kind) ? MAESTRO_ARENA_BYTES : d->kind==AUDIO_SB16 ? AUDIO_HW_BYTES : 4,
            .boundary=maestro_kind(d->kind) ? 0x400000u : d->kind==AUDIO_SB16 ? 131072u : 0,
            .direction=maestro_kind(d->kind) ? MM_PIN_BIDIRECTIONAL : MM_PIN_TO_DEVICE};
        e=mm_pin(&backend_pins[0]);
        if (!e && d->desc_phys) {
            backend_pins[1]=(struct mm_pin){.generation=generation,.first_pfn=d->desc_phys/PAGE_SIZE,
                .pages=d->desc_pages,.address_mask=dma_mask,.alignment=emu10k1_kind(d->kind) ? PAGE_SIZE : d->kind==AUDIO_YMFPCI ? 256 : 8,
                .direction=(emu10k1_kind(d->kind) || d->kind==AUDIO_YMFPCI) ? MM_PIN_BIDIRECTIONAL : MM_PIN_TO_DEVICE};
            e=mm_pin(&backend_pins[1]);
        }
        if (!e && d->cmi.phys) {
            backend_pins[2]=(struct mm_pin){.generation=generation,.first_pfn=d->cmi.phys/PAGE_SIZE,
                .pages=AUDIO_HW_BYTES/PAGE_SIZE,.address_mask=UINT32_MAX,.alignment=4,.direction=MM_PIN_FROM_DEVICE};
            e=mm_pin(&backend_pins[2]);
        }
    }
    if (!e) {
        d->activation_deadline=audio_hw_deadline(d,7000);
        e = d->ops->prepare(d); audio.backend_touched = d->touched;
    }
    if (!e) e = audio_dma_prove(d);
    if (!e && d->kind == AUDIO_CMIPCI) e=cmipci_capture_prove(d);
    if (!e && d->kind == AUDIO_CMIPCI)
        klog("[audio] cmipci model=%u playback=stereo48k capture=private legacy=disabled",d->cmi.model);
    if (!e) {
        d->state.ready = true;
        if (d->kind == AUDIO_SB16) { irq_set_handler(5, backend_isa_irq); d->irq_attached = true; pic_unmask(5); }
        else { e = irq_chain_add(d->state.irq, backend_irq, d->name); d->irq_attached = !e; }
    }
    d->activation_deadline=0; /* rollback has its own bounded stop budget */
    if (e) {
        audio.reason = d->reason ? d->reason : "activation_failed";
        audio_backend_shutdown(); return e;
    }
    uint32_t generation = gen_alloc();
    if (!generation) { audio.reason = "generation_exhausted"; audio_backend_shutdown(); return -ENOMEM; }
    audio.caps = (struct ciuki_audio_caps){ .version=1,.size=80,.formats=3,.min_rate=8000,.max_rate=48000,
        .output_rate=d->state.rate,.max_streams=8,.ring_frames=2048,.period_frames=256,.published_periods=8,
        .backend=d->kind,.generation=generation,.vendor=d->pci.vendor,.device=d->pci.device,
        .subsystem=(uint32_t)d->pci.sub_vendor | (uint32_t)d->pci.sub_device << 16,.revision=d->pci.revision,
        .codec_id=d->state.codec_id,.drain_ms=20 };
    audio.reason = "ready"; audio.master_in_codec = false;
    klog("[audio] backend=%s controller=%04x:%04x codec=%08x rate=%u irq=%u silent_dma=proved",
         d->name,d->pci.vendor,d->pci.device,d->state.codec_id,d->state.rate,d->state.irq);
    return 0;
}
int audio_backend_init(void)
{
    /* Quarantined DMA retains its original owner; never select a fallback. */
    if (audio.quarantined || audio_dma.state.quarantine) { audio.reason = "stop_unproved"; return -EIO; }
    dma_selected = false;
    int e = ac97_init();
    if (e != -ENODEV || ac97.pci.vendor) {
        audio.backend_name = "ac97"; audio.backend_pci = ac97.pci; audio.backend_touched = ac97.touched;
        audio.master_in_codec = !ac97.software_master;
        return e;
    }
    struct audio_dma_device *d = &audio_dma; memset(d, 0, sizeof(*d));
    for (unsigned i = 0; i < 8; i++) d->claims[i] = -1;
    d->io = (struct ac97_io){0,backend_read_io,backend_write_io,backend_now,backend_sleep};
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *p = pci_get(i);
        if (es137x_match(p, &d->kind)) { d->ops = &es137x_ops; d->name = "es137x"; d->bytes = 64; }
        else if (via82xx_match(p, &d->kind)) { d->ops = &via82xx_ops; d->name = "via82xx"; d->bytes = 256; }
        else if (maestro_match(p, &d->kind)) { d->ops = &maestro_ops; d->name = "ess1978"; d->bytes = 256; }
        else if (emu10k1_match(p, &d->kind)) { d->ops = &emu10k1_ops; d->name = "emu10k1"; d->bytes = 32; }
        else if (cmipci_match(p, &d->kind)) { d->ops = &cmipci_ops; d->name = "cmipci"; d->bytes = 256; }
        else if (ymfpci_match(p, &d->kind)) { d->ops = &ymfpci_ops; d->name = "ymfpci"; d->bytes = 0x8000; }
        else if (trident_match(p, &d->kind)) { d->ops = &trident_ops; d->name = "trident"; d->bytes = 256; }
        else if (ali5451_match(p, &d->kind)) { d->ops = &ali5451_ops; d->name = "ali5451"; d->bytes = 256; }
        else continue;
        d->pci = *p; d->command = p->command; d->state.irq = p->irq_line;
        audio.backend_name=d->name; audio.backend_pci=d->pci; break;
    }
    if (!d->ops) { d->kind = AUDIO_SB16; d->ops = &sb16_ops; d->name = "sb16"; d->base = 0x220; d->bytes = 16; d->state.irq = 5; }
    else if (d->kind == AUDIO_YMFPCI) {
        audio.backend_name = d->name; audio.backend_pci = d->pci;
        e = ymfpci_resources(d);
        if (e) { audio.reason = d->reason; return e; }
    } else {
        uint32_t base = d->pci.bar[0] & ~(uint32_t)(d->bytes - 1);
        if ((d->pci.header_type & 0x7f) || !(d->pci.bar[0] & 1) || (d->pci.bar[0] & (d->bytes - 2)) || !base || base > 0x10000u - d->bytes) {
            audio.reason = "invalid_bars"; return -ENODEV;
        }
        d->base = base;
        if (!(d->command&1) && ((d->command&4) || d->kind==AUDIO_VIA686 || d->kind==AUDIO_VIA8233 || emu10k1_kind(d->kind) || d->kind==AUDIO_CMIPCI)) {
            audio.reason="firmware_state_unreadable"; return -ENODEV;
        }
        if (!audio_pci_routed(&d->io, &d->pci)) { audio.reason = "routing_unqualified"; return -ENODEV; }
    }
    dma_selected = true; audio.backend_name = d->name; audio.backend_pci = d->pci;
    return backend_dma_init(d);
}
void audio_backend_snapshot(struct audio_transport *s)
{
    if (dma_selected) { uint32_t f=irq_save(); *s=audio_dma.state; irq_restore(f); return; }
    struct ac97_device *d = &ac97;
    uint32_t flags = irq_save(); /* 64-bit IRQ counters must not tear on i686 */
    *s = (struct audio_transport){.published=d->published,.retired=d->retired,.fetched=d->fetched,
        .generation_fetched=d->generation_fetched,.frame_base=d->frame_base,.fifo_errors=d->fifo_errors,
        .dma_halts=d->dma_halts,.deadline_misses=d->deadline_misses,.irqs=d->irqs,.codec_id=d->codec_id,
        .rate=d->rate,.power=d->power,.master=d->master,.pcm_volume=d->pcm_volume,.remaining=d->cursor.remaining,
        .slot=d->cursor.civ,.prefetch=d->cursor.piv,.lvi=d->cursor.lvi,.status=d->cursor.status,
        .irq=d->pci.irq_line,.ready=d->ready,.running=d->running,.quarantine=d->quarantine,
        .idle_tail=d->idle_tail,.tail_drained=d->tail_drained};
    irq_restore(flags);
}
void audio_backend_idle(bool idle) { if (dma_selected) audio_dma.state.idle_tail=idle; else ac97.idle_tail=idle; }
void audio_backend_rebase(uint64_t frame) { if (dma_selected) audio_dma.state.frame_base=frame; else ac97.frame_base=frame; }
int audio_backend_progress(void) { return dma_selected ? audio_dma_progress(&audio_dma) : ac97_progress(&ac97); }
int audio_backend_publish(const struct audio_frame *p) { return dma_selected ? audio_dma_publish(&audio_dma,p) : ac97_publish(&ac97,p); }
int audio_backend_start(void) { return dma_selected ? audio_dma_start(&audio_dma) : ac97_start(&ac97); }
int audio_backend_stop(void) { return dma_selected ? audio_dma_stop(&audio_dma) : ac97_stop(&ac97); }
int audio_backend_master(const struct ciuki_mixer_master *m)
{
    if (!dma_selected) return ac97_set_master(&ac97,m);
    kmutex_lock(&audio_dma.register_lock);
    if (audio_dma.state.quarantine) { kmutex_unlock(&audio_dma.register_lock); return -EIO; }
    int e=audio_dma.ops->mute(&audio_dma,m->mute);
    kmutex_unlock(&audio_dma.register_lock); return e;
}
void audio_backend_deadline(void) { if (dma_selected) audio_dma.state.deadline_misses++; else ac97.deadline_misses++; }
