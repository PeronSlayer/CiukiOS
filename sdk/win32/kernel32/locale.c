/* G24: US English / Italian regional formats over the shared user settings.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#define US 0x0409u
#define IT 0x0410u
#include "../../libciuki/regional_model.h"
#include "regional_io.h"
#define INVARIANT 0x007fu
#define ERROR_INVALID_FLAGS 1004u
extern void W32API GetLocalTime(SYSTEMTIME *);
DWORD W32API GetUserDefaultLCID(void) {
  struct regional r; k32_regional(&r); return r.italian ? IT : US;
}
DWORD W32API GetSystemDefaultLCID(void) { return GetUserDefaultLCID(); }
uint16_t W32API GetUserDefaultLangID(void) { return (uint16_t)GetUserDefaultLCID(); }
uint16_t W32API GetSystemDefaultLangID(void) { return GetUserDefaultLangID(); }
uint16_t W32API GetUserDefaultUILanguage(void) { return US; }
uint16_t W32API GetSystemDefaultUILanguage(void) { return US; }
DWORD W32API GetThreadLocale(void) { return GetUserDefaultLCID(); }
/* LOCALE_SYSTEM_DEFAULT, LOCALE_USER_DEFAULT and LOCALE_NEUTRAL become the
 * defaults; a neutral sublanguage becomes SUBLANG_DEFAULT. */
