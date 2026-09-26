/* CiukiOS read-only native media service. All words little-endian, packed.
 * This ABI is an in-process FAR call; it never starts a command interpreter.
 */
#ifndef CIUKIOS_MEDIA_DRIVER_ABI_H
#define CIUKIOS_MEDIA_DRIVER_ABI_H
#define MD_ABI 1
#define MD_ROWS 6
#define MD_PATH 192
#define MD_ERROR 128
#define MD_PREVIEW 512
#define MD_MOUNT 1
#define MD_LIST 2
#define MD_PREVIEW_FILE 3
#define MD_IMPORT_FILE 4
#define MD_OPEN_SELECTED 5
#define MD_IMPORT_SELECTED 6
#define MD_UP 7
#define MD_REFRESH 8
#define MD_FLOPPY 1
#define MD_USB 2
#define MD_CD 3
#define MD_CD_IDE 4
/* status 0=success, 1=error; failed requests never return stale rows/preview. */
typedef struct __attribute__((packed)) {
    char name[40];
    u32 size;
    u8 directory;
    u8 reserved;
} MediaRow;
typedef struct __attribute__((packed)) {
    u16 version, operation, status, device, first, count, total, preview_bytes;
    u16 preview_more, bios_drive;
    char path[MD_PATH];
    char destination[64];
    char message[MD_ERROR];
    MediaRow rows[MD_ROWS];
    char preview[MD_PREVIEW];
} MediaRequest;
#endif
