/* FormatMessageA/W (f5-20A): message strings, RT_MESSAGETABLE resources of a
 * module and the system table (English messages of the common Win32 error
 * codes and COM HRESULTs). The formatter works in UTF-16; the ANSI entry
 * point converts its template, its narrow string inserts and its result
 * through ACP 1252. Inserts %1..%99 take an optional !printf! format (no
 * floating point, as documented); %0, %n, %r, %t, %%, "% ", %. and %! are
 * the escapes. Output is limited to 64 KiB characters.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#define ALLOCATE 0x100u
#define IGNORE 0x200u
#define FROM_STRING 0x400u
#define FROM_MODULE 0x800u
#define FROM_SYSTEM 0x1000u
#define ARRAY 0x2000u
#define LIMIT 65536u
extern HANDLE W32API LocalAlloc(DWORD, DWORD);
extern HANDLE k32_find_resource(HANDLE, const void *, const void *, DWORD, int);
extern HANDLE W32API LoadResource(HANDLE, HANDLE);
extern DWORD W32API SizeofResource(HANDLE, HANDLE);
static const struct {
  DWORD id;
  const char *text;
} system_messages[] = {
    {0, "The operation completed successfully."},
    {1, "Incorrect function."},
    {2, "The system cannot find the file specified."},
    {3, "The system cannot find the path specified."},
    {4, "The system cannot open the file."},
    {5, "Access is denied."},
    {6, "The handle is invalid."},
    {8, "Not enough storage is available to process this command."},
    {13, "The data is invalid."},
    {14, "Not enough storage is available to complete this operation."},
    {15, "The system cannot find the drive specified."},
    {16, "The directory cannot be removed."},
    {17, "The system cannot move the file to a different disk drive."},
    {18, "There are no more files."},
    {19, "The media is write protected."},
    {21, "The device is not ready."},
    {24, "The program issued a command but the command length is incorrect."},
    {29, "The system cannot write to the specified device."},
    {30, "The system cannot read from the specified device."},
    {31, "A device attached to the system is not functioning."},
    {32, "The process cannot access the file because it is being used by another process."},
    {33, "The process cannot access the file because another process has locked a portion of "
         "the file."},
    {38, "Reached the end of the file."},
    {39, "The disk is full."},
    {50, "The network request is not supported."},
    {53, "The network path was not found."},
    {80, "The file exists."},
    {82, "The directory or file cannot be created."},
    {87, "The parameter is incorrect."},
    {110, "The system cannot open the device or file specified."},
    {111, "The file name is too long."},
    {112, "There is not enough space on the disk."},
    {120, "This function is not supported on this system."},
    {122, "The data area passed to a system call is too small."},
    {123, "The filename, directory name, or volume label syntax is incorrect."},
    {126, "The specified module could not be found."},
    {127, "The specified procedure could not be found."},
    {145, "The directory is not empty."},
    {170, "The requested resource is in use."},
    {183, "Cannot create a file when that file already exists."},
    {193, "%1 is not a valid Win32 application."},
    {203, "The system could not find the environment option that was entered."},
    {206, "The filename or extension is too long."},
    {234, "More data is available."},
    {259, "No more data is available."},
    {267, "The directory name is invalid."},
    {317, "The system cannot find message text for message number 0x%1 in the message file "
          "for %2."},
    {998, "Invalid access to memory location."},
    {1004, "Invalid flags."},
    {1010, "The configuration registry key is invalid."},
    {1011, "The configuration registry key could not be opened."},
    {1012, "The configuration registry key could not be read."},
    {1013, "The configuration registry key could not be written."},
    {1018, "Illegal operation attempted on a registry key that has been marked for deletion."},
    {1062, "The service has not been started."},
    {1113, "No mapping for the Unicode character exists in the target multi-byte code page."},
    {1114, "A dynamic link library (DLL) initialization routine failed."},
    {1223, "The operation was canceled by the user."},
    {1400, "Invalid window handle."},
    {1812, "The specified image file did not contain a resource section."},
    {1813, "The specified resource type cannot be found in the image file."},
    {1814, "The specified resource name cannot be found in the image file."},
    {1815, "The specified resource language ID cannot be found in the image file."},
    {0x80004001u, "Not implemented"},
    {0x80004002u, "No such interface supported"},
    {0x80004003u, "Invalid pointer"},
    {0x80004004u, "Operation aborted"},
    {0x80004005u, "Unspecified error"},
    {0x8000ffffu, "Catastrophic failure"},
};
struct sink {
  WCHAR *p;
  DWORD n, cap, column, width;
  int overflow;
};
static void emit(struct sink *s, WCHAR c) {
  if (s->n + 1 >= s->cap) {
    s->overflow = 1;
    return;
  }
  s->p[s->n++] = c;
}
static WCHAR acp(uint8_t c) {
  WCHAR w = c;
  MultiByteToWideChar(1252, 0, (const char *)&c, 1, &w, 1);
  return w;
}
/* Argument slot `i` (0-based) of the array or of an i386 va_list. */
static uintptr_t slot(const uintptr_t *args, DWORD i) { return args[i]; }
static void put_unsigned(char *buffer, uint64_t v, unsigned base, int upper) {
  char digits[24];
  unsigned n = 0;
  do {
    unsigned d = (unsigned)(v % base);
    digits[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
    v /= base;
  } while (v);
  for (unsigned i = 0; i < n; i++)
    buffer[i] = digits[n - 1 - i];
  buffer[n] = 0;
}
/* One printf conversion: [-+ #0][width|*][.precision|*][h|l|w|I64]type. */
static int insert(struct sink *s, const WCHAR *spec, DWORD length, const uintptr_t *args,
                  DWORD *next, DWORD count, int wide) {
  int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
  DWORD i = 0;
  for (; i < length; i++) {
    WCHAR c = spec[i];
    if (c == '-')
      left = 1;
    else if (c == '+')
      plus = 1;
    else if (c == ' ')
      space = 1;
    else if (c == '#')
      alt = 1;
    else if (c == '0')
      zero = 1;
    else
      break;
  }
  DWORD width = 0, precision = ~0u;
  if (i < length && spec[i] == '*') {
    if (*next >= count)
      return 0;
    width = (DWORD)slot(args, (*next)++);
    i++;
  } else
    while (i < length && spec[i] >= '0' && spec[i] <= '9')
      width = width * 10 + (spec[i++] - '0');
  if (i < length && spec[i] == '.') {
    i++;
    precision = 0;
    if (i < length && spec[i] == '*') {
      if (*next >= count)
        return 0;
      precision = (DWORD)slot(args, (*next)++);
      i++;
    } else
      while (i < length && spec[i] >= '0' && spec[i] <= '9')
        precision = precision * 10 + (spec[i++] - '0');
  }
  int size = 0; /* 1 narrow, 2 wide, 3 64-bit */
  if (i < length && spec[i] == 'h')
    size = 1, i++;
  else if (i < length && (spec[i] == 'l' || spec[i] == 'w'))
    size = 2, i++;
  else if (i + 2 < length && spec[i] == 'I' && spec[i + 1] == '6' && spec[i + 2] == '4')
    size = 3, i += 3;
  if (i + 1 != length || *next >= count)
    return 0;
  WCHAR type = spec[i];
  uintptr_t value = slot(args, (*next)++);
  uint64_t number = value;
  if (size == 3 && sizeof(uintptr_t) == 4) {
    if (*next >= count)
      return 0;
    number |= (uint64_t)slot(args, (*next)++) << 32;
  }
  WCHAR text[72];
  DWORD n = 0;
  const void *string = 0;
  int string_wide = 0;
  char digits[24];
  const char *prefix = "";
  switch (type) {
  case 's':
  case 'S':
    string = (const void *)value;
    string_wide = size == 2 ? 1 : size == 1 ? 0 : (type == 's') == !!wide;
    break;
  case 'c':
  case 'C': {
    int w = size == 2 ? 1 : size == 1 ? 0 : (type == 'c') == !!wide;
    text[n++] = w ? (WCHAR)value : acp((uint8_t)value);
    break;
  }
  case 'd':
  case 'i': {
    int64_t v = size == 3 ? (int64_t)number : size == 1 ? (int16_t)value : (int32_t)value;
    put_unsigned(digits, v < 0 ? 0 - (uint64_t)v : (uint64_t)v, 10, 0);
    prefix = v < 0 ? "-" : plus ? "+" : space ? " " : "";
    break;
  }
  case 'u':
    put_unsigned(digits, size == 3 ? number : size == 1 ? (uint16_t)value : (uint32_t)value,
                 10, 0);
    break;
  case 'x':
  case 'X':
  case 'o':
  case 'p': {
    uint64_t v = size == 3 ? number : size == 1 ? (uint16_t)value : (uint32_t)value;
    if (type == 'p')
      v = value;
    put_unsigned(digits, v, type == 'o' ? 8 : 16, type != 'x');
    if (alt && v)
      prefix = type == 'o' ? "0" : type == 'x' ? "0x" : "0X";
    if (type == 'p' && precision == ~0u)
      precision = sizeof(void *) * 2;
    break;
  }
  default:
    return 0;
  }
  if (type == 'd' || type == 'i' || type == 'u' || type == 'x' || type == 'X' || type == 'o' ||
      type == 'p') {
    DWORD digit_count = w32_strlen(digits), pad = 0;
    if (precision != ~0u && precision > digit_count)
      pad = precision - digit_count;
    for (const char *p = prefix; *p; p++)
      text[n++] = (uint8_t)*p;
    if (zero && !left && precision == ~0u) {
      DWORD body = n + digit_count;
      while (body + pad < width && n + pad < 40)
        pad++;
    }
    for (DWORD k = 0; k < pad && n < 64; k++)
      text[n++] = '0';
    for (DWORD k = 0; k < digit_count; k++)
      text[n++] = (uint8_t)digits[k];
  }
  DWORD total = n;
  if (string) {
    total = 0;
    while (total < precision && (string_wide ? ((const WCHAR *)string)[total]
                                             : ((const char *)string)[total]))
      total++;
  } else if (type == 's' || type == 'S') {
    static const char null_text[] = "(null)";
    string = null_text;
    string_wide = 0;
    total = precision < 6 ? precision : 6;
  }
  for (DWORD k = total; !left && k < width; k++)
    emit(s, ' ');
  for (DWORD k = 0; k < total; k++)
    emit(s, string ? (string_wide ? ((const WCHAR *)string)[k]
                                  : acp(((const uint8_t *)string)[k]))
                   : text[k]);
  for (DWORD k = total; left && k < width; k++)
    emit(s, ' ');
  return 1;
}
static void newline(struct sink *s) {
  emit(s, '\r');
  emit(s, '\n');
  s->column = 0;
}
/* Template walk. `width` 0 keeps template line breaks, 0xff turns them into
 * spaces, otherwise lines are wrapped at the last space before `width`. */
static int render(struct sink *s, const WCHAR *t, DWORD flags, const uintptr_t *args, DWORD count,
                  int wide) {
  DWORD width = flags & 0xff;
  for (DWORD i = 0; t[i];) {
    WCHAR c = t[i];
    if (c == '%') {
      WCHAR d = t[i + 1];
      if (d >= '1' && d <= '9') {
        DWORD index = d - '0', j = i + 2;
        if (t[j] >= '0' && t[j] <= '9')
          index = index * 10 + (t[j++] - '0');
        DWORD end = j;
        if (t[j] == '!') {
          end = j + 1;
          while (t[end] && t[end] != '!')
            end++;
          if (!t[end]) {
            SetLastError(87);
            return 0;
          }
          end++;
        }
        if (flags & IGNORE) {
          for (DWORD k = i; k < end; k++)
            emit(s, t[k]);
        } else {
          static const WCHAR plain[1] = {'s'};
          DWORD next = index - 1;
          if (!args || next >= count ||
              !insert(s, end > j ? t + j + 1 : plain, end > j ? end - j - 2 : 1, args, &next,
                      count, wide)) {
            SetLastError(87);
            return 0;
          }
        }
        i = end;
        continue;
      }
      i += 2;
      if (d == '0')
        return 1;
      if (d == 'n')
        newline(s);
      else if (d == 'r')
        emit(s, '\r');
      else if (d == 't')
        emit(s, '\t');
      else if (d == ' ' || d == '.' || d == '!' || d == '%')
        emit(s, d);
      else if (!d) {
        emit(s, '%');
        i--;
      } else {
        emit(s, '%');
        emit(s, d);
      }
      continue;
    }
    if ((c == '\r' || c == '\n') && width) {
      i += c == '\r' && t[i + 1] == '\n' ? 2 : 1;
      if (width == 0xff)
        emit(s, ' ');
      else
        newline(s);
      continue;
    }
    emit(s, c);
    s->column = c == '\n' ? 0 : s->column + 1;
    i++;
    if (width && width != 0xff && s->column >= width) {
      DWORD back = s->n;
      while (back && s->p[back - 1] != ' ' && s->n - back < width)
        back--;
      if (back && s->p[back - 1] == ' ' && s->n + 1 < s->cap) {
        DWORD tail = s->n - back;
        w32_copy(s->p + back + 1, s->p + back, tail * 2);
        s->p[back - 1] = '\r';
        s->p[back] = '\n';
        s->n++;
        s->column = tail;
      } else
        newline(s);
    }
  }
  return 1;
}
/* Message-table lookup: MESSAGE_RESOURCE_DATA blocks and entries (Flags 1
 * marks UTF-16 text). Returns an allocated UTF-16 copy. */
static WCHAR *module_message(HANDLE module, DWORD id, DWORD lang, DWORD *bytes) {
  if (!module)
    module = NtCurrentTeb()->ProcessEnvironmentBlock->ImageBaseAddress;
  HANDLE r = k32_find_resource(module, (const void *)(uintptr_t)1, (const void *)(uintptr_t)11,
                              lang & 0xffff, 1);
  const uint8_t *data = r ? LoadResource(module, r) : 0;
  DWORD size = r ? SizeofResource(module, r) : 0;
  if (!data || size < 4) {
    SetLastError(317);
    return 0;
  }
  DWORD blocks = k32_u32(data);
  for (DWORD b = 0; b < blocks && b < (size - 4) / 12; b++) {
    const uint8_t *block = data + 4 + b * 12;
    DWORD low = k32_u32(block), high = k32_u32(block + 4), at = k32_u32(block + 8);
    if (id < low || id > high)
      continue;
    for (DWORD k = low; at <= size - 4; k++) {
      DWORD length = k32_u16(data + at), unicode = k32_u16(data + at + 2) & 1;
      if (length < 4 || length > size - at)
        break;
      if (k == id) {
        DWORD chars = unicode ? (length - 4) / 2 : length - 4;
        WCHAR *w = k32_alloc((chars + 1) * 2);
        if (!w) {
          SetLastError(8);
          return 0;
        }
        for (DWORD c = 0; c < chars; c++)
          w[c] = unicode ? k32_u16(data + at + 4 + c * 2) : acp(data[at + 4 + c]);
        w[chars] = 0;
        DWORD n = 0;
        while (w[n])
          n++;
        w[n] = 0;
        *bytes = (chars + 1) * 2;
        return w;
      }
      at += length;
    }
  }
  SetLastError(317);
  return 0;
}
static WCHAR *system_message(DWORD id, DWORD lang, DWORD *bytes) {
  DWORD primary = lang & 0x3ff;
  if (lang && lang != 0x400 && lang != 0x800 && primary != 9) {
    SetLastError(15100);
    return 0;
  }
  if ((id & 0xffff0000u) == 0x80070000u)
    id &= 0xffff;
  for (unsigned i = 0; i < sizeof(system_messages) / sizeof(*system_messages); i++)
    if (system_messages[i].id == id) {
      DWORD n = w32_strlen(system_messages[i].text);
      WCHAR *w = k32_alloc((n + 3) * 2);
      if (!w) {
        SetLastError(8);
        return 0;
      }
      k32_widen(system_messages[i].text, w, n);
      w[n] = '\r';
      w[n + 1] = '\n';
      w[n + 2] = 0;
      *bytes = (n + 3) * 2;
      return w;
    }
  SetLastError(317);
  return 0;
}
/* Shared driver: returns the formatted UTF-16 text in an allocation of
 * LIMIT characters, or 0 with the last error set. */
static WCHAR *format(DWORD flags, const void *source, DWORD id, DWORD lang, void *arguments,
                     int wide, DWORD *length) {
  WCHAR *template_text = 0;
  DWORD template_bytes = 0;
  if ((flags & FROM_STRING) && (flags & (FROM_MODULE | FROM_SYSTEM))) {
    SetLastError(87);
    return 0;
  }
  if (flags & FROM_STRING) {
    if (!source) {
      SetLastError(87);
      return 0;
    }
    DWORD n = 0;
    if (wide)
      while (((const WCHAR *)source)[n])
        n++;
    else
      n = w32_strlen(source);
    template_bytes = (n + 1) * 2;
    template_text = k32_alloc(template_bytes);
    if (!template_text) {
      SetLastError(8);
      return 0;
    }
    if (wide)
      w32_copy(template_text, source, template_bytes);
    else {
      k32_widen(source, template_text, n);
      template_text[n] = 0;
    }
  } else {
    if (flags & FROM_MODULE)
      template_text = module_message((HANDLE)source, id, lang, &template_bytes);
    if (!template_text && (flags & FROM_SYSTEM))
      template_text = system_message(id, lang, &template_bytes);
    if (!template_text) {
      if (!(flags & (FROM_MODULE | FROM_SYSTEM)))
        SetLastError(87);
      return 0;
    }
  }
  /* Insert numbers address pointer-sized slots: the DWORD_PTR array, or
   * the i386 va_list (the arguments as they lie on the stack). */
  const uintptr_t *args = !arguments ? 0
                          : (flags & ARRAY) ? (const uintptr_t *)arguments
                                            : *(const uintptr_t *const *)arguments;
  struct sink s = {.p = k32_alloc(LIMIT * 2), .cap = LIMIT};
  int ok = s.p && render(&s, template_text, flags, args, 99, wide);
  if (!s.p)
    SetLastError(8);
  k32_free(template_text, template_bytes);
  if (ok && s.overflow) {
    SetLastError(122);
    ok = 0;
  }
  if (!ok) {
    k32_free(s.p, LIMIT * 2);
    return 0;
  }
  s.p[s.n] = 0;
  *length = s.n;
  return s.p;
}
static DWORD deliver(DWORD flags, const WCHAR *text, DWORD n, void *buffer, DWORD size,
                     int wide) {
  DWORD unit = wide ? 2 : 1;
  if (!buffer) {
    SetLastError(87);
    return 0;
  }
  void *out = buffer;
  if (flags & ALLOCATE) {
    DWORD chars = n + 1 > size ? n + 1 : size;
    out = LocalAlloc(0x40, chars * unit);
    if (!out) {
      SetLastError(8);
      return 0;
    }
    *(void **)buffer = out;
  } else if (n + 1 > size) {
    SetLastError(122);
    return 0;
  }
  if (wide)
    w32_copy(out, text, (n + 1) * 2);
  else
    WideCharToMultiByte(1252, 0, text, (int)n + 1, out, (int)n + 1, 0, 0);
  return n;
}
static DWORD format_message(DWORD flags, const void *source, DWORD id, DWORD lang, void *buffer,
                            DWORD size, void *arguments, int wide) {
  DWORD n = 0;
  WCHAR *text = format(flags, source, id, lang, arguments, wide, &n);
  if (!text)
    return 0;
  DWORD r = deliver(flags, text, n, buffer, size, wide);
  k32_free(text, LIMIT * 2);
  return r;
}
DWORD W32API FormatMessageA(DWORD flags, const void *source, DWORD id, DWORD lang, char *buffer,
                            DWORD size, void *arguments) {
  return format_message(flags, source, id, lang, buffer, size, arguments, 0);
}
DWORD W32API FormatMessageW(DWORD flags, const void *source, DWORD id, DWORD lang, WCHAR *buffer,
                            DWORD size, void *arguments) {
  return format_message(flags, source, id, lang, buffer, size, arguments, 1);
}
