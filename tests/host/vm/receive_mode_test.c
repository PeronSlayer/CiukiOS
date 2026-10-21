/* Physical receive-mode operations and native software destination filtering.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main unused_nic_suite_main
#include "../net/net_test.c"
#undef main
static void first_set(enum model_kind kind)
{
    struct model h; struct netif d;
    model_setup(&h, &d, kind); CHECK(!model_attach(&h, &d));
    unsigned writes = h.writes;
    CHECK(netif_promiscuous(&d, d.generation + 1, true) == -19 && h.writes == writes);
    uint8_t normal[22];
    if (kind == M_E100) e100_config(&d, normal, false);
    uint32_t rtl_config = model_get(h.reg + 0x44, 4);
    CHECK(!netif_promiscuous(&d, d.generation, true) && d.promiscuous);
    if (kind == M_RTL) CHECK(model_get(h.reg + 0x44, 4) == (rtl_config | 1));
    if (kind == M_NE) CHECK(h.reg[0x0c] == 0x14 && !(h.reg[0] & 0xc0));
    if (kind == M_E100) {
        CHECK(d.dma[E100_SETUP + 8 + 15] == (normal[15] | 1));
        for (unsigned i = 0; i < 22; i++) if (i != 15) CHECK(d.dma[E100_SETUP + 8 + i] == normal[i]);
        CHECK((h.reg[0] & 0x3c) == 0x10); /* RU remained ready. */
        d.tx_used[0] = true; writes = h.writes;
        CHECK(netif_promiscuous(&d, d.generation, false) == -16);
        CHECK(d.promiscuous && h.writes == writes && !d.quarantined);
        d.tx_used[0] = false;
    }
    struct net_packet packet = {.length = 60};
    memcpy(packet.data, d.address, 6); CHECK(native_destination(&d, &packet));
    packet.data[5] ^= 1; CHECK(!native_destination(&d, &packet));
    memset(packet.data, 255, 6); CHECK(native_destination(&d, &packet));
    packet.data[0] = 1; CHECK(native_destination(&d, &packet));
    packet.length = 13; CHECK(!native_destination(&d, &packet));
    CHECK(!netif_promiscuous(&d, d.generation, false) && !d.promiscuous);
    if (kind == M_RTL) CHECK(model_get(h.reg + 0x44, 4) == rtl_config);
    if (kind == M_NE) CHECK(h.reg[0x0c] == 4);
    if (kind == M_E100) CHECK(!memcmp(d.dma + E100_SETUP + 8, normal, 22));
    CHECK(!netif_stop(&d)); writes = h.writes;
    CHECK(netif_promiscuous(&d, d.generation, true) == -19 && writes == h.writes);
}
int main(void)
{
    for (enum model_kind kind = M_RTL; kind <= M_NE; kind++) first_set(kind);
    struct second_model h; struct netif d;
    second_setup(&h, &d, S_E1000); CHECK(!second_attach(&h, &d));
    uint32_t rctl = h.intel[0x100 / 4];
    CHECK(!netif_promiscuous(&d, d.generation, true) && d.promiscuous);
    CHECK(h.intel[0x100 / 4] == (rctl | 8));
    CHECK(!netif_promiscuous(&d, d.generation, false));
    CHECK(h.intel[0x100 / 4] == rctl && !d.promiscuous); CHECK(!netif_stop(&d));
    second_setup(&h, &d, S_PCNET); CHECK(!second_attach(&h, &d));
    CHECK(netif_promiscuous(&d, d.generation, true) == -ENOSYS && !d.promiscuous && !d.quarantined);
    CHECK(!netif_stop(&d));
    printf("receive mode: %u checks passed\n", checks); return 0;
}