DWORD W32API ConvertDefaultLocale(DWORD lcid) {
  DWORD lang = lcid & 0xffff;
  if (lang == 0x800 || lang == 0x400 || !lang)
    return GetUserDefaultLCID();
  if (!(lang & 0xfc00) && (lang & 0x3ff) && lang != INVARIANT)
    return (lcid & ~0xffffu) | 0x400u | (lang & 0x3ff);
  return lcid;
}
static DWORD resolve(DWORD lcid) {
  lcid = ConvertDefaultLocale(lcid);
  return lcid == US || lcid == IT || lcid == INVARIANT ? lcid : 0;
}
BOOL W32API IsValidLocale(DWORD lcid, DWORD flags) {
  return (flags == 1 || flags == 2) && (lcid == US || lcid == IT || lcid == INVARIANT);
}
struct field {
  uint16_t type;
  uint8_t number; /* 1 decimal, 2 hexadecimal under LOCALE_RETURN_NUMBER */
  const char *value;
};
static const struct field fields[] = {
    {0x01, 2, "0409"}, {0x02, 0, "English (United States)"}, {0x03, 0, "ENU"},
    {0x04, 0, "English"}, {0x05, 1, "1"}, {0x06, 0, "United States"}, {0x07, 0, "USA"},
    {0x08, 0, "United States"}, {0x09, 2, "0409"}, {0x0a, 1, "1"}, {0x0b, 1, "437"},
    {0x0c, 0, ","}, {0x0d, 1, "1"}, {0x0e, 0, "."}, {0x0f, 0, ","}, {0x10, 0, "3;0"},
    {0x11, 1, "2"}, {0x12, 1, "1"}, {0x13, 0, "0123456789"}, {0x14, 0, "$"},
    {0x15, 0, "USD"}, {0x16, 0, "."}, {0x17, 0, ","}, {0x18, 0, "3;0"}, {0x19, 1, "2"},
    {0x1a, 1, "2"}, {0x1b, 1, "0"}, {0x1c, 1, "0"}, {0x1d, 0, "/"}, {0x1e, 0, ":"},
    {0x1f, 0, "M/d/yy"}, {0x20, 0, "dddd, MMMM dd, yyyy"}, {0x21, 1, "0"}, {0x22, 1, "0"},
    {0x23, 1, "0"}, {0x24, 1, "0"}, {0x25, 1, "0"}, {0x26, 1, "0"}, {0x27, 1, "0"},
    {0x28, 0, "AM"}, {0x29, 0, "PM"}, {0x2a, 0, "Monday"}, {0x2b, 0, "Tuesday"},
    {0x2c, 0, "Wednesday"}, {0x2d, 0, "Thursday"}, {0x2e, 0, "Friday"},
    {0x2f, 0, "Saturday"}, {0x30, 0, "Sunday"}, {0x31, 0, "Mon"}, {0x32, 0, "Tue"},
    {0x33, 0, "Wed"}, {0x34, 0, "Thu"}, {0x35, 0, "Fri"}, {0x36, 0, "Sat"}, {0x37, 0, "Sun"},
    {0x38, 0, "January"}, {0x39, 0, "February"}, {0x3a, 0, "March"}, {0x3b, 0, "April"},
    {0x3c, 0, "May"}, {0x3d, 0, "June"}, {0x3e, 0, "July"}, {0x3f, 0, "August"},
    {0x40, 0, "September"}, {0x41, 0, "October"}, {0x42, 0, "November"},
    {0x43, 0, "December"}, {0x44, 0, "Jan"}, {0x45, 0, "Feb"}, {0x46, 0, "Mar"},
    {0x47, 0, "Apr"}, {0x48, 0, "May"}, {0x49, 0, "Jun"}, {0x4a, 0, "Jul"}, {0x4b, 0, "Aug"},
    {0x4c, 0, "Sep"}, {0x4d, 0, "Oct"}, {0x4e, 0, "Nov"}, {0x4f, 0, "Dec"}, {0x50, 0, ""},
    {0x51, 0, "-"}, {0x52, 1, "3"}, {0x53, 1, "0"}, {0x54, 1, "1"}, {0x55, 1, "0"},
    {0x56, 1, "1"}, {0x57, 1, "0"}, {0x59, 0, "en"}, {0x5a, 0, "US"}, {0x5c, 0, "en-US"},
    {0x5d, 0, "h:mm:ss"}, {0x5e, 0, "0409:00000409"}, {0x60, 0, "Mo"}, {0x61, 0, "Tu"},
    {0x62, 0, "We"}, {0x63, 0, "Th"}, {0x64, 0, "Fr"}, {0x65, 0, "Sa"}, {0x66, 0, "Su"},
    {0x67, 0, "eng"}, {0x68, 0, "USA"}, {0x69, 0, "NaN"}, {0x6a, 0, "Infinity"},
    {0x6b, 0, "-Infinity"}, {0x6c, 0, "Latn;"}, {0x6d, 0, "en"}, {0x6e, 0, "en-US"},
    {0x6f, 0, "English (United States)"}, {0x70, 1, "0"}, {0x71, 1, "0"},
    {0x72, 0, "English (United States)"}, {0x73, 0, "English (United States)"},
    {0x74, 1, "1"}, {0x75, 1, "1"}, {0x76, 0, "%"}, {0x77, 0, "\x89"},
    {0x78, 0, "MMMM dd"}, {0x79, 0, "h:mm tt"}, {0x7a, 0, "dflt"}, {0x7b, 0, "en-US"},
    {0x1001, 0, "English"}, {0x1002, 0, "United States"}, {0x1003, 0, "h:mm:ss tt"},
    {0x1004, 1, "1252"}, {0x1005, 1, "0"}, {0x1006, 0, "MMMM, yyyy"},
    {0x1007, 0, "US Dollar"}, {0x1008, 0, "US Dollar"}, {0x1009, 1, "1"},
    {0x100a, 1, "1"}, {0x100b, 1, "0"}, {0x100c, 1, "6"}, {0x100d, 1, "0"},
    {0x100e, 0, ""}, {0x100f, 0, ""}, {0x1010, 1, "1"}, {0x1011, 1, "10000"},
    {0x1012, 1, "037"}, {0x1013, 0, "Default"}, {0x1014, 1, "1"},
};
static const struct field *field(DWORD lcid, DWORD type) {
  static const struct field invariant[] = {
      {0x01, 2, "007f"}, {0x02, 0, "Invariant Language (Invariant Country)"},
      {0x03, 0, "IVL"}, {0x06, 0, "Invariant Country"}, {0x07, 0, "IVC"},
      {0x1001, 0, "Invariant Language"}, {0x1002, 0, "Invariant Country"},
      {0x59, 0, "iv"}, {0x5a, 0, "IV"}, {0x5c, 0, ""}, {0x14, 0, "\xa4"},
      {0x15, 0, "XDR"}, {0x1f, 0, "MM/dd/yyyy"}, {0x6d, 0, ""}};
  static const struct field italian[] = {
      {0x01,2,"0410"},{0x02,0,"Italian (Italy)"},{0x03,0,"ITA"},{0x04,0,"italiano"},
      {0x05,1,"39"},{0x06,0,"Italy"},{0x07,0,"ITA"},{0x08,0,"Italia"},{0x09,2,"0410"},
      {0x0a,1,"39"},{0x0b,1,"850"},{0x0c,0,";"},{0x0d,1,"0"},
      {0x0e,0,","},{0x0f,0,"."},{0x14,0,"EUR"},{0x15,0,"EUR"},{0x16,0,","},{0x17,0,"."},
      {0x1b,1,"3"},{0x1c,1,"8"},{0x1f,0,"dd/MM/yyyy"},{0x20,0,"dddd d MMMM yyyy"},
      {0x21,1,"1"},{0x22,1,"1"},{0x23,1,"1"},{0x24,1,"1"},{0x25,1,"1"},{0x26,1,"1"},{0x27,1,"1"},
      {0x28,0,""},{0x29,0,""},
      {0x2a,0,"luned\xec"},{0x2b,0,"marted\xec"},{0x2c,0,"mercoled\xec"},
      {0x2d,0,"gioved\xec"},{0x2e,0,"venerd\xec"},{0x2f,0,"sabato"},{0x30,0,"domenica"},
      {0x31,0,"lun"},{0x32,0,"mar"},{0x33,0,"mer"},{0x34,0,"gio"},{0x35,0,"ven"},{0x36,0,"sab"},{0x37,0,"dom"},
      {0x38,0,"gennaio"},{0x39,0,"febbraio"},{0x3a,0,"marzo"},{0x3b,0,"aprile"},
      {0x3c,0,"maggio"},{0x3d,0,"giugno"},{0x3e,0,"luglio"},{0x3f,0,"agosto"},
      {0x40,0,"settembre"},{0x41,0,"ottobre"},{0x42,0,"novembre"},{0x43,0,"dicembre"},
      {0x44,0,"gen"},{0x45,0,"feb"},{0x46,0,"mar"},{0x47,0,"apr"},{0x48,0,"mag"},{0x49,0,"giu"},
      {0x4a,0,"lug"},{0x4b,0,"ago"},{0x4c,0,"set"},{0x4d,0,"ott"},{0x4e,0,"nov"},{0x4f,0,"dic"},
      {0x54,1,"0"},{0x55,1,"1"},{0x59,0,"it"},{0x5a,0,"IT"},{0x5c,0,"it-IT"},
      {0x5d,0,"HH:mm:ss"},{0x5e,0,"0410:00000410"},
      {0x60,0,"lu"},{0x61,0,"ma"},{0x62,0,"me"},{0x63,0,"gi"},{0x64,0,"ve"},{0x65,0,"sa"},{0x66,0,"do"},
      {0x67,0,"ita"},{0x68,0,"ITA"},{0x6d,0,"it"},{0x6e,0,"it-IT"},
      {0x6f,0,"Italian (Italy)"},{0x72,0,"Italian (Italy)"},{0x73,0,"italiano (Italia)"},
      {0x78,0,"d MMMM"},{0x79,0,"HH:mm"},{0x7b,0,"it-IT"},
      {0x1001,0,"Italian"},{0x1002,0,"Italy"},{0x1003,0,"HH:mm:ss"},{0x1006,0,"MMMM yyyy"},
      {0x1007,0,"Euro"},{0x1008,0,"euro"},{0x1012,1,"20280"}};
  if (lcid == IT)
    for (unsigned i=0;i<sizeof(italian)/sizeof(*italian);i++)
      if (italian[i].type==type) return &italian[i];
  if (lcid == INVARIANT)
    for (unsigned i = 0; i < sizeof(invariant) / sizeof(*invariant); i++)
      if (invariant[i].type == type)
        return &invariant[i];
  for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); i++)
    if (fields[i].type == type)
      return &fields[i];
  return 0;
}
/* Per-call snapshot: pointers never escape the caller's regional record. */
static const struct field *regional_field(const struct regional *r,DWORD lcid,DWORD type,
                                          DWORD flags,struct field *scratch) {
  const struct field *f=field(lcid,type);
  if(!f || lcid!=(r->italian?IT:US) || (flags&0x80000000u)) return f;
  static const char *const groups[]={"0","1;0","2;0","3;0","4;0","5;0","6;0","7;0","8;0","9;0"};
  if(type==0x10 || type==0x18) {
    *scratch=*f; scratch->value=groups[r->value[REG_GROUPING][0]-'0']; return scratch;
  }
  const char *derived=0;
  switch(type) {
  case 0x1d: derived=reg_separator(r->value[REG_DATE]); break;
  case 0x1e: derived=reg_separator(r->value[REG_TIME]); break;
  case 0x21: derived=reg_date_order(r->value[REG_DATE]); break;
  case 0x22: derived=reg_date_order(r->value[REG_LONG_DATE]); break;
  case 0x23: derived=reg_run(r->value[REG_TIME],'H')?"1":"0"; break;
  case 0x24: derived=reg_run(r->value[REG_DATE],'y')>2?"1":"0"; break;
  case 0x25: derived=(reg_run(r->value[REG_TIME],'H')>1 || reg_run(r->value[REG_TIME],'h')>1)?"1":"0"; break;
  case 0x26: derived=reg_run(r->value[REG_DATE],'d')==2?"1":"0"; break;
  case 0x27: derived=reg_run(r->value[REG_DATE],'M')==2?"1":"0"; break;
  }
  if(derived) { *scratch=*f; scratch->value=derived; return scratch; }
  int index=-1;
  switch(type) {
  case 0x0d:index=REG_MEASURE;break;
  case 0x0e:index=REG_DECIMAL;break; case 0x0f:index=REG_THOUSAND;break;
  case 0x11:index=REG_DIGITS;break; case 0x1010:index=REG_NEGATIVE;break;
  case 0x14:index=REG_SYMBOL;break; case 0x16:index=REG_MON_DECIMAL;break;
  case 0x17:index=REG_MON_THOUSAND;break; case 0x19:index=REG_MON_DIGITS;break;
  case 0x1b:index=REG_MON_POSITIVE;break; case 0x1c:index=REG_MON_NEGATIVE;break;
  case 0x1f:index=REG_DATE;break; case 0x20:index=REG_LONG_DATE;break;
  case 0x1003:index=REG_TIME;break; case 0x28:index=REG_AM;break; case 0x29:index=REG_PM;break;
  }
  if(index<0) return f;
  *scratch=*f; scratch->value=r->value[index]; return scratch;
}
static const char *regional_name(const struct regional *r,DWORD lcid,DWORD type,DWORD flags) {
  struct field scratch; return regional_field(r,lcid,type,flags,&scratch)->value;
}
static DWORD regional_resolve(DWORD lcid,const struct regional *r) {
  if(lcid==0 || lcid==0x400 || lcid==0x800) return r->italian?IT:US;
  lcid=ConvertDefaultLocale(lcid);
  return lcid==US || lcid==IT || lcid==INVARIANT ? lcid : 0;
}
static DWORD number(const struct field *f) {
  DWORD n = 0;
  for (const char *p = f->value; *p; p++)
    n = n * (f->number == 2 ? 16 : 10) +
        (DWORD)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
  return n;
}
/* LOCALE_NOUSEROVERRIDE and LOCALE_USE_CP_ACP change nothing here;
 * LOCALE_RETURN_NUMBER writes the value as a DWORD (`units` per DWORD). */
