/* Small-heap geometry shared by production and host substitutes.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_KHEAP_LIMITS_H
#define CIUKI_KHEAP_LIMITS_H
#define KHEAP_CLASS_COUNT 7u
#define KHEAP_CLASS_MIN 32u
#define KHEAP_HEADER_BYTES 8u
#define KHEAP_CLASS_MAX (KHEAP_CLASS_MIN << (KHEAP_CLASS_COUNT - 1))
#define KHEAP_MAX_REQUEST (KHEAP_CLASS_MAX - KHEAP_HEADER_BYTES)
#endif
