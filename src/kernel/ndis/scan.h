/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_SCAN_H
#define CIUKI_NDIS_SCAN_H
#include "ntimage.h"
struct ndis_scan_error { uint32_t rva; const char *reason; };
/* 32-bit legacy instruction lengths; unknown/optional ISA fails closed.
 * This is a linear admission scan, not proof of safe control flow. */
int ndis_instruction(const uint8_t *, uint32_t, uint32_t *, const char **);
/* Length-only decoding for flag patch preparation. Never an admission proof. */
int ndis_instruction_length(const uint8_t *, uint32_t, uint32_t *, const char **);
int ndis_scan_image(const struct pe_image *, struct ndis_scan_error *);
#endif
