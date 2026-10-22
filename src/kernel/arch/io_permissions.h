/* Complete TSS permission installation, including the trailing FF byte.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_IO_PERMISSIONS_H
#define CIUKI_IO_PERMISSIONS_H
#include <stdint.h>
#include <stdbool.h>
#define ARCH_IO_BYTES 8193u
struct arch_io_range { uint32_t first, end; };
/* Caller excludes interrupts throughout installation. Validate before granting
 * anything; malformed allowlists leave the entire bitmap denied. */
static inline bool arch_io_bitmap(uint8_t map[ARCH_IO_BYTES],
                                  const struct arch_io_range *ranges, unsigned count)
{
    for (unsigned i=0;i<ARCH_IO_BYTES;i++) map[i]=0xff;
    if (count>6 || (count && !ranges)) return false;
    for (unsigned i=0;i<count;i++) {
        if (ranges[i].first>=ranges[i].end || ranges[i].end>65536) return false;
        for(unsigned j=0;j<i;j++) if(ranges[i].first<ranges[j].end &&
            ranges[j].first<ranges[i].end) return false;
    }
    for (unsigned i=0;i<count;i++) {
        uint32_t p=ranges[i].first;
        while(p<ranges[i].end) {
            unsigned bit=p%8, bits=8-bit;
            if(bits>ranges[i].end-p) bits=ranges[i].end-p;
            map[p/8]&=(uint8_t)~(((1u<<bits)-1u)<<bit);
            p+=bits;
        }
    }
    return true;
}
#endif
