/* Full DOS game launch, kernel side: the production client boundary
 * (dosvm_client.c), lifecycle, BIOS, keyboard, launch gate, presenter and
 * destroy of tests/host/dosvm_test.c, driven by the production
 * /Programs/dosbox in game_launch_app.c. The guest is a scripted DOS: it
 * reports DOSREADY, idles at its prompt, reads the typed keys through the
 * BIOS (INT 9 and INT 16h), EXECs the typed program, sets mode 13h and
 * draws. No guest instruction stream, desktop, QEMU or image.
 * Modes (argv[1]):
 *   frame  Programs > Games > DOOM: the first game frame reaches the window;
 *   exit   the game quits to DOS: "Close on exit" closes the window and the
 *          VM is destroyed (Windows 98 PIF "Close on exit");
 *   close  a plain DOS Box at its idle C:\> prompt is closed from the desktop
 *          (close button, Alt+F4): CLOSE ends the VM and the window closes.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main dosvm_test_suite_main
#include "../dosvm_test.c"
#undef main
#include <pthread.h>

/* One kernel CPU: a system call, the scripted guest and a sleep never overlap. */
static pthread_mutex_t game_cpu = PTHREAD_MUTEX_INITIALIZER;
extern uint32_t game_guest_screen[CIUKI_DOSVM_WIDTH * CIUKI_DOSVM_HEIGHT];
extern unsigned game_desktop_frames(void);
static struct dosvm *game_vm;
static enum { GUEST_NONE, GUEST_BOOT, GUEST_PROMPT, GUEST_PROGRAM, GUEST_DRAWING } game_state;
static uint64_t game_state_at;
static char game_line[128];
static unsigned game_line_length, game_lines, game_execs, game_draws, game_calls[32];
static char game_program[128];
static bool game_stepping;
int game_mode;                      /* 0 frame, 1 exit, 2 close */
static uint64_t game_returned_at;

uint64_t game_now_ms(void) { return g_ticks; }
/* The DOS command interpreter waits for input: the VM task sleeps in
 * idle_wait (INT 28h), the state SHELL_READY reports. */
static void game_prompt_idle(struct dosvm *v)
{
    v->idle.sleeping = true; v->idle.seen = true; v->idle.last_tick = g_ticks;
    v->idle.reasons |= 1u << DOSVM_IDLE_28;
}
static void game_execute(struct dosvm *v)
{
    game_line[game_line_length] = 0; game_lines++;
    printf("guest: C:\\>%s\n", game_line);
    if (!strncmp(game_line, "CD ", 3)) { game_line_length = 0; return; }
    /* EXEC through the guest's INT 21h, as the shell issues it: the
     * production interrupt service records the launch frame. */
    struct v86_frame exec = vf(0x4B00, 0, 0, 0);
    exec.tf.cs = 0x3000; exec.tf.eip = 20; exec.tf.user_ss = 0x2000; exec.tf.user_esp = 0x100;
    CHECK(interrupt_service(v, &exec, 0x21) == -ENOSYS);
    snprintf(game_program, sizeof(game_program), "%s", game_line);
    game_execs++; game_line_length = 0;
    game_state = GUEST_PROGRAM; game_state_at = g_ticks;
}
/* Advance the scripted guest to the current time (kernel CPU held). */
static void game_guest_step(void)
{
    struct dosvm *v = game_vm;
    if (game_stepping || !v || !dosvm_valid(dosvm_key(v)) || v->info.state >= DOSVM_STOPPING) return;
    game_stepping = true;
    switch (game_state) {
    case GUEST_NONE: break;
    case GUEST_BOOT:
        if (g_ticks - game_state_at >= 1500) {
            line(v, "DOSREADY 07.0A free=7BD2 C");
            CHECK(v->info.dos_ready);
            game_prompt_idle(v); game_state = GUEST_PROMPT; game_state_at = g_ticks;
        }
        break;
    case GUEST_PROMPT:
        if (!v->kbd.count && !v->key_count) { if (!v->idle.sleeping) game_prompt_idle(v); break; }
        v->idle.sleeping = false; v->idle.last_tick = g_ticks;   /* woken by IRQ 1 */
        for (uint16_t key; game_state == GUEST_PROMPT && (key = take_key(v));) {
            uint8_t ch = (uint8_t)key;
            if (ch == '\r') game_execute(v);
            else if (ch >= 32 && ch < 127 && game_line_length + 1 < sizeof(game_line)) game_line[game_line_length++] = (char)ch;
        }
        if (game_state == GUEST_PROMPT && !v->kbd.count && !v->key_count) game_prompt_idle(v);
        break;
    case GUEST_PROGRAM:
        /* The game initialises (DOS/4GW, WAD), then sets mode 13h. */
        if (g_ticks - game_state_at >= 300) {
            int10(v, 0x13, 0, 0, 0);
            game_state = GUEST_DRAWING; game_state_at = g_ticks;
        }
        break;
    case GUEST_DRAWING:
        if (game_mode == 1 && game_draws >= 5 && g_ticks - game_state_at >= 28) {
            /* The player quits: text mode, DOS terminates the program and the
             * kernel's IRET reaches the shell's EXEC frame; dosvm.c's trap
             * exit then retires that frame and marks the armed launch
             * returned (the same three fields, as dosvm_properties_test.inc
             * sets them). The shell prints its prompt and waits for keys. */
            int10(v, 0x03, 0, 0, 0);
            CHECK(v->launch_exec && v->exec_pending == 1 && !v->launch_returned);
            v->exec_pending = 0; v->launch_returned = true; v->launch_returned_tick = g_ticks;
            dosvm_present_notify(v);
            game_returned_at = g_ticks; printf("guest: program returned to C:\\> at t=%llu ms\n", (unsigned long long)g_ticks);
            game_prompt_idle(v); game_state = GUEST_PROMPT; game_state_at = g_ticks;
            break;
        }
        if (g_ticks - game_state_at >= 28 && v->video_direct == VIDEO_LINEAR) {
            /* One frame: a band of colour 0x2A at the top of the screen. */
            for (unsigned x = 0; x < 320; x += 4) linear_store(v, 10 * 320 + x, 0x2A);
            game_draws++; game_state_at = g_ticks;
        }
        break;
    }
    game_stepping = false;
}
static void game_sleep_hook(void) { game_guest_step(); }
/* The desktop's close request in mode "close": the plain DOS Box has sat at
 * its idle prompt for a second. */
