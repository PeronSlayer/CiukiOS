/* Private f5-12B MCI boundary: winmm core <-> device drivers (internal
 * waveaudio/sequencer, mciavi32.dll avivideo). Constants and parameter
 * blocks follow the Windows 95/98 Platform SDK mmsystem.h/digitalv.h/vfw.h
 * values; layouts are the i386 ones (pointer-sized fields are uintptr_t so
 * the native host model keeps the same field order).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32_MCI_H
#define W32_MCI_H
#include <w32/base.h>

#define MCI_OPEN_DRIVER 0x0801u
#define MCI_CLOSE_DRIVER 0x0802u
#define MCI_OPEN 0x0803u
#define MCI_CLOSE 0x0804u
#define MCI_ESCAPE 0x0805u
#define MCI_PLAY 0x0806u
#define MCI_SEEK 0x0807u
#define MCI_STOP 0x0808u
#define MCI_PAUSE 0x0809u
#define MCI_INFO 0x080Au
#define MCI_GETDEVCAPS 0x080Bu
#define MCI_SPIN 0x080Cu
#define MCI_SET 0x080Du
#define MCI_STEP 0x080Eu
#define MCI_RECORD 0x080Fu
#define MCI_SYSINFO 0x0810u
#define MCI_BREAK 0x0811u
#define MCI_SAVE 0x0813u
#define MCI_STATUS 0x0814u
#define MCI_CUE 0x0830u
#define MCI_REALIZE 0x0840u
#define MCI_WINDOW 0x0841u
#define MCI_PUT 0x0842u
#define MCI_WHERE 0x0843u
#define MCI_FREEZE 0x0844u
#define MCI_UNFREEZE 0x0845u
#define MCI_LOAD 0x0850u
#define MCI_CUT 0x0851u
#define MCI_COPY 0x0852u
#define MCI_PASTE 0x0853u
#define MCI_UPDATE 0x0854u
#define MCI_RESUME 0x0855u
#define MCI_DELETE 0x0856u
#define MCI_CONFIGURE 0x087Au
#define MM_MCINOTIFY 0x03B9u
#define MCI_ALL_DEVICE_ID 0xFFFFu

#define MCI_NOTIFY 0x1u
#define MCI_WAIT 0x2u
#define MCI_FROM 0x4u
#define MCI_TO 0x8u
#define MCI_TRACK 0x10u
#define MCI_NOTIFY_SUCCESSFUL 1u
#define MCI_NOTIFY_SUPERSEDED 2u
#define MCI_NOTIFY_ABORTED 4u
#define MCI_NOTIFY_FAILURE 8u

#define MCI_OPEN_SHAREABLE 0x100u
#define MCI_OPEN_ELEMENT 0x200u
#define MCI_OPEN_ALIAS 0x400u
#define MCI_OPEN_ELEMENT_ID 0x800u
#define MCI_OPEN_TYPE_ID 0x1000u
#define MCI_OPEN_TYPE 0x2000u
#define MCI_SEEK_TO_START 0x100u
#define MCI_SEEK_TO_END 0x200u
#define MCI_STATUS_ITEM 0x100u
#define MCI_STATUS_START 0x200u
#define MCI_STATUS_LENGTH 1u
#define MCI_STATUS_POSITION 2u
#define MCI_STATUS_NUMBER_OF_TRACKS 3u
#define MCI_STATUS_MODE 4u
#define MCI_STATUS_MEDIA_PRESENT 5u
#define MCI_STATUS_TIME_FORMAT 6u
#define MCI_STATUS_READY 7u
#define MCI_STATUS_CURRENT_TRACK 8u
#define MCI_INFO_PRODUCT 0x100u
#define MCI_INFO_FILE 0x200u
#define MCI_INFO_MEDIA_UPC 0x400u
#define MCI_INFO_MEDIA_IDENTITY 0x800u
#define MCI_INFO_NAME 0x1000u
#define MCI_INFO_COPYRIGHT 0x2000u
#define MCI_GETDEVCAPS_ITEM 0x100u
#define MCI_GETDEVCAPS_CAN_RECORD 1u
#define MCI_GETDEVCAPS_HAS_AUDIO 2u
#define MCI_GETDEVCAPS_HAS_VIDEO 3u
#define MCI_GETDEVCAPS_DEVICE_TYPE 4u
#define MCI_GETDEVCAPS_USES_FILES 5u
#define MCI_GETDEVCAPS_COMPOUND_DEVICE 6u
#define MCI_GETDEVCAPS_CAN_EJECT 7u
#define MCI_GETDEVCAPS_CAN_PLAY 8u
#define MCI_GETDEVCAPS_CAN_SAVE 9u
#define MCI_SYSINFO_QUANTITY 0x100u
#define MCI_SYSINFO_OPEN 0x200u
#define MCI_SYSINFO_NAME 0x400u
#define MCI_SYSINFO_INSTALLNAME 0x800u
#define MCI_SET_DOOR_OPEN 0x100u
#define MCI_SET_DOOR_CLOSED 0x200u
#define MCI_SET_TIME_FORMAT 0x400u
#define MCI_SET_AUDIO 0x800u
#define MCI_SET_VIDEO 0x1000u
#define MCI_SET_ON 0x2000u
#define MCI_SET_OFF 0x4000u
#define MCI_BREAK_KEY 0x100u
#define MCI_BREAK_HWND 0x200u
#define MCI_BREAK_OFF 0x400u

#define MCI_FORMAT_MILLISECONDS 0u
#define MCI_FORMAT_HMS 1u
#define MCI_FORMAT_MSF 2u
#define MCI_FORMAT_FRAMES 3u
#define MCI_FORMAT_SMPTE_24 4u
#define MCI_FORMAT_SMPTE_25 5u
#define MCI_FORMAT_SMPTE_30 6u
#define MCI_FORMAT_SMPTE_30DROP 7u
#define MCI_FORMAT_BYTES 8u
#define MCI_FORMAT_SAMPLES 9u
#define MCI_FORMAT_TMSF 10u
#define MCI_SEQ_FORMAT_SONGPTR 0x4001u

/* String resource identifiers of the string interface (mmsystem.h). */
#define MCI_DEVTYPE_CD_AUDIO 516u
#define MCI_DEVTYPE_DIGITAL_VIDEO 520u
#define MCI_DEVTYPE_OTHER 521u
#define MCI_DEVTYPE_WAVEFORM_AUDIO 522u
#define MCI_DEVTYPE_SEQUENCER 523u
#define MCI_MODE_NOT_READY 524u
#define MCI_MODE_STOP 525u
#define MCI_MODE_PLAY 526u
#define MCI_MODE_RECORD 527u
#define MCI_MODE_SEEK 528u
#define MCI_MODE_PAUSE 529u
#define MCI_MODE_OPEN 530u
#define MCI_TRUE 531u
#define MCI_FALSE 532u
#define MCI_FORMAT_RETURN_BASE 533u
#define MCI_CDA_TRACK_AUDIO 1088u
#define MCI_CDA_TRACK_OTHER 1089u
#define MCI_WAVE_PCM 1152u
#define MCI_WAVE_MAPPER 1153u
#define MCI_SEQ_DIV_PPQN 1216u
#define MCI_SEQ_DIV_SMPTE_24 1217u
#define MCI_SEQ_DIV_SMPTE_25 1218u
#define MCI_SEQ_DIV_SMPTE_30DROP 1219u
#define MCI_SEQ_DIV_SMPTE_30 1220u
/* Private resource ids (string interface only, never returned as values). */
#define MCI_SEQ_MAPPER_S 1221u
#define MCI_SEQ_SONGPTR_S 1222u
#define MCI_SEQ_NONE_S 1223u

