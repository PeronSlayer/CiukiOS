/* Firmware-configured PCI IDE bus master, SFF-8038i/Intel IDEMS100.
 * Contract and sources: docs/design/vfs-storage-contract.md, F1-44.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/ata.h>

static struct mm_pin ata_pins[ATA_CHANNELS][2];

_Static_assert(sizeof(struct ata_prd) == 8, "SFF PRD layout");

int ata_dma_prd(struct ata_prd *prd, unsigned capacity, uint32_t phys, uint32_t bytes)
{
    if (!prd || !bytes || (phys & 1) || (bytes & 1) || bytes > ATA_DMA_BYTES ||
        (uint64_t)phys + bytes > (1ull << 32)) return -FS_EINVAL;
    unsigned needed = ((phys & 65535u) + bytes + 65535u) / 65536u;
    if (needed > capacity) return -FS_EINVAL;
    for (unsigned i = 0; i < needed; i++) {
        uint32_t take = 65536u - (phys & 65535u);
        if (take > bytes) take = bytes;
        prd[i] = (struct ata_prd){ phys, (uint16_t)take, i + 1 == needed ? 0x8000 : 0 };
        phys += take; bytes -= take;
    }
    return (int)needed;
}

int ata_dma_restore_mode(int mode, const uint8_t data[512])
{
    uint16_t supported = fs_rd16(data + 88 * 2) & 0x7f;
    uint16_t cable = fs_rd16(data + 93 * 2);
    if (mode < 0 || mode > 6 || !(fs_rd16(data + 49 * 2) & 0x100) || !(fs_rd16(data + 53 * 2) & 4) ||
        (supported & ((1u << (mode + 1)) - 1)) != ((1u << (mode + 1)) - 1) ||
        (mode > 2 && ((cable & 0xc000) != 0x4000 || !(cable & 0x2000)))) return -1;
    return mode;
}
int ata_dma_mode(const uint8_t data[512])
{
    uint16_t selected = fs_rd16(data + 88 * 2) >> 8;
    if (!selected || (selected & (selected - 1)) || (fs_rd16(data + 63 * 2) & 0x700)) return -1;
    unsigned mode = 0;
    while ((1u << mode) != selected) mode++;
    return ata_dma_restore_mode((int)mode, data);
}

uint16_t ata_dma_pci_base(const struct pci_func *p, unsigned channel)
{
    if (!p || channel >= ATA_CHANNELS || p->class_code != 1 || p->subclass != 1 ||
        (p->header_type & 0x7f) != 0 || !(p->prog_if & 0x80) ||
        (p->prog_if & (1u << (channel * 2))) || !(p->command & 1)) return 0;
    uint32_t bar = p->bar[4], base = bar & ~3u;
    if (!(bar & 1) || !base || (base & 15) || base > 0xfff0) return 0;
    return (uint16_t)(base + channel * 8);
}

int ata_dma_setup(struct ata_channel *c, uint16_t base, const struct ata_dma_ops *ops, void *ctx,
                  struct ata_prd *prd, uint32_t prd_phys, void *bounce, uint32_t bounce_phys)
{
    if (!c || !base || (base & 7) || base > 0xfff8 || !ops || !ops->in8 || !ops->out8 ||
        !ops->out32 || !ops->physical || !prd || ((uintptr_t)prd & 3) ||
        (prd_phys & 3) || (prd_phys & 65535u) > 65536u - ATA_DMA_PRDS * sizeof(*prd) ||
        !bounce || (bounce_phys & 1) || (uint64_t)bounce_phys + ATA_DMA_BYTES > (1ull << 32) ||
        ((uint64_t)prd_phys < (uint64_t)bounce_phys + ATA_DMA_BYTES &&
         (uint64_t)bounce_phys < (uint64_t)prd_phys + ATA_DMA_PRDS * sizeof(*prd)) ||
        c->native || c->discovered || c->stopped || c->quiescing || c->dma.ops)
        return -FS_EINVAL;
    c->dma = (struct ata_dma){ .ops = ops, .ctx = ctx, .base = base, .prd = prd,
        .prd_phys = prd_phys, .bounce = bounce, .bounce_phys = bounce_phys, .claim = -1 };
    return 0;
}

static uint8_t bm_read(void *ctx, uint16_t port) { (void)ctx; return inb(port); }
static void bm_write(void *ctx, uint16_t port, uint8_t v) { (void)ctx; outb(port, v); }
static void bm_write32(void *ctx, uint16_t port, uint32_t v) { (void)ctx; outl(port, v); }
static bool bm_physical(void *ctx, const void *buf, uint32_t bytes, bool write, uint32_t *phys)
{
    (void)ctx;
    uintptr_t va = (uintptr_t)buf;
    if (va < KERNEL_VBASE || va > UINT32_MAX || bytes > UINT32_MAX - va) return false;
    uint32_t first = 0;
    for (uint32_t off = 0; off < bytes;) {
        uint32_t address = (uint32_t)va + off, pte = vmm_kernel_pte(address);
        if (!(pte & PTE_P) || (write && !(pte & PTE_W)) || (pte & (PTE_PCD | PTE_PWT))) return false;
        uint32_t p = (pte & ~4095u) | (address & 4095u);
        if (p >= PMM_MAX_PAGES * 4096u || pmm_is_reserved(p & ~4095u)) return false;
        if (!off) first = p;
        else if (p != first + off) return false;
        uint32_t take = 4096u - (address & 4095u);
        if (take > bytes - off) take = bytes - off;
        off += take;
    }
    *phys = first;
    return true;
}
static const struct ata_dma_ops bm_ops = { bm_read, bm_write, bm_write32, bm_physical };

void ata_dma_attach(struct ata_channel *c, unsigned index)
{
    if (index >= ATA_CHANNELS) return;
    const struct pci_func *p = 0;
    uint16_t base = 0;
    for (unsigned i = 0; i < pci_count(); i++) {
        uint16_t candidate = ata_dma_pci_base(pci_get(i), index);
        if (!candidate) continue;
        if (p) return; /* ambiguous ownership, no port/config writes */
        p = pci_get(i); base = candidate;
    }
    if (!p) return;
    const char *owner = index ? "ata1" : "ata0";
    int claim = registry_claim(RES_PORT, base, base + 8u, owner, false);
    if (claim < 0) return;
    gen_t generation = registry_get((unsigned)claim)->generation;
    /* No rollback transition exists for a claimed resource. Retain its
     * quarantine on failed setup, without affecting the PIO claims. */
    uint8_t status = inb(base + 2);
    uint8_t master_command = inb(base);
    if ((status & ATA_BM_ACTIVE) || (master_command & 1)) {
        /* An unfinished firmware transfer cannot become a PIO channel.
         * Own/stop its master first, retain every claim and refuse discovery. */
        outb(base, master_command & ATA_BM_READ);
        c->dma.claim = claim; c->dma.generation = generation;
        c->dma.pci_config = 0x80000004u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 | (uint32_t)p->fn << 8;
        if ((inb(base + 2) & ATA_BM_ACTIVE) || (inb(base) & 1)) ata_dma_revoke(c);
        c->stopped = true;
        for (unsigned i = 0; i < 3; i++) if (c->claims[i] >= 0) registry_quarantine(c->claims[i], c->generations[i]);
        registry_quarantine(claim, generation); return;
    }
    if (index && (status & 0x80)) {
        registry_quarantine(claim, generation); return;
    }
    uint32_t table = pmm_alloc_contig(1), bounce = table ? pmm_alloc_contig(ATA_DMA_BYTES / PAGE_SIZE) : 0;
    if (!bounce) {
        if (table) pmm_free(table);
        registry_quarantine(claim, generation); return;
    }
    struct mm_pin *pins = ata_pins[index];
    pins[0] = (struct mm_pin){.generation=generation,.first_pfn=table/PAGE_SIZE,.pages=1,
        .address_mask=UINT32_MAX,.alignment=4,.boundary=65536,.direction=MM_PIN_TO_DEVICE};
    pins[1] = (struct mm_pin){.generation=generation,.first_pfn=bounce/PAGE_SIZE,.pages=ATA_DMA_BYTES/PAGE_SIZE,
        .address_mask=UINT32_MAX,.alignment=2,.direction=MM_PIN_BIDIRECTIONAL};
    int pin_error = mm_pin ? mm_pin(&pins[0]) : 0;
    if (!pin_error && mm_pin) pin_error = mm_pin(&pins[1]);
    if (pin_error) {
        if (pins[0].ticket && mm_unpin) (void)mm_unpin(&pins[0],generation);
        pmm_free(table);
        for (unsigned i=0;i<ATA_DMA_BYTES/PAGE_SIZE;i++) pmm_free(bounce+i*PAGE_SIZE);
        registry_quarantine(claim,generation); return;
    }
    uint32_t f = irq_save();
    uint32_t address = 0x80000004u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 | (uint32_t)p->fn << 8;
    outl(0xcf8, address);
    uint16_t command = c->ops->in16(c->port_ctx, 0xcfc);
    bool enabled = command != 0xffff && (command & 1);
    if (enabled) {
        c->ops->out16(c->port_ctx, 0xcfc, command | 4);
        enabled = c->ops->in16(c->port_ctx, 0xcfc) == (uint16_t)(command | 4);
    }
    irq_restore(f);
    if (!enabled || ata_dma_setup(c, base, &bm_ops, 0, P2V(table), table, P2V(bounce), bounce)) {
        if (mm_unpin) for (unsigned i=0;i<2;i++) if (pins[i].ticket) (void)mm_unpin(&pins[i],generation);
        pmm_free(table);
        for (unsigned i = 0; i < ATA_DMA_BYTES / PAGE_SIZE; i++) pmm_free(bounce + i * PAGE_SIZE);
        registry_quarantine(claim, generation); return;
    }
    mm_claim(table,1,MM_DMA); mm_claim(bounce,ATA_DMA_BYTES/PAGE_SIZE,MM_DMA);
    c->dma.claim = claim; c->dma.generation = generation;
    c->dma.bounce_pin=&pins[1];
    c->dma.pci_config = address;
}

