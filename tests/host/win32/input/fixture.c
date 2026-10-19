/* Authored PE input fixture, supplement to f5-02's loader corpus.
 * Exact i386 stdcall calls through dinput/dinput8 and their COM vtables.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/dinput.h>
extern DWORD W32API AdapterInput(DWORD, LONG, LONG, DWORD);
#define EXPECT(c, n)                                                           \
  do {                                                                         \
    if (!(c))                                                                  \
      return n;                                                                \
  } while (0)
int entry(void) {
  IDirectInput *old = 0, *input = 0;
  EXPECT(DirectInputCreateA(0, 0x300, &old, 0) == DI_OK, 1);
  EXPECT(DirectInput8Create(0, 0x800, &IID_IDirectInput8A, (void **)&input,
                            0) == DI_OK,
         2);
  AdapterInput(10, 1, 0, 0);
  IDirectInputDevice *mouse = 0, *keyboard = 0;
  EXPECT(input->lpVtbl->CreateDevice(input, &GUID_SysMouse, &mouse, 0) == DI_OK,
         3);
  EXPECT(old->lpVtbl->CreateDevice(old, &GUID_SysKeyboard, &keyboard, 0) ==
             DI_OK,
         4);
  const IDirectInputDeviceVtbl *m = mouse->lpVtbl, *k = keyboard->lpVtbl;
  EXPECT(m->SetDataFormat(mouse, &c_dfDIMouse2) == DI_OK, 5);
  EXPECT(k->SetDataFormat(keyboard, &c_dfDIKeyboard) == DI_OK, 6);
  EXPECT(m->SetCooperativeLevel(mouse, (HWND)1, 5) == DI_OK, 7);
  EXPECT(k->SetCooperativeLevel(keyboard, (HWND)1, 10) == DI_OK, 8);
  DIPROPDWORD buffer = {{20, 16, 0, 0}, 8};
  EXPECT(m->SetProperty(mouse, DIPROP_BUFFERSIZE, &buffer.diph) == DI_OK, 9);
  EXPECT(m->SetEventNotification(mouse, (HANDLE)77) == DI_OK, 10);
  EXPECT(m->Acquire(mouse) == DI_OK && k->Acquire(keyboard) == DI_OK, 11);
  EXPECT(AdapterInput(12, 0, 0, 0) == 3, 12);
  AdapterInput(1, 17, -9, 111);
  AdapterInput(11, 500, 400, 0);
  AdapterInput(1, -4, 3, 112);
  /* One notification per buffered axis record (two per motion). */
  EXPECT(AdapterInput(13, 0, 0, 0) == 4, 26);
  DIMOUSESTATE2 state;
  EXPECT(m->GetDeviceState(mouse, 20, &state) == DI_OK && state.lX == 13 &&
             state.lY == -6,
         13);
  EXPECT(m->GetDeviceState(mouse, 20, &state) == DI_OK && state.lX == 0, 14);
  DIDEVICEOBJECTDATA data[8];
  DWORD count = 8;
  EXPECT(m->GetDeviceData(mouse, 20, data, &count, DIGDD_PEEK) == DI_OK &&
             count == 4,
         15);
  EXPECT(data[0].dwTimeStamp == 111 && data[0].dwSequence < data[2].dwSequence,
         16);
  count = 8;
  EXPECT(m->GetDeviceData(mouse, 16, data, &count, 0) == DI_OK && count == 4,
         17);
  AdapterInput(2, 1, 0, 113);
  uint8_t keys[256];
  EXPECT(k->GetDeviceState(keyboard, 256, keys) == DI_OK && keys[0x1e] == 128,
         18);
  AdapterInput(10, 0, 0, 0);
  EXPECT(m->GetDeviceState(mouse, 20, &state) == DIERR_INPUTLOST, 19);
  EXPECT(k->GetDeviceState(keyboard, 256, keys) == DI_OK && keys[0x1e] == 0,
         20);
  AdapterInput(10, 1, 0, 0);
  EXPECT(m->Acquire(mouse) == DI_OK, 21);
  EXPECT(m->Unacquire(mouse) == DI_OK &&
             m->GetDeviceState(mouse, 20, &state) == DIERR_NOTACQUIRED,
         22);
  EXPECT(m->Release(mouse) == 0 && k->Release(keyboard) == 0, 23);
  EXPECT(input->lpVtbl->Release(input) == 0 && old->lpVtbl->Release(old) == 0,
         24);
  /* Loss of acquisition is a device state change that also sets the event
   * (Microsoft, IDirectInputDevice8::SetEventNotification): 4 records + 1. */
  EXPECT(AdapterInput(14, 0, 0, 0) == 0 && AdapterInput(13, 0, 0, 0) == 5, 25);
  return 0;
}
