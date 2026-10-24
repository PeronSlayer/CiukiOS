/* SPDX-License-Identifier: MIT
 * fx-26: event -> frame latency of the volume popup and the volume
 * notification, through the production desktop.c service loop functions
 * (mixer_poll, the poll() watch set, watch_result, shell_service,
 * desktop_pause_ns, desk_frame_due, desk_redraw) in virtual time.
 *
 * The kernel changes the master at a timestamp (a key mirrored by the EC
 * worker, another program's change). The model sleeps exactly as the loop
 * does: until the earliest deadline desktop_pause_ns returns (and the shell's
 * one-second boundary), or until the change when the loop watches the mixer
 * description (POLLIN while that description has not read the new master).
 *
 * Before fx-26 (mixer_events=0, the 250 ms observation, which is also the
 * fallback when the kernel answers POLLNVAL) a change waited up to 250 ms
 * plus a frame. fx-26: the popup value and the notification are on the
 * first frame after the change. Included by shell_launch_test.c
 * (DESK_FOOTPRINT_TEST build), run with --event-latency. */
static uint64_t lat_now;
static uint64_t lat_clock(void) { return lat_now; }
struct lat_result { uint64_t latency, turns, queries; int shown; };
static void lat_setup(struct desktop *d, int events)
{
    fake_reset(); desk_init(d,&fake_ops,1024,768); desk_shell_enable(d);
    d->time_ns=lat_clock; d->sh.gate=0; d->mixer_fd=MIXER_FD; d->realtime_second=36000;
    d->pixels=calloc((size_t)d->width*d->height,4); assert(d->pixels);
    mixer_available=1; mixer_error=0;
    mixer_fixture=(struct ciuki_mixer_master){.version=CIUKI_AUDIO_VERSION,.size=sizeof(mixer_fixture),
        .left=50,.right=50,.save_state=CIUKI_MIXER_SAVED};
    mixer_fixture_serial++;
    mixer_events=events; ec_candidate=last_ec_preference=-1;
    lat_now=1000000000ull; d->now_ns=lat_now;
    mixer_read(d); d->sh.want_reload=0;
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    assert(!desk_redraw(d));
    d->sh.volume_open=1; desk_shell_volume_damage(d); assert(!desk_redraw(d)); /* the tray popup is open */
}
static void lat_free(struct desktop *d)
{
    desk_render_release(d); free(d->pixels); d->pixels=NULL; free(d->sh.icons); d->sh.icons=NULL;
    free(d->sh.wall); d->sh.wall=NULL; mixer_available=0; mixer_events=-1;
}
/* One run: the master becomes 75 % at change_at; returns the time from the
 * change to the first presented frame that shows it in the popup and in
 * the notification card. */
