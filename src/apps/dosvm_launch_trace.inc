/* Parent-only persistence of held child launch checkpoints. The child cannot
 * pass a checkpoint before a complete write and successful close. */
static u8 launch_record[256];

static int launch_record_save(int vm)
{
    char path[32];
    int h, wrote, closed;
    /* Keep the last DPMI/command boundary when VMFORK later reports exit.
     * This replaces its existing write; it does not add checkpoint I/O. */
    if (*(u16 *)(launch_record + 12) >= 64) {
        str_copy(path, "\\SYSTEM\\DPMIRUN1.BIN");
        path[15] = '0' + vm;
    } else {
        str_copy(path, "\\SYSTEM\\VMFORK1.BIN");
        path[14] = '0' + vm;
    }
    h = dos_create(path);
    if (h < 0) return 0;
    wrote = dos_write(h, launch_record, sizeof launch_record);
    closed = dos_close(h);
    return wrote == sizeof launch_record && closed >= 0;
}

static void launch_trace_poll(struct guest_window *g)
{
    struct regs r;
    u32 sequence;
    u16 code, generation;
    if (g->trace_failed) return;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x52; r.cx = 1; r.bx = g->vm; r.dx = g->generation;
    r.di = (u16)launch_record;
    if (vm_call(&r)) {
        /* A completed/reused slot cannot publish another checkpoint. An
         * error for this still-live generation must not strand a held VM. */
        if (vm_status(g->vm, &code, &generation) == VM_READY &&
            generation == g->generation) goto failed;
        return;
    }
    if (r.cx != 1) return;
    if (*(u32 *)(launch_record + 0) != 0x4C465643UL ||
        *(u32 *)(launch_record + 4) != 0x01000001UL ||
        *(u16 *)(launch_record + 8) != g->vm ||
        *(u16 *)(launch_record + 10) != g->generation) goto failed;
    sequence = *(u32 *)(launch_record + 40);
    if (!sequence || !launch_record_save(g->vm)) goto failed;
    if (!g->launch_snapshot_saved) {
        snapshot_sample(g);
        if (!snapshot_save(g, SNAP_LAUNCH, VM_READY, 0xFFFF)) goto failed;
        g->launch_snapshot_saved = 1;
    }
    mem_set(&r, 0, sizeof r);
    r.ax = 0x52; r.cx = 2; r.bx = g->vm; r.dx = g->generation;
    r.si = (u16)sequence; r.cxh = (u16)(sequence >> 16);
    if (vm_call(&r)) goto failed;
    return;
failed:
    /* Never ACK a short/failed write or failed close. Kill releases the held
     * private VM; if kill itself fails the pending mailbox still prevents run. */
    g->trace_failed = 1;
    disk_log("Launch checkpoint failed", g->title, 0xF0E2);
    mem_set(&r, 0, sizeof r);
    r.ax = 0x45; r.bx = g->vm;
    vm_call(&r);
}