static int locale_info(DWORD lcid, DWORD type, void *out, int cap, int wide) {
  DWORD base = type & ~0xc0000000u, numeric = base & 0x20000000u;
  base &= ~0x20000000u;
  struct regional r; k32_regional(&r); struct field scratch;
  const struct field *f = (lcid = regional_resolve(lcid,&r)) ? regional_field(&r,lcid,base,type,&scratch) : 0;
  if (cap < 0 || (cap && !out)) {
    SetLastError(87);
    return 0;
  }
  if (!lcid) {
    SetLastError(87);
    return 0;
  }
  if (!f || (numeric && !f->number)) {
    SetLastError(ERROR_INVALID_FLAGS);
    return 0;
  }
  int units = wide ? 2 : 4;
  if (numeric) {
    if (!cap)
      return units;
    if (cap < units) {
      SetLastError(122);
      return 0;
    }
    DWORD n = number(f);
    w32_copy(out, &n, 4);
    return units;
  }
  int n = (int)w32_strlen(f->value) + 1;
  if (!cap)
    return n;
  if (cap < n) {
    SetLastError(122);
    return 0;
  }
  if (wide)
    k32_widen(f->value, out, (DWORD)n);
  else
    w32_copy(out, f->value, (DWORD)n);
  return n;
}
int W32API GetLocaleInfoA(DWORD lcid, DWORD type, char *out, int cap) {
  return locale_info(lcid, type, out, cap, 0);
}
/* f5-20B (amendment 1): the ANSI table widened through code page 1252
 * (Windows 98 refused the W form; Microsoft Layer for Unicode model). */
int W32API GetLocaleInfoW(DWORD lcid, DWORD type, WCHAR *out, int cap) {
  return locale_info(lcid, type, out, cap, 1);
}
/* Locale names: "en-US" (any case), "en" with LOCALE_ALLOW_NEUTRAL_NAMES,
 * "" for the invariant locale, NULL or "!x-sys-default-locale" for the
 * default. */
