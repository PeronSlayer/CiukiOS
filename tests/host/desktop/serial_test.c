/* Serial Terminal host tests (G27, AX-8): screen, XMODEM, the connection model and the window.
 * SPDX-License-Identifier: MIT */
#include "serial.h"
#include "acc_test.h"
#include <errno.h>

/* ---- a scripted UART: bytes pushed into rx are read by the program; tx collects its writes; peers cross-wire two ports ---- */
struct fake {
    uint8_t rx[1 << 17], tx[1 << 17]; size_t rx_len, rx_at, tx_len;
    int open_error, opened, opened_number, closes, configures, accept_limit, write_error, read_error;
    struct serial_line line; unsigned outputs, modem_inputs;
    struct fake *peer;
};
static int f_open(void *c, unsigned n) { struct fake *f = c; if (f->open_error) return f->open_error; f->opened = 1; f->opened_number = (int)n; return 0; }
static void f_close(void *c) { struct fake *f = c; f->opened = 0; f->closes++; }
static int f_configure(void *c, const struct serial_line *l) { struct fake *f = c; f->line = *l; f->configures++; return 0; }
static int f_read(void *c, uint8_t *b, size_t n)
{
    struct fake *f = c; if (f->read_error) return f->read_error;
    size_t have = f->rx_len - f->rx_at, k = have < n ? have : n; memcpy(b, f->rx + f->rx_at, k); f->rx_at += k; return (int)k;
}
static int f_write(void *c, const uint8_t *b, size_t n)
{
    struct fake *f = c; if (f->write_error) return f->write_error;
    size_t k = f->accept_limit && n > (size_t)f->accept_limit ? (size_t)f->accept_limit : n;
    assert(f->tx_len + k < sizeof(f->tx)); memcpy(f->tx + f->tx_len, b, k); f->tx_len += k;
    if (f->peer) { assert(f->peer->rx_len + k < sizeof(f->peer->rx)); memcpy(f->peer->rx + f->peer->rx_len, b, k); f->peer->rx_len += k; }
    return (int)k;
}
static int f_modem(void *c, unsigned o) { struct fake *f = c; f->outputs = o; return 0; }
static int f_status(void *c, struct serial_status *s) { struct fake *f = c; *s = (struct serial_status){.modem = f->modem_inputs}; return 0; }
static struct serial_ops ops_for(struct fake *f) { return (struct serial_ops){f_open, f_close, f_configure, f_read, f_write, f_modem, f_status, f}; }
static void push(struct fake *f, const char *s) { size_t n = strlen(s); memcpy(f->rx + f->rx_len, s, n); f->rx_len += n; }
static void write_file(const char *path, const void *data, size_t n) { FILE *f = fopen(path, "wb"); assert(f && fwrite(data, 1, n, f) == n && !fclose(f)); }
static size_t read_file(const char *path, uint8_t *out, size_t cap) { FILE *f = fopen(path, "rb"); if (!f) return (size_t)-1; size_t n = fread(out, 1, cap, f); fclose(f); return n; }