int game_prompt_ready(void)
{
    return game_vm && game_state == GUEST_PROMPT && !game_lines && g_ticks - game_state_at >= 1000;
}

int game_dosvm(uint32_t op, struct ciuki_dosvm_command *c)
{
    pthread_mutex_lock(&game_cpu);
    if (op < 32) game_calls[op]++;
    game_guest_step();
    /* The process's descriptor 7 is the guest surface (fake_desc[0]). */
    if (op == CIUKI_DOSVM_CREATE) { CHECK(c->surface_fd == 7); c->surface_fd = 0; }
    int e = dosvm_call(op, c, sizeof(*c));
    if (op == CIUKI_DOSVM_CREATE) {
        c->surface_fd = 7;
        if (!e) {
            game_vm = dosvm_lookup((struct dosvm_key){c->id, c->generation});
            /* DOOM is game 2 of the catalogue; a plain DOS Box names none. */
            CHECK(game_vm && game_vm->game == (game_mode == 2 ? 0u : 2u));
            game_state = GUEST_BOOT; game_state_at = g_ticks;
            printf("kernel: CREATE game=%u id=%u state=%u\n", game_vm->game, c->id, c->state);
        } else printf("kernel: CREATE error=%d\n", e);
    }
    if (op == CIUKI_DOSVM_INPUT || op == CIUKI_DOSVM_LAUNCH) game_guest_step();
    pthread_mutex_unlock(&game_cpu);
    return e;     /* the application side turns it into errno */
}
/* The window thread's poll(): kernel time passes; the guest runs. */
void game_sleep(unsigned ms)
{
    pthread_mutex_lock(&game_cpu);
    for (unsigned i = 0; i < ms; i++) { g_ticks++; host_clock_tick(); game_guest_step(); }
    pthread_mutex_unlock(&game_cpu);
}

