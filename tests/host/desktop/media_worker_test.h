/* SPDX-License-Identifier: MIT
 * Included after desktop.c; real service turns with 25 ms device queries. */
static void test_slow_media_panel(void)
{
    static struct desktop d;
    fake_reset();desk_init(&d,&fake_ops,640,480);desk_shell_enable(&d);
    media_available=1;media_query_error=media_volume_error=0;
    media_fixture=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_fixture),.removable_mask=1u<<4};
    media_fixture.type[4]=CIUKI_MEDIA_USB;media_fixture.generation[4]=77;
    media_volumes[4]=(struct ciuki_media_volume){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_volumes[4]),.drive=4,.flags=CIUKI_MEDIA_REMOVABLE,.generation=77};
    strcpy(media_volumes[4].label,"SLOW VOLUME");
    desk_shell_media_update(&d,&media_fixture);
    media_frame_thread=pthread_self();media_slow_queries=0;media_slow=1;
    struct desk_box icon=desk_shell_tray_icon_box(&d,SH_TRAY_MEDIA);
    d.mouse_x=icon.x+icon.w/2;d.mouse_y=icon.y+icon.h/2;
    assert(desk_shell_button(&d,1,1));desk_shell_button(&d,1,0);
    assert(d.sh.media_open && (d.sh.want_reload&32));
    uint64_t start=monotonic(),max=0;unsigned frames=0;
    desktop_watchdog.desktop=&d;
    while (monotonic()-start<3000000000ull) {
        uint64_t begin=monotonic();d.now_ns=begin;
        __atomic_store_n(&desktop_watchdog.frame_ms,(uint32_t)(begin/1000000ull),__ATOMIC_RELEASE);
        desktop_stage(STALL_SHELL);shell_service(&d);
        uint64_t elapsed=monotonic()-begin;if(elapsed>max)max=elapsed;
        assert(elapsed<100000000ull);
        char stall[512];assert(!desktop_stall_record((uint32_t)(monotonic()/1000000ull),stall,sizeof(stall)));
        if (++frames%60==0)d.sh.want_reload|=32;
        struct timespec pause={.tv_sec=0,.tv_nsec=10000000};nanosleep(&pause,NULL);
    }
    assert(frames>100 && __atomic_load_n(&media_slow_queries,__ATOMIC_RELAXED)>=24);
    assert(d.sh.media.removable_mask==(1u<<4) && d.sh.media_open);
    /* Even a contended mailbox is deferred without blocking a frame. */
    pthread_mutex_lock(&media_worker.lock);
    uint64_t begin=monotonic();assert(!media_read(&d));media_poll(&d);
    assert(monotonic()-begin<100000000ull);pthread_mutex_unlock(&media_worker.lock);
    observers_close(&d);media_slow=0;media_available=0;
    desktop_watchdog.desktop=NULL;desk_render_release(&d);free(d.sh.icons);fake_reset();
    printf("PASS F2-80 media panel: 25 ms/call, %u frames, max_service_us=%llu, worker-only queries, no media-read frame stall\n",frames,(unsigned long long)(max/1000));
}

/* Real desktop watch_result/media worker/shell service. Device queries cost
 * 25 ms, as in the slow-stick replay; an E: arrival follows service startup
 * by the T23's 3459-1844 ms. Readiness stays level-triggered until GET_STATE. */
static void test_late_media(void)
{
    static struct desktop d;
    fake_reset();desk_init(&d,&fake_ops,1024,768);desk_shell_enable(&d);
    media_available=1;media_query_error=media_volume_error=0;
    media_fixture=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_fixture)};
    memset(media_volumes,0,sizeof(media_volumes));
    media_frame_thread=pthread_self();media_slow_queries=0;media_slow=1;
    media_events=-1;media_fixture_serial=1;media_observed_serial=0;
#if defined(DESK_FOOTPRINT_TEST) || defined(DESK_POLL_LEDGER_TEST)
    mixer_available=1;mixer_error=0;mixer_events=-1;
    mixer_fixture=(struct ciuki_mixer_master){.version=CIUKI_AUDIO_VERSION,.size=sizeof(mixer_fixture),.left=50,.right=50};
    unsigned initial_mixer_queries=mixer_queries,mixer_wakes=0;
#endif
    observers_prepare(&d);assert(media_read(&d));
#if defined(DESK_FOOTPRINT_TEST) || defined(DESK_POLL_LEDGER_TEST)
    mixer_read(&d);
#endif
    uint64_t start=monotonic(),max=0;unsigned frames=0,wakes=0;int arrived=0;
    while (monotonic()-start<3500000000ull) {
        uint64_t begin=monotonic();d.now_ns=begin;
        int fd=media_watch_fd(&d);
        if (!arrived && begin-start>=1615000000ull && fd>=0) {
            media_fixture.removable_mask=1u<<4;media_fixture.type[4]=CIUKI_MEDIA_USB;
            media_fixture.generation[4]=495;
            media_volumes[4]=(struct ciuki_media_volume){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_volumes[4]),
                .drive=4,.flags=CIUKI_MEDIA_REMOVABLE,.generation=495};
            strcpy(media_volumes[4].label,"T23 USB");
            __atomic_add_fetch(&media_fixture_serial,1,__ATOMIC_RELEASE);arrived=1;
#if defined(DESK_FOOTPRINT_TEST) || defined(DESK_POLL_LEDGER_TEST)
            mixer_fixture_serial++; /* an EC/master notification at arrival */
#endif
        }
        if (fd>=0 && __atomic_load_n(&media_observed_serial,__ATOMIC_ACQUIRE)!=
                      __atomic_load_n(&media_fixture_serial,__ATOMIC_ACQUIRE)) {
            watch_result(&d,-1,POLLIN);wakes++;
        }
#if defined(DESK_FOOTPRINT_TEST) || defined(DESK_POLL_LEDGER_TEST)
        if (mixer_watch_fd(&d)>=0) {
            int ready=mixer_observed_serial!=mixer_fixture_serial ? POLLIN : 0;
            watch_result(&d,ready,-1);mixer_wakes+=ready!=0;
        }
#endif
        shell_service(&d);
        uint64_t elapsed=monotonic()-begin;if(elapsed>max)max=elapsed;
        assert(elapsed<100000000ull);frames++;
        struct timespec pause={.tv_sec=0,.tv_nsec=10000000};nanosleep(&pause,NULL);
    }
    assert(arrived && frames>150 && wakes==1);
    assert(d.sh.media.removable_mask==(1u<<4) && d.sh.media.generation[4]==495);
    assert(media_worker.requested==2 && media_observed_serial==media_fixture_serial);
    assert(media_watch_fd(&d)>=0); /* idle and acknowledged: subsequent poll can sleep */
#if defined(DESK_FOOTPRINT_TEST) || defined(DESK_POLL_LEDGER_TEST)
    assert(mixer_wakes==1 && mixer_queries==initial_mixer_queries+2);
    assert(mixer_observed_serial==mixer_fixture_serial);mixer_available=0;
#endif
    observers_close(&d);media_slow=0;media_available=0;desk_render_release(&d);free(d.sh.icons);fake_reset();
    printf("PASS T23 desktop media: +1615 ms E arrival, 25 ms queries, %u service turns, wakes=%u, requests=2, max_service_us=%llu, media/mixer readiness consumed\n",
        frames,wakes,(unsigned long long)(max/1000));
}
