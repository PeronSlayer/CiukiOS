/* f3-46 (contract D1) combined host gate: production DOS VM services, the
 * pool object (vm/dosvm_pool.c), f0-48 reclaim and f0-49 pager together.
 * Guest CPU stores are played as the MMU would: a #PF through dosvm_trap,
 * then the store sets the translation's A/D bits and changes the frame.
 * Included by dosvm_test.c after the DPMI helpers. SPDX-License-Identifier: GPL-2.0-only */
static uint32_t vmp_pattern(uint32_t page, uint32_t salt) { return 0x46000000u ^ (page * 2654435761u) ^ salt; }
/* Guest CPU store of one word through linear (V86 or the client's flat
 * linear, as cur's mode says): fault in if absent, then A/D on that PTE. */
static void vmp_cpu_store(struct dosvm *v, uint32_t linear, uint32_t value)
{
    uint32_t *p = pte_of(v, linear);
    if (!p || !(*p & PTE_P)) {
        fake_cr2 = linear; step(v, 14, 6);
        CHECK(v->info.state < DOSVM_STOPPING);
        p = pte_of(v, linear);
    }
    CHECK(p && (*p & PTE_P));
    *p |= PTE_A | PTE_D;
    memcpy((uint8_t *)P2V(*p & ~0xFFFu) + (linear & 0xFFFu), &value, 4);
}
static uint32_t vmp_cpu_load(struct dosvm *v, uint32_t linear)
{
    uint32_t *p = pte_of(v, linear), value;
    if (!p || !(*p & PTE_P)) {
        fake_cr2 = linear; step(v, 14, 4);
        CHECK(v->info.state < DOSVM_STOPPING);
        p = pte_of(v, linear);
    }
    CHECK(p && (*p & PTE_P));
    *p |= PTE_A;
    memcpy(&value, (uint8_t *)P2V(*p & ~0xFFFu) + (linear & 0xFFFu), 4);
    return value;
}
static uint32_t vmp_word(struct dosvm *v, uint32_t linear)
{ uint32_t x = 0; CHECK(!dosvm_read(v, linear, &x, 4)); return x; }
static bool vmp_mapped(struct dosvm *v, uint32_t linear)
{ uint32_t *p = pte_of(v, linear); return p && (*p & PTE_P); }
static unsigned vmp_pinned_pages(void)
{ unsigned n = 0; for (unsigned i = 0; i < HOST_PAGES; i++) n += host_descriptors[i].pins; return n; }

/* XMS, EMS windows and DPMI client memory paged out and back through every
 * alias; dirty history through an alias forces a write; clean copies are
 * discarded without I/O; device/DPMI pins and transactions keep pages. */
