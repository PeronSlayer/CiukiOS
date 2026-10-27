/* sec-k05 (H06): a VM retains its creator's immutable identity from before
 * guest boot until it is DEAD; BIOS INT 13h of a user VM (administrator
 * included) reaches only its private provider: no physical FDC lease, no
 * other drive number, no transfer or buffer change on refusal. Included by
 * dosvm_test.c after the production sources; fakes in dosvm_video_test.inc.
 * SPDX-License-Identifier: GPL-2.0-only */
/* --- sec-k07 (begin): logoff teardown calls the production
 * dosvm_session_revoke from PID 1 while the session is CLOSING; it ends
 * exactly that session's window VMs, leaves others running, is idempotent
 * and reports what it had to retain (0 here: no binding was busy). */
static void test_session_revoke(void)
{
    struct process *saved_current = fake_current;
    window_fixtures();
    fake_identity(1, true, true); /* proc 1: Standard user, session 8; proc 0: session 7 */
    struct ciuki_dosvm_command a, b;
    fake_current = &fake_proc[0]; CHECK(!create_window(0, "Alice", &a));
    fake_current = &fake_proc[1]; CHECK(!create_window(3, "Bob", &b));
    struct dosvm_key ka = {a.id, a.generation}, kb = {b.id, b.generation};
    CHECK(dosvm_valid(ka) && dosvm_valid(kb) && fake_subject[0].refs && fake_subject[1].refs);
    CHECK(!dosvm_session_revoke(9) && !dosvm_session_revoke(0) && dosvm_valid(ka) && dosvm_valid(kb));
    CHECK(!dosvm_session_revoke(7) && !dosvm_valid(ka) && dosvm_valid(kb) && !fake_subject[0].refs);
    CHECK(!dosvm_session_revoke(7) && dosvm_valid(kb));
    CHECK(!dosvm_session_revoke(8) && !dosvm_valid(kb) && !fake_subject[1].refs);
    { struct dosvm_ledger l; dosvm_snapshot(&l); CHECK(!l.vms); }
    memset(fake_proc, 0, sizeof(fake_proc));
    fake_current = saved_current;
    empty();
    puts("H09 DOS teardown (sec-k07): dosvm_session_revoke ends only the closing session's window VMs, idempotently PASS");
}
/* --- sec-k07 (end) --- */
static void test_vm_identity(void)
{
    empty();
    memset(fake_proc, 0, sizeof(fake_proc));
    for (unsigned i = 0; i < 3; i++) { fake_proc[i].pid = 10 + i; fake_proc[i].state = PROC_LIVE; }
    fake_identity(0, true, false); fake_identity(1, true, true); fake_identity(2, false, false);
    /* Capture: an interactive session only; SYSTEM/revoked callers have none. */
    struct dos_identity admin, standard, none;
    CHECK(dos_identity_capture(&none, &fake_proc[2]) == -EPERM && !none.subject);
    fake_subject[0].live = false; CHECK(dos_identity_capture(&none, &fake_proc[0]) == -EPERM); fake_subject[0].live = true;
    fake_subject[0].credential.flags = 0; CHECK(dos_identity_capture(&none, &fake_proc[0]) == -EPERM);
    fake_subject[0].credential.flags = CIUKI_CRED_INTERACTIVE;
    CHECK(!dos_identity_capture(&admin, &fake_proc[0]) && admin.admin && !admin.system && fake_subject[0].refs == 1);
    CHECK(!dos_identity_capture(&standard, &fake_proc[1]) && !standard.admin && fake_subject[1].refs == 1);
    CHECK(dos_identity_owner(&admin, &fake_proc[0]) && !dos_identity_owner(&admin, &fake_proc[1]) &&
          !dos_identity_owner(&standard, &fake_proc[0]) && !dos_identity_owner(&admin, &fake_proc[2]));
    /* The VM takes its own reference before any storage/guest work. */
    struct dosvm *v = 0; struct dosvm_config config = {.commitment = DOSVM_LOW_CAP, .identity = &standard};
    CHECK(!dosvm_create(&config, &v) && v && fake_subject[1].refs == 2 && v->identity.credential.uid == 1001 &&
          !v->identity.system && v->file_gate.identity == &v->identity);
    dos_identity_release(&standard);
    CHECK(fake_subject[1].refs == 1 && v->identity.subject == &fake_subject[1]);
    /* Its scratch overlay lives in the owner's committed profile, never the
     * machine directory (a Standard user cannot write C:\SYSTEM\DOS). */
    static const char scratch[] = "C:\\Windows\\Profiles\\u000003e9\\Settings\\DOS\\";
    CHECK(!strncmp(v->overlay_path, scratch, sizeof(scratch) - 1) && !strcmp(last_mkdir, "C:\\Windows\\Profiles\\u000003e9\\Settings\\DOS"));
    /* No physical FDC for a user VM, even before guest boot. */
    CHECK(dosvm_floppy_assign(v->info.key, 0, 0) == -FS_EACCES);
    /* INT 13h: only the private provider (drive 80h). */
    struct v86_frame f = disk_frame(0x0201); uint8_t sentinel[512], got[512];
    memset(sentinel, 0xa5, sizeof(sentinel)); CHECK(!dosvm_write(v, 0x10FFF, sentinel, sizeof(sentinel)));
    unsigned reads = disk_reads, writes = disk_writes;
    static const uint8_t forged[] = {0x81, 0x82, 0x9f, 0xe0, 0xff};
    for (unsigned i = 0; i < sizeof(forged); i++) for (unsigned fn = 0; fn < 4; fn++) {
        static const uint16_t ops[] = {0x0201, 0x0301, 0x4200, 0x4300};
        f = disk_frame(ops[fn]); f.tf.edx = forged[i];
        CHECK(!dosvm_bios(v, &f, 0x13) && (f.tf.eflags & V86_CF) && ((f.tf.eax >> 8) & 255) == 1);
    }
    CHECK(disk_reads == reads && disk_writes == writes);
    CHECK(!dosvm_read(v, 0x10FFF, got, sizeof(got)) && !memcmp(got, sentinel, sizeof(got)));
    f = disk_frame(0x0201); CHECK(!dosvm_bios(v, &f, 0x13) && !(f.tf.eflags & V86_CF) && disk_reads > reads);
    /* Released exactly once, when the VM is DEAD. */
    destroy(v, false);
    CHECK(fake_subject[1].refs == 0);
    dos_identity_release(&admin); CHECK(fake_subject[0].refs == 0);
    /* An internal VM (no user caller) is SYSTEM and never reaches a profile. */
    config.identity = 0; CHECK(!dosvm_create(&config, &v) && v->identity.system && !v->identity.profile[0]);
    destroy(v, false);
    memset(fake_proc, 0, sizeof(fake_proc));
    empty();
    puts("H06 VM identity: PASS (capture refusals, own reference before boot, owner match, no physical FDC, INT13 private provider only, release when DEAD)");
    test_session_revoke(); /* sec-k07 */
}
