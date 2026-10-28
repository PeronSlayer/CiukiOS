/* SPDX-License-Identifier: MIT
 * Private provisioning constants shared by Setup (apps/setup), libsetup's
 * configuration step, the image builder (scripts/build_image.py mirrors these
 * values in Python) and the ACCOUNT first-boot adapter (sec-user-02). Never
 * installed in the public SDK sysroot.
 *
 * Binding layouts: docs/design/user-security-contract.md section 3.6
 * (CIUKISEE seed: section 3.3 encoding, 96 + 3 * 256 bytes; CIUKIBST
 * bootstrap.state: exactly 160 bytes). The seed itself is encoded with the
 * account service's own codec (apps/accounts/codec.c, acct_encode seed=1);
 * this header only produces the initial UNPROVISIONED state record, which
 * Builder/Setup write because ACCOUNT never invents initial evidence.
 */
#ifndef CIUKI_PRIVATE_PROVISION_H
#define CIUKI_PRIVATE_PROVISION_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Destination paths (system volume, native spelling; FAT is case-insensitive). */
#define CIUKI_PROVISION_DIR "System/Security"
#define CIUKI_PROVISION_SEED "System/Security/provision.seed"
#define CIUKI_PROVISION_STATE "System/Security/bootstrap.state"
#define CIUKI_PROVISION_PROFILES "Windows/Profiles"
/* Seed geometry and origins (section 3.6). */
#define CIUKI_PROVISION_SEED_BYTES (96u + 3u * 256u)
#define CIUKI_PROVISION_STATE_BYTES 160u
#define CIUKI_PROVISION_READY 1u
#define CIUKI_PROVISION_SETUP 2u
#define CIUKI_PROVISION_ENROLLMENT 3u

/* Managed-volume identity used for bootstrap.state destination_volume_id and
 * the OWNER/ACCOUNT store key: the FAT32 partition's first LBA in the high
 * half and the BPB volume serial (offset 67 of the boot sector) in the low
 * half. Both are fixed for a formatted volume and known to the builder
 * (mkfs.fat volume id at partition LBA 2048), to libsetup (libdisk's fresh
 * entropy volume id and planned partition start) and to the kernel (mounted
 * fat_volume.volume_serial and partition start). Zero is never valid. */
static inline uint64_t ciuki_provision_volume_id(uint64_t partition_start, uint32_t volume_serial)
{
    if (!partition_start || partition_start > UINT32_MAX || !volume_serial) return 0;
    return partition_start << 32 | volume_serial;
}

static inline void ciuki_provision_put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline void ciuki_provision_put64(uint8_t *p, uint64_t v)
{ ciuki_provision_put32(p, (uint32_t)v); ciuki_provision_put32(p + 4, (uint32_t)(v >> 32)); }
static inline uint32_t ciuki_provision_get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* The initial CIUKIBST record (state UNPROVISIONED=1, state_generation 1,
 * next_uid 1001, flags/SID/generations zero) for origin 1 (ready) or 2
 * (Setup) seeds and origin 3 (G32 enrollment source). digest() is the
 * caller's SHA-256 implementation; seed_digest is SHA-256 of the exact seed
 * (or enrollment source) bytes. Returns 0, or -1 for invalid input. */
static inline int ciuki_provision_state(uint8_t out[CIUKI_PROVISION_STATE_BYTES], uint32_t origin, uint64_t volume,
                                        const uint8_t seed_digest[32],
                                        void (*digest)(const void *, size_t, uint8_t[32]))
{
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 32; i++) nonzero |= seed_digest[i];
    if (!out || !digest || !volume || !nonzero || origin < CIUKI_PROVISION_READY || origin > CIUKI_PROVISION_ENROLLMENT)
        return -1;
    memset(out, 0, CIUKI_PROVISION_STATE_BYTES);
    memcpy(out, "CIUKIBST", 8);
    ciuki_provision_put32(out + 8, 1);
    ciuki_provision_put32(out + 12, CIUKI_PROVISION_STATE_BYTES);
    ciuki_provision_put32(out + 16, 1);            /* UNPROVISIONED */
    ciuki_provision_put32(out + 20, origin);
    ciuki_provision_put64(out + 24, volume);
    memcpy(out + 32, seed_digest, 32);
    ciuki_provision_put32(out + 104, 1001);         /* next_uid watermark */
    ciuki_provision_put64(out + 112, 1);            /* state_generation */
    digest(out, CIUKI_PROVISION_STATE_BYTES, out + 120); /* digest bytes are zero while hashing */
    return 0;
}

/* Header checks a writer performs before staging a seed it did not encode
 * (libsetup receives Setup's seed as opaque bytes). The account codec still
 * validates every field at promotion. */
static inline int ciuki_provision_seed_header(const uint8_t *seed, size_t bytes, uint32_t origin)
{
    if (!seed || bytes != CIUKI_PROVISION_SEED_BYTES || memcmp(seed, "CIUKISEE", 8) ||
        ciuki_provision_get32(seed + 8) != 2 || ciuki_provision_get32(seed + 12) != 96 ||
        ciuki_provision_get32(seed + 24) != 1001 || ciuki_provision_get32(seed + 28) != 3 ||
        ciuki_provision_get32(seed + 36) != 256 || ciuki_provision_get32(seed + 52) != origin ||
        ciuki_provision_get32(seed + 56) != (origin == CIUKI_PROVISION_READY ? 1000u : 0u)) return -1;
    return 0;
}
#endif