/* Driver return flags in the high word (mmddk.h), private to the boundary. */
#define MCI_RESOURCE_RETURNED 0x00010000u
#define MCI_COLONIZED3_RETURN 0x00020000u
#define MCI_COLONIZED4_RETURN 0x00040000u
#define MCI_INTEGER_RETURNED 0x00080000u
#define MCI_RECT_RETURNED 0x00800000u /* private: "x y w h" */
#define MCI_ASYNC 0x01000000u /* private: completes later via host */
#define MCI_MAKERESOURCE(value, id) ((uint32_t)(value) | ((uint32_t)(id) << 16))

/* waveaudio (mmsystem.h) */
#define MCI_WAVE_OPEN_BUFFER 0x10000u
#define MCI_WAVE_SET_FORMATTAG 0x10000u
#define MCI_WAVE_SET_CHANNELS 0x20000u
#define MCI_WAVE_SET_SAMPLESPERSEC 0x40000u
#define MCI_WAVE_SET_AVGBYTESPERSEC 0x80000u
#define MCI_WAVE_SET_BLOCKALIGN 0x100000u
#define MCI_WAVE_SET_BITSPERSAMPLE 0x200000u
#define MCI_WAVE_INPUT 0x400000u
#define MCI_WAVE_OUTPUT 0x800000u
#define MCI_WAVE_SET_ANYINPUT 0x4000000u
#define MCI_WAVE_SET_ANYOUTPUT 0x8000000u
#define MCI_WAVE_STATUS_FORMATTAG 0x4001u
#define MCI_WAVE_STATUS_CHANNELS 0x4002u
#define MCI_WAVE_STATUS_SAMPLESPERSEC 0x4003u
#define MCI_WAVE_STATUS_AVGBYTESPERSEC 0x4004u
#define MCI_WAVE_STATUS_BLOCKALIGN 0x4005u
#define MCI_WAVE_STATUS_BITSPERSAMPLE 0x4006u
#define MCI_WAVE_STATUS_LEVEL 0x4007u
#define MCI_WAVE_GETDEVCAPS_INPUTS 0x4001u
#define MCI_WAVE_GETDEVCAPS_OUTPUTS 0x4002u
/* sequencer */
#define MCI_SEQ_STATUS_TEMPO 0x4002u
#define MCI_SEQ_STATUS_PORT 0x4003u
#define MCI_SEQ_STATUS_SLAVE 0x4007u
#define MCI_SEQ_STATUS_MASTER 0x4008u
#define MCI_SEQ_STATUS_OFFSET 0x4009u
#define MCI_SEQ_STATUS_DIVTYPE 0x400Au
#define MCI_SEQ_STATUS_NAME 0x400Bu
#define MCI_SEQ_STATUS_COPYRIGHT 0x400Cu
#define MCI_SEQ_SET_TEMPO 0x10000u
#define MCI_SEQ_SET_PORT 0x20000u
#define MCI_SEQ_SET_SLAVE 0x40000u
#define MCI_SEQ_SET_MASTER 0x80000u
#define MCI_SEQ_SET_OFFSET 0x1000000u
#define MCI_SEQ_NONE 65533u
#define MCI_SEQ_MAPPER 65535u
#define MCI_SEQ_FILE 0x4002u
#define MCI_SEQ_MIDI 0x4003u
#define MCI_SEQ_SMPTE 0x4004u
/* cdaudio */
#define MCI_CDA_STATUS_TYPE_TRACK 0x4001u
/* digitalvideo / avivideo (digitalv.h, mmsystem.h MCI_ANIM_*, mciavi.h) */
#define MCI_DGV_OPEN_WS 0x10000u
#define MCI_DGV_OPEN_PARENT 0x20000u
#define MCI_DGV_OPEN_NOSTATIC 0x40000u
#define MCI_DGV_OPEN_16BIT 0x80000u
#define MCI_DGV_OPEN_32BIT 0x100000u
#define MCI_DGV_PLAY_REVERSE 0x20000u
#define MCI_DGV_PLAY_REPEAT 0x40000u
#define MCI_MCIAVI_PLAY_WINDOW 0x01000000u
#define MCI_MCIAVI_PLAY_FULLSCREEN 0x02000000u
#define MCI_MCIAVI_PLAY_FULLBY2 0x04000000u
#define MCI_DGV_WINDOW_HWND 0x10000u
#define MCI_DGV_WINDOW_STATE 0x40000u
#define MCI_DGV_WINDOW_TEXT 0x80000u
#define MCI_DGV_WINDOW_DEFAULT 0u
#define MCI_DGV_RECT 0x10000u
#define MCI_DGV_PUT_SOURCE 0x20000u
#define MCI_DGV_PUT_DESTINATION 0x40000u
#define MCI_DGV_PUT_FRAME 0x80000u
#define MCI_DGV_PUT_VIDEO 0x100000u
#define MCI_DGV_PUT_WINDOW 0x200000u
#define MCI_DGV_PUT_CLIENT 0x400000u
#define MCI_DGV_WHERE_SOURCE 0x20000u
#define MCI_DGV_WHERE_DESTINATION 0x40000u
#define MCI_DGV_WHERE_FRAME 0x80000u
#define MCI_DGV_WHERE_VIDEO 0x100000u
#define MCI_DGV_WHERE_WINDOW 0x200000u
#define MCI_DGV_WHERE_MAX 0x400000u
#define MCI_DGV_STEP_REVERSE 0x10000u
#define MCI_DGV_STEP_FRAMES 0x20000u
#define MCI_DGV_UPDATE_HDC 0x20000u
#define MCI_DGV_UPDATE_PAINT 0x40000u
#define MCI_DGV_SET_SEEK_EXACTLY 0x10000u
#define MCI_DGV_SET_SPEED 0x20000u
#define MCI_DGV_INFO_TEXT 0x10000u
#define MCI_DGV_INFO_ITEM 0x20000u
#define MCI_DGV_STATUS_HWND 0x4001u
#define MCI_DGV_STATUS_SPEED 0x4003u
#define MCI_DGV_STATUS_FRAME_RATE 0x400Eu
#define MCI_DGV_STATUS_AUDIO 0x4014u
#define MCI_DGV_STATUS_VIDEO 0x4026u
#define MCI_DGV_STATUS_WINDOW_VISIBLE 0x4021u
#define MCI_DGV_STATUS_WINDOW_MINIMIZED 0x4022u
#define MCI_DGV_STATUS_WINDOW_MAXIMIZED 0x4023u
#define MCI_AVI_STATUS_FRAMES_SKIPPED 0x8001u
#define MCI_AVI_STATUS_LAST_PLAY_SPEED 0x8002u
#define MCI_AVI_STATUS_AUDIO_BREAKS 0x8003u
#define MCI_DGV_GETDEVCAPS_CAN_LOCK 0x4000u
#define MCI_DGV_GETDEVCAPS_CAN_STRETCH 0x4001u
#define MCI_DGV_GETDEVCAPS_CAN_FREEZE 0x4002u
#define MCI_DGV_GETDEVCAPS_MAX_WINDOWS 0x4003u
#define MCI_DGV_GETDEVCAPS_CAN_REVERSE 0x4004u
#define MCI_DGV_GETDEVCAPS_HAS_STILL 0x4005u
#define MCI_DGV_GETDEVCAPS_MAXIMUM_RATE 0x400Au
#define MCI_DGV_GETDEVCAPS_MINIMUM_RATE 0x400Bu

