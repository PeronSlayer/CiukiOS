/* SPDX-License-Identifier: MIT
 * /Programs/serial: CiukiOS Serial Terminal (desktop client, protocol v2).
 * Usage: serial [--channel-fd=N] [--test=serial]. Ports: /dev/com1 .. /dev/com8 (f1-111 native COM ABI). */
#include "serial.h"
#include <ciuki/com.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int port_fd = -1;
static int native_open(void *ctx, unsigned number)
{
    (void)ctx; char path[16];
    snprintf(path, sizeof(path), "/dev/com%u", number);
    port_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    return port_fd < 0 ? -errno : 0;
}
static void native_close(void *ctx) { (void)ctx; if (port_fd >= 0) close(port_fd); port_fd = -1; }
static int native_configure(void *ctx, const struct serial_line *line)
{
    (void)ctx;
    struct ciuki_com_line l = {.version = CIUKI_COM_VERSION, .size = sizeof(l), .baud = line->baud, .data_bits = (uint32_t)line->data_bits,
        .stop_bits = (uint32_t)line->stop_bits, .parity = (uint32_t)line->parity};
    return ciuki_devctl(port_fd, COM_SET_LINE, &l, sizeof(l)) ? -errno : 0;
}
static int native_read(void *ctx, uint8_t *buffer, size_t size)
{
    (void)ctx; ssize_t n = read(port_fd, buffer, size);
    if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -errno;
    return (int)n;
}
static int native_write(void *ctx, const uint8_t *buffer, size_t size)
{
    (void)ctx; ssize_t n = write(port_fd, buffer, size);
    if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -errno;
    return (int)n;
}
static int native_modem(void *ctx, unsigned outputs)
{
    (void)ctx;
    struct ciuki_com_modem m = {.version = CIUKI_COM_VERSION, .size = sizeof(m), .outputs = (outputs & SERIAL_DTR ? CIUKI_COM_DTR : 0u) | (outputs & SERIAL_RTS ? CIUKI_COM_RTS : 0u)};
    return ciuki_devctl(port_fd, COM_SET_MODEM, &m, sizeof(m)) ? -errno : 0;
}
static int native_status(void *ctx, struct serial_status *out)
{
    (void)ctx; struct ciuki_com_status s = {.version = CIUKI_COM_VERSION, .size = sizeof(s)};
    if (ciuki_devctl(port_fd, COM_GET_STATUS, &s, sizeof(s))) return -errno;
    out->modem = s.modem; out->rx_queued = s.rx_queued; out->tx_queued = s.tx_queued; out->errors = s.errors;
    return 0;
}
static const struct serial_ops native = {native_open, native_close, native_configure, native_read, native_write, native_modem, native_status, NULL};
const struct serial_ops *serial_native_ops(void) { return &native; }

struct serial_main { struct du_window window; struct du_ui ui; struct serial_app app; };
static void quit(void *host) { du_window_close(&((struct serial_main *)host)->window); }
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "serial", &channel, &test, &path) || path) return 2;
    struct serial_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    int w, h;
    acc_window_size(SERIAL_W, SERIAL_H, &w, &h);
    if (du_window_open(&m->window, &m->ui, channel, w, h, "Serial Terminal")) {
        fprintf(stderr, "serial: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    serial_init(&m->app.term, serial_native_ops());
    m->app.quit = quit; m->app.host = m;
    serial_attach(&m->app, &m->ui);
    du_timer_start(&m->ui, SV_TIMER, 20000000ull, 1);   /* the port is polled every 20 ms */
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    serial_disconnect(&m->app.term); serial_close_files(&m->app.term);
    du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
