/* F3-47: default game commitment through the real DPMI services. Synthetic
 * client setup, NOT execution of DOS/4GW. SPDX-License-Identifier: GPL-2.0-only */
static void test_zone_memory(void)
{
    struct dosvm *v = dpmi_vm_commitment(true, DOSVM_RESIDENT_CAP);
    uint32_t largest, total;
    dosvm_pool_query(v, &largest, &total);
    CHECK(largest == VM_POOL_CAP && total == VM_POOL_CAP);
    v->info.state = DOSVM_CREATING;
    CHECK(!dosvm_boot(v));
    dosvm_pool_query(v, &largest, &total);
    CHECK(largest == VM_POOL_CAP && total == VM_POOL_CAP);
    v86_start(); enter(v, true);
    uint16_t data = r16(cur.tf.ds); uint32_t info[12];
    cur.tf.es = data; cur.tf.edi = 0x300; int31(v, 0x0500); CHECK(!cf());
    CHECK(!dosvm_read(v, (DATA_SEG << 4) + 0x300, info, sizeof(info)));
    CHECK(info[0] == VM_POOL_CAP - DPMI_HSTACK_BYTES && info[5] * PAGE_SIZE == info[0]);
    printf("f3-47 memory default: boot largest/total=%u/%u client-entry 0500h=%u/%u\n",
        largest, total, info[0], info[5] * PAGE_SIZE);
    /* Representative 1 MiB extender/program block, then a 16 MiB zone. */
    cur.tf.ebx = 0x10; cur.tf.ecx = 0; int31(v, 0x0501); CHECK(!cf());
    cur.tf.es = data; cur.tf.edi = 0x300; int31(v, 0x0500); CHECK(!cf());
    CHECK(!dosvm_read(v, (DATA_SEG << 4) + 0x300, info, sizeof(info)));
    CHECK(info[0] >= (20u << 20) && info[5] * PAGE_SIZE == info[0]);
    printf("f3-47 memory synthetic-extender 0500h largest/total=%u/%u >=20MiB\n", info[0], info[5] * PAGE_SIZE);
    cur.tf.ebx = 0x100; cur.tf.ecx = 0; int31(v, 0x0501); CHECK(!cf());
    CHECK(v->perf.memory.queries == 2 && v->perf.memory.allocations == 2);
    CHECK(v->perf.memory.requested == (16u << 20));
    CHECK(v->perf.memory.after_largest == VM_POOL_CAP - DPMI_HSTACK_BYTES - (17u << 20));
    destroy(v, true); empty();
}