static int name_equal(const WCHAR *a, const char *b) {
  for (; *b; a++, b++) {
    WCHAR x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = (WCHAR)(uint8_t)*b;
    if (x != (y >= 'A' && y <= 'Z' ? y + 32 : y))
      return 0;
  }
  return !*a;
}
DWORD W32API LocaleNameToLCID(const WCHAR *name, DWORD flags) {
  if (!name || name_equal(name, "!x-sys-default-locale")) return GetUserDefaultLCID();
  if (name_equal(name,"en-US")) return US;
  if (name_equal(name,"it-IT")) return IT;
  if (name_equal(name,"it")) return flags & 0x08000000u ? 0x0010u : IT;
  if (!*name)
    return INVARIANT;
  if (name_equal(name, "en"))
    return flags & 0x08000000u ? 0x0009u : US;
  SetLastError(87);
  return 0;
}
BOOL W32API IsValidLocaleName(const WCHAR *name) {
  return name && (!*name || name_equal(name, "en-US") || name_equal(name, "en") || name_equal(name,"it-IT") || name_equal(name,"it"));
}
static int copy_name(const char *value, WCHAR *out, int cap) {
  int n = (int)w32_strlen(value) + 1;
  if (!out || !cap)
    return n;
  if (cap < n) {
    SetLastError(122);
    return 0;
  }
  k32_widen(value, out, (DWORD)n);
  return n;
}
int W32API LCIDToLocaleName(DWORD lcid, WCHAR *out, int cap, DWORD flags) {
  if (cap < 0) {
    SetLastError(87);
    return 0;
  }
  if ((lcid & 0xffff) == 0x0009 && flags & 0x08000000u)
    return copy_name("en", out, cap);
  if ((lcid & 0xffff)==0x0010 && (flags&0x08000000u)) return copy_name("it",out,cap);
  DWORD r = resolve(lcid);
  if (!r) {
    SetLastError(87);
    return 0;
  }
  return copy_name(r == INVARIANT ? "" : r == IT ? "it-IT" : "en-US", out, cap);
}
int W32API GetUserDefaultLocaleName(WCHAR *out, int cap) {
  if (!out || cap <= 0) {
    SetLastError(87);
    return 0;
  }
  return copy_name(GetUserDefaultLCID()==IT ? "it-IT" : "en-US", out, cap);
}
int W32API GetLocaleInfoEx(const WCHAR *name, DWORD type, WCHAR *out, int cap) {
  DWORD lcid = LocaleNameToLCID(name, 0);
  if (!lcid)
    return 0;
  if ((type & ~0xe0000000u) == 0x5c && lcid == US && name && name_equal(name, "en"))
    return copy_name("en", out, cap);
  if ((type & ~0xe0000000u)==0x5c && lcid==IT && name && name_equal(name,"it"))
    return copy_name("it",out,cap);
  return locale_info(lcid, type, out, cap, 1);
}
/* f7 (the Quake III Arena C runtime imports it): EnumSystemLocalesA calls
 * the procedure with each locale identifier as eight hexadecimal digits
 * ("00000409"), stopping when it returns FALSE. LCID_INSTALLED (1) and
 * LCID_SUPPORTED (2) list the locales IsValidLocale accepts (US English and
 * Italian; the invariant locale is not enumerated); LCID_ALTERNATE_SORTS (4)
 * adds none, since no alternate sort orders exist. */
typedef BOOL(W32API *k32_locale_proc_a)(char *);
BOOL W32API EnumSystemLocalesA(k32_locale_proc_a proc, DWORD flags) {
  if (!proc) {
    SetLastError(87);
    return 0;
  }
  if ((flags & ~7u) || (flags & 3u) == 3u) {
    SetLastError(ERROR_INVALID_FLAGS);
    return 0;
  }
  if (flags == 4u)
    return 1;
  char text[9] = "00000409";
  if (!proc(text))
    return 1;
  w32_copy(text, "00000410", 9);
  proc(text);
  return 1;
}
typedef BOOL(W32API *k32_locale_proc)(WCHAR *, DWORD, intptr_t);
/* LOCALE_WINDOWS 1, LOCALE_NEUTRALDATA 0x10, LOCALE_SPECIFICDATA 0x20. */
BOOL W32API EnumSystemLocalesEx(k32_locale_proc proc, DWORD flags, intptr_t param,
                                void *reserved) {
  if (!proc || reserved || (flags & ~0x3fu)) {
    SetLastError(87);
    return 0;
  }
  WCHAR specific[6], neutral[3];
  k32_widen("en-US", specific, 6);
  k32_widen("en", neutral, 3);
  int all = !(flags & 0x30);
  if ((all || flags & 0x20) && !proc(specific, 0x21, param))
    return 1;
  if ((all || flags & 0x10) && !proc(neutral,0x11,param)) return 1;
  k32_widen("it-IT",specific,6); k32_widen("it",neutral,3);
  if ((all || flags & 0x20) && !proc(specific,0x21,param)) return 1;
  if (all || flags & 0x10) proc(neutral,0x11,param);
  return 1;
}
/* CP_ACP 0, CP_OEMCP 1, CP_THREAD_ACP 3 and the three tables of the core. */
typedef struct {
  DWORD max_size;
  uint8_t default_char[2], lead[12];
  WCHAR unicode_default;
  DWORD code_page;
  WCHAR name[260];
} CPINFOEXW;
static DWORD code_page(DWORD cp) {
  cp = cp == 0 || cp == 3 ? 1252 : cp == 1 ? 437 : cp;
  return cp == 437 || cp == 850 || cp == 1252 ? cp : 0;
}
BOOL W32API GetCPInfoExW(DWORD cp, DWORD flags, CPINFOEXW *out) {
  DWORD page = code_page(cp);
  if (!page || flags || !out) {
    SetLastError(87);
    return 0;
  }
  CPINFOEXW info = {.max_size = 1, .default_char = {'?', 0}, .unicode_default = '?',
                    .code_page = page};
  const char *name = page == 1252 ? "1252  (ANSI - Latin I)"
                     : page == 437 ? "437   (OEM - United States)"
                                   : "850   (OEM - Multilingual Latin I)";
  k32_widen(name, info.name, w32_strlen(name) + 1);
  *out = info;
  return 1;
}
/* Character types (CT_CTYPE1/2/3) of UTF-16 code units: exact for Latin-1
 * and the CP1252 additions, letters elsewhere apart from the general
 * punctuation and CJK space blocks. */