/* Errors (mmsystem.h, MCIERR_BASE 256). */
#define MCIERR_INVALID_DEVICE_ID 257u
#define MCIERR_UNRECOGNIZED_KEYWORD 259u
#define MCIERR_UNRECOGNIZED_COMMAND 261u
#define MCIERR_HARDWARE 262u
#define MCIERR_INVALID_DEVICE_NAME 263u
#define MCIERR_OUT_OF_MEMORY 264u
#define MCIERR_DEVICE_OPEN 265u
#define MCIERR_CANNOT_LOAD_DRIVER 266u
#define MCIERR_MISSING_COMMAND_STRING 267u
#define MCIERR_PARAM_OVERFLOW 268u
#define MCIERR_MISSING_STRING_ARGUMENT 269u
#define MCIERR_BAD_INTEGER 270u
#define MCIERR_PARSER_INTERNAL 271u
#define MCIERR_DRIVER_INTERNAL 272u
#define MCIERR_MISSING_PARAMETER 273u
#define MCIERR_UNSUPPORTED_FUNCTION 274u
#define MCIERR_FILE_NOT_FOUND 275u
#define MCIERR_DEVICE_NOT_READY 276u
#define MCIERR_INTERNAL 277u
#define MCIERR_DRIVER 278u
#define MCIERR_CANNOT_USE_ALL 279u
#define MCIERR_MULTIPLE 280u
#define MCIERR_EXTENSION_NOT_FOUND 281u
#define MCIERR_OUTOFRANGE 282u
#define MCIERR_FLAGS_NOT_COMPATIBLE 284u
#define MCIERR_FILE_NOT_SAVED 286u
#define MCIERR_DEVICE_TYPE_REQUIRED 287u
#define MCIERR_DEVICE_LOCKED 288u
#define MCIERR_DUPLICATE_ALIAS 289u
#define MCIERR_BAD_CONSTANT 290u
#define MCIERR_MUST_USE_SHAREABLE 291u
#define MCIERR_MISSING_DEVICE_NAME 292u
#define MCIERR_BAD_TIME_FORMAT 293u
#define MCIERR_NO_CLOSING_QUOTE 294u
#define MCIERR_DUPLICATE_FLAGS 295u
#define MCIERR_INVALID_FILE 296u
#define MCIERR_NULL_PARAMETER_BLOCK 297u
#define MCIERR_UNNAMED_RESOURCE 298u
#define MCIERR_NEW_REQUIRES_ALIAS 299u
#define MCIERR_NOTIFY_ON_AUTO_OPEN 300u
#define MCIERR_NO_ELEMENT_ALLOWED 301u
#define MCIERR_NONAPPLICABLE_FUNCTION 302u
#define MCIERR_ILLEGAL_FOR_AUTO_OPEN 303u
#define MCIERR_FILENAME_REQUIRED 304u
#define MCIERR_EXTRA_CHARACTERS 305u
#define MCIERR_DEVICE_NOT_INSTALLED 306u
#define MCIERR_GET_CD 307u
#define MCIERR_SET_CD 308u
#define MCIERR_SET_DRIVE 309u
#define MCIERR_DEVICE_LENGTH 310u
#define MCIERR_DEVICE_ORD_LENGTH 311u
#define MCIERR_NO_INTEGER 312u
#define MCIERR_WAVE_OUTPUTSINUSE 320u
#define MCIERR_WAVE_SETOUTPUTINUSE 321u
#define MCIERR_WAVE_INPUTSINUSE 322u
#define MCIERR_WAVE_SETINPUTINUSE 323u
#define MCIERR_WAVE_OUTPUTUNSPECIFIED 324u
#define MCIERR_WAVE_INPUTUNSPECIFIED 325u
#define MCIERR_WAVE_OUTPUTSUNSUITABLE 326u
#define MCIERR_WAVE_SETOUTPUTUNSUITABLE 327u
#define MCIERR_WAVE_INPUTSUNSUITABLE 328u
#define MCIERR_WAVE_SETINPUTUNSUITABLE 329u
#define MCIERR_SEQ_DIV_INCOMPATIBLE 336u
#define MCIERR_SEQ_PORT_INUSE 337u
#define MCIERR_SEQ_PORT_NONEXISTENT 338u
#define MCIERR_SEQ_PORT_MAPNODEVICE 339u
#define MCIERR_SEQ_PORT_MISCERROR 340u
#define MCIERR_SEQ_TIMER 341u
#define MCIERR_SEQ_PORTUNSPECIFIED 342u
#define MCIERR_SEQ_NOMIDIPRESENT 343u
#define MCIERR_NO_WINDOW 346u
#define MCIERR_CREATEWINDOW 347u
#define MCIERR_FILE_READ 348u
#define MCIERR_FILE_WRITE 349u
#define MCIERR_NO_IDENTITY 350u

