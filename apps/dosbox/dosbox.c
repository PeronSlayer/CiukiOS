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
#include "../desktop/protocol.h"
#include "clipboard.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "loading.h"
#include "settings.h"
#include "network_settings.h"

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
/* G3 end of session: SESSION_ANSWER (serial 0 AGREE registers the window). */
static int session_answer(int channel, uint32_t serial, uint32_t answer)
{
    struct ciuki_message m;
    desk_message(&m, DESK_SESSION_ANSWER, 12);
    desk_put32(m.data + 4, serial); desk_put32(m.data + 8, answer);
    return send_packet(channel, &m);
}
/* A VM whose program runs (not at an idle DOS prompt, not stopped) cannot
 * be shut down automatically (Windows 98 warns; the user decides). */
static int dos_program_active(uint32_t id, uint32_t generation)
{
    if (!id) return 0;
    struct ciuki_dosvm_command c;
    command(&c, id, generation);
    if (ciuki_dosvm(CIUKI_DOSVM_STATUS, &c)) return 0;   /* gone: nothing to lose */
    return c.state != CIUKI_DOSVM_STOPPED && !(c.changed & CIUKI_DOSVM_SHELL_READY);
}
static uint64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
/* Each line waits for the actual empty shell input loop, rather than fixed
 * 400/800 ms sleeps. Keep the existing bounded key-event pace. */
#define AUTORUN_EVENTS_PER_TICK 1u
static struct dosbox_key autorun[DOSBOX_KEYS_MAX];
static unsigned autorun_count, autorun_next;
static uint64_t autorun_at;   /* 0: not yet armed (VM still booting) */
static int autorun_launch_armed, autorun_launch_sent;
static int autorun_wait_prompt=1;
/* Types the next events; 0, or -1 on a fatal VM/input error. */
static int autorun_step(uint32_t id, uint32_t generation, int focused, int prompt_ready)
{
    uint64_t now = now_ms();
    if (autorun_next >= autorun_count || !focused || now < autorun_at) return 0;
    if (autorun_wait_prompt) {
        if (!prompt_ready) return 0;
        autorun_wait_prompt=0;
    }
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
        if (k->line_end) { autorun_at = now + 1; autorun_wait_prompt=1; return 0; }
    }
    return 0;
}
/* fx-4b: desktop messages before the VM exists (the kernel is checking the
 * DOS system files). 1: closed by the user or the desktop; -1: protocol
 * error; 0: continue. Focus is kept and forwarded once the VM exists. */
