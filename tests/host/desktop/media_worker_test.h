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
