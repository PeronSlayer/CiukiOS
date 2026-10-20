/* MCI time formats shared by the devices: milliseconds <-> HMS, MSF (75
 * frames/s, MCI_MAKE_MSF byte order), SMPTE 24/25/30 and 30 drop-frame
 * (hours in the low byte, frames in the high byte). Pure arithmetic, no
 * device state. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "mci.h"
static uint32_t pack4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return (a & 255) | (b & 255) << 8 | (c & 255) << 16 | (d & 255) << 24;
}
static uint32_t smpte_rate(uint32_t format) {
  return format == MCI_FORMAT_SMPTE_24 ? 24 : format == MCI_FORMAT_SMPTE_25 ? 25 : 30;
}
/* Returns the packed value and its string-interface kind. */
uint32_t mci_ms_to_format(uint32_t ms, uint32_t format, uint32_t *kind) {
  uint32_t k = 0, v = ms;
  switch (format) {
  case MCI_FORMAT_HMS: {
    uint32_t s = ms / 1000;
    uint32_t h = s / 3600 > 255 ? 255 : s / 3600;
    v = pack4(h, s / 60 % 60, s % 60, 0);
    k = MCI_COLONIZED3_RETURN;
    break;
  }
  case MCI_FORMAT_MSF: {
    uint64_t f = (uint64_t)ms * 75 / 1000;
    uint64_t m = f / 4500;
    v = pack4(m > 255 ? 255 : (uint32_t)m, (uint32_t)(f / 75 % 60),
              (uint32_t)(f % 75), 0);
    k = MCI_COLONIZED3_RETURN;
    break;
  }
  case MCI_FORMAT_SMPTE_24:
  case MCI_FORMAT_SMPTE_25:
  case MCI_FORMAT_SMPTE_30: {
    uint32_t rate = smpte_rate(format);
    uint64_t f = (uint64_t)ms * rate / 1000;
    uint64_t h = f / (rate * 3600u);
    v = pack4(h > 255 ? 255 : (uint32_t)h, (uint32_t)(f / (rate * 60u) % 60),
              (uint32_t)(f / rate % 60), (uint32_t)(f % rate));
    k = MCI_COLONIZED4_RETURN;
    break;
  }
  case MCI_FORMAT_SMPTE_30DROP: {
    /* 29.97 frames/s; drop frames 0 and 1 of every minute not divisible
     * by ten (SMPTE 12M drop-frame numbering). */
    uint64_t n = (uint64_t)ms * 30000 / 1001000;
    uint64_t d = n / 17982, m = n % 17982;
    n += 18 * d + (m >= 2 ? 2 * ((m - 2) / 1798) : 0);
    uint64_t h = n / 108000;
    v = pack4(h > 255 ? 255 : (uint32_t)h, (uint32_t)(n / 1800 % 60),
              (uint32_t)(n / 30 % 60), (uint32_t)(n % 30));
    k = MCI_COLONIZED4_RETURN;
    break;
  }
  default:
    break;
  }
  if (kind)
    *kind = k;
  return v;
}
/* Converts a packed HMS/MSF/SMPTE value to milliseconds; nonzero if a
 * field is out of range (MCIERR_OUTOFRANGE). Other formats are the
 * devices' own. */
int mci_format_to_ms(uint32_t value, uint32_t format, uint32_t *ms) {
  uint32_t a = value & 255, b = value >> 8 & 255, c = value >> 16 & 255,
           d = value >> 24;
  switch (format) {
  case MCI_FORMAT_MILLISECONDS:
    *ms = value;
    return 0;
  case MCI_FORMAT_HMS:
    if (b > 59 || c > 59 || d)
      return 1;
    *ms = ((a * 60 + b) * 60 + c) * 1000;
    return 0;
  case MCI_FORMAT_MSF:
    if (b > 59 || c > 74 || d)
      return 1;
    *ms = (uint32_t)(((uint64_t)a * 60 + b) * 1000 + (uint64_t)c * 1000 / 75);
    return 0;
  case MCI_FORMAT_SMPTE_24:
  case MCI_FORMAT_SMPTE_25:
  case MCI_FORMAT_SMPTE_30: {
    uint32_t rate = smpte_rate(format);
    if (b > 59 || c > 59 || d >= rate)
      return 1;
    *ms = ((a * 60 + b) * 60 + c) * 1000 + d * 1000 / rate;
    return 0;
  }
  case MCI_FORMAT_SMPTE_30DROP: {
    if (b > 59 || c > 59 || d > 29 || (d < 2 && !c && b % 10))
      return 1;
    uint64_t minutes = (uint64_t)a * 60 + b;
    uint64_t n = (uint64_t)a * 108000 + (uint64_t)b * 1800 + c * 30 + d -
                 2 * (minutes - minutes / 10);
    *ms = (uint32_t)(n * 1001 / 30);
    return 0;
  }
  }
  return 1;
}
