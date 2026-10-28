/* Internal allocator limits shared with host substitutes.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MEMORY_LIMITS_H
#define CIUKI_MEMORY_LIMITS_H
#define PAGE_SIZE 4096u
#define PMM_DMA_START_PFN 256u
#define PMM_NORMAL_START_PFN 4096u
#define PMM_MAX_PAGES (768u * 256u) /* direct map limit: 768 MiB */
#endif
