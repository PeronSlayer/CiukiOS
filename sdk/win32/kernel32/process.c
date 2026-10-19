/* Public CreateProcess A/W argument preparation. Search/publication is
 * kernel-owned. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
struct startup_a {
  DWORD size;
  char *reserved, *desktop, *title;
  DWORD x, y, width, height, chars_x, chars_y, fill, flags;
  uint16_t show, reserved_bytes;
  uint8_t *reserved2;
  HANDLE input, output, error;
};
struct process_info {
  HANDLE process, thread;
  DWORD pid, tid;
};
extern char *W32API GetCommandLineA(void);
extern char *W32API GetEnvironmentStringsA(void);
extern BOOL W32API FreeEnvironmentStringsA(char *);
BOOL W32API CreateProcessA(const char *application, char *command,
                           SECURITY_ATTRIBUTES *ps, SECURITY_ATTRIBUTES *ts,
                           BOOL inherit, DWORD flags, void *environment,
                           const char *directory, struct startup_a *startup,
                           struct process_info *out) {
  (void)ps;
  (void)ts;
  if (!startup || startup->size != sizeof(*startup) || !out ||
      (!application && !command)) {
    SetLastError(87);
    return 0;
  }
  /* No descriptor for an arbitrary desktop exists before f5-09. */
  if (startup->desktop && *startup->desktop) {
    SetLastError(120);
    return 0;
  }
  char module[260], path[778], cwd[778];
  const char *module_start = 0, *module_end = 0;
  if (application) {
    if (!w32_copy_string(module, sizeof(module), application)) {
      SetLastError(206);
      return 0;
    }
  } else {
    const char *q = command;
    while (*q == ' ' || *q == '\t')
      q++;
    int quoted = *q == '"';
    if (quoted)
      q++;
    else
      module_start = q;
    DWORD n = 0;
    while (*q && (quoted ? *q != '"' : *q != ' ' && *q != '\t')) {
      if (n + 1 >= sizeof(module)) {
        SetLastError(206);
        return 0;
      }
      module[n++] = *q++;
    }
    module[n] = 0;
    module_end = q;
    if (!n) {
      SetLastError(2);
      return 0;
    }
  }
  if (!k32_path(module, path) || (directory && !k32_path(directory, cwd)))
    return 0;
  char *owned = 0, *env = environment;
  if (!env) {
    owned = GetEnvironmentStringsA();
    if (!owned)
      return 0;
    env = owned;
  }
  if ((flags & 0x400) && environment) {
    const WCHAR *wide=environment;
    DWORD chars=0;
    while (chars<32767 && (wide[chars] || wide[chars+1])) chars++;
    if (chars==32767) { SetLastError(87); return 0; }
    chars+=2;
    owned=k32_alloc(chars);
    if (!owned) { SetLastError(8); return 0; }
    BOOL used=0;
    if (!WideCharToMultiByte(1252,0,wide,(int)chars,owned,(int)chars,0,&used) || used) {
      k32_free(owned,chars);SetLastError(1113);return 0;
    }
    env=owned;
  }
  DWORD bytes=0;
  while (bytes<32767 && env[bytes]) {
    while (bytes<32767 && env[bytes]) bytes++;
    bytes++;
  }
  bytes++;
  if (bytes<2) bytes=2;
  if (bytes>32768) {
    if (owned) { if ((flags&0x400) && environment) k32_free(owned,bytes); else FreeEnvironmentStringsA(owned); }
    SetLastError(87);return 0;
  }
  int unicode_owned=(flags&0x400) && environment;
  flags&=~0x400u;
  struct ciuki_w32_startup wire = {.x = startup->x,
                                   .y = startup->y,
                                   .width = startup->width,
                                   .height = startup->height,
                                   .chars_x = startup->chars_x,
                                   .chars_y = startup->chars_y,
                                   .fill = startup->fill,
                                   .flags = startup->flags,
                                   .show = startup->show,
                                   .input = K32_PTR(startup->input),
                                   .output = K32_PTR(startup->output),
                                   .error = K32_PTR(startup->error),
                                   .title = K32_PTR(startup->title)};
  struct ciuki_w32_spawn_args a = {
      .image = K32_PTR(path),
      .startup = K32_PTR(&wire),
      .handle_flags = (ps && ps->inherit ? 1 : 0) | (ts && ts->inherit ? 2 : 0),
      .command = K32_PTR(command ? command : module),
      .directory = directory ? K32_PTR(cwd) : 0,
      .environment = K32_PTR(env),
      .environment_bytes = bytes,
      .inherit = !!inherit,
      .flags = flags};
  a.header = K32_HEADER(W32_PROCESS, a);
  int32_t status;
  for (;;) {
    status = w32_ciuki_call(&a, sizeof(a));
    if (status || (a.error != 2 && a.error != 3) || !module_start)
      break;
    const char *q = module_end;
    while (*q == ' ' || *q == '\t')
      q++;
    if (!*q)
      break;
    while (*q && *q != ' ' && *q != '\t')
      q++;
    DWORD n = (DWORD)(q - module_start);
    if (n >= sizeof(module)) {
      a.error = 206;
      break;
    }
    w32_copy(module, module_start, n);
    module[n] = 0;
    module_end = q;
    if (!k32_path(module, path)) {
      if (owned) { if (unicode_owned) k32_free(owned,bytes); else FreeEnvironmentStringsA(owned); }
      return 0;
    }
    a.error = 0;
  }
  if (owned) { if (unicode_owned) k32_free(owned,bytes); else FreeEnvironmentStringsA(owned); }
  if (!k32_status(status))
    return 0;
  if (a.error) {
    SetLastError(a.error);
    return 0;
  }
  *out =
      (struct process_info){(HANDLE)(uintptr_t)a.process_handle,
                            (HANDLE)(uintptr_t)a.thread_handle, a.pid, a.tid};
  return 1;
}
void W32API GetStartupInfoA(struct startup_a *out) {
  if (!out)
    return;
  struct w32_process_parameters *p =
      NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters;
  struct startup_a s = {.size = sizeof(s),
                        .x = p->StartingX,
                        .y = p->StartingY,
                        .width = p->CountX,
                        .height = p->CountY,
                        .chars_x = p->CountCharsX,
                        .chars_y = p->CountCharsY,
                        .fill = p->FillAttribute,
                        .flags = p->WindowFlags,
                        .show = (uint16_t)p->ShowWindowFlags,
                        .input = p->StandardInput,
                        .output = p->StandardOutput,
                        .error = p->StandardError};
  *out = s;
}

DWORD W32API WinExec(const char *command, unsigned show) {
  struct startup_a startup = {
      .size = sizeof(startup), .flags = 1, .show = (uint16_t)show};
  struct process_info info;
  if (!CreateProcessA(0, (char *)command, 0, 0, 0, 0, 0, 0, &startup, &info)) {
    DWORD error = GetLastError();
    return error < 32 ? error : 11;
  }
  CloseHandle(info.process);
  CloseHandle(info.thread);
  return 33;
}
