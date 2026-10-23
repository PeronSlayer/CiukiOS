/* fx-4 (T23 trial of image 9a60215f): DOS window CREATE failed with
 * error=-5 (DOOM, Quake) and error=-4 (CiukiDOS) with no other record.
 * Production dosvm_create/client paths with the fake PMM/VFS transport:
 * every refusal leaves one record naming its stage, internal error and
 * step (memory: the shortage); an owner ended during the cold base hash
 * stops it; repeated launch/close cycles at the 128 MiB settled figure
 * return every page. The real inventory/partition path is tested in
 * tests/host/desktop/program_owner_test.c, the zone figures in the
 * production PMM/ledger replay (ledger_test.c --dosvm-profile).
 * SPDX-License-Identifier: GPL-2.0-only */
static struct ciuki_dosvm_command fx4_window(const char *name, unsigned game)
{
    struct ciuki_dosvm_command c = command_for(0, 0);
    c.surface_fd = 0; memcpy(c.name, name, strlen(name)); c.launch.game = game;
    return c;
}
static unsigned fx4_stop_after;
static void fx4_stop_owner(void)
{
    if (fx4_stop_after && !--fx4_stop_after) fake_proc[0].state = PROC_STOPPING;
}
static void test_fx4_vm_create(void)
{
    window_fixtures(); empty();
    uint32_t saved_bytes = bundled_bytes; void (*saved_sector)(uint32_t, void *) = disk_base_sector;
    bundled_bytes = 27676672u; disk_base_sector = 0;    /* the trial's UNIFIED.IMG size */
    fake_current = &fake_proc[0];
    fake_dma_free_pages = 574;                          /* trial zone 1: usable=3840 free=574 */

    /* 1. The trial's DOOM/Quake failure: the inventory mounted the MBR as a
     * BPB (EINVAL), the window ABI reported EIO. The record names it. */
    unsigned refusals = refusal_logs, errors = launch_errors;
    struct ciuki_dosvm_command w = fx4_window("DOOM", 2);
    program_result = -FS_EINVAL; program_step = "mount"; program_detail = "BPB signature/sector size/jump";
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) == -EIO);
    CHECK(refusal_logs == refusals + 1 && launch_errors == errors + 1 && refusal_sequence < launch_error_sequence);
    CHECK(strstr(last_refusal_log, "[dosvm] create refused stage=game-inventory error=-22 game=2 reason=invalid step=mount detail=BPB signature/sector size/jump dma_free_pages=574 normal_free_pages="));
    CHECK(strstr(last_launch_log, "[launch] name=dosbox pid=10 error=-5 stage=vm-create window=DOOM"));
    CHECK(fake_surface[0].references == 1 && !creating[0]); empty();
    printf("fx-4 trial refusal: %s\n", last_refusal_log);
    /* A removed game keeps its own errno and names the step. */
    program_result = -ENOENT; program_step = "program-tree"; program_detail = "none";
    w = fx4_window("Quake", 3); CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) == -ENOENT);
    CHECK(strstr(last_refusal_log, "stage=game-inventory error=-2 game=3 reason=not-installed step=program-tree"));
    program_result = 1; w = fx4_window("Quake", 3); CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) == -ENOENT);
    CHECK(strstr(last_refusal_log, "stage=game-inventory error=-2 game=3 reason=not-installed step=pending-removal"));
    program_result = 0; program_step = "fixture"; empty();

    /* 2. Admission refusal names the shortage: the arithmetic and the
     * free pages of each zone. One page more is admitted (boundary). */
    const uint32_t need = (DOSVM_RESIDENT_CAP + DOSVM_HEADROOM) / PAGE_SIZE;
    withheld_pages = 0; withheld_pages = pmm_free_count() - need + 1;
    w = fx4_window("DOOM", 2); refusals = refusal_logs;
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) == -ENOMEM && refusal_logs == refusals + 1);
    char expect[200];
    snprintf(expect, sizeof(expect), "stage=admission error=-12 game=2 reason=memory free_kib=%u outstanding_kib=0 request_kib=49152 headroom_kib=32768 dma_free_pages=574 normal_free_pages=%u",
             (need - 1) * 4u, need - 1);
    CHECK(strstr(last_refusal_log, expect));
    CHECK(strstr(last_launch_log, "error=-12 stage=vm-create window=DOOM")); empty();
    printf("fx-4 admission refusal: %s\n", last_refusal_log);
    withheld_pages--; w = fx4_window("DOOM", 2); refusals = refusal_logs;
    CHECK(!dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) && refusal_logs == refusals);
    struct ciuki_dosvm_command shut = command_for(w.id, w.generation);
    CHECK(!finish_test_vm(&shut, sizeof(shut))); empty(); withheld_pages = 0;
    /* A late allocation failure names its stage and the zones. */
    allocation_calls = 0; fail_at = 3; w = fx4_window("DOOM", 2);
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) == -ENOMEM); fail_at = 0;
    CHECK(strstr(last_refusal_log, "stage=low-memory error=-12 game=2 reason=memory step=none") &&
          strstr(last_refusal_log, "dma_free_pages=574 normal_free_pages=")); empty();

    /* 3. The owner ended (Tasks) during the cold identity hash of the
     * 27.7 MB base: the hash stops at the next block with EINTR (-4),
     * the trial's CiukiDOS record, and nothing is cached or leaked. */
    dosvm_game_digest_invalidate();
    const unsigned blocks = (bundled_bytes + DOSVM_READ_SECTORS * 512u - 1) / (DOSVM_READ_SECTORS * 512u);
    unsigned reads = disk_reads; refusals = refusal_logs;
    fx4_stop_after = 5; disk_read_hook = fx4_stop_owner;
    w = fx4_window("CiukiDOS", 0);
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) == -EINTR);
    disk_read_hook = 0; fake_proc[0].state = PROC_LIVE;
    CHECK(disk_reads - reads == 5 && disk_reads - reads < blocks && refusal_logs == refusals + 1);
    CHECK(strstr(last_refusal_log, "stage=disk error=-4 game=0 reason=owner-gone step=base-identity"));
    CHECK(strstr(last_launch_log, "error=-4 stage=vm-create window=CiukiDOS"));
    CHECK(fake_surface[0].references == 1 && !creating[0]); empty();
    /* The next launch hashes the whole base again (nothing was cached). */
    reads = disk_reads; w = fx4_window("CiukiDOS", 0);
    CHECK(!dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) && disk_reads - reads == blocks + 1);
    struct dosvm *v = dosvm_lookup((struct dosvm_key){w.id, w.generation});
    CHECK(v && !v->base_cached && !v->create_cancelled && !v->create_cancel_arg);
    shut = command_for(w.id, w.generation); CHECK(!finish_test_vm(&shut, sizeof(shut))); empty();

    /* 4. Repeated DOOM/Quake/CiukiDOS create/close cycles with the free
     * pages of the 128 MiB settled desktop (ledger replay: 99352576 bytes):
     * each is admitted, takes nothing from zone 1, returns every page. */
    const uint32_t settled = 99352576u / PAGE_SIZE;
    CHECK(pmm_free_count() > settled); withheld_pages = pmm_free_count() - settled;
    static const char *const names[] = {"DOOM", "Quake", "CiukiDOS"};
    static const unsigned games[] = {2, 3, 0};
    refusals = refusal_logs; unsigned created = 0;
    for (unsigned cycle = 0; cycle < 12; cycle++) {
        w = fx4_window(names[cycle % 3], games[cycle % 3]);
        CHECK(!dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) && w.state == CIUKI_DOSVM_BOOTING);
        v = dosvm_lookup((struct dosvm_key){w.id, w.generation});
        CHECK(v && v->game == games[cycle % 3] && v->kernel == 2 && v->frames[0] >= 0x1000000u);
        for (unsigned i = 0; i < DOSVM_PAGES; i++) CHECK(v->frames[i] >= 0x1000000u); /* above ISA DMA */
        CHECK(pmm_zone_free(ZONE_DMA) == 574);
        shut = command_for(w.id, w.generation); CHECK(!finish_test_vm(&shut, sizeof(shut)));
        empty(); CHECK(pmm_free_count() == settled); created++;
    }
    CHECK(created == 12 && refusal_logs == refusals);
    withheld_pages = 0; fake_dma_free_pages = 0;
    bundled_bytes = saved_bytes; disk_base_sector = saved_sector;
    puts("fx-4 VM creation: PASS trial EINVAL->EIO refusal named (stage/step/detail), not-installed, admission shortage with zone figures and boundary, allocation stage, owner-gone cold hash stopped (EINTR), 12 create/close cycles at 128 MiB settled free memory without leaks");
}

