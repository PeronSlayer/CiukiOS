/* fx-10: MSCDEX 2.x INT 2Fh/15xxh and the CD-ROM device driver audio
 * requests for DOS VMs, over a virtual audio disc. Pure model: guest memory,
 * clock and the audio backend are supplied through mscdex_ops.
 * Sources: Microsoft MS-DOS CD-ROM Extensions 2.1 (MSCDEX21.DOC: request
 * header, status word, IOCTL input/output control blocks, PLAY/STOP/RESUME
 * AUDIO state machine, Red Book/HSG addressing), the MSCDEX Function
 * Requests Specification (INT 2Fh AX=1500h..1510h), Ralf Brown's Interrupt
 * List INT 21h/4402h table 01450 (device status bit 10 audio playing, bit
 * 11 no disc) and id Software's Quake cd_audio.c (GPL release), which is
 * the request sequence the host test replays. docs/design/quake-shareware.md
 * "CD audio (fx-10)". SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_MSCDEX_H
#define CIUKI_DOSVM_MSCDEX_H
#include <ciuki/kernel.h>

#define MSCDEX_VERSION 0x0217u          /* 2.23: BH major, BL minor, binary */
#define MSCDEX_TRACKS 99u
#define MSCDEX_SECTOR_BYTES 2352u       /* CD-DA: 588 stereo S16LE frames */
#define MSCDEX_SECTORS_PER_SECOND 75u
#define MSCDEX_PREGAP 150u              /* Red Book 2 s offset of LBA 0 */
#define MSCDEX_DATA_SECTORS 300u        /* placeholder data track 1 (4 s minimum track) */
/* Request header status word (MS-PRM device driver format). */
#define MSCDEX_ST_ERROR 0x8000u
#define MSCDEX_ST_BUSY 0x0200u
#define MSCDEX_ST_DONE 0x0100u
/* Device driver error codes. */
#define MSCDEX_ERR_UNKNOWN_UNIT 0x01u
#define MSCDEX_ERR_NOT_READY 0x02u
#define MSCDEX_ERR_UNKNOWN_COMMAND 0x03u
#define MSCDEX_ERR_SECTOR_NOT_FOUND 0x08u
#define MSCDEX_ERR_GENERAL 0x0Cu
/* Device driver commands. */
#define MSCDEX_CMD_IOCTL_INPUT 3u
#define MSCDEX_CMD_INPUT_FLUSH 7u
#define MSCDEX_CMD_OUTPUT_FLUSH 11u
#define MSCDEX_CMD_IOCTL_OUTPUT 12u
#define MSCDEX_CMD_OPEN 13u
#define MSCDEX_CMD_CLOSE 14u
#define MSCDEX_CMD_READ_LONG 128u
#define MSCDEX_CMD_READ_PREFETCH 130u
#define MSCDEX_CMD_SEEK 131u
#define MSCDEX_CMD_PLAY 132u
#define MSCDEX_CMD_STOP 133u
#define MSCDEX_CMD_RESUME 136u
/* Device status (IOCTL input 6). */
#define MSCDEX_DEV_DOOR_OPEN 0x001u
#define MSCDEX_DEV_UNLOCKED 0x002u
#define MSCDEX_DEV_RAW 0x004u
#define MSCDEX_DEV_AUDIO 0x010u
#define MSCDEX_DEV_CHANNELS 0x100u
#define MSCDEX_DEV_REDBOOK 0x200u
#define MSCDEX_DEV_PLAYING 0x400u
#define MSCDEX_DEV_NO_DISC 0x800u
/* Track control/ADR byte: CONTROL in the high nibble, ADR 1. */
#define MSCDEX_CONTROL_AUDIO 0x01u
#define MSCDEX_CONTROL_DATA 0x41u

struct mscdex_track { uint32_t lba, sectors; uint8_t control; };
struct mscdex_disc {
    bool present;
    uint8_t first, last;
    uint32_t leadout;                              /* HSG sector after the last track */
    struct mscdex_track track[MSCDEX_TRACKS + 1];  /* by track number, 1..99 */
};
enum mscdex_event {
    MSCDEX_EV_INSTALL = 1,   /* a: drive, b: disc present */
    MSCDEX_EV_PLAY,          /* a: track, b: lba, c: sectors */
    MSCDEX_EV_STOP,          /* a: position, b: paused */
    MSCDEX_EV_RESUME,        /* a: lba, b: sectors */
    MSCDEX_EV_COMPLETE,      /* a: end */
    MSCDEX_EV_REFUSED,       /* a: command, b: control code, c: error */
};
struct mscdex_ops {
    void *ctx;
    int (*read)(void *ctx, uint32_t linear, void *buffer, unsigned bytes);
    int (*write)(void *ctx, uint32_t linear, const void *buffer, unsigned bytes);
    uint32_t (*now_ms)(void *ctx);
    /* Backend: play [lba, lba + sectors) from the start, or stop (play false). */
    void (*audio)(void *ctx, bool play, uint32_t lba, uint32_t sectors);
    void (*volume)(void *ctx, uint8_t left, uint8_t right);
    void (*event)(void *ctx, unsigned event, uint32_t a, uint32_t b, uint32_t c);
};
struct mscdex {
    struct mscdex_ops ops;
    struct mscdex_disc disc;
    uint8_t drive;                   /* 0 = A: */
    bool playing, paused, door_open, locked, changed, installed;
    uint32_t start, end;             /* last play, or the next resume: [start, end) */
    uint32_t play_from, play_ms;     /* sector and clock of the running segment */
    uint32_t head;                   /* stationary position */
    uint8_t channel[4][2];           /* input, volume per output channel */
    uint32_t requests, plays, stops, resumes, completions, refused;
};
struct mscdex_regs { uint16_t ax, bx, cx, dx, si, di, es, flags; };

void mscdex_init(struct mscdex *, const struct mscdex_ops *, uint8_t drive);
/* 0: answered (registers and CF in r); -ENOSYS: not an AH=15h call. */
int mscdex_interrupt(struct mscdex *, struct mscdex_regs *r);
/* Current head sector; retires a completed play first. */
uint32_t mscdex_position(struct mscdex *);
uint32_t mscdex_redbook(uint32_t lba);
/* UINT32_MAX for an address before 00:02:00 or with seconds/frames out of range. */
uint32_t mscdex_hsg(uint32_t redbook);
/* Track number holding lba, 0 outside the disc. */
unsigned mscdex_track_at(const struct mscdex_disc *, uint32_t lba);
/* Appends the next track; a data track is followed by the 2 s pregap. */
int mscdex_disc_add(struct mscdex_disc *, uint32_t sectors, bool data);

/* RIFF/WAVE: PCM, 2 channels, 44100 Hz, 16 bits (CD-DA sample format). */
struct mscdex_wav { uint32_t data_offset, data_bytes; };
int mscdex_wav_parse(const uint8_t *header, unsigned bytes, uint32_t file_bytes,
                     struct mscdex_wav *out, const char **reason);
/* Linear percent gain from an MSCDEX channel volume (0..255) and the
 * CT1745 CD and master levels (5-bit, 2 dB steps, 31 = 0 dB). */
unsigned mscdex_gain(uint8_t volume, uint8_t cd_level, uint8_t master_level);
#endif
