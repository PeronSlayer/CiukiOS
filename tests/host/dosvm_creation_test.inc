/* Full production creation, ownership and copy safety, not just a model. */
static unsigned creation_alloc_last, creation_alloc_max;
static void creation_allocation_point(void)
{
    unsigned span = allocation_calls - creation_alloc_last;
    if (span > creation_alloc_max) creation_alloc_max = span;
    creation_alloc_last = allocation_calls;
    CHECK(cpu_flags & V86_IF);
    CHECK(!g_current->preempt_holds);
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) CHECK(!slots[i].disk_busy);
}
static void test_maximum_creation(void)
{
    empty();
    struct dosvm *v;
    struct dosvm_config c = {.commitment = DOSVM_RESIDENT_CAP};
    uint64_t total[2], longest[2];
    for (unsigned points = 0; points < 2; points++) {
        creation_cost_ns = creation_cost_span = creation_cost_max = 0;
        creation_cost_enabled = true; creation_cost_points = points != 0;
        CHECK(!dosvm_create(&c, &v));
        creation_cost_boundary(); total[points] = creation_cost_ns; longest[points] = creation_cost_max;
        creation_cost_enabled = false;
        CHECK(v->memory.quota == VM_POOL_CAP);
        destroy(v, true); empty();
    }
    CHECK(longest[0] >= 110000000 && longest[1] <= 2000000);
    CHECK(total[1] >= total[0] && total[1] <= total[0] * 110 / 100);
    printf("F0-41 full creation work model: total=%llu->%lluns longest=%llu->%lluns PASS\n",
           (unsigned long long)total[0], (unsigned long long)total[1],
           (unsigned long long)longest[0], (unsigned long long)longest[1]);
    creation_alloc_last = allocation_calls; creation_alloc_max = 0;
    preempt_hook = creation_allocation_point;
    CHECK(!dosvm_create(&c, &v));
    creation_allocation_point(); preempt_hook = 0;
    CHECK(v->memory.quota == VM_POOL_CAP && creation_alloc_max <= 17);
    struct dosvm_ledger ledger; dosvm_snapshot(&ledger);
    CHECK(ledger.reserved == DOSVM_RESIDENT_CAP && ledger.resident == v->info.resident);
    CHECK(v->info.resident <= DOSVM_RESIDENT_CAP && v->memory.used_pages == 0);
    unsigned range_before = preempt_points;
    CHECK(dosvm_range(v, VM_POOL_BASE, VM_POOL_CAP, true));
    CHECK(preempt_points == range_before + VM_POOL_PAGES / 16);
    for (unsigned off = 0; off < VM_POOL_CAP; off += PAGE_SIZE) {
        uint32_t phys = as_lookup(&v->as, VM_POOL_BASE + off, 0);
        CHECK(phys && allocated[phys / PAGE_SIZE]);
        for (unsigned byte = 0; byte < PAGE_SIZE; byte++) CHECK(!((uint8_t *)P2V(phys))[byte]);
    }
    /* External copies can service the scheduler at page boundaries. */
    uint8_t data[3 * PAGE_SIZE], copy[sizeof(data)]; memset(data, 0x59, sizeof(data));
    unsigned before = preempt_points;
    CHECK(!dosvm_write(v, VM_POOL_BASE + 1, data, sizeof(data)));
    CHECK(!dosvm_read(v, VM_POOL_BASE + 1, copy, sizeof(copy)));
    CHECK(preempt_points >= before + 6 && !memcmp(data, copy, sizeof(data)));
    before = preempt_points; uint8_t latches[4]; memcpy(latches, v->vga.latch, sizeof(latches));
    unsigned cursor; CHECK(dosvm_text(v, &cursor) == v->text);
    CHECK(preempt_points >= before + 15 && !memcmp(latches, v->vga.latch, sizeof(latches)));
    for (unsigned points = 0; points < 2; points++) {
        creation_cost_ns = creation_cost_span = creation_cost_max = 0;
        creation_cost_enabled = true; creation_cost_points = points != 0;
        dosvm_text(v, 0); creation_cost_boundary();
        creation_cost_enabled = false;
        longest[points] = creation_cost_max;
    }
    CHECK(longest[0] >= 8000000 && longest[1] <= 2000000);
    printf("F0-41 text copy work model: longest=%llu->%lluns, latches preserved PASS\n",
           (unsigned long long)longest[0], (unsigned long long)longest[1]);
    /* The physical VGA aperture has the same 256-byte checkpoint bound. */
    v->fullscreen = true; before = preempt_points;
    CHECK(!dosvm_read(v, 0xA0000, copy, PAGE_SIZE));
    CHECK(preempt_points == before + PAGE_SIZE / 256); v->fullscreen = false;
    /* No new point is allowed in monitor transactions or disk gates;
     * kpreempt_point itself enforces IF and spinlock/preempt holds. */
    struct task *saved = g_current; g_current = v->task;
    before = preempt_points;
    CHECK(!dosvm_read(v, VM_POOL_BASE, copy, sizeof(copy)));
    dosvm_text(v, 0); CHECK(preempt_points == before); g_current = saved;
    for (unsigned guard = 0; guard < 3; guard++) {
        if (guard == 0) v->disk_busy = true;
        if (guard == 1) cpu_flags = 0;
        if (guard == 2) kpreempt_hold();
        before = preempt_points;
        CHECK(!dosvm_read(v, VM_POOL_BASE, copy, sizeof(copy)));
        dosvm_text(v, 0); CHECK(preempt_points == before);
        v->disk_busy = false; cpu_flags = V86_IF;
        if (guard == 2) kpreempt_release();
    }
    before = preempt_points; CHECK(!dosvm_boot(v) && preempt_points > before);
    destroy(v, true); empty();
    /* Allocation failure after several pool checkpoints must unwind every
     * resident page, without leaking admission or reusing live backing. */
    fail_at = allocation_calls + 512;
    v = (void *)1; CHECK(dosvm_create(&c, &v) == -ENOMEM && !v);
    fail_at = 0; empty();
    printf("F0-41 maximum VM: 6144 zeroed/reserved pool pages, max allocation span=%u pages, copy/text/boot points and transaction guards, pool rollback PASS\n", creation_alloc_max);
}