/* Parameter blocks, i386 layout (uintptr_t = DWORD_PTR/pointer/HWND). */
struct mci_generic {
  uintptr_t callback;
};
struct mci_open {
  uintptr_t callback;
  uint32_t device;
  uintptr_t type, element, alias;
  uint32_t style;  /* wave: dwBufferSeconds; dgv/anim: dwStyle */
  uintptr_t parent; /* dgv/anim: hWndParent */
};
struct mci_play {
  uintptr_t callback;
  uint32_t from, to;
};
struct mci_seek {
  uintptr_t callback;
  uint32_t to;
};
struct mci_status {
  uintptr_t callback, value;
  uint32_t item, track;
};
struct mci_info {
  uintptr_t callback, text;
  uint32_t size;
};
struct mci_caps {
  uintptr_t callback;
  uint32_t value, item;
};
struct mci_sysinfo {
  uintptr_t callback, text;
  uint32_t size, number, type;
};
struct mci_set {
  uintptr_t callback;
  uint32_t time_format, audio;
  uint32_t extra[12]; /* wave: inputs/outputs/format fields; seq: tempo..; dgv */
};
struct mci_break {
  uintptr_t callback;
  int32_t key;
  uintptr_t window;
};
struct mci_window {
  uintptr_t callback, window;
  uint32_t show;
  uintptr_t text;
};
struct mci_rect {
  int32_t left, top, right, bottom;
};
struct mci_put { /* MCI_DGV_PUT_PARMS / MCI_DGV_RECT_PARMS */
  uintptr_t callback;
  struct mci_rect rect;
};
struct mci_step {
  uintptr_t callback;
  uint32_t frames;
};
struct mci_update {
  uintptr_t callback;
  struct mci_rect rect;
  uintptr_t dc;
};
/* Largest block the string parser builds. */
union mci_parms {
  struct mci_generic generic;
  struct mci_open open;
  struct mci_play play;
  struct mci_seek seek;
  struct mci_status status;
  struct mci_info info;
  struct mci_caps caps;
  struct mci_sysinfo sysinfo;
  struct mci_set set;
  struct mci_break brk;
  struct mci_window window;
  struct mci_put put;
  struct mci_step step;
  struct mci_update update;
};