/* ---- screen ---- */
static struct screen S;
static void feed(const char *s) { screen_feed(&S, (const uint8_t *)s, strlen(s)); }
static void expect_row(int row, const char *want)
{
    char got[SCREEN_COLS + 1]; screen_row_text(&S, row, got);
    if (strcmp(got, want)) { fprintf(stderr, "screen row %d: [%s], want [%s]\n", row, got, want); abort(); }
}
static void screen_tests(void)
{
    screen_init(&S, 1);
    feed("Hello\r\nWorld"); expect_row(0, "Hello"); expect_row(1, "World"); assert(S.row == 1 && S.col == 5);
    feed("\b\bXY"); expect_row(1, "WorXY");
    feed("\r\n\tTab"); expect_row(2, "        Tab");
    /* Wrapping at column 80 and the deferred wrap of the last column. */
    screen_clear(&S); char eighty[81]; memset(eighty, 'a', 80); eighty[80] = 0; feed(eighty);
    assert(S.row == 0); feed("b"); assert(S.row == 1 && S.col == 1); expect_row(1, "b");
    /* Scrolling and the history. */
    screen_init(&S, 1);
    for (int i = 0; i < 30; i++) { char line[16]; snprintf(line, sizeof(line), "line%d\r\n", i); feed(line); }
    assert(S.history_count == 7 && S.row == SCREEN_ROWS - 1);
    char text[SCREEN_COLS + 1]; assert(screen_history_text(&S, 0, text) && !strcmp(text, "line6") && screen_history_text(&S, 6, text) && !strcmp(text, "line0") && !screen_history_text(&S, 7, text));
    expect_row(0, "line7"); expect_row(22, "line29");
    for (int i = 0; i < 400; i++) feed("x\r\n"); assert(S.history_count == SCREEN_SCROLLBACK);   /* bounded */
    /* ANSI sequences. */
    screen_init(&S, 1);
    feed("\x1b[2J\x1b[5;10Hhere"); assert(S.row == 4 && S.col == 13); char r5[SCREEN_COLS + 1]; screen_row_text(&S, 4, r5); assert(!strcmp(r5 + 9, "here"));
    feed("\x1b[2;3Hab\x1b[1;1H\x1b[0Kcd\x1b[3Bz"); assert(S.cells[0][0].ch == 'c' && S.cells[1][2].ch == 'a' && S.cells[3][2].ch == 'z');
    screen_init(&S, 1); feed("0123456789\x1b[5D\x1b[K"); expect_row(0, "01234");                          /* left 5 then erase to the end */
    screen_init(&S, 1); feed("0123456789\x1b[1;4H\x1b[1K"); expect_row(0, "    456789");                 /* erase to the cursor */
    screen_init(&S, 1); feed("abc\r\ndef\r\nghi\x1b[2;2H\x1b[J"); expect_row(0, "abc"); expect_row(1, "d"); expect_row(2, "");
    screen_init(&S, 1); feed("abc\r\ndef\r\nghi\x1b[2;2H\x1b[1J"); expect_row(0, ""); expect_row(1, "  f"); expect_row(2, "ghi");
    screen_init(&S, 1); feed("xy\x1b[s\x1b[10;10Hq\x1b[uz"); assert(S.cells[0][2].ch == 'z');            /* save and restore the cursor */
    screen_init(&S, 1); feed("\x1b[1mB\x1b[0mn\x1b[7mR\x1b[27mn\x1b[1;7mX");
    assert(S.cells[0][0].attr == SCREEN_BOLD && S.cells[0][1].attr == 0 && S.cells[0][2].attr == SCREEN_REVERSE && S.cells[0][3].attr == 0 && S.cells[0][4].attr == (SCREEN_BOLD | SCREEN_REVERSE));
    screen_init(&S, 1); feed("\x1b[1"); feed(";3Hx"); assert(S.cells[0][2].ch == 'x');                      /* a sequence split across reads */
    screen_init(&S, 1); feed("\x1b[99;99Hx"); assert(S.cells[SCREEN_ROWS - 1][SCREEN_COLS - 1].ch == 'x');   /* clamped */
    screen_init(&S, 1); feed("\x1b[Axy\x1b[Dz"); assert(S.row == 0);
    screen_init(&S, 1); feed("ab\x1b[\nc"); assert(S.cells[1][2].ch == 'c');                                 /* a broken sequence is dropped */
    screen_init(&S, 1); feed("a\x1b" "Zb"); expect_row(0, "ab");                                            /* unknown ESC is ignored */
    screen_init(&S, 1); feed("junk"); feed("\x1b" "c"); expect_row(0, "");
    screen_init(&S, 1); feed("a\ab\ac"); assert(S.bells == 2);
    screen_init(&S, 1); feed("abc\fd"); expect_row(0, "d");
    screen_init(&S, 1); feed("a\x80z\xe8"); { char t2[SCREEN_COLS + 1]; screen_row_text(&S, 0, t2); assert(!strcmp(t2, "a?z\xe8")); }   /* C1 shown as ?, Latin-1 kept */
    /* Plain TTY: escape sequences are not interpreted (the escape byte is dropped, the rest is text). */
    screen_init(&S, 0); feed("\x1b[1mA"); expect_row(0, "[1mA");
}