static void test_pool_paging_aliases(void)
{
    vmp_attach();
    unsigned base_pages = used_pages;
    struct dosvm *v = dpmi_vm(true); v86_start();
    CHECK(!v->memory.pool.locked && !v->memory.pool.resident && v->memory.pool.pages == 1024);
    CHECK(host_commit[MM_VM_POOL] == 1024 && host_reserve[MM_VM_POOL] == 1024);
    /* XMS: 64 KiB EMB written by the kernel (transfer) path. */
    struct v86_frame f = xf(9, 64); CHECK(XMS(v, f) && AX(f) == 1);
    unsigned xh = DX(f); uint32_t xbase = v->memory.xms[xh - 1].base;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t x = vmp_pattern(i, 1); CHECK(!dosvm_write(v, xbase + i * PAGE_SIZE + 8, &x, 4));
    }
    /* EMS: four logical pages; window 0 and 1 map logical pages 0 and 1.
     * Logical page 0 is written only by guest stores through the window. */
    f = ef(0x4300, 0, 4); CHECK(EMS(v, f) && !EMSERR(f));
    unsigned eh = DX(f); uint32_t ebase = v->memory.ems[eh - 1].block.base;
    f = ef(0x4400, eh, 0); CHECK(EMS(v, f) && !EMSERR(f));
    f = ef(0x4401, eh, 1); CHECK(EMS(v, f) && !EMSERR(f));
    CHECK(!vmp_mapped(v, 0xE0000) && !vmp_mapped(v, ebase)); /* allocation is demand-zero */
    for (uint32_t i = 0; i < 4; i++) vmp_cpu_store(v, 0xE0000 + i * PAGE_SIZE + 16, vmp_pattern(i, 2));
    CHECK(vmp_mapped(v, ebase) && (*pte_of(v, ebase) & ~0xFFFu) == (*pte_of(v, 0xE0000) & ~0xFFFu));
    CHECK(!(*pte_of(v, ebase) & PTE_D) && (*pte_of(v, 0xE0000) & PTE_D)); /* dirt only on the alias */
    for (uint32_t i = 0; i < 4; i++) CHECK(vmp_word(v, ebase + i * PAGE_SIZE + 16) == vmp_pattern(i, 2));
    /* DPMI: a 32-bit client's 0501h block, written by guest stores. */
    enter(v, true); CHECK(in_pm());
    cur.tf.ebx = 1; cur.tf.ecx = 0; int31(v, 0x0501); CHECK(!cf()); /* 64 KiB */
    uint32_t dbase = ((cur.tf.ebx & 0xFFFF) << 16) | (cur.tf.ecx & 0xFFFF);
    for (uint32_t i = 0; i < 16; i++) vmp_cpu_store(v, dbase + i * PAGE_SIZE + 24, vmp_pattern(i, 3));
    uint32_t pinned = v->memory.pool.pinned;
    CHECK(pinned == v->dpmi.hstack.pages && v->memory.pool.resident >= 16 + 4 + 16 + pinned);
    /* Global replacement pages out every unpinned pool page; the locked
     * host stack (interrupt frames) stays resident. */
    unsigned writes = vmp_writes;
    vmp_page_out(v, pinned);
    CHECK(vmp_writes > writes && v->memory.pool.swapped >= 36);
    CHECK(!vmp_mapped(v, ebase) && !vmp_mapped(v, 0xE0000) && !vmp_mapped(v, dbase) && !vmp_mapped(v, xbase));
    CHECK(vmp_mapped(v, v->dpmi.hstack.base));
    /* Page-in through each alias kind preserves every byte. */
    for (uint32_t i = 0; i < 16; i++) CHECK(vmp_cpu_load(v, dbase + i * PAGE_SIZE + 24) == vmp_pattern(i, 3));
    CHECK(in_pm());
    /* DPMI lock (0600h) pins; unlock returns the page to replacement. */
    cur.tf.ebx = dbase >> 16; cur.tf.ecx = dbase & 0xFFFF; cur.tf.esi = 0; cur.tf.edi = 2 * PAGE_SIZE;
    int31(v, 0x0600); CHECK(!cf() && v->memory.pool.pinned == pinned + 2);
    vmp_page_out(v, pinned + 2);
    CHECK(vmp_mapped(v, dbase) && vmp_mapped(v, dbase + PAGE_SIZE));
    cur.tf.ebx = dbase >> 16; cur.tf.ecx = dbase & 0xFFFF; cur.tf.esi = 0; cur.tf.edi = 2 * PAGE_SIZE;
    int31(v, 0x0601); CHECK(!cf() && v->memory.pool.pinned == pinned);
    vmp_page_out(v, pinned);
    CHECK(!vmp_mapped(v, dbase));
    /* 0703h discards whole unlocked pages: no frame, no slot, reads zero. */
    unsigned swapped = v->memory.pool.swapped;
    cur.tf.ebx = dbase >> 16; cur.tf.ecx = dbase & 0xFFFF; cur.tf.esi = 0; cur.tf.edi = 4 * PAGE_SIZE;
    int31(v, 0x0703); CHECK(!cf() && v->memory.pool.swapped == swapped - 4);
    CHECK(!vmp_word(v, dbase + 24) && vmp_word(v, dbase + 4 * PAGE_SIZE + 24) == vmp_pattern(4, 3));
    /* Back to V86 for the EMS/XMS checks: a fresh V86 frame on this VM. */
    v86_start();
    for (uint32_t i = 0; i < 2; i++) CHECK(vmp_cpu_load(v, 0xE0000 + i * PAGE_SIZE + 16) == vmp_pattern(i, 2));
    CHECK(vmp_mapped(v, ebase) && vmp_mapped(v, 0xE0000)); /* both aliases published */
    for (uint32_t i = 2; i < 4; i++) CHECK(vmp_word(v, ebase + i * PAGE_SIZE + 16) == vmp_pattern(i, 2));
    uint8_t move[16] = {0}; vm_w32(move, 16 * PAGE_SIZE); vm_w16(move + 4, xh); vm_w32(move + 6, 0);
    vm_w16(move + 10, 0); vm_w32(move + 12, 0x40000000u);   /* XMS move EMB -> 4000:0000 */
    CHECK(!dosvm_write(v, 0x20000, move, sizeof(move)));
    f = xf(0xB, 0); f.ds = 0x2000; CHECK(XMS(v, f) && AX(f) == 1);
    for (uint32_t i = 0; i < 16; i++) CHECK(vmp_word(v, 0x40000 + i * PAGE_SIZE + 8) == vmp_pattern(i, 1));
    /* A kept clean copy is discarded without I/O; a store through the EMS
     * window alias after page-in must be written again, never dropped. */
    CHECK(v->memory.pool.stats.page_ins >= 36);
    uint32_t window_page = (*pte_of(v, 0xE0000) & ~0xFFFu);
    CHECK(window_page && pmm_descriptor(window_page)->dirty_epoch == 0);
    vmp_cpu_store(v, 0xE0000 + 16, vmp_pattern(0, 4));      /* dirt on the alias only */
    writes = vmp_writes; unsigned clean = v->memory.pool.stats.clean_discards;
    vmp_page_out(v, pinned);
    /* Writes: the page dirtied only through the window alias, and the
     * discarded DPMI page read back as a fresh zero page (no copy yet). */
    CHECK(v->memory.pool.stats.clean_discards > clean && vmp_writes == writes + 2);
    CHECK(vmp_word(v, ebase + 16) == vmp_pattern(0, 4) && vmp_word(v, ebase + PAGE_SIZE + 16) == vmp_pattern(1, 2));
    /* Remapping a window folds the replaced alias's dirt into the page. */
    vmp_cpu_store(v, 0xE0000 + 32, vmp_pattern(9, 9));
    f = ef(0x4400, eh, 2); CHECK(EMS(v, f) && !EMSERR(f)); /* window 0 now logical page 2 */
    vmp_page_out(v, pinned);
    CHECK(vmp_word(v, ebase + 32) == vmp_pattern(9, 9));
    /* A transaction hold keeps this VM's pages from the pager. */
    uint32_t resident = v->memory.pool.resident;
    dosvm_pool_hold(v, true);
    host_pressure = true; vmp_cycle(4096); host_pressure = false;
    CHECK(v->memory.pool.resident == resident);
    dosvm_pool_hold(v, false);
    /* Second task of the same VM (the controller) works during a worker
     * transaction: no metadata pin, no EBUSY (f0-44/dbb0d714 lesson). */
    dosvm_pool_hold(v, true);
    struct task *saved = g_current; g_current = &caller;
    uint32_t x = 0x77; CHECK(!dosvm_write(v, xbase + 3 * PAGE_SIZE + 100, &x, 4));
    CHECK(vmp_word(v, xbase + 3 * PAGE_SIZE + 100) == 0x77);
    g_current = saved; dosvm_pool_hold(v, false);
    printf("f3-46 paging aliases: xms/ems/dpmi pages out=%u in=%u clean=%u redirtied=%u writes=%u reads=%u pinned=%u PASS\n",
           v->memory.pool.stats.pageouts, v->memory.pool.stats.page_ins, v->memory.pool.stats.clean_discards,
           v->memory.pool.stats.redirtied, vmp_writes, vmp_reads, pinned);
    destroy(v, true);
    CHECK(!host_commit[MM_VM_POOL] && !host_commit[MM_VM_PRIVATE] && !host_reserve[MM_VM_POOL] && !host_reserve[MM_VM_PRIVATE]);
    CHECK(used_pages == base_pages && !vmp_pinned_pages() && !pager.counts[ST_USED] && !host_swap_used);
    vmp_detach(); empty();
}

