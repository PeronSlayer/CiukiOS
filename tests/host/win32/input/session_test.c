/* Production USER/v3/kernel queue fixture with original DirectInput engine.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define main user32_original_main
#include "../user32/user32_test.c"
#undef main
#include "input_tables.h"
#include <w32/dinput.h>
#include <w32/input_bridge.h>
static unsigned notifications;
BOOL W32API SetEvent(HANDLE h) {
  CHECK(h == (HANDLE)77);
  notifications++;
  return 1;
}
static void di_event(HWND h, UINT opcode, UINT type, UINT code, int x, int y,
                     DWORD stamp) {
  struct u32_window *w = u32_window(h);
  struct ciuki_input_event e = {.type = type,
                                .code = code,
                                .value = x,
                                .value2 = y,
                                .monotonic_ns = (uint64_t)stamp * 1000000};
  UINT values[10] = {0};
  w32_copy(values, &e, 40);
  event(w, opcode, values, 10);
  u32_pump();
}
static unsigned enum_count, enum_size;
static BOOL W32API count_object(const void *record, void *context) {
  (void)context;
  enum_count++;
  CHECK(w32_u32(record) == enum_size);
  return 1;
}
static void layouts(void) {
  uint8_t keys[256] = {0};
  uint16_t out = 0;
  char name[64];
  uintptr_t list[2];
  CHECK(GetKeyboardLayoutList(0, 0) == 2 &&
        GetKeyboardLayoutList(2, list) == 2);
  CHECK(GetKeyboardType(0) == 4 && GetKeyboardType(2) == 12);
  for (UINT layout = 0; layout < 2; layout++) {
    u32_layout = layout;
    uintptr_t h = layout ? 0x04100410 : 0x04090409;
    CHECK(GetKeyboardLayoutNameA(name) &&
          !strcmp(name, layout ? "00000410" : "00000409"));
    for (UINT i = 0; i < sizeof(pinned_keys) / sizeof(*pinned_keys); i++) {
      const unsigned *row = pinned_keys[i];
      CHECK(u32_scan_vk(layout, row[0]) == row[1 + layout]);
      CHECK(u32_dik(row[0]) == row[3]);
      CHECK(u32_key_lparam(row[0], 1, 0, 0) == row[4]);
      CHECK(MapVirtualKeyExA(row[0], 1, h) == row[1 + layout]);
    }
    for (UINT i = 0; i < sizeof(pinned_chars) / sizeof(*pinned_chars); i++) {
      const unsigned *r = pinned_chars[i];
      if (r[0] != layout)
        continue;
      memset(keys, 0, 256);
      keys[16] = r[2] & 1 ? 128 : 0;
      keys[165] = r[2] & 2 ? 128 : 0;
      keys[20] = r[2] & 4 ? 1 : 0;
      UINT vk = u32_scan_vk(layout, r[1]);
      CHECK(ToAsciiEx(vk, r[1], keys, &out, 0, h) == 1 &&
            out == u32_ansi(r[3]));
    }
    CHECK(MapVirtualKeyA('Q', 0) == 0x10 && MapVirtualKeyA('Q', 2) == 'Q');
    CHECK(MapVirtualKeyA(0x36, 3) == 161 && MapVirtualKeyA(0xe01d, 3) == 163);
    CHECK(MapVirtualKeyA(165, 0) == 0x38);
    CHECK(GetKeyNameTextA(0x11d0000, name, 64) && !strcmp(name, "Right Ctrl"));
    CHECK(GetKeyNameTextA(0x37 << 16 | 1u << 24, name, 64) &&
          !strcmp(name, "Print Screen"));
    CHECK(VkKeyScanA('A') == ('A' | 0x100));
    CHECK(OemKeyScan('a') == 0x1e && OemKeyScan(0xffff) == 0xffffffffu);
  }
  u32_layout = 1;
  memset(keys, 0, 256);
  keys[16] = 128;
  CHECK(ToAscii(0xdd, 0x0d, keys, &out, 0) == 1 && out == '^');
  keys[16] = 0;
  CHECK(ToAscii('A', 0x1e, keys, &out, 0) == 1 && out == 'a');
  CHECK(VkKeyScanA((char)0x80) == ('E' | 0x600));
  CHECK(ActivateKeyboardLayout(0x04090409, 0) == 0x04100410 &&
        GetKeyboardLayout(0) == 0x04090409);
  CHECK(ToAscii(0x67, 0x47, keys, &out, 0) == 1 && out == '7');
  CHECK(ToAscii(0x24, 0x47, keys, &out, 0) == 0);
  puts("PASS pinned US/Italian scan VK DIK lParam characters; spacing accents, "
       "no dead keys");
}
static MSG next_key(HWND h) {
  MSG m;
  CHECK(PeekMessageA(&m, h, WM_KEYDOWN, WM_SYSKEYUP, PM_REMOVE));
  return m;
}
static void message_edges(HWND h) {
  MSG msg;
  while (PeekMessageA(&msg, 0, WM_KEYDOWN, WM_SYSKEYUP, PM_REMOVE)) {
  }
  u32_clear_input();
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x2a, 1, 0, 200);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x36, 1, 0, 201);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x2a, 0, 0, 202);
  CHECK(GetAsyncKeyState(16) & 0x8000);
  CHECK(!(GetAsyncKeyState(160) & 0x8000));
  CHECK(GetAsyncKeyState(161) & 0x8000);
  CHECK(next_key(h).lParam == 0x002a0001);
  CHECK(next_key(h).lParam == 0x00360001);
  next_key(h);
  CHECK(GetKeyState(16) & 0x8000);
  CHECK(GetKeyState(161) & 0x8000);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x36, 0, 0, 203);
  next_key(h);
  CHECK(!GetKeyState(16));
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x47, 1, 0, 204);
  CHECK(next_key(h).wParam == 36);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x47, 0, 0, 205);
  next_key(h);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x45, 1, 0, 206);
  next_key(h);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x45, 2, 0, 207);
  CHECK((DWORD)next_key(h).lParam == 0x41450001);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x45, 0, 0, 208);
  next_key(h);
  CHECK(GetKeyState(144) == 1);
  char lock_name[32];
  CHECK(GetKeyNameTextA(0x01450001, lock_name, 32) &&
        !strcmp(lock_name, "Num Lock"));
  CHECK(GetKeyNameTextA(0x00450001, lock_name, 32) &&
        !strcmp(lock_name, "Pause"));
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x44, 1, 0, 208);
  msg = next_key(h);
  CHECK(msg.message == WM_SYSKEYDOWN && !((DWORD)msg.lParam & (1u << 29)));
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x44, 0, 0, 208);
  CHECK(next_key(h).message == WM_SYSKEYUP);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x47, 1, 0, 209);
  CHECK(next_key(h).wParam == 0x67);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x2a, 1, 0, 210);
  next_key(h);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x47, 0, 0, 211);
  CHECK(next_key(h).wParam == 0x67);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x2a, 0, 0, 212);
  next_key(h);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x200, 1, 0, 213);
  msg = next_key(h);
  CHECK(msg.wParam == 19 && (DWORD)msg.lParam == 0x00450001);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x200, 0, 0, 214);
  CHECK((DWORD)next_key(h).lParam == 0xc0450001);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x146, 1, 0, 215);
  CHECK(next_key(h).wParam == 3);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x146, 0, 0, 216);
  next_key(h);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x137, 1, 0, 217);
  CHECK(!PeekMessageA(&msg, h, WM_KEYDOWN, WM_SYSKEYUP, PM_REMOVE));
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x137, 0, 0, 218);
  msg = next_key(h);
  CHECK(msg.wParam == 44 && (DWORD)msg.lParam == 0xc1370001);
  di_event(h, DESK_MOTION, CIUKI_INPUT_WHEEL, 0, -2, 0, 219);
  CHECK(PeekMessageA(&msg, h, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_REMOVE) &&
        (int16_t)(msg.wParam >> 16) == -240);
  DWORD lines = 0;
  CHECK(SystemParametersInfoA(104, 0, &lines, 0) && lines == 3);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x1e, 1, 0, 220);
  ui_kernel_switch(1);
  CHECK(!GetKeyState('A') && (GetAsyncKeyState('A') & 0x8000));
  ui_kernel_switch(0);
  next_key(h);
  CHECK(GetKeyState('A') & 0x8000);
  u32_clear_input();
  CHECK(!GetKeyState('A') && !GetAsyncKeyState('A'));
  RECT old_clip = {15, 40, 50, 70};
  CHECK(ClipCursor(&old_clip));
  u32_clear_input();
  RECT bounds;
  CHECK(GetClipCursor(&bounds) && bounds.right == (LONG)u32_screen_w);
  CHECK(outgoing[outgoing_count - 1].data[1] == DESK_V3_POINTER &&
        !desk_get32(outgoing[outgoing_count - 1].data + 16));
  puts("PASS modifier sides, lock repeats, keypad, Pause/Break, Snapshot-up, "
       "wheel and thread/global key state");
}
static IDirectInputDevice *device(IDirectInput *input, const GUID *guid,
                                  const DIDATAFORMAT *f, HWND h, UINT coop) {
  IDirectInputDevice *d = 0;
  CHECK(input->lpVtbl->CreateDevice(input, guid, &d, 0) == DI_OK && d);
  CHECK(d->lpVtbl->SetDataFormat(d, f) == DI_OK);
  CHECK(d->lpVtbl->SetCooperativeLevel(d, h, coop) == DI_OK);
  return d;
}
static void versions(HWND h) {
  const GUID *iids[] = {&IID_IDirectInputA,  &IID_IDirectInputW,
                        &IID_IDirectInput2A, &IID_IDirectInput2W,
                        &IID_IDirectInput7A, &IID_IDirectInput7W,
                        &IID_IDirectInput8A, &IID_IDirectInput8W};
  const GUID *diids[] = {&IID_IDirectInputDeviceA,  &IID_IDirectInputDeviceW,
                         &IID_IDirectInputDevice2A, &IID_IDirectInputDevice2W,
                         &IID_IDirectInputDevice7A, &IID_IDirectInputDevice7W,
                         &IID_IDirectInputDevice8A, &IID_IDirectInputDevice8W};
  DWORD versions[] = {0x300, 0x500, 0x700, 0x800};
  for (UINT v = 0; v < 4; v++)
    for (UINT wide = 0; wide < 2; wide++) {
      IDirectInput *input = 0;
      CHECK(DirectInputCreateEx(0, versions[v], iids[v * 2 + wide],
                                (void **)&input, 0) == DI_OK);
      enum_count = 0;
      enum_size =
          versions[v] < 0x500 ? (wide ? 1080 : 560) : (wide ? 1100 : 580);
      CHECK(input->lpVtbl->EnumDevices(input, 0, count_object, 0,
                                       DIEDFL_ATTACHEDONLY) == DI_OK &&
            enum_count == 2);
      enum_count = 0;
      CHECK(input->lpVtbl->EnumDevices(input, 4, count_object, 0, 1) == DI_OK &&
            enum_count == 0);
      enum_count = 0;
      CHECK(input->lpVtbl->EnumDevices(input, 1, count_object, 0, 1) == DI_OK &&
            enum_count == 0);
      enum_count = 0;
      CHECK(input->lpVtbl->EnumDevices(input, 2, count_object, 0, 1) == DI_OK &&
            enum_count == 1);
      for (UINT kind = 0; kind < 2; kind++) {
        IDirectInputDevice *d = 0;
        CHECK(input->lpVtbl->CreateDevice(
                  input, kind ? &GUID_SysMouse : &GUID_SysKeyboard, &d, 0) ==
              DI_OK);
        IDirectInputDevice *view = 0;
        CHECK(d->lpVtbl->QueryInterface(d, diids[v * 2 + wide],
                                        (void **)&view) == DI_OK);
        enum_count = 0;
        enum_size =
            versions[v] < 0x500 ? (wide ? 552 : 292) : (wide ? 576 : 316);
        CHECK(view->lpVtbl->EnumObjects(view, count_object, 0, 0) == DI_OK &&
              enum_count == (kind ? 11 : 256));
        uint8_t info[1104];
        memset(info, 0xa5, sizeof(info));
        w32_put32(info, wide ? 1080 : 560);
        CHECK(view->lpVtbl->GetDeviceInfo(view, info) == DI_OK &&
              info[wide ? 1080 : 560] == 0xa5);
        w32_put32(info, wide ? 1100 : 580);
        CHECK(view->lpVtbl->GetDeviceInfo(view, info) == DI_OK &&
              info[wide ? 1100 : 580] == 0xa5);
        w32_put32(info, 12);
        CHECK(view->lpVtbl->GetDeviceInfo(view, info) == DIERR_INVALIDPARAM);
        DIDEVCAPS caps = {.dwSize = 24};
        CHECK(view->lpVtbl->GetCapabilities(view, &caps) == DI_OK &&
              caps.dwButtons == (kind ? 8 : 256));
        caps.dwSize = 44;
        CHECK(view->lpVtbl->GetCapabilities(view, &caps) == DI_OK);
        DWORD old_size = wide ? 552 : 292, full_size = old_size + 24;
        DWORD object = kind ? DIDFT_AXIS | DIDFT_MAKEINSTANCE(1)
                            : DIDFT_BUTTON | DIDFT_MAKEINSTANCE(30);
        memset(info, 0xa5, sizeof(info));
        w32_put32(info, old_size);
        CHECK(view->lpVtbl->GetObjectInfo(view, info, object, DIPH_BYID) ==
                  DI_OK &&
              info[old_size] == 0xa5);
        w32_put32(info, full_size);
        CHECK(view->lpVtbl->GetObjectInfo(view, info, object, DIPH_BYID) ==
                  DI_OK &&
              info[full_size] == 0xa5);
        w32_put32(info, full_size - 1);
        CHECK(view->lpVtbl->GetObjectInfo(view, info, object, DIPH_BYID) ==
              DIERR_INVALIDPARAM);
        caps.dwSize = 25;
        CHECK(view->lpVtbl->GetCapabilities(view, &caps) == DIERR_INVALIDPARAM);
        CHECK(view->lpVtbl->SetDataFormat(
                  view, kind ? &c_dfDIMouse : &c_dfDIKeyboard) == DI_OK);
        CHECK(view->lpVtbl->SetCooperativeLevel(
                  view, h, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE) == DI_OK);
        CHECK(view->lpVtbl->Acquire(view) == DI_OK &&
              view->lpVtbl->Acquire(view) == DI_NOEFFECT);
        uint8_t state[256];
        CHECK(view->lpVtbl->GetDeviceState(view, kind ? 16 : 256, state) ==
              DI_OK);
        CHECK(view->lpVtbl->GetDeviceState(view, kind ? 20 : 255, state) ==
              DIERR_INVALIDPARAM);
        CHECK(view->lpVtbl->SetDataFormat(view, &c_dfDIMouse2) ==
              DIERR_ACQUIRED);
        DWORD count = DI_INFINITE;
        DIDEVICEOBJECTDATA data;
        CHECK(view->lpVtbl->GetDeviceData(view, 16, &data, &count, 0) ==
                  DIERR_NOTBUFFERED &&
              count == 0);
        DIPROPDWORD prop = {{20, 16, 0, 0}, 4};
        CHECK(view->lpVtbl->SetProperty(view, DIPROP_BUFFERSIZE, &prop.diph) ==
              DIERR_ACQUIRED);
        CHECK(view->lpVtbl->Poll(view) == DI_NOEFFECT &&
              view->lpVtbl->RunControlPanel(view, h, 0) == DI_OK);
        CHECK(view->lpVtbl->Unacquire(view) == DI_OK);
        CHECK(view->lpVtbl->GetDeviceState(view, kind ? 16 : 256, state) ==
              DIERR_NOTACQUIRED);
        if (kind)
          CHECK(view->lpVtbl->SetDataFormat(view, &c_dfDIMouse2) == DI_OK);
        CHECK(view->lpVtbl->Initialize(view, 0, versions[v],
                                       kind ? &GUID_SysMouse
                                            : &GUID_SysKeyboard) == DI_OK);
        CHECK(view->lpVtbl->BuildActionMap(view, 0, 0, 0) == DIERR_UNSUPPORTED);
        void *effect = (void *)1;
        CHECK(view->lpVtbl->CreateEffect(view, 0, 0, &effect, 0) ==
                  DIERR_UNSUPPORTED &&
              !effect);
        CHECK(view->lpVtbl->EnumCreatedEffectObjects(view, 0, 0, 0) ==
              DIERR_UNSUPPORTED);
        view->lpVtbl->Release(view);
        CHECK(d->lpVtbl->Release(d) == 0);
      }
      if (v == 3) {
        const IDirectInput8Vtbl *vt = (const void *)input->lpVtbl;
        CHECK(vt->EnumDevicesBySemantics(input, 0, 0, 0, 0, 0) == DI_OK);
        CHECK(vt->ConfigureDevices(input, 0, 0, 0, 0) == DIERR_UNSUPPORTED);
      } else {
        IDirectInputDevice *d = 0;
        CHECK(input->lpVtbl->CreateDeviceEx(input, &GUID_SysKeyboard,
                                            diids[v * 2 + wide], (void **)&d,
                                            0) == DI_OK);
        d->lpVtbl->Release(d);
      }
      CHECK(input->lpVtbl->Release(input) == 0);
    }
  puts("PASS all interface versions A/W, sizes, objects and standard formats");
}
static void stream(HWND h) {
  IDirectInput *input = 0;
  CHECK(DirectInput8Create(0, 0x800, &IID_IDirectInput8A, (void **)&input, 0) ==
        DI_OK);
  HWND popup =
      CreateWindowExA(0, "input fixture", "Owned", WS_POPUP | WS_VISIBLE, 15,
                      15, 60, 60, h, 0, 0, 0);
  HWND child_window =
      CreateWindowExA(0, "input fixture", "Child", WS_CHILD | WS_VISIBLE, 1, 1,
                      20, 20, h, 0, 0, 0);
  CHECK(popup && child_window && GetParent(popup) == h);
  IDirectInputDevice *probe = 0;
  CHECK(input->lpVtbl->CreateDevice(input, &GUID_SysKeyboard, &probe, 0) ==
        DI_OK);
  CHECK(probe->lpVtbl->SetCooperativeLevel(
            probe, popup, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE) == DI_OK);
  CHECK(probe->lpVtbl->SetCooperativeLevel(
            probe, child_window, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE) ==
        DIERR_INVALIDPARAM);
  CHECK(probe->lpVtbl->Release(probe) == 0);
  CHECK(DestroyWindow(child_window) && DestroyWindow(popup));
  IDirectInputDevice *mouse = device(input, &GUID_SysMouse, &c_dfDIMouse2, h,
                                     DISCL_EXCLUSIVE | DISCL_FOREGROUND);
  const IDirectInputDeviceVtbl *vt = mouse->lpVtbl;
  DIPROPDWORD prop = {{20, 16, 0, 0}, 2};
  CHECK(vt->SetProperty(mouse, DIPROP_BUFFERSIZE, &prop.diph) == DI_OK);
  CHECK(vt->SetEventNotification(mouse, (HANDLE)77) == DI_OK);
  RECT ordinary = {15, 40, 65, 80};
  CHECK(SetCapture(h) == 0);
  CHECK(ClipCursor(&ordinary));
  CHECK(vt->Acquire(mouse) == DI_OK);
  RECT actual;
  CHECK(GetClipCursor(&actual) && memcmp(&actual, &ordinary, sizeof(actual)));
  CHECK(CiukiUserInputPointer(h, 3));
  for (UINT i = 0; i < sizeof(raw_session) / sizeof(*raw_session); i++) {
    const int *r = raw_session[i];
    di_event(h, r[0], r[1], r[3], r[4], r[5], r[6]);
    if (!i)
      CHECK(SetCursorPos(55, 60));
  }
  CHECK(notifications == 4);
  DIMOUSESTATE2 state;
  CHECK(vt->GetDeviceState(mouse, 20, &state) == DI_OK && state.lX == 13 &&
        state.lY == -6);
  CHECK(vt->GetDeviceState(mouse, 20, &state) == DI_OK && !state.lX &&
        !state.lY);
  DIDEVICEOBJECTDATA data[8];
  DWORD count = DI_INFINITE;
  CHECK(vt->GetDeviceData(mouse, 20, data, &count, DIGDD_PEEK) ==
            DI_BUFFEROVERFLOW &&
        count == 2);
  CHECK(data[0].dwOfs == 0 && data[0].dwData == 17 &&
        data[0].dwTimeStamp == 1001 && data[0].uAppData == 0);
  CHECK(data[1].dwOfs == 4 && (LONG)data[1].dwData == -9 &&
        data[0].dwSequence == data[1].dwSequence);
  DWORD first = data[0].dwSequence;
  count = 1;
  CHECK(vt->GetDeviceData(mouse, 16, data, &count, 0) == DI_BUFFEROVERFLOW &&
        count == 1);
  count = DI_INFINITE;
  CHECK(vt->GetDeviceData(mouse, 20, 0, &count, DIGDD_PEEK) == DI_OK &&
        count == 1);
  count = DI_INFINITE;
  CHECK(vt->GetDeviceData(mouse, 20, 0, &count, 0) == DI_OK && count == 1);
  di_event(h, DESK_MOTION, CIUKI_INPUT_WHEEL, 0, 2, 0, 1010);
  CHECK(vt->GetDeviceState(mouse, 20, &state) == DI_OK && state.lZ == 240);
  count = 8;
  CHECK(vt->GetDeviceData(mouse, 20, data, &count, 0) == DI_OK && count == 1 &&
        data[0].dwSequence > first);
  CHECK(vt->Unacquire(mouse) == DI_OK && GetClipCursor(&actual) &&
        !memcmp(&actual, &ordinary, sizeof(actual)));
  CHECK(GetCapture() == h);
  DIOBJECTDATAFORMAT custom[] = {
      {&GUID_YAxis, 0, DIDFT_RELAXIS | DIDFT_MAKEINSTANCE(1), 0},
      {&GUID_Button, 7, DIDFT_BUTTON | DIDFT_MAKEINSTANCE(1), 0},
      {&GUID_XAxis, 8, DIDFT_RELAXIS, 0}};
  DIDATAFORMAT fmt = {sizeof(fmt), sizeof(*custom), DIDF_RELAXIS, 12,
                      3,           custom};
  CHECK(vt->SetDataFormat(mouse, &fmt) == DI_OK);
  DIDEVICEOBJECTINSTANCEA info = {.dwSize = sizeof(info)};
  CHECK(vt->GetObjectInfo(mouse, &info, 0, DIPH_BYOFFSET) == DI_OK &&
        info.guidType.Data1 == GUID_YAxis.Data1);
  CHECK(vt->GetObjectInfo(mouse, &info, 99, DIPH_BYOFFSET) == DIERR_NOTFOUND);
  custom[2].dwOfs = 0;
  CHECK(vt->SetDataFormat(mouse, &fmt) == DIERR_INVALIDPARAM);
  custom[2].dwOfs = 8;
  fmt.dwObjSize--;
  CHECK(vt->SetDataFormat(mouse, &fmt) == DIERR_INVALIDPARAM);
  fmt.dwObjSize++;
  CHECK(vt->Acquire(mouse) == DI_OK);
  di_event(h, DESK_V3_RELATIVE_EVENT, CIUKI_INPUT_MOTION, 0, 6, -2, 1020);
  di_event(h, DESK_BUTTON, CIUKI_INPUT_BUTTON, 2, 1, 0, 1021);
  uint8_t packed[12];
  CHECK(vt->GetDeviceState(mouse, 12, packed) == DI_OK &&
        (LONG)w32_u32(packed) == -2 && packed[7] == 128 &&
        w32_u32(packed + 8) == 6);
  CHECK(vt->Unacquire(mouse) == DI_OK);
  prop = (DIPROPDWORD){{20, 16, 0, 0}, DIPROPAXISMODE_ABS};
  CHECK(vt->SetProperty(mouse, DIPROP_AXISMODE, &prop.diph) == DI_OK);
  DIPROPRANGE range = {{24, 16, 0, DIPH_BYOFFSET}, -100, 100};
  CHECK(vt->SetProperty(mouse, DIPROP_RANGE, &range.diph) == DI_OK);
  prop = (DIPROPDWORD){{20, 16, 0, DIPH_BYOFFSET}, 0};
  CHECK(vt->GetProperty(mouse, DIPROP_GRANULARITY, &prop.diph) == DI_OK &&
        prop.dwData == 1);
  prop.dwData = 300;
  CHECK(vt->SetProperty(mouse, DIPROP_DEADZONE, &prop.diph) == DI_OK);
  prop.dwData = 0;
  CHECK(vt->GetProperty(mouse, DIPROP_DEADZONE, &prop.diph) == DI_OK &&
        prop.dwData == 300);
  CHECK(vt->Acquire(mouse) == DI_OK && SetCursorPos(55, 60));
  CHECK(vt->GetDeviceState(mouse, 12, packed) == DI_OK &&
        w32_u32(packed) == 60 && w32_u32(packed + 8) == 55);
  IDirectInputDevice *conflict = device(input, &GUID_SysMouse, &c_dfDIMouse, h,
                                        DISCL_EXCLUSIVE | DISCL_FOREGROUND);
  CHECK(conflict->lpVtbl->Acquire(conflict) == DIERR_OTHERAPPHASPRIO);
  conflict->lpVtbl->Release(conflict);
  IDirectInputDevice *keyboard =
      device(input, &GUID_SysKeyboard, &c_dfDIKeyboard, h,
             DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
  CHECK(keyboard->lpVtbl->SetCooperativeLevel(
            keyboard, h, DISCL_BACKGROUND | DISCL_EXCLUSIVE) ==
        DIERR_UNSUPPORTED);
  CHECK(keyboard->lpVtbl->Acquire(keyboard) == DI_OK);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x200, 1, 0, 1028);
  uint8_t pause_keys[256];
  CHECK(keyboard->lpVtbl->GetDeviceState(keyboard, 256, pause_keys) == DI_OK &&
        pause_keys[0xc5] == 128);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x200, 0, 0, 1029);
  CHECK(keyboard->lpVtbl->GetDeviceState(keyboard, 256, pause_keys) == DI_OK &&
        !pause_keys[0xc5]);
  di_event(h, DESK_KEY, CIUKI_INPUT_KEY, 0x1e, 1, 0, 1030);
  uint8_t keys[256];
  CHECK(keyboard->lpVtbl->GetDeviceState(keyboard, 256, keys) == DI_OK &&
        keys[0x1e] == 128);
  UINT off[] = {0, 0};
  event(u32_window(h), DESK_V3_ACTIVATE, off, 2);
  u32_pump();
  CHECK(vt->GetDeviceState(mouse, 12, packed) == DIERR_INPUTLOST);
  CHECK(vt->Acquire(mouse) == DIERR_OTHERAPPHASPRIO);
  CHECK(keyboard->lpVtbl->GetDeviceState(keyboard, 256, keys) == DI_OK &&
        !keys[0x1e]);
  CHECK(!GetAsyncKeyState('A') && !GetKeyState('A'));
  CHECK(keyboard->lpVtbl->Unacquire(keyboard) == DI_OK &&
        keyboard->lpVtbl->Acquire(keyboard) == DI_OK);
  UINT on[] = {1, 1};
  event(u32_window(h), DESK_V3_ACTIVATE, on, 2);
  u32_pump();
  CHECK(vt->Acquire(mouse) == DI_OK);
  di_event(h, DESK_KEY, CIUKI_INPUT_RESYNC, 0, 0, 0, 1040);
  CHECK(vt->GetDeviceState(mouse, 12, packed) == DIERR_INPUTLOST);
  CHECK(keyboard->lpVtbl->GetDeviceState(keyboard, 256, keys) == DI_OK &&
        !keys[0x1e]);
  CHECK(vt->Unacquire(mouse) == DI_NOEFFECT &&
        vt->GetDeviceState(mouse, 12, packed) == DIERR_NOTACQUIRED);
  CHECK(vt->Release(mouse) == 0);
  CHECK(keyboard->lpVtbl->Release(keyboard) == 0);
  CHECK(input->lpVtbl->Release(input) == 0);
  ClipCursor(0);
  ReleaseCapture();
  puts("PASS buffered timestamps sequence peek overflow notification and raw "
       "counts independent of warp");
  puts("PASS foreground exclusive loss/reacquire/resync and background "
       "nonexclusive cleared state");
}
static void input_run(void) {
  ui_kernel_init();
  struct wr_io io;
  char dir[] = "build/host/win32/input/state-XXXXXX";
  CHECK(mkdtemp(dir));
  wr_posix_io(&io, dir);
  CHECK(wr_init(&server, &io, 0));
  struct u32_backend backend = {ui_kernel_call, w32r_gate,     w32r_exchange,
                                ui_kernel_tid,  ui_kernel_pid, ui_kernel_ticks,
                                surface,        drop,          ui_kernel_pause};
  u32_set_backend(&backend);
  u32_set_gdi(&test_gdi);
  WNDCLASSA cls = {.lpfnWndProc = procedure, .lpszClassName = "input fixture"};
  CHECK(RegisterClassA(&cls));
  HWND h = CreateWindowExA(0, "input fixture", "Input", WS_VISIBLE | WS_CAPTION,
                           10, 10, 100, 100, 0, 0, 0, 0);
  CHECK(h);
  UINT on[] = {1, 1};
  event(u32_window(h), DESK_V3_ACTIVATE, on, 2);
  u32_pump();
  layouts();
  message_edges(h);
  versions(h);
  stream(h);
  CHECK(DestroyWindow(h));
  CHECK(UnregisterClassA("input fixture", 0));
  wr_destroy(&server);
  for (UINT i = 0; i < 2; i++) {
    char path[256];
    snprintf(path, sizeof(path), "%s/SERVICE.CRS.%u", dir, i);
    unlink(path);
  }
  rmdir(dir);
  CHECK(!allocations);
  ui_kernel_finish();
  puts("PASS INPUT production USER/v3 DirectInput");
}
int main(void) {
  setbuf(stdout, 0);
  setbuf(stderr, 0);
  void *stack = mmap(0, 4 * 1024 * 1024, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  assert(stack != MAP_FAILED);
  ucontext_t current, low;
  getcontext(&low);
  low.uc_stack.ss_sp = stack;
  low.uc_stack.ss_size = 4 * 1024 * 1024;
  low.uc_link = &current;
  makecontext(&low, input_run, 0);
  swapcontext(&current, &low);
  munmap(stack, 4 * 1024 * 1024);
  return 0;
}
