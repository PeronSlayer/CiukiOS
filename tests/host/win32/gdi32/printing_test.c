/* Production printer queue protocol over a real temporary filesystem.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define CreateFileA unused_CreateFileA
#define ReadFile unused_ReadFile
#define WriteFile unused_WriteFile
#define CloseHandle unused_CloseHandle
#define DeleteFileA unused_DeleteFileA
#define CreateDirectoryA unused_CreateDirectoryA
#define RemoveDirectoryA unused_RemoveDirectoryA
#define MoveFileA unused_MoveFileA
#define GetLastError unused_GetLastError
#define SetLastError unused_SetLastError
#define GetFullPathNameA unused_GetFullPathNameA
#define FlushFileBuffers unused_FlushFileBuffers
#include "support.c"
#undef CreateFileA
#undef ReadFile
#undef WriteFile
#undef CloseHandle
#undef DeleteFileA
#undef CreateDirectoryA
#undef RemoveDirectoryA
#undef MoveFileA
#undef GetLastError
#undef SetLastError
#undef GetFullPathNameA
#undef FlushFileBuffers
#include "../../../../apps/libprint/print.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <w32/printing.h>
static char root[512];
static DWORD last_error;
static unsigned short_write;
DWORD W32API GetFullPathNameA(const char *s, DWORD n, char *out, char **file) {
  (void)file;
  unsigned len = (unsigned)strlen(s);
  if (len + 1 > n)
    return len + 1;
  memcpy(out, s, len + 1);
  return len;
}
void W32API SetLastError(DWORD e) { last_error = e; }
DWORD W32API GetLastError(void) { return last_error; }
static int host_path(char *out, const char *p) {
  if (strncmp(p, CP_ROOT, strlen(CP_ROOT)))
    return 0;
  return snprintf(out, 1024, "%s%s", root, p + strlen(CP_ROOT)) < 1024;
}
static void host_error(void) {
  last_error = errno == EEXIST ? 183 : errno == ENOENT ? 2 : 5;
}
void *W32API CreateFileA(const char *name, DWORD access, DWORD sharing,
                         void *sa, DWORD create, DWORD flags, void *template) {
  (void)sharing;
  (void)sa;
  (void)flags;
  (void)template;
  char path[1024];
  if (!host_path(path, name)) {
    last_error = 2;
    return (void *)(intptr_t)-1;
  }
  int f = (access & 0x40000000u) ? O_WRONLY : O_RDONLY;
  if (create == 1)
    f |= O_CREAT | O_EXCL;
  if (create == 2)
    f |= O_CREAT | O_TRUNC;
  int fd = open(path, f, 0600);
  if (fd < 0) {
    host_error();
    return (void *)(intptr_t)-1;
  }
  return (void *)(uintptr_t)(fd + 1);
}
BOOL W32API ReadFile(void *h, void *out, DWORD n, DWORD *got, void *ov) {
  (void)ov;
  ssize_t result = read((int)(uintptr_t)h - 1, out, n);
  *got = result < 0 ? 0 : (DWORD)result;
  return result >= 0;
}
BOOL W32API WriteFile(void *h, const void *in, DWORD n, DWORD *put, void *ov) {
  (void)ov;
  if (short_write && n)
    n--;
  ssize_t result = write((int)(uintptr_t)h - 1, in, n);
  *put = result < 0 ? 0 : (DWORD)result;
  return result >= 0;
}
BOOL W32API FlushFileBuffers(void *h) { return !fsync((int)(uintptr_t)h - 1); }
BOOL W32API CloseHandle(void *h) { return close((int)(uintptr_t)h - 1) == 0; }
BOOL W32API DeleteFileA(const char *p) {
  char path[1024];
  return host_path(path, p) && unlink(path) == 0;
}
BOOL W32API RemoveDirectoryA(const char *p) {
  char path[1024];
  return host_path(path, p) && rmdir(path) == 0;
}
BOOL W32API CreateDirectoryA(const char *p, void *sa) {
  char path[1024];
  (void)sa;
  if (host_path(path, p) && !mkdir(path, 0700))
    return 1;
  host_error();
  return 0;
}
BOOL W32API MoveFileA(const char *a, const char *b) {
  char from[1024], to[1024];
  return host_path(from, a) && host_path(to, b) && !rename(from, to);
}
static void store_config(struct cp_config *c) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/printers", root);
  FILE *f = fopen(path, "wb");
  assert(f);
  assert(fwrite(c, sizeof(*c), 1, f) == 1);
  assert(!fclose(f));
}
static void load_job(unsigned id, struct cp_job *job) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/j%03u/job", root, id);
  FILE *f = fopen(path, "rb");
  assert(f);
  assert(fread(job, sizeof(*job), 1, f) == 1);
  assert(!fclose(f));
}
static void discard_job(unsigned id) {
  char p[1024];
  const char *files[] = {"job", "data", "ctl", "job.tmp"};
  for (unsigned i = 0; i < 4; i++) {
    snprintf(p, sizeof(p), "%s/j%03u/%s", root, id, files[i]);
    unlink(p);
  }
  snprintf(p, sizeof(p), "%s/j%03u", root, id);
  assert(!rmdir(p));
}
int main(int argc, char **argv) {
  setbuf(stdout, 0);
  assert(argc == 2);
  assert(strlen(argv[1]) < sizeof(root));
  strcpy(root, argv[1]);
  assert(!mkdir(root, 0700));
  struct cp_config c = {.magic = CP_CONFIG_MAGIC, .version = 1};
  c.printers[0] =
      (struct cp_printer){.present = 1,
                          .driver = CP_POSTSCRIPT,
                          .page = {.width = 612, .height = 792, .copies = 1}};
  strcpy(c.printers[0].name, "PostScript");
  snprintf(c.printers[0].port, sizeof(c.printers[0].port), "%s/output.prn",
           root);
  c.printers[1] = c.printers[0];
  c.printers[1].driver = CP_PCL;
  strcpy(c.printers[1].name, "PCL");
  c.printers[2] = c.printers[0];
  c.printers[2].driver = CP_TEXT;
  strcpy(c.printers[2].name, "Text");
  store_config(&c);
  DWORD needed = 0, returned = 9;
  assert(!EnumPrintersA(PRINTER_ENUM_LOCAL, 0, 2, 0, 0, &needed, &returned));
  assert(last_error == 122 && returned == 0 &&
         needed > sizeof(PRINTER_INFO_2A) * 2);
  void *buf = malloc(needed + 8);
  memset(buf, 0xa5, needed + 8);
  assert(EnumPrintersA(2, 0, 2, buf, needed, &needed, &returned));
  assert(returned == 3);
  PRINTER_INFO_2A *p = buf;
  assert(!strcmp(p[0].pPrinterName, "PostScript") &&
         !strcmp(p[1].pPortName, c.printers[0].port));
  assert(!strcmp(p[0].pDatatype, "RAW"));
  assert(((uint8_t *)buf)[needed] == 0xa5);
  free(buf);
  /* EnumPorts: the native parallel devices, exact sizes, levels 1/2. */
  assert(!EnumPortsA(0, 1, 0, 0, &needed, &returned) && last_error == 122 &&
         returned == 0 && needed == 3 * sizeof(PORT_INFO_1A) + 30);
  buf = malloc(needed + 8);
  memset(buf, 0xa5, needed + 8);
  assert(!EnumPortsA(0, 1, buf, needed - 1, &needed, &returned) &&
         last_error == 122 && returned == 0);
  assert(EnumPortsA("", 1, buf, needed, &needed, &returned) && returned == 3);
  assert(!strcmp(((PORT_INFO_1A *)buf)[0].pName, "/dev/lpt0") &&
         !strcmp(((PORT_INFO_1A *)buf)[2].pName, "/dev/lpt2"));
  assert(((uint8_t *)buf)[needed] == 0xa5);
  free(buf);
  assert(!EnumPortsW(0, 2, 0, 0, &needed, &returned) && last_error == 122);
  buf = malloc(needed);
  assert(EnumPortsW(0, 2, buf, needed, &needed, &returned) && returned == 3);
  PORT_INFO_2W *port = buf;
  assert(port[1].pPortName[0] == '/' && port[1].pPortName[8] == '1' &&
         !port[1].pPortName[9] && port[1].fPortType == PORT_TYPE_WRITE &&
         port[1].pDescription[17] == '2' && !port[1].Reserved);
  free(buf);
  assert(!EnumPortsA("\\\\SERVER", 1, 0, 0, &needed, &returned) &&
         last_error == 50);
  assert(!EnumPortsA(0, 3, 0, 0, &needed, &returned) && last_error == 124);
  assert(!EnumPortsA(0, 1, 0, 0, 0, &returned) && last_error == 87);
  WCHAR wide[64];
  DWORD size = 64;
  assert(GetDefaultPrinterW(wide, &size) && size == 11 && wide[0] == 'P');
  assert(SetDefaultPrinterA("PCL"));
  size = 64;
  char name[64];
  assert(GetDefaultPrinterA(name, &size) && !strcmp(name, "PCL"));
  assert(SetDefaultPrinterA("PostScript"));
  void *handle;
  assert(!OpenPrinterA("Missing", &handle, 0) && !handle);
  assert(OpenPrinterA("PostScript", &handle, 0));
  assert(!GetPrinterA(handle, 2, 0, 0, &needed) && last_error == 122);
  buf = malloc(needed);
  assert(GetPrinterA(handle, 2, buf, needed, &needed));
  assert(((PRINTER_INFO_2A *)buf)->Attributes & PRINTER_ATTRIBUTE_DEFAULT);
  assert(SetDefaultPrinterA("PCL"));
  assert(!cp_printer_pause(root, 0, 1));
  assert(GetPrinterA(handle, 2, buf, needed, &needed));
  assert(!(((PRINTER_INFO_2A *)buf)->Attributes & PRINTER_ATTRIBUTE_DEFAULT));
  assert(((PRINTER_INFO_2A *)buf)->Status == 1);
  assert(!cp_printer_pause(root, 0, 0));
  assert(SetDefaultPrinterA("PostScript"));
  free(buf);
  DOC_INFO_1A doc = {"RAW test", 0, "EMF"};
  assert(!StartDocPrinterA(handle, 1, &doc) && last_error == 1804);
  doc.pDatatype = "RAW";
  unsigned id = StartDocPrinterA(handle, 1, &doc);
  assert(id);
  assert(!EndPagePrinter(handle));
  assert(StartPagePrinter(handle));
  DWORD put;
  assert(WritePrinter(handle, "abc", 3, &put) && put == 3);
  assert(!EndDocPrinter(handle));
  assert(EndPagePrinter(handle));
  assert(EndDocPrinter(handle));
  struct cp_job job;
  load_job(id, &job);
  assert(job.magic == CP_JOB_MAGIC && job.bytes == 3 &&
         !strcmp(job.title, "RAW test"));
  assert(!EnumJobsA(handle, 0, 10, 1, 0, 0, &needed, &returned) &&
         last_error == 122);
  buf = malloc(needed);
  assert(EnumJobsA(handle, 0, 10, 1, buf, needed, &needed, &returned) &&
         returned == 1);
  assert(((JOB_INFO_1A *)buf)->JobId == id &&
         !strcmp(((JOB_INFO_1A *)buf)->pDocument, "RAW test"));
  free(buf);
  assert(SetJobA(handle, id, 0, 0, JOB_CONTROL_PAUSE));
  char path[1024];
  snprintf(path, sizeof(path), "%s/j%03u/ctl", root, id);
  FILE *f = fopen(path, "rb");
  assert(f && fgetc(f) == 'p');
  fclose(f);
  assert(cp_service_step(root) == 0);
  load_job(id, &job);
  assert(job.state == CP_PAUSED);
  assert(SetJobA(handle, id, 0, 0, JOB_CONTROL_RESUME));
  for (unsigned step = 0; step < 100; step++) {
    int progress = cp_service_step(root);
    assert(progress >= 0);
    if (!progress)
      break;
  }
  f = fopen(c.printers[0].port, "rb");
  assert(f && fgetc(f) == 'a' && fgetc(f) == 'b' && fgetc(f) == 'c' &&
         fgetc(f) == EOF);
  fclose(f);
  assert(!unlink(c.printers[0].port));
  id = StartDocPrinterA(handle, 1, &doc);
  assert(id);
  short_write = 1;
  assert(!WritePrinter(handle, "xx", 2, &put) && put == 1);
  short_write = 0;
  assert(!EndDocPrinter(handle));
  assert(AbortPrinter(handle));
  assert(ClosePrinter(handle));
  assert(!ClosePrinter(handle));
  HDC dc = CreateDCA("WINSPOOL", "PostScript", 0, 0);
  assert(dc && GetDeviceCaps(dc, 2) == 2 && GetDeviceCaps(dc, 8) == 816 &&
         GetDeviceCaps(dc, 88) == 96);
  DOCINFOA info = {sizeof(info), "GDI page", 0, 0, 0};
  assert(StartPage(dc) == -1);
  id = (unsigned)StartDocA(dc, &info);
  assert(id > 0);
  assert(StartPage(dc) == 1);
  assert(SetPixel(dc, 0, 0, 0) != CLR_INVALID);
  assert(EndDoc(dc) == -1);
  assert(EndPage(dc) == 1);
  assert(EndDoc(dc) == 1);
  assert(DeleteDC(dc));
  load_job(id, &job);
  assert(job.bytes > 1000);
  snprintf(path, sizeof(path), "%s/j%03u/data", root, id);
  f = fopen(path, "rb");
  assert(f);
  char prefix[32] = {0};
  assert(fread(prefix, 1, 15, f) == 15 &&
         !strncmp(prefix, "%!PS-Adobe-3.0", 13));
  fclose(f);
  discard_job(id);
  dc = CreateDCA(0, "PCL", 0, 0);
  assert(dc);
  id = (unsigned)StartDocA(dc, &info);
  assert(id > 0 && StartPage(dc) == 1 && EndPage(dc) == 1 && EndDoc(dc) == 1);
  assert(DeleteDC(dc));
  load_job(id, &job);
  assert(job.bytes > 1000);
  snprintf(path, sizeof(path), "%s/j%03u/data", root, id);
  f = fopen(path, "rb");
  assert(f);
  char pcl[80] = {0};
  assert(fread(pcl, 1, sizeof(pcl) - 1, f) > 20);
  fclose(f);
  assert(strstr(pcl, "\033&l2A\033&l0O"));
  assert(strstr(pcl, "\033*t100R\033*r850S\033*r1A\033*b0M\033*b107W"));
  assert(job.bytes ==
         (uint32_t)(strlen("\033E\033&l2A\033&l0O\033*t100R\033*r850S\033*"
                           "r1A\033*b0M") +
                    1100 * (strlen("\033*b107W") + 107) + strlen("\033*rB\f")));
  discard_job(id);
  dc = CreateDCA("WINSPOOL", "PostScript", 0, 0);
  assert(dc);
  assert(StartDocA(dc, &info) > 0);
  assert(DeleteDC(dc));
  dc = CreateDCA("WINSPOOL", "Text", 0, 0);
  assert(dc);
  id = (unsigned)StartDocA(dc, &info);
  assert(id > 0 && StartPage(dc) == 1);
  assert(TextOutA(dc, 0, 0, "Hello", 5));
  assert(TextOutA(dc, 0, 16, "World", 5));
  assert(EndPage(dc) == 1 && EndDoc(dc) == 1 && DeleteDC(dc));
  snprintf(path, sizeof(path), "%s/j%03u/data", root, id);
  f = fopen(path, "rb");
  assert(f);
  char text[64] = {0};
  assert(fread(text, 1, sizeof(text) - 1, f) == 15);
  assert(!strcmp(text, "Hello\r\nWorld\r\n\f"));
  fclose(f);
  discard_job(id);
  assert(OpenPrinterA("PostScript", &handle, 0));
  doc.pOutputFile = "C:\\Documents\\file.prn";
  id = StartDocPrinterA(handle, 1, &doc);
  assert(id && EndDocPrinter(handle));
  load_job(id, &job);
  assert(!strcmp(job.target, "/Documents/file.prn"));
  assert(ClosePrinter(handle));
  discard_job(id);
  uint8_t mode_a[156] = {0};
  w32_put16(mode_a + 36, sizeof(mode_a));
  w32_put32(mode_a + 40, 3);
  w32_put16(mode_a + 44, 2);
  w32_put16(mode_a + 46, 9);
  dc = CreateDCA("WINSPOOL", "PostScript", 0, mode_a);
  assert(dc && GetDeviceCaps(dc, 8) == 1122 && GetDeviceCaps(dc, 10) == 793);
  assert(DeleteDC(dc));
  uint8_t mode_w[220] = {0};
  w32_put16(mode_w + 68, sizeof(mode_w));
  w32_put32(mode_w + 72, 0x101);
  w32_put16(mode_w + 76, 2);
  w32_put16(mode_w + 86, 3);
  WCHAR device[] = {'P', 'o', 's', 't', 'S', 'c', 'r', 'i', 'p', 't', 0};
  dc = CreateDCW(0, device, 0, mode_w);
  assert(dc && GetDeviceCaps(dc, 8) == 1056);
  id = (unsigned)StartDocA(dc, &info);
  assert(id > 0 && EndDoc(dc) == 1 && DeleteDC(dc));
  load_job(id, &job);
  assert(job.page.copies == 3 && job.page.landscape == 1);
  discard_job(id);
  c.printers[0].page.landscape = 1;
  c.printers[0].page.left = 700;
  assert(cp_page_valid(&c.printers[0].page));
  store_config(&c);
  dc = CreateDCA("WINSPOOL", "PostScript", 0, 0);
  assert(dc && GetDeviceCaps(dc, 8) == 1056 && DeleteDC(dc));
  test_reset();
  snprintf(path, sizeof(path), "%s/printers", root);
  assert(!unlink(path));
  assert(!rmdir(root));
  puts("PASS G31 Win32 enumeration, shared queue, RAW, controls, faults, "
       "PS/PCL GDI lifecycle and cleanup");
  return 0;
}