/* Virtual DMA: unmasking pins the channel's window at the settle point;
 * the clock work reads only resident frames; masking (or terminal count)
 * releases the pins at the next settle. */
static void test_pool_paging_dma(void)
{
    vmp_attach();
    struct dosvm *v; struct dosvm_config c = {.commitment = DOSVM_LOW_CAP + 4u * 1024 * 1024};
    CHECK(!dosvm_create(&c, &v));
    /* Channel 1 (8-bit), page 11h: window 110000h-11FFFFh, the pool's first 16 pages. */
    for (uint32_t i = 0; i < 16; i++) { uint8_t b = (uint8_t)(0x40 + i); CHECK(!dosvm_write(v, VM_POOL_BASE + i * PAGE_SIZE, &b, 1)); }
    vmp_page_out(v, 0);
    dosvm_dma_io(v, 0x83, true, 0x11);                       /* page register, channel 1 */
    dosvm_dma_io(v, 0x0C, true, 0);                          /* clear flip-flop */
    dosvm_dma_io(v, 0x02, true, 0); dosvm_dma_io(v, 0x02, true, 0);  /* address 0000h */
    dosvm_dma_io(v, 0x03, true, 0xFF); dosvm_dma_io(v, 0x03, true, 0xFF); /* count FFFFh */
    dosvm_dma_io(v, 0x0B, true, 0x59);                       /* single, increment, autoinit, read, ch1 */
    dosvm_dma_io(v, 0x0A, true, 0x01);                       /* unmask channel 1 */
    /* Before the settle point the clock work sees no resident frame: an
     * unanswered DREQ, never a DMA fault. */
    uint8_t buffer[16]; bool tc;
    unsigned faults = v->audio.dma.faults, waits = v->memory.pool.stats.dma_waits;
    CHECK(!dosvm_dma_read(v, 1, buffer, 4, &tc) && v->audio.dma.faults == faults && v->memory.pool.stats.dma_waits == waits + 1);
    dosvm_pool_settle(v);
    CHECK(v->memory.pool.pinned == 16 && v->memory.pool.resident == 16);
    CHECK(dosvm_dma_read(v, 1, buffer, 1, &tc) == 1 && buffer[0] == 0x40);
    /* A second context (the controller) pins and unpins the same page: the
     * DMA pin stands, no refusal, no wait (counted pins, no metadata lock). */
    struct task *saved = g_current; g_current = &caller;
    CHECK(!dosvm_pool_pin(v, VM_POOL_BASE, 1) && page_at(v, 0)->pins == 2);
    dosvm_pool_unpin(v, VM_POOL_BASE, 1);
    g_current = saved;
    CHECK(page_at(v, 0)->pins == 1 && pmm_descriptor(page_at(v, 0)->phys)->pins == 1);
    host_pressure = true; vmp_cycle(4096); host_pressure = false;
    CHECK(v->memory.pool.resident == 16); /* pinned under pressure */
    dosvm_dma_io(v, 0x0A, true, 0x05);                       /* mask channel 1 */
    dosvm_pool_settle(v);
    CHECK(!v->memory.pool.pinned && !vmp_pinned_pages());
    vmp_page_out(v, 0);
    printf("f3-46 paging DMA: window pinned at settle=16 pages, pre-settle reads waited=%u, faults=0, released at mask PASS\n",
           v->memory.pool.stats.dma_waits);
    destroy(v, true); vmp_detach(); empty();
}

