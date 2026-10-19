/* DirectInput PE32 ABI, original CiukiOS declarations from published SDK facts.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_DINPUT_H
#define CIUKI_W32_DINPUT_H
#include <w32/user32.h>
typedef int32_t HRESULT;
typedef struct {
  DWORD Data1;
  uint16_t Data2, Data3;
  uint8_t Data4[8];
} GUID;
#define DI_OK ((HRESULT)0)
#define DI_NOEFFECT ((HRESULT)1)
#define DI_BUFFEROVERFLOW ((HRESULT)1)
#define DIERR_INVALIDPARAM ((HRESULT)0x80070057)
#define DIERR_NOINTERFACE ((HRESULT)0x80004002)
#define DIERR_UNSUPPORTED ((HRESULT)0x80004001)
#define DIERR_OUTOFMEMORY ((HRESULT)0x8007000e)
#define DIERR_NOTFOUND ((HRESULT)0x80070002)
#define DIERR_OTHERAPPHASPRIO ((HRESULT)0x80070005)
#define DIERR_INPUTLOST ((HRESULT)0x8007001e)
#define DIERR_ACQUIRED ((HRESULT)0x800700aa)
#define DIERR_NOTACQUIRED ((HRESULT)0x8007000c)
#define DIERR_NOTINITIALIZED ((HRESULT)0x80070015)
#define DIERR_NOTBUFFERED ((HRESULT)0x80040207)
#define DIERR_NOAGGREGATION ((HRESULT)0x80040110)
#define DISCL_EXCLUSIVE 1u
#define DISCL_NONEXCLUSIVE 2u
#define DISCL_FOREGROUND 4u
#define DISCL_BACKGROUND 8u
#define DISCL_NOWINKEY 16u
#define DIEDFL_ATTACHEDONLY 1u
#define DI8DEVCLASS_ALL 0u
#define DI8DEVCLASS_DEVICE 1u
#define DI8DEVCLASS_POINTER 2u
#define DI8DEVCLASS_KEYBOARD 3u
#define DI8DEVCLASS_GAMECTRL 4u
#define DIDF_ABSAXIS 1u
#define DIDF_RELAXIS 2u
#define DIDFT_ALL 0u
#define DIDFT_RELAXIS 1u
#define DIDFT_ABSAXIS 2u
#define DIDFT_AXIS 3u
#define DIDFT_PSHBUTTON 4u
#define DIDFT_TGLBUTTON 8u
#define DIDFT_BUTTON 12u
#define DIDFT_ANYINSTANCE 0x00ffff00u
#define DIDFT_OPTIONAL 0x80000000u
#define DIDFT_MAKEINSTANCE(n) ((DWORD)(n) << 8)
#define DIPH_DEVICE 0u
#define DIPH_BYOFFSET 1u
#define DIPH_BYID 2u
#define DIPROP_BUFFERSIZE ((const GUID *)(uintptr_t)1)
#define DIPROP_AXISMODE ((const GUID *)(uintptr_t)2)
#define DIPROP_GRANULARITY ((const GUID *)(uintptr_t)3)
#define DIPROP_RANGE ((const GUID *)(uintptr_t)4)
#define DIPROP_DEADZONE ((const GUID *)(uintptr_t)5)
#define DIPROPAXISMODE_ABS 0u
#define DIPROPAXISMODE_REL 1u
#define DIGDD_PEEK 1u
#define DI_INFINITE 0xffffffffu
extern const GUID GUID_SysKeyboard, GUID_SysMouse, GUID_Key, GUID_Button,
    GUID_XAxis, GUID_YAxis, GUID_ZAxis;
extern const GUID IID_IDirectInputA, IID_IDirectInputW, IID_IDirectInput2A,
    IID_IDirectInput2W;
extern const GUID IID_IDirectInput7A, IID_IDirectInput7W, IID_IDirectInput8A,
    IID_IDirectInput8W;
extern const GUID IID_IDirectInputDeviceA, IID_IDirectInputDeviceW,
    IID_IDirectInputDevice2A, IID_IDirectInputDevice2W;
extern const GUID IID_IDirectInputDevice7A, IID_IDirectInputDevice7W,
    IID_IDirectInputDevice8A, IID_IDirectInputDevice8W;
typedef struct {
  const GUID *pguid;
  DWORD dwOfs, dwType, dwFlags;
} DIOBJECTDATAFORMAT;
typedef struct {
  DWORD dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs;
  const DIOBJECTDATAFORMAT *rgodf;
} DIDATAFORMAT;
typedef struct {
  DWORD dwOfs, dwData, dwTimeStamp, dwSequence;
} DIDEVICEOBJECTDATA_DX3;
/* PE32 app data is a DWORD even in native host models. */
typedef struct {
  DWORD dwOfs, dwData, dwTimeStamp, dwSequence, uAppData;
} DIDEVICEOBJECTDATA;
typedef struct {
  DWORD dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs;
} DIDEVCAPS_DX3;
typedef struct {
  DWORD dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs;
  DWORD dwFFSamplePeriod, dwFFMinTimeResolution, dwFirmwareRevision,
      dwHardwareRevision, dwFFDriverVersion;
} DIDEVCAPS;
typedef struct {
  DWORD dwSize;
  GUID guidInstance, guidProduct;
  DWORD dwDevType;
  char tszInstanceName[260], tszProductName[260];
} DIDEVICEINSTANCE_DX3A;
typedef struct {
  DWORD dwSize;
  GUID guidInstance, guidProduct;
  DWORD dwDevType;
  WCHAR tszInstanceName[260], tszProductName[260];
} DIDEVICEINSTANCE_DX3W;
typedef struct {
  DWORD dwSize;
  GUID guidInstance, guidProduct;
  DWORD dwDevType;
  char tszInstanceName[260], tszProductName[260];
  GUID guidFFDriver;
  uint16_t wUsagePage, wUsage;
} DIDEVICEINSTANCEA;
typedef struct {
  DWORD dwSize;
  GUID guidInstance, guidProduct;
  DWORD dwDevType;
  WCHAR tszInstanceName[260], tszProductName[260];
  GUID guidFFDriver;
  uint16_t wUsagePage, wUsage;
} DIDEVICEINSTANCEW;
typedef struct {
  DWORD dwSize;
  GUID guidType;
  DWORD dwOfs, dwType, dwFlags;
  char tszName[260];
} DIDEVICEOBJECTINSTANCE_DX3A;
typedef struct {
  DWORD dwSize;
  GUID guidType;
  DWORD dwOfs, dwType, dwFlags;
  WCHAR tszName[260];
} DIDEVICEOBJECTINSTANCE_DX3W;
typedef struct {
  DWORD dwSize;
  GUID guidType;
  DWORD dwOfs, dwType, dwFlags;
  char tszName[260];
  DWORD dwFFMaxForce, dwFFForceResolution;
  uint16_t wCollectionNumber, wDesignatorIndex, wUsagePage, wUsage;
  DWORD dwDimension;
  uint16_t wExponent, wReportId;
} DIDEVICEOBJECTINSTANCEA;
typedef struct {
  DWORD dwSize;
  GUID guidType;
  DWORD dwOfs, dwType, dwFlags;
  WCHAR tszName[260];
  DWORD dwFFMaxForce, dwFFForceResolution;
  uint16_t wCollectionNumber, wDesignatorIndex, wUsagePage, wUsage;
  DWORD dwDimension;
  uint16_t wExponent, wReportId;
} DIDEVICEOBJECTINSTANCEW;
typedef struct {
  DWORD dwSize, dwHeaderSize, dwObj, dwHow;
} DIPROPHEADER;
typedef struct {
  DIPROPHEADER diph;
  DWORD dwData;
} DIPROPDWORD;
typedef struct {
  DIPROPHEADER diph;
  LONG lMin, lMax;
} DIPROPRANGE;
typedef struct {
  LONG lX, lY, lZ;
  uint8_t rgbButtons[4];
} DIMOUSESTATE;
typedef struct {
  LONG lX, lY, lZ;
  uint8_t rgbButtons[8];
} DIMOUSESTATE2;
extern const DIDATAFORMAT c_dfDIKeyboard, c_dfDIMouse, c_dfDIMouse2;
typedef BOOL(W32API *DIENUM)(const void *, void *);
typedef struct IDirectInput IDirectInput;
typedef struct IDirectInputDevice IDirectInputDevice;
typedef struct {
  HRESULT(W32API *QueryInterface)(IDirectInputDevice *, const GUID *, void **);
  ULONG(W32API *AddRef)(IDirectInputDevice *);
  ULONG(W32API *Release)(IDirectInputDevice *);
  HRESULT(W32API *GetCapabilities)(IDirectInputDevice *, DIDEVCAPS *);
  HRESULT(W32API *EnumObjects)(IDirectInputDevice *, DIENUM, void *, DWORD);
  HRESULT(W32API *GetProperty)(IDirectInputDevice *, const GUID *,
                               DIPROPHEADER *);
  HRESULT(W32API *SetProperty)(IDirectInputDevice *, const GUID *,
                               const DIPROPHEADER *);
  HRESULT(W32API *Acquire)(IDirectInputDevice *);
  HRESULT(W32API *Unacquire)(IDirectInputDevice *);
  HRESULT(W32API *GetDeviceState)(IDirectInputDevice *, DWORD, void *);
  HRESULT(W32API *GetDeviceData)(IDirectInputDevice *, DWORD, void *, DWORD *,
                                 DWORD);
  HRESULT(W32API *SetDataFormat)(IDirectInputDevice *, const DIDATAFORMAT *);
  HRESULT(W32API *SetEventNotification)(IDirectInputDevice *, HANDLE);
  HRESULT(W32API *SetCooperativeLevel)(IDirectInputDevice *, HWND, DWORD);
  HRESULT(W32API *GetObjectInfo)(IDirectInputDevice *, void *, DWORD, DWORD);
  HRESULT(W32API *GetDeviceInfo)(IDirectInputDevice *, void *);
  HRESULT(W32API *RunControlPanel)(IDirectInputDevice *, HWND, DWORD);
  HRESULT(W32API *Initialize)(IDirectInputDevice *, HINSTANCE, DWORD,
                              const GUID *);
  HRESULT(W32API *CreateEffect)(IDirectInputDevice *, const GUID *,
                                const void *, void **, void *);
  HRESULT(W32API *EnumEffects)(IDirectInputDevice *, DIENUM, void *, DWORD);
  HRESULT(W32API *GetEffectInfo)(IDirectInputDevice *, void *, const GUID *);
  HRESULT(W32API *GetForceFeedbackState)(IDirectInputDevice *, DWORD *);
  HRESULT(W32API *SendForceFeedbackCommand)(IDirectInputDevice *, DWORD);
  HRESULT(W32API *EnumCreatedEffectObjects)(IDirectInputDevice *, DIENUM,
                                            void *, DWORD);
  HRESULT(W32API *Escape)(IDirectInputDevice *, void *);
  HRESULT(W32API *Poll)(IDirectInputDevice *);
  HRESULT(W32API *SendDeviceData)(IDirectInputDevice *, DWORD, const void *,
                                  DWORD *, DWORD);
  HRESULT(W32API *EnumEffectsInFile)(IDirectInputDevice *, const void *, DIENUM,
                                     void *, DWORD);
  HRESULT(W32API *WriteEffectToFile)(IDirectInputDevice *, const void *, DWORD,
                                     const void *, DWORD);
  HRESULT(W32API *BuildActionMap)(IDirectInputDevice *, void *, const void *,
                                  DWORD);
  HRESULT(W32API *SetActionMap)(IDirectInputDevice *, void *, const void *,
                                DWORD);
  HRESULT(W32API *GetImageInfo)(IDirectInputDevice *, void *);
} IDirectInputDeviceVtbl;
struct IDirectInputDevice {
  const IDirectInputDeviceVtbl *lpVtbl;
};
typedef struct {
  HRESULT(W32API *QueryInterface)(IDirectInput *, const GUID *, void **);
  ULONG(W32API *AddRef)(IDirectInput *);
  ULONG(W32API *Release)(IDirectInput *);
  HRESULT(W32API *CreateDevice)(IDirectInput *, const GUID *,
                                IDirectInputDevice **, void *);
  HRESULT(W32API *EnumDevices)(IDirectInput *, DWORD, DIENUM, void *, DWORD);
  HRESULT(W32API *GetDeviceStatus)(IDirectInput *, const GUID *);
  HRESULT(W32API *RunControlPanel)(IDirectInput *, HWND, DWORD);
  HRESULT(W32API *Initialize)(IDirectInput *, HINSTANCE, DWORD);
  HRESULT(W32API *FindDevice)(IDirectInput *, const GUID *, const void *,
                              GUID *);
  /* Slot 9 is CreateDeviceEx on v7; different on v8. */
  HRESULT(W32API *CreateDeviceEx)(IDirectInput *, const GUID *, const GUID *,
                                  void **, void *);
} IDirectInputVtbl;
typedef struct {
  HRESULT(W32API *QueryInterface)(IDirectInput *, const GUID *, void **);
  ULONG(W32API *AddRef)(IDirectInput *);
  ULONG(W32API *Release)(IDirectInput *);
  HRESULT(W32API *CreateDevice)(IDirectInput *, const GUID *,
                                IDirectInputDevice **, void *);
  HRESULT(W32API *EnumDevices)(IDirectInput *, DWORD, DIENUM, void *, DWORD);
  HRESULT(W32API *GetDeviceStatus)(IDirectInput *, const GUID *);
  HRESULT(W32API *RunControlPanel)(IDirectInput *, HWND, DWORD);
  HRESULT(W32API *Initialize)(IDirectInput *, HINSTANCE, DWORD);
  HRESULT(W32API *FindDevice)(IDirectInput *, const GUID *, const void *,
                              GUID *);
  HRESULT(W32API *EnumDevicesBySemantics)(IDirectInput *, const void *, void *,
                                          void *, void *, DWORD);
  HRESULT(W32API *ConfigureDevices)(IDirectInput *, void *, void *, DWORD,
                                    void *);
} IDirectInput8Vtbl;
struct IDirectInput {
  const IDirectInputVtbl *lpVtbl;
};
HRESULT W32API DirectInputCreateA(HINSTANCE, DWORD, IDirectInput **, void *);
HRESULT W32API DirectInputCreateW(HINSTANCE, DWORD, IDirectInput **, void *);
HRESULT W32API DirectInputCreateEx(HINSTANCE, DWORD, const GUID *, void **,
                                   void *);
HRESULT W32API DirectInput8Create(HINSTANCE, DWORD, const GUID *, void **,
                                  void *);
#endif
