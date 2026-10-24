/* G1: the DOS VM's view of the real-time clock. A DOS VM sees the system
 * LOCAL time (Windows 98 DOS sessions run on the machine's local clock, the
 * RTC of a Windows-configured PC holds local time) plus a per-VM offset
 * that DOS TIME/DATE (INT 21h 2Bh/2Dh, which the CiukiDOS CLOCK$ driver
 * forwards to INT 1Ah 03h/05h) changes inside that VM only.
 * INT 1Ah register images: IBM PC AT BIOS Technical Reference, INT 1Ah
 * AH=02h..05h (BCD CH/CL/DH/DL); RBIL 61 INT 1A/AH=02h..05h,
 * https://fd.lod.bz/rbil/interrup/bios/1a02.html. MC146818 registers 0..9,
 * 0Ah..0Dh and the century byte 32h for direct CMOS reads.
 * Pure, header-only: no kernel or library calls.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_RTC_H
#define CIUKI_DOSVM_RTC_H
#include <stdbool.h>
#include <stdint.h>

#define DOSVM_RTC_TICKS_PER_DAY 0x1800B0u

static inline int64_t dosvm_rtc_floor(int64_t a, int64_t b)
{ int64_t q = a / b; return (a % b) && ((a < 0) != (b < 0)) ? q - 1 : q; }
static inline int64_t dosvm_rtc_days(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = dosvm_rtc_floor(y, 400), yoe = y - era * 400;
    int64_t doy = (153 * (int64_t)(m > 2 ? m - 3 : m + 9) + 2) / 5 + (int64_t)d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
static inline void dosvm_rtc_civil(int64_t z, unsigned *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = dosvm_rtc_floor(z, 146097), doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    *m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    *y = (unsigned)(yoe + era * 400 + (*m <= 2));
}
static inline uint8_t dosvm_rtc_bcd(unsigned v) { return (uint8_t)((v / 10) * 16 + v % 10); }
static inline bool dosvm_rtc_unbcd(uint8_t b, unsigned max, unsigned *out)
{
    if ((b & 15) > 9 || (b >> 4) > 9) return false;
    *out = (b >> 4) * 10u + (b & 15u);
    return *out <= max;
}
/* AH=02h: CH hours, CL minutes, DH seconds (BCD), DL 0 (no DST option). */
static inline void dosvm_rtc_time(int64_t local, uint16_t *cx, uint16_t *dx)
{
    int64_t s = local - dosvm_rtc_floor(local, 86400) * 86400;
    *cx = (uint16_t)(dosvm_rtc_bcd((unsigned)(s / 3600)) << 8 | dosvm_rtc_bcd((unsigned)(s / 60 % 60)));
    *dx = (uint16_t)(dosvm_rtc_bcd((unsigned)(s % 60)) << 8);
}
/* AH=04h: CH century, CL year, DH month, DL day (BCD). */
static inline void dosvm_rtc_date(int64_t local, uint16_t *cx, uint16_t *dx)
{
    unsigned y, m, d;
    dosvm_rtc_civil(dosvm_rtc_floor(local, 86400), &y, &m, &d);
    *cx = (uint16_t)(dosvm_rtc_bcd(y / 100 % 100) << 8 | dosvm_rtc_bcd(y % 100));
    *dx = (uint16_t)(dosvm_rtc_bcd(m) << 8 | dosvm_rtc_bcd(d));
}
/* AH=03h: the same day at CH:CL:DH. False on a bad BCD value. */
static inline bool dosvm_rtc_set_time(int64_t local, uint16_t cx, uint16_t dx, int64_t *out)
{
    unsigned h, m, s;
    if (!dosvm_rtc_unbcd((uint8_t)(cx >> 8), 23, &h) || !dosvm_rtc_unbcd((uint8_t)cx, 59, &m) ||
        !dosvm_rtc_unbcd((uint8_t)(dx >> 8), 59, &s)) return false;
    *out = dosvm_rtc_floor(local, 86400) * 86400 + h * 3600 + m * 60 + s;
    return true;
}
/* AH=05h: CX:DX date, the same time of day. Years 1980..2099. */
static inline bool dosvm_rtc_set_date(int64_t local, uint16_t cx, uint16_t dx, int64_t *out)
{
    unsigned c, y, m, d;
    if (!dosvm_rtc_unbcd((uint8_t)(cx >> 8), 99, &c) || !dosvm_rtc_unbcd((uint8_t)cx, 99, &y) ||
        !dosvm_rtc_unbcd((uint8_t)(dx >> 8), 12, &m) || !dosvm_rtc_unbcd((uint8_t)dx, 31, &d) || !m || !d) return false;
    unsigned year = c * 100 + y;
    static const uint8_t md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned limit = md[m - 1] + (m == 2 && !(year % 4) && (year % 100 || !(year % 400)));
    if (year < 1980 || year > 2099 || d > limit) return false;
    int64_t tod = local - dosvm_rtc_floor(local, 86400) * 86400;
    *out = dosvm_rtc_days(year, m, d) * 86400 + tod;
    return true;
}
/* BIOS tick count since midnight (0040:006Ch) for a local time. */
static inline uint32_t dosvm_rtc_ticks(int64_t local)
{
    int64_t s = local - dosvm_rtc_floor(local, 86400) * 86400;
    return (uint32_t)((uint64_t)s * DOSVM_RTC_TICKS_PER_DAY / 86400u);
}
/* Direct CMOS read of a clock register: BCD, 24-hour, no update in
 * progress, valid RAM. False for registers this model does not own. */
static inline bool dosvm_rtc_cmos(int64_t local, uint8_t index, uint8_t *value)
{
    unsigned y, m, d;
    int64_t days = dosvm_rtc_floor(local, 86400), s = local - days * 86400;
    dosvm_rtc_civil(days, &y, &m, &d);
    int64_t w = (days + 4) % 7;
    switch (index & 0x7F) {
    case 0x00: *value = dosvm_rtc_bcd((unsigned)(s % 60)); return true;
    case 0x02: *value = dosvm_rtc_bcd((unsigned)(s / 60 % 60)); return true;
    case 0x04: *value = dosvm_rtc_bcd((unsigned)(s / 3600)); return true;
    case 0x06: *value = (uint8_t)((w < 0 ? w + 7 : w) + 1); return true;
    case 0x07: *value = dosvm_rtc_bcd(d); return true;
    case 0x08: *value = dosvm_rtc_bcd(m); return true;
    case 0x09: *value = dosvm_rtc_bcd(y % 100); return true;
    case 0x0A: *value = 0x26; return true;   /* 32.768 kHz, 1024 Hz, UIP clear */
    case 0x0B: *value = 0x02; return true;   /* 24-hour, BCD, no interrupts */
    case 0x0C: *value = 0x00; return true;
    case 0x0D: *value = 0x80; return true;   /* valid RAM and time */
    case 0x32: *value = dosvm_rtc_bcd(y / 100 % 100); return true;
    default: return false;
    }
}
#endif
