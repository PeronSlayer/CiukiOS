# Regional Settings (G24)

## Reference behaviour and implementation decision

Research checked on 2026-10-23 before implementation:

* Windows 98 Resource Kit Appendix A, “Specifying International Settings”,
  `wrkappa_3.htm`, and chapter 23 Table 23.1, `wrkc23_2.htm` (the owner's
  read-only extraction in `/home/peronslayer/Desktop/CiukiOS/build/rskt`).
  Regional Settings selects defaults and customizes numbers, currency, time
  and dates. It does not translate the operating system UI. The existing DOS
  code page is independent of subsequent regional changes.
* Microsoft [NLS](https://learn.microsoft.com/en-us/windows/win32/intl/national-language-support),
  [GetLocaleInfoA](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getlocaleinfoa),
  [GetLocaleInfoEx](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getlocaleinfoex),
  [GetDateFormatA](https://learn.microsoft.com/en-us/windows/win32/api/datetimeapi/nf-datetimeapi-getdateformata).
  Default LCIDs resolve to the user's region; explicit supported LCIDs retain
  their own defaults. Overrides apply to the user's LCID unless
  `LOCALE_NOUSEROVERRIDE` is specified. Buffer counts include the terminator;
  `LOCALE_RETURN_NUMBER` returns a DWORD with A/W character counts 4/2.
  [GetNumberFormatA](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getnumberformata),
  [CURRENCYFMTA](https://learn.microsoft.com/en-us/windows/win32/api/winnls/ns-winnls-currencyfmta)
  and [GetTimeFormatA](https://learn.microsoft.com/en-us/windows/win32/api/datetimeapi/nf-datetimeapi-gettimeformata)
  define number inputs, grouping, currency orders and time pictures.
* [Newlib locale documentation](https://sourceware.org/newlib/libc.html#setlocale)
  and the pinned newlib 4.5.0 sources (`locale.c`, `setlocale.h`,
  `localeconv.c`, `strftime.c`, `strtod.c`, `vfprintf.c`). The current
  `--disable-newlib-mb` build accepts only C/POSIX, ignores environment
  locales and hardwires the numeric/time table getters. A narrow CiukiOS
  port patch delegates locale selection and those getters to libciuki;
  the upstream parsers, formatters and C locale remain in use.

## Contract

The Control Panel Time category contains Regional Settings (`control regional`).
Its Region, Number, Currency, Time and Date tabs edit a private snapshot.
Selecting English (United States) or Italian (Italy) resets format defaults.
Apply merges only regional keys into the latest desktop.cfg, preserving
unrelated and unknown settings; OK applies and closes after durable save;
Cancel/back/close discard edits since the last Apply. Failed saves remain
visible and retryable. The existing asynchronous settings worker owns I/O.
No new syscall, wire record or message is required.

`regional_locale=en_US|it_IT` and `regional_<field>` keys store the formats in
`/system/settings/desktop.cfg`. Missing/malformed keys keep safe defaults.
The shared parser bounds all values, rejects control characters, invalid
orders, ambiguous decimal/group separators and malformed pictures. Supported
date pictures use d/M/y and quoted literals; time pictures use H/h/m/s/t.
Gregorian dates only. Grouping supports 0 (none) or 1–9 repeated digits.

English defaults preserve the existing Windows 98 US NLS values. Italian
defaults use decimal comma, thousands dot, day/month/year, a 24-hour clock,
metric measurement and EUR with two decimals after the amount. EUR is a
deliberate current-currency default rather than the historical lira; the
currency symbol, precision and positive/negative orders are editable. The
ASCII symbol EUR avoids an encoding ambiguity between native text and ACP1252.
UI text and the UI language identifiers stay English.

Native programs start in the standard C locale and opt in with
`setlocale(LC_ALL, "")`. Selection honors nonempty LC_ALL, then the category's
LC_* variable, then LANG, then the persisted region. `C`, `POSIX`, `en_US`
and `it_IT` are accepted. LC_NUMERIC, LC_MONETARY and LC_TIME can differ;
LC_ALL queries return a restorable composite. Unsupported names fail without
changing any category. Native localeconv, printf/scanf/strtod and strftime
use the selected tables. Character classification, collation and messages
retain the current C/English implementation; this item adds formatting,
not multibyte character conversion or translated UI. Existing processes
reread regional preferences on their next `setlocale(..., "")`.
The desktop launcher passes LANG for the selected region instead of forcing
LC_ALL=C. Files opts in at startup and uses localeconv for numeric separators
and grouping, and strftime %x/%X for displayed local file dates.
Direct native launches from Files leave LC_* and LANG unset, so setlocale("")
in the child reads the current persisted preference.

Win32 NLS reads the same preferences for default/user LCIDs, locale names,
GetLocaleInfo and A/W date/time/number/currency formatting; explicit US,
Italian and invariant locales remain available. No registry mirror,
SetLocaleInfo setter, thread-specific locale selection or WM_SETTINGCHANGE
broadcast is added. DOS code pages and the G1 time-zone/RTC model do not change.

## Validation

Host coverage exercises defaults, persistence, invalid data, category
precedence, C restoration, native parsing/formatting, NLS A/W buffer and flag
semantics, and Control Panel Apply/OK/Cancel/save failure. The fx-15 model
covers regional client content updates through its existing DESK_DAMAGE path.
Guest and hardware qualification belong to the lead.
