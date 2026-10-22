/* F6-09: production e100 ISR/worker ordering and QEMU's flagless RFDs.
 * Included after the shared register/DMA fixtures in net_test.c.
 * SPDX-License-Identifier: GPL-2.0-only */
static bool model_e100_irq_asserted(const struct model *h)
{
    return !(h->reg[3] & 1) && (h->reg[1] & (~h->reg[3] | 0x0f));
}
static void e100_check_write(const struct model *h, unsigned at, unsigned reg,
                             unsigned value, unsigned width, bool in_irq)
{
    CHECK(at < h->scb_write_count);
    const struct model_scb_write *w = &h->scb_writes[at];
    CHECK(w->reg == reg && w->value == value && w->width == width);
    CHECK(!!(w->flags & 0x200) == !in_irq);
}
static void e100_test_interrupt(struct model *h)
{
    struct netif *d = h->device;
    unsigned causes = h->reg[1] & 0xfc, command = h->reg[2], pointer = h->pointer;
    unsigned pending = d->pending, slept = sleeps, packets = h->packet_io, pages = page_count;
    uint64_t interrupts = d->stats.interrupts;
    CHECK(causes && d->irq_attached);
    h->trace_scb = true; h->scb_write_count = 0;
    uint32_t flags = irq_save(); CHECK(netif_irq(d)); irq_restore(flags);
    CHECK(h->scb_write_count == 2);
    e100_check_write(h, 0, 3, 1, 1, true); /* mask before W1C on the shared chain */
    e100_check_write(h, 1, 1, causes, 1, true);
    CHECK(h->reg[2] == command && h->pointer == pointer);
    CHECK(!h->reg[1] && !model_e100_irq_asserted(h));
    CHECK(d->pending == (pending | causes) && d->irq_pending);
    CHECK(d->stats.interrupts == interrupts + 1);
    CHECK(sleeps == slept && h->packet_io == packets && page_count == pages);
}
static void e100_arp_reply_frame(uint8_t frame[60], const uint8_t local[6])
{
    const uint8_t peer[6] = {0x52,0x55,10,0,2,2};
    CHECK(net_arp_request(frame, local) == 60);
    memcpy(frame, local, 6); memcpy(frame + 6, peer, 6); frame[21] = 2;
    memcpy(frame + 22, peer, 6); memcpy(frame + 32, local, 6);
    frame[31] = 2; frame[41] = 15;
}
static void e100_physical_profile(void)
{
    g_boot.flags = 0;
    /* Qualify the same IRQ through firmware $PIR, without the QEMU flag. */
    memcpy(route_bios, "$PIR", 4); route_bios[5] = 1; route_bios[6] = 48; route_bios[9] = 8;
    route_bios[33] = 3 * 8; route_bios[34] = 0x60; route_bios[36] = 8;
    for (unsigned i = 0; i < 48; i++) if (i != 31) route_bios[31] -= route_bios[i];
}
static void test_e100_arp_modes(void)
{
    for (unsigned irq = 0; irq < 2; irq++) for (unsigned flags = 0; flags < 2; flags++)
        for (unsigned timing = 0; timing < 4; timing++) {
            struct model h; struct netif *d = &devices[0]; model_setup(&h, d, M_E100);
            h.rfd_count_flags = flags ? 0xc000 : 0; irq_add_fail = !irq;
            CHECK(!model_attach(&h, d) && d->irq_attached == !!irq); device_count = 1;
            net_worker_step(); /* drain setup CNA independently of the ARP */
            CHECK(!h.reg[1]); h.tx_auto = timing != 0;
            uint8_t frame[60]; CHECK(net_arp_request(frame, d->address) == sizeof(frame));
            CHECK(!netif_transmit(d, d->generation, frame, sizeof(frame)));
            struct e100_block *cb = model_dma(d->dma_phys + E100_TX, sizeof(*cb));
            CHECK(cb->command == 0xa004 && cb->count == 0x803c && cb->size == 8);
            CHECK(!memcmp(cb->data, frame, sizeof(frame)));
            if (!timing) {
                CHECK(!cb->status && (h.reg[0] & 0xc0) == 0x80);
                h.reg[1] |= 4; /* an interrupt before transmit DMA completes */
                if (irq) e100_test_interrupt(&h);
                net_worker_step(); CHECK(d->tx_used[0] && !d->quarantined && !d->stats.tx_errors);
                model_e100_complete_tx(&h);
            }
            if (irq && timing == 3) {
                /* TX IRQ first, then a peer's shared interrupt with no cause. */
                e100_test_interrupt(&h);
                unsigned writes = h.scb_write_count;
                uint32_t saved = irq_save(); CHECK(!netif_irq(d)); irq_restore(saved);
                CHECK(h.scb_write_count == writes && d->stats.interrupts == 1);
            }
            e100_arp_reply_frame(frame, d->address); model_receive(&h, frame, sizeof(frame), false);
            CHECK(e100_rfd(d, 0)->count == (h.rfd_count_flags | 60));
            if (irq && timing != 2 && timing != 3) {
                CHECK(model_e100_irq_asserted(&h)); e100_test_interrupt(&h);
            }
            if (irq && timing == 3) CHECK(!model_e100_irq_asserted(&h)); /* new FR while masked */
            if (irq && timing != 2) {
                CHECK(cb->status == 0xa000 && model_get(h.reg, 2) == (timing == 3 ? 0x4010u : 0x10u));
            }
            net_worker_step();
            CHECK(!d->quarantined && d->running && d->stats.rx_packets == 1 && d->stats.tx_completed == 1);
            CHECK(!d->stats.rx_errors && !d->stats.tx_errors && !d->stats.budget_violations);
            CHECK(!d->pending && !d->irq_pending && !h.reg[1] && h.reg[3] == (irq ? 0 : 1));
            if (irq && timing != 2) {
                e100_check_write(&h, 2, 1, timing == 3 ? 0x40 : 0, 1, false);
                e100_check_write(&h, 3, 3, 0, 1, false);
                CHECK(h.scb_write_count == 4);
            }
            CHECK(d->stats.interrupts == (!irq || timing == 2 ? 0u : timing == 0 ? 2u : 1u));
            struct net_packet packet; uint8_t peer[6];
            CHECK(netif_pop(d, &packet) && packet.length == sizeof(frame));
            CHECK(net_arp_reply(packet.data, packet.length, d->address, peer));
            CHECK(!memcmp(peer, "\x52\x55\x0a\x00\x02\x02", 6));
            CHECK(!e100_rfd(d, 0)->status && !e100_rfd(d, 0)->count && d->rx_head == 1 && d->rx_tail == 0);
            h.trace_scb = false; CHECK(!netif_stop(d) && !page_count && releases == 3);
            device_count = 0;
        }
    puts("PASS e100 ARP: IRQ/poll, count=c03c/003c, early/late/lost/shared IRQ, mask/W1C/worker/rearm");
}
static void test_e100_rfd_flags(void)
{
    const unsigned count_flags[] = {0,0x4000,0x8000,0xc000};
    uint8_t frame[60] = {0};
    for (unsigned qemu = 0; qemu < 2; qemu++) for (unsigned irq = 0; irq < 2; irq++)
        for (unsigned i = 0; i < ARRAY_SIZE(count_flags); i++) {
            struct model h; struct netif d; model_setup(&h, &d, M_E100);
            if (!qemu) e100_physical_profile();
            irq_add_fail = !irq;
            h.rfd_count_flags = count_flags[i]; CHECK(!model_attach(&h, &d) && d.irq_attached == !!irq);
            model_receive(&h, frame, sizeof(frame), false);
            if (irq) e100_test_interrupt(&h);
            netif_poll(&d);
            bool valid = count_flags[i] == 0xc000 || (qemu && !count_flags[i]);
            CHECK(d.quarantined == !valid && d.stats.rx_packets == (unsigned)valid);
            CHECK(d.stats.rx_errors == (unsigned)!valid);
            if (valid) { CHECK(!netif_stop(&d) && !page_count); }
            else CHECK(!strcmp(d.reason, "receive_fault") && !d.queue_count && !releases && page_count == 6);
        }
    for (unsigned qemu = 0; qemu < 2; qemu++) {
        struct model h; struct netif d; model_setup(&h, &d, M_E100);
        if (!qemu) e100_physical_profile();
        CHECK(!model_attach(&h, &d));
        e100_rfd(&d, 0)->count = 0xffff; /* count is irrelevant before C */
        netif_poll(&d); CHECK(!d.quarantined && !d.stats.rx_errors && !d.stats.rx_packets);
        model_receive(&h, frame, sizeof(frame), false); e100_rfd(&d, 0)->count = 0xffff;
        e100_test_interrupt(&h); netif_poll(&d);
        CHECK(d.quarantined && d.stats.rx_errors == 1 && !d.queue_count && !releases);
    }
    puts("PASS e100 RFD: physical F/EOF required, QEMU flagless allowed, partial/oversized faults, incomplete ignored");
}
/* f6-22: the T23 (5a10eef4) admitted its e100 with link up and never got a
 * DHCP lease. Every physical RFD carries bit 5 for an Ethernet II frame and
 * bit 1 for a broadcast; the former 0x0fff mask counted each as an error. */
