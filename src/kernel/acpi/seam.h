/* Included after ACPICA's public header. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ACPI_SEAM_H
#define CIUKI_ACPI_SEAM_H
#include "acpi.h"
/* Upstream's default casts via subtraction from NULL. A direct integer
 * conversion preserves the address without undefined pointer arithmetic. */
#undef ACPI_TO_INTEGER
#define ACPI_TO_INTEGER(p) ((ACPI_SIZE)(p))
#undef ACPI_OFFSET
#define ACPI_OFFSET(type, field) __builtin_offsetof(type, field)
static inline UINT32 AcpiCiukiName32(const char *p)
{
    UINT32 value;
    __builtin_memcpy(&value, p, sizeof(value));
    return value;
}
#undef ACPI_COMPARE_NAMESEG
#define ACPI_COMPARE_NAMESEG(a, b) (AcpiCiukiName32((const char *)(a)) == AcpiCiukiName32((const char *)(b)))
ACPI_STATUS AcpiCiukiBudgetStep(void);
ACPI_STATUS AcpiCiukiParseResult(ACPI_STATUS status);
#endif