int game_dosbox_main(int, char **);
void game_debug_surfaces(void);
uint64_t game_window_closed(void), game_close_requested(void);
int main(int argc, char **argv)
{
    CHECK(argc <= 2);
    const char *mode = argc == 2 ? argv[1] : "frame";
    game_mode = !strcmp(mode, "exit") ? 1 : !strcmp(mode, "close") ? 2 : 0;
    CHECK(game_mode || !strcmp(mode, "frame"));
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    g_ticks = 1000;
    /* The desktop-launched /Programs/dosbox process and its guest surface. */
    memset(fake_proc, 0, sizeof(fake_proc));
    fake_proc[0].pid = 10; fake_proc[0].state = PROC_LIVE; fake_proc[0].dosvm_capability = true;
    fake_current = &fake_proc[0];
    fake_surface[0] = (struct surface){{sizeof(struct ciuki_surface_info), CIUKI_DOSVM_WIDTH, CIUKI_DOSVM_HEIGHT,
        CIUKI_DOSVM_WIDTH * 4, CIUKI_SURFACE_XRGB8888, CIUKI_DOSVM_WIDTH * CIUKI_DOSVM_HEIGHT * 4},
        (uint8_t *)game_guest_screen, 1};
    fake_desc[0] = (struct desktop_description){.kind = DESKTOP_SURFACE, .maximum = PROT_READ | PROT_WRITE,
                                                .u.surface = &fake_surface[0]};
    desc_owner[0] = &fake_proc[0];
    bundled_bytes = 4u * 1024u * 1024u;    /* a short bundled base: CREATE hashes it */
    image_state = CIUKI_DOSVM_IMAGE_VERIFIED; image_progress = 1000; /* the background check is done */
    sleep_hook = game_sleep_hook;
    char *game_argv[] = {"dosbox", "--name=DOOM", "\\DOOM\\DOOM.EXE -config CIUKI.CFG", 0};
    char *plain_argv[] = {"dosbox", 0};
    int result = game_mode == 2 ? game_dosbox_main(1, plain_argv) : game_dosbox_main(3, game_argv);
    struct dosvm *v = game_vm;
    if (game_mode) {
        /* The DOS Box closed itself: CLOSE at the idle prompt is an ordinary
         * EXIT, the VM is destroyed (no retained VM, no forced termination)
         * and the window went away within a second of the trigger. */
        struct dosvm_key key = {0, 0};
        if (v) key = dosvm_key(v);
        uint64_t trigger = game_mode == 1 ? game_returned_at : game_close_requested();
        printf("kernel: dosbox exit=%d calls create=%u close=%u launch=%u status=%u; trigger t=%llu ms, window closed t=%llu ms\n",
               result, game_calls[CIUKI_DOSVM_CREATE], game_calls[CIUKI_DOSVM_CLOSE], game_calls[CIUKI_DOSVM_LAUNCH],
               game_calls[CIUKI_DOSVM_STATUS], (unsigned long long)trigger, (unsigned long long)game_window_closed());
        CHECK(v && !result && game_calls[CIUKI_DOSVM_CREATE] == 1 && game_calls[CIUKI_DOSVM_CLOSE] == 1);
        CHECK(game_mode == 1 ? game_execs == 1 && game_returned_at && game_draws >= 5 : !game_execs && !game_lines);
        CHECK(trigger && game_window_closed() >= trigger && game_window_closed() - trigger <= 1000);
        CHECK(!dosvm_valid(key) && v->info.state == DOSVM_DEAD && v->info.reason == DOSVM_EXIT && !v->info.data_loss);
        printf(game_mode == 1 ? "PASS DOS game exit: DOOM quit to C:\\>, Close on exit closed the window, VM destroyed as EXIT\n" :
                                "PASS DOS Box close: CLOSE at the idle C:\\> prompt destroyed the VM as EXIT and closed the window\n");
        fflush(stdout);
        return 0;
    }
    printf("kernel: dosbox exit=%d lines=%u execs=%u program=\"%s\" draws=%u desktop_frames=%u "
           "calls create=%u background=%u policy=%u focus=%u input=%u launch=%u status=%u wait=%u present=%u\n",
           result, game_lines, game_execs, game_program, game_draws, game_desktop_frames(),
           game_calls[CIUKI_DOSVM_CREATE], game_calls[CIUKI_DOSVM_BACKGROUND], game_calls[CIUKI_DOSVM_POLICY],
           game_calls[CIUKI_DOSVM_FOCUS], game_calls[CIUKI_DOSVM_INPUT], game_calls[CIUKI_DOSVM_LAUNCH],
           game_calls[CIUKI_DOSVM_STATUS], game_calls[CIUKI_DOSVM_WAIT_PRESENT], game_calls[CIUKI_DOSVM_PRESENT_BANDS]);
    game_debug_surfaces();
    printf("frame: video_direct=%u mode=%u launch_ready=%u video_presented=%u frames=%u\n", v ? v->video_direct : 99, v ? v->vbe_mode : 0, v ? v->launch_ready : 0, v ? v->video_presented : 0, v ? v->video_frames : 0);
    CHECK(v && game_execs == 1 && !strcmp(game_program, "DOOM.EXE -config CIUKI.CFG"));
    CHECK(v->launch_exec && v->launch_ready && !v->launch_failed);
    CHECK(game_draws && game_desktop_frames());
    /* The window closed while the game ran: CLOSE asked it to quit (Ctrl+C);
     * the test ends the session as the owner's exit does. */
    struct dosvm_key key = dosvm_key(v);
    if (dosvm_valid(key)) {
        CHECK(!dosvm_stop(key, DOSVM_EXIT));
        fake_proc[0].state = PROC_STOPPING; dosvm_client_poll();
    }
    printf("PASS DOS game launch: dosbox CREATE (game=2) on its worker, BACKGROUND/POLICY, typed \\DOOM\\DOOM.EXE, "
           "LAUNCH, EXEC, mode 13h, first frame in the window\n");
    fflush(stdout);
    return 0;
}
