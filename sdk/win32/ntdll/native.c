/* NT native services reached by the reused Wine 10.0 upper DLLs (f5-20A).
 * Original implementation over the CiukiOS copied-record transport; the
 * semantics follow the Microsoft NT DDK/Win32 documentation of each call.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include <w32/ldr.h>
#include <ciuki/w32_core.h>
#define STATUS_INVALID_INFO_CLASS ((NTSTATUS)0xc0000003)
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xc0000004)
#define STATUS_ACCESS_VIOLATION ((NTSTATUS)0xc0000005)
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xc0000008)
#define STATUS_INVALID_PARAMETER_3 ((NTSTATUS)0xc00000f1)
#define STATUS_INVALID_PARAMETER_4 ((NTSTATUS)0xc00000f2)
#define W32_PTR(p) ((ULONG)(uintptr_t)(p))
uint64_t w32_divmod64(uint64_t, uint64_t, uint32_t);
int w32_nt_key_close(void *);
/* Transport result: negative errno-style codes from the copied-record gate,
 * otherwise an NTSTATUS. */
static NTSTATUS transport(int32_t s) {
    if (s < 0 && s >= -4095)
        return s == -12 ? STATUS_NO_MEMORY : s == -14 ? STATUS_ACCESS_VIOLATION
                                                       : STATUS_INVALID_PARAMETER;
    return s;
}
NTSTATUS w32_status_from_error(ULONG e) {
    switch (e) {
    case 0: return 0;
    case 2: return (NTSTATUS)0xc0000034;
    case 3: return (NTSTATUS)0xc000003a;
    case 5: return (NTSTATUS)0xc0000022;
    case 6: return STATUS_INVALID_HANDLE;
    case 8: case 14: return STATUS_NO_MEMORY;
    case 87: return STATUS_INVALID_PARAMETER;
    case 120: return STATUS_NOT_IMPLEMENTED;
    case 122: return STATUS_BUFFER_TOO_SMALL;
    case 1018: return (NTSTATUS)0xc000017c;
    default: return (NTSTATUS)0xc0000001;
    }
}
/* MEM_COMMIT/MEM_RESERVE in the calling process only; the kernel rounds the
 * region, so the returned base and size describe the pages it covers. */