/* ---- XMODEM ---- */
struct xfile { uint8_t data[8192]; size_t length, at; uint8_t out[8192]; size_t out_length; int read_error, write_error; };
static int x_read(void *c, uint8_t *b, size_t n) { struct xfile *f = c; if (f->read_error) return -EIO; size_t k = f->length - f->at < n ? f->length - f->at : n; memcpy(b, f->data + f->at, k); f->at += k; return (int)k; }
static int x_write(void *c, const uint8_t *b, size_t n) { struct xfile *f = c; if (f->write_error) return -EIO; assert(f->out_length + n <= sizeof(f->out)); memcpy(f->out + f->out_length, b, n); f->out_length += n; return 0; }
/* Connects a sender and a receiver: `damage` flips a byte of every nth frame sent, `drop` loses every nth byte-run (answers included). */
struct wire { int corrupt_every, drop_every; unsigned frames, runs; };
static void run(struct xmodem *tx, struct xmodem *rx, struct wire *w, unsigned limit_ms)
{
    unsigned elapsed = 0;
    while (!(xmodem_finished(tx) && xmodem_finished(rx)) && elapsed <= limit_ms) {
        int moved = 0; uint8_t buf[XMODEM_OUT];
        for (int dir = 0; dir < 2; dir++) {
            struct xmodem *from = dir ? rx : tx, *to = dir ? tx : rx; size_t n;
            while ((n = xmodem_take(from, buf, sizeof(buf))) > 0) {
                moved = 1; w->runs++;
                if (w->drop_every && w->runs % (unsigned)w->drop_every == 0) continue;
                if (n > 100 && w->corrupt_every && ++w->frames % (unsigned)w->corrupt_every == 0) buf[n / 2] ^= 0x55;
                size_t used = 0; while (used < n) { size_t took = xmodem_input(to, buf + used, n - used); if (!took) break; used += took; }
            }
        }
        if (!moved) { elapsed += 1000; xmodem_tick(tx, 1000); xmodem_tick(rx, 1000); }
    }
}
static void transfer(size_t size, int one_k, int crc, int corrupt, int drop, int expect_done)
{
    static struct xfile src, dst; memset(&src, 0, sizeof(src)); memset(&dst, 0, sizeof(dst));
    for (size_t i = 0; i < size; i++) src.data[i] = (uint8_t)(i * 7 + (i >> 8) * 13 + 1);   /* never ends in the pad byte for these sizes */
    src.length = size;
    struct xmodem tx, rx; struct xmodem_io rd = {x_read, NULL, &src}, wr = {NULL, x_write, &dst};
    xmodem_send_init(&tx, &rd, one_k); xmodem_receive_init(&rx, &wr, crc);
    struct wire w = {corrupt, drop, 0, 0}; run(&tx, &rx, &w, 600000);
    if (expect_done) {
        if (!(tx.state == XM_DONE && rx.state == XM_DONE) || dst.out_length != size || memcmp(src.data, dst.out, size)) {
            fprintf(stderr, "xmodem size=%zu 1k=%d crc=%d corrupt=%d drop=%d: tx=%d [%s] rx=%d [%s] got %zu bytes\n", size, one_k, crc, corrupt, drop, tx.state, tx.message, rx.state, rx.message, dst.out_length);
            abort();
        }
    }
}
static void xmodem_tests(void)
{
    /* CRC reference: "123456789" is 0x31C3 (CRC-16/XMODEM). */
    assert(xmodem_crc16((const uint8_t *)"123456789", 9) == 0x31c3 && xmodem_crc16((const uint8_t *)"", 0) == 0);
    static const size_t sizes[] = {0, 1, 127, 128, 129, 255, 256, 1000, 1024, 1025, 5000};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        transfer(sizes[i], 0, 1, 0, 0, 1);       /* XMODEM-CRC */
        transfer(sizes[i], 0, 0, 0, 0, 1);       /* the original checksum XMODEM: the receiver NAKs to start */
        transfer(sizes[i], 1, 1, 0, 0, 1);       /* XMODEM-1K */
    }
    /* Damaged frames are repeated; lost bytes are recovered by the timeouts. */
    transfer(3000, 0, 1, 3, 0, 1); transfer(3000, 0, 0, 4, 0, 1); transfer(4000, 1, 1, 2, 0, 1);
    transfer(2000, 0, 1, 0, 5, 1); transfer(2000, 0, 0, 0, 7, 1); transfer(3000, 1, 1, 3, 4, 1);
    /* Too many errors: every frame damaged. */
    { static struct xfile src, dst; memset(&src, 0, sizeof(src)); memset(&dst, 0, sizeof(dst)); src.length = 500; memset(src.data, 'q', 500);
      struct xmodem tx, rx; struct xmodem_io rd = {x_read, NULL, &src}, wr = {NULL, x_write, &dst};
      xmodem_send_init(&tx, &rd, 0); xmodem_receive_init(&rx, &wr, 1); struct wire w = {1, 0, 0, 0}; run(&tx, &rx, &w, 600000);
      assert(tx.state == XM_FAILED && strstr(tx.message, "Too many errors")); assert(rx.state == XM_FAILED || rx.state == XM_RUN || rx.state == XM_START); }
    /* The receiver's first requests: C every three seconds, then the checksum fallback. */
    { static struct xfile dst; memset(&dst, 0, sizeof(dst)); struct xmodem rx; struct xmodem_io wr = {NULL, x_write, &dst}; xmodem_receive_init(&rx, &wr, 1);
      uint8_t out[XMODEM_OUT]; xmodem_tick(&rx, 2999); assert(!xmodem_take(&rx, out, sizeof(out)));
      xmodem_tick(&rx, 1); assert(xmodem_take(&rx, out, sizeof(out)) == 1 && out[0] == 'C');
      for (int i = 0; i < 2; i++) { xmodem_tick(&rx, 3000); assert(xmodem_take(&rx, out, sizeof(out)) == 1 && out[0] == 'C'); }
      xmodem_tick(&rx, 3000); assert(xmodem_take(&rx, out, sizeof(out)) == 1 && out[0] == XMODEM_NAK && !rx.crc);   /* an old sender */
      for (int i = 0; i < 40 && !xmodem_finished(&rx); i++) xmodem_tick(&rx, 3000);
      assert(rx.state == XM_FAILED && strstr(rx.message, "did not answer")); }
    /* The sender gives up after a minute of silence, and stops on a double CAN. */
    { static struct xfile src; memset(&src, 0, sizeof(src)); src.length = 10; struct xmodem tx; struct xmodem_io rd = {x_read, NULL, &src}; xmodem_send_init(&tx, &rd, 0);
      xmodem_tick(&tx, 59000); assert(tx.state == XM_START); xmodem_tick(&tx, 1000); assert(tx.state == XM_FAILED && strstr(tx.message, "did not answer"));
      xmodem_send_init(&tx, &rd, 0); uint8_t can[2] = {XMODEM_CAN, XMODEM_CAN}; assert(xmodem_input(&tx, can, 2) == 2 && tx.state == XM_CANCELLED);
      xmodem_send_init(&tx, &rd, 0); uint8_t one[3] = {XMODEM_CAN, 'C', XMODEM_CAN}; xmodem_input(&tx, one, 3); assert(tx.state != XM_CANCELLED); }
    { static struct xfile dst; memset(&dst, 0, sizeof(dst)); struct xmodem rx; struct xmodem_io wr = {NULL, x_write, &dst}; xmodem_receive_init(&rx, &wr, 1);
      uint8_t can[2] = {XMODEM_CAN, XMODEM_CAN}; xmodem_input(&rx, can, 2); assert(rx.state == XM_CANCELLED);
      xmodem_receive_init(&rx, &wr, 1); xmodem_cancel(&rx); uint8_t out[8]; assert(xmodem_take(&rx, out, 8) == 2 && out[0] == XMODEM_CAN && rx.state == XM_CANCELLED && xmodem_finished(&rx)); }
    /* Out-of-sequence and duplicate blocks. */
    { static struct xfile dst; memset(&dst, 0, sizeof(dst)); struct xmodem rx; struct xmodem_io wr = {NULL, x_write, &dst}; xmodem_receive_init(&rx, &wr, 0);
      uint8_t frame[3 + 128 + 1] = {XMODEM_SOH}; frame[1] = 1; frame[2] = 0xfe; memset(frame + 3, 'a', 128); frame[131] = (uint8_t)(128 * 'a');
      uint8_t out[16];
      assert(xmodem_input(&rx, frame, sizeof(frame)) == sizeof(frame) && xmodem_take(&rx, out, 16) == 1 && out[0] == XMODEM_ACK);
      assert(xmodem_input(&rx, frame, sizeof(frame)) == sizeof(frame) && xmodem_take(&rx, out, 16) == 1 && out[0] == XMODEM_ACK && rx.blocks == 1);   /* a repeat: acknowledged, not stored twice */
      frame[1] = 5; frame[2] = 0xfa; xmodem_input(&rx, frame, sizeof(frame)); assert(xmodem_take(&rx, out, 16) == 2 && rx.state == XM_FAILED);
      uint8_t eot = XMODEM_EOT; xmodem_input(&rx, &eot, 1); }
    /* A file that cannot be written or read ends the transfer cleanly. */
    { static struct xfile src, dst; memset(&src, 0, sizeof(src)); memset(&dst, 0, sizeof(dst)); src.length = 300; memset(src.data, 'k', 300); dst.write_error = 1;
      struct xmodem tx, rx; struct xmodem_io rd = {x_read, NULL, &src}, wr = {NULL, x_write, &dst}; xmodem_send_init(&tx, &rd, 0); xmodem_receive_init(&rx, &wr, 1);
      struct wire w = {0, 0, 0, 0}; run(&tx, &rx, &w, 100000); assert(rx.state == XM_FAILED && strstr(rx.message, "could not be written"));
      memset(&dst, 0, sizeof(dst)); src.at = 0; src.read_error = 1; xmodem_send_init(&tx, &rd, 0); xmodem_receive_init(&rx, &wr, 1); run(&tx, &rx, &w, 100000);
      assert(tx.state == XM_FAILED && strstr(tx.message, "could not be read")); }
    /* The bytes of a pending answer are taken in pieces. */
    { static struct xfile src; memset(&src, 0, sizeof(src)); src.length = 300; memset(src.data, 'k', 300); struct xmodem tx; struct xmodem_io rd = {x_read, NULL, &src}; xmodem_send_init(&tx, &rd, 0);
      uint8_t c = 'C'; xmodem_input(&tx, &c, 1); uint8_t part[100]; size_t total = 0, n; while ((n = xmodem_take(&tx, part, sizeof(part))) > 0) total += n; assert(total == 133); }
}

