/* SPDX-License-Identifier: MIT
 * Same pinned scene/assets and f2-48 exclusive painter instrumentation.
 * Timing pass and instrumentation pass are separate. The clock advances
 * through pacing waits without sleeping; painting/presentation use real time.
 */
#define main throughput_main
#include "compositor_benchmark.c"
#undef main

static struct desktop *replay;
static uint32_t *scanout;
static uint64_t virtual_ns, host_begin, upload_bytes, input_cycles;
static unsigned engine_caps, operation_count;
static int first_client=-1;
static const char *frame_action;
static uint64_t realtime_clock(void) { return virtual_ns+ns()-host_begin; }
static int replay_present(int display, int output, const struct ciuki_rect *r)
{
    (void)display; (void)output;
    for (unsigned y=0;y<r->height;y++) memcpy(scanout+(r->dst_y+y)*W+r->dst_x,
        replay->pixels+(r->src_y+y)*W+r->src_x,r->width*4);
    upload_bytes+=(uint64_t)r->width*r->height*4; operation_count++;
    return 0;
}
static int replay_accel(int display, const struct ciuki_accel_request *q)
{
    (void)display;
    if (q->operation==CIUKI_ACCEL_QUERY) return (int)engine_caps;
    if (q->operation==CIUKI_ACCEL_COPY) {
        for (unsigned n=0;n<q->rect.height;n++) {
            unsigned y=q->rect.dst_y>q->rect.src_y ? q->rect.height-1-n : n;
            memmove(scanout+(q->rect.dst_y+y)*W+q->rect.dst_x,
                    scanout+(q->rect.src_y+y)*W+q->rect.src_x,q->rect.width*4);
        }
        operation_count++;
    } else if (q->operation==CIUKI_ACCEL_FILL) {
        for (unsigned y=0;y<q->rect.height;y++) for (unsigned x=0;x<q->rect.width;x++)
            scanout[(q->rect.dst_y+y)*W+q->rect.dst_x+x]=q->colour;
        operation_count++;
    }
    return 0;
}
static int launch(struct desktop *d, const struct desk_program *program)
{
    (void)program;
    fake_channel(80); first_client=desk_add_client(d,80); assert(first_client>=0);
    struct desk_client *c=&d->clients[first_client];
    c->awaiting_frame=1; c->launched_ns=d->now_ns; c->pid=88;
    struct ciuki_message m; desk_message(&m,DESK_HELLO,4); m.sender_pid=88;
    desk_receive(d,first_client,&m);
    fake_surface(81,400,300,0x234567);
    desk_message(&m,DESK_CREATE_WINDOW,26); m.sender_pid=88; m.fd_count=1; m.fds[0]=81;
    desk_put32(m.data+4,400); desk_put32(m.data+8,300); desk_put32(m.data+12,10);
    memcpy(m.data+16,"Replay app",10); desk_receive(d,first_client,&m);
    assert(c->window && c->awaiting_frame); return 88;
}
static void first_frame(struct desktop *d)
{
    assert(first_client>=0);
    struct ciuki_message m; desk_message(&m,DESK_DAMAGE,20); m.sender_pid=88;
    desk_put32(m.data+12,400); desk_put32(m.data+16,300);
    desk_receive(d,first_client,&m);
    assert(d->clients[first_client].launch_frame_pending);
}
struct recorded_event { unsigned ms; char action[24]; int a,b; };
static void apply(struct desktop *d, const struct recorded_event *r)
{
    struct ciuki_input_event e={.monotonic_ns=1000000000ull+r->ms*1000000ull};
    d->now_ns=realtime_clock();
    uint64_t begin=ticks();
    if (!strcmp(r->action,"motion")) {
        e.type=CIUKI_INPUT_MOTION; e.value=r->a-d->mouse_x; e.value2=r->b-d->mouse_y;
        desk_input(d,&e); frame_action=d->drag>=0 ? "drag" : "pointer";
    } else if (!strcmp(r->action,"button")) {
        e.type=CIUKI_INPUT_BUTTON; e.code=r->a; e.value=r->b; desk_input(d,&e);
        if (d->menu_open) frame_action="start_open";
    } else if (!strcmp(r->action,"key")) {
        e.type=CIUKI_INPUT_KEY; e.code=r->a; e.value=r->b; desk_input(d,&e);
        frame_action="start_close";
    } else if (!strcmp(r->action,"launch")) {
        d->frame_input_ns=e.monotonic_ns;
        assert(desk_task_launch(d,"demo",NULL)==88); frame_action="launch";
    } else if (!strcmp(r->action,"first_frame")) {
        first_frame(d); frame_action="first_frame";
    } else {
        assert(!strcmp(r->action,"full")); desk_damage_add(&d->damage,screen,W,H); frame_action="full";
    }
    input_cycles+=ticks()-begin;
}
static void verify(struct desktop *d)
{
    /* Reference drawing and validation stay outside all timing windows. */
    uint32_t *expected=calloc(W*H,4); assert(expected);
    uint32_t *output=d->pixels; d->pixels=expected; desk_composite(d,screen); d->pixels=output;
    assert(!memcmp(output,expected,W*H*4));
    /* Hardware cursor is an overlay, absent from the RAM/LFB comparison. */
    assert(!memcmp(scanout,expected,W*H*4)); free(expected);
}
static void release(struct desktop *d)
{
    if (first_client>=0) desk_drop_client(d,first_client);
    desk_render_release(d);
    for (int i=0;i<4;i++) {
        const uint32_t *pixels=d->clients[i].pixels;
        d->clients[i].pixels=NULL; desk_drop_client(d,i); free((void *)pixels);
    }
    assert(!fake_live());
    free((void *)d->portrait); free(d->pixels-1); free(d->sh.wall); free(d->sh.icons); free(d);
    free(scanout); first_client=-1;
}
static void replay_trace(const char *portrait, const char *store, const char *theme,
                         const struct recorded_event *events, unsigned count, int instrument)
{
    struct desktop *d=calloc(1,sizeof(*d)); assert(d); replay=d;
    setup(d,portrait,store,theme); desk_menu_close(d);
    struct desk_client *c=&d->clients[3]; free((void *)c->pixels);
    uint32_t *client=malloc(400*300*4); assert(client);
    for (unsigned k=0;k<400*300;k++) client[k]=0xa5000000u|((k*7631u)&0xffffffu);
    c->pixels=client; c->info.width=400; c->info.height=300;
    c->frame=desk_frame(400,300,510,340);
    for (int i=0;i<4;i++) { fake_channel(10+i); d->clients[i].channel=10+i; }
    d->launch=launch;
    /* Program discovery is not timed; add the same launcher table entry
     * without ELF inspection, which belongs to the runtime inventory. */
    strcpy(d->programs[0].name,"demo"); strcpy(d->programs[0].path,"/Programs/demo"); d->program_count=1;
    d->ops.present=replay_present; d->time_ns=realtime_clock;
    d->accel=engine_caps ? replay_accel : NULL;
    virtual_ns=1000000000ull; host_begin=ns(); d->now_ns=virtual_ns;
    scanout=calloc(W*H,4); assert(scanout);
    if (engine_caps) desk_accel_reset(d,55);
    desk_damage_add(&d->damage,screen,W,H); assert(!desk_redraw(d)); verify(d); desk_stats_sample(d);
    uint64_t latencies[128], nominal[128]; unsigned frames=0, event=0;
    uint64_t nominal_by_action[3][128]; unsigned action_counts[3]={0};
    while (event<count || desk_frame_due(d,d->next_frame_ns)) {
        uint64_t next_event=event<count ? 1000000000ull+events[event].ms*1000000ull : UINT64_MAX;
        uint64_t deadline=d->next_frame_ns;
        if (!d->damage.count && !d->move_damage.count && !d->cursor_dirty && !d->present_damage.count) deadline=UINT64_MAX;
        if (next_event<=deadline) {
            if (virtual_ns<next_event) virtual_ns=next_event;
            host_begin=ns(); apply(d,&events[event++]); continue;
        }
        if (virtual_ns<deadline) virtual_ns=deadline;
        host_begin=ns(); d->now_ns=realtime_clock();
        uint64_t queued=d->frame_input_ns && d->frame_input_ns<=d->now_ns ? d->now_ns-d->frame_input_ns : 0;
        if (frame_action && !strcmp(frame_action,"first_frame")) queued=d->now_ns-d->clients[first_client].launched_ns;
        upload_bytes=operation_count=0;
#ifdef DESK_PROFILE
        memset(painter_cycles,0,sizeof(painter_cycles)); memset(painter_calls,0,sizeof(painter_calls));
        profiling=instrument;
#else
        (void)instrument;
#endif
        uint64_t begin=ticks(); assert(!desk_redraw(d)); uint64_t cycles=ticks()-begin;
#ifdef DESK_PROFILE
        profiling=0; assert(!profile_depth);
#endif
        assert(frames<128);
        latencies[frames]=d->frame_stats.frame_ns;
        /* Directional 1 GHz normalization plus the T23's old measured upload
         * bandwidth. Modern TSC ticks are not PIII retired CPU cycles. */
        nominal[frames]=queued/1000+(cycles+input_cycles)/1000+(upload_bytes+133)/134;
        if (!instrument) printf("FRAME theme=%s accel=%u n=%u action=%s latency_us=%llu nominal_1ghz_us=%llu "
            "render_cycles=%llu composite_us=%llu present_us=%llu scene_px=%llu upload_bytes=%llu ops=%u\n",
            theme,engine_caps,frames,frame_action ? frame_action : "other",
            (unsigned long long)(latencies[frames]/1000),(unsigned long long)nominal[frames],
            (unsigned long long)cycles,(unsigned long long)(d->frame_stats.composite_ns/1000),
            (unsigned long long)(d->frame_stats.present_ns/1000),(unsigned long long)d->frame_stats.scene_px,
            (unsigned long long)upload_bytes,operation_count);
#ifdef DESK_PROFILE
        else {
            printf("BREAKDOWN theme=%s accel=%u n=%u",theme,engine_caps,frames);
            for (unsigned k=0;k<9;k++) printf(" %s=%llu",painter_names[k],(unsigned long long)painter_cycles[k]);
            putchar('\n');
            assert(!painter_calls[8]);
        }
#endif
        int action=frame_action && !strcmp(frame_action,"drag") ? 0 : frame_action && !strcmp(frame_action,"start_open") ? 1 :
            frame_action && !strcmp(frame_action,"full") ? 2 : -1;
        if (action>=0) nominal_by_action[action][action_counts[action]++]=nominal[frames]-queued/1000;
        frames++; input_cycles=0; frame_action=NULL;
        virtual_ns=realtime_clock(); host_begin=ns(); verify(d);
    }
    if (!instrument) {
        qsort(latencies,frames,sizeof(*latencies),compare); qsort(nominal,frames,sizeof(*nominal),compare);
        unsigned p50=(frames+1)/2-1, p95=frames-frames/20-1;
        printf("LATENCY theme=%s accel=%u frames=%u p50_us=%llu p95_us=%llu max_us=%llu "
               "nominal_p50_us=%llu nominal_p95_us=%llu nominal_max_us=%llu record_p95_us=%llu record_max_us=%llu\n",
            theme,engine_caps,frames,(unsigned long long)(latencies[p50]/1000),
            (unsigned long long)(latencies[p95]/1000),(unsigned long long)(latencies[frames-1]/1000),
            (unsigned long long)nominal[p50],(unsigned long long)nominal[p95],(unsigned long long)nominal[frames-1],
            (unsigned long long)(d->stats.frame_p95_ns/1000),(unsigned long long)(d->stats.frame_max_ns/1000));
        const char *names[3]={"drag","start_open","full"};
        for (unsigned k=0;k<3;k++) {
            assert(action_counts[k]); qsort(nominal_by_action[k],action_counts[k],sizeof(uint64_t),compare);
            printf("OPERATION theme=%s accel=%u action=%s frames=%u nominal_max_render_us=%llu\n",theme,engine_caps,
                names[k],action_counts[k],(unsigned long long)nominal_by_action[k][action_counts[k]-1]);
        }
    }
    release(d);
}
int main(int argc, char **argv)
{
    assert(argc==5); setvbuf(stdout,NULL,_IOLBF,0);
    cpu_set_t allowed, selected;
    assert(!sched_getaffinity(0,sizeof(allowed),&allowed));
    int cpu=0;
    while (cpu<CPU_SETSIZE && !CPU_ISSET(cpu,&allowed)) cpu++;
    assert(cpu<CPU_SETSIZE); CPU_ZERO(&selected); CPU_SET(cpu,&selected);
    assert(!sched_setaffinity(0,sizeof(selected),&selected));
    printf("REPLAY_CLOCK cpu=%d pacing=virtual render=monotonic cycles=serialized_rdtsc\n",cpu);
    struct recorded_event events[128]; unsigned count=0; char line[128];
    FILE *trace=fopen(argv[4],"r"); assert(trace);
    while (fgets(line,sizeof(line),trace)) {
        if (line[0]=='#') continue;
        assert(count<128 && sscanf(line,"%u,%23[^,],%d,%d",&events[count].ms,events[count].action,
            &events[count].a,&events[count].b)==4);
        if (count) assert(events[count].ms>=events[count-1].ms);
        count++;
    }
    assert(!fclose(trace) && count);
    for (engine_caps=0;engine_caps<=3;engine_caps+=3) {
        replay_trace(argv[1],argv[2],argv[3],events,count,0);
#ifdef DESK_PROFILE
        replay_trace(argv[1],argv[2],argv[3],events,count,1);
#endif
    }
    puts("PASS rendering replay: recorded drag/menu/launch, complete scanout, pacing latency and exclusive painters");
    return 0;
}