/* Contained backing errors and termination with I/O and an IRQ pending. */
static struct dosvm_key vmp_victim;
static unsigned vmp_destroy_ok, vmp_orphans;
static void vmp_destroy_during_io(struct pager_io *io)
{
    (void)io;
    struct task *saved = g_current; g_current = &caller;
    struct dosvm *v = dosvm_lookup(vmp_victim);
    vmp_orphans = 0;
    for (uint32_t i = 0; i < v->memory.pool.pages; i++) {
        struct vm_page *page = page_at(v, i);
        vmp_orphans += page->frame && page->frame->holds; /* queued: close must orphan, not free */
    }
    CHECK(dosvm_irq_raise(vmp_victim, 0));
    CHECK(dosvm_destroy(vmp_victim, true) == 0);
    vmp_destroy_ok++;
    g_current = saved;
}
static struct dosvm *vmp_racer;
static int vmp_race_result;
static void vmp_second_fault(struct pager_io *io)
{
    (void)io;
    struct task *saved = g_current; g_current = &caller;
    uint32_t x;
    vmp_race_result = dosvm_read_resident(vmp_racer, VM_POOL_BASE + PAGE_SIZE + 8, &x, 4); /* busy: never a second I/O */
    g_current = saved;
}
static void test_pool_paging_failures(void)
{
    vmp_attach();
    unsigned base_pages = used_pages;
    struct dosvm *v; struct dosvm_config c = {.commitment = DOSVM_LOW_CAP + 4u * 1024 * 1024};
    CHECK(!dosvm_create(&c, &v));
    for (uint32_t i = 0; i < 32; i++) { uint32_t x = vmp_pattern(i, 5); CHECK(!dosvm_write(v, VM_POOL_BASE + i * PAGE_SIZE + 8, &x, 4)); }
    /* A failed page-out keeps the page and its dirty version. */
    vmp_fail_write = 4;
    host_pressure = true;
    for (unsigned round = 0; round < 64 && vmp_fail_write; round++) vmp_cycle(1024);
    host_pressure = false;
    CHECK(v->memory.pool.resident == 32 && !v->memory.pool.swapped && !vmp_fail_write);
    for (uint32_t i = 0; i < 32; i++) CHECK(vmp_word(v, VM_POOL_BASE + i * PAGE_SIZE + 8) == vmp_pattern(i, 5));
    pager.sticky = 0; pager.write_errors = 0; publish(); /* operator-cleared sticky state for the next case */
    vmp_page_out(v, 0);
    /* A failed page-in is contained: no zeros, the slot is kept, retry works. */
    vmp_fail_read = 1; uint32_t x = 0;
    CHECK(dosvm_read(v, VM_POOL_BASE + 8, &x, 4) == -EFAULT && !x && v->memory.pool.swapped == 32);
    CHECK(v->memory.pool.stats.page_in_errors == 1 && vmp_word(v, VM_POOL_BASE + 8) == vmp_pattern(0, 5));
    /* A second task faulting the same page during the page-in is coalesced. */
    vmp_racer = v; vmp_during = vmp_second_fault; unsigned reads = vmp_reads;
    CHECK(vmp_word(v, VM_POOL_BASE + PAGE_SIZE + 8) == vmp_pattern(1, 5));
    CHECK(vmp_race_result == -EAGAIN && vmp_reads == reads + 1); /* in-flight page: refused, one I/O */
    /* A guest fault whose page-in fails stops the VM (fault), never zero-fills. */
    v86_start(); vmp_fail_read = 1; fake_cr2 = VM_POOL_BASE + 2 * PAGE_SIZE;
    v->info.dos_ready = true; v->info.state = DOSVM_RUNNABLE; v->monitor.state = V86_RUNNING;
    step(v, 14, 4);
    CHECK(v->info.state == DOSVM_STOPPING && v->info.reason == DOSVM_FAULT && v->info.fault_vector == 14 &&
          v->info.fault_address == VM_POOL_BASE + 2 * PAGE_SIZE);
    destroy(v, true);
    CHECK(used_pages == base_pages && !pager.counts[ST_USED] && !host_commit[MM_VM_POOL]);
    /* Termination while a page-out write is in flight, with an IRQ pending:
     * queued frames are orphaned to the reclaim queue and freed exactly once. */
    CHECK(!dosvm_create(&c, &v));
    for (uint32_t i = 0; i < 64; i++) { uint32_t y = vmp_pattern(i, 6); CHECK(!dosvm_write(v, VM_POOL_BASE + i * PAGE_SIZE, &y, 4)); }
    vmp_victim = dosvm_key(v); vmp_destroy_ok = 0; vmp_during = vmp_destroy_during_io;
    host_pressure = true;
    for (unsigned round = 0; round < 64 && !vmp_destroy_ok; round++) vmp_cycle(1024);
    host_pressure = false;
    CHECK(vmp_destroy_ok == 1 && vmp_orphans && !dosvm_valid(vmp_victim) && v->info.state == DOSVM_DEAD);
    host_pressure = true; vmp_cycle(64); host_pressure = false; /* drain retained completions */
    CHECK(!reclaim_writeback_waiting() && !pager.inflight && !pager.retries);
    CHECK(used_pages == base_pages && !vmp_pinned_pages() && !pager.counts[ST_USED] && !host_swap_used);
    CHECK(!host_commit[MM_VM_POOL] && !host_commit[MM_VM_PRIVATE] && !host_reserve[MM_VM_POOL]);
    puts("f3-46 paging failures: failed write keeps dirty page, failed read contained (no zeros), coalesced second fault, faulting VM stopped, destroy during in-flight page-out with IRQ pending: counters at baseline PASS");
    vmp_detach(); empty();
}

