/* Shared native spool producer and scalar printer DCs. No Windows driver DLLs.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../apps/libprint/print.h"
#include "internal.h"
#include <w32/printing.h>
#if defined(_WIN32) && !defined(W32_HOST_MODEL)
#define IMPORT __declspec(dllimport)
#else
#define IMPORT extern
#endif
IMPORT void *W32API CreateFileA(const char *, DWORD, DWORD, void *, DWORD,
                                DWORD, void *);
IMPORT BOOL W32API ReadFile(void *, void *, DWORD, DWORD *, void *);
IMPORT BOOL W32API WriteFile(void *, const void *, DWORD, DWORD *, void *);
IMPORT BOOL W32API CloseHandle(void *);
IMPORT BOOL W32API FlushFileBuffers(void *);
IMPORT BOOL W32API CreateDirectoryA(const char *, void *);
IMPORT BOOL W32API RemoveDirectoryA(const char *);
IMPORT BOOL W32API DeleteFileA(const char *);
IMPORT BOOL W32API MoveFileA(const char *, const char *);
IMPORT DWORD W32API GetLastError(void);
IMPORT DWORD W32API GetCurrentProcessId(void);
IMPORT DWORD W32API GetFullPathNameA(const char *, DWORD, char *, char **);
#define BAD_FILE ((void *)(intptr_t)-1)
#define HANDLES 16u
struct printer_handle {
  unsigned used, generation, index, page, failed;
  struct cp_printer printer;
  struct cp_job job;
  void *file;
};
struct printer_dc {
  void *handle;
  unsigned width, height, pitch, size, page, document;
  char output[CP_PATH];
  char *text;
  unsigned columns, rows;
};
static struct printer_handle handles[HANDLES];
static int local_copy(char *out, unsigned cap, const char *in) {
  unsigned n = w32_strlen(in);
  if (n >= cap)
    return 0;
  w32_copy(out, in, n + 1);
  return 1;
}
static int fail(unsigned error) {
  SetLastError(error);
  return 0;
}
static int narrow(char *out, unsigned cap, const WCHAR *in) {
  unsigned i = 0;
  if (in)
    for (; i + 1 < cap && in[i]; i++) {
      int c = gdi_cp1252_ansi(in[i]);
      if (c < 0)
        return fail(1113);
      out[i] = (char)c;
    }
  if (in && in[i])
    return fail(206);
  out[i] = 0;
  return 1;
}
static int copy_name(char *out, unsigned cap, const char *in) {
  if (!in)
    in = "";
  return local_copy(out, cap, in) ? 1 : fail(206);
}
static int output_path(char out[CP_PATH], const char *input) {
  out[0] = 0;
  if (!input || !*input)
    return 1;
  char full[260];
  DWORD n = GetFullPathNameA(input, sizeof(full), full, 0);
  if (!n)
    return 0;
  if (n >= sizeof(full))
    return fail(206);
  if (n < 3 || full[1] != ':' || (full[2] != '/' && full[2] != '\\'))
    return fail(123);
  unsigned at = 0;
  char drive = full[0];
  if (drive >= 'a' && drive <= 'z')
    drive -= 32;
  if (drive < 'A' || drive > 'Z')
    return fail(15);
  if (drive != 'C') {
    local_copy(out, CP_PATH, "/mnt/X");
    out[5] = drive;
    at = 6;
  }
  for (unsigned i = 2; i < n; i++) {
    DWORD ch = gdi_cp1252_unicode((uint8_t)full[i]);
    if (ch == '\\')
      ch = '/';
    unsigned count = ch < 128 ? 1 : ch < 2048 ? 2 : 3;
    if (at + count >= CP_PATH)
      return fail(206);
    if (count == 1)
      out[at++] = (char)ch;
    else if (count == 2) {
      out[at++] = (char)(0xc0 | (ch >> 6));
      out[at++] = (char)(0x80 | (ch & 63));
    } else {
      out[at++] = (char)(0xe0 | (ch >> 12));
      out[at++] = (char)(0x80 | ((ch >> 6) & 63));
      out[at++] = (char)(0x80 | (ch & 63));
    }
  }
  out[at] = 0;
  return 1;
}
static int io_write(void *file, const void *p, DWORD n) {
  DWORD put = 0;
  return WriteFile(file, p, n, &put, 0) && put == n;
}
static int read_record(const char *path, void *out, DWORD size) {
  void *f = CreateFileA(path, 0x80000000u, 3, 0, 3, 0, 0);
  if (f == BAD_FILE)
    return 0;
  DWORD got = 0;
  int ok = ReadFile(f, out, size, &got, 0) && got == size;
  CloseHandle(f);
  return ok;
}
static struct cp_config *config_load(void) {
  struct cp_config *c = gdi_alloc(sizeof(*c));
  if (!c) {
    fail(8);
    return 0;
  }
  int loaded = read_record(CP_ROOT "/printers", c, sizeof(*c));
  if (!loaded && GetLastError() == 2 &&
      read_record(CP_ROOT "/printers.bak", c, sizeof(*c))) {
    /* Native record replacement keeps the previous complete configuration. */
    if (!read_record(CP_ROOT "/printers.tmp", c, sizeof(*c)))
      read_record(CP_ROOT "/printers.bak", c, sizeof(*c));
    loaded = 1;
  }
  if (!loaded || c->magic != CP_CONFIG_MAGIC || c->version != 1 ||
      c->default_printer >= CP_PRINTERS) {
    gdi_free(c, sizeof(*c));
    fail(1801);
    return 0;
  }
  for (unsigned i = 0; i < CP_PRINTERS; i++) {
    struct cp_printer *p = &c->printers[i];
    if (!p->present)
      continue;
    unsigned width = p->page.landscape ? p->page.height : p->page.width;
    unsigned height = p->page.landscape ? p->page.width : p->page.height;
    if (p->driver > CP_PCL || p->name[63] || p->port[CP_PATH - 1] ||
        p->page.width < 144 || p->page.height < 144 || p->page.width > 2448 ||
        p->page.height > 2448 || p->page.landscape > 1 || !p->page.copies ||
        p->page.copies > 99 || p->page.left >= width ||
        p->page.right >= width - p->page.left || p->page.top >= height ||
        p->page.bottom >= height - p->page.top ||
        (p->page.last && p->page.last < p->page.first)) {
      gdi_free(c, sizeof(*c));
      fail(13);
      return 0;
    }
  }
  return c;
}
static int select_printer(const struct cp_config *c, const char *name) {
  if (!name || !*name)
    return c->printers[c->default_printer].present ? (int)c->default_printer
                                                   : -1;
  for (unsigned i = 0; i < CP_PRINTERS; i++)
    if (c->printers[i].present && !w32_stricmp(name, c->printers[i].name))
      return (int)i;
  return -1;
}
static void job_path(char *out, unsigned id, const char *leaf) {
  char base[] = CP_ROOT "/j000/";
  unsigned at = sizeof(CP_ROOT "/j") - 1;
  base[at] = (char)('0' + id / 100);
  base[at + 1] = (char)('0' + id / 10 % 10);
  base[at + 2] = (char)('0' + id % 10);
  local_copy(out, 300, base);
  if (!leaf)
    out[sizeof(base) - 2] = 0;
  else
    local_copy(out + sizeof(base) - 1, 300 - (sizeof(base) - 1), leaf);
}
static struct printer_handle *lookup(void *h) {
  uintptr_t v = (uintptr_t)h;
  unsigned i = (unsigned)(v & 31u);
  if (!i || i > HANDLES || !handles[i - 1].used ||
      (v >> 5) != handles[i - 1].generation) {
    fail(6);
    return 0;
  }
  return &handles[i - 1];
}
BOOL W32API OpenPrinterA(char *name, void **out, void *defaults) {
  GDI_GUARD;
  (void)defaults;
  if (!out)
    return fail(87);
  *out = 0;
  struct cp_config *c = config_load();
  if (!c)
    return 0;
  int index = select_printer(c, name);
  if (index < 0) {
    gdi_free(c, sizeof(*c));
    return fail(1801);
  }
  for (unsigned i = 0; i < HANDLES; i++)
    if (!handles[i].used) {
      unsigned generation = handles[i].generation + 1;
      if (!generation || generation >= 0x4000000u)
        generation = 1;
      handles[i] = (struct printer_handle){.used = 1,
                                           .generation = generation,
                                           .index = (unsigned)index,
                                           .printer = c->printers[index]};
      *out = (void *)(uintptr_t)((generation << 5) | (i + 1));
      gdi_free(c, sizeof(*c));
      return 1;
    }
  gdi_free(c, sizeof(*c));
  return fail(8);
}
BOOL W32API OpenPrinterW(WCHAR *name, void **out, void *defaults) {
  char n[64];
  if (!narrow(n, sizeof(n), name))
    return 0;
  return OpenPrinterA(n, out, defaults);
}
BOOL W32API AbortPrinter(void *handle) {
  GDI_GUARD;
  struct printer_handle *h = lookup(handle);
  if (!h)
    return 0;
  if (h->file) {
    CloseHandle(h->file);
    h->file = 0;
  }
  if (h->job.id) {
    char path[300];
    job_path(path, h->job.id, "data");
    DeleteFileA(path);
    job_path(path, h->job.id, "job.tmp");
    DeleteFileA(path);
    job_path(path, h->job.id, 0);
    RemoveDirectoryA(path);
  }
  h->job = (struct cp_job){0};
  h->page = h->failed = 0;
  return 1;
}
BOOL W32API ClosePrinter(void *handle) {
  GDI_GUARD;
  struct printer_handle *h = lookup(handle);
  if (!h)
    return 0;
  AbortPrinter(handle);
  h->used = 0;
  return 1;
}
DWORD W32API StartDocPrinterA(void *handle, DWORD level, const void *info) {
  GDI_GUARD;
  struct printer_handle *h = lookup(handle);
  const DOC_INFO_1A *d = info;
  if (!h)
    return 0;
  if (level != 1 || !d || h->job.id)
    return fail(87);
  if (d->pDatatype && w32_stricmp(d->pDatatype, "RAW"))
    return fail(1804);
  struct cp_job job = {.magic = CP_JOB_MAGIC,
                       .version = 1,
                       .printer = h->index,
                       .state = CP_QUEUED,
                       .page = h->printer.page};
  if (!copy_name(job.title, sizeof(job.title),
                 d->pDocName ? d->pDocName : "Document") ||
      !output_path(job.target, d->pOutputFile))
    return 0;
  char path[300];
  for (unsigned id = 1; id <= CP_JOBS; id++) {
    job_path(path, id, 0);
    if (!CreateDirectoryA(path, 0)) {
      DWORD err = GetLastError();
      if (err != 183 && err != 80)
        return 0;
      continue;
    }
    job.id = id;
    job_path(path, id, "data");
    h->file = CreateFileA(path, 0x40000000u, 0, 0, 1, 0, 0);
    h->job = job;
    if (h->file == BAD_FILE) {
      h->file = 0;
      AbortPrinter(handle);
      return 0;
    }
    return id;
  }
  return fail(61);
}
DWORD W32API StartDocPrinterW(void *h, DWORD level, const void *info) {
  const DOC_INFO_1W *d = info;
  if (!d)
    return fail(87);
  char title[CP_TITLE], output[CP_PATH], type[32];
  if (!narrow(title, sizeof(title), d->pDocName) ||
      !narrow(output, sizeof(output), d->pOutputFile) ||
      !narrow(type, sizeof(type), d->pDatatype))
    return 0;
  DOC_INFO_1A a = {d->pDocName ? title : 0, d->pOutputFile ? output : 0,
                   d->pDatatype ? type : 0};
  return StartDocPrinterA(h, level, &a);
}
BOOL W32API StartPagePrinter(void *handle) {
  GDI_GUARD;
  struct printer_handle *h = lookup(handle);
  if (!h)
    return 0;
  if (!h->file || h->page || h->failed)
    return fail(1906);
  h->page = 1;
  return 1;
}
BOOL W32API EndPagePrinter(void *handle) {
  GDI_GUARD;
  struct printer_handle *h = lookup(handle);
  if (!h)
    return 0;
  if (!h->page || h->failed)
    return fail(1906);
  h->page = 0;
  return 1;
}
BOOL W32API WritePrinter(void *handle, const void *data, DWORD bytes,
                         DWORD *written) {
  GDI_GUARD;
  if (written)
    *written = 0;
  struct printer_handle *h = lookup(handle);
  if (!h)
    return 0;
  if (!written || (!data && bytes) || !h->file || h->failed)
    return fail(87);
  if (bytes > CP_MAX_BYTES - h->job.bytes) {
    h->failed = 1;
    return fail(112);
  }
  DWORD put = 0;
  int ok = WriteFile(h->file, data, bytes, &put, 0);
  h->job.bytes += put;
  *written = put;
  if (!ok || put != bytes) {
    h->failed = 1;
    return fail(29);
  }
  return 1;
}
BOOL W32API EndDocPrinter(void *handle) {
  GDI_GUARD;
  struct printer_handle *h = lookup(handle);
  if (!h)
    return 0;
  if (!h->file || h->page || h->failed)
    return fail(1906);
  if (!FlushFileBuffers(h->file)) {
    h->failed = 1;
    return 0;
  }
  if (!CloseHandle(h->file)) {
    h->file = 0;
    h->failed = 1;
    return 0;
  }
  h->file = 0;
  char tmp[300], final[300];
  job_path(tmp, h->job.id, "job.tmp");
  job_path(final, h->job.id, "job");
  void *f = CreateFileA(tmp, 0x40000000u, 0, 0, 1, 0, 0);
  if (f == BAD_FILE) {
    h->failed = 1;
    return 0;
  }
  int ok = io_write(f, &h->job, sizeof(h->job)) && FlushFileBuffers(f);
  if (!CloseHandle(f))
    ok = 0;
  if (!ok || !MoveFileA(tmp, final)) {
    h->failed = 1;
    return 0;
  }
  h->job = (struct cp_job){0};
  return 1;
}
static DWORD str_bytes(const char *s, int wide) {
  return (w32_strlen(s) + 1) * (wide ? 2u : 1u);
}
static void *pack_string(uint8_t **next, const char *s, int wide) {
  uint8_t *out = *next;
  if (wide)
    for (unsigned i = 0;; i++) {
      WCHAR c = (WCHAR)gdi_cp1252_unicode((uint8_t)s[i]);
      w32_put16(*next, c);
      *next += 2;
      if (!s[i])
        break;
    }
  else {
    DWORD n = str_bytes(s, 0);
    w32_copy(*next, s, n);
    *next += n;
  }
  return out;
}
static DWORD record_size(DWORD level) {
  return level == 2   ? sizeof(PRINTER_INFO_2A)
         : level == 1 ? sizeof(PRINTER_INFO_1A)
         : level == 4 ? sizeof(PRINTER_INFO_4A)
         : level == 5 ? sizeof(PRINTER_INFO_5A)
                      : 0;
}
static DWORD printer_bytes(const struct cp_printer *p, DWORD level, int wide) {
  return record_size(level) +
         (level == 2 ? (str_bytes("", wide) * 5 + str_bytes(p->port, wide) +
                        str_bytes("CiukiOS native", wide) +
                        str_bytes("winprint", wide) + str_bytes("RAW", wide))
                     : 0) +
         str_bytes(p->name, wide) + str_bytes(level == 5 ? p->port : "", wide) +
         (level == 1 ? str_bytes("CiukiOS native printer", wide) : 0);
}
static unsigned printer_jobs(unsigned index) {
  unsigned count = 0;
  for (unsigned id = 1; id <= CP_JOBS; id++) {
    char path[300];
    struct cp_job job;
    job_path(path, id, "job");
    if (read_record(path, &job, sizeof(job)) && job.magic == CP_JOB_MAGIC &&
        job.version == 1 && job.printer == index)
      count++;
  }
  return count;
}
static void pack_printer(void *out, uint8_t **next, const struct cp_printer *p,
                         DWORD level, int wide, int def, unsigned index) {
  DWORD attrs = PRINTER_ATTRIBUTE_LOCAL | (def ? PRINTER_ATTRIBUTE_DEFAULT : 0);
  void *name = pack_string(next, p->name, wide);
  void *extra = pack_string(next, level == 5 ? p->port : "", wide);
  if (level == 2) {
    PRINTER_INFO_2A *r = out;
    w32_fill(r, 0, sizeof(*r));
    r->pPrinterName = name;
    r->pServerName = extra;
    r->pShareName = pack_string(next, "", wide);
    r->pPortName = pack_string(next, p->port, wide);
    r->pDriverName = pack_string(next, "CiukiOS native", wide);
    r->pComment = pack_string(next, "", wide);
    r->pLocation = pack_string(next, "", wide);
    r->pSepFile = pack_string(next, "", wide);
    r->pPrintProcessor = pack_string(next, "winprint", wide);
    r->pDatatype = pack_string(next, "RAW", wide);
    r->pParameters = pack_string(next, "", wide);
    r->Attributes = attrs;
    r->Priority = r->DefaultPriority = 1;
    r->Status = p->paused ? 1 : 0;
    r->cJobs = printer_jobs(index);
  }
  if (level == 1) {
    PRINTER_INFO_1A *r = out;
    r->Flags = 0;
    r->pName = name;
    r->pComment = extra;
    r->pDescription = pack_string(next, "CiukiOS native printer", wide);
  }
  if (level == 4) {
    PRINTER_INFO_4A *r = out;
    r->pPrinterName = name;
    r->pServerName = extra;
    r->Attributes = attrs;
  }
  if (level == 5) {
    PRINTER_INFO_5A *r = out;
    r->pPrinterName = name;
    r->pPortName = extra;
    r->Attributes = attrs;
    r->DeviceNotSelectedTimeout = 15000;
    r->TransmissionRetryTimeout = 45000;
  }
}
static BOOL enum_printers(DWORD flags, const void *name, DWORD level, void *out,
                          DWORD bytes, DWORD *needed, DWORD *returned,
                          int wide) {
  if (!needed || !returned)
    return fail(87);
  *needed = *returned = 0;
  if (!record_size(level))
    return fail(124);
  if (name || flags & ~(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS |
                        PRINTER_ENUM_DEFAULT))
    return fail(50);
  if (!(flags & (PRINTER_ENUM_LOCAL | PRINTER_ENUM_DEFAULT)))
    return 1;
  struct cp_config *c = config_load();
  if (!c)
    return 0;
  unsigned count = 0;
  for (unsigned i = 0; i < CP_PRINTERS; i++)
    if (c->printers[i].present &&
        (!(flags & PRINTER_ENUM_DEFAULT) || i == c->default_printer)) {
      count++;
      *needed += printer_bytes(&c->printers[i], level, wide);
    }
  if (bytes < *needed || (!out && *needed)) {
    gdi_free(c, sizeof(*c));
    return fail(122);
  }
  uint8_t *next = (uint8_t *)out + record_size(level) * count;
  for (unsigned i = 0; i < CP_PRINTERS; i++)
    if (c->printers[i].present &&
        (!(flags & PRINTER_ENUM_DEFAULT) || i == c->default_printer)) {
      pack_printer((uint8_t *)out + record_size(level) * (*returned), &next,
                   &c->printers[i], level, wide, i == c->default_printer, i);
      (*returned)++;
    }
  gdi_free(c, sizeof(*c));
  return 1;
}
BOOL W32API EnumPrintersA(DWORD f, char *n, DWORD l, void *b, DWORD s, DWORD *z,
                          DWORD *r) {
  GDI_GUARD;
  return enum_printers(f, n, l, b, s, z, r, 0);
}
BOOL W32API EnumPrintersW(DWORD f, WCHAR *n, DWORD l, void *b, DWORD s,
                          DWORD *z, DWORD *r) {
  GDI_GUARD;
  return enum_printers(f, n, l, b, s, z, r, 1);
}
/* EnumPorts lists the native spool queue's output devices, /dev/lpt0 to
 * /dev/lpt2 (the Printers port list), spelled exactly as PRINTER_INFO_2/5
 * report pPortName. Print to file names a path, not a port. Local server
 * only (NULL or empty name), levels 1 and 2, exact required-size reporting.
 * The InstallShield 5 engine imports EnumPortsA. */