/* ---- the connection model ---- */
static struct serial_term *new_term(struct fake *f) { static struct serial_term t; struct serial_ops o = ops_for(f); serial_init(&t, &o); return &t; }
static void format_tests(void)
{
    struct serial_line l; assert(!serial_parse_format("8N1", &l) && l.data_bits == 8 && l.parity == SERIAL_NONE && l.stop_bits == 1);
    assert(!serial_parse_format("7e2", &l) && l.data_bits == 7 && l.parity == SERIAL_EVEN && l.stop_bits == 2);
    assert(!serial_parse_format("5M1", &l) && l.parity == SERIAL_MARK && !serial_parse_format("6s1", &l) && l.parity == SERIAL_SPACE && !serial_parse_format("8o1", &l) && l.parity == SERIAL_ODD);
    assert(serial_parse_format("9N1", &l) && serial_parse_format("4N1", &l) && serial_parse_format("8X1", &l) && serial_parse_format("8N3", &l) && serial_parse_format("8N0", &l) && serial_parse_format("8N", &l) && serial_parse_format("", &l) && serial_parse_format(NULL, &l));
    char text[8]; serial_format_text(&(struct serial_line){9600, 7, SERIAL_ODD, 2}, text, sizeof(text)); assert(!strcmp(text, "7O2"));
    assert(serial_baud_valid(9600) && serial_baud_valid(115200) && serial_baud_valid(110) && !serial_baud_valid(0) && !serial_baud_valid(12345));
}
static void model_tests(const char *scratch)
{
    static struct fake f; memset(&f, 0, sizeof(f));
    struct serial_term *t = new_term(&f);
    assert(t->port == 1 && t->line.baud == 9600 && !t->connected && t->ansi);
    f.open_error = -EBUSY; assert(serial_connect(t) == -EBUSY && !t->connected && strstr(t->message, "in use")); f.open_error = -ENOENT; assert(serial_connect(t) == -ENOENT && strstr(t->message, "no such port"));
    f.open_error = 0; assert(!serial_connect(t) && t->connected && f.opened && f.opened_number == 1 && f.configures == 1 && f.line.baud == 9600 && f.outputs == (SERIAL_DTR | SERIAL_RTS));
    assert(!serial_connect(t));                                                                  /* connecting twice does nothing */
    char status[256]; serial_status_line(t, status, sizeof(status)); assert(strstr(status, "Connected   COM1 9600 8N1"));
    /* Port, line and modem changes while connected. */
    assert(!serial_set_port(t, 3) && f.opened_number == 3 && f.closes == 1 && t->connected);
    assert(serial_set_port(t, 0) == -EINVAL && serial_set_port(t, 9) == -EINVAL && t->port == 3);
    assert(!serial_set_line(t, &(struct serial_line){19200, 7, SERIAL_EVEN, 2}) && f.line.baud == 19200 && f.line.data_bits == 7 && f.line.parity == SERIAL_EVEN && f.line.stop_bits == 2);
    assert(serial_set_line(t, &(struct serial_line){1234, 8, 0, 1}) == -EINVAL && serial_set_line(t, &(struct serial_line){9600, 9, 0, 1}) == -EINVAL && serial_set_line(t, &(struct serial_line){9600, 8, 7, 1}) == -EINVAL && t->line.baud == 19200);
    assert(!serial_set_modem(t, SERIAL_DTR) && f.outputs == SERIAL_DTR && t->outputs == SERIAL_DTR && !serial_set_modem(t, 0xff) && f.outputs == (SERIAL_DTR | SERIAL_RTS));
    serial_disconnect(t); assert(!t->connected && !f.opened && f.closes == 2); serial_disconnect(t); assert(f.closes == 2);
    assert(!serial_set_port(t, 2) && !t->connected && t->port == 2);                              /* idle: only the number changes */
    assert(!serial_connect(t) && f.opened_number == 2);

    /* Keys. */
    f.tx_len = 0;
    assert(serial_key(t, 0x1e, 'a', 0) == 1 && serial_key(t, DU_KEY_ENTER, 0, 0) == 1 && serial_key(t, DU_KEY_BACKSPACE, 0, 0) == 1 && serial_key(t, DU_KEY_TAB, 0, 0) == 1);
    assert(serial_key(t, DU_KEY_ESC, 0, 0) == 1 && serial_key(t, DU_KEY_DELETE, 0, 0) == 1 && serial_key(t, DU_KEY_UP, 0, 0) == 3 && serial_key(t, DU_KEY_LEFT, 0, 0) == 3 && serial_key(t, DU_KEY_HOME, 0, 0) == 3);
    assert(serial_key(t, 0x2e, 0, DU_CTRL) == 1 && serial_key(t, 0x10, 0, DU_CTRL) == 1 && serial_key(t, 0x2c, 0, DU_CTRL) == 1 && serial_key(t, 0x1e, 0, DU_CTRL) == 1);
    assert(serial_key(t, 0x02, 0, DU_CTRL) == 0 && serial_key(t, DU_KEY_F1, 0, 0) == 0 && serial_key(t, 0, 0xe8, 0) == 1 && serial_key(t, 0, 0x100, 0) == 0);
    serial_pump(t, 20);
    static const uint8_t wanted[] = {'a', '\r', 8, 9, 0x1b, 0x7f, 0x1b, '[', 'A', 0x1b, '[', 'D', 0x1b, '[', 'H', 3, 17, 26, 1, 0xe8};
    assert(f.tx_len == sizeof(wanted) && !memcmp(f.tx, wanted, sizeof(wanted)) && t->transmitted == sizeof(wanted));
    t->send_lf = 1; f.tx_len = 0; serial_key(t, DU_KEY_ENTER, 0, 0); serial_pump(t, 20); assert(f.tx_len == 2 && f.tx[0] == '\r' && f.tx[1] == '\n'); t->send_lf = 0;
    /* Local echo goes to the screen; without it nothing does. */
    serial_clear(t); serial_key(t, 0x2d, 'x', 0); { char row[81]; screen_row_text(&t->screen, 0, row); assert(!row[0]); }
    t->local_echo = 1; serial_key(t, 0x2d, 'x', 0); { char row[81]; screen_row_text(&t->screen, 0, row); assert(!strcmp(row, "x")); } t->local_echo = 0;
    /* Receiving: the screen, the received count, the line-end option, scrolling the view back. */
    serial_clear(t); push(&f, "Hello\r\nline2"); serial_pump(t, 20); { char row[81]; screen_row_text(&t->screen, 1, row); assert(!strcmp(row, "line2") && t->received >= 12); }
    t->receive_lf = 1; serial_clear(t); push(&f, "a\rb\r\nc\r"); serial_pump(t, 20);
    { char r0[81], r1[81], r2[81]; screen_row_text(&t->screen, 0, r0); screen_row_text(&t->screen, 1, r1); screen_row_text(&t->screen, 2, r2); assert(!strcmp(r0, "a") && !strcmp(r1, "b") && !strcmp(r2, "c")); }
    t->receive_lf = 0;
    /* A full transmit queue accepts what fits, and a port that takes a few bytes at a time loses nothing. */
    f.tx_len = 0; f.accept_limit = 3; { uint8_t big[200]; for (int i = 0; i < 200; i++) big[i] = (uint8_t)('A' + i % 26); assert(serial_send(t, big, 200) == 200); for (int i = 0; i < 400 && t->tx_count; i++) serial_pump(t, 20); assert(f.tx_len == 200 && !memcmp(f.tx, big, 200)); }
    f.accept_limit = 0; { static uint8_t huge[SERIAL_TX + 500]; memset(huge, 'h', sizeof(huge)); f.tx_len = 0; assert(serial_send(t, huge, sizeof(huge)) == SERIAL_TX); serial_pump(t, 20); assert(f.tx_len == SERIAL_TX); }
    f.write_error = -EIO; serial_send(t, (const uint8_t *)"zz", 2); serial_pump(t, 20); assert(!t->tx_count && strstr(t->message, "write error")); f.write_error = 0; t->message[0] = 0;
    f.read_error = -EIO; serial_pump(t, 20); assert(strstr(t->message, "read error")); f.read_error = 0; t->message[0] = 0;
    f.modem_inputs = 0x30; serial_pump(t, 20); serial_status_line(t, status, sizeof(status)); assert(strstr(status, "CTS DSR"));
    /* Capture. */
    char path[1100]; snprintf(path, sizeof(path), "%s/capture.txt", scratch); remove(path);
    assert(!serial_capture_start(t, path) && t->capture); push(&f, "alpha\r\nbeta\x01\x1b[2J\t\r\n"); serial_pump(t, 20);
    serial_status_line(t, status, sizeof(status)); assert(strstr(status, "CAPTURE")); serial_capture_stop(t); assert(!t->capture);
    { uint8_t got[200]; size_t n = read_file(path, got, sizeof(got)); got[n] = 0; assert(!strcmp((char *)got, "alpha\r\nbeta[2J\t\r\n")); }   /* control bytes are not written */
    assert(!serial_capture_start(t, path)); push(&f, "more"); serial_pump(t, 20); serial_capture_stop(t);
    { uint8_t got[200]; size_t n = read_file(path, got, sizeof(got)); got[n] = 0; assert(strstr((char *)got, "[2J\t\r\nmore")); }   /* appended */
    assert(serial_capture_start(t, "/no-such-folder/x.txt") < 0 && !t->capture);
    char longpath[600]; memset(longpath, 'p', sizeof(longpath)); longpath[599] = 0; assert(serial_capture_start(t, longpath) == -ENAMETOOLONG);
    remove(path);
    /* Sending a text file: line ends become returns, large files go out over many turns. */
    char text_path[1100]; snprintf(text_path, sizeof(text_path), "%s/send.txt", scratch);
    write_file(text_path, "one\ntwo\r\nthree", 14); f.tx_len = 0;
    assert(!serial_send_text_file(t, text_path) && t->text && serial_send_text_file(t, text_path) == -EBUSY); serial_pump(t, 20);
    assert(!t->text && f.tx_len == 13 && !memcmp(f.tx, "one\rtwo\rthree", 13));
    t->send_lf = 1; f.tx_len = 0; assert(!serial_send_text_file(t, text_path)); serial_pump(t, 20); assert(f.tx_len == 15 && !memcmp(f.tx, "one\r\ntwo\r\nthree", 15)); t->send_lf = 0;
    { static char big[20000]; for (int i = 0; i < 20000; i++) big[i] = (char)('a' + i % 26); write_file(text_path, big, sizeof(big)); f.tx_len = 0;
      assert(!serial_send_text_file(t, text_path)); int turns = 0; while (t->text && turns++ < 100) serial_pump(t, 20); assert(!t->text && turns > 2 && f.tx_len == 20000 && !memcmp(f.tx, big, 20000)); }
    t->local_echo = 1; serial_clear(t); write_file(text_path, "xy", 2); serial_send_text_file(t, text_path); serial_pump(t, 20); { char row[81]; screen_row_text(&t->screen, 0, row); assert(!strcmp(row, "xy")); } t->local_echo = 0;
    assert(serial_send_text_file(t, "/no/such/file") < 0);
    serial_disconnect(t); assert(serial_send_text_file(t, text_path) == -ENOTCONN && serial_xmodem_send(t, text_path, 0) == -ENOTCONN && serial_xmodem_receive(t, text_path, 1) == -ENOTCONN);
    assert(serial_key(t, 0x1e, 'a', 0) == 0);                                                      /* not connected: nothing is sent */
    serial_pump(t, 20);
    serial_close_files(t);
}
/* Two terminals joined by a null-modem cable transfer a file by XMODEM through the whole stack. */
static void cable_tests(const char *scratch)
{
    static struct fake fa, fb; memset(&fa, 0, sizeof(fa)); memset(&fb, 0, sizeof(fb)); fa.peer = &fb; fb.peer = &fa;
    static struct serial_term a, b; struct serial_ops oa = ops_for(&fa), ob = ops_for(&fb); serial_init(&a, &oa); serial_init(&b, &ob);
    assert(!serial_connect(&a) && !serial_connect(&b));
    static uint8_t data[7000]; for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 31 + 5);
    char src[1100], dst[1100]; snprintf(src, sizeof(src), "%s/xsrc.bin", scratch); snprintf(dst, sizeof(dst), "%s/xdst.bin", scratch);
    for (int mode = 0; mode < 2; mode++) {
        write_file(src, data, mode ? 7000 : 3333); remove(dst);
        assert(!serial_xmodem_receive(&b, dst, 1) && !serial_xmodem_send(&a, src, mode) && a.xm_active && b.xm_active);
        assert(serial_send_text_file(&a, src) == -EBUSY && serial_xmodem_send(&a, src, 0) == -EBUSY && serial_key(&a, 0x1e, 'a', 0) == 0);   /* busy: no typing mid-transfer */
        for (int turn = 0; turn < 20000 && (a.xm_active || b.xm_active); turn++) { serial_pump(&a, 20); serial_pump(&b, 20); }
        assert(!a.xm_active && !b.xm_active && strstr(a.message, "Transfer complete") && strstr(b.message, "Transfer complete"));
        uint8_t got[8000]; size_t n = read_file(dst, got, sizeof(got)); assert(n == (mode ? 7000u : 3333u) && !memcmp(got, data, n));
    }
    /* Cancelling mid-way: the sender stops, the receiver sees CAN CAN, no half file stays behind. */
    write_file(src, data, 7000); remove(dst);
    assert(!serial_xmodem_receive(&b, dst, 1) && !serial_xmodem_send(&a, src, 0));
    for (int turn = 0; turn < 30; turn++) { serial_pump(&a, 20); serial_pump(&b, 20); }
    serial_xmodem_cancel(&a); for (int turn = 0; turn < 20 && (a.xm_active || b.xm_active); turn++) { serial_pump(&a, 20); serial_pump(&b, 20); }
    assert(!a.xm_active && !b.xm_active && strstr(a.message, "cancelled") && strstr(b.message, "cancelled"));
    serial_xmodem_cancel(&a);                                                                      /* nothing running: no effect */
    serial_disconnect(&a); serial_disconnect(&b);
    /* Disconnecting a running transfer cancels it. */
    assert(!serial_connect(&a) && !serial_xmodem_send(&a, src, 0) && a.xm_active); serial_disconnect(&a); assert(!a.xm_active && !a.connected);
    serial_close_files(&a); serial_close_files(&b);
    assert(serial_xmodem_send(&a, "/no/such/file", 0) == -ENOTCONN);
    remove(src); remove(dst);
}

