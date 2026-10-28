/* Production startup and Start > Shut Down > Restart, no guest/hardware. */
static void test_display_start(void)
{
    static struct desktop d;
    for (unsigned scenario=0;scenario<6;scenario++) {
        fake_reset(); desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d);
        startup_display_info=(struct ciuki_display_info){.version=CIUKI_DISPLAY_VERSION,
            .size=sizeof(startup_display_info),.width=1024,.height=768,.bpp=32,.mode_count=2};
        startup_display_info.modes[0]=(struct ciuki_display_mode){.id=280,.current=1};
        startup_display_info.modes[1]=(struct ciuki_display_mode){.id=274};
        if (scenario==5) {
            startup_display_info.pitch=4096;
            startup_display_info.modes[1]=(struct ciuki_display_mode){.id=274,
                .width=1024,.height=768,.bpp=32,.pitch=4096};
        }
        startup_display_enabled=1; startup_display_error=scenario==0 ? -EIO : 0;
        startup_display_sets=0;
        d.sh.cfg.display_mode=scenario==1 ? 999 : scenario==2 ? 280 : 274;
        d.sh.gate=scenario==3;
        display_start(&d);
        if (scenario<2) {
            assert(startup_display_sets==(scenario==0));
            assert(d.display_mode==280 && d.sh.cfg.display_mode==280 && d.sh.want_save);
            assert(!d.display_start_mode && !d.display_pending);
            assert(!strcmp(d.sh.message_title,"Display Settings"));
            assert(strstr(d.sh.message_text,"1024x768") && !strstr(d.sh.message_text,"(5)"));
            struct ciuki_settings durable; desk_preferences_persistent(&d,&durable);
            char text[CFG_TEXT_MAX]; assert(cfg_format(&durable,text,sizeof(text)));
            assert(strstr(text,"display_mode=280\n"));
        } else if (scenario<4 || scenario==5) {
            assert(!startup_display_sets && !d.sh.want_save && !d.display_start_mode);
            assert(d.sh.cfg.display_mode==(scenario==2 ? 280 : 274));
        } else {
            assert(startup_display_sets==1 && d.display_pending==274 && d.display_start_mode==274);
            assert(d.sh.cfg.display_mode==274 && !d.sh.want_save);
            d.display_mode=274; d.damage.count=0;
            assert(desk_display_frame_commit(&d,0)==DESK_DISPLAY_COMPLETE_FRAME);
            assert(!d.display_start_mode && !d.sh.want_save);
        }
        for (int i=0;i<DESK_CLIENTS;i++) desk_drop_client(&d,i);
        free(d.sh.wall);
        startup_display_enabled=0;
        assert(!fake_live());
    }
    puts("PASS fx-dispmode startup: refused/missing preference resets to working mode with words; same-mode/gate untouched; accepted frame retains preference");
    /* Control Panel has durably confirmed 274. Reload must adopt its file,
     * even if a later lease/return observes 280 before another settings save. */
    shutdown_setup();
    struct desktop *desktop=&shutdown_desktop;
    char path[1024],temporary[1030];
    snprintf(path,sizeof(path),"%s/display-XXXXXX",getenv("TMPDIR"));
    int fd=mkstemp(path); assert(fd>=0);
    FILE *file=fdopen(fd,"w"); assert(file);
    struct ciuki_settings confirmed=desktop->sh.cfg; confirmed.display_mode=274;
    char text[CFG_TEXT_MAX]; size_t n=cfg_format(&confirmed,text,sizeof(text)); assert(n);
    assert(fwrite(text,1,n,file)==n && !fclose(file));
    snprintf(temporary,sizeof(temporary),"%s.new",path);
    memset(&settings_writer,0,sizeof(settings_writer));
    assert(!settings_writer_started && !cfg_writer_start(&settings_writer,path,temporary));
    settings_writer_started=1;
    desktop->sh.cfg.display_mode=280; desktop->sh.want_reload=1;
    for (unsigned turn=0;turn<1000;turn++) {
        shell_service(desktop);
        if (!settings_reload_pending && !(desktop->sh.want_reload&1)) break;
        struct timespec pause={.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    assert(!settings_reload_pending && desktop->sh.cfg.display_mode==274);
    desktop->display_mode=desktop->display_pending=280; desktop->damage.count=0;
    assert(desk_display_frame_commit(desktop,0)==DESK_DISPLAY_COMPLETE_FRAME);
    assert(!settings_save(desktop));
    for (unsigned turn=0;turn<1000 && cfg_writer_busy(&settings_writer);turn++) {
        struct timespec pause={.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    assert(!cfg_writer_busy(&settings_writer));
    cfg_writer_stop(&settings_writer); settings_writer_started=0;
    file=fopen(path,"r"); assert(file);
    n=fread(text,1,sizeof(text)-1,file); text[n]=0; assert(!fclose(file));
    assert(strstr(text,"display_mode=274\n")); assert(!unlink(path));
    shutdown_cleanup();
    puts("PASS fx-dispmode durable confirmation: desktop reload adopts Control Panel file; later mode notification and settings save retain it");
}
static void display_click(struct desktop *d, int32_t x, int32_t y)
{
    d->mouse_x=x; d->mouse_y=y;
    struct ciuki_input_event e={.type=CIUKI_INPUT_BUTTON,.code=CIUKI_BUTTON_LEFT,.value=1};
    desk_input(d,&e); e.value=0; desk_input(d,&e);
}
static void test_display_safe(void)
{
    shutdown_setup(); struct desktop *d=&shutdown_desktop;
    d->safe_mode=1; d->sh.cfg.display_mode=280; d->sh.cfg.icon_auto_arrange=1;
    assert(cfg_icon_move_at(&d->sh.cfg,"Computer",1024,768,800,600));
    assert(cfg_icon_move_at(&d->sh.cfg,"Computer",640,480,500,300));
    char positions[CFG_EXTRA_MAX]; strcpy(positions,d->sh.cfg.extra);
    startup_display_info=(struct ciuki_display_info){.version=CIUKI_DISPLAY_VERSION,
        .size=sizeof(startup_display_info),.width=640,.height=480,.bpp=32,.mode_count=1};
    startup_display_info.modes[0]=(struct ciuki_display_mode){.id=274,.current=1};
    startup_display_enabled=1; startup_display_error=0; startup_display_sets=0;
    display_start(d);
    assert(!startup_display_sets && d->display_mode==274 && d->sh.cfg.display_mode==280);
    assert(!d->sh.want_save && !d->display_start_mode);
    assert(d->display_set(d,280)==-EOPNOTSUPP && !startup_display_sets);
    desk_shell_apply(d); /* auto-arranged low-resolution icons are transient */
    assert(!strcmp(positions,d->sh.cfg.extra));
    d->display_pending=274; d->damage.count=0;
    assert(desk_display_frame_commit(d,0)==DESK_DISPLAY_COMPLETE_FRAME);
    assert(d->sh.cfg.display_mode==280);
    d->sh.cfg.clock_seconds=!d->sh.cfg.clock_seconds; /* an unrelated save */
    char path[1024],temporary[1030],text[CFG_TEXT_MAX];
    snprintf(path,sizeof(path),"%s/display-safe-XXXXXX",getenv("TMPDIR"));
    int fd=mkstemp(path); assert(fd>=0 && !close(fd));
    snprintf(temporary,sizeof(temporary),"%s.new",path);
    memset(&settings_writer,0,sizeof(settings_writer));
    assert(!settings_writer_started && !cfg_writer_start(&settings_writer,path,temporary));
    settings_writer_started=1; assert(!settings_save(d));
    for (unsigned turn=0;turn<1000 && cfg_writer_busy(&settings_writer);turn++) {
        struct timespec pause={.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    assert(!cfg_writer_busy(&settings_writer));
    cfg_writer_stop(&settings_writer); settings_writer_started=0;
    FILE *file=fopen(path,"r"); assert(file);
    size_t n=fread(text,1,sizeof(text),file); assert(!fclose(file) && !unlink(path));
    struct ciuki_settings saved; cfg_defaults(&saved); assert(!cfg_parse(&saved,text,n));
    assert(saved.display_mode==280 && saved.clock_seconds==d->sh.cfg.clock_seconds);
    assert(!strcmp(saved.extra,positions));
    /* The following normal boot still requests the original normal mode. */
    d->safe_mode=0; d->sh.cfg=saved;
    startup_display_info.mode_count=2; startup_display_info.modes[1].id=280;
    display_start(d);
    assert(startup_display_sets==1 && startup_display_last_mode==280 && d->display_start_mode==280);
    startup_display_enabled=0; shutdown_cleanup();
    puts("PASS fx-dispmode Safe Mode: 640x480 inventory omits saved 280; no apply/reset, positions unchanged, unrelated durable save retains normal mode; next normal boot requests 280");
}
static void test_display_restart(void)
{
    for (unsigned scenario=0;scenario<4;scenario++) {
        unsigned standby=scenario==1;
        shutdown_setup();
        struct desktop *d=&shutdown_desktop;
        if (scenario>=2) {
            d->power_request=power_request; power_kernel_success=1;
            power_kernel_reboots=0;
        }
        d->display_mode=274; d->sh.cfg.display_mode=280;
        d->safe_mode=scenario==3;
        d->sh.power_can_standby=(int)standby;
        struct desk_box start=desk_start_bounds(d);
        display_click(d,start.x+start.w/2,start.y+start.h/2);
        assert(d->sh.levels==1 && d->sh.menus[0].kind==SH_MENU_START);
        struct sh_menu *m=&d->sh.menus[0];
        assert(!m->scroll_max && m->items[m->count-1].action==SH_ACT_SHUTDOWN);
        display_click(d,m->box.x+m->box.w/2,m->box.y+m->box.h-3-m->row_h/2);
        int i=d->focus; assert(i>=0 && d->clients[i].internal==DESK_INTERNAL_SHUTDOWN);
        struct desk_box content=desk_content(&d->clients[i]);
        display_click(d,content.x+SD_RADIO_X+30,content.y+SD_RADIO_Y+(1+(int)standby)*SD_RADIO_STEP+6);
        assert(d->sh.shutdown_sel==SD_RESTART);
        display_click(d,content.x+content.w-10-3*81+6+37,content.y+content.h-22);
        assert(d->sh.want_power==1);
        shell_service(d);
        assert(d->power.action==1 && d->power.state==DESK_POWER_CLOSING);
        if (scenario<2) {
            shutdown_at(d->now_ns);
            assert(shutdown_calls==1 && d->power.state==DESK_POWER_DONE);
        } else {
            for (unsigned turn=0;turn<1000 && d->power.state!=DESK_POWER_DONE;turn++) {
                desk_power_tick(d);
                d->now_ns+=250000000ull;
                struct timespec pause={.tv_nsec=1000000}; nanosleep(&pause,NULL);
            }
            assert(d->power.state==DESK_POWER_DONE);
            assert(__atomic_load_n(&power_kernel_reboots,__ATOMIC_RELAXED)==1);
            power_kernel_success=0;
        }
        assert(d->sh.cfg.display_mode==280);
        shutdown_cleanup();
    }
    puts("PASS fx-dispmode Restart: 640x480 Start menu, Restart radio and OK reach power request, with/without standby; normal/Safe Mode use the production worker through CIUKI_POWER_REBOOT");
}