/* Commitment: paged pools commit RAM or published pagefile; a refused
 * commitment grows the pagefile first (sleepable, no object level held),
 * then refuses before any partial state. The manager runs while waiting. */
static void vmp_manager_hook(void)
{
    struct task *saved = g_current; g_current = &vmp_manager_task;
    pager_manager_step(g_ticks);
    g_current = saved;
}
static void test_pool_commitment(void)
{
    vmp_attach();
    struct dosvm *v;
    unsigned base_pages = used_pages;
    /* RAM covers only the fixed part (8 MiB); the 24 MiB pool needs the
     * pagefile: one more 16 MiB extent is grown before admission. */
    struct dosvm_config c = {.commitment = VM_POOL_CAP + DOSVM_LOW_CAP};
    host_commit_limit = 2048; sleep_hook = vmp_manager_hook;
    CHECK(!dosvm_create(&c, &v) && host_commit[MM_VM_POOL] == VM_POOL_PAGES && pager.extents == 2);
    CHECK(host_commit[MM_VM_PRIVATE] == 2048 && pager.stats.commit_growths == 1);
    destroy(v, true);
    /* 48 MiB: even the largest pagefile cannot back it: refused, nothing
     * partially admitted or allocated. */
    c.commitment = DOSVM_RESIDENT_CAP;
    v = (void *)1; CHECK(dosvm_create(&c, &v) == -ENOMEM && !v);
    CHECK(pager.grow_blocked && !host_commit[MM_VM_POOL] && !host_commit[MM_VM_PRIVATE]);
    sleep_hook = 0; host_commit_limit = -1;
    CHECK(used_pages == base_pages + 1); /* the grown extent's metadata page */
    /* Locked (per-game compatibility) pool: eager, resident, pinned. */
    struct dosvm_config locked = {.commitment = DOSVM_LOW_CAP + 4u * 1024 * 1024, .memory_locked = true};
    CHECK(!dosvm_create(&locked, &v) && v->memory.pool.locked && v->memory.pool.resident == 1024);
    host_pressure = true; vmp_cycle(4096); host_pressure = false;
    CHECK(v->memory.pool.resident == 1024 && !v->memory.pool.swapped);
    destroy(v, true);
    puts("f3-46 commitment: pagefile grown before admission, refusal before partial state, locked pool never paged PASS");
    vmp_detach(); empty();
}