#define NATIVE_PORTS 3u
static const char *const native_ports[NATIVE_PORTS] = {"/dev/lpt0", "/dev/lpt1",
                                                       "/dev/lpt2"};
static const char *const port_descriptions[NATIVE_PORTS] = {
    "Printer Port (LPT1)", "Printer Port (LPT2)", "Printer Port (LPT3)"};
static BOOL enum_ports(int remote, DWORD level, void *out, DWORD bytes,
                       DWORD *needed, DWORD *returned, int wide) {
  if (!needed || !returned)
    return fail(87);
  *needed = *returned = 0;
  if (level != 1 && level != 2)
    return fail(124);
  if (remote)
    return fail(50);
  DWORD record = level == 1 ? sizeof(PORT_INFO_1A) : sizeof(PORT_INFO_2A);
  for (unsigned i = 0; i < NATIVE_PORTS; i++)
    *needed += record + str_bytes(native_ports[i], wide) +
               (level == 2 ? str_bytes("CiukiOS native", wide) +
                                 str_bytes(port_descriptions[i], wide)
                           : 0);
  if (bytes < *needed || !out)
    return fail(122);
  uint8_t *next = (uint8_t *)out + record * NATIVE_PORTS;
  for (unsigned i = 0; i < NATIVE_PORTS; i++) {
    void *port = pack_string(&next, native_ports[i], wide);
    if (level == 1) {
      ((PORT_INFO_1A *)out)[i].pName = port;
    } else {
      PORT_INFO_2A *r = (PORT_INFO_2A *)out + i;
      r->pPortName = port;
      r->pMonitorName = pack_string(&next, "CiukiOS native", wide);
      r->pDescription = pack_string(&next, port_descriptions[i], wide);
      r->fPortType = PORT_TYPE_WRITE;
      r->Reserved = 0;
    }
    (*returned)++;
  }
  return 1;
}
BOOL W32API EnumPortsA(char *n, DWORD l, void *b, DWORD s, DWORD *z, DWORD *r) {
  GDI_GUARD;
  return enum_ports(n && *n, l, b, s, z, r, 0);
}
BOOL W32API EnumPortsW(WCHAR *n, DWORD l, void *b, DWORD s, DWORD *z,
                       DWORD *r) {
  GDI_GUARD;
  return enum_ports(n && *n, l, b, s, z, r, 1);
}
static BOOL get_default(void *out, DWORD *size, int wide) {
  if (!size)
    return fail(87);
  struct cp_config *c = config_load();
  if (!c)
    return 0;
  int index = select_printer(c, 0);
  if (index < 0) {
    gdi_free(c, sizeof(*c));
    return fail(1801);
  }
  const char *name = c->printers[index].name;
  DWORD n = w32_strlen(name) + 1, old = *size;
  *size = n;
  if (!out || old < n) {
    gdi_free(c, sizeof(*c));
    return fail(122);
  }
  uint8_t *next = out;
  pack_string(&next, name, wide);
  gdi_free(c, sizeof(*c));
  return 1;
}
BOOL W32API GetDefaultPrinterA(char *out, DWORD *n) {
  GDI_GUARD;
  return get_default(out, n, 0);
}
BOOL W32API GetDefaultPrinterW(WCHAR *out, DWORD *n) {
  GDI_GUARD;
  return get_default(out, n, 1);
}
static int config_lock(void) {
  if (!CreateDirectoryA(CP_ROOT "/config.lock", 0))
    return fail(170);
  DWORD pid = GetCurrentProcessId();
  void *f =
      CreateFileA(CP_ROOT "/config.lock/owner", 0x40000000u, 0, 0, 1, 0, 0);
  int ok = f != BAD_FILE && io_write(f, &pid, sizeof(pid));
  if (f != BAD_FILE && !CloseHandle(f))
    ok = 0;
  if (!ok) {
    DeleteFileA(CP_ROOT "/config.lock/owner");
    RemoveDirectoryA(CP_ROOT "/config.lock");
  }
  return ok;
}
static void config_unlock(void) {
  DeleteFileA(CP_ROOT "/config.lock/owner");
  RemoveDirectoryA(CP_ROOT "/config.lock");
}
static BOOL save_config(struct cp_config *c) {
  void *f = CreateFileA(CP_ROOT "/printers.tmp", 0x40000000u, 0, 0, 2, 0, 0);
  if (f == BAD_FILE)
    return 0;
  int ok = io_write(f, c, sizeof(*c)) && FlushFileBuffers(f);
  if (!CloseHandle(f))
    ok = 0;
  if (!ok) {
    DeleteFileA(CP_ROOT "/printers.tmp");
    return 0;
  }
  DeleteFileA(CP_ROOT "/printers.bak");
  if (!MoveFileA(CP_ROOT "/printers", CP_ROOT "/printers.bak") &&
      GetLastError() != 2)
    return 0;
  if (!MoveFileA(CP_ROOT "/printers.tmp", CP_ROOT "/printers"))
    return 0;
  DeleteFileA(CP_ROOT "/printers.bak");
  return 1;
}
BOOL W32API SetDefaultPrinterA(const char *name) {
  GDI_GUARD;
  if (!config_lock())
    return 0;
  struct cp_config *c = config_load();
  if (!c) {
    config_unlock();
    return 0;
  }
  int index = select_printer(c, name);
  if (index < 0) {
    gdi_free(c, sizeof(*c));
    config_unlock();
    return fail(1801);
  }
  c->default_printer = (unsigned)index;
  int ok = save_config(c);
  gdi_free(c, sizeof(*c));
  config_unlock();
  return ok;
}
BOOL W32API SetDefaultPrinterW(const WCHAR *name) {
  char n[64];
  return narrow(n, sizeof(n), name) ? SetDefaultPrinterA(n) : 0;
}
static BOOL get_printer(void *handle, DWORD level, void *out, DWORD bytes,
                        DWORD *needed, int wide) {
  struct printer_handle *h = lookup(handle);
  if (!h)
    return 0;
  if (!needed)
    return fail(87);
  *needed = 0;
  if (!record_size(level))
    return fail(124);
  struct cp_config *c = config_load();
  if (!c)
    return 0;
  if (!c->printers[h->index].present) {
    gdi_free(c, sizeof(*c));
    return fail(1801);
  }
  const struct cp_printer *p = &c->printers[h->index];
  *needed = printer_bytes(p, level, wide);
  if (!out || bytes < *needed) {
    gdi_free(c, sizeof(*c));
    return fail(122);
  }
  uint8_t *next = (uint8_t *)out + record_size(level);
  pack_printer(out, &next, p, level, wide, h->index == c->default_printer,
               h->index);
  gdi_free(c, sizeof(*c));
  return 1;
}