/* fx-4b: dosvm_disk.c with the background verifier's results. The verifier
 * itself (background hash, stamp, in-flight join, mismatch, removal hold)
 * runs over the production FAT/VFS in tests/host/dos/image_verify_test.c;
 * here its two calls are replaced by this fixture. Mode 0 (default for every
 * other case of this harness): no result, the launch hashes as before. */
static int image_mode;
static char image_sha[65];
static unsigned image_lookups, image_publishes;
static uint32_t image_published_size;
int dosvm_image_lookup(struct vfs_table *t, const struct vfs_file_version *version, char sha[65],
                       const char **source, bool (*cancelled)(void *), void *arg)
{
    (void)t; CHECK(version && sha && source); image_lookups++;
    if (image_mode == 2) { CHECK(cancelled && cancelled(arg)); return -EINTR; }
    if (!image_mode) return -ENOENT;
    memcpy(sha, image_sha, 65); *source = image_mode == 1 ? "background" : "stamp";
    return 0;
}
void dosvm_image_publish(struct vfs_table *t, const struct vfs_file_version *version, const char sha[65])
{ (void)t; CHECK(version && sha && strlen(sha) == 64); image_publishes++; image_published_size = version->size; }
static uint32_t image_state = CIUKI_DOSVM_IMAGE_VERIFYING, image_progress = 420;
void dosvm_image_query(uint32_t *state, uint32_t *progress, uint32_t *error)
{ *state = image_state; *progress = image_progress; *error = 0; }
static void test_fx4b_launch_paths(void)
{
    window_fixtures(); empty();
    uint32_t saved_bytes = bundled_bytes; void (*saved_sector)(uint32_t, void *) = disk_base_sector;
    bundled_bytes = 27676672u; disk_base_sector = 0;
    fake_current = &fake_proc[0];
    const unsigned blocks = (bundled_bytes + DOSVM_READ_SECTORS * 512u - 1) / (DOSVM_READ_SECTORS * 512u);
    /* IMAGE_QUERY: strict record, read-only, no VM capability needed. */
    fake_current = &fake_proc[2];
    struct ciuki_dosvm_command q = command_for(0, 0);
    CHECK(!dosvm_call(CIUKI_DOSVM_IMAGE_QUERY, &q, sizeof(q)) && q.state == CIUKI_DOSVM_IMAGE_VERIFYING && q.changed == 420 && !q.reason);
    q = command_for(0, 0); q.launch.game = 1; CHECK(dosvm_call(CIUKI_DOSVM_IMAGE_QUERY, &q, sizeof(q)) == -EINVAL);
    q = command_for(0, 0); q.version = 2; CHECK(dosvm_call(CIUKI_DOSVM_IMAGE_QUERY, &q, sizeof(q)) == -EOPNOTSUPP);
    q = command_for(0, 0); CHECK(dosvm_call(CIUKI_DOSVM_IMAGE_QUERY, &q, sizeof(q) - 1) == -EINVAL);
    fake_current = &fake_proc[0];
    /* No background result: the launch hashes and stamps its match. */
    dosvm_game_digest_invalidate(); image_mode = 0; image_publishes = image_lookups = 0;
    unsigned reads = disk_reads;
    struct ciuki_dosvm_command w = fx4_window("DOOM", 2);
    CHECK(!dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) && disk_reads - reads == blocks + 1);
    CHECK(image_lookups == 1 && image_publishes == 1 && image_published_size == bundled_bytes);
    CHECK(strstr(game_launch_record, " cached=0 source=hash"));
    struct dosvm *v = dosvm_lookup((struct dosvm_key){w.id, w.generation});
    memcpy(image_sha, v->base_sha256, 65);
    struct ciuki_dosvm_command shut = command_for(w.id, w.generation); CHECK(!finish_test_vm(&shut, sizeof(shut))); empty();
    /* The session cache: neither the verifier nor the hash. */
    reads = disk_reads; w = fx4_window("Quake", 3);
    CHECK(!dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) && disk_reads - reads == 1 && image_lookups == 1);
    CHECK(strstr(game_launch_record, " cached=1 source=session"));
    shut = command_for(w.id, w.generation); CHECK(!finish_test_vm(&shut, sizeof(shut))); empty();
    /* The in-flight/background result (or a stamp): no base block read. */
    for (int mode = 1; mode <= 3; mode += 2) {
        dosvm_game_digest_invalidate(); image_mode = mode; reads = disk_reads;
        w = fx4_window("DOOM", 2);
        CHECK(!dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) && disk_reads - reads == 1);
        CHECK(strstr(game_launch_record, mode == 1 ? " cached=1 source=background" : " cached=1 source=stamp"));
        CHECK(image_publishes == 1);                    /* reused results are not re-stamped */
        shut = command_for(w.id, w.generation); CHECK(!finish_test_vm(&shut, sizeof(shut))); empty();
    }
    /* The owner ends while the launch waits for the background check. */
    dosvm_game_digest_invalidate(); image_mode = 2; fake_proc[0].state = PROC_STOPPING;
    unsigned refusals = refusal_logs; w = fx4_window("DOOM", 2);
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) == -EINTR && refusal_logs == refusals + 1);
    fake_proc[0].state = PROC_LIVE;
    CHECK(strstr(last_refusal_log, "stage=disk error=-4 game=2 reason=owner-gone step=base-identity"));
    CHECK(strstr(last_launch_log, "error=-4 stage=vm-create window=DOOM")); empty();
    /* Mismatch: the verifier's digest names other bytes than UNIFIED.SHA.
     * The launch refuses honestly at the sidecar check. */
    dosvm_game_digest_invalidate(); image_mode = 1; image_sha[0] = image_sha[0] == '0' ? '1' : '0';
    w = fx4_window("DOOM", 2);
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &w, sizeof(w)) == -EIO && refusal_logs == refusals + 2);
    CHECK(strstr(last_refusal_log, "stage=disk error=-5 game=2 reason=io step=base-sidecar"));
    CHECK(strstr(last_launch_log, "error=-5 stage=vm-create window=DOOM") && image_publishes == 1);
    CHECK(fake_surface[0].references == 1 && !creating[0]); empty();
    image_mode = 0; dosvm_game_digest_invalidate();
    bundled_bytes = saved_bytes; disk_base_sector = saved_sector;
    puts("fx-4b launch paths: PASS IMAGE_QUERY record, hash+stamp when no result, session cache, background/stamp result without base reads, owner-gone wait EINTR, mismatch refused at base-sidecar");
}