/* VM read cache: clean blocks in the global replacement, regrown on a miss
 * once out of pressure; the overlay index is never cache debt. */
static void test_pool_paging_cache(void)
{
    vmp_attach();
    struct dosvm *v; struct dosvm_config c = {.commitment = VM_POOL_CAP + DOSVM_LOW_CAP};
    CHECK(!dosvm_create(&c, &v) && v->read_capacity == 32);
    uint8_t sector[512], got[512]; memset(sector, 0x3C, sizeof(sector));
    CHECK(!dosvm_disk_write(v, 130, sector));
    for (unsigned i = 0; i < 32; i++) CHECK(!dosvm_disk_read(v, i * 128, got));
    unsigned rows = v->sector_count;
    host_pressure = true;
    for (unsigned round = 0; round < 16 && v->read_pages; round++) vmp_cycle(2048);
    host_pressure = false;
    CHECK(!v->read_pages && v->read_reclaimed == 32 && v->sector_count == rows && rows == 1);
    unsigned reads = disk_reads;
    CHECK(!dosvm_disk_read(v, 130, got) && !memcmp(got, sector, 512) && disk_reads > reads);
    CHECK(!dosvm_disk_read(v, 129, got) && got[0] == 129 % 251 && v->read_regrown >= 1 && v->read_pages == DOSVM_READ_PAGES);
    reads = disk_reads; CHECK(!dosvm_disk_read(v, 131, got) && disk_reads == reads); /* regrown block hit */
    /* Under pressure a miss never allocates: it evicts or reads uncached. */
    host_pressure = true; unsigned regrown = v->read_regrown;
    CHECK(!dosvm_disk_read(v, 5 * 128, got) && got[0] == (5 * 128) % 251 && v->read_regrown == regrown);
    host_pressure = false;
    printf("f3-46 VM cache: reclaimed=%u blocks without I/O, overlay rows kept=%u, regrown=%u, correct data PASS\n",
           v->read_reclaimed, rows, v->read_regrown);
    destroy(v, true); vmp_detach(); empty();
}

/* Idle prewrite (f0-49) of cold pool pages: frames stay mapped, a later
 * pressure episode discards the clean copies without I/O. */
