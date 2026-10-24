/* SPDX-License-Identifier: MIT
 * Serial Terminal (G27, AX-8): the Windows 98 HyperTerminal equivalent for
 * the native COM ports of the kernel (/dev/com1 .. /dev/com8): a connection
 * with port, speed, data bits, parity and stop bits; an 80x24 TTY/ANSI screen
 * with scroll-back; local echo and line-end options; DTR and RTS; capturing
 * the session to a text file; sending a text file; XMODEM, XMODEM-CRC and
 * XMODEM-1K, YMODEM, CRC16 ZMODEM and basic Kermit transfers.
 * Modem dialling and saved connection files remain separate work. */
#ifndef CIUKI_SERIAL_H
#define CIUKI_SERIAL_H
#include "screen.h"
#include "xmodem.h"
#include "transfer.h"
#include "../accessories/acc.h"
#include <stdio.h>
#define SERIAL_TX 4096
#define SERIAL_PORTS 8
enum serial_parity { SERIAL_NONE, SERIAL_ODD, SERIAL_EVEN, SERIAL_MARK, SERIAL_SPACE };
enum { SERIAL_DTR = 1, SERIAL_RTS = 2 };
struct serial_line { unsigned baud; int data_bits, parity, stop_bits; };
struct serial_status { unsigned modem, rx_queued, tx_queued, errors; };   /* modem: MSR bits (CTS 0x10, DSR 0x20, RI 0x40, DCD 0x80) */
/* The port seam: the native COM calls in the guest, a loopback or a scripted UART in host tests. */
struct serial_ops {
    int (*open)(void *ctx, unsigned number);                       /* 1..8: 0 or -errno */
    void (*close)(void *ctx);
    int (*configure)(void *ctx, const struct serial_line *);
    int (*read)(void *ctx, uint8_t *buffer, size_t size);          /* bytes available now, 0 none, -errno */
    int (*write)(void *ctx, const uint8_t *buffer, size_t size);   /* bytes accepted now, 0 when full, -errno */
    int (*modem)(void *ctx, unsigned outputs);                     /* SERIAL_DTR | SERIAL_RTS */
    int (*status)(void *ctx, struct serial_status *);
    void *ctx;
};
struct serial_term {
    struct serial_ops ops;
    int connected; unsigned port; struct serial_line line; unsigned outputs;
    int local_echo, send_lf, receive_lf, ansi;
    struct screen screen; int scroll_back;            /* lines the view is scrolled up from the live screen */
    uint8_t tx[SERIAL_TX]; unsigned tx_head, tx_count;
    uint64_t received, transmitted; struct serial_status status;
    FILE *capture; char capture_path[512];
    char *text; size_t text_length, text_at;            /* a text file being sent */
    struct xmodem xm; int xm_active, xm_sending; FILE *xm_file; char xm_path[512]; uint64_t xm_size;
    struct transfer *transfer;
    char transfer_temp[512];
    unsigned xm_linger_ms;                               /* a finished receiver keeps answering repeated EOTs for a few seconds */
    char message[128];                                   /* the last result or error, for the status bar */
};
void serial_init(struct serial_term *, const struct serial_ops *);
void serial_close_files(struct serial_term *);          /* before the program ends */
int serial_connect(struct serial_term *);               /* 0 or -errno (EBUSY: a log or mouse owns the port) */
void serial_disconnect(struct serial_term *);
int serial_set_port(struct serial_term *, unsigned number);   /* 1..8; reconnects when connected */
int serial_set_line(struct serial_term *, const struct serial_line *);
int serial_set_modem(struct serial_term *, unsigned outputs);
/* "9600 8-N-1"-style text helpers. */
int serial_parse_format(const char *text, struct serial_line *);   /* "8N1", "7e2": data bits, parity letter, stop bits */
void serial_format_text(const struct serial_line *, char *out, size_t size);   /* "8N1" */
int serial_baud_valid(unsigned baud);
/* Keyboard to the line: returns the bytes queued; also echoes locally when enabled. */
size_t serial_key(struct serial_term *, unsigned key, unsigned ch, unsigned modifiers);
size_t serial_send(struct serial_term *, const uint8_t *bytes, size_t length);   /* raw bytes (paste) */
/* One step of the program loop: read the port, run the screen, capture, transfers, flush output. */
void serial_pump(struct serial_term *, unsigned elapsed_ms);
int serial_capture_start(struct serial_term *, const char *path);   /* appends; 0 or -errno */
void serial_capture_stop(struct serial_term *);
int serial_send_text_file(struct serial_term *, const char *path);
int serial_xmodem_send(struct serial_term *, const char *path, int one_k);
int serial_xmodem_receive(struct serial_term *, const char *path, int crc);
int serial_transfer_start(struct serial_term *, const char *path, int protocol, int sending);
void serial_xmodem_cancel(struct serial_term *);
void serial_status_line(const struct serial_term *, char *out, size_t size);
void serial_clear(struct serial_term *);

#define SERIAL_W 584
#define SERIAL_H 436
struct serial_app {
    struct du_ui *ui; struct serial_term term;
    struct du_menu menus[4]; struct du_menu_item call_items[5], transfer_items[14], view_items[7], help_items[1];
    void (*quit)(void *host); void *host;
    const char *folder;
    int xm_kind;                       /* 1 send, 2 send 1K, 3 receive: waiting for the file dialog */
    unsigned pending_port, pending_baud; char pending_format[8];
};
enum { SV_CANVAS = 1, SV_STATUS, SV_SCROLL, SV_TIMER = 1,
       SV_CONNECT = 10, SV_DISCONNECT, SV_PROPERTIES, SV_EXIT, SV_SEND_TEXT, SV_CAPTURE, SV_XM_SEND, SV_XM_SEND_1K, SV_XM_RECEIVE, SV_XM_CANCEL,
       SV_ECHO, SV_ANSI, SV_SEND_LF, SV_RECEIVE_LF, SV_CLEAR, SV_DTR, SV_RTS, SV_ABOUT,
       SV_YM_SEND = 60, SV_YM_RECEIVE, SV_ZM_SEND, SV_ZM_RECEIVE, SV_KERMIT_SEND, SV_KERMIT_RECEIVE,
       SV_D_TEXT = 40, SV_D_CAPTURE, SV_D_SEND, SV_D_RECEIVE, SV_D_PROPS, SV_D_INFO };
void serial_attach(struct serial_app *, struct du_ui *);
void serial_event(struct du_ui *, const struct du_event *, void *);
void serial_paint(struct du_ui *, void *);
void serial_tick(struct serial_app *, unsigned elapsed_ms);
const struct serial_ops *serial_native_ops(void);
#endif