NTSTATUS W32API NtAllocateVirtualMemory(void *process, void **base, ULONG zero_bits,
                                        ULONG *size, ULONG type, ULONG protect) {
    if (process != (void *)(intptr_t)-1)
        return STATUS_INVALID_HANDLE;
    if (!base || !size)
        return STATUS_ACCESS_VIOLATION;
    if (zero_bits)
        return STATUS_INVALID_PARAMETER_3;
    if (!*size)
        return STATUS_INVALID_PARAMETER_4;
    ULONG wanted = W32_PTR(*base), bytes = *size;
    struct ciuki_w32_vm a = {.header = {1, sizeof(a), W32_VM_ALLOC, 0},
                             .address = wanted, .bytes = bytes, .flags = type,
                             .protect = protect};
    NTSTATUS s = transport(w32_ciuki_call(&a, sizeof(a)));
    if (s)
        return s;
    uint64_t end = wanted ? (uint64_t)wanted + bytes : (uint64_t)a.address + bytes;
    end = (end + 4095) & ~(uint64_t)4095;
    *base = (void *)(uintptr_t)a.address;
    *size = (ULONG)(end - a.address);
    return 0;
}
NTSTATUS W32API NtClose(void *handle) {
    if (w32_nt_key_close(handle))
        return 0;
    struct ciuki_w32_object_args a = {.header = {1, sizeof(a), W32_OBJECT, 0},
                                      .action = W32_OBJ_CLOSE, .handle = W32_PTR(handle)};
    NTSTATUS s = transport(w32_ciuki_call(&a, sizeof(a)));
    return s ? s : w32_status_from_error(a.error);
}
NTSTATUS W32API NtQuerySystemTime(int64_t *out) {
    if (!out)
        return STATUS_ACCESS_VIOLATION;
    struct ciuki_w32_system_args a = {.header = {1, sizeof(a), W32_SYSTEM, 0}, .action = W32_S_TIME};
    NTSTATUS s = transport(w32_ciuki_call(&a, sizeof(a)));
    if (s)
        return s;
    *out = (int64_t)(((uint64_t)a.high << 32) | a.low);
    return 0;
}
/* 100 ns intervals from 1601-01-01 to 1970-01-01. */
#define EPOCH_1970 116444736000000000ull
BOOLEAN W32API RtlTimeToSecondsSince1970(const int64_t *time, ULONG *seconds) {
    if (!time || !seconds || *time < (int64_t)EPOCH_1970)
        return 0;
    uint64_t n = w32_divmod64((uint64_t)*time - EPOCH_1970, 10000000u, 2);
    if (n > 0xffffffffu)
        return 0;
    *seconds = (ULONG)n;
    return 1;
}
/* Days from 1601-01-01 to the first day of year y (Gregorian). */
static ULONG days_before(ULONG y) {
    ULONG n = y - 1601;
    return n * 365 + n / 4 - n / 100 + n / 400;
}
static int64_t dos_time(uint16_t date, uint16_t time, ULONG hundredths) {
    static const uint16_t before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (!date)
        return 0;
    ULONG year = 1980 + (date >> 9), month = (date >> 5) & 15, day = date & 31;
    if (month < 1 || month > 12 || !day)
        return 0;
    int leap = !(year % 4) && (year % 100 || !(year % 400));
    ULONG days = days_before(year) + before[month - 1] + (month > 2 && leap) + day - 1;
    uint64_t seconds = (uint64_t)days * 86400 + (time >> 11) * 3600u +
                       ((time >> 5) & 63) * 60u + (time & 31) * 2u;
    return (int64_t)(seconds * 10000000u + (uint64_t)hundredths * 100000u);
}
struct w32_io_status {
    NTSTATUS Status;
    ULONG Information;
};
static NTSTATUS file_record(struct ciuki_w32_file_args *a) {
    a->header = (struct ciuki_w32_header){1, sizeof(*a), W32_FILE, 0};
    NTSTATUS s = transport(w32_ciuki_call(a, sizeof(*a)));
    return s ? s : w32_status_from_error(a->error);
}
/* FileBasicInformation (4), FileStandardInformation (5) and
 * FilePositionInformation (14) over the open description; FAT keeps local
 * DOS timestamps (UTC in CiukiOS), the change time is the write time. */
