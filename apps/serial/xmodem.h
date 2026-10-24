/* SPDX-License-Identifier: MIT
 * XMODEM, XMODEM-CRC and XMODEM-1K file transfer (the protocols of the
 * Windows 98 HyperTerminal Transfer menu that need no negotiation), as a
 * pure state machine: bytes in, bytes out, elapsed milliseconds in. File
 * access goes through two callbacks, so host tests connect a sender and a
 * receiver in memory with lost and damaged bytes.
 * Reference: Chuck Forsberg, "XMODEM/YMODEM Protocol Reference" (1988): SOH/STX
 * blocks, block number and complement, 8-bit checksum or CRC-16 (CCITT,
 * polynomial 0x1021), ACK/NAK/CAN/EOT, 'C' to request CRC, 0x1A padding. */
#ifndef CIUKI_SERIAL_XMODEM_H
#define CIUKI_SERIAL_XMODEM_H
#include <stddef.h>
#include <stdint.h>
#define XMODEM_SOH 0x01
#define XMODEM_STX 0x02
#define XMODEM_EOT 0x04
#define XMODEM_ACK 0x06
#define XMODEM_NAK 0x15
#define XMODEM_CAN 0x18
#define XMODEM_PAD 0x1a
#define XMODEM_OUT 1100            /* bytes an input/tick call may produce at most: cap must be at least this */
#define XMODEM_RETRIES 10
#define XMODEM_START_MS 3000       /* the receiver repeats its request this often */
#define XMODEM_WAIT_MS 10000       /* the sender repeats its block after this much silence */
#define XMODEM_RECEIVER_WAIT_MS 20000   /* the receiver NAKs later than the sender repeats: otherwise both repeat together and the duplicate
                                         * acknowledgements (an ACK carries no block number) would make the sender skip a block */
#define XMODEM_SENDER_WAIT_MS 60000
enum xmodem_state { XM_START, XM_RUN, XM_END, XM_DONE, XM_FAILED, XM_CANCELLED };
struct xmodem_io {
    int (*read)(void *ctx, uint8_t *buffer, size_t size);          /* sender: bytes read, 0 at end, -errno */
    int (*write)(void *ctx, const uint8_t *buffer, size_t size);   /* receiver: 0 or -errno */
    void *ctx;
};
struct xmodem {
    struct xmodem_io io;
    int sending, crc, one_k, state;
    unsigned number, retries, quiet_ms, start_attempts;
    uint8_t frame[1029]; size_t frame_length;
    uint8_t last[1029]; size_t last_length;           /* sender: the frame to repeat */
    uint8_t held[1024]; size_t held_length; int holding;   /* receiver: the last block, kept until its tail padding can be judged */
    int eof; unsigned cans;
    uint8_t pending[2 * XMODEM_OUT]; size_t pending_length;   /* bytes waiting to be transmitted */
    uint64_t bytes; unsigned blocks;
    char message[96];
};
void xmodem_send_init(struct xmodem *, const struct xmodem_io *, int one_k);
void xmodem_receive_init(struct xmodem *, const struct xmodem_io *, int crc);
/* Bytes from the line: returns how many were consumed (all of them unless the output is backed up: drain it with
 * xmodem_take and call again with the rest). Answers and blocks wait in the engine until taken. */
size_t xmodem_input(struct xmodem *, const uint8_t *in, size_t length);
void xmodem_tick(struct xmodem *, unsigned elapsed_ms);              /* time passes: requests and resends */
void xmodem_cancel(struct xmodem *);                                  /* queues CAN CAN, then CANCELLED */
size_t xmodem_take(struct xmodem *, uint8_t *out, size_t cap);        /* the bytes to transmit, up to cap */
int xmodem_finished(const struct xmodem *);                          /* DONE, FAILED or CANCELLED */
uint16_t xmodem_crc16(const uint8_t *, size_t);
#endif