BOOL W32API GetPrinterA(void *h, DWORD l, void *b, DWORD s, DWORD *n) {
  GDI_GUARD;
  return get_printer(h, l, b, s, n, 0);
}
BOOL W32API GetPrinterW(void *h, DWORD l, void *b, DWORD s, DWORD *n) {
  GDI_GUARD;
  return get_printer(h, l, b, s, n, 1);
}
BOOL W32API SetPrinterA(void *handle, DWORD level, void *data, DWORD command) {
  GDI_GUARD;
  struct printer_handle *h = lookup(handle);
  if (!h)
    return 0;
  if (level || data || command < 1 || command > 3)
    return fail(87);
  if (command == 3) {
    for (unsigned id = 1; id <= CP_JOBS; id++) {
      char path[300];
      struct cp_job job;
      job_path(path, id, "job");
      if (!read_record(path, &job, sizeof(job)) || job.magic != CP_JOB_MAGIC ||
          job.printer != h->index)
        continue;
      job_path(path, id, "ctl");
      void *f = CreateFileA(path, 0x40000000u, 0, 0, 2, 0, 0);
      if (f == BAD_FILE)
        return 0;
      int ok = io_write(f, "c", 1);
      if (!CloseHandle(f))
        ok = 0;
      if (!ok)
        return 0;
    }
    return 1;
  }
  if (!config_lock())
    return 0;
  struct cp_config *c = config_load();
  if (!c) {
    config_unlock();
    return 0;
  }
  c->printers[h->index].paused = command == 1;
  int ok = save_config(c);
  if (ok)
    h->printer = c->printers[h->index];
  gdi_free(c, sizeof(*c));
  config_unlock();
  return ok;
}
BOOL W32API SetPrinterW(void *h, DWORD l, void *d, DWORD c) {
  return SetPrinterA(h, l, d, c);
}
static int device_mode(struct cp_page *page, const void *input, int wide) {
  if (!input)
    return 1;
  const uint8_t *m = input;
  unsigned base = wide ? 68 : 36;
  if (w32_u16(m + base) < base + 24)
    return fail(13);
  DWORD fields = w32_u32(m + base + 4);
  if (fields & 1u) {
    unsigned n = w32_u16(m + base + 8);
    if (n != 1 && n != 2)
      return fail(87);
    page->landscape = n == 2;
  }
  if (fields & 2u) {
    unsigned paper = w32_u16(m + base + 10);
    switch (paper) {
    case 1:
      page->width = 612;
      page->height = 792;
      break;
    case 5:
      page->width = 612;
      page->height = 1008;
      break;
    case 9:
      page->width = 595;
      page->height = 842;
      break;
    default:
      return fail(50);
    }
  }
  if (fields & 4u)
    page->height = (unsigned)w32_u16(m + base + 12) * 72 / 254;
  if (fields & 8u)
    page->width = (unsigned)w32_u16(m + base + 14) * 72 / 254;
  if ((fields & 16u) && w32_u16(m + base + 16) != 100)
    return fail(50);
  if (fields & 0x100u)
    page->copies = w32_u16(m + base + 18);
  if (page->width < 144 || page->height < 144 || page->width > 2448 ||
      page->height > 2448 || !page->copies || page->copies > 99)
    return fail(87);
  return 1;
}
HDC gdi_printer_create(const char *driver, const char *device,
                       const char *output, const void *mode, int wide) {
  if (driver && w32_stricmp(driver, "WINSPOOL") &&
      w32_stricmp(driver, "CiukiOS")) {
    fail(50);
    return 0;
  }
  void *handle = 0;
  if (!OpenPrinterA((char *)device, &handle, 0))
    return 0;
  struct printer_handle *h = lookup(handle);
  if (!device_mode(&h->printer.page, mode, wide)) {
    ClosePrinter(handle);
    return 0;
  }
  struct printer_dc *p = gdi_alloc(sizeof(*p));
  if (!p) {
    ClosePrinter(handle);
    return 0;
  }
  if (!copy_name(p->output, sizeof(p->output), output)) {
    gdi_free(p, sizeof(*p));
    ClosePrinter(handle);
    return 0;
  }
  p->handle = handle;
  p->width = h->printer.page.width * 4 / 3;
  p->height = h->printer.page.height * 4 / 3;
  if (h->printer.page.landscape) {
    unsigned n = p->width;
    p->width = p->height;
    p->height = n;
  }
  p->pitch = (p->width + 7) / 8;
  p->size = p->pitch * p->height;
  if (h->printer.driver == CP_TEXT) {
    p->columns = p->width / 8;
    p->rows = p->height / 16;
    p->text = gdi_alloc(p->columns * p->rows);
    if (!p->text) {
      gdi_free(p, sizeof(*p));
      ClosePrinter(handle);
      return 0;
    }
  }

  uint8_t *bits = gdi_alloc(p->size);
  if (!bits) {
    gdi_free(p->text, p->columns * p->rows);
    gdi_free(p, sizeof(*p));
    ClosePrinter(handle);
    return 0;
  }
  w32_fill(bits, 255, p->size);
  struct w32_dib surface = {.bits = bits,
                            .size = p->size,
                            .width = p->width,
                            .height = p->height,
                            .pitch = p->pitch,
                            .bpp = 1,
                            .top_down = 1,
                            .color_count = 2,
                            .colors = {0, 0xffffff}};
  HDC dc = w32_gdi_surface_dc(&surface, (POINT){0, 0}, 0, 0);
  if (!dc) {
    gdi_free(bits, p->size);
    gdi_free(p->text, p->columns * p->rows);
    gdi_free(p, sizeof(*p));
    ClosePrinter(handle);
    return 0;
  }
  gdi_dc(dc)->printer = p;
  return dc;
}
void gdi_printer_free(struct dc *d) {
  struct printer_dc *p = d->printer;
  if (!p)
    return;
  ClosePrinter(p->handle);
  gdi_free(p->text, p->columns * p->rows);
  gdi_free(d->surface.bits, p->size);
  gdi_free(p, sizeof(*p));
  d->printer = 0;
}
void gdi_printer_exit(void) {
  for (unsigned i = 0; i < HANDLES; i++)
    if (handles[i].used)
      ClosePrinter((void *)(uintptr_t)((handles[i].generation << 5) | (i + 1)));
}
int gdi_printer_caps(struct dc *d, int cap) {
  struct printer_dc *p = d->printer;
  switch (cap) {
  case 0:
    return 0x400;
  case 2:
    return 2;
  case 4:
    return (int)(p->width * 254 / 960);
  case 6:
    return (int)(p->height * 254 / 960);
  case 8:
  case 110:
    return (int)p->width;
  case 10:
  case 111:
    return (int)p->height;
  case 12:
  case 14:
    return 1;
  case 24:
    return 2;
  case 28:
    return 0x3e;
  case 30:
    return 0x1e;
  case 32:
    return 0x7f;
  case 34:
    return 0x1ff;
  case 38:
    return 0x8e99;
  case 88:
  case 90:
    return 96;
  case 112:
  case 113:
    return 0;
  case 114:
  case 115:
    return 100;
  default:
    return 0;
  }
}
static int print_write(struct printer_dc *p, const void *data, unsigned bytes) {
  DWORD put;
  return WritePrinter(p->handle, data, bytes, &put);
}
static int print_string(struct printer_dc *p, const char *s) {
  return print_write(p, s, w32_strlen(s));
}
static int print_number(struct printer_dc *p, unsigned n) {
  char digits[12];
  unsigned i = sizeof(digits);
  do {
    digits[--i] = (char)('0' + n % 10);
    n /= 10;
  } while (n);
  return print_write(p, digits + i, sizeof(digits) - i);
}
int W32API StartDocA(HDC dc, const void *info) {
  GDI_GUARD;
  struct dc *d = gdi_dc(dc);
  const DOCINFOA *doc = info;
  if (!d || !d->printer) {
    fail(6);
    return -1;
  }
  struct printer_dc *p = d->printer;
  if (!doc || doc->cbSize < (int)sizeof(*doc) || doc->fwType || p->document ||
      (doc->lpszDatatype && w32_stricmp(doc->lpszDatatype, "RAW"))) {
    fail(87);
    return -1;
  }
  DOC_INFO_1A raw = {doc->lpszDocName,
                     doc->lpszOutput ? doc->lpszOutput
                                     : (p->output[0] ? p->output : 0),
                     "RAW"};
  DWORD id = StartDocPrinterA(p->handle, 1, &raw);
  if (!id)
    return -1;
  p->document = 1;
  struct printer_handle *h = lookup(p->handle);
  if (h->printer.driver == CP_POSTSCRIPT &&
      !print_string(p, "%!PS-Adobe-3.0\n")) {
    AbortDoc(dc);
    return -1;
  }
  if (h->printer.driver == CP_PCL && !print_string(p, "\033E")) {
    AbortDoc(dc);
    return -1;
  }
  return (int)id;
}
int W32API StartDocW(HDC dc, const void *info) {
  const DOCINFOW *w = info;
  if (!w || w->cbSize < (int)sizeof(*w)) {
    fail(87);
    return -1;
  }
  char title[CP_TITLE], out[CP_PATH], type[32];
  if (!narrow(title, sizeof(title), w->lpszDocName) ||
      !narrow(out, sizeof(out), w->lpszOutput) ||
      !narrow(type, sizeof(type), w->lpszDatatype))
    return -1;
  DOCINFOA a = {sizeof(a), w->lpszDocName ? title : 0, w->lpszOutput ? out : 0,
                w->lpszDatatype ? type : 0, w->fwType};
  return StartDocA(dc, &a);
}
int W32API StartPage(HDC dc) {
  GDI_GUARD;
  struct dc *d = gdi_dc(dc);
  if (!d || !d->printer) {
    fail(6);
    return -1;
  }
  struct printer_dc *p = d->printer;
  if (!p->document || p->page || !StartPagePrinter(p->handle)) {
    fail(1906);
    return -1;
  }
  w32_fill(d->surface.bits, 255, p->size);
  if (p->text)
    w32_fill(p->text, ' ', p->columns * p->rows);
  p->page = 1;
  return 1;
}
int W32API EndPage(HDC dc) {
  GDI_GUARD;
  struct dc *d = gdi_dc(dc);
  if (!d || !d->printer) {
    fail(6);
    return -1;
  }
  struct printer_dc *p = d->printer;
  if (!p->page) {
    fail(1906);
    return -1;
  }
  struct printer_handle *h = lookup(p->handle);
  int ok = 1;
  if (h->printer.driver == CP_TEXT) {
    unsigned last = p->rows;
    while (last) {
      unsigned x;
      for (x = 0; x < p->columns; x++)
        if (p->text[(last - 1) * p->columns + x] != ' ')
          break;
      if (x < p->columns)
        break;
      last--;
    }
    for (unsigned y = 0; ok && y < last; y++) {
      unsigned n = p->columns;
      while (n && p->text[y * p->columns + n - 1] == ' ')
        n--;
      ok = print_write(p, p->text + y * p->columns, n) &&
           print_string(p, "\r\n");
    }
    if (ok)
      ok = print_string(p, "\f");
  } else if (h->printer.driver == CP_POSTSCRIPT) {
    unsigned paper_width = h->printer.page.landscape ? h->printer.page.height
                                                     : h->printer.page.width;
    unsigned paper_height = h->printer.page.landscape ? h->printer.page.width
                                                      : h->printer.page.height;
    ok = print_string(p, "<< /PageSize [") && print_number(p, paper_width) &&
         print_string(p, " ") && print_number(p, paper_height) &&
         print_string(p, "] >> setpagedevice\ngsave\n") &&
         print_number(p, paper_width) && print_string(p, " ") &&
         print_number(p, paper_height) && print_string(p, " scale\n") &&
         print_number(p, p->width) && print_string(p, " ") &&
         print_number(p, p->height) && print_string(p, " 1 [") &&
         print_number(p, p->width) && print_string(p, " 0 0 -") &&
         print_number(p, p->height) && print_string(p, " 0 ") &&
         print_number(p, p->height) && print_string(p, "]\n{currentfile ") &&
         print_number(p, p->pitch) &&
         print_string(p, " string readhexstring pop} image\n");
    static const char hex[] = "0123456789abcdef";
    char row[512];
    unsigned at = 0;
    for (unsigned i = 0; ok && i < p->size; i++) {
      unsigned b = d->surface.bits[i];
      row[at++] = hex[b >> 4];
      row[at++] = hex[b & 15];
      if (at == sizeof(row)) {
        ok = print_write(p, row, at);
        at = 0;
      }
    }
    if (ok && at)
      ok = print_write(p, row, at);
    if (ok)
      ok = print_string(p, "\ngrestore showpage\n");
  } else {
    /* Standard PCL5 raster resolutions include 100 DPI, not GDI's 96.
     * Resample the logical page so its physical dimensions remain constant. */
    unsigned width = (p->width * 100 + 95) / 96,
             height = (p->height * 100 + 95) / 96;
    unsigned pitch = (width + 7) / 8;
    unsigned paper =
        h->printer.page.width == 612 && h->printer.page.height == 792    ? 2
        : h->printer.page.width == 612 && h->printer.page.height == 1008 ? 3
        : h->printer.page.width == 595 && h->printer.page.height == 842  ? 26
                                                                         : 0;
    if (paper)
      ok = print_string(p, "\033&l") && print_number(p, paper) &&
           print_string(p, "A");
    ok = ok && print_string(p, "\033&l") &&
         print_number(p, h->printer.page.landscape ? 1 : 0) &&
         print_string(p, "O");
    ok = ok && print_string(p, "\033*t100R\033*r") && print_number(p, width) &&
         print_string(p, "S\033*r1A\033*b0M");
    uint8_t row[512];
    for (unsigned y = 0; ok && y < height; y++) {
      w32_fill(row, 0, pitch);
      for (unsigned x = 0; x < width; x++)
        if (!w32_dib_raw(&d->surface, (int)(x * 96 / 100), (int)(y * 96 / 100)))
          row[x / 8] |= (uint8_t)(0x80u >> (x & 7));
      ok = print_string(p, "\033*b") && print_number(p, pitch) &&
           print_string(p, "W") && print_write(p, row, pitch);
    }
    if (ok)
      ok = print_string(p, "\033*rB\f");
  }
  if (!ok || !EndPagePrinter(p->handle))
    return -1;
  p->page = 0;
  return 1;
}
int W32API EndDoc(HDC dc) {
  GDI_GUARD;
  struct dc *d = gdi_dc(dc);
  if (!d || !d->printer) {
    fail(6);
    return -1;
  }
  struct printer_dc *p = d->printer;
  if (!p->document || p->page || !EndDocPrinter(p->handle)) {
    fail(1906);
    return -1;
  }
  p->document = 0;
  return 1;
}
int W32API AbortDoc(HDC dc) {
  GDI_GUARD;
  struct dc *d = gdi_dc(dc);
  if (!d || !d->printer) {
    fail(6);
    return -1;
  }
  struct printer_dc *p = d->printer;
  if (!AbortPrinter(p->handle))
    return -1;
  p->document = p->page = 0;
  return 1;
}
static DWORD job_status(unsigned state) {
  return state == CP_PAUSED     ? JOB_STATUS_PAUSED
         : state == CP_PRINTING ? JOB_STATUS_PRINTING
         : state == CP_OFFLINE  ? JOB_STATUS_OFFLINE
         : state == CP_ERROR    ? JOB_STATUS_ERROR
                                : 0;
}
static int get_job(struct printer_handle *h, unsigned id, struct cp_job *job) {
  char path[300];
  if (!id || id > CP_JOBS)
    return 0;
  job_path(path, id, "job");
  return read_record(path, job, sizeof(*job)) && job->magic == CP_JOB_MAGIC &&
         job->version == 1 && job->id == id && job->printer == h->index &&
         !job->title[CP_TITLE - 1];
}
static DWORD job_bytes(struct printer_handle *h, struct cp_job *j, int wide) {
  return sizeof(JOB_INFO_1A) + str_bytes(h->printer.name, wide) +
         str_bytes(j->title, wide) + str_bytes("RAW", wide) +
         str_bytes("", wide) * 3;
}
static BOOL enum_jobs(void *handle, DWORD first, DWORD count, DWORD level,
                      void *out, DWORD bytes, DWORD *needed, DWORD *returned,
                      int wide) {
  struct printer_handle *h = lookup(handle);
  if (!h)
    return 0;
  if (!needed || !returned)
    return fail(87);
  *needed = *returned = 0;
  if (level != 1)
    return fail(124);
  struct cp_job *jobs = gdi_alloc(sizeof(*jobs) * CP_JOBS);
  if (!jobs)
    return fail(8);
  struct cp_job job;
  unsigned position = 0, n = 0;
  for (unsigned id = 1; id <= CP_JOBS; id++)
    if (get_job(h, id, &job)) {
      if (position++ < first)
        continue;
      if (n >= count)
        break;
      jobs[n++] = job;
      *needed += job_bytes(h, &job, wide);
    }
  if (*needed && (!out || bytes < *needed)) {
    gdi_free(jobs, sizeof(*jobs) * CP_JOBS);
    return fail(122);
  }
  if (!n) {
    gdi_free(jobs, sizeof(*jobs) * CP_JOBS);
    return 1;
  }
  uint8_t *next = (uint8_t *)out + sizeof(JOB_INFO_1A) * n;
  for (unsigned i = 0; i < n; i++) {
    struct cp_job *j = &jobs[i];
    JOB_INFO_1A *r = (JOB_INFO_1A *)out + i;
    w32_fill(r, 0, sizeof(*r));
    r->JobId = j->id;
    r->pPrinterName = pack_string(&next, h->printer.name, wide);
    r->pDocument = pack_string(&next, j->title, wide);
    r->pDatatype = pack_string(&next, "RAW", wide);
    r->pMachineName = pack_string(&next, "", wide);
    r->pUserName = pack_string(&next, "", wide);
    r->pStatus = pack_string(&next, "", wide);
    r->Status = job_status(j->state);
    r->Priority = 1;
    r->Position = first + i + 1;
  }
  *returned = n;
  gdi_free(jobs, sizeof(*jobs) * CP_JOBS);
  return 1;
}
BOOL W32API EnumJobsA(void *h, DWORD f, DWORD n, DWORD l, void *b, DWORD s,
                      DWORD *z, DWORD *r) {
  GDI_GUARD;
  return enum_jobs(h, f, n, l, b, s, z, r, 0);
}
BOOL W32API EnumJobsW(void *h, DWORD f, DWORD n, DWORD l, void *b, DWORD s,
                      DWORD *z, DWORD *r) {
  GDI_GUARD;
  return enum_jobs(h, f, n, l, b, s, z, r, 1);
}
BOOL W32API SetJobA(void *handle, DWORD id, DWORD level, void *data,
                    DWORD command) {
  GDI_GUARD;
  struct printer_handle *h = lookup(handle);
  if (!h)
    return 0;
  if (level || data ||
      (command != JOB_CONTROL_PAUSE && command != JOB_CONTROL_RESUME &&
       command != JOB_CONTROL_CANCEL && command != JOB_CONTROL_DELETE))
    return fail(87);
  struct cp_job job;
  if (!get_job(h, id, &job))
    return fail(87);
  char path[300];
  job_path(path, id, "ctl");
  void *f = CreateFileA(path, 0x40000000u, 0, 0, 2, 0, 0);
  if (f == BAD_FILE)
    return 0;
  const char c = command == JOB_CONTROL_PAUSE    ? 'p'
                 : command == JOB_CONTROL_RESUME ? 'r'
                                                 : 'c';
  int ok = io_write(f, &c, 1);
  if (!CloseHandle(f))
    ok = 0;
  return ok;
}
BOOL W32API SetJobW(void *h, DWORD id, DWORD l, void *d, DWORD c) {
  return SetJobA(h, id, l, d, c);
}

