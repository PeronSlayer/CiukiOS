/* Ciuki ACPICA platform, including host fixtures. No Linux OSL.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ACPI_ENV_H
#define CIUKI_ACPI_ENV_H
#ifdef CIUKI_ACPI_HOST
#define ACPI_MACHINE_WIDTH (__SIZEOF_POINTER__ * 8)
#else
#define ACPI_MACHINE_WIDTH 32
#endif
#define ACPI_USE_LOCAL_CACHE
#define USE_NATIVE_ALLOCATE_ZEROED
#define ACPI_MUTEX_TYPE ACPI_OSL_MUTEX
#define ACPI_USE_NATIVE_DIVIDE
#define ACPI_USE_NATIVE_MATH64
/* AML names/table fields are byte-aligned. Use ACPICA's portable moves
 * instead of x86-tolerated, undefined C typed loads. */
#define ACPI_MISALIGNMENT_NOT_SUPPORTED
/* No foreign-OS/compiler extended assembly glue is in the audited set.
 * Ciuki uses the portable native arithmetic selected above. */
#define __ACENVEX_H__
/* Firmware's global lock is never acquired/written by inventory. */
#define ACPI_ACQUIRE_GLOBAL_LOCK(p, a) do { (void)(p); (a) = 0; } while (0)
#define ACPI_RELEASE_GLOBAL_LOCK(p, a) do { (void)(p); (a) = 0; } while (0)
#endif
