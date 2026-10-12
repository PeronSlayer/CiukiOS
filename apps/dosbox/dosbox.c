/* /Programs/dosbox: one CiukiDOS virtual machine in one desktop window (f3-02).
 * A native desktop client of the Ciuki VMM, not the third-party DOSBox
 * emulator. The kernel owns the VM and renders its frame into our surface;
 * this process forwards window focus and input, reports damage and closes
 * the VM with the window. Its death terminates the VM through the
 * supervisor. Protocol: apps/desktop/protocol.h (v2 close requests).
 * Usage: dosbox [--channel-fd=N] [--name=TITLE] [COMMAND...]: the optional
 * command (its words joined by spaces) is typed at the DOS prompt once the
 * VM is running and the window has the focus (autorun.h; Programs > Games).
 * SPDX-License-Identifier: MIT */
#include "autorun.h"
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

static void command(struct ciuki_dosvm_command *c, uint32_t id, uint32_t generation)
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
static uint64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
/* After the VM reports DOS ready, the shell still prints its prompt; after
 * each line, the shell runs it (CD) before the next one is typed. */
#define AUTORUN_SETTLE_MS 400u
#define AUTORUN_LINE_MS 800u
#define AUTORUN_EVENTS_PER_TICK 1u   /* about 20 ms per character: the BIOS buffer holds 15 keys */
static struct dosbox_key autorun[DOSBOX_KEYS_MAX];
static unsigned autorun_count, autorun_next;
static uint64_t autorun_at;   /* 0: not yet armed (VM still booting) */
/* Types the next events of the command; 0, or -1 when the VM is gone. */
static int autorun_step(uint32_t id, uint32_t generation, int focused)
{
    uint64_t now = now_ms();
    if (autorun_next >= autorun_count || !focused || now < autorun_at) return 0;
    for (unsigned sent = 0; sent < AUTORUN_EVENTS_PER_TICK && autorun_next < autorun_count; sent++) {
        const struct dosbox_key *k = &autorun[autorun_next];
        struct ciuki_dosvm_command c;
        command(&c, id, generation);
        c.event.type = CIUKI_INPUT_KEY; c.event.code = k->code; c.event.value = k->down;
        if (ciuki_dosvm(CIUKI_DOSVM_INPUT, &c)) {
            if (errno == EAGAIN) return 0;                       /* guest key queue full: next tick */
            if (errno == EACCES) {                               /* focus lost: held keys were released */
                autorun_next = dosbox_resume(autorun, autorun_count, autorun_next);
                return 0;
            }
            if (errno == ESRCH) return -1;
            autorun_next = autorun_count;                        /* anything else: stop typing */
            return 0;
        }
        autorun_next++;
        if (k->line_end) { autorun_at = now + AUTORUN_LINE_MS; return 0; }
    }
    return 0;
}
static int name_valid(const char *name)
{
    size_t n = strlen(name);
    if (!n || n > CIUKI_DOSVM_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) if (name[i] < 32 || name[i] > 126) return 0;
    return 1;
}
int main(int argc, char **argv)
{
    int channel = 3, result = 1;
    const char *name = "CiukiDOS";
    char typed[DOSBOX_COMMAND_MAX + 1] = "";
    size_t typed_length = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= CIUKI_OPEN_MAX) return 2;
            channel = (int)n;
        } else if (!strncmp(argv[i], "--name=", 7)) name = argv[i] + 7;
        else if (!strncmp(argv[i], "--", 2)) return 2;
        else {                                                    /* COMMAND words */
            size_t n = strlen(argv[i]);
            if (typed_length + !!typed_length + n > DOSBOX_COMMAND_MAX) return 2;
            if (typed_length) typed[typed_length++] = ' ';
            memcpy(typed + typed_length, argv[i], n + 1); typed_length += n;
        }
    }
    if (!name_valid(name)) return 2;
    if (typed_length) {
        int n = dosbox_command_keys(typed, autorun, DOSBOX_KEYS_MAX);
        if (n <= 0) { fprintf(stderr, "dosbox: cannot type '%s' at the DOS prompt\n", typed); return 2; }
        autorun_count = (unsigned)n;
    }
    size_t len = strlen(name);
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) return 1;
    int surface = ciuki_surface_create(CIUKI_DOSVM_WIDTH, CIUKI_DOSVM_HEIGHT, CIUKI_SURFACE_XRGB8888);
    if (surface < 0) { perror("dosbox: surface"); return 1; }
    struct ciuki_dosvm_command c;
    command(&c, 0, 0); c.surface_fd = surface; memcpy(c.name, name, len);
    if (ciuki_dosvm(CIUKI_DOSVM_CREATE, &c)) { perror("dosbox: create"); close(surface); return 1; }
    uint32_t id = c.id, generation = c.generation, server = 0;
    struct ciuki_message m;
    desk_message(&m, DESK_HELLO, 4);
    if (send_packet(channel, &m)) goto done;
    desk_message(&m, DESK_CREATE_WINDOW, 16 + (unsigned)len);
    desk_put32(m.data + 4, CIUKI_DOSVM_WIDTH); desk_put32(m.data + 8, CIUKI_DOSVM_HEIGHT);
    desk_put32(m.data + 12, (uint32_t)len); memcpy(m.data + 16, name, len);
    m.fd_count = 1; m.fds[0] = surface;
    if (send_packet(channel, &m)) goto done;
    int configured = 0, running = 1, focused = 0;
    while (running) {
        for (unsigned budget = 0; budget < 32; budget++) {
            int n = ciuki_channel_recv(channel, &m, DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
            if (n <= 0) { running = 0; break; }               /* desktop gone */
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
                command(&c, id, generation);
                unsigned op = packet.opcode == DESK_FOCUS ? CIUKI_DOSVM_FOCUS : CIUKI_DOSVM_INPUT;
                if (op == CIUKI_DOSVM_FOCUS) c.focused = focused = packet.focused ? 1 : 0;
                else c.event = packet.event;
                /* A full guest key queue drops the key, as a full BIOS buffer does. */
                if (ciuki_dosvm(op, &c) && errno != EAGAIN && errno != EACCES) {
                    if (errno == ESRCH) { running = 0; break; }
                    goto done;
                }
            }
        }
        if (!running) break;
        command(&c, id, generation);
        if (ciuki_dosvm(CIUKI_DOSVM_STATUS, &c)) goto done;
        if (c.state == CIUKI_DOSVM_STOPPED) break;             /* EXIT, reset or fault */
        if (c.state == CIUKI_DOSVM_RUNNING && autorun_next < autorun_count) {
            if (!autorun_at) autorun_at = now_ms() + AUTORUN_SETTLE_MS;
            if (autorun_step(id, generation, focused)) break;
        }
        if (configured) {
            command(&c, id, generation);
            if (ciuki_dosvm(CIUKI_DOSVM_PRESENT, &c)) {
                if (errno == ESRCH) break;
                goto done;
            }
            if (c.changed) {
                desk_message(&m, DESK_DAMAGE, 20);
                desk_put32(m.data + 4, (uint32_t)c.damage.dst_x); desk_put32(m.data + 8, (uint32_t)c.damage.dst_y);
                desk_put32(m.data + 12, c.damage.width); desk_put32(m.data + 16, c.damage.height);
                if (send_packet(channel, &m)) goto done;
            }
        }
        struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
    }
    result = 0;
done:
    command(&c, id, generation);
    if (ciuki_dosvm(CIUKI_DOSVM_CLOSE, &c)) result = 1;
    desk_message(&m, DESK_CLOSE, 4); send_packet(channel, &m);
    close(surface); close(channel);
    return result;
}
