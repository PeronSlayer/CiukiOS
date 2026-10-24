/* SPDX-License-Identifier: MIT
 * Serial Terminal model: connection, keyboard, capture, text and XMODEM transfers. */
#include "serial.h"
#if !defined(__linux__) && !defined(__APPLE__)
#include <unistd.h>
#else
/* Only these POSIX calls are needed; avoid host sysconf enums colliding with
 * the deliberately freestanding CiukiOS ABI used by the model harness. */
int close(int);
int fsync(int);
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const unsigned bauds[] = {110, 300, 600, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200};
int serial_baud_valid(unsigned baud)
{
    for (unsigned i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++) if (bauds[i] == baud) return 1;
    return 0;
}
int serial_parse_format(const char *text, struct serial_line *line)
{
    if (!text || strlen(text) != 3) return -EINVAL;
    int bits = text[0] - '0', stop = text[2] - '0', parity;
    switch (text[1] | 0x20) { case 'n': parity = SERIAL_NONE; break; case 'o': parity = SERIAL_ODD; break; case 'e': parity = SERIAL_EVEN; break;
                              case 'm': parity = SERIAL_MARK; break; case 's': parity = SERIAL_SPACE; break; default: return -EINVAL; }
    if (bits < 5 || bits > 8 || stop < 1 || stop > 2) return -EINVAL;
    line->data_bits = bits; line->parity = parity; line->stop_bits = stop;
    return 0;
}
void serial_format_text(const struct serial_line *line, char *out, size_t size)
{
    snprintf(out, size, "%d%c%d", line->data_bits, "NOEMS"[line->parity], line->stop_bits);
}
void serial_init(struct serial_term *t, const struct serial_ops *ops)
{
    memset(t, 0, sizeof(*t)); t->ops = *ops; t->port = 1;
    t->line = (struct serial_line){9600, 8, SERIAL_NONE, 1};
    t->ansi = 1; screen_init(&t->screen, 1);
    t->outputs = SERIAL_DTR | SERIAL_RTS;
}
void serial_close_files(struct serial_term *t)
{
    if (t->transfer_temp[0]) { remove(t->transfer_temp);t->transfer_temp[0]=0; }
    free(t->transfer);t->transfer=NULL;
    serial_capture_stop(t);
    if (t->xm_file) { fclose(t->xm_file); t->xm_file = NULL; }
    free(t->text); t->text = NULL; t->text_length = t->text_at = 0;
    t->xm_active = 0;
}
int serial_connect(struct serial_term *t)
{
    if (t->connected) return 0;
    int r = t->ops.open(t->ops.ctx, t->port);
    if (r) { snprintf(t->message, sizeof(t->message), "COM%u could not be opened: %s", t->port, r == -EBUSY ? "the port is in use" : r == -ENOENT || r == -ENODEV ? "there is no such port" : "an error occurred"); return r; }
    r = t->ops.configure(t->ops.ctx, &t->line);
    if (!r && t->ops.modem) r = t->ops.modem(t->ops.ctx, t->outputs);
    if (r) { t->ops.close(t->ops.ctx); snprintf(t->message, sizeof(t->message), "COM%u could not be set up", t->port); return r; }
    t->connected = 1; t->tx_head = t->tx_count = 0; t->message[0] = 0;
    return 0;
}
void serial_disconnect(struct serial_term *t)
{
    if (!t->connected) return;
    if (t->xm_active) {      /* a transfer cannot outlive the connection */
        serial_xmodem_cancel(t); xmodem_cancel(&t->xm);
        if (t->xm_file) { fclose(t->xm_file); t->xm_file = NULL; }
        if (!t->xm_sending && !t->transfer) remove(t->xm_path);
        snprintf(t->message, sizeof(t->message), "Transfer cancelled: the port was closed");
        t->xm_active = 0;
        if (t->transfer_temp[0]) { remove(t->transfer_temp);t->transfer_temp[0]=0; }
        free(t->transfer);t->transfer=NULL;
    }
    free(t->text); t->text = NULL; t->text_length = t->text_at = 0;
    t->ops.close(t->ops.ctx); t->connected = 0; t->tx_count = 0;
}
int serial_set_port(struct serial_term *t, unsigned number)
{
    if (number < 1 || number > SERIAL_PORTS) return -EINVAL;
    int was = t->connected;
    if (was) serial_disconnect(t);
    t->port = number;
    return was ? serial_connect(t) : 0;
}
int serial_set_line(struct serial_term *t, const struct serial_line *line)
{
    if (!serial_baud_valid(line->baud) || line->data_bits < 5 || line->data_bits > 8 || line->parity < 0 || line->parity > SERIAL_SPACE ||
        line->stop_bits < 1 || line->stop_bits > 2) return -EINVAL;
    int r = t->connected ? t->ops.configure(t->ops.ctx, line) : 0;
    if (!r) t->line = *line;
    return r;
}
int serial_set_modem(struct serial_term *t, unsigned outputs)
{
    outputs &= SERIAL_DTR | SERIAL_RTS;
    int r = t->connected && t->ops.modem ? t->ops.modem(t->ops.ctx, outputs) : 0;
    if (!r) t->outputs = outputs;
    return r;
}
/* ---- keyboard ---- */
static size_t queue_bytes(struct serial_term *t, const uint8_t *bytes, size_t length)
{
    size_t room = SERIAL_TX - t->tx_count, n = length < room ? length : room;
    for (size_t i = 0; i < n; i++) t->tx[(t->tx_head + t->tx_count++) % SERIAL_TX] = bytes[i];
    return n;
}
size_t serial_send(struct serial_term *t, const uint8_t *bytes, size_t length)
{
    if (!t->connected || t->xm_active) return 0;
    size_t n = queue_bytes(t, bytes, length);
    if (t->local_echo) screen_feed(&t->screen, bytes, n);
    return n;
}
size_t serial_key(struct serial_term *t, unsigned key, unsigned ch, unsigned modifiers)
{
    uint8_t out[4]; size_t n = 0;
    static const char qrow[] = "qwertyuiop", arow[] = "asdfghjkl", zrow[] = "zxcvbnm";
    t->scroll_back = 0;
    if (key == DU_KEY_ENTER || key == 0x11c) { out[n++] = '\r'; if (t->send_lf) out[n++] = '\n'; }
    else if (key == DU_KEY_BACKSPACE) out[n++] = 0x08;
    else if (key == DU_KEY_TAB) out[n++] = 0x09;
    else if (key == DU_KEY_ESC) out[n++] = 0x1b;
    else if (key == DU_KEY_DELETE) out[n++] = 0x7f;
    else if (key == DU_KEY_UP || key == DU_KEY_DOWN || key == DU_KEY_RIGHT || key == DU_KEY_LEFT) {
        out[n++] = 0x1b; out[n++] = '['; out[n++] = key == DU_KEY_UP ? 'A' : key == DU_KEY_DOWN ? 'B' : key == DU_KEY_RIGHT ? 'C' : 'D';
    } else if (key == DU_KEY_HOME) { out[n++] = 0x1b; out[n++] = '['; out[n++] = 'H'; }
    else if (modifiers & DU_CTRL) {
        int letter = -1;
        if (key >= 0x10 && key <= 0x19) letter = qrow[key - 0x10]; else if (key >= 0x1e && key <= 0x26) letter = arow[key - 0x1e];
        else if (key >= 0x2c && key <= 0x32) letter = zrow[key - 0x2c];
        if (letter < 0) return 0;
        out[n++] = (uint8_t)(letter & 0x1f);
    } else if (ch >= 32 && ch < 127) out[n++] = (uint8_t)ch;
    else if (ch >= 0xa0 && ch <= 0xff) out[n++] = (uint8_t)ch;
    else return 0;
    return serial_send(t, out, n);
}
/* ---- capture and transfers ---- */
int serial_capture_start(struct serial_term *t, const char *path)
{
    serial_capture_stop(t);
    if (strlen(path) >= sizeof(t->capture_path)) return -ENAMETOOLONG;
    t->capture = fopen(path, "ab");
    if (!t->capture) return -errno;
    snprintf(t->capture_path, sizeof(t->capture_path), "%s", path);
    return 0;
}
void serial_capture_stop(struct serial_term *t)
{
    if (t->capture) { fclose(t->capture); t->capture = NULL; }
    t->capture_path[0] = 0;
}
int serial_send_text_file(struct serial_term *t, const char *path)
{
    if (!t->connected) return -ENOTCONN;
    if (t->xm_active || t->text) return -EBUSY;
    FILE *f = fopen(path, "rb");
    if (!f) return -errno;
    fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
    if (size < 0 || size > 1024 * 1024) { fclose(f); return -EFBIG; }
    char *data = malloc((size_t)size + 1);
    if (!data) { fclose(f); return -ENOMEM; }
    size_t got = fread(data, 1, (size_t)size, f); int bad = ferror(f); fclose(f);
    if (bad) { free(data); return -EIO; }
    t->text = data; t->text_length = got; t->text_at = 0;
    return 0;
}
static int xm_read(void *ctx, uint8_t *buffer, size_t size)
{
    struct serial_term *t = ctx; size_t n = fread(buffer, 1, size, t->xm_file);
    return ferror(t->xm_file) ? -EIO : (int)n;
}
static int xm_write(void *ctx, const uint8_t *buffer, size_t size)
{
    struct serial_term *t = ctx;
    return fwrite(buffer, 1, size, t->xm_file) == size ? 0 : -EIO;
}
static int xm_begin(struct serial_term *t, const char *path, int sending, int one_k_or_crc)
{
    if (!t->connected) return -ENOTCONN;
    if (t->xm_active || t->text) return -EBUSY;
    if (strlen(path) >= sizeof(t->xm_path)) return -ENAMETOOLONG;
    t->xm_file = fopen(path, sending ? "rb" : "wb");
    if (!t->xm_file) return -errno;
    snprintf(t->xm_path, sizeof(t->xm_path), "%s", path);
    struct xmodem_io io = {xm_read, xm_write, t};
    if (sending) xmodem_send_init(&t->xm, &io, one_k_or_crc); else xmodem_receive_init(&t->xm, &io, one_k_or_crc);
    t->xm_active = 1; t->xm_sending = sending; t->xm_linger_ms = 0;
    return 0;
}
int serial_xmodem_send(struct serial_term *t, const char *path, int one_k) { return xm_begin(t, path, 1, one_k); }
int serial_xmodem_receive(struct serial_term *t, const char *path, int crc) { return xm_begin(t, path, 0, crc); }
static int transfer_seek(void *ctx, uint32_t offset)
{ struct serial_term *t=ctx;return fseek(t->xm_file,(long)offset,SEEK_SET) ? -EIO : 0; }
int serial_transfer_start(struct serial_term *t,const char *path,int protocol,int sending)
{
    if (!t->connected) return -ENOTCONN;
    if (t->xm_active || t->text) return -EBUSY;
    if (!path || strlen(path)+8>=sizeof(t->xm_path)) return -ENAMETOOLONG;
    struct transfer *engine=calloc(1,sizeof(*engine));if(!engine)return -ENOMEM;
    FILE *f=NULL;int e=0;long size=0;
    if (sending) {
        f=fopen(path,"rb");
        if (f) { if(fseek(f,0,SEEK_END) || (size=ftell(f))<0 || fseek(f,0,SEEK_SET))e=-EIO; }
    } else {
        snprintf(t->transfer_temp,sizeof(t->transfer_temp),"%s.XXXXXX",path);
        int fd=mkstemp(t->transfer_temp);
        if(fd>=0) { f=fdopen(fd,"wb");if(!f)close(fd); }
    }
    if(!f && !e)e=-errno;
    if(!e) {
        struct transfer_io io={xm_read,xm_write,transfer_seek,t};
        const char *base=strrchr(path,'/');base=base ? base+1 : path;
        e=transfer_init(engine,protocol,sending,&io,base,(uint32_t)size);
    }
    if(e) {
        if(f)fclose(f);
        if(t->transfer_temp[0]) { remove(t->transfer_temp);t->transfer_temp[0]=0; }
        free(engine);return e;
    }
    t->xm_file=f;t->transfer=engine;t->xm_active=1;t->xm_sending=sending;t->xm_linger_ms=0;
    snprintf(t->xm_path,sizeof(t->xm_path),"%s",path);return 0;
}
static void transfer_drain(struct serial_term *t)
{
    uint8_t out[256];size_t n;
    while(t->tx_count<SERIAL_TX && (n=transfer_take(t->transfer,out,SERIAL_TX-t->tx_count<sizeof(out) ? SERIAL_TX-t->tx_count : sizeof(out))))queue_bytes(t,out,n);
}
static void transfer_finish(struct serial_term *t,unsigned elapsed_ms)
{
    struct transfer *x=t->transfer;
    if(!x->state || x->out_length)return;
    if(t->xm_file) {
        int bad=0;
        if(!t->xm_sending && x->state==1 && (fflush(t->xm_file) || fsync(fileno(t->xm_file))))bad=1;
        if(fclose(t->xm_file))bad=1;
        t->xm_file=NULL;
        if(!t->xm_sending && x->state==1 && !bad && rename(t->transfer_temp,t->xm_path))bad=1;
        if(bad)x->state=-1;
        if(t->transfer_temp[0]) { remove(t->transfer_temp);t->transfer_temp[0]=0; }
        snprintf(t->message,sizeof(t->message),"Transfer %s: %u bytes",x->state==1 ? "complete" : x->state==-2 ? "cancelled" : "failed",x->position);
    }
    if(!t->xm_sending && x->state==1 && (t->xm_linger_ms+=elapsed_ms)<3000)return;
    free(t->transfer);t->transfer=NULL;t->xm_active=0;
}
void serial_xmodem_cancel(struct serial_term *t)
{
    if (!t->xm_active) return;
    if (t->transfer) { transfer_cancel(t->transfer);transfer_drain(t);return; }
    xmodem_cancel(&t->xm);
    uint8_t out[8]; size_t n = xmodem_take(&t->xm, out, sizeof(out));
    queue_bytes(t, out, n);
}
#define XM_LINGER_MS 3000
static void xm_finish_if_done(struct serial_term *t, unsigned elapsed_ms)
{
    if (!t->xm_active || !xmodem_finished(&t->xm) || t->xm.pending_length) return;
    if (!t->xm_sending && t->xm.state == XM_DONE && (t->xm_linger_ms += elapsed_ms) < XM_LINGER_MS) {   /* the file is complete: close it now, stay quiet-ready */
        if (t->xm_file) { fclose(t->xm_file); t->xm_file = NULL; }
        snprintf(t->message, sizeof(t->message), "Transfer complete: %s", t->xm.message);
        return;
    }
    t->xm_linger_ms = 0;
    if (t->xm_file) { fclose(t->xm_file); t->xm_file = NULL; }
    snprintf(t->message, sizeof(t->message), "%s: %s", t->xm.state == XM_DONE ? "Transfer complete" : t->xm.state == XM_CANCELLED ? "Transfer cancelled" : "Transfer failed", t->xm.message);
    if (t->xm.state == XM_FAILED && !t->xm_sending) remove(t->xm_path);   /* never leave a half-received file */
    t->xm_active = 0;
}
/* ---- the loop ---- */
static void flush_output(struct serial_term *t)
{
    while (t->tx_count) {
        uint8_t chunk[256]; size_t n = t->tx_count < sizeof(chunk) ? t->tx_count : sizeof(chunk);
        for (size_t i = 0; i < n; i++) chunk[i] = t->tx[(t->tx_head + i) % SERIAL_TX];
        int w = t->ops.write(t->ops.ctx, chunk, n);
        if (w <= 0) { if (w < 0) { snprintf(t->message, sizeof(t->message), "The port reported a write error"); t->tx_count = 0; } break; }
        t->tx_head = (t->tx_head + (unsigned)w) % SERIAL_TX; t->tx_count -= (unsigned)w; t->transmitted += (uint64_t)w;
    }
}
static void deliver(struct serial_term *t, const uint8_t *data, size_t length)
{
    uint8_t shown[1024]; size_t n = 0;
    for (size_t i = 0; i < length; i++) {
        shown[n++] = data[i];
        if (t->receive_lf && data[i] == '\r' && (i + 1 == length || data[i + 1] != '\n')) shown[n++] = '\n';
        if (n + 2 > sizeof(shown) || i + 1 == length) { screen_feed(&t->screen, shown, n); n = 0; }
    }
    if (t->capture) {
        for (size_t i = 0; i < length; i++) if (data[i] == '\t' || data[i] == '\n' || data[i] == '\r' || (data[i] >= 32 && data[i] != 0x7f)) fputc(data[i], t->capture);
        fflush(t->capture);
    }
}
void serial_pump(struct serial_term *t, unsigned elapsed_ms)
{
    if (!t->connected) return;
    uint8_t buffer[512]; int n=0, rounds = 0;
    if(t->transfer)transfer_drain(t);
    while ((!t->transfer || t->transfer->out_length<2000) && rounds++ < 8 && (n = t->ops.read(t->ops.ctx, buffer, sizeof(buffer))) > 0) {
        t->received += (uint64_t)n;
        if (t->xm_active) {
            size_t used = 0;
            while (used < (size_t)n) {
                if(t->transfer) {
                    size_t took=transfer_input(t->transfer,buffer+used,(size_t)n-used);
                    transfer_drain(t);
                    if(!took)break;
                    used+=took;continue;
                }
                size_t took = xmodem_input(&t->xm, buffer + used, (size_t)n - used);
                uint8_t out[XMODEM_OUT]; size_t m;
                while ((m = xmodem_take(&t->xm, out, sizeof(out))) > 0) { if (queue_bytes(t, out, m) < m) break; }
                if (!took) break;
                used += took;
            }
        } else deliver(t, buffer, (size_t)n);
        if (n < (int)sizeof(buffer)) break;
    }
    if (n < 0) { snprintf(t->message, sizeof(t->message), "The port reported a read error"); }
    if(t->transfer) {
        transfer_tick(t->transfer,elapsed_ms);transfer_drain(t);
        if(!t->transfer->state)snprintf(t->transfer->message,sizeof(t->transfer->message),"%s %u bytes",t->xm_sending ? "Sending" : "Receiving",t->transfer->position);
        transfer_finish(t,elapsed_ms);
    }
    else if (t->xm_active) {
        xmodem_tick(&t->xm, elapsed_ms);
        uint8_t out[XMODEM_OUT]; size_t m;
        while (t->tx_count + 256 < SERIAL_TX && (m = xmodem_take(&t->xm, out, 256)) > 0) queue_bytes(t, out, m);
        xm_finish_if_done(t, elapsed_ms);
    } else if (t->text) {
        while (t->text_at < t->text_length && t->tx_count < SERIAL_TX - 2) {
            uint8_t c = (uint8_t)t->text[t->text_at++];
            if (c == '\n') {   /* line ends go out as a return (and a line feed when asked); a CR LF pair already sent its return */
                int pair = t->text_at >= 2 && t->text[t->text_at - 2] == '\r';
                if (!pair) { c = '\r'; queue_bytes(t, &c, 1); }
                if (t->send_lf) { c = '\n'; queue_bytes(t, &c, 1); }
                continue;
            }
            queue_bytes(t, &c, 1);
            if (t->local_echo) screen_feed(&t->screen, &c, 1);
        }
        if (t->text_at >= t->text_length) { free(t->text); t->text = NULL; t->text_length = t->text_at = 0; }
    }
    flush_output(t);
    if (t->ops.status) t->ops.status(t->ops.ctx, &t->status);
}
void serial_clear(struct serial_term *t) { screen_clear(&t->screen); t->scroll_back = 0; }
void serial_status_line(const struct serial_term *t, char *out, size_t size)
{
    char fmt[8]; serial_format_text(&t->line, fmt, sizeof(fmt));
    if (!t->connected) { snprintf(out, size, "Disconnected   COM%u %u %s%s%s", t->port, t->line.baud, fmt, t->message[0] ? "   " : "", t->message); return; }
    snprintf(out, size, "Connected   COM%u %u %s   %s %s %s %s   RX %llu  TX %llu%s%s%s", t->port, t->line.baud, fmt,
             t->outputs & SERIAL_DTR ? "DTR" : "dtr", t->outputs & SERIAL_RTS ? "RTS" : "rts", t->status.modem & 0x10 ? "CTS" : "cts", t->status.modem & 0x20 ? "DSR" : "dsr",
             (unsigned long long)t->received, (unsigned long long)t->transmitted, t->capture ? "   CAPTURE" : "",
             t->xm_active ? "   " : t->message[0] ? "   " : "", t->xm_active ? (t->transfer ? t->transfer->message : t->xm.message) : t->message);
}