NTSTATUS W32API NtQueryInformationFile(void *handle, struct w32_io_status *io, void *out,
                                       ULONG length, ULONG info_class) {
    ULONG needed = info_class == 4 ? 40 : info_class == 5 ? 24 : info_class == 14 ? 8 : 0;
    if (!needed)
        return STATUS_INVALID_INFO_CLASS;
    if (!io || !out)
        return STATUS_ACCESS_VIOLATION;
    if (length < needed)
        return STATUS_INFO_LENGTH_MISMATCH;
    uint8_t data[40];
    w32_fill(data, 0, sizeof(data));
    NTSTATUS s;
    if (info_class == 14) {
        struct ciuki_w32_file_args a = {.action = W32_F_SEEK, .handle = W32_PTR(handle), .flags = 1};
        s = file_record(&a);
        if (!s) {
            w32_put32(data, a.low);
            w32_put32(data + 4, a.high);
        }
    } else {
        struct ciuki_w32_file_info f;
        struct ciuki_w32_file_args a = {.action = W32_F_STAT, .handle = W32_PTR(handle),
                                        .buffer = W32_PTR(&f), .bytes = sizeof(f)};
        s = file_record(&a);
        if (!s && info_class == 4) {
            int64_t times[4] = {dos_time(f.create_date, f.create_time, f.create_tenths),
                                dos_time(f.access_date, 0, 0),
                                dos_time(f.write_date, f.write_time, 0),
                                dos_time(f.write_date, f.write_time, 0)};
            w32_copy(data, times, 32);
            w32_put32(data + 32, f.attributes);
        } else if (!s) {
            uint64_t size = ((uint64_t)f.size_high << 32) | f.size_low;
            uint64_t allocation = (size + 4095) & ~(uint64_t)4095;
            w32_copy(data, &allocation, 8);
            w32_copy(data + 8, &size, 8);
            w32_put32(data + 16, 1);
            data[21] = (f.attributes & 0x10) != 0;
        }
    }
    io->Status = s;
    io->Information = s ? 0 : needed;
    if (!s)
        w32_copy(out, data, needed);
    return s;
}
struct w32_bitmap {
    ULONG SizeOfBitMap;
    ULONG *Buffer;
};
void W32API RtlInitializeBitMap(struct w32_bitmap *map, ULONG *buffer, ULONG bits) {
    map->SizeOfBitMap = bits;
    map->Buffer = buffer;
}
static int range(const struct w32_bitmap *map, ULONG start, ULONG count) {
    return map && map->Buffer && count && start < map->SizeOfBitMap &&
           count <= map->SizeOfBitMap - start;
}
void W32API RtlSetBits(struct w32_bitmap *map, ULONG start, ULONG count) {
    if (!range(map, start, count))
        return;
    for (ULONG i = start; i < start + count; i++)
        map->Buffer[i / 32] |= 1u << (i % 32);
}
static BOOLEAN all(const struct w32_bitmap *map, ULONG start, ULONG count, int set) {
    if (!range(map, start, count))
        return 0;
    for (ULONG i = start; i < start + count; i++)
        if (!!(map->Buffer[i / 32] & (1u << (i % 32))) != set)
            return 0;
    return 1;
}
BOOLEAN W32API RtlAreBitsSet(const struct w32_bitmap *map, ULONG start, ULONG count) {
    return all(map, start, count, 1);
}
BOOLEAN W32API RtlAreBitsClear(const struct w32_bitmap *map, ULONG start, ULONG count) {
    return all(map, start, count, 0);
}
/* Park-Miller minimal standard generator (Schrage's method, 32-bit only):
 * results lie in [1, MAXLONG-1], inside the documented [0, MAXLONG-1]. */
ULONG W32API RtlRandom(ULONG *seed) {
    if (!seed)
        return 0;
    ULONG s = *seed % 0x7fffffffu;
    if (!s)
        s = 1;
    LONG t = (LONG)(16807u * (s % 127773u)) - (LONG)(2836u * (s / 127773u));
    if (t <= 0)
        t += 0x7fffffff;
    *seed = (ULONG)t;
    return (ULONG)t;
}
/* Mapped-file (not loaded) image: section table lookup by raw data size. */
void *W32API RtlImageRvaToVa(const void *nt, void *base, ULONG rva, const void **last) {
    if (!nt || !base)
        return 0;
    const uint8_t *headers = nt;
    ULONG count = w32_u16(headers + 6), optional = w32_u16(headers + 20);
    const uint8_t *table = headers + 24 + optional, *found = 0;
    if (last && *last) {
        const uint8_t *s = *last;
        ULONG va = w32_u32(s + 12), raw = w32_u32(s + 16);
        if (rva >= va && rva - va < raw)
            found = s;
    }
    for (ULONG i = 0; !found && i < count; i++) {
        const uint8_t *s = table + i * 40;
        ULONG va = w32_u32(s + 12), raw = w32_u32(s + 16);
        if (rva >= va && rva - va < raw)
            found = s;
    }
    if (!found)
        return 0;
    if (last)
        *last = found;
    return (uint8_t *)base + (rva - w32_u32(found + 12)) + w32_u32(found + 20);
}
