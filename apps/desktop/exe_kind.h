/* SPDX-License-Identifier: MIT
 * fx-autorun: which personality runs an MZ executable, decided from its
 * header as the Windows loader decides it. Wine 10.0 server/mapping.c
 * get_image_params reads the header at e_lfanew: "PE\0\0" is a Win32 image;
 * "NE" with target operating system 2 (Windows, ne_exetyp at NE+0x36) is a
 * 16-bit Windows program; NE for OS/2, European MS-DOS 4 or BOSS, LE/LX
 * (DOS-extended programs such as DOS/4GW titles, VxDs) and a plain MZ
 * header start as MS-DOS programs (kernelbase CreateProcessInternalW hands
 * all three to the VDM). NE field offsets: Wine 10.0 include/winnt.h
 * IMAGE_OS2_HEADER (ne_magic at 0x00, ne_exetyp "target OS" at 0x36).
 *
 * CiukiOS starts both Windows kinds through the Win32 personality: the
 * kernel's spawn admission runs the native InstallShield 5 bootstrap for an
 * IS5 setup stub (src/kernel/proc/win32/is5boot.h) and refuses every other
 * 16-bit Windows program with the "needs 16-bit Windows support" dialog;
 * MS-DOS programs go to the DOS Box. The kernel admission and the PE
 * validator require e_lfanew >= 64, so a smaller value is an MS-DOS header. */
#ifndef DESK_EXE_KIND_H
#define DESK_EXE_KIND_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
enum desk_exe_kind { DESK_EXE_NONE, DESK_EXE_DOS, DESK_EXE_WIN32, DESK_EXE_WIN16 };
#define DESK_EXE_HEADER 64u          /* bytes read at offset 0 and at e_lfanew */
#define DESK_EXE_LFANEW_MAX (16u*1024u*1024u)
/* e_lfanew of a complete 64-byte MZ header, else 0 (no extended header). */
static inline uint32_t desk_exe_lfanew(const uint8_t *mz, size_t bytes)
{
    if (bytes<DESK_EXE_HEADER || mz[0]!='M' || mz[1]!='Z') return 0;
    uint32_t at=(uint32_t)mz[60]|(uint32_t)mz[61]<<8|(uint32_t)mz[62]<<16|(uint32_t)mz[63]<<24;
    return at>=DESK_EXE_HEADER && at<=DESK_EXE_LFANEW_MAX ? at : 0;
}
/* mz: the first bytes of the file; next: the bytes read at e_lfanew (none
 * when desk_exe_lfanew is 0 or the file ends there). */
static inline int desk_exe_classify(const uint8_t *mz, size_t mz_bytes, const uint8_t *next, size_t next_bytes)
{
    if (mz_bytes<2 || mz[0]!='M' || mz[1]!='Z') return DESK_EXE_NONE;
    if (!next) return DESK_EXE_DOS;
    if (next_bytes>=4 && !memcmp(next,"PE\0\0",4)) return DESK_EXE_WIN32;
    if (next_bytes>0x36 && next[0]=='N' && next[1]=='E' && next[0x36]==2) return DESK_EXE_WIN16;
    return DESK_EXE_DOS;
}
static inline const char *desk_exe_name(int kind)
{
    return kind==DESK_EXE_WIN32 ? "win32" : kind==DESK_EXE_WIN16 ? "win16" : kind==DESK_EXE_DOS ? "dos" : "none";
}
#endif
