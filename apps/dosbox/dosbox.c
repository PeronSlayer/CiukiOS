/* A native desktop client, not the third-party DOSBox emulator.
 * SPDX-License-Identifier: MIT */
#include <ciuki/dosvm.h>
#include <ciuki/channel.h>
#include <ciuki/surface.h>
#include <ciuki/desktop.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void init(struct ciuki_dosvm_command *c, uint32_t id, uint32_t generation)
{
    memset(c, 0, sizeof(*c));
    c->version = CIUKI_DOSVM_VERSION; c->size = sizeof(*c);
    c->id = id; c->generation = generation;
}
static int send_packet(int fd, struct ciuki_message *m)
{
    m->data[0] = DESK_SHELL_VERSION;
    int e;
    do { e = ciuki_channel_send(fd, m, 0); } while (e < 0 && errno == EINTR);
    return e;
}
int main(int argc, char **argv)
{
    int channel = 3, result = 1;
    const char *name = "CiukiDOS";
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= CIUKI_OPEN_MAX) return 2;
            channel = n;
        } else if (!strncmp(argv[i], "--name=", 7)) name = argv[i] + 7;
        else return 2;
    }
    size_t len = strlen(name);
    if (!len || len > 31) return 2;
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) return 1;
    int surface = ciuki_surface_create(CIUKI_DOSVM_WIDTH, CIUKI_DOSVM_HEIGHT, CIUKI_SURFACE_XRGB8888);
    if (surface < 0) return 1;
    struct ciuki_dosvm_command c;
    init(&c, 0, 0); c.surface_fd = surface; memcpy(c.name, name, len);
    if (ciuki_dosvm(CIUKI_DOSVM_CREATE, &c)) { perror("dosbox: create"); close(surface); return 1; }
    uint32_t id = c.id, generation = c.generation, server = 0;
    struct ciuki_message m;
    desk_message(&m, DESK_HELLO, 4);
    if (send_packet(channel, &m)) goto done;
    desk_message(&m, DESK_CREATE_WINDOW, 16 + len);
    desk_put32(m.data + 4, CIUKI_DOSVM_WIDTH); desk_put32(m.data + 8, CIUKI_DOSVM_HEIGHT);
    desk_put32(m.data + 12, len); memcpy(m.data + 16, name, len);
    m.fd_count = 1; m.fds[0] = surface;
    if (send_packet(channel, &m)) goto done;
    int configured = 0, running = 1;
    while (running) {
        for (unsigned budget = 0; budget < 32; budget++) {
            int n = ciuki_channel_recv(channel, &m, DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
            if (n <= 0) { running = 0; break; }
            struct desk_packet packet;
            int valid = m.sender_pid && (!server || server == m.sender_pid) &&
                        m.data[0] == DESK_SHELL_VERSION && desk_parse(&m, 0, NULL, &packet);
            for (unsigned i = 0; i < m.fd_count && i < CIUKI_MESSAGE_FD_MAX; i++) close(m.fds[i]);
            if (!valid) goto done;
            server = m.sender_pid;
            if (packet.opcode == DESK_CLOSE_REQUEST || packet.opcode == DESK_CLOSED) { running = 0; break; }
            if (packet.opcode == DESK_CONFIGURE) {
                if (packet.width != CIUKI_DOSVM_WIDTH || packet.height != CIUKI_DOSVM_HEIGHT) goto done;
                configured = 1;
            } else if (packet.opcode == DESK_PING) {
                desk_message(&m, DESK_PONG, 8); desk_put32(m.data + 4, packet.serial);
                if (send_packet(channel, &m)) goto done;
            } else if (packet.opcode == DESK_FOCUS || packet.opcode == DESK_KEY ||
                       packet.opcode == DESK_MOTION || packet.opcode == DESK_BUTTON) {
                init(&c, id, generation);
                unsigned op = packet.opcode == DESK_FOCUS ? CIUKI_DOSVM_FOCUS : CIUKI_DOSVM_INPUT;
                if (op == CIUKI_DOSVM_FOCUS) c.focused = packet.focused;
                else c.event = packet.event;
                if (ciuki_dosvm(op, &c) && errno != EAGAIN && errno != EACCES && errno != ESRCH) goto done;
            }
        }
        init(&c, id, generation);
        if (ciuki_dosvm(CIUKI_DOSVM_STATUS, &c)) goto done;
        if (c.state >= 4) break;
        if (configured) {
            init(&c, id, generation);
            if (ciuki_dosvm(CIUKI_DOSVM_PRESENT, &c)) goto done;
            if (c.changed) {
                desk_message(&m, DESK_DAMAGE, 20);
                desk_put32(m.data + 4, c.damage.dst_x); desk_put32(m.data + 8, c.damage.dst_y);
                desk_put32(m.data + 12, c.damage.width); desk_put32(m.data + 16, c.damage.height);
                if (send_packet(channel, &m)) goto done;
            }
        }
        struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
    }
    result = 0;
done:
    init(&c, id, generation);
    if (ciuki_dosvm(CIUKI_DOSVM_CLOSE, &c)) result = 1;
    desk_message(&m, DESK_CLOSE, 4); send_packet(channel, &m);
    close(surface); close(channel);
    return result;
}
