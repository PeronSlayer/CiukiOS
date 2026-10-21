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
#include "settings.h"
#include "network_settings.h"
#include "network_panel.h"

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
    const char *kernel_override=NULL, *memory_override=NULL, *network_override=NULL;
    char typed[DOSBOX_COMMAND_MAX + 1] = "";
    size_t typed_length = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= CIUKI_OPEN_MAX) return 2;
            channel = (int)n;
        } else if (!strncmp(argv[i], "--name=", 7)) name = argv[i] + 7;
        else if (!strncmp(argv[i],"--dos-kernel=",13)) kernel_override=argv[i]+13;
        else if (!strncmp(argv[i],"--dos-memory=",13)) memory_override=argv[i]+13;
        else if (!strncmp(argv[i],"--dos-network=",14)) network_override=argv[i]+14;
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
    unsigned kernel=game ? dosbox_setting_load(game) : 0;
    unsigned memory=game ? dosbox_memory_load(game) : 0;
    unsigned network=game ? dosbox_network_load(game) : CIUKI_DOSVM_NETWORK_OFF;
    if (kernel_override) {
        if (!game || (strcmp(kernel_override,"ciukidos") && strcmp(kernel_override,"freedos-1.3") &&
                      strcmp(kernel_override,"unified"))) return 2;
        kernel=dosbox_kernel_value(kernel_override);
        if (dosbox_setting_save(game,kernel)) { perror("dosbox: DOS kernel setting"); return 1; }
    }
    if (memory_override) {
        if (!game || (strcmp(memory_override,"locked") && strcmp(memory_override,"paged"))) return 2;
        memory=dosbox_memory_value(memory_override);
        if (dosbox_memory_save(game,memory)) { perror("dosbox: DOS memory setting"); return 1; }
    }
    if (network_override) {
        if (!game || (strcmp(network_override,"off") && strcmp(network_override,"ipx"))) return 2;
        network=!strcmp(network_override,"ipx") ? CIUKI_DOSVM_NETWORK_IPX : CIUKI_DOSVM_NETWORK_OFF;
        if (dosbox_network_save(game,network)) { perror("dosbox: network setting"); return 1; }
    }
    int settings_open=0, settings_repaint=0, settings_dirty=0;
    uint32_t *settings_backup=NULL;
    char setting_detail[96]="Changes apply when you close and relaunch this game.";
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
    int panel_sent = 0, panel_redraw = 0, notice_painted = 0, redraw = 1;
    unsigned frame_ms = 15;
    const char *failure = NULL;
    char detail[80] = "Preparing...";
    char fullscreen_notice[80]="";
    uint64_t fullscreen_notice_until=0;
    uint32_t fullscreen_error_seen=0;
    if (loading) {
        dosbox_panel(&canvas, name, icon, NULL, detail, 0);
        if (game) du_text_wrap(&canvas,(struct du_rect){120,300,400,18},"F12: DOS kernel settings",DU_INK);
    }
    if (game) fprintf(stderr,"dosbox: DOS kernel: %s; memory: %s; F12 opens game settings\n",
                      dosbox_kernel_label(kernel),dosbox_memory_label(memory));
    struct ciuki_message m;
    desk_message(&m, DESK_HELLO, 4);
    if (send_packet(channel, &m)) goto done;
    desk_message(&m, DESK_CREATE_WINDOW, 16 + (unsigned)len);
    desk_put32(m.data + 4, CIUKI_DOSVM_WIDTH); desk_put32(m.data + 8, CIUKI_DOSVM_HEIGHT);
    desk_put32(m.data + 12, (uint32_t)len); memcpy(m.data + 16, name, len);
    m.fd_count = 1; m.fds[0] = surface;
    if (send_packet(channel, &m)) goto done;
    command(&c, 0, 0); c.surface_fd = surface; memcpy(c.name, name, len);
    c.launch.kernel=kernel; c.launch.game=game; c.launch.memory=memory;
    dosvm_launch_network_set(&c,network);
    if (ciuki_dosvm(CIUKI_DOSVM_CREATE, &c)) {
        failure = "Unable to start";
        snprintf(detail, sizeof(detail), "%s", kernel==CIUKI_DOSVM_UNIFIED ?
                 "Unified DOS could not be opened. Your game settings have been kept." :
                 errno==ENOENT && game ? "This bundled game has been removed. Reinstall CiukiOS to restore it." : kernel==CIUKI_DOSVM_FREEDOS13 ?
                 "FreeDOS 1.3 is unavailable. Install or repair FreeDOS support, then relaunch." : strerror(errno));
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
                if (game && packet.opcode==DESK_KEY && packet.event.code==CIUKI_KEY_F12) {
                    if (packet.event.value==1) {
                        if (settings_open) { dosbox_settings_restore(&canvas,&settings_backup); settings_repaint=1; }
                        else if (dosbox_settings_capture(&canvas,&settings_backup)) continue;
                        settings_open=!settings_open;
                        settings_dirty=settings_open;
                        if (id) {
                            command(&c,id,generation); c.focused=focused && !settings_open;
                            if (ciuki_dosvm(CIUKI_DOSVM_FOCUS,&c) && errno!=ESRCH) goto done;
                        }
                    }
                    continue;
                }
                if (settings_open && packet.opcode==DESK_KEY) {
                    unsigned previous_kernel=kernel, previous_memory=memory, previous_network=network;
                    char previous_detail[sizeof(setting_detail)];
                    memcpy(previous_detail,setting_detail,sizeof(previous_detail));
                    if (packet.event.value==1 && (packet.event.code==CIUKI_KEY_1 || packet.event.code==CIUKI_KEY_2 ||
                                                 packet.event.code==CIUKI_KEY_5)) {
                        unsigned choice=packet.event.code==CIUKI_KEY_5 ? CIUKI_DOSVM_UNIFIED :
                                        packet.event.code==CIUKI_KEY_2 ? CIUKI_DOSVM_FREEDOS13 : 0;
                        if (dosbox_setting_save(game,choice)) snprintf(setting_detail,sizeof(setting_detail),"Setting could not be saved: %s",strerror(errno));
                        else { kernel=choice; snprintf(setting_detail,sizeof(setting_detail),"Saved. Close and relaunch this game to apply."); }
                    }
                    if (packet.event.value==1 && (packet.event.code==CIUKI_KEY_3 || packet.event.code==CIUKI_KEY_4)) {
                        unsigned choice=packet.event.code==CIUKI_KEY_4 ? CIUKI_DOSVM_MEMORY_LOCKED : CIUKI_DOSVM_MEMORY_PAGED;
                        if (dosbox_memory_save(game,choice)) snprintf(setting_detail,sizeof(setting_detail),"Setting could not be saved: %s",strerror(errno));
                        else { memory=choice; snprintf(setting_detail,sizeof(setting_detail),"Saved. Close and relaunch this game to apply."); }
                    }
                    if (packet.event.value==1 && (packet.event.code==CIUKI_KEY_5 || packet.event.code==CIUKI_KEY_6)) {
                        unsigned choice=packet.event.code==CIUKI_KEY_6 ? CIUKI_DOSVM_NETWORK_IPX : CIUKI_DOSVM_NETWORK_OFF;
                        if (dosbox_network_save(game,choice)) snprintf(setting_detail,sizeof(setting_detail),"Setting could not be saved: %s",strerror(errno));
                        else { network=choice; snprintf(setting_detail,sizeof(setting_detail),"Saved. Close and relaunch this game to apply."); }
                    }
                    if (kernel!=previous_kernel || memory!=previous_memory || network!=previous_network || strcmp(setting_detail,previous_detail)) settings_dirty=1;
                    if (packet.event.code==CIUKI_KEY_ESCAPE && packet.event.value==1) {
                        settings_open=0;
                        dosbox_settings_restore(&canvas,&settings_backup); settings_repaint=1;
                        if (id) {
                            command(&c,id,generation); c.focused=focused;
                            if (ciuki_dosvm(CIUKI_DOSVM_FOCUS,&c) && errno!=ESRCH) goto done;
                        }
                    }
                    continue;
                }
                if (settings_open && packet.opcode!=DESK_FOCUS) continue;
                if (!id || failure) continue;
                command(&c, id, generation);
                unsigned op = packet.opcode == DESK_FOCUS ? CIUKI_DOSVM_FOCUS : CIUKI_DOSVM_INPUT;
                if (op == CIUKI_DOSVM_FOCUS) { focused=packet.focused ? 1 : 0; c.focused=focused && !settings_open; }
                else c.event = packet.event;
                /* A full guest key queue drops the key, as a full BIOS buffer does. */
                if (ciuki_dosvm(op, &c) && errno != EAGAIN && errno != EACCES) {
                    if (errno == ESRCH) { running = 0; break; }
                    goto done;
                }
            }
        }
        if (!running) break;
        if (settings_repaint && configured) {
            desk_message(&m,DESK_DAMAGE,20);
            desk_put32(m.data+4,DOSBOX_SETTINGS_X); desk_put32(m.data+8,DOSBOX_SETTINGS_Y);
            desk_put32(m.data+12,DOSBOX_SETTINGS_W); desk_put32(m.data+16,DOSBOX_SETTINGS_H);
            if (send_packet(channel,&m)) goto done;
            settings_repaint=0;
        }
        if (settings_open && configured) {
            if (settings_dirty) {
                char selection[128];
                snprintf(selection,sizeof(selection),"Kernel: %s. Memory: %s. Network: %s.",dosbox_kernel_label(kernel),dosbox_memory_label(memory),dosbox_network_label(network));
                dosbox_network_panel(&canvas,selection,setting_detail);
                desk_message(&m,DESK_DAMAGE,20);
                desk_put32(m.data+4,DOSBOX_SETTINGS_X); desk_put32(m.data+8,DOSBOX_SETTINGS_Y);
                desk_put32(m.data+12,DOSBOX_SETTINGS_W); desk_put32(m.data+16,DOSBOX_SETTINGS_H);
                if (send_packet(channel,&m)) goto done;
                settings_dirty=0;
            }
            struct timespec pause={.tv_nsec=50000000}; nanosleep(&pause,NULL);
            continue;
        }
        if (failure) goto panel;
        command(&c,id,generation); c.wait.timeout_ms=frame_ms;
        if (ciuki_dosvm(CIUKI_DOSVM_WAIT_PRESENT,&c)) {
            if (errno==EINTR) continue;
            if (errno==ESRCH) break;
            goto done;
        }
        frame_ms=c.wait.frame_ms;
        unsigned video_events=c.wait.events;
        int polled_launch = autorun_launch_armed;
        command(&c, id, generation);
        if (ciuki_dosvm(CIUKI_DOSVM_STATUS, &c)) goto done;
        fullscreen=!!(c.changed&CIUKI_DOSVM_FULL_SCREEN);
        if (!(c.changed&CIUKI_DOSVM_FULLSCREEN_REFUSED)) fullscreen_error_seen=0;
        else if (c.reason!=fullscreen_error_seen) {
            fullscreen_error_seen=c.reason;
            snprintf(fullscreen_notice,sizeof(fullscreen_notice),"%s",strerror((int)c.reason));
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
            if (shown) break;
            failure = c.reason == 4 ? "Startup timed out" : "Unable to start";
            snprintf(detail, sizeof(detail), "%s", c.name[0] ? c.name : "Close this window and try again.");
            result = 1;
            goto panel;
        }
        if (configured && now_ms()<fullscreen_notice_until) {
            if (!notice_painted) {
                dosbox_panel(&canvas,name,icon,"Full screen unavailable",fullscreen_notice,0);
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
        if (configured && (redraw || (video_events&CIUKI_DOSVM_EVENT_VIDEO))) {
            command(&c, id, generation);
            c.presentation.redraw=redraw;
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
            redraw=0;
        }
        continue;
panel:
        if (configured && (failure ? !failure_painted : !panel_sent || now_ms() - panel_at >= DOSBOX_PULSE_MS)) {
            struct du_rect damage={0,0,CIUKI_DOSVM_WIDTH,CIUKI_DOSVM_HEIGHT};
            if (failure || panel_redraw) {
                dosbox_panel(&canvas, name, icon, failure, detail, now_ms() - started);
                if (game) du_text_wrap(&canvas,(struct du_rect){120,300,400,18},"F12: DOS kernel settings",DU_INK);
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
        if (failure) { struct timespec pause = {.tv_nsec = 50000000}; nanosleep(&pause, NULL); }
    }
    if (!failure) result = 0;
done:
    free(settings_backup);
    command(&c, id, generation);
    if (id && ciuki_dosvm(CIUKI_DOSVM_CLOSE, &c)) result = 1;
    desk_message(&m, DESK_CLOSE, 4); send_packet(channel, &m);
    close(surface); close(channel);
    return result;
}
