/* Literal public ABI oracle: native host and i686 compile the same layout.
 * SPDX-License-Identifier: MIT */
#include <ciuki/abi.h>
#define OFFSET(type, field, expected) _Static_assert(__builtin_offsetof(struct type,field)==expected,#type "." #field)
_Static_assert(CIUKI_HID_VERSION==1 && INPUT_HID_GET_CAPS==0x4901 && INPUT_HID_GET_STATE==0x4902 && INPUT_HID_CALIBRATE==0x4903,"HID version/commands");
_Static_assert(CIUKI_INPUT_KEY==1 && CIUKI_INPUT_RESYNC==5 && CIUKI_INPUT_WHEEL==6,"existing input ABI");
_Static_assert(CIUKI_INPUT_USAGE==7 && CIUKI_INPUT_CONSUMER==8 && CIUKI_INPUT_JOY_AXIS==9 && CIUKI_INPUT_JOY_HAT==10 && CIUKI_INPUT_JOY_BUTTON==11 && CIUKI_INPUT_DEVICE==12,"input extension types");
_Static_assert(CIUKI_CONSUMER_VOLUME_UP==1 && CIUKI_CONSUMER_VOLUME_DOWN==2 && CIUKI_CONSUMER_MUTE==3 && CIUKI_CONSUMER_PLAY_PAUSE==4 && CIUKI_CONSUMER_NEXT==5 && CIUKI_CONSUMER_PREVIOUS==6,"consumer actions");
_Static_assert(sizeof(struct ciuki_hid_axis)==28 && sizeof(struct ciuki_hid_caps)==340 && sizeof(struct ciuki_hid_state)==116 && sizeof(struct ciuki_hid_calibration)==32,"HID wire sizes");
_Static_assert(_Alignof(struct ciuki_hid_caps)==4 && _Alignof(struct ciuki_hid_state)==4,"HID wire alignment");
_Static_assert(sizeof(struct ciuki_input_event)==40,"existing event size");
OFFSET(ciuki_input_event,value2,32); OFFSET(ciuki_input_event,lost_count,36);
OFFSET(ciuki_hid_caps,version,0); OFFSET(ciuki_hid_caps,size,4); OFFSET(ciuki_hid_caps,device,8); OFFSET(ciuki_hid_caps,generation,12);
OFFSET(ciuki_hid_caps,attached,16); OFFSET(ciuki_hid_caps,classes,20); OFFSET(ciuki_hid_caps,axes,24); OFFSET(ciuki_hid_caps,hats,28);
OFFSET(ciuki_hid_caps,buttons,32); OFFSET(ciuki_hid_caps,consumers,36); OFFSET(ciuki_hid_caps,host,40); OFFSET(ciuki_hid_caps,address,44);
OFFSET(ciuki_hid_caps,interface,48); OFFSET(ciuki_hid_caps,reserved,52); OFFSET(ciuki_hid_caps,keyboard,56); OFFSET(ciuki_hid_caps,axis,88);
OFFSET(ciuki_hid_state,attached,16); OFFSET(ciuki_hid_state,consumers,20); OFFSET(ciuki_hid_state,buttons,24); OFFSET(ciuki_hid_state,reserved,28);
OFFSET(ciuki_hid_state,keyboard,32); OFFSET(ciuki_hid_state,axes,64); OFFSET(ciuki_hid_state,hats,100);
OFFSET(ciuki_hid_axis,physical_minimum,12); OFFSET(ciuki_hid_axis,unit_exponent,24);
OFFSET(ciuki_hid_calibration,axis,16); OFFSET(ciuki_hid_calibration,minimum,20); OFFSET(ciuki_hid_calibration,centre,24); OFFSET(ciuki_hid_calibration,maximum,28);