static uint16_t ctype1(WCHAR c) {
  if (c < 0x20 || (c >= 0x7f && c < 0xa0)) {
    uint16_t t = 0x220;
    if ((c >= 9 && c <= 13) || c == 0x85)
      t |= 8;
    if (c == 9)
      t |= 0x40;
    return t;
  }
  if (c == 0x20 || c == 0xa0 || c == 0x3000)
    return 0x248;
  if (c >= '0' && c <= '9')
    return 0x284;
  if (c >= 'A' && c <= 'Z')
    return 0x301 | (c <= 'F' ? 0x80 : 0);
  if (c >= 'a' && c <= 'z')
    return 0x302 | (c <= 'f' ? 0x80 : 0);
  if (c == 0xaa || c == 0xba || c == 0xdf || c == 0xff || (c >= 0xdf && c <= 0xff && c != 0xf7))
    return 0x302;
  if ((c >= 0xc0 && c <= 0xde && c != 0xd7))
    return 0x301;
  if (c == 0x152 || c == 0x160 || c == 0x178 || c == 0x17d)
    return 0x301;
  if (c == 0x153 || c == 0x161 || c == 0x17e || c == 0x192)
    return 0x302;
  if (c < 0x100 || (c >= 0x2000 && c <= 0x206f) || c == 0x20ac || c == 0x2122 || c == 0x2c6 ||
      c == 0x2dc)
    return 0x210;
  return 0x300;
}
static uint16_t ctype2(WCHAR c, uint16_t t1) {
  if (t1 & 0x100)
    return 1;
  if (t1 & 4)
    return 3;
  if (c == '+' || c == '-')
    return 4;
  if (c == '#' || c == '$' || c == '%' || c == 0xa2 || c == 0xa3 || c == 0xa5 || c == 0xb0 ||
      c == 0x20ac)
    return 5;
  if (c == ',' || c == '.' || c == ':' || c == '/' || c == 0xa0)
    return 7;
  if (c == 0x0a || c == 0x0d || c == 0x1c || c == 0x1d || c == 0x1e || c == 0x85)
    return 8;
  if (c == 9 || c == 0x0b || c == 0x1f)
    return 9;
  if (c == 0x0c || c == 0x20 || c == 0x3000)
    return 10;
  if (t1 & 0x20)
    return 0;
  return 11;
}
static uint16_t ctype3(WCHAR c, uint16_t t1) {
  if (t1 & 0x100)
    return 0x8000;
  if ((t1 & 0x10) && c != '_')
    return 8;
  return 0;
}
static BOOL string_type(DWORD type, const WCHAR *s, int n, uint16_t *out) {
  if (!s || !out || n == 0 || n < -1 || (type != 1 && type != 2 && type != 4)) {
    SetLastError(type == 1 || type == 2 || type == 4 ? 87 : ERROR_INVALID_FLAGS);
    return 0;
  }
  if (n == -1)
    n = (int)k32_wlen(s) + 1;
  for (int i = 0; i < n; i++) {
    uint16_t t1 = ctype1(s[i]);
    out[i] = type == 1 ? t1 : type == 2 ? ctype2(s[i], t1) : ctype3(s[i], t1);
  }
  return 1;
}
BOOL W32API GetStringTypeExW(DWORD lcid, DWORD type, const WCHAR *s, int n, uint16_t *out) {
  (void)lcid;
  return string_type(type, s, n, out);
}
BOOL W32API GetStringTypeExA(DWORD lcid, DWORD type, const char *s, int n, uint16_t *out) {
  (void)lcid;
  if (!s || !out || n == 0 || n < -1) {
    SetLastError(87);
    return 0;
  }
  if (n == -1)
    n = (int)w32_strlen(s) + 1;
  for (int i = 0; i < n; i++) {
    WCHAR w;
    MultiByteToWideChar(1252, 0, s + i, 1, &w, 1);
    if (!string_type(type, &w, 1, out + i))
      return 0;
  }
  return 1;
}
/* The Windows 9x signature: (Locale, InfoType, Source, Count, CharType). */
BOOL W32API GetStringTypeA(DWORD lcid, DWORD type, const char *s, int n, uint16_t *out) {
  return GetStringTypeExA(lcid, type, s, n, out);
}
/* f5-20B (amendment 1): (InfoType, Source, Count, CharType), the tables of
 * the ANSI form for the user default locale. */