/* Full maximum lifecycle: both quiescent normal stop and a disk-open failure
 * after the entire 24 MiB pool exists. Disable only modeled service for the
 * control, keeping the production loops, mappings and cleanup unchanged. */
static struct dosvm_key teardown_key;
static void repeat_teardown_point(void)
{
    preempt_hook=0;
    CHECK(dosvm_destroy(teardown_key,true)==-EBUSY);
}
static void test_maximum_teardown(void)
{
    for (unsigned failure=0; failure<2; failure++) {
        uint64_t total[2], maximum[2];
        for (unsigned points=0; points<2; points++) {
            empty(); struct dosvm *v=0;
            if (!failure) CHECK(!dosvm_create(0,&v));
            creation_cost_ns=creation_cost_span=creation_cost_max=0;
            creation_cost_enabled=true; creation_cost_points=points!=0;
            if (failure) {
                io_fail_at=io_calls+1;
                CHECK(dosvm_create(0,&v)<0 && !v); io_fail_at=0;
            } else {
                if (points) { teardown_key=dosvm_key(v); preempt_hook=repeat_teardown_point; }
                destroy(v,false); CHECK(!preempt_hook);
            }
            creation_cost_boundary(); creation_cost_enabled=false;
            total[points]=creation_cost_ns; maximum[points]=creation_cost_max;
            empty();
        }
        CHECK(maximum[0]>=40000000 && maximum[1]<=2000000);
        CHECK(total[1]>=total[0] && total[1]<=total[0]*110/100);
        printf("F0-44 %s cleanup: total=%llu->%lluns max=%llu->%lluns no ownership leaks PASS\n",
               failure ? "late-failed-creation" : "normal-stop",
               (unsigned long long)total[0],(unsigned long long)total[1],
               (unsigned long long)maximum[0],(unsigned long long)maximum[1]);
    }
}

static void test_vga_transaction_bound(void)
{
    struct dosvm *v=create();
    video_exchange(v,VIDEO_LINEAR);
    uint32_t *pd=P2V(v->as.pd_phys), *pt=P2V(pd[0]&~0xfffu);
    for (unsigned i=0;i<VIDEO_WINDOW_PAGES;i++) {
        memset(P2V(v->frames[VIDEO_WINDOW_PAGE+i]),0x44,PAGE_SIZE);
        pt[VIDEO_WINDOW_PAGE+i]|=0x40;
    }
    for (unsigned plane=0;plane<4;plane++) v->video_pending[plane]=0xffff;
    uint8_t latches[4]; memcpy(latches,v->vga.latch,4);
    unsigned before=preempt_points;
    creation_cost_ns=creation_cost_span=creation_cost_max=0;
    creation_cost_enabled=creation_cost_points=true;
    video_harvest(v);
    /* Charge the entire fixed aperture for each scalar conversion (8 ns
     * per byte, rounded up from 4 * 8 us / 4096). Mask and PTE accounting
     * is charged conservatively even when a particular entry is unchanged. */
    creation_cost_charge(CVGA_PLANE_SIZE*8u+64u*1000u);
    video_linear_seed(v); creation_cost_charge(CVGA_PLANE_SIZE*8u);
    video_exchange(v,0); creation_cost_charge(128000);
    creation_cost_boundary(); creation_cost_enabled=false;
    CHECK(preempt_points==before && creation_cost_max<=2000000);
    CHECK(!memcmp(latches,v->vga.latch,4));
    printf("F0-44 VGA combined transaction: aperture=65536 bytes span=%lluns no internal point, latches preserved PASS\n",
           (unsigned long long)creation_cost_max);
    destroy(v,true); empty();
}
