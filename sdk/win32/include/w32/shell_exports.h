/* Explicit platform-owned export set; everything else stays in the reused
 * Wine module. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_SHELL_EXPORTS_H
#define CIUKI_SHELL_EXPORTS_H
#define W32_SHELL_EXPORTS(X) \
 X(SHGetSpecialFolderPathA) X(SHGetSpecialFolderPathW) \
 X(SHGetFolderPathA) X(SHGetFolderPathW) X(SHFileOperationA) \
 X(ShellExecuteA) X(ShellExecuteW) X(ShellExecuteExA) X(ShellExecuteExW) \
 X(FindExecutableA) X(SHAppBarMessage) X(Shell_NotifyIconA) X(Shell_NotifyIconW) \
 X(SHChangeNotify) X(DllGetClassObject) X(SHGetMalloc) X(SHGetFileInfoA) \
 X(ExtractIconA) X(ExtractIconW) X(ExtractIconExA) X(ExtractIconExW) \
 X(DragAcceptFiles) X(DragQueryFileA) X(DragFinish)
#endif