/* The generic text-only driver places characters on an 8 by 16 logical-pixel grid.
 * Graphics remain unavailable on this device; PS/PCL use the scalar surface. */
int gdi_printer_text(struct dc *d, int x, int y, unsigned flags,
                     const RECT *rect, const void *text, unsigned count,
                     const int *dx, int wide) {
  struct printer_dc *p = d->printer;
  if (!p || !p->text)
    return -1;
  if (!p->page || (flags & ETO_GLYPH_INDEX))
    return fail(50);
  POINT at = {x, y};
  if (!gdi_transform(d, &at, 1, 0))
    return 0;
  RECT clip = {0, 0, (LONG)p->width, (LONG)p->height};
  if ((flags & ETO_CLIPPED) && rect)
    clip = gdi_rect(d, *rect);
  int64_t advance = 0;
  for (unsigned i = 0; i < count; i++) {
    int64_t px = (int64_t)at.x + (dx ? advance : (int64_t)i * 8);
    int py = at.y;
    unsigned ch = wide ? ((const WCHAR *)text)[i] : ((const uint8_t *)text)[i];
    int c = wide ? gdi_cp1252_ansi(ch) : (int)ch;
    if (px >= clip.left && px < clip.right && py >= clip.top &&
        py < clip.bottom && px >= 0 && py >= 0 &&
        (unsigned)(px / 8) < p->columns && (unsigned)(py / 16) < p->rows)
      p->text[(unsigned)(py / 16) * p->columns + (unsigned)(px / 8)] =
          (char)(c >= 32 ? c : '?');
    if (dx)
      advance += dx[i];
  }
  return 1;
}