static void test_pool_prewrite(void)
{
    vmp_attach();
    struct dosvm *v; struct dosvm_config c = {.commitment = DOSVM_LOW_CAP + 4u * 1024 * 1024};
    CHECK(!dosvm_create(&c, &v));
    for (uint32_t i = 0; i < 32; i++) { uint32_t x = vmp_pattern(i, 7); CHECK(!dosvm_write(v, VM_POOL_BASE + i * PAGE_SIZE, &x, 4)); }
    host_normal_free = 3000; /* near the high watermark, no pressure */
    uint64_t now = g_ticks + 1000;
    struct task *saved = g_current; g_current = &vmp_pager_task;
    for (unsigned n = 0; n < 4096 && v->memory.pool.stats.prewrites < 32; n++) {
        pager_prewrite_step(now); now += PAGER_PREWRITE_GAP_MS;
        while (pager_pageout_step()) {}
    }
    g_current = saved; host_normal_free = 0;
    CHECK(v->memory.pool.stats.prewrites == 32 && v->memory.pool.resident == 32);
    unsigned writes = vmp_writes;
    vmp_page_out(v, 0);
    CHECK(vmp_writes == writes && v->memory.pool.stats.clean_discards >= 32);
    for (uint32_t i = 0; i < 32; i++) CHECK(vmp_word(v, VM_POOL_BASE + i * PAGE_SIZE) == vmp_pattern(i, 7));
    puts("f3-46 idle prewrite: 32 cold pool pages written while mapped, later discarded without I/O PASS");
    destroy(v, true); vmp_detach(); empty();
}
/* Scheduling boundary (contract D1): a pending guest IRQ is attributed to
 * the worker's page-in wait, the guest's own IF clear, or host
 * descheduling, separately; the published vm_perf_page rows carry them. */
static void test_pool_irq_split(void)
{
    struct dosvm *v; struct dosvm_config c = {.commitment = DOSVM_LOW_CAP + 4u * 1024 * 1024};
    CHECK(!dosvm_create(&c, &v));
    CHECK(dosvm_irq_raise(dosvm_key(v), 0) && dosvm_pic_pending(&v->monitor) >= 0);
    struct task *saved = g_current;
    g_current = v->task; v->memory.pool.waiting = true; v->monitor.vif = true;
    dosvm_pool_irq_sample(v, 3);
    v->memory.pool.waiting = false; v->monitor.vif = false;
    dosvm_pool_irq_sample(v, 5);
    g_current = &caller; v->monitor.vif = true;
    dosvm_pool_irq_sample(v, 7);
    g_current = saved;
    const struct vm_pool_stats *ps = &v->memory.pool.stats;
    CHECK(ps->irq_page_ms == 3 && ps->irq_guest_if_ms == 5 && ps->irq_host_ms == 7);
    /* No pending IRQ, no attribution. */
    v->monitor.pic[0].irr = 0; dosvm_pool_irq_sample(v, 11);
    CHECK(ps->irq_page_ms == 3 && ps->irq_guest_if_ms == 5 && ps->irq_host_ms == 7);
    uint32_t x = 1; CHECK(!dosvm_write(v, VM_POOL_BASE, &x, 4));
    memset(page_records, 0, sizeof(page_records));
    uint32_t flags = g_boot.flags; uint64_t tsc = g_tsc_per_ms;
    g_boot.flags = 0; g_tsc_per_ms = 1000;
    v->perf.report_tick = g_ticks; g_ticks += DOSVM_PERF_INTERVAL; dosvm_perf_report(v);
    g_boot.flags = flags; g_tsc_per_ms = tsc;
    CHECK(strstr(page_records[0], "mode=paged quota=1024 resident=1 swapped=0 pinned=0 zero=1"));
    CHECK(strstr(page_records[1], "irq_ms=3/5/7") && strstr(page_records[2], "part=cache pages="));
    destroy(v, true); empty();
    puts("f3-46 scheduling boundary: IRQ delay split page-in/guest-IF/host, vm_perf_page rows PASS");
}
/* IRQ commits with physical IF clear never page in: the guest stack page
 * (here a V86 stack inside an EMS window) is resolved before cli(). */
