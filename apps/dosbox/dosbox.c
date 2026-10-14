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
#include "loading.h"

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
static int autorun_launch_armed, autorun_launch_sent;
/* Types the next events; 0, or -1 on a fatal VM/input error. */
static int autorun_step(uint32_t id, uint32_t generation, int focused)
{
    uint64_t now = now_ms();
    if (autorun_next >= autorun_count || !focused || now < autorun_at) return 0;
    for (unsigned sent = 0; sent < AUTORUN_EVENTS_PER_TICK && autorun_next < autorun_count; sent++) {
        const struct dosbox_key *k = &autorun[autorun_next];
        struct ciuki_dosvm_command c;
        if (k->code == CIUKI_KEY_ENTER && k->down && autorun_next + 2 == autorun_count && !autorun_launch_armed) {
            command(&c, id, generation);
            if (ciuki_dosvm(CIUKI_DOSVM_LAUNCH, &c)) return -1;
            autorun_launch_armed = 1;
        }
        command(&c, id, generation);
        c.event.type = CIUKI_INPUT_KEY; c.event.code = k->code; c.event.value = k->down;
        if (ciuki_dosvm(CIUKI_DOSVM_INPUT, &c)) {
            if (errno == EAGAIN) return 0;                       /* guest key queue full: next tick */
            if (errno == EACCES) {                               /* focus lost: held keys were released */
                autorun_next = dosbox_resume(autorun, autorun_count, autorun_next);
                return 0;
            }
            if (errno == ESRCH) return -1;
            return -1;
        }
        if (k->code == CIUKI_KEY_ENTER && k->down && autorun_next + 2 == autorun_count)
            autorun_launch_sent = 1;
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
    int channel = 3, result = 1, pause_background = 0;
    const char *name = "CiukiDOS";
    char typed[DOSBOX_COMMAND_MAX + 1] = "";
    size_t typed_length = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= CIUKI_OPEN_MAX) return 2;
            channel = (int)n;
        } else if (!strncmp(argv[i], "--name=", 7)) name = argv[i] + 7;
        else if (!strcmp(argv[i], "--pause-background")) pause_background = 1;
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
        int n = dosbox_launch_keys(typed, autorun, DOSBOX_KEYS_MAX);
        if (n <= 0) { fprintf(stderr, "dosbox: cannot type '%s' at the DOS prompt\n", typed); return 2; }
        autorun_count = (unsigned)n;
    }
    size_t len = strlen(name);
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) return 1;
    int surface = ciuki_surface_create(CIUKI_DOSVM_WIDTH, CIUKI_DOSVM_HEIGHT, CIUKI_SURFACE_XRGB8888);
    if (surface < 0) { perror("dosbox: surface"); return 1; }
    struct ciuki_dosvm_command c;
    uint32_t id = 0, generation = 0, server = 0;
    uint32_t *pixels = ciuki_surface_map(surface, PROT_READ | PROT_WRITE);
    if (pixels == (void *)-1 || !pixels) { close(surface); return 1; }
    struct du_canvas canvas = {pixels, CIUKI_DOSVM_WIDTH, CIUKI_DOSVM_HEIGHT,
                              {0, 0, CIUKI_DOSVM_WIDTH, CIUKI_DOSVM_HEIGHT}};
    const char *icon = dosbox_launch_icon(typed);
    uint64_t started = now_ms(), panel_at = 0, launch_at = 0;
    int loading = !!typed_length, shown = !loading;
    int failure_painted = 0;
    const char *failure = NULL;
    char detail[80] = "Preparing...";
    char fullscreen_notice[80]="";
    uint64_t fullscreen_notice_until=0;
    uint32_t fullscreen_error_seen=0;
    if (loading) dosbox_panel(&canvas, name, icon, NULL, detail, 0);
    struct ciuki_message m;
    desk_message(&m, DESK_HELLO, 4);
    if (send_packet(channel, &m)) goto done;
    desk_message(&m, DESK_CREATE_WINDOW, 16 + (unsigned)len);
    desk_put32(m.data + 4, CIUKI_DOSVM_WIDTH); desk_put32(m.data + 8, CIUKI_DOSVM_HEIGHT);
    desk_put32(m.data + 12, (uint32_t)len); memcpy(m.data + 16, name, len);
    m.fd_count = 1; m.fds[0] = surface;
    if (send_packet(channel, &m)) goto done;
    command(&c, 0, 0); c.surface_fd = surface; memcpy(c.name, name, len);
    if (ciuki_dosvm(CIUKI_DOSVM_CREATE, &c)) {
        failure = "Unable to start";
        snprintf(detail, sizeof(detail), "%s", strerror(errno));
        loading = 1; result = 1;
    } else { id = c.id; generation = c.generation; }
    if (id && pause_background) {
        command(&c,id,generation); c.focused=1;
        if (ciuki_dosvm(CIUKI_DOSVM_BACKGROUND,&c)) goto done;
    }
    int configured = 0, running = 1, focused = 0, fullscreen = 0, fullscreen_painted = 0;
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
            if (packet.opcode == DESK_DOS_FULLSCREEN && id) {
                command(&c,id,generation); c.focused=!fullscreen;
                if (ciuki_dosvm(CIUKI_DOSVM_FULLSCREEN,&c)) {
                    snprintf(fullscreen_notice,sizeof(fullscreen_notice),"%s",strerror(errno));
                    fullscreen_notice_until=now_ms()+3000;
                }
            } else if (packet.opcode == DESK_CONFIGURE) {
                if (packet.width != CIUKI_DOSVM_WIDTH || packet.height != CIUKI_DOSVM_HEIGHT) goto done;
                configured = 1;
            } else if (packet.opcode == DESK_PING) {
                desk_message(&m, DESK_PONG, 8); desk_put32(m.data + 4, packet.serial);
                if (send_packet(channel, &m)) goto done;
            } else if (packet.opcode == DESK_FOCUS || packet.opcode == DESK_KEY ||
                       packet.opcode == DESK_MOTION || packet.opcode == DESK_BUTTON) {
                if (packet.opcode == DESK_MOTION && packet.event.type == CIUKI_INPUT_WHEEL) continue;
                if (!id || failure) continue;
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
        if (failure) goto panel;
        int polled_launch = autorun_launch_armed;
        command(&c, id, generation);
        if (ciuki_dosvm(CIUKI_DOSVM_STATUS, &c)) goto done;
        fullscreen=!!(c.changed&CIUKI_DOSVM_FULL_SCREEN);
        if (!(c.changed&CIUKI_DOSVM_FULLSCREEN_REFUSED)) fullscreen_error_seen=0;
        else if (c.reason!=fullscreen_error_seen) {
            fullscreen_error_seen=c.reason;
            snprintf(fullscreen_notice,sizeof(fullscreen_notice),"%s",strerror((int)c.reason));
            fullscreen_notice_until=now_ms()+3000;
        }
        if (fullscreen) {
            if (!fullscreen_painted && configured) {
                dosbox_panel(&canvas,name,icon,NULL,"Running full screen — Alt+Enter to return",0);
                desk_message(&m,DESK_DAMAGE,20);
                desk_put32(m.data+12,CIUKI_DOSVM_WIDTH); desk_put32(m.data+16,CIUKI_DOSVM_HEIGHT);
                if (send_packet(channel,&m)) goto done;
                fullscreen_painted=1;
            }
            struct timespec pause={.tv_nsec=10000000}; nanosleep(&pause,NULL);
            continue;
        }
        fullscreen_painted=0;
        if (c.state == CIUKI_DOSVM_STOPPED) {
            if (shown) break;
            failure = c.reason == 4 ? "Startup timed out" : "Unable to start";
            snprintf(detail, sizeof(detail), "%s", c.name[0] ? c.name : "Close this window and try again.");
            result = 1;
            goto panel;
        }
        if (configured && now_ms()<fullscreen_notice_until) {
            dosbox_panel(&canvas,name,icon,"Full screen unavailable",fullscreen_notice,0);
            desk_message(&m,DESK_DAMAGE,20);
            desk_put32(m.data+12,CIUKI_DOSVM_WIDTH); desk_put32(m.data+16,CIUKI_DOSVM_HEIGHT);
            if (send_packet(channel,&m)) goto done;
            struct timespec pause={.tv_nsec=10000000}; nanosleep(&pause,NULL);
            continue;
        }
        if (polled_launch && (c.changed & CIUKI_DOSVM_LAUNCH_FAILED)) {
            failure = "Program did not start";
            snprintf(detail, sizeof(detail), "%s", c.name[0] ? c.name : "Check the program name and its files.");
            result = 1;
            goto panel;
        }
        if (c.state == CIUKI_DOSVM_RUNNING && autorun_next < autorun_count) {
            if (!autorun_at) autorun_at = now_ms() + AUTORUN_SETTLE_MS;
            if (autorun_step(id, generation, focused)) {
                failure = "Unable to start";
                snprintf(detail, sizeof(detail), "%s", strerror(errno));
                result = 1;
                goto panel;
            }
        }
        if (autorun_launch_sent && !launch_at) launch_at = now_ms();
        if (loading && polled_launch && (c.changed & CIUKI_DOSVM_VIDEO_READY)) {
            loading = 0; shown = 1;
        }
        if (loading && launch_at && now_ms() - launch_at > 60000) {
            failure = "Startup timed out";
            snprintf(detail, sizeof(detail), "The program has not displayed a frame.");
            result = 1;
        }
        if (loading) goto panel;
        if (configured) {
            command(&c, id, generation);
            if (ciuki_dosvm(CIUKI_DOSVM_PRESENT_BANDS, &c)) {
                if (errno == ESRCH) break;
                goto done;
            }
            if (c.changed) {
                uint64_t bands = c.presentation.bands[0] | ((uint64_t)c.presentation.bands[1] << 32);
                for (unsigned y = 0; y < CIUKI_DOSVM_HEIGHT / 8;) {
                    if (!(bands & (1ull << y))) { y++; continue; }
                    unsigned first = y++;
                    while (y < CIUKI_DOSVM_HEIGHT / 8 && (bands & (1ull << y))) y++;
                    desk_message(&m, DESK_DAMAGE, 20);
                    desk_put32(m.data + 4, 0); desk_put32(m.data + 8, first * 8);
                    desk_put32(m.data + 12, CIUKI_DOSVM_WIDTH); desk_put32(m.data + 16, (y - first) * 8);
                    if (send_packet(channel, &m)) goto done;
                }
            }
        }
        { struct timespec pause = {.tv_nsec = focused ? 2000000 : 10000000}; nanosleep(&pause, NULL); }
        continue;
panel:
        if (configured && (failure ? !failure_painted : now_ms() - panel_at >= 80)) {
            dosbox_panel(&canvas, name, icon, failure, detail, now_ms() - started);
            desk_message(&m, DESK_DAMAGE, 20);
            desk_put32(m.data + 4, 0); desk_put32(m.data + 8, 0);
            desk_put32(m.data + 12, CIUKI_DOSVM_WIDTH); desk_put32(m.data + 16, CIUKI_DOSVM_HEIGHT);
            if (send_packet(channel, &m)) goto done;
            panel_at = now_ms();
            if (failure) failure_painted = 1;
        }
        { struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL); }
    }
    if (!failure) result = 0;
done:
    command(&c, id, generation);
    if (id && ciuki_dosvm(CIUKI_DOSVM_CLOSE, &c)) result = 1;
    desk_message(&m, DESK_CLOSE, 4); send_packet(channel, &m);
    close(surface); close(channel);
    return result;
}
