/* First F3 lifecycle row. Runtime-selected; no build variants or firmware.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/dosvm.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/storage.h>

static bool prompt(struct dosvm *v)
{
    const uint16_t *text = dosvm_text(v, 0);
    for (unsigned row = 0; row < 25; row++) {
        bool drive = false;
        for (unsigned col = 0; col < 80; col++) {
            unsigned i = row * 80 + col;
            if (col < 78 && (uint8_t)text[i] == 'C' && (uint8_t)text[i + 1] == ':' && (uint8_t)text[i + 2] == '\\') drive = true;
            if (drive && (uint8_t)text[i] == '>') return true;
        }
    }
    return false;
}
static int boot_prompt(struct dosvm *v)
{
    int e = dosvm_boot(v);
    if (e) return e;
    uint64_t until = deadline_after_ms(30000);
    for (;;) {
        struct dosvm_info info;
        dosvm_info(v, &info);
        if (info.state >= DOSVM_STOPPING) return -V86_EIO;
        if (info.dos_ready && prompt(v)) return 0;
        if (deadline_passed(until)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
}
static int command(struct dosvm *v, const char *text)
{
    struct dosvm_key key = dosvm_key(v);
    for (unsigned i = 0; text[i]; i++)
        if (!dosvm_keyboard(key, (uint8_t)text[i])) return -ENOSPC;
    if (!dosvm_keyboard(key, 0x1C0D)) return -ENOSPC;
    uint64_t until = deadline_after_ms(5000);
    for (;;) {
        struct dosvm_info info;
        dosvm_info(v, &info);
        if (info.state >= DOSVM_STOPPING) return 0;
        if (deadline_passed(until)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
}

static struct task *heartbeat(uint32_t *data)
{
    struct task *t = task_create_user("f3-heartbeat", P_NORMAL, 0x400000, 0x800000, 0, 0, 0);
    if (!t) return 0;
    static const uint8_t code[] = {0xFF,0x05,0x00,0x00,0x50,0x00,0xEB,0xF8};
    for (unsigned i = 0; i < 3; i++) {
        uint32_t p = pmm_alloc();
        if (!p) goto fail;
        memset(P2V(p), 0, PAGE_SIZE);
        uint32_t address = i == 0 ? 0x400000 : i == 1 ? 0x500000 : 0x7FF000;
        if (as_map(&t->as, address, p, PTE_U | (i ? PTE_W : 0))) { pmm_free(p); goto fail; }
        if (i == 0) memcpy(P2V(p), code, sizeof(code));
        if (i == 1) *data = p;
    }
    task_start(t);
    return t;
fail:
    task_kill(t, 0); task_reap(t); return 0;
}
static bool ledger_equal(const struct dosvm_ledger *a, const struct dosvm_ledger *b)
{
    return a->vms == b->vms && a->tasks == b->tasks && a->pages == b->pages &&
           a->handles == b->handles && a->devices == b->devices &&
           a->reserved == b->reserved && a->resident == b->resident;
}

static int probe_f3_vm_boot(void)
{
    rec_emit("vm-boot", "BEGIN", "version=1");
    struct dosvm *witness = 0, *v = 0;
    struct task *native = 0;
    uint32_t native_data = 0;
    unsigned clean = 0, faults = 0, privileged = 0, ports = 0;
    int error = 0;
    bool isolated = true, restored = true;
    const struct dosvm_config config = {DOSVM_LOW_CAP};
    struct dosvm_ledger before, baseline, after;
    dosvm_snapshot(&before);
    /* Test boots start with read-only mounts. The existing F1 write gate
     * must qualify C: before private scratch overlays can be created. */
    if ((error = storage_enable_write(storage_get(), 2))) goto cleanup;
    native = heartbeat(&native_data);
    if (!native) { error = -ENOMEM; goto cleanup; }
    if ((error = dosvm_create(&config, &witness)) || (error = boot_prompt(witness))) goto cleanup;
    /* Warm the retained heap pools before measuring the repeated lifecycle. */
    if ((error = dosvm_create(&config, &v)) || (error = boot_prompt(v))) goto cleanup;
    struct dosvm_info first;
    dosvm_info(v, &first);
    rec_emit("vm-boot", "DATA", "case=boot ready=%u prompt=%u major=%u minor=%u free_paras=%u drive=%u",
             first.dos_ready, prompt(v), first.dos_major, first.dos_minor, first.free_paragraphs, first.drive);
    if ((error = command(v, "EXIT")) || (error = dosvm_destroy(dosvm_key(v), false))) goto cleanup;
    v = 0;
    /* Neither sentinel is the synthesized vector-60 default F000:0600. */
    uint32_t ivt = 0xF0000770, candidate_ivt = 0xF0000780;
    uint8_t sector[512], verify[512];
    memset(sector, 0x6B, sizeof(sector));
    if ((error = dosvm_write(witness, 0x180, &ivt, 4)) ||
        (error = dosvm_disk_write(witness, 262143, sector))) goto cleanup;
    dosvm_snapshot(&baseline);
    uint32_t free_before = pmm_free_count(), tasks_before = g_task_count;
    size_t heap_before = kheap_in_use();
    uint32_t native_before = *(volatile uint32_t *)P2V(native_data);
    for (unsigned cycle = 0; cycle < 200; cycle++) {
        if ((error = dosvm_create(&config, &v)) || (error = boot_prompt(v))) break;
        uint32_t observed;
        if ((error = dosvm_read(v, 0x180, &observed, 4)) || observed == ivt ||
            (error = dosvm_disk_read(v, 262143, verify)) || !memcmp(verify, sector, sizeof(sector))) {
            isolated = false; error = error ? error : -EFAULT; break;
        }
        memset(verify, 0xA7, sizeof(verify));
        if ((error = dosvm_write(v, 0x180, &candidate_ivt, 4)) ||
            (error = dosvm_disk_write(v, 262143, verify))) break;
        bool fault = cycle >= 100;
        if ((error = command(v, !fault ? "EXIT" : cycle & 1 ? "VMPORT.COM" : "VMPRIV.COM"))) break;
        struct dosvm_info ended;
        dosvm_info(v, &ended);
        if (ended.reason != (fault ? DOSVM_FAULT : DOSVM_EXIT) || (fault && ended.fault_vector != 13)) {
            error = -EFAULT; break;
        }
        struct dosvm_key expired = dosvm_key(v);
        if ((error = dosvm_destroy(expired, fault))) break;
        v = 0;
        if (dosvm_irq_raise(expired, 0) || dosvm_keyboard(expired, 'X')) { error = -EFAULT; break; }
        if ((error = dosvm_read(witness, 0x180, &observed, 4)) || observed != ivt ||
            (error = dosvm_disk_read(witness, 262143, verify)) || memcmp(verify, sector, sizeof(sector))) {
            isolated = false; error = error ? error : -EFAULT; break;
        }
        dosvm_snapshot(&after);
        if (!ledger_equal(&baseline, &after) || pmm_free_count() != free_before ||
            g_task_count != tasks_before || kheap_in_use() != heap_before) {
            restored = false; error = -ENOMEM; break;
        }
        if (fault) { faults++; if (cycle & 1) ports++; else privileged++; }
        else clean++;
    }
    dosvm_snapshot(&after);
    uint32_t native_after = *(volatile uint32_t *)P2V(native_data);
    rec_emit("vm-boot", "DATA", "case=cycles clean=%u faults=%u privileged=%u ports=%u", clean, faults, privileged, ports);
    rec_emit("vm-boot", "DATA", "case=isolation ivt=%u disk=%u heartbeat=%u native_dispatches=%u",
             isolated, isolated, native_after != native_before, native->dispatches);
    rec_emit("vm-boot", "DATA", "case=baselines pages_before=%u pages_after=%u handles_before=%u handles_after=%u devices_before=%u devices_after=%u restored=%u",
             free_before, pmm_free_count(), baseline.handles, after.handles, baseline.devices, after.devices, restored);
    if (clean != 100 || faults != 100 || native_after == native_before || !isolated || !restored) error = -EFAULT;
cleanup:
    if (v) { int e = dosvm_destroy(dosvm_key(v), true); if (!error) error = e; }
    if (witness) { int e = dosvm_destroy(dosvm_key(witness), true); if (!error) error = e; }
    if (native) { task_kill(native, 0); task_reap(native); }
    dosvm_snapshot(&after);
    bool cleaned = ledger_equal(&before, &after);
    if (!cleaned && !error) error = -ENOMEM;
    rec_emit("vm-boot", "DATA", "case=cleanup vms=%u tasks=%u pages=%u handles=%u devices=%u restored=%u",
             after.vms, after.tasks, after.pages, after.handles, after.devices, cleaned);
    rec_emit("vm-boot", "END", "status=%s error=%d", error ? "FAIL" : "PASS", error);
    return !!error;
}
CIUKI_F3_PROBE("vm-boot", probe_f3_vm_boot);