static struct lat_result lat_run(struct desktop *d, uint64_t change_at, uint64_t limit, uint64_t busy_ns)
{
    struct lat_result r={UINT64_MAX,0,0,0};
    uint64_t timer=0, busy_next=busy_ns ? lat_now : UINT64_MAX; int changed=0; unsigned queries=mixer_queries;
    for (;;) {
        d->now_ns=lat_now; r.turns++;
        if (lat_now>=busy_next) {
            /* Something else keeps the frames running (a progress bar, a
             * meter): small damage every busy_ns. */
            desk_damage_add(&d->damage,(struct desk_box){40,300,200,12},d->width,d->height);
            busy_next+=busy_ns;
        }
        mixer_poll(d,lat_now,&timer);
        shell_service(d);
        if (desk_frame_due(d,lat_now)) {
            fake_presents=0; assert(!desk_redraw(d));
            if (changed && d->sh.volume==75 && d->sh.volume_open && d->sh.notification.active &&
                strstr(d->sh.notification.text,"Volume: 75%")) {
                r.latency=lat_now-change_at; r.shown=1; break;
            }
        }
        if (lat_now>=limit) break;
        uint64_t t=d->shell && !d->power.state ? timer : UINT64_MAX;
        uint64_t wait=desktop_pause_ns(d,lat_now,0,t);
        uint64_t second=1000000000ull-lat_now%1000000000ull;
        if (second<wait) wait=second;
        if (busy_next>lat_now && busy_next-lat_now<wait) wait=busy_next-lat_now;
        /* poll(): du_wait_ms rounds the wait up to whole milliseconds. */
        uint64_t ms=wait/1000000ull+(wait%1000000ull!=0); if (!ms) ms=0;
        uint64_t next=lat_now+ms*1000000ull;
        int watch=mixer_watch_fd(d);
        if (!changed && change_at<next) {
            /* The kernel changes the master while the desktop sleeps. */
            mixer_fixture.left=mixer_fixture.right=75; mixer_fixture.save_state=CIUKI_MIXER_UNSAVED;
            mixer_fixture_serial++; changed=1;
            if (watch>=0) next=change_at; /* poll() wakes on the readiness */
        }
        lat_now=next<=lat_now ? lat_now+1000000ull : next;
        if (watch>=0) watch_result(d,mixer_observed_serial!=mixer_fixture_serial ? POLLIN : 0,-1);
    }
    r.queries=mixer_queries-queries;
    return r;
}
static void test_event_latency(void)
{
    static struct desktop d;
    uint64_t worst[2]={0,0}, best[2]={UINT64_MAX,UINT64_MAX}, busy_worst[2]={0,0};
    for (int events=0;events<2;events++) {
        for (unsigned phase=0;phase<250;phase+=7) for (unsigned busy=0;busy<2;busy++) {
            lat_setup(&d,events);
            uint64_t change_at=lat_now+2000000000ull+(uint64_t)phase*1000000ull+123456ull;
            struct lat_result r=lat_run(&d,change_at,change_at+3000000000ull,busy ? 20000000ull : 0);
            assert(r.shown);
            uint64_t *w=busy ? &busy_worst[events] : &worst[events];
            if (r.latency>*w) *w=r.latency;
            if (!busy && r.latency<best[events]) best[events]=r.latency;
            lat_free(&d);
        }
    }
    /* Before (the 250 ms observation): several frames late at most phases. */
    assert(worst[0]>200000000ull);
    /* fx-26: the next frame after the change. With no input for 250 ms an
     * isolated small update may wait for the 30 fps small-damage spacing
     * from the previous frame (DESK_SMALL_FRAME_NS), never more. */
    assert(worst[1]<=DESK_SMALL_FRAME_NS+1000000ull && busy_worst[1]<=DESK_SMALL_FRAME_NS+1000000ull);
    assert(busy_worst[0]>200000000ull);
    /* Idle: no mixer queries without a change in events mode. */
    lat_setup(&d,1); struct lat_result idle=lat_run(&d,UINT64_MAX-1,lat_now+10000000000ull,0);
    unsigned events_queries=(unsigned)idle.queries; uint64_t events_turns=idle.turns; lat_free(&d);
    lat_setup(&d,0); idle=lat_run(&d,UINT64_MAX-1,lat_now+10000000000ull,0);
    unsigned poll_queries=(unsigned)idle.queries; uint64_t poll_turns=idle.turns; lat_free(&d);
    assert(!events_queries && poll_queries>=39 && events_turns<poll_turns);
    /* Fallback: a kernel without mixer readiness (POLLNVAL) keeps the 250 ms observation. */
    lat_setup(&d,-1); watch_result(&d,POLLNVAL,-1); assert(mixer_events==0 && mixer_watch_fd(&d)<0); lat_free(&d);
    lat_setup(&d,-1); watch_result(&d,0,-1); assert(mixer_events==1 && mixer_watch_fd(&d)==MIXER_FD); lat_free(&d);
    /* With events the one remaining deadline is the quiet-period save of a
     * key-applied master: one read at MASTER_QUIET_NS, retried every 250 ms
     * while unsaved, none once saved. */
    lat_setup(&d,1);
    uint64_t timer=UINT64_MAX; d.sh.want_reload=0;
    ec_candidate=ciuki_master_pack(75,75,0); ec_candidate_ns=lat_now;
    mixer_poll(&d,lat_now,&timer); assert(timer==lat_now+MASTER_QUIET_NS && !(d.sh.want_reload&4));
    mixer_poll(&d,timer-1,&timer); assert(!(d.sh.want_reload&4));
    uint64_t due=timer; mixer_poll(&d,due,&timer); assert((d.sh.want_reload&4) && timer==due+250000000ull);
    d.sh.want_reload=0; last_ec_preference=ec_candidate; mixer_poll(&d,timer,&timer);
    assert(timer==UINT64_MAX && !d.sh.want_reload);
    ec_candidate=last_ec_preference=-1;
    printf("fx-26 volume popup and notification: change -> frame before (250 ms observation) %llu..%llu ms, "
           "fx-26 (mixer poll readiness) %llu..%llu us; with other updates every 20 ms before up to %llu ms, now up to %llu us; idle 10 s: %u mixer queries and %llu loop turns before, %u and %llu now PASS\n",
           (unsigned long long)(best[0]/1000000ull),(unsigned long long)(worst[0]/1000000ull),
           (unsigned long long)(best[1]/1000ull),(unsigned long long)(worst[1]/1000ull),
           (unsigned long long)(busy_worst[0]/1000000ull),(unsigned long long)(busy_worst[1]/1000ull),
           poll_queries,(unsigned long long)poll_turns,events_queries,(unsigned long long)events_turns);
    lat_free(&d);
}
