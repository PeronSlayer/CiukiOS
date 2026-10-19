/* Ciuki private real-mode hook protocol v1; all wire fields little endian.
 * This header is kernel-internal. No native pointer is placed on the wire. */
#ifndef CIUKI_DOS_FILE_HOOK_H
#define CIUKI_DOS_FILE_HOOK_H
#include "dos_file.h"
#define DOS_HOOK_VERSION 1u
#define DOS_HOOK_BIND_BYTES 68u
#define DOS_HOOK_EVENT_BYTES 8u
#define DOS_HOOK_ROM_SEG 0xf000u
/* Reserved after the complete BIOS/DPMI/mouse ROM regions, not in their holes. */
#define DOS_HOOK_DISPATCH 0x1800u
#define DOS_HOOK_EVENT 0x1804u
#define DOS_HOOK_CRITICAL 0x1808u
#define DOS_HOOK_DESCRIPTOR 0x1810u
#define DOS_HOOK_CASEMAP 0x1828u
#define DOS_HOOK_COUNTRY 0x1840u
#define DOS_HOOK_UPPER 0x1870u
#define DOS_HOOK_LOWER 0x1900u
#define DOS_HOOK_TERMINATOR 0x1a10u
#define DOS_HOOK_COLLATING 0x1a40u
#define DOS_HOOK_DBCS 0x1b50u
#define DOS_HOOK_END 0x1b54u
void dos_file_hook_rom(uint8_t *rom_page);
bool dos_file_hook_discover(struct dos_file_frame *);
int dos_file_hook_bind(struct dos_file *,struct dos_file_frame *,uint16_t caller_cs);
int dos_file_hook_event(struct dos_file *,struct dos_file_frame *,uint16_t caller_cs);
#endif
