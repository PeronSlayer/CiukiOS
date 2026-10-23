/* Strict UHCI 1.1 section 3.4 schedule walker, shared by HID and BOT fixtures.
 * The peripheral callback changes TD status and returns whether it ACKed.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_TEST_UHCI_MODEL_H
#define CIUKI_TEST_UHCI_MODEL_H
struct uhci_model_io {
    void *cookie;
    volatile uint32_t *(*dma)(void *, uint32_t, unsigned);
    bool (*transaction)(void *, uint32_t, uint32_t, volatile uint32_t *);
    uint32_t *status;
    /* Optional separation of TD status from the later QH element writeback.
     * f1-96b: returns whether the element advanced; a queue whose element
     * did not advance (early-Intel erratum) is left for the horizontal link. */
    bool (*advance)(void *, uint32_t, volatile uint32_t *, uint32_t);
    /* fx-17 negative control: the QH whose horizontal link the walker treats
     * as terminate (the driver's reclamation loop removed), or 0. */
    uint32_t no_reclamation_qh;
};
static int uhci_model_frame(const struct uhci_model_io *io, uint32_t link)
{
    volatile uint32_t *qh = NULL;
    uint32_t qh_address = 0, horizontal = 1;
    unsigned reserved = 12;
    unsigned transactions = 16; /* conservative full-speed frame bandwidth */
    /* fx-17: the driver closes a bandwidth-reclamation loop (terminating QH
     * back to the control QH) while a bulk stage runs. The HC keeps cycling
     * until the frame's bandwidth is spent; a NAK costs one transaction
     * slot. A full pass that executes nothing (every TD inactive or the
     * queues empty) idles to the end of the frame. Malformed cycles stay
     * bounded by the step limit. */
    enum { SEEN_MAX = 96 }; /* every interrupt QH plus control, bulk and terminating QHs */
    uint32_t seen[SEEN_MAX] = {0}; unsigned seen_tx[SEEN_MAX] = {0}, seen_count = 0;
    for (unsigned steps = 0; steps < 1024; steps++) {
        if (link & 1) {
            if (!qh) return 0;
            link = horizontal; qh = NULL; reserved = 12;
            continue;
        }
        /* Bit 2 is ignored in a QH element, but reserved in a frame/horizontal
         * link. Bit 3 is always reserved. Never silently mask illegal bits. */
        if (link & reserved) return -USB_EPROTO;
        uint32_t address = link & ~15u;
        if (link & 2) {
            unsigned i = 0;
            while (i < seen_count && seen[i] != address) i++;
            if (i < seen_count && seen_tx[i] == transactions) return 0; /* idle reclamation pass */
            if (i == seen_count && seen_count < SEEN_MAX) seen[seen_count++] = address;
            if (i < SEEN_MAX) seen_tx[i] = transactions;
            qh = io->dma(io->cookie, address, 8); qh_address = address;
            horizontal = io->no_reclamation_qh && address == io->no_reclamation_qh ? 1 : qh[0]; link = qh[1]; reserved = 8;
            continue; /* entering a QH replaces, never stacks, the Q context */
        }
        volatile uint32_t *td = io->dma(io->cookie, address, 16);
        if (td[0] & 8) return -USB_EPROTO;
        bool active = td[1] & (1u << 23);
        if (active && !transactions--) return 0;
        bool ack = active && io->transaction(io->cookie, address, qh ? qh_address : 0, td);
        /* IOC is status, not permission to execute an inactive TD. The caller
         * routes the resulting status only when its IRQ gate is enabled. */
        if ((td[1] & (1u << 24)) && !(td[1] & (1u << 23))) *io->status |= 1;
        if (td[1] & (1u << 20)) return 0; /* babble ends this frame */
        if (!qh) { link = td[0]; reserved = 8; continue; }
        unsigned expected = (((td[2] >> 21) & 0x7ff) + 1) & 0x7ff;
        unsigned actual = ((td[1] & 0x7ff) + 1) & 0x7ff;
        bool short_packet = ack && (td[1] & (1u << 29)) && (td[2] & 255) == USB_PID_IN && actual < expected;
        if (short_packet) *io->status |= 1;
        bool advanced = false;
        if (ack && !short_packet) {
            if (io->advance) advanced = io->advance(io->cookie, address, qh, td[0]);
            else { qh[1] = td[0]; advanced = true; }
        }
        if (advanced && (td[0] & 4)) { link = td[0]; reserved = 8; }
        else { link = horizontal; qh = NULL; reserved = 12; }
    }
    return -USB_EPROTO;
}
#endif