static unsigned ingress_frames, ingress_length;
static uint8_t ingress_last[NET_FRAME_MAX];
static void e100_ingress(struct netif *d, const uint8_t *frame, unsigned bytes, void *cookie)
{
    (void)d; (void)cookie; CHECK(bytes <= sizeof(ingress_last));
    ingress_frames++; ingress_length = bytes; memcpy(ingress_last, frame, bytes);
}
/* A router's DHCPOFFER as many servers send it: Ethernet and IPv4 broadcast,
 * UDP 67 -> 68, BOOTREPLY with the client's chaddr (RFC 2131 section 4.1). */
static unsigned e100_dhcp_offer(uint8_t frame[342], const uint8_t client[6])
{
    static const uint8_t router[6] = {0x00,0x0d,0x88,0x01,0x02,0x03};
    memset(frame, 0, 342); memset(frame, 0xff, 6); memcpy(frame + 6, router, 6);
    frame[12] = 0x08; frame[13] = 0x00;
    uint8_t *ip = frame + 14;
    ip[0] = 0x45; ip[2] = 328 >> 8; ip[3] = 328 & 255; ip[8] = 64; ip[9] = 17;
    ip[12] = 192; ip[13] = 168; ip[14] = 1; ip[15] = 1; memset(ip + 16, 0xff, 4);
    uint8_t *udp = ip + 20; udp[1] = 67; udp[3] = 68; udp[4] = 308 >> 8; udp[5] = 308 & 255;
    uint8_t *b = udp + 8; b[0] = 2; b[1] = 1; b[2] = 6;
    b[16] = 192; b[17] = 168; b[18] = 1; b[19] = 20; memcpy(b + 28, client, 6);
    b[236] = 0x63; b[237] = 0x82; b[238] = 0x53; b[239] = 0x63;
    b[240] = 53; b[241] = 1; b[242] = 2; b[243] = 255;
    return 342;
}
static void e100_deliver(struct model *h, struct netif *d, const uint8_t *frame, unsigned len, unsigned status, bool accepted)
{
    unsigned frames = ingress_frames; uint64_t errors = d->stats.rx_errors;
    model_receive(h, frame, len, false);
    CHECK(e100_rfd(d, d->rx_head)->status == status);
    net_worker_step();
    CHECK(!d->quarantined && d->running);
    if (accepted) CHECK(ingress_frames == frames + 1 && ingress_length == len && !memcmp(ingress_last, frame, len) && d->stats.rx_errors == errors);
    else CHECK(ingress_frames == frames && d->stats.rx_errors == errors + 1);
}
static void test_e100_rfd_status(void)
{
    static const unsigned errors[] = {0x800,0x400,0x200,0x100,0x80,0x10};
    for (unsigned qemu = 0; qemu < 2; qemu++) {
        struct model h; struct netif *d = &devices[0]; model_setup(&h, d, M_E100);
        if (!qemu) { e100_physical_profile(); h.rfd_count_flags = 0xc000; }
        CHECK(!model_attach(&h, d)); device_count = 1;
        net_worker_step(); net_set_input(e100_ingress, NULL); ingress_frames = 0;
        uint8_t frame[NET_FRAME_MAX];
        unsigned n = e100_dhcp_offer(frame, d->address);
        e100_deliver(&h, d, frame, n, qemu ? 0xa002 : 0xa022, true);       /* broadcast type frame */
        e100_arp_reply_frame(frame, d->address);
        e100_deliver(&h, d, frame, 60, qemu ? 0xa000 : 0xa020, true);      /* unicast type frame */
        frame[12] = 0; frame[13] = 46;
        e100_deliver(&h, d, frame, 60, 0xa000, true);                      /* IEEE 802.3 length frame */
        e100_arp_reply_frame(frame, d->address);
        for (unsigned extra = 1; extra <= 4; extra *= 4) {                 /* collision/TCO, no match */
            h.rfd_status_extra = extra;
            e100_deliver(&h, d, frame, 60, (qemu ? 0xa000 : 0xa020) | extra, true);
        }
        for (unsigned i = 0; i < ARRAY_SIZE(errors); i++) {                /* every damaged-frame bit */
            h.rfd_status_extra = errors[i];
            e100_deliver(&h, d, frame, 60, (qemu ? 0xa000 : 0xa020) | errors[i], false);
        }
        h.rfd_status_extra = 0;
        CHECK(ingress_frames == 5 && d->stats.rx_packets == 5 && d->stats.rx_errors == ARRAY_SIZE(errors));
        net_set_input(0, 0); h.trace_scb = false;
        CHECK(!netif_stop(d) && !page_count); device_count = 0;
    }
    puts("PASS e100 RFD status: DHCP OFFER broadcast, Ethernet II, 802.3, TCO/no-match reach ingress; table 49 errors counted (physical and QEMU)");
}
static void test_e100_tx_timing(void)
{
    uint8_t frame[60] = {0};
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        struct model h; struct netif d; model_setup(&h, &d, M_E100);
        CHECK(!model_attach(&h, &d)); netif_poll(&d); h.tx_auto = false;
        CHECK(!netif_transmit(&d, d.generation, frame, sizeof(frame)));
        struct e100_block *cb = model_dma(d.dma_phys + E100_TX, sizeof(*cb));
        if (scenario != 2) cb->status = 0xa000; /* C can precede CU idle */
        h.reg[1] |= 4; e100_test_interrupt(&h); netif_poll(&d);
        CHECK(d.tx_used[0] && !d.stats.tx_completed && !d.quarantined);
        CHECK(netif_transmit(&d, d.generation, frame, sizeof(frame)) == -16);
        g_ticks = d.tx_deadline[0];
        if (!scenario) h.reg[0] &= ~0xc0; /* completion wins over deadline without a second IRQ */
        netif_poll(&d);
        if (!scenario) {
            CHECK(d.stats.tx_completed == 1 && !d.tx_used[0] && !d.quarantined);
            h.trace_scb = false; CHECK(!netif_stop(&d) && !page_count);
        } else CHECK(d.quarantined && d.stats.tx_errors == 1 && !releases && page_count == 6);
    }
    struct model h; struct netif d; model_setup(&h, &d, M_E100);
    CHECK(!model_attach(&h, &d)); h.reg[0] = h.reg[1] = 0xff;
    netif_poll(&d); CHECK(d.quarantined && !d.stats.rx_errors && !d.stats.tx_errors && !releases);
    puts("PASS e100 TX: C before CU idle, missed completion IRQ, CU/CB deadline faults, absent SCB fault");
}
static void test_e100_scb_bytes(void)
{
    struct model h; struct netif d; model_setup(&h, &d, M_E100);
    CHECK(!model_attach(&h, &d)); netif_poll(&d);
    h.reg[2] = 0x10; h.reg[1] = 4; e100_test_interrupt(&h);
    netif_poll(&d); CHECK(h.reg[2] == 0x10 && !d.quarantined);
    e100_check_write(&h, 2, 1, 0, 1, false); e100_check_write(&h, 3, 3, 0, 1, false);
    h.trace_scb = false;
    e100_irq_enable(&d, false); CHECK(h.reg[2] == 0x10 && h.reg[3] == 1);
    e100_irq_enable(&d, true); CHECK(h.reg[2] == 0x10 && !h.reg[3]);
    h.command_busy_reads = 3; unsigned slept = sleeps;
    CHECK(!e100_command(&d, 1, d.dma_phys) && sleeps == slept + 3 && !h.reg[2]);
    /* RNR plus a pending command is deferred by polling, never overwritten. */
    h.reg[0] = 8; h.reg[1] = 0x10; h.reg[2] = 1; e100_test_interrupt(&h);
    unsigned pointer = h.pointer; h.command_busy_reads = 2;
    netif_poll(&d); CHECK(!d.quarantined && h.pointer == pointer && h.reg[2] == 1);
    netif_poll(&d); CHECK(!d.quarantined && h.pointer == pointer && !h.reg[2]);
    h.trace_scb = false; netif_poll(&d); CHECK(h.reg[0] == 0x10 && !d.quarantined);
    /* A genuinely wedged command retains the bounded error path. */
    h.reg[2] = 0x10; slept = sleeps;
    CHECK(e100_command(&d, 1, d.dma_phys) == -110 && sleeps == slept + 100);
    CHECK(h.reg[2] == 0x10 && h.pointer == pointer);
    h.reg[2] = 0; CHECK(!netif_stop(&d) && !page_count);
    puts("PASS e100 SCB: byte-3 mask preserves busy byte-2 command, repeated W1C safe, deferred RNR, bounded timeout");
}
static void test_e100_irq(void)
{
    test_e100_arp_modes();
    test_e100_rfd_flags();
    test_e100_rfd_status();
    test_e100_tx_timing();
    test_e100_scb_bytes();
}