/* ---- the window ---- */
static uint32_t pixels[SERIAL_W * SERIAL_H];
static struct du_ui ui; static struct serial_app app;
static unsigned lit(struct du_rect r)    /* pixels of the grid area that differ from the black paper */
{
    unsigned n = 0; for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++) n += (pixels[y * SERIAL_W + x] & 0xffffff) != 0; return n;
}
static void window_tests(const char *scratch, const char *folder)
{
    static struct fake f; memset(&f, 0, sizeof(f));
    du_init(&ui, pixels, SERIAL_W, SERIAL_H);
    memset(&app, 0, sizeof(app)); struct serial_ops o = ops_for(&f); serial_init(&app.term, &o); app.folder = scratch;
    serial_attach(&app, &ui); du_render(&ui);
    at_save(folder, "serial-disconnected", &ui);
    struct serial_term *t = &app.term;
    assert((app.call_items[1].flags & DU_ITEM_DISABLED) && !(app.call_items[0].flags & DU_ITEM_DISABLED) && (app.transfer_items[0].flags & DU_ITEM_DISABLED));
    assert(strstr(du_get(&ui, SV_STATUS)->text, "Disconnected") && strstr(du_get(&ui, SV_STATUS)->text, "COM1 9600 8N1"));
    struct du_rect grid = {4, 24, 80 * 7, 24 * 16};
    assert(lit(grid) == 0);
    /* Keys do nothing while disconnected; Connect opens the port. */
    at_tap(&ui, 0x1e); assert(!t->tx_count && !f.tx_len);
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_CONNECT}, &app);
    assert(t->connected && f.opened && (app.call_items[0].flags & DU_ITEM_DISABLED) && !(app.call_items[1].flags & DU_ITEM_DISABLED) && !(app.transfer_items[0].flags & DU_ITEM_DISABLED));
    assert(strstr(du_get(&ui, SV_STATUS)->text, "Connected"));
    /* Typed keys go out through the pump; received text appears on the grid. */
    at_tap(&ui, 0x23); at_tap(&ui, 0x17); at_tap(&ui, DU_KEY_ENTER); serial_tick(&app, 20);
    assert(f.tx_len == 3 && f.tx[0] == 'h' && f.tx[1] == 'i' && f.tx[2] == '\r');
    push(&f, "login: \x1b[1mroot\x1b[0m\r\nPassword:"); serial_tick(&app, 20); du_render(&ui);
    assert(lit(grid) > 100); at_save(folder, "serial-connected", &ui);
    assert(strstr(du_get(&ui, SV_STATUS)->text, "RX ") && strstr(du_get(&ui, SV_STATUS)->text, "TX 3"));
    { char row[81]; screen_row_text(&t->screen, 0, row); assert(!strcmp(row, "login: root")); }
    /* The timer event runs a tick. */
    push(&f, "!"); serial_event(&ui, &(struct du_event){.type = DU_EV_TICK, .id = SV_TIMER}, &app); { char row[81]; screen_row_text(&t->screen, 1, row); assert(!strcmp(row, "Password:!")); }
    /* View options. */
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_ECHO}, &app); assert(t->local_echo && (app.view_items[0].flags & DU_ITEM_CHECKED));
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_SEND_LF}, &app); serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_RECEIVE_LF}, &app);
    assert(t->send_lf && t->receive_lf);
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_ANSI}, &app); assert(!t->ansi && !t->screen.ansi); serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_ANSI}, &app); assert(t->ansi);
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_ECHO}, &app); serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_SEND_LF}, &app); serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_RECEIVE_LF}, &app);
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_DTR}, &app); assert(f.outputs == SERIAL_RTS && !(app.view_items[5].flags & DU_ITEM_CHECKED) && (app.view_items[6].flags & DU_ITEM_CHECKED));
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_RTS}, &app); assert(f.outputs == 0);
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_DTR}, &app); serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_RTS}, &app); assert(f.outputs == 3);
    /* Scroll-back with Page Up. */
    for (int i = 0; i < 60; i++) { char line[24]; snprintf(line, sizeof(line), "scroll %d\r\n", i); push(&f, line); }
    serial_tick(&app, 20); serial_tick(&app, 20); serial_tick(&app, 20); serial_tick(&app, 20); assert(t->screen.history_count > 30);
    at_tap(&ui, DU_KEY_PGUP); assert(t->scroll_back == 12); at_tap(&ui, DU_KEY_PGUP); assert(t->scroll_back == 24);
    for (int i = 0; i < 10; i++) at_tap(&ui, DU_KEY_PGUP); assert(t->scroll_back == (int)t->screen.history_count);
    du_render(&ui); at_save(folder, "serial-scrollback", &ui);
    at_tap(&ui, DU_KEY_PGDN); assert(t->scroll_back < (int)t->screen.history_count); for (int i = 0; i < 10; i++) at_tap(&ui, DU_KEY_PGDN); assert(t->scroll_back == 0);
    at_tap(&ui, DU_KEY_PGUP); at_tap(&ui, 0x1e); assert(t->scroll_back == 0);                         /* typing returns to the live screen */
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_CLEAR}, &app); { char row[81]; screen_row_text(&t->screen, 0, row); assert(!row[0]); }
    /* Properties: port, speed and format; invalid input keeps the old values and says so. */
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_PROPERTIES}, &app);
    assert(ui.dialog.kind && !strcmp(ui.dialog.items[3].text, "1") && !strcmp(ui.dialog.items[5].text, "9600") && !strcmp(ui.dialog.items[7].text, "8N1"));
    du_render(&ui); at_save(folder, "serial-properties", &ui);
    ui.dialog.kind = 0;
    du_set_text(&ui.dialog.items[3], "4"); du_set_text(&ui.dialog.items[5], "115200"); du_set_text(&ui.dialog.items[7], "7E2");
    serial_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = SV_D_PROPS, .index = 0}, &app);
    assert(t->port == 4 && f.opened_number == 4 && t->line.baud == 115200 && t->line.data_bits == 7 && t->line.parity == SERIAL_EVEN && t->line.stop_bits == 2 && f.line.baud == 115200 && t->connected);
    du_set_text(&ui.dialog.items[5], "1234"); ui.dialog.kind = 0;
    serial_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = SV_D_PROPS, .index = 0}, &app); assert(t->line.baud == 115200 && ui.dialog.kind); ui.dialog.kind = 0;
    du_set_text(&ui.dialog.items[5], "9600"); du_set_text(&ui.dialog.items[3], "12"); serial_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = SV_D_PROPS, .index = 0}, &app); assert(t->port == 4); ui.dialog.kind = 0;
    du_set_text(&ui.dialog.items[3], "4"); du_set_text(&ui.dialog.items[7], "9X9"); serial_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = SV_D_PROPS, .index = 0}, &app); assert(t->line.data_bits == 7); ui.dialog.kind = 0;
    serial_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = SV_D_PROPS, .index = 1}, &app);   /* Cancel */
    /* Transfers through the file dialogs' results. */
    char path[1100]; snprintf(path, sizeof(path), "%s/ui-send.txt", scratch); write_file(path, "from the window\n", 16);
    f.tx_len = 0; serial_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = SV_D_TEXT, .index = 0, .text = path}, &app); serial_tick(&app, 20);
    assert(f.tx_len == 16);
    snprintf(path, sizeof(path), "%s/ui-capture.txt", scratch); remove(path);
    serial_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = SV_D_CAPTURE, .index = 0, .text = path}, &app); assert(t->capture && (app.transfer_items[3].flags & DU_ITEM_CHECKED));
    push(&f, "captured\r\n"); serial_tick(&app, 20);
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_CAPTURE}, &app); assert(!t->capture && !(app.transfer_items[3].flags & DU_ITEM_CHECKED));
    { uint8_t got[64]; size_t n = read_file(path, got, sizeof(got)); got[n] = 0; assert(!strcmp((char *)got, "captured\r\n")); }
    snprintf(path, sizeof(path), "%s/ui-x.bin", scratch); write_file(path, "0123456789", 10);
    app.xm_kind = 2; serial_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = SV_D_SEND, .index = 0, .text = path}, &app);
    assert(t->xm_active && t->xm_sending && t->xm.one_k && !(app.transfer_items[7].flags & DU_ITEM_DISABLED) && (app.transfer_items[1].flags & DU_ITEM_DISABLED));
    du_render(&ui); at_save(folder, "serial-transfer", &ui);
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_XM_CANCEL}, &app); serial_tick(&app, 20); assert(!t->xm_active && (app.transfer_items[7].flags & DU_ITEM_DISABLED));
    serial_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = SV_D_RECEIVE, .index = 0, .text = path}, &app); assert(t->xm_active && !t->xm_sending);
    serial_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = SV_D_TEXT, .index = 0, .text = path}, &app); assert(strstr(t->message, "in use") || t->message[0]);   /* busy */
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_XM_CANCEL}, &app); serial_tick(&app, 20);
    serial_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = SV_D_TEXT, .index = 1, .text = path}, &app);   /* a cancelled dialog does nothing */
    remove(path);
    /* Disconnect and the port error path. */
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_DISCONNECT}, &app); assert(!t->connected && !f.opened && (app.view_items[5].flags & DU_ITEM_DISABLED));
    f.open_error = -EBUSY; serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_CONNECT}, &app); assert(!t->connected && ui.dialog.kind && strstr(du_get(&ui, SV_STATUS)->text, "in use")); ui.dialog.kind = 0;
    f.open_error = 0;
    serial_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SV_ABOUT}, &app); assert(ui.dialog.kind); ui.dialog.kind = 0;
    serial_close_files(t);
}
int main(int argc, char **argv)
{
    assert(argc >= 2);
    screen_tests(); format_tests(); xmodem_tests(); model_tests(argv[1]); cable_tests(argv[1]); window_tests(argv[1], argc > 2 ? argv[2] : NULL);
    puts("serial: PASS");
    return 0;
}
