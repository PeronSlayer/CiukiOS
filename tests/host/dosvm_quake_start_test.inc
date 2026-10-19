/* F3-25: real bundled FAT16 coordinates and Quake 1.06 MZ header; sparse
 * sector transport, production disk/BIOS and assembled DOS validator.
 * No image construction or guest boot. SPDX-License-Identifier: GPL-2.0-only */
#define QSTART_SECTORS 52656u
#define QSTART_EXE_LBA 48089u
static const uint8_t quake_mz_header[] = {
    0x4D,0x5A,0x00,0x00,0x04,0x00,0x00,0x00,0x20,0x00,0x28,0x00,
    0xFF,0xFF,0x00,0x00,0x70,0x07,0x00,0x00,0x54,0x00,0x00,0x00,
    0x0D,0x0A,0x73,0x74
};
static void quake_base_sector(uint32_t lba, void *data)
{
    uint8_t *b = data;
    /* Distinguish every LBA, including sectors 128 apart. */
    for (unsigned i = 0; i < 512; i++) b[i] = (uint8_t)((lba >> ((i % 4) * 8)) ^ i);
    if (!lba) {
        memset(b, 0, 512); w16(b + 11, 512); b[13] = 8;
        w16(b + 14, 73); b[16] = 2; w16(b + 17, 512);
        w16(b + 22, 128); w32(b + 32, DOSVM_SECTORS); w16(b + 510, 0xAA55);
    } else if (lba == 329 || lba == 377) {
        memset(b, 0, 512);
        memcpy(b, lba == 329 ? "QUAKE      " : "QUAKE   EXE", 11);
        b[11] = lba == 329 ? 0x10 : 0x20;
        w16(b + 26, lba == 329 ? 4 : 5968);
        if (lba == 377) w32(b + 28, 404480);
    } else if (lba == 96 || lba == 224) {
        /* Both FAT copies: the existing image's 99 contiguous EXE clusters. */
        memset(b, 0, 512);
        for (unsigned c = 5968; c <= 6066; c++) w16(b + (c % 256) * 2, c == 6066 ? 0xFFFF : c + 1);
    } else if (lba == QSTART_EXE_LBA) memcpy(b, quake_mz_header, sizeof(quake_mz_header));
}
static void quake_sector_check(const void *data, uint32_t lba)
{
    uint8_t expected[512]; quake_base_sector(lba, expected);
    CHECK(!memcmp(data, expected, sizeof(expected)));
}
#ifdef CIUKI_DOSVM_GUEST_CPU
#include "dosvm_mz_reloc.inc"
static bool quake_mz_accepts(struct dosvm *v, const uint8_t *header)
{
    /* ES:0 is the same DOS_IO_BUF_SEG used by the real DOS EXEC path. */
    CHECK(!dosvm_write(v, 0x10000, mz_reloc_code, sizeof(mz_reloc_code)));
    CHECK(!dosvm_write(v, 0x16000, header, 512));
    const struct gcpu_ops ops = {if_cpu_read, if_cpu_write, if_cpu_descriptor, if_cpu_unexpected, 0};
    if_cpu_vm = v; gcpu_init(&ops);
    struct gcpu_state s = {.cs = 0x1000, .es = 0x1600, .ss = 0x2000, .esp = 0xFFFE, .eflags = 0x203};
    struct gcpu_event ev;
    /* V86 INT3 traps before execution; only the wrapper's terminal INT3
     * at offset 3 may stop the validator. Its returned CF is untouched. */
    CHECK(gcpu_run(&s, 100, &ev) == GCPU_TRAP && ev.vector == 13 && ev.fault && s.eip == 3);
    gcpu_done(); if_cpu_vm = 0;
    return !(s.eflags & V86_CF);
}
#endif
static void test_quake_start(void)
{
    uint32_t saved_bytes = bundled_bytes;
    bundled_bytes = QSTART_SECTORS * 512u; disk_base_sector = quake_base_sector;
    struct dosvm_config config = {.base = "C:\\GAMES\\CIUKIGAM.IMG"};
    struct dosvm *v;
    CHECK(!dosvm_create(&config, &v) && v->base_sectors == QSTART_SECTORS);
    disk_base_sector = 0;
    uint8_t sector[512], expected[512], header[512];
    CHECK(!dosvm_disk_read(v, 0, sector) && u16(sector + 14) == 73 && sector[13] == 8);
    CHECK(!dosvm_disk_read(v, 329, sector) && !memcmp(sector, "QUAKE      ", 11) && u16(sector + 26) == 4);
    CHECK(!dosvm_disk_read(v, 361 + (4 - 2) * 8, sector) && !memcmp(sector, "QUAKE   EXE", 11));
    CHECK(u16(sector + 26) == 5968 && u32(sector + 28) == 404480);
    CHECK(!dosvm_disk_read(v, QSTART_EXE_LBA, header)); quake_sector_check(header, QSTART_EXE_LBA);
    /* Walk the actual file extent while alternating its data and FAT window,
     * as the probe does before EXEC. Check every returned byte. */
    uint32_t left = 404480;
    unsigned cluster = 5968, clusters = 0;
    while (left) {
        CHECK(cluster >= 5968 && cluster <= 6066); clusters++;
        for (unsigned s = 0; s < 8 && left; s++) {
            uint32_t lba = 361 + (cluster - 2) * 8 + s;
            CHECK(!dosvm_disk_read(v, lba, sector)); quake_sector_check(sector, lba);
            left -= left < 512 ? left : 512;
        }
        CHECK(!dosvm_disk_read(v, 73 + cluster / 256, sector));
        cluster = u16(sector + (cluster % 256) * 2);
    }
    CHECK(clusters == 99 && cluster == 0xFFFF);
    /* BIOS multi-sector request straddling the 64 KiB base-cache boundary. */
    uint8_t dap[16] = {16}, run[3 * 512];
    w16(dap + 2, 3); w16(dap + 6, 0x3000); w32(dap + 8, 48127);
    CHECK(!dosvm_write(v, 0x20000, dap, sizeof(dap)));
    struct v86_frame f = disk_frame(0x4200);
    CHECK(!dosvm_bios(v, &f, 0x13) && !(f.tf.eflags & V86_CF));
    CHECK(!dosvm_read(v, 0x20000, dap, sizeof(dap)) && u16(dap + 2) == 3);
    CHECK(!dosvm_read(v, 0x30000, run, sizeof(run)));
    for (unsigned i = 0; i < 3; i++) quake_sector_check(run + i * 512, 48127 + i);
    /* A new/rewritten COW sector inside a warm base window always wins. */
    memset(expected, 0xA5, sizeof(expected));
    CHECK(!dosvm_disk_write(v, 48128, expected) && !dosvm_disk_read(v, 48128, sector) && !memcmp(sector, expected, 512));
    memset(expected, 0x5A, sizeof(expected));
    CHECK(!dosvm_disk_write(v, 48128, expected) && !dosvm_disk_read(v, 48128, sector) && !memcmp(sector, expected, 512));
    CHECK(!dosvm_disk_read(v, 48129, sector)); quake_sector_check(sector, 48129);
    /* The real cut-image end is 48 sectors into its final cache window. */
    CHECK(!dosvm_disk_read(v, QSTART_SECTORS - 1, sector)); quake_sector_check(sector, QSTART_SECTORS - 1);
    CHECK(v->read_cache[v->read_recent].count == DOSVM_READ_SECTORS);
    unsigned reads = disk_reads;
    memset(expected, 0, sizeof(expected));
    CHECK(!dosvm_disk_read(v, QSTART_SECTORS, sector) && !memcmp(sector, expected, 512) && disk_reads == reads);
    memset(expected, 0xC3, sizeof(expected));
    CHECK(!dosvm_disk_write(v, QSTART_SECTORS + 1, expected) && !dosvm_disk_read(v, QSTART_SECTORS + 1, sector) && !memcmp(sector, expected, 512));
    /* Per-image cache isolation, then reuse the same slot with another base. */
    struct dosvm *other = create();
    CHECK(!dosvm_disk_read(other, 329, sector) && sector[0] == 329 % 251);
    CHECK(!dosvm_disk_read(v, 329, sector)); quake_sector_check(sector, 329);
    destroy(other, true);
    struct dosvm_key old = dosvm_key(v);
    destroy(v, true); empty();
    v = create();
    CHECK(v->info.key.id == old.id && v->info.key.generation != old.generation);
    CHECK(!dosvm_disk_read(v, QSTART_EXE_LBA, sector) && sector[0] == QSTART_EXE_LBA % 251);
    puts("f3-25 disk: FAT16 root=329 data=361 exe=48089 clusters=99; BIOS window crossing, COW overwrite, EOF=52656, image/generation isolation PASS");
#ifdef CIUKI_DOSVM_GUEST_CPU
    bool accepted = quake_mz_accepts(v, header);
    printf("f3-25 EXEC: Quake header relocations=%u offset=%04x header_bytes=%u accepted=%u\n",
           u16(header + 6), u16(header + 24), u16(header + 8) * 16, accepted);
    CHECK(accepted);
    w16(header + 24, 0xFFFF); CHECK(quake_mz_accepts(v, header));
    w16(header + 8, 1); CHECK(!quake_mz_accepts(v, header));
    w16(header + 8, 0x1000); CHECK(!quake_mz_accepts(v, header));
    w16(header + 8, 32); w16(header + 6, 1); w16(header + 24, 508); CHECK(quake_mz_accepts(v, header));
    w16(header + 24, 509); CHECK(!quake_mz_accepts(v, header));
    w16(header + 24, 0xFFFE); CHECK(!quake_mz_accepts(v, header));
    w16(header + 6, 0x4000); w16(header + 24, 28); CHECK(!quake_mz_accepts(v, header));
    puts("f3-25 EXEC: production 16-bit validator accepts empty relocation tables; malformed headers/nonempty tables still rejected PASS");
#else
    puts("f3-25 EXEC: SKIP (no libx86emu headers)");
#endif
    destroy(v, true); empty(); bundled_bytes = saved_bytes;
}
