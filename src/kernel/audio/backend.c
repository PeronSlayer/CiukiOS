/* Backend selection and cyclic DMA ownership. No imported source.
 * References: docs/design/f4-audio-second-set.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/arch.h>
#include <ciuki/timing.h>
#include <ciuki/audio_backend.h>
struct audio_dma_device audio_dma;
static bool dma_selected;
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
{ return d->io.read(d->io.cookie, d->base + off, width); }
void audio_hw_write(struct audio_dma_device *d, unsigned off, uint32_t v, unsigned width)
{ d->io.write(d->io.cookie, d->base + off, v, width); }
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
    int e = write(d, 2, mute ? 0x8000 : 0);
    if (!e) e = write(d, 0x18, (mute ? 0x8000 : 0) | 0x0808);
    if (!e) { d->state.master = mute ? 0x8000 : 0; d->state.pcm_volume = (mute ? 0x8000 : 0) | 0x0808; }
    return e;
}
int audio_dma_codec_prepare(struct audio_dma_device *d, int (*read)(struct audio_dma_device *,uint16_t,uint16_t *),
                            int (*write)(struct audio_dma_device *,uint16_t,uint16_t))
{
    static const uint16_t regs[] = {2,0x18,0x26,0x2a};
    uint16_t hi, lo;
    int e = read(d, 0x7c, &hi); if (!e) e = read(d, 0x7e, &lo);
    if (e || !hi || hi == 0xffff || lo == 0xffff) return e ? e : -EIO;
    d->state.codec_id = (uint32_t)hi << 16 | lo;
    for (unsigned i = 0; !e && i < 4; i++) {
        e = read(d, regs[i], &d->codec_saved[i]);
        if (!e) d->codec_saved_count++;
    }
    if (e) return e;
    d->codec_changed = true;
    e = write(d, 0x26, 0); /* AC-link and DAC powered */
    uint64_t limit = audio_hw_deadline(d,1000);
    while (!e) {
        e = read(d, 0x26, &d->state.power);
        if (e || (d->state.power & 0xf) == 0xf) break;
        if ((int64_t)(d->io.now(d->io.cookie) - limit) >= 0) return -ETIMEDOUT;
        d->io.sleep(d->io.cookie, 1);
    }
    if (!e) e = write(d, 2, 0x8101); /* reject stale zero / mute */
    if (!e) e = write(d, 2, 0x8000);
    if (!e) e = write(d, 0x2a, d->codec_saved[3] & ~0xfu); /* fixed 48 kHz, analog */
    if (!e) e = audio_dma_codec_mute(d, audio.master.mute, write);
    return e;
}
static int backend_dma_progress_locked(struct audio_dma_device *d)
{
    struct audio_transport *s = &d->state;
    s->tail_drained = false;
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
    if (e==-EAGAIN && now-d->last_observation<=2) return -EAGAIN;
    if (e || frame >= AUDIO_HW_SLOTS * AUDIO_QUANTUM) return -EIO;
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
    if (s->quarantine || d->inhibited || s->published - s->retired >= 30) return -EAGAIN;
    unsigned slot = s->published % AUDIO_HW_SLOTS;
    if (s->running) {
        int e = audio_dma_progress(d); if (e) return e;
        unsigned current = d->last_frame / AUDIO_QUANTUM;
        if (slot == current || slot == (current + 1) % AUDIO_HW_SLOTS) return -EIO;
    }
    memcpy(d->pcm + slot * AUDIO_QUANTUM, pcm, AUDIO_QUANTUM * sizeof(*pcm));
    __asm__ volatile("" ::: "memory");
    uint32_t flags = irq_save(); s->published++; irq_restore(flags);
    return 0;
}
int audio_dma_start(struct audio_dma_device *d)
{
    if (!d->state.ready || d->state.quarantine || d->inhibited || d->state.published != AUDIO_LEAD) return -EIO;
    kmutex_lock(&d->register_lock);
    if (!d->state.ready || d->state.quarantine || audio.error) { kmutex_unlock(&d->register_lock); return -EIO; }
    d->state.running = true;
    int e = d->ops->start(d, false);
    d->last_observation = d->last_progress = d->io.now(d->io.cookie);
    kmutex_unlock(&d->register_lock); return e;
}
int audio_dma_stop(struct audio_dma_device *d)
{
    kmutex_lock(&d->register_lock);
    int e = d->ops->halt(d);
    if (e || d->state.quarantine) { d->state.quarantine = true; kmutex_unlock(&d->register_lock); return -EIO; }
    d->state.running = false; d->inhibited = false;
    d->state.published = d->state.retired = d->state.generation_fetched = 0;
    d->last_frame = 0;
    if (d->pcm) memset(d->pcm, 0, AUDIO_HW_BYTES);
    kmutex_unlock(&d->register_lock); return 0;
}
int audio_dma_prove(struct audio_dma_device *d)
{
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
    if ((!d->state.ready && !d->proving) || !d->ops->ack(d)) return false;
    d->state.irqs++;
    /* IRQ cannot sleep. An exhausted cyclic buffer is inhibited using only
     * the immediate register operations in ack(); service then proves stop. */
    if (!d->proving && d->state.running) {
        int e=audio_dma_progress(d);
        if (e && e!=-EAGAIN) { d->inhibited = true; d->ops->inhibit(d); audio_fail(-EIO); }
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
    if (!d->codec_changed) return 0;
    if (d->kind == AUDIO_SB16) return sb16_restore(d);
    if (d->kind == AUDIO_ES1370) return d->ops->mute(d, true); /* write-only AK4531: safe mute */
    static const uint16_t regs[] = {2,0x18,0x26,0x2a};
    int (*write)(struct audio_dma_device *,uint16_t,uint16_t) = d->kind == AUDIO_ES1371 ? es137x_codec_write : via82xx_codec_write;
    /* Restore rate mode while still powered, power last. */
    static const unsigned order[] = {3,1,0,2};
    for (unsigned i = 0; i < 4; i++) { unsigned at = order[i]; int e = write(d, regs[at], d->codec_saved[at]); if (e) return e; }
    d->codec_changed = false; return 0;
}
void audio_backend_shutdown(void)
{
    if (!dma_selected) { ac97_shutdown(); return; }
    struct audio_dma_device *d = &audio_dma; d->state.ready = false;
    bool unsafe = d->state.quarantine;
    if (d->touched) {
        if (d->state.quarantine) d->ops->inhibit(d); /* no reset after permanent quarantine */
        else if (audio_dma_stop(d)) unsafe = true;
    }
    if (d->irq_attached) {
        if (d->kind == AUDIO_SB16) { pic_mask(5); irq_set_handler(5, 0); d->irq_attached = false; }
        else if (irq_chain_remove(d->state.irq, backend_irq, d->name)) unsafe = true;
        else d->irq_attached = false;
    }
    if (!unsafe && backend_codec_restore(d)) unsafe = true;
    if (!unsafe && d->touched && d->kind != AUDIO_SB16) {
        if (d->link_changed) audio_pci_write(&d->io, &d->pci, 0x41, d->saved_link, 1);
        if (d->legacy_changed) audio_pci_write(&d->io, &d->pci, 0x42, d->saved_legacy, 1);
        uint32_t config=audio_pci_read(&d->io,&d->pci,0x40);
        if ((d->link_changed && ((config>>8)&255)!=d->saved_link) ||
            (d->legacy_changed && ((config>>16)&255)!=d->saved_legacy)) unsafe=true;
        audio_pci_write(&d->io, &d->pci, 4, d->command & ~4u, 2);
        if ((audio_pci_read(&d->io, &d->pci, 4) & 0xffff) != (d->command & ~4u)) unsafe = true;
    }
    d->state.quarantine = unsafe;
    for (unsigned i = 0; i < 8; i++) if (d->claims[i] >= 0) {
        int e = unsafe ? -EIO : registry_quiesce(d->claims[i], d->generations[i], backend_idle_proof);
        if (!e) e = registry_release(d->claims[i], d->generations[i]);
        if (e) { unsafe = d->state.quarantine = true; registry_quarantine(d->claims[i], d->generations[i]); }
        else d->claims[i] = -1;
    }
    if (unsafe) { audio.quarantined = true; audio.reason = "stop_unproved"; return; }
    if (d->phys) {
        for (unsigned i = 0; i < AUDIO_HW_BYTES / PAGE_SIZE; i++) { pmm_free(d->phys + i * PAGE_SIZE); audio.commitment_pages--; }
        d->phys = 0; d->pcm = 0;
    }
    if (d->desc_phys) { pmm_free(d->desc_phys); audio.commitment_pages--; d->desc_phys = 0; d->desc = 0; }
}
static int backend_dma_init(struct audio_dma_device *d)
{
    kmutex_init(&d->register_lock);
    int e = backend_claim(d, 0, RES_PORT, d->base, d->base + d->bytes, false);
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
        d->phys = d->kind == AUDIO_SB16 ? pmm_alloc_dma_run(AUDIO_HW_BYTES / PAGE_SIZE, AUDIO_HW_BYTES / PAGE_SIZE) : pmm_alloc_contig(AUDIO_HW_BYTES / PAGE_SIZE);
        if (!d->phys) e = -ENOMEM;
        else { d->pcm = P2V(d->phys); memset(d->pcm, 0, AUDIO_HW_BYTES); audio.commitment_pages += AUDIO_HW_BYTES / PAGE_SIZE; }
    }
    if (!e) e = backend_claim(d, 4, RES_MMIO, d->phys, d->phys + AUDIO_HW_BYTES, false);
    if (!e && (d->kind == AUDIO_VIA686 || d->kind == AUDIO_VIA8233)) {
        d->desc_phys = pmm_alloc();
        if (!d->desc_phys) e = -ENOMEM;
        else { d->desc = P2V(d->desc_phys); memset(d->desc, 0, PAGE_SIZE); audio.commitment_pages++;
               e = backend_claim(d, 5, RES_MMIO, d->desc_phys, d->desc_phys + PAGE_SIZE, false); }
    }
    if (!e) {
        d->activation_deadline=audio_hw_deadline(d,7000);
        e = d->ops->prepare(d); audio.backend_touched = d->touched;
    }
    if (!e) e = audio_dma_prove(d);
    if (!e) {
        d->state.ready = true;
        if (d->kind == AUDIO_SB16) { pic_mask(5); irq_set_handler(5, backend_isa_irq); d->irq_attached = true; pic_unmask(5); }
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
        else continue;
        d->pci = *p; d->command = p->command; d->state.irq = p->irq_line; break;
    }
    if (!d->ops) { d->kind = AUDIO_SB16; d->ops = &sb16_ops; d->name = "sb16"; d->base = 0x220; d->bytes = 16; d->state.irq = 5; }
    else {
        uint32_t base = d->pci.bar[0] & ~(uint32_t)(d->bytes - 1);
        if ((d->pci.header_type & 0x7f) || !(d->pci.bar[0] & 1) || (d->pci.bar[0] & (d->bytes - 2)) || !base || base > 0x10000u - d->bytes) {
            audio.reason = "invalid_bars"; return -ENODEV;
        }
        d->base = base;
        if (!(d->command&1) && ((d->command&4) || d->kind>=AUDIO_VIA686)) {
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
    *s = (struct audio_transport){.published=d->published,.retired=d->retired,.fetched=d->fetched,
        .generation_fetched=d->generation_fetched,.frame_base=d->frame_base,.fifo_errors=d->fifo_errors,
        .dma_halts=d->dma_halts,.deadline_misses=d->deadline_misses,.irqs=d->irqs,.codec_id=d->codec_id,
        .rate=d->rate,.power=d->power,.master=d->master,.pcm_volume=d->pcm_volume,.remaining=d->cursor.remaining,
        .slot=d->cursor.civ,.prefetch=d->cursor.piv,.status=d->cursor.status,
        .irq=d->pci.irq_line,.ready=d->ready,.running=d->running,.quarantine=d->quarantine,
        .idle_tail=d->idle_tail,.tail_drained=d->tail_drained};
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
    int e=audio_dma.ops->mute(&audio_dma,m->mute);
    kmutex_unlock(&audio_dma.register_lock); return e;
}
void audio_backend_deadline(void) { if (dma_selected) audio_dma.state.deadline_misses++; else ac97.deadline_misses++; }
