/* SPDX-License-Identifier: MIT
 * Included by shell_launch_test.c; queries stop in the real process ledger. */
static void test_poll_handles(void)
{
    static struct desktop d;
    fake_reset(); desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    desktop_poll_host_init(); poll_bridge=1;
    media_available=network_available=mixer_available=1;
    network_fixture=(struct ciuki_net_state){.version=CIUKI_NET_VERSION,.size=sizeof(network_fixture)};
    media_fixture=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_fixture)};
    mixer_fixture=(struct ciuki_mixer_master){.version=CIUKI_AUDIO_VERSION,.size=sizeof(mixer_fixture),
        .left=50,.right=50,.save_state=CIUKI_MIXER_SAVED};
    for (unsigned i=3;i<26;i++) media_volumes[i]=(struct ciuki_media_volume){
        .version=CIUKI_MEDIA_VERSION,.size=sizeof(media_volumes[i]),.drive=i};

    /* Reproduce the previous open/query/close window with no count allowance.
     * Each devctl interleaves the production kernel snapshot and diagnostics. */
    desktop_poll_host_baseline(); poll_snapshot=1; poll_equal=0;
    struct ciuki_net_state net=network_fixture;
    struct ciuki_media_state media=media_fixture;
    struct ciuki_mixer_master mixer=mixer_fixture;
    int fd=shell_open("/dev/net",O_RDONLY|O_CLOEXEC);
    assert(!ciuki_devctl(fd,NET_GET_STATE,&net,sizeof(net))); shell_close(fd);
    fd=shell_open("/dev/media",O_RDONLY|O_CLOEXEC);
    assert(!ciuki_devctl(fd,MEDIA_GET_STATE,&media,sizeof(media))); shell_close(fd);
    fd=shell_open("/dev/mixer",O_RDONLY|O_CLOEXEC);
    assert(!ciuki_devctl(fd,MIXER_GET_MASTER,&mixer,sizeof(mixer))); shell_close(fd);
    poll_snapshot=0; desktop_poll_host_check(1);

    observers_prepare(&d); assert(d.network_fd>=0 && d.media_fd>=0 && d.mixer_observer_fd>=0 && d.mixer_fd<0);
    assert(media_read(&d)); /* Warm the worker-owned observation descriptor. */
    desktop_poll_host_baseline(); poll_snapshot=1; poll_equal=1;
    unsigned opens=network_opens, queries=mixer_queries;
    for (unsigned i=0;i<100;i++) {
        network_read(&d); assert(media_read(&d)); mixer_read(&d);
        assert(d.sh.mixer_state==1); /* observing never grants control */
        desktop_poll_host_check(1);
    }
    assert(network_opens==opens && mixer_queries==queries+100 && !mixer_sets);
    assert(!mixer_refreshes); /* f1-100: periodic observation never asks for a hardware read */
    poll_snapshot=0;
    /* An extra media handle still fails against the warmed observer baseline. */
    desktop_poll_host_leak(1); desktop_poll_host_check(0);
    desktop_poll_host_leak(0); desktop_poll_host_check(1);

    /* Media's retained descriptor belongs to its worker after startup.
     * Failed observations release only the affected observer; later reads
     * reopen it. An absent medium/NIC/audio device is still a valid query. */
    network_error=1; network_read(&d); assert(d.network_fd<0 && d.media_fd<0 && d.mixer_observer_fd>=0);
    network_error=0; network_read(&d); assert(d.network_fd>=0);
    media_query_error=1; assert(!media_read(&d) && d.media_fd<0);
    media_query_error=0; assert(media_read(&d) && d.media_fd<0);
    media_volume_error=1; assert(!media_read(&d) && d.media_fd<0);
    media_volume_error=0; assert(media_read(&d) && d.media_fd<0);
    mixer_error=1; mixer_read(&d); assert(d.mixer_observer_fd<0 && d.sh.mixer_state<0);
    mixer_error=0; mixer_read(&d); assert(d.mixer_observer_fd>=0 && d.sh.mixer_state==1);
    desktop_poll_host_check(1);

    /* A delegated control handle is reused, not replaced with public access. */
    d.mixer_fd=d.mixer_observer_fd; d.mixer_observer_fd=-1;
    observers_prepare(&d); assert(d.mixer_observer_fd<0);
    poll_snapshot=1; mixer_read(&d); assert(d.sh.mixer_state==2); poll_snapshot=0;
    d.power.state=DESK_POWER_RETRY; d.power.prepared=1; power_prepare(&d);
    assert(d.media_fd<0 && d.network_fd<0 && d.mixer_observer_fd<0 && d.mixer_fd<0);
    desktop_poll_host_baseline(); poll_snapshot=1;
    network_read(&d); assert(!media_read(&d)); mixer_read(&d); desktop_poll_host_check(1);
    poll_snapshot=0;
    desk_power_cancel(&d); /* prepared remains set; cancellation must allow recovery */
    network_read(&d); assert(media_read(&d)); mixer_read(&d);
    assert(d.network_fd>=0 && d.media_fd<0 && d.mixer_observer_fd>=0 && d.sh.mixer_state==1);
    observers_close(&d); observers_close(&d); /* idempotent shutdown */
    desktop_poll_host_finish(); poll_bridge=0;
    desk_render_release(&d); fake_reset();
    puts("PASS poll handles: old interleaving identifies net/media/mixer; 100 retained polls equal; errors, authority, shutdown/cancel and zero final handles");
}
