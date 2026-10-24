/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_PRINTING_H
#define CIUKI_W32_PRINTING_H
#include "gdi32.h"
typedef struct {
  uint16_t wYear;
  uint16_t wMonth;
  uint16_t wDayOfWeek;
  uint16_t wDay;
  uint16_t wHour;
  uint16_t wMinute;
  uint16_t wSecond;
  uint16_t wMilliseconds;
} W32_PRINT_TIME;
typedef struct {
  int cbSize;
  const char *lpszDocName;
  const char *lpszOutput;
  const char *lpszDatatype;
  DWORD fwType;
} DOCINFOA;
typedef struct {
  int cbSize;
  const WCHAR *lpszDocName;
  const WCHAR *lpszOutput;
  const WCHAR *lpszDatatype;
  DWORD fwType;
} DOCINFOW;
typedef struct {
  const char *pDocName;
  const char *pOutputFile;
  const char *pDatatype;
} DOC_INFO_1A;
typedef struct {
  const WCHAR *pDocName;
  const WCHAR *pOutputFile;
  const WCHAR *pDatatype;
} DOC_INFO_1W;
typedef struct {
  DWORD Flags;
  char *pDescription;
  char *pName;
  char *pComment;
} PRINTER_INFO_1A;
typedef struct {
  DWORD Flags;
  WCHAR *pDescription;
  WCHAR *pName;
  WCHAR *pComment;
} PRINTER_INFO_1W;
typedef struct {
  char *pPrinterName;
  char *pServerName;
  DWORD Attributes;
} PRINTER_INFO_4A;
typedef struct {
  WCHAR *pPrinterName;
  WCHAR *pServerName;
  DWORD Attributes;
} PRINTER_INFO_4W;
typedef struct {
  char *pPrinterName;
  char *pPortName;
  DWORD Attributes;
  DWORD DeviceNotSelectedTimeout;
  DWORD TransmissionRetryTimeout;
} PRINTER_INFO_5A;
typedef struct {
  WCHAR *pPrinterName;
  WCHAR *pPortName;
  DWORD Attributes;
  DWORD DeviceNotSelectedTimeout;
  DWORD TransmissionRetryTimeout;
} PRINTER_INFO_5W;
typedef struct {
  char *pServerName;
  char *pPrinterName;
  char *pShareName;
  char *pPortName;
  char *pDriverName;
  char *pComment;
  char *pLocation;
  void *pDevMode;
  char *pSepFile;
  char *pPrintProcessor;
  char *pDatatype;
  char *pParameters;
  void *pSecurityDescriptor;
  DWORD Attributes;
  DWORD Priority;
  DWORD DefaultPriority;
  DWORD StartTime;
  DWORD UntilTime;
  DWORD Status;
  DWORD cJobs;
  DWORD AveragePPM;
} PRINTER_INFO_2A;
typedef struct {
  DWORD JobId;
  char *pPrinterName;
  char *pMachineName;
  char *pUserName;
  char *pDocument;
  char *pDatatype;
  char *pStatus;
  DWORD Status;
  DWORD Priority;
  DWORD Position;
  DWORD TotalPages;
  DWORD PagesPrinted;
  W32_PRINT_TIME Submitted;
} JOB_INFO_1A;
typedef struct {
  WCHAR *pServerName;
  WCHAR *pPrinterName;
  WCHAR *pShareName;
  WCHAR *pPortName;
  WCHAR *pDriverName;
  WCHAR *pComment;
  WCHAR *pLocation;
  void *pDevMode;
  WCHAR *pSepFile;
  WCHAR *pPrintProcessor;
  WCHAR *pDatatype;
  WCHAR *pParameters;
  void *pSecurityDescriptor;
  DWORD Attributes;
  DWORD Priority;
  DWORD DefaultPriority;
  DWORD StartTime;
  DWORD UntilTime;
  DWORD Status;
  DWORD cJobs;
  DWORD AveragePPM;
} PRINTER_INFO_2W;
typedef struct {
  DWORD JobId;
  WCHAR *pPrinterName;
  WCHAR *pMachineName;
  WCHAR *pUserName;
  WCHAR *pDocument;
  WCHAR *pDatatype;
  WCHAR *pStatus;
  DWORD Status;
  DWORD Priority;
  DWORD Position;
  DWORD TotalPages;
  DWORD PagesPrinted;
  W32_PRINT_TIME Submitted;
} JOB_INFO_1W;
#define PRINTER_ENUM_DEFAULT 1u
#define PRINTER_ENUM_LOCAL 2u
#define PRINTER_ENUM_CONNECTIONS 4u
#define PRINTER_ATTRIBUTE_DEFAULT 4u
#define PRINTER_ATTRIBUTE_LOCAL 0x40u
#define PRINTER_CONTROL_PAUSE 1u
#define PRINTER_CONTROL_RESUME 2u
#define PRINTER_CONTROL_PURGE 3u
#define JOB_CONTROL_PAUSE 1u
#define JOB_CONTROL_RESUME 2u
#define JOB_CONTROL_CANCEL 3u
#define JOB_CONTROL_DELETE 5u
#define JOB_STATUS_PAUSED 1u
#define JOB_STATUS_ERROR 2u
#define JOB_STATUS_PRINTING 0x10u
#define JOB_STATUS_OFFLINE 0x20u
BOOL W32API EnumJobsA(void *, DWORD, DWORD, DWORD, void *, DWORD, DWORD *,
                      DWORD *);
BOOL W32API EnumJobsW(void *, DWORD, DWORD, DWORD, void *, DWORD, DWORD *,
                      DWORD *);
BOOL W32API SetJobA(void *, DWORD, DWORD, void *, DWORD);
BOOL W32API SetJobW(void *, DWORD, DWORD, void *, DWORD);
BOOL W32API EnumPrintersA(DWORD, char *, DWORD, void *, DWORD, DWORD *,
                          DWORD *);
BOOL W32API EnumPrintersW(DWORD, WCHAR *, DWORD, void *, DWORD, DWORD *,
                          DWORD *);
BOOL W32API GetDefaultPrinterA(char *, DWORD *);
BOOL W32API GetDefaultPrinterW(WCHAR *, DWORD *);
BOOL W32API SetDefaultPrinterA(const char *);
BOOL W32API SetDefaultPrinterW(const WCHAR *);
BOOL W32API OpenPrinterA(char *, void **, void *);
BOOL W32API OpenPrinterW(WCHAR *, void **, void *);
BOOL W32API ClosePrinter(void *);
DWORD W32API StartDocPrinterA(void *, DWORD, const void *);
DWORD W32API StartDocPrinterW(void *, DWORD, const void *);
BOOL W32API StartPagePrinter(void *);
BOOL W32API EndPagePrinter(void *);
BOOL W32API WritePrinter(void *, const void *, DWORD, DWORD *);
BOOL W32API EndDocPrinter(void *);
BOOL W32API AbortPrinter(void *);
BOOL W32API GetPrinterA(void *, DWORD, void *, DWORD, DWORD *);
BOOL W32API GetPrinterW(void *, DWORD, void *, DWORD, DWORD *);
BOOL W32API SetPrinterA(void *, DWORD, void *, DWORD);
BOOL W32API SetPrinterW(void *, DWORD, void *, DWORD);
int W32API StartDocA(HDC, const void *);
int W32API StartDocW(HDC, const void *);
int W32API StartPage(HDC);
int W32API EndPage(HDC);
int W32API EndDoc(HDC);
int W32API AbortDoc(HDC);
#endif