/* Driver boundary. All driver entry points run with the MCI lock held; a
 * driver must not call public MCI APIs. `command` returns the MCI error in
 * the low word and the return-kind flags in the high word. A command that
 * completes later returns MCI_ASYNC after host->begin() (or while its
 * continued operation is still busy); the end calls host->complete() with
 * MCI_NOTIFY_SUCCESSFUL/FAILURE, a cancellation host->abort(). */
struct mci_device;
struct mci_host {
  uint32_t version;
  void (*begin)(struct mci_device *);
  void (*complete)(struct mci_device *, uint32_t status);
  void (*abort)(struct mci_device *);
  void *(*alloc)(uint32_t);
  void (*free)(void *);
  void (*lock)(void);
  void (*unlock)(void);
  void (*wake)(void);
};
struct mci_driver {
  uint32_t version;
  const char *type;
  uint32_t devtype;
  uint32_t (*open)(struct mci_device *, uint32_t flags, struct mci_open *);
  uint32_t (*command)(struct mci_device *, uint32_t msg, uint32_t flags,
                      void *parms);
  void (*close)(struct mci_device *);
  void (*service)(struct mci_device *, uint32_t now_ms);
};
#define MCI_BOUNDARY_VERSION 1u
struct mci_device {
  uint32_t id, devtype, creator, shareable, auto_open;
  char alias[260], type[32], element[260];
  const struct mci_driver *driver;
  const struct mci_host *host;
  void *data;
  uintptr_t notify; /* pending notification window, 0 = none */
  uint32_t notify_pending, busy, busy_serial;
  uintptr_t yield;
  uint32_t yield_data;
  int32_t break_key;
  uintptr_t break_window;
};
const struct mci_driver *mci_wave_driver(void);
const struct mci_driver *mci_seq_driver(void);
const struct mci_driver *mci_cd_driver(void);
/* mciavi32.dll export: returns its driver table or 0 on version mismatch. */
typedef const struct mci_driver *(W32API *mci_driver_entry)(
    const struct mci_host *);
/* Shared helpers in mcitime.c (pure, host-tested). */
uint32_t mci_ms_to_format(uint32_t ms, uint32_t format, uint32_t *kind);
int mci_format_to_ms(uint32_t value, uint32_t format, uint32_t *ms);
int mci_strieq(const char *, const char *);
#endif
