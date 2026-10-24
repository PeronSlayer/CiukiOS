/* SPDX-License-Identifier: MIT
 * fx-24: heap growth for the browser process.
 *
 * newlib's malloc (newlib/libc/stdlib/_mallocr.c: DEFAULT_TOP_PAD 0,
 * DEFAULT_TRIM_THRESHOLD 128 KiB, no mmap) extends the heap with sbrk by the
 * request rounded up to a 4 KiB page and trims its top whenever more than
 * 128 KiB is free there. On CiukiOS every extension is two BRK system calls
 * (sdk/libciuki/posix.c) and a new address-space extent with its own memory
 * object (src/kernel/proc/uaddr.c: ua_brk -> map_area); the kernel's extent
 * list is walked linearly by every page fault, mapping change and wait-word
 * call (ua_find, vacant, pinned). One encyclopedia article grows NetSurf's
 * heap by tens of megabytes in small steps, i.e. thousands of extents. A
 * 1 MiB top pad and a 4 MiB trim threshold (SVID mallopt, honoured by
 * newlib's mallocr) turn that into a few dozen extensions, with at most
 * 1 MiB of untouched pad and 4 MiB of free top kept between pages. */
#include <malloc.h>
#ifndef CIUKI_HOST
#define CIUKI_HEAP_TOP_PAD (1024 * 1024)
#define CIUKI_HEAP_TRIM (4 * 1024 * 1024)
__attribute__((constructor)) static void ciuki_netsurf_heap(void)
{
    mallopt(M_TOP_PAD, CIUKI_HEAP_TOP_PAD);
    mallopt(M_TRIM_THRESHOLD, CIUKI_HEAP_TRIM);
}
#else
/* Host builds use the host C library's own allocator policy. */
int ciuki_netsurf_heap_host;
#endif
