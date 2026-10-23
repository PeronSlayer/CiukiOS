/* Private installed-package format, shared by the importer and NDIS gate.
 * Never an enable/qualification authority. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_PACKAGE_H
#define CIUKI_NDIS_PACKAGE_H
#include <stdint.h>
#include <stddef.h>
#define NDP_VERSION 1u
#define NDP_FILES 32u
#define NDP_PARAMS 96u
#define NDP_DATA 256u
#define NDP_ROOT "/System/drivers"
#define NDP_DISABLED "No lead qualification record; installed disabled."
#define NDP_QUALIFIED "Lead qualified; native driver remains the default."
struct ndp_device {
    uint32_t bus, slot, function, vendor, device, sub_vendor, sub_device, revision;
};
struct ndp_file {
    char name[64];
    uint32_t bytes;
    uint8_t hash[32];
};
enum ndp_type { NDP_STRING = 1, NDP_BINARY = 3, NDP_DWORD = 4 };
struct ndp_parameter {
    char key[128], name[64];
    uint32_t type, bytes;
    uint8_t data[NDP_DATA];
};
struct ndp_manifest {
    uint32_t version, size, enabled, file_count, parameter_count, binary;
    struct ndp_device device;
    char package[64], inf[64], models[96], install[96], services[96];
    char service[64], hardware_id[96], name[128], provider[128], driver_version[96];
    char reason[96];
    struct ndp_file files[NDP_FILES];
    /* Relative HKR views: adapter/ and service/. */
    struct ndp_parameter parameters[NDP_PARAMS];
    uint8_t seal[32];
};
_Static_assert(sizeof(struct ndp_device)==32,"package device layout");
_Static_assert(sizeof(struct ndp_file)==100,"package file layout");
_Static_assert(sizeof(struct ndp_parameter)==456,"package parameter layout");
_Static_assert(offsetof(struct ndp_manifest,seal)==48056,"package seal offset");
_Static_assert(sizeof(struct ndp_manifest)==48088,"package manifest layout");
int ndp_component(const char *);
int ndp_relative(const char *);
int ndp_manifest_valid(const struct ndp_manifest *);
void ndp_seal(struct ndp_manifest *);
/* Reader is rooted in the installed package, not in the source media. */
typedef int (*ndp_reader)(void *, const char *, uint32_t, void *, size_t, size_t *);
int ndp_verify(const struct ndp_manifest *, const struct ndp_device *, ndp_reader, void *);
struct pci_func;
int ndis_package_admit(const char *path, const uint8_t hash[32], const struct pci_func *, const uint8_t qualified_seal[32]);
#endif