static int pre_create_service(int channel, uint32_t *server, int *configured, int *focused)
{
    for (unsigned budget = 0; budget < 32; budget++) {
        struct ciuki_message m;
        int n = dos_clip_recv(&m);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        if (n <= 0) return 1;                                     /* desktop gone */
        struct desk_packet packet;
        int valid = m.sender_pid && (!*server || *server == m.sender_pid) &&
                    m.data[0] == DESK_SHELL_VERSION && desk_parse(&m, 0, NULL, &packet);
        for (unsigned i = 0; i < m.fd_count && i < CIUKI_MESSAGE_FD_MAX; i++) close(m.fds[i]);
        if (!valid) return -1;
        *server = m.sender_pid;
        if (packet.opcode == DESK_CLOSED || packet.opcode == DESK_CLOSE_REQUEST) return 1;
        if (packet.opcode == DESK_END_SESSION && packet.focused) return 1;
        if (packet.opcode == DESK_QUERY_END_SESSION) {          /* no VM yet: nothing to lose */
            if (session_answer(channel, packet.serial, DESK_SESSION_AGREE)) return -1;
            continue;
        }
        if (packet.opcode == DESK_CONFIGURE) {
            if (packet.width != CIUKI_DOSVM_WIDTH || packet.height != CIUKI_DOSVM_HEIGHT) return -1;
            *configured = 1;
        } else if (packet.opcode == DESK_FOCUS) *focused = packet.focused ? 1 : 0;
        else if (packet.opcode == DESK_PING) {
            desk_message(&m, DESK_PONG, 8); desk_put32(m.data + 4, packet.serial);
            if (send_packet(channel, &m)) return -1;
        }
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
    const char *kernel_override=NULL;
    char typed[DOSBOX_COMMAND_MAX + 1] = "";
    size_t typed_length = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= CIUKI_OPEN_MAX) return 2;
            channel = (int)n;
        } else if (!strncmp(argv[i], "--name=", 7)) name = argv[i] + 7;
        else if (!strncmp(argv[i],"--dos-kernel=",13)) kernel_override=argv[i]+13;
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
    unsigned game=dosbox_game(typed);
    unsigned kernel=CIUKI_DOSVM_UNIFIED;
    unsigned memory=game ? dosbox_memory_load(game) : 0;
    unsigned network=game ? dosbox_network_load(game) : CIUKI_DOSVM_NETWORK_OFF;
    /* Runtime selector retained for existing test callers; it never writes. */
    if (kernel_override && strcmp(kernel_override,"unified")) return 2;
    size_t len = strlen(name);
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) return 1;
    int surface = ciuki_surface_create(CIUKI_DOSVM_WIDTH, CIUKI_DOSVM_HEIGHT, CIUKI_SURFACE_XRGB8888);
    if (surface < 0) { perror("dosbox: surface"); return 1; }
    struct ciuki_dosvm_command c;
    uint32_t id = 0, generation = 0, server = 0;
    uint32_t *pixels = ciuki_surface_map(surface, PROT_READ | PROT_WRITE);
    if (pixels == (void *)-1 || !pixels) { close(surface); return 1; }
    dos_clip_init(channel,pixels);
    dos_clip_settings(typed);
    struct du_canvas canvas = {pixels, CIUKI_DOSVM_WIDTH, CIUKI_DOSVM_HEIGHT,
                              {0, 0, CIUKI_DOSVM_WIDTH, CIUKI_DOSVM_HEIGHT}};
    const char *icon = dosbox_launch_icon(typed);
    uint64_t started = now_ms(), panel_at = 0, launch_at = 0;
    int loading = !!typed_length, shown = !loading;
    int failure_painted = 0;
    int panel_sent = 0, panel_redraw = 0, notice_painted = 0, redraw = 1, save_full_seen=0;
    unsigned precise_rows = 1;
    const char *failure = NULL;
    char detail[80] = "Preparing...";
    char fullscreen_notice[128]="";
    const char *fullscreen_title="Full screen unavailable";
    uint64_t fullscreen_notice_until=0;
    uint32_t fullscreen_error_seen=0;
    if (loading) {
        dosbox_panel(&canvas, name, icon, NULL, detail, 0);
    }
    if (game) fprintf(stderr,"dosbox: DOS kernel: %s; memory: %s\n",
                      dosbox_kernel_label(kernel),dosbox_memory_label(memory));
    struct ciuki_message m;
    desk_message(&m, DESK_HELLO, 4);
    if (send_packet(channel, &m)) goto done;
    desk_message(&m, DESK_CREATE_WINDOW, 16 + (unsigned)len);
    desk_put32(m.data + 4, CIUKI_DOSVM_WIDTH); desk_put32(m.data + 8, CIUKI_DOSVM_HEIGHT);
    desk_put32(m.data + 12, (uint32_t)len); memcpy(m.data + 16, name, len);
    m.fd_count = 1; m.fds[0] = surface;
    if (send_packet(channel, &m)) goto done;
    if (session_answer(channel, 0, DESK_SESSION_AGREE)) goto done;   /* G3 registration */
    int configured = 0, focused = 0;
    /* fx-4b: after the desktop starts, the kernel checks the DOS system files
     * in the background. While it runs, show its progress and keep answering
     * the desktop instead of blocking inside CREATE (the window would look
     * "Not responding"); a close request ends the launch here. */
    for (unsigned checks = 0;; checks++) {
        command(&c, 0, 0);
        if (ciuki_dosvm(CIUKI_DOSVM_IMAGE_QUERY, &c) || c.state != CIUKI_DOSVM_IMAGE_VERIFYING) {
            if (checks && loading) dosbox_panel(&canvas, name, icon, NULL, "Preparing...", 0);
            break;
        }
        int service = pre_create_service(channel, &server, &configured, &focused);
        if (service) { result = service > 0 ? 0 : 1; goto done; }
        snprintf(detail, sizeof(detail), "Checking the DOS system files: %u%%", (unsigned)(c.changed > 1000u ? 100u : c.changed / 10u));
        dosbox_panel_progress(&canvas, name, icon, detail, c.changed);
        if (configured) {
            desk_message(&m, DESK_DAMAGE, 20);
            desk_put32(m.data + 12, CIUKI_DOSVM_WIDTH); desk_put32(m.data + 16, CIUKI_DOSVM_HEIGHT);
            if (send_packet(channel, &m)) goto done;
        }
        struct pollfd wait = {channel, POLLIN, 0};
        if (poll(&wait, 1, 200) < 0 && errno != EINTR) goto done;
    }
    snprintf(detail, sizeof(detail), "Preparing...");
    command(&c, 0, 0); c.surface_fd = surface; memcpy(c.name, name, len);
    c.launch.kernel=kernel; c.launch.game=game; c.launch.memory=memory;
    dosvm_launch_network_set(&c,network);
    if (ciuki_dosvm(CIUKI_DOSVM_CREATE, &c)) {
        int error = errno;
        struct ciuki_dosvm_command image;
        command(&image, 0, 0);
        failure = "Unable to start";
        snprintf(detail, sizeof(detail), "%s", error==EBUSY ? "A COM or LPT port is in use. Check Game properties." : strerror(error));
        if (error == EIO && !ciuki_dosvm(CIUKI_DOSVM_IMAGE_QUERY, &image) && image.state == CIUKI_DOSVM_IMAGE_MISMATCH)
            snprintf(detail, sizeof(detail), "The DOS system files are damaged (checksum mismatch).");
        loading = 1; result = 1;
    } else {
        id = c.id; generation = c.generation;
        if (focused) {                       /* focus that arrived during the check */
            struct ciuki_dosvm_command f;
            command(&f, id, generation); f.focused = 1;
            if (ciuki_dosvm(CIUKI_DOSVM_FOCUS, &f)) focused = 0;
        }
        if (c.changed&CIUKI_DOSVM_JOYSTICK_FALLBACK) {
            fullscreen_title="Game controller";
            snprintf(fullscreen_notice,sizeof(fullscreen_notice),"Selected controller is absent. Using the first available joystick, or none if disconnected.");
            fullscreen_notice_until=now_ms()+5000;
            fprintf(stderr,"dosbox: %s\n",fullscreen_notice);
        }
        if (c.changed&(CIUKI_DOSVM_PORT_FALLBACK|CIUKI_DOSVM_PORT_SETTINGS_INVALID)) {
            fullscreen_title="Game peripherals";
            snprintf(fullscreen_notice,sizeof(fullscreen_notice),"%s%s",
                c.changed&CIUKI_DOSVM_PORT_FALLBACK ?
                "Unavailable ports: COM uses null; LPT is off. Check Game properties." :
                "Invalid port settings: using previous settings or defaults. Check Game properties.",
                c.changed&CIUKI_DOSVM_JOYSTICK_FALLBACK ? " Joystick uses first available." : "");
            fullscreen_notice_until=now_ms()+10000;
            fprintf(stderr,"dosbox: %s\n",fullscreen_notice);
        }
    }
    if (id && pause_background) {
        command(&c,id,generation); c.focused=1;
        if (ciuki_dosvm(CIUKI_DOSVM_BACKGROUND,&c)) goto done;
    }
    int running = 1, fullscreen = 0, fullscreen_painted = 0;
    while (running) {
        dos_clip_vm(id,generation,server);
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
            if (packet.opcode == DESK_CLOSED) { running = 0; break; }
            if (packet.opcode == DESK_QUERY_END_SESSION) {
                /* G3: an idle prompt agrees; a running program gets the
                 * desktop's "cannot shut down this program automatically". */
                if (session_answer(channel, packet.serial, dos_program_active(id, generation) ?
                                   DESK_SESSION_DOS_ACTIVE : DESK_SESSION_AGREE)) goto done;
                continue;
            }
            if (packet.opcode == DESK_END_SESSION) {
                if (packet.focused) { running = 0; break; }  /* every program agreed: close (forced if still running) */
                continue;
            }
            if (packet.opcode == DESK_CLOSE_REQUEST) {
                command(&c,id,generation);
                if (id && ciuki_dosvm(CIUKI_DOSVM_CLOSE,&c)) {
                    if (errno!=EBUSY) goto done;
                    /* G3: the program is still running (it was asked to quit
                     * through Ctrl+C and may decline): the desktop shows the
                     * Windows 98 warning; Yes ends this process, No keeps the
                     * session. Input is still forwarded so the game can save. */
                    desk_message(&m,DESK_CLOSE_ACK,8); desk_put32(m.data+4,DESK_CLOSE_DOS_ACTIVE);
                    if (send_packet(channel,&m)) goto done;
                    continue;
                }
                id=0; running=0; break;
            }
            if (packet.opcode==DESK_DOS_CLIPBOARD) {
                int error=dos_clip_action(packet.flags);
                if (error) { snprintf(fullscreen_notice,sizeof(fullscreen_notice),"Clipboard: %s",strerror(-error)); fullscreen_notice_until=now_ms()+3000; notice_painted=0; }
                continue;
            }
            if (dos_clip_input(&packet)) continue;
            if (packet.opcode == DESK_DOS_FULLSCREEN && id) {
                (void)dos_clip_action(0);
                command(&c,id,generation); c.focused=!fullscreen;
                if (ciuki_dosvm(CIUKI_DOSVM_FULLSCREEN,&c)) {
                    snprintf(fullscreen_notice,sizeof(fullscreen_notice),"%s",strerror(errno));
                    fullscreen_notice_until=now_ms()+3000;
                    notice_painted=0;
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
                if (op == CIUKI_DOSVM_FOCUS) { focused=packet.focused ? 1 : 0; c.focused=focused; }
                else c.event = packet.event;
                /* A full guest key queue drops the key, as a full BIOS buffer does. */
                if (ciuki_dosvm(op, &c) && errno != EAGAIN && errno != EACCES) {
                    if (errno == ESRCH) { running = 0; break; }
                    goto done;
                }
            }
        }
        if (!running) break;
        dos_clip_tick(focused);
        if (configured && dos_clip_dirty()) {
            desk_message(&m,DESK_DAMAGE,20); desk_put32(m.data+12,640); desk_put32(m.data+16,480);
            if (send_packet(channel,&m)) goto done;
            redraw=1;
        }
        if (failure) goto panel;
        /* Producer events and the VM's fractional VGA deadline determine
         * the frame rate. This timeout bounds input/lifecycle service only. */
        command(&c,id,generation); c.wait.timeout_ms=CIUKI_DOSVM_WAIT_MAX_MS;
        if (ciuki_dosvm(CIUKI_DOSVM_WAIT_PRESENT,&c)) {
            if (errno==EINTR) continue;
            if (errno==ESRCH) break;
            goto done;
        }
        unsigned video_events=c.wait.events;
        if (video_events&CIUKI_DOSVM_EVENT_CLIPBOARD) (void)dos_clip_service();
        int polled_launch = autorun_launch_armed;
        command(&c, id, generation);
        if (ciuki_dosvm(CIUKI_DOSVM_STATUS, &c)) goto done;
        fullscreen=!!(c.changed&CIUKI_DOSVM_FULL_SCREEN);
        if ((c.changed&CIUKI_DOSVM_SAVE_FULL) && !save_full_seen) {
            save_full_seen=1; fullscreen_title="Game data is full";
            snprintf(fullscreen_notice,sizeof(fullscreen_notice),"Game data is full (64 MiB limit or disk space). Further writes are disabled. Export or reset data in Properties.");
            fullscreen_notice_until=now_ms()+8000; notice_painted=0;
            if (fullscreen) {
                struct ciuki_dosvm_command leave;
                command(&leave,id,generation);
                if (!ciuki_dosvm(CIUKI_DOSVM_FULLSCREEN,&leave)) fullscreen=0;
            }
        }
        if (!(c.changed&CIUKI_DOSVM_FULLSCREEN_REFUSED)) fullscreen_error_seen=0;
        else if (c.reason!=fullscreen_error_seen) {
            fullscreen_error_seen=c.reason;
            /* Still painted as full screen: the kernel ended the session
             * itself (watchdog, display error); the game keeps running. */
            fullscreen_title=fullscreen_painted ? "Back in the window" : "Full screen unavailable";
            snprintf(fullscreen_notice,sizeof(fullscreen_notice),"%s",
                     fullscreen_painted ? dosbox_return_text((int)c.reason) : strerror((int)c.reason));
            fullscreen_notice_until=now_ms()+3000;
            notice_painted=0;
        }
        if (fullscreen) {
            if (!fullscreen_painted && configured) {
                dosbox_panel(&canvas,name,icon,NULL,"Running full screen — Alt+Enter to return",0);
                desk_message(&m,DESK_DAMAGE,20);
                desk_put32(m.data+12,CIUKI_DOSVM_WIDTH); desk_put32(m.data+16,CIUKI_DOSVM_HEIGHT);
                if (send_packet(channel,&m)) goto done;
                fullscreen_painted=1;
            }
            continue;
        }
        if (fullscreen_painted) {
            fullscreen_painted=0; redraw=1;
            if (loading) { panel_redraw=1; panel_sent=0; }
        }
        if (c.state == CIUKI_DOSVM_STOPPED) {
            if (shown && !dosbox_stop_reported(c.reason)) break;
            failure = shown ? "The DOS session stopped" : c.reason == 4 ? "Startup timed out" : "Unable to start";
            snprintf(detail, sizeof(detail), "%s", c.name[0] ? c.name : shown ?
                     "The program stopped with an error. Close this window." : "Close this window and try again.");
            result = 1;
            goto panel;
        }
        if (configured && now_ms()<fullscreen_notice_until) {
            if (!notice_painted) {
                dosbox_panel_framed(&canvas,name,name,icon,fullscreen_title,fullscreen_notice,0);
                desk_message(&m,DESK_DAMAGE,20);
                desk_put32(m.data+12,CIUKI_DOSVM_WIDTH); desk_put32(m.data+16,CIUKI_DOSVM_HEIGHT);
                if (send_packet(channel,&m)) goto done;
                notice_painted=1;
            }
            continue;
        }
        if (notice_painted) {
            notice_painted=0; redraw=1; panel_sent=0;
            if (loading) panel_redraw=1;
        }
        if (polled_launch && (c.changed & CIUKI_DOSVM_LAUNCH_FAILED)) {
            failure = "Program did not start";
            snprintf(detail, sizeof(detail), "%s", c.name[0] ? c.name : "Check the program name and its files.");
            result = 1;
            goto panel;
        }
        if (c.state == CIUKI_DOSVM_RUNNING && autorun_next < autorun_count) {
            if (!autorun_at) autorun_at = now_ms();
            if (autorun_step(id, generation, focused, !!(c.changed&CIUKI_DOSVM_SHELL_READY))) {
                failure = "Unable to start";
                snprintf(detail, sizeof(detail), "%s", errno==EBUSY ? "A COM or LPT port is in use. Check Game properties." : strerror(errno));
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
        if (configured && !dos_clip_marking() && (redraw || (video_events&CIUKI_DOSVM_EVENT_VIDEO))) {
            command(&c, id, generation);
            c.presentation.redraw=redraw;
            c.presentation.flags=precise_rows ? CIUKI_DOSVM_PRESENT_SCANLINES : 0;
            int error=ciuki_dosvm(CIUKI_DOSVM_PRESENT_BANDS, &c);
            if (error && errno==EINVAL && precise_rows) {
                /* Older kernels retain the original eight-line protocol. */
                precise_rows=0;command(&c,id,generation);c.presentation.redraw=redraw;
                error=ciuki_dosvm(CIUKI_DOSVM_PRESENT_BANDS,&c);
            }
            if (error) {
                if (errno == ESRCH) break;
                goto done;
            }
            if (c.changed) {
                uint32_t rows[15]={0};
                if (precise_rows) memcpy(rows,c.scanlines.bits,sizeof(rows));
                else {
                    uint64_t bands=c.presentation.bands[0]|((uint64_t)c.presentation.bands[1]<<32);
                    for (unsigned y=0;y<CIUKI_DOSVM_HEIGHT;y++)
                        if (bands&(1ull<<(y/8))) rows[y/32]|=1u<<(y%32);
                }
                for (unsigned y = 0; y < CIUKI_DOSVM_HEIGHT;) {
                    if (!(rows[y/32] & (1u<<(y%32)))) { y++; continue; }
                    unsigned first = y++;
                    while (y < CIUKI_DOSVM_HEIGHT && (rows[y/32] & (1u<<(y%32)))) y++;
                    desk_message(&m, DESK_DAMAGE, 20);
                    desk_put32(m.data + 4, 0); desk_put32(m.data + 8, first);
                    desk_put32(m.data + 12, CIUKI_DOSVM_WIDTH); desk_put32(m.data + 16, y - first);
                    if (send_packet(channel, &m)) goto done;
                }
            }
            redraw=0;
        }
        continue;
panel:
        if (configured && (failure ? !failure_painted : !panel_sent || now_ms() - panel_at >= DOSBOX_PULSE_MS)) {
            struct du_rect damage={0,0,CIUKI_DOSVM_WIDTH,CIUKI_DOSVM_HEIGHT};
            if (failure || panel_redraw) {
                if (failure && shown) dosbox_panel_framed(&canvas, name, name, icon, failure, detail, now_ms() - started);
                else dosbox_panel(&canvas, name, icon, failure, detail, now_ms() - started);
                panel_redraw=0;
            } else if (panel_sent) damage=dosbox_panel_pulse(&canvas,now_ms()-started);
            desk_message(&m, DESK_DAMAGE, 20);
            desk_put32(m.data + 4, damage.x); desk_put32(m.data + 8, damage.y);
            desk_put32(m.data + 12, damage.w); desk_put32(m.data + 16, damage.h);
            if (send_packet(channel, &m)) goto done;
            panel_at = now_ms();
            panel_sent=1;
            if (failure) failure_painted = 1;
        }
        if (failure) {
            struct pollfd fd={channel,POLLIN,0};
            int r=poll(&fd,1,-1);
            if ((r<0 && errno!=EINTR) || (fd.revents&POLLNVAL)) goto done;
        }
    }
    if (!failure) result = 0;
done:
    dos_clip_destroy();
    command(&c, id, generation);
    if (id && ciuki_dosvm(CIUKI_DOSVM_CLOSE, &c)) result = 1;
    desk_message(&m, DESK_CLOSE, 4); send_packet(channel, &m);
    close(surface); close(channel);
    return result;
}
