/* SDK layout facts, checked on both i686 compiler targets. */
#include <w32/dinput.h>
_Static_assert(sizeof(GUID) == 16, "GUID");
_Static_assert(sizeof(DIDATAFORMAT) == 24 && sizeof(DIOBJECTDATAFORMAT) == 16,
               "format");
_Static_assert(sizeof(DIDEVICEOBJECTDATA_DX3) == 16 &&
                   sizeof(DIDEVICEOBJECTDATA) == 20,
               "buffer versions");
_Static_assert(sizeof(DIDEVCAPS_DX3) == 24 && sizeof(DIDEVCAPS) == 44,
               "caps versions");
_Static_assert(sizeof(DIDEVICEINSTANCE_DX3A) == 560 &&
                   sizeof(DIDEVICEINSTANCEA) == 580,
               "instance ANSI versions");
_Static_assert(sizeof(DIDEVICEINSTANCE_DX3W) == 1080 &&
                   sizeof(DIDEVICEINSTANCEW) == 1100,
               "instance Unicode versions");
_Static_assert(sizeof(DIDEVICEOBJECTINSTANCE_DX3A) == 292 &&
                   sizeof(DIDEVICEOBJECTINSTANCEA) == 316,
               "object ANSI versions");
_Static_assert(sizeof(DIDEVICEOBJECTINSTANCE_DX3W) == 552 &&
                   sizeof(DIDEVICEOBJECTINSTANCEW) == 576,
               "object Unicode versions");
_Static_assert(sizeof(DIPROPHEADER) == 16 && sizeof(DIPROPDWORD) == 20 &&
                   sizeof(DIPROPRANGE) == 24,
               "properties");
_Static_assert(sizeof(DIMOUSESTATE) == 16 && sizeof(DIMOUSESTATE2) == 20,
               "mouse formats");
_Static_assert(offsetof(IDirectInputDeviceVtbl, GetDeviceState) == 9 * 4,
               "GetDeviceState slot");
_Static_assert(offsetof(IDirectInputDeviceVtbl, Poll) == 25 * 4, "Poll slot");
_Static_assert(offsetof(IDirectInputDeviceVtbl, BuildActionMap) == 29 * 4,
               "BuildActionMap slot");
_Static_assert(sizeof(IDirectInputDeviceVtbl) == 32 * 4, "device8 slots");
_Static_assert(offsetof(IDirectInputVtbl, CreateDeviceEx) == 9 * 4,
               "device7 factory slot");
_Static_assert(offsetof(IDirectInput8Vtbl, EnumDevicesBySemantics) == 9 * 4 &&
                   sizeof(IDirectInput8Vtbl) == 11 * 4,
               "input8 slots");