BOOL W32API GetStringTypeW(DWORD type, const WCHAR *s, int n, uint16_t *out) {
  return GetStringTypeExW(0x400u, type, s, n, out);
}
/* CSTR_LESS_THAN 1, CSTR_EQUAL 2, CSTR_GREATER_THAN 3 over code units. */
int W32API CompareStringOrdinal(const WCHAR *a, int n, const WCHAR *b, int m, BOOL fold) {
  if (!a || !b || n < -1 || m < -1) {
    SetLastError(87);
    return 0;
  }
  if (n == -1)
    n = (int)k32_wlen(a);
  if (m == -1)
    m = (int)k32_wlen(b);
  for (int i = 0; i < n && i < m; i++) {
    WCHAR x = a[i], y = b[i];
    if (fold) {
      x = RtlUpcaseUnicodeChar(x);
      y = RtlUpcaseUnicodeChar(y);
    }
    if (x != y)
      return x < y ? 1 : 3;
  }
  return n < m ? 1 : n > m ? 3 : 2;
}
/* Output assembly for the picture-string formatters. */
struct text {
  char data[256];
  DWORD n;
  int overflow;
};
static void put(struct text *t, const char *s) {
  for (; *s; s++)
    if (t->n < sizeof(t->data) - 1)
      t->data[t->n++] = *s;
    else t->overflow=1;
}
static void put_number(struct text *t, DWORD v, unsigned width) {
  char digits[12];
  unsigned n = 0;
  do {
    digits[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n < width)
    digits[n++] = '0';
  char s[2] = {0, 0};
  while (n) {
    s[0] = digits[--n];
    put(t, s);
  }
}
static int finish(const struct text *t, char *out, int cap) {
  if(t->overflow) { SetLastError(122); return 0; }
  int n = (int)t->n + 1;
  if (cap < 0 || (cap && !out)) {
    SetLastError(87);
    return 0;
  }
  if (!cap)
    return n;
  if (cap < n) {
    SetLastError(122);
    return 0;
  }
  w32_copy(out, t->data, t->n);
  out[t->n] = 0;
  return n;
}
static unsigned run(const char *s, char c) {
  unsigned n = 0;
  while (s[n] == c)
    n++;
  return n;
}
/* 'quoted text', '' a literal quote. Returns characters consumed. */
static unsigned quoted(const char *s, struct text *t) {
  unsigned i = 1;
  char one[2] = {0, 0};
  if (s[1] == '\'') {
    put(t, "'");
    return 2;
  }
  for (; s[i]; i++) {
    if (s[i] == '\'') {
      if (s[i + 1] != '\'')
        return i + 1;
      i++;
    }
    one[0] = s[i];
    put(t, one);
  }
  return i;
}
static int leap(unsigned y) { return !(y % 4) && (y % 100 || !(y % 400)); }
static int valid_date(const SYSTEMTIME *d) {
  static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return d->year >= 1601 && d->year <= 30827 && d->month >= 1 && d->month <= 12 && d->day >= 1 &&
         d->day <= days[d->month - 1] + (d->month == 2 && leap(d->year));
}
static unsigned weekday(const SYSTEMTIME *d) {
  static const uint8_t offset[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  unsigned y = d->year - (d->month < 3);
  return (y + y / 4 - y / 100 + y / 400 + offset[d->month - 1] + d->day) % 7;
}
/* DATE_SHORTDATE 1, DATE_LONGDATE 2, DATE_YEARMONTH 8; an explicit picture
 * excludes those flags (ERROR_INVALID_FLAGS). */
int W32API GetDateFormatA(DWORD lcid, DWORD flags, const SYSTEMTIME *date, const char *picture,
                          char *out, int cap) {
  SYSTEMTIME d;
  struct regional r; k32_regional(&r);
  if (!(lcid=regional_resolve(lcid,&r))) {
    SetLastError(87);
    return 0;
  }
  if ((flags & ~0xc000003fu) || (picture && (flags & 0xb)) ||
      ((flags & 1) + !!(flags & 2) + !!(flags & 8) > 1) || ((flags & 0x30) == 0x30)) {
    SetLastError(ERROR_INVALID_FLAGS);
    return 0;
  }
  if (date)
    d = *date;
  else
    GetLocalTime(&d);
  if (!valid_date(&d)) {
    SetLastError(87);
    return 0;
  }
  if (!picture)
    picture = regional_name(&r,lcid,flags & 2 ? 0x20 : flags & 8 ? 0x1006 : 0x1f,flags);
  struct text t = {.n = 0};
  unsigned wd = weekday(&d);
  for (const char *p = picture; *p;) {
    unsigned n = run(p, *p);
    if (*p == 'd') {
      if (n <= 2)
        put_number(&t, d.day, n);
      else
        put(&t, regional_name(&r,lcid,(n == 3 ? 0x31 : 0x2a) + (wd + 6) % 7,flags));
    } else if (*p == 'M') {
      if (n <= 2)
        put_number(&t, d.month, n);
      else
        put(&t, regional_name(&r,lcid,(n == 3 ? 0x44 : 0x38) + d.month - 1,flags));
    } else if (*p == 'y') {
      if (n <= 2)
        put_number(&t, d.year % 100, n);
      else
        put_number(&t, d.year, 4);
    } else if (*p == 'g')
      put(&t, "A.D.");
    else if (*p == '\'') {
      p += quoted(p, &t);
      continue;
    } else {
      char one[2] = {*p, 0};
      put(&t, one);
      n = 1;
    }
    p += n;
  }
  return finish(&t, out, cap);
}
/* TIME_NOMINUTESORSECONDS 1, TIME_NOSECONDS 2, TIME_NOTIMEMARKER 4,
 * TIME_FORCE24HOURFORMAT 8. A suppressed field also drops the separator
 * text written since the previous field. */
int W32API GetTimeFormatA(DWORD lcid, DWORD flags, const SYSTEMTIME *time, const char *picture,
                          char *out, int cap) {
  SYSTEMTIME v;
  struct regional r; k32_regional(&r);
  if (!(lcid=regional_resolve(lcid,&r))) {
    SetLastError(87);
    return 0;
  }
  if (flags & ~0xc000000fu) {
    SetLastError(ERROR_INVALID_FLAGS);
    return 0;
  }
  if (time)
    v = *time;
  else
    GetLocalTime(&v);
  if (v.hour > 23 || v.minute > 59 || v.second > 59 || v.milliseconds > 999) {
    SetLastError(87);
    return 0;
  }
  if (!picture)
    picture = regional_name(&r,lcid,0x1003,flags);
  struct text t = {.n = 0};
  DWORD mark = 0;
  for (const char *p = picture; *p;) {
    unsigned n = run(p, *p);
    char c = *p;
    int skip = ((c == 'm' || c == 's') && (flags & 1)) || (c == 's' && (flags & 2)) ||
               (c == 't' && (flags & 4));
    if (skip)
      t.n = mark;
    else if (c == 'h' || c == 'H') {
      unsigned h = v.hour;
      if (c == 'h' && !(flags & 8))
        h = h % 12 ? h % 12 : 12;
      put_number(&t, h, n > 1 ? 2 : 1);
    } else if (c == 'm')
      put_number(&t, v.minute, n > 1 ? 2 : 1);
    else if (c == 's')
      put_number(&t, v.second, n > 1 ? 2 : 1);
    else if (c == 't') {
      const char *m = regional_name(&r,lcid,v.hour < 12 ? 0x28 : 0x29,flags);
      if (n == 1) {
        char one[2] = {m[0], 0};
        put(&t, one);
      } else
        put(&t, m);
    } else if (c == '\'') {
      p += quoted(p, &t);
      continue;
    } else {
      char one[2] = {c, 0};
      put(&t, one);
      p++;
      continue;
    }
    mark = t.n;
    p += n;
  }
  return finish(&t, out, cap);
}
/* NUMBERFMT/CURRENCYFMT, ANSI form. Grouping 0-9, or 32 (3 then 2). */
typedef struct {
  DWORD digits, leading_zero, grouping;
  const char *decimal, *thousand;
  DWORD negative;
} NUMBERFMTA;
typedef struct {
  DWORD digits, leading_zero, grouping;
  const char *decimal, *thousand;
  DWORD negative, positive;
  const char *symbol;
} CURRENCYFMTA;
/* value: [-]digits[.digits]; rounded half up to `digits` decimals. */
static int format_number(const char *value, DWORD digits, DWORD leading, DWORD grouping,
                         const char *decimal, const char *thousand, struct text *t,
                         int *negative) {
  const char *p = value;
  *negative = *p == '-';
  p += *negative;
  char whole[128], frac[128];
  DWORD w = 0, f = 0;
  while (*p >= '0' && *p <= '9' && w < sizeof(whole) - 2)
    whole[w++] = *p++;
  if (*p == '.') {
    p++;
    while (*p >= '0' && *p <= '9' && f < sizeof(frac) - 1)
      frac[f++] = *p++;
  }
  if (*p || (!w && !f) || digits > 9 || (grouping > 9 && grouping != 32)) {
    SetLastError(87);
    return 0;
  }
  while (f < digits)
    frac[f++] = '0';
  if (f > digits && frac[digits] >= '5') {
    int i = (int)digits - 1, carry = 1;
    for (; i >= 0 && carry; i--)
      frac[i] = frac[i] == '9' ? '0' : (char)(frac[i] + 1), carry = frac[i] == '0';
    for (i = (int)w - 1; i >= 0 && carry; i--)
      whole[i] = whole[i] == '9' ? '0' : (char)(whole[i] + 1), carry = whole[i] == '0';
    if (carry) {
      w32_copy(whole + 1, whole, w);
      whole[0] = '1';
      w++;
    }
  }
  DWORD start = 0;
  while (start + 1 < w && whole[start] == '0')
    start++;
  int zero = 1;
  for (DWORD i = start; i < w; i++)
    zero &= whole[i] == '0';
  for (DWORD i = 0; i < digits; i++)
    zero &= frac[i] == '0';
  if (zero)
    *negative = 0;
  if (w - start == 1 && whole[start] == '0' && !leading)
    start = w;
  if (start == w && !w && leading)
    put(t, "0");
  char one[2] = {0, 0};
  DWORD count = w - start, first = grouping == 32 ? 3 : grouping, emitted = 0;
  for (DWORD i = start; i < w; i++) {
    one[0] = whole[i];
    put(t, one);
    emitted++;
    DWORD left = count - emitted;
    if (left && first) {
      int boundary = grouping == 32 ? (left == 3 || (left > 3 && (left - 3) % 2 == 0))
                                    : left % first == 0;
      if (boundary)
        put(t, thousand);
    }
  }
  if (digits) {
    put(t, decimal);
    for (DWORD i = 0; i < digits; i++) {
      one[0] = frac[i];
      put(t, one);
    }
  }
  if(t->overflow) { SetLastError(122); return 0; }
  return 1;
}
static int number_format(const char *value, const NUMBERFMTA *fmt, DWORD flags, char *out,
                         int cap) {
  if (!value || (fmt && (flags & 0x80000000u)) || (flags & ~0xc0000000u)) {
    SetLastError(!value ? 87 : ERROR_INVALID_FLAGS);
    return 0;
  }
  NUMBERFMTA d = {2, 1, 3, ".", ",", 1};
  if (fmt)
    d = *fmt;
  if (!d.decimal || !d.thousand || d.negative > 4) {
    SetLastError(87);
    return 0;
  }
  struct text body = {.n = 0}, t = {.n = 0};
  int negative;
  if (!format_number(value, d.digits, d.leading_zero, d.grouping, d.decimal, d.thousand, &body,
                     &negative))
    return 0;
  body.data[body.n] = 0;
  static const char *const forms[5][2] = {{"(", ")"}, {"-", ""}, {"- ", ""}, {"", "-"}, {"", " -"}};
  if (negative)
    put(&t, forms[d.negative][0]);
  put(&t, body.data);
  if (negative)
    put(&t, forms[d.negative][1]);
  return finish(&t, out, cap);
}
int W32API GetNumberFormatA(DWORD lcid, DWORD flags, const char *value, const NUMBERFMTA *fmt,
                            char *out, int cap) {
  struct regional r; k32_regional(&r);
  if (!(lcid=regional_resolve(lcid,&r))) {
    SetLastError(87);
    return 0;
  }
  NUMBERFMTA d = {
      (DWORD)(regional_name(&r,lcid,0x11,flags)[0]-'0'),1,3,
      regional_name(&r,lcid,0x0e,flags),regional_name(&r,lcid,0x0f,flags),
      (DWORD)(regional_name(&r,lcid,0x1010,flags)[0]-'0')};
  if(lcid==(r.italian?IT:US) && !(flags&0x80000000u)) d.grouping=(DWORD)(r.value[REG_GROUPING][0]-'0');
  /* Preserve flag validation for caller-supplied formats. */
  if(fmt) return number_format(value,fmt,flags,out,cap);
  if(flags&~0xc0000000u) { SetLastError(ERROR_INVALID_FLAGS); return 0; }
  return number_format(value,&d,flags&~0x80000000u,out,cap);
}
int W32API GetCurrencyFormatA(DWORD lcid, DWORD flags, const char *value,
                              const CURRENCYFMTA *fmt, char *out, int cap) {
  struct regional r; k32_regional(&r);
  if (!(lcid=regional_resolve(lcid,&r))) {
    SetLastError(87);
    return 0;
  }
  if (!value || (fmt && (flags & 0x80000000u)) || (flags & ~0xc0000000u)) {
    SetLastError(!value ? 87 : ERROR_INVALID_FLAGS);
    return 0;
  }
  struct field nf,pf;
  CURRENCYFMTA d = {
      (DWORD)(regional_name(&r,lcid,0x19,flags)[0]-'0'),1,3,
      regional_name(&r,lcid,0x16,flags),regional_name(&r,lcid,0x17,flags),
      number(regional_field(&r,lcid,0x1c,flags,&nf)),
      number(regional_field(&r,lcid,0x1b,flags,&pf)),regional_name(&r,lcid,0x14,flags)};
  if(lcid==(r.italian?IT:US) && !(flags&0x80000000u)) d.grouping=(DWORD)(r.value[REG_GROUPING][0]-'0');
  if (fmt)
    d = *fmt;
  if (!d.decimal || !d.thousand || !d.symbol || d.negative > 15 || d.positive > 3) {
    SetLastError(87);
    return 0;
  }
  struct text body = {.n = 0}, t = {.n = 0};
  int negative;
  if (!format_number(value, d.digits, d.leading_zero, d.grouping, d.decimal, d.thousand, &body,
                     &negative))
    return 0;
  body.data[body.n] = 0;
  /* '$' symbol, '1' number, '-' sign, other characters literal. */
  static const char *const positive[4] = {"$1", "1$", "$ 1", "1 $"};
  static const char *const negatives[16] = {"($1)", "-$1", "$-1", "$1-", "(1$)", "-1$",
                                            "1-$",  "1$-", "-1 $", "-$ 1", "1 $-", "$ 1-",
                                            "$ -1", "1- $", "($ 1)", "(1 $)"};
  const char *form = negative ? negatives[d.negative] : positive[d.positive];
  char one[2] = {0, 0};
  for (; *form; form++) {
    if (*form == '$')
      put(&t, d.symbol);
    else if (*form == '1')
      put(&t, body.data);
    else {
      one[0] = *form;
      put(&t, one);
    }
  }
  return finish(&t, out, cap);
}
/* Unicode entry points: the picture/value strings convert to ACP 1252,
 * the formatted text widens back with the same character count. */
static int widen_result(int n, const char *a, WCHAR *out, int cap) {
  if (n > 0 && cap)
    k32_widen(a, out, (DWORD)n);
  return n;
}
#define WIDE_TEXT(w, a)                                                        \
  char a##_buffer[256], *a = 0;                                                \
  if (w && !k32_narrow(w, a = a##_buffer, sizeof(a##_buffer), 1113))
static int wide_cap(int cap, WCHAR *out) {
  if (cap < 0 || (cap && !out)) {
    SetLastError(87);
    return -1;
  }
  return cap > 256 ? 256 : cap;
}
int W32API GetDateFormatW(DWORD lcid, DWORD flags, const SYSTEMTIME *date, const WCHAR *picture,
                          WCHAR *out, int cap) {
  int c = wide_cap(cap, out);
  if (c < 0)
    return 0;
  WIDE_TEXT(picture, p) return 0;
  char a[256];
  return widen_result(GetDateFormatA(lcid, flags, date, p, c ? a : 0, c), a, out, c);
}
int W32API GetTimeFormatW(DWORD lcid, DWORD flags, const SYSTEMTIME *time, const WCHAR *picture,
                          WCHAR *out, int cap) {
  int c = wide_cap(cap, out);
  if (c < 0)
    return 0;
  WIDE_TEXT(picture, p) return 0;
  char a[256];
  return widen_result(GetTimeFormatA(lcid, flags, time, p, c ? a : 0, c), a, out, c);
}
typedef struct {
  DWORD digits, leading_zero, grouping;
  const WCHAR *decimal, *thousand;
  DWORD negative;
} NUMBERFMTW;
typedef struct {
  DWORD digits, leading_zero, grouping;
  const WCHAR *decimal, *thousand;
  DWORD negative, positive;
  const WCHAR *symbol;
} CURRENCYFMTW;
int W32API GetNumberFormatW(DWORD lcid, DWORD flags, const WCHAR *value, const NUMBERFMTW *fmt,
                            WCHAR *out, int cap) {
  int c = wide_cap(cap, out);
  if (c < 0)
    return 0;
  WIDE_TEXT(value, v) return 0;
  NUMBERFMTA f, *pf = 0;
  char dec[8], thou[8];
  if (fmt) {
    if (!fmt->decimal || !fmt->thousand || !k32_narrow(fmt->decimal, dec, sizeof(dec), 87) ||
        !k32_narrow(fmt->thousand, thou, sizeof(thou), 87)) {
      SetLastError(87);
      return 0;
    }
    f = (NUMBERFMTA){fmt->digits, fmt->leading_zero, fmt->grouping, dec, thou, fmt->negative};
    pf = &f;
  }
  char a[256];
  return widen_result(GetNumberFormatA(lcid, flags, v, pf, c ? a : 0, c), a, out, c);
}
int W32API GetCurrencyFormatW(DWORD lcid, DWORD flags, const WCHAR *value,
                              const CURRENCYFMTW *fmt, WCHAR *out, int cap) {
  int c = wide_cap(cap, out);
  if (c < 0)
    return 0;
  WIDE_TEXT(value, v) return 0;
  CURRENCYFMTA f, *pf = 0;
  char dec[8], thou[8], sym[16];
  if (fmt) {
    if (!fmt->decimal || !fmt->thousand || !fmt->symbol ||
        !k32_narrow(fmt->decimal, dec, sizeof(dec), 87) ||
        !k32_narrow(fmt->thousand, thou, sizeof(thou), 87) ||
        !k32_narrow(fmt->symbol, sym, sizeof(sym), 87)) {
      SetLastError(87);
      return 0;
    }
    f = (CURRENCYFMTA){fmt->digits, fmt->leading_zero, fmt->grouping, dec, thou,
                       fmt->negative, fmt->positive, sym};
    pf = &f;
  }
  char a[256];
  return widen_result(GetCurrencyFormatA(lcid, flags, v, pf, c ? a : 0, c), a, out, c);
}
