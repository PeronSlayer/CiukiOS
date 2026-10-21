/* Shared state of the MCI host harnesses. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#ifndef MCI_HOST_H
#define MCI_HOST_H
#include "../../../../sdk/win32/audio/audio.h"
#include "../../../../sdk/win32/audio/mci.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <w32/kernel32.h>
int hf_open(const char *);
unsigned hf_open_handles(void);
long hf_read(int, void *, unsigned long);
long hf_seek(int, long, int);
long hf_size(int);
int hf_identity(int, uint32_t *, uint32_t *, uint32_t *);
void hf_close(int);
int hf_chdir(const char *);
int hf_write_file(const char *, const void *, unsigned long);
uint32_t mciSendStringA(const char *, char *, uint32_t, uintptr_t);
uint32_t mciSendStringW(const WCHAR *, WCHAR *, uint32_t, uintptr_t);
uint32_t mciSendCommandA(uint32_t, uint32_t, uintptr_t, uintptr_t);
uint32_t mciSendCommandW(uint32_t, uint32_t, uintptr_t, uintptr_t);
uint32_t mciGetDeviceIDA(const char *);
uint32_t mciGetDeviceIDW(const WCHAR *);
BOOL mciGetErrorStringA(uint32_t, char *, uint32_t);
BOOL mciGetErrorStringW(uint32_t, WCHAR *, uint32_t);
uintptr_t mciGetCreatorTask(uint32_t);
BOOL mciSetYieldProc(uint32_t, uintptr_t, uint32_t);
uintptr_t mciGetYieldProc(uint32_t, uint32_t *);
void mci_service_pass(void);
uint32_t acmStreamOpen(uintptr_t *, uintptr_t, const void *, const void *,
                       void *, uintptr_t, uintptr_t, uint32_t);
uint32_t acmStreamClose(uintptr_t, uint32_t);
uint32_t acmStreamPrepareHeader(uintptr_t, void *, uint32_t);
uint32_t acmStreamUnprepareHeader(uintptr_t, void *, uint32_t);
uint32_t acmStreamConvert(uintptr_t, void *, uint32_t);
extern FILE *trace;
extern uint32_t now_ms, digest;
extern uint64_t written;
extern long live_allocations;
extern unsigned mci_identity_fail;
extern int key_down;
extern struct wa_engine engine;
extern int16_t *capture;
extern size_t capture_frames, capture_cap;
void out(const char *format, ...);
void advance(uint32_t ms);
#endif