int ata_dma_prepare(struct ata_channel *c, void *buf, uint32_t bytes, bool write)
{
    struct ata_dma *m = &c->dma;
    if (!m->ops || m->armed || !bytes || bytes > ATA_DMA_BYTES) return -FS_EINVAL;
    /* DMA ports are already owned; never prepare over active hardware. */
    if ((m->ops->in8(m->ctx, m->base) & 1) ||
        (m->ops->in8(m->ctx, m->base + 2) & ATA_BM_ACTIVE)) return -FS_EIO;
    uint32_t phys = 0, offset=0;
    unsigned entries=0;
    m->pin_count=0;
    m->bounced=false;
    if (m->ops->physical(m->ctx,buf,bytes,!write,&phys) && !(phys&1) &&
        (uint64_t)phys+bytes<=(1ull<<32)) {
        struct mm_pin *pin=&m->pins[0];
        *pin=(struct mm_pin){.generation=m->generation,.first_pfn=phys/4096,
            .pages=((phys&4095u)+bytes+4095u)/4096,.address_mask=UINT32_MAX,.alignment=2,
            .direction=write ? MM_PIN_TO_DEVICE : MM_PIN_FROM_DEVICE};
        if (!mm_pin || !mm_pin(pin)) {
            int n=ata_dma_prd(m->prd,ATA_DMA_PRDS,phys,bytes);
            if (n>0) { entries=(unsigned)n; offset=bytes; m->pin_count=1; }
            else if (mm_unpin && pin->ticket) (void)mm_unpin(pin,pin->generation);
        }
    }
    /* Translate page fragments independently: virtual adjacency does not
     * imply physical adjacency. Every direct fragment is pinned before any
     * descriptor becomes visible to the controller. */
    while (offset<bytes) {
        uintptr_t address=(uintptr_t)buf+offset;
        uint32_t take=4096u-(address&4095u); if (take>bytes-offset) take=bytes-offset;
        if ((take&1) || entries==ATA_DMA_PRDS ||
            !m->ops->physical(m->ctx,(uint8_t *)buf+offset,take,!write,&phys) || (phys&1) ||
            (phys&4095u)+take>4096u || (phys&65535u)+take>65536u) {
            m->bounced=true; break;
        }
        struct mm_pin *pin=&m->pins[m->pin_count];
        *pin=(struct mm_pin){.generation=m->generation,.first_pfn=phys/4096,.pages=1,
            .address_mask=UINT32_MAX,.alignment=2,
            .direction=write ? MM_PIN_TO_DEVICE : MM_PIN_FROM_DEVICE};
        if (mm_pin && mm_pin(pin)) { m->bounced=true; break; }
        m->pin_count++;
        m->prd[entries++]=(struct ata_prd){phys,(uint16_t)take,0};
        offset+=take;
    }
    if (m->bounced) {
        for (unsigned i=0;i<m->pin_count;i++) if (mm_unpin && m->pins[i].ticket)
            (void)mm_unpin(&m->pins[i],m->pins[i].generation);
        m->pin_count=0;
        phys = m->bounce_phys;
        if (write) memcpy(m->bounce, buf, bytes);
        int n = ata_dma_prd(m->prd, ATA_DMA_PRDS, phys, bytes);
        if (n < 0) return n;
        entries=(unsigned)n;
    } else {
        m->prd[entries-1].flags=0x8000;
    }
    struct blk_request *request=c->requests.active;
    if (request) {
        request->page_count=0;
        for (unsigned i=0;i<entries && i<BLKREQ_PAGES;i++) {
            request->pages[i]=(struct blk_page){.physical=m->prd[i].address,
                .bytes=m->prd[i].count ? m->prd[i].count : 65536u,
                .pin=m->bounced ? m->bounce_pin : &m->pins[m->pin_count==1 ? 0 : i]};
            request->page_count++;
        }
        request->pin_owner=m->bounced ? m->bounce_pin : &m->pins[0];
    }
    m->bytes = bytes; m->direction = write ? 0 : ATA_BM_READ;
    m->started = false;
    m->irq_status = 0;
    m->ops->out8(m->ctx, m->base, m->direction);
    __asm__ volatile("" ::: "memory"); /* publish descriptors before port I/O */
    m->ops->out32(m->ctx, m->base + 4, m->prd_phys);
    uint8_t s = m->ops->in8(m->ctx, m->base + 2);
    m->ops->out8(m->ctx, m->base + 2, (s & 0x60) | ATA_BM_IRQ | ATA_BM_ERROR);
    m->armed = true;
    return 0;
}
void ata_dma_start(struct ata_channel *c)
{
    struct ata_dma *m = &c->dma;
    m->started = true;
    m->ops->out8(m->ctx, m->base, m->direction | 1);
}
void ata_dma_irq(struct ata_channel *c)
{
    struct ata_dma *m = &c->dma;
    /* f1-92: the first completion captured after Start is final; a later
     * edge before disarm must not replace it with a stray snapshot. */
    if (m->started && (m->irq_status & (ATA_BM_IRQ | ATA_BM_ERROR))) return;
    /* Read BEFORE clearing Start: exact-size PRDs must already be retired.
     * Read again after Stop to flush posted writes/prove inactivity. */
    uint8_t s = m->ops->in8(m->ctx, m->base + 2);
    if (m->started && !(s & ATA_BM_ERROR)) {
        /* f1-92. Intel "Programming Interface for Bus Master IDE Controller"
         * 1.0, 3.1 Data Synchronization: Interrupt clear means the device did
         * not generate this edge (libata ata_bmdma_port_intr: "not our irq").
         * Interrupt with Active set while the drive still shows BSY or DRQ is
         * not a completion either: ATA-6 9.7 sets interrupt pending only
         * after the data or on an abort (BSY=0, DRQ=0). Stopping the master
         * here would abort a live transfer, so leave it running and let the
         * waiter classify the edge; the real completion edge follows. */
        bool stray = !(s & ATA_BM_IRQ);
        if (!stray && (s & ATA_BM_ACTIVE)) {
            uint8_t drive = c->ops->in8(c->port_ctx, c->control); /* Alternate Status: no acknowledge */
            stray = drive != 0xff && (drive & (0x80 | 0x08));   /* BSY or DRQ */
        }
        if (stray) { m->irq_status = (uint8_t)(s & ~ATA_BM_IRQ); return; }
    }
    m->ops->out8(m->ctx, m->base, m->direction);
    uint8_t stopped = m->ops->in8(m->ctx, m->base + 2);
    m->irq_status = s | (stopped & (ATA_BM_ACTIVE | ATA_BM_ERROR));
    m->ops->out8(m->ctx, m->base + 2, (stopped & 0x60) | ATA_BM_IRQ | ATA_BM_ERROR);
}
bool ata_dma_stop(struct ata_channel *c)
{
    struct ata_dma *m = &c->dma;
    if (!m->ops) return true;
    m->ops->out8(m->ctx, m->base, m->direction);
    uint8_t s = m->ops->in8(m->ctx, m->base + 2);
    m->ops->out8(m->ctx, m->base + 2, (s & 0x60) | ATA_BM_IRQ | ATA_BM_ERROR);
    __asm__ volatile("" ::: "memory");
    return !(s & ATA_BM_ACTIVE) && !(m->ops->in8(m->ctx, m->base) & 1);
}
void ata_dma_revoke(struct ata_channel *c)
{
    /* A broken master must not continue accessing a caller's direct buffer
     * after EIO returns. Stop failed: revoke PCI bus mastering for the whole
     * function, preserving other command bits. Never resume DMA afterward.
     * If even configuration readback fails, halt before returning buffers. */
    if (!c->dma.pci_config) return;
    uint32_t f = irq_save();
    outl(0xcf8, c->dma.pci_config);
    uint16_t command = c->ops->in16(c->port_ctx, 0xcfc);
    c->ops->out16(c->port_ctx, 0xcfc, command & ~4u);
    uint16_t got = c->ops->in16(c->port_ctx, 0xcfc);
    irq_restore(f);
    if (command == 0xffff || got == 0xffff || (got & 4)) panic("ATA DMA cannot revoke bus master");
}
void ata_dma_finish(struct ata_channel *c, void *buf, bool write, bool success)
{
    __asm__ volatile("" ::: "memory");
    if (success && !write && c->dma.bounced) memcpy(buf, c->dma.bounce, c->dma.bytes);
    for (unsigned i=0;i<c->dma.pin_count;i++) if (mm_unpin && c->dma.pins[i].ticket)
        (void)mm_unpin(&c->dma.pins[i],c->dma.pins[i].generation);
    c->dma.pin_count=0;
}