static void test_pool_stack_prefault(void)
{
    vmp_attach();
    struct dosvm *v; struct dosvm_config c = {.commitment = DOSVM_LOW_CAP + 4u * 1024 * 1024};
    CHECK(!dosvm_create(&c, &v));
    struct v86_frame f = ef(0x4300, 0, 1); CHECK(EMS(v, f) && !EMSERR(f));
    unsigned eh = DX(f);
    f = ef(0x4400, eh, 0); CHECK(EMS(v, f) && !EMSERR(f));
    uint16_t word = 0x1234; CHECK(!dosvm_write(v, 0xE00F0, &word, 2));
    vmp_page_out(v, 0);
    struct v86_frame frame = {.tf = {.user_ss = 0xE000, .user_esp = 0x100, .eflags = V86_VM | V86_IF | 2}};
    v->frame_pm = false;
    cpu_flags = 0;
    CHECK(dosvm_write(v, 0xE00F8, &word, 2) == -EAGAIN); /* would be a VM fault at delivery */
    sti();
    stack_prefault(v, &frame);
    cpu_flags = 0;
    CHECK(!dosvm_write(v, 0xE00F8, &word, 2));
    sti();
    CHECK((vmp_word(v, 0xE00F0) & 0xFFFF) == 0x1234 && (vmp_word(v, 0xE00F8) & 0xFFFF) == 0x1234);
    puts("f3-46 IF-off delivery: guest stack page (EMS window alias) resolved before cli, no fault PASS");
    destroy(v, true); vmp_detach(); empty();
}
/* f3-45's DOOM flat private-stack ISR case with that stack in the paged
 * pool: every frame push and IRET pop crosses paged-out pages, the nested
 * IRQ0 is committed with physical IF clear, IRQRET survives, each ISR
 * closes once and no frame byte is lost. */
static void test_pool_flat_stack_isr(void)
{
    vmp_attach();
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    struct dpmi *d = &v->dpmi;
    /* The private stack lives in the client's own 0501h block (64 KiB). */
    cur.tf.ebx = 1; cur.tf.ecx = 0; int31(v, 0x0501); CHECK(!cf());
    const uint32_t stack = (((cur.tf.ebx & 0xFFFF) << 16) | (cur.tf.ecx & 0xFFFF)) + 0x8000;
    CHECK(stack >= VM_POOL_BASE + 0x8000);
    uint16_t code = (uint16_t)cur.tf.cs;
    d->vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
    d->vec[15] = (struct dpmi_vec){.sel=code, .off=0x720};
    d->vec[0x1C] = (struct dpmi_vec){.sel=code, .off=0x740};
    device_out(v, 0x21, 0x7E);
    struct v86_frame original = cur;
    CHECK(!dpmi_enter_pm_handler(v, &cur, 0x1C, 0));
    struct v86_frame software = cur;
    client_desc(v, 902, 0, 0xFFFFFFFFu, 0xF2, 0xC0);
    cur.tf.user_ss = DPMI_SEL(902); cur.tf.user_esp = stack;
    uint32_t pinned = v->memory.pool.pinned;
    vmp_page_out(v, pinned);
    v->monitor.vif = true;
    CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
    CHECK(d->depth == 2 && cur.tf.user_ss == DPMI_SEL(902) && cur.tf.user_esp == stack - 12);
    struct v86_frame handler = cur;
    vmp_page_out(v, pinned); /* the IRQ7 IRET frame leaves RAM */
    cur.tf.user_esp = stack + 0x4000; cur.tf.eip++;
    v->monitor.vif = true;
    CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
    CHECK(cur.tf.eip == 0x700 && v->info.state < DOSVM_STOPPING);
    const uint8_t eoi[] = {0xE6, 0x20}; cur.tf.eax = 0x20;
    exec(v, eoi, sizeof(eoi), 13, 0);
    vmp_page_out(v, pinned);
    pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    cur = handler; cur.tf.eax = 0x20;
    exec(v, eoi, sizeof(eoi), 13, 0);
    vmp_page_out(v, pinned);
    pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    CHECK(v->info.state < DOSVM_STOPPING && d->depth == 1);
    CHECK(v->perf.isr[7].count == 1 && v->perf.isr[0].count == 1 && !v->perf.isr_retired);
    CHECK(!v->monitor.pic[0].isr && !v->perf.isr_late_return && v->memory.pool.stats.page_ins >= 3);
    cur = software; pm_iret(v); at_stub(v, DPMI_PM_SOFTRET);
    CHECK(!d->depth && cur.tf.eip == original.tf.eip && cur.tf.user_esp == original.tf.user_esp);
    printf("f3-45/f3-46 flat private stack in the paged pool: nested IRQ0 over a paged-out IRQ7 frame, IRQRET survives, page-ins=%u PASS\n",
           v->memory.pool.stats.page_ins);
    destroy(v, true); vmp_detach(); empty();
}
static void test_pool_paging(void)
{
    test_pool_flat_stack_isr();
    test_pool_irq_split();
    test_pool_stack_prefault();
    test_pool_paging_aliases();
    test_pool_paging_dma();
    test_pool_paging_failures();
    test_pool_commitment();
    test_pool_paging_cache();
    test_pool_prewrite();
}
