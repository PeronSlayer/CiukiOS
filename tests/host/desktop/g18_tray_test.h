/* G18 rows: production v3 requests, shell painter and five presenters. */
#ifndef CIUKI_G18_TRAY_TEST_H
#define CIUKI_G18_TRAY_TEST_H
static int g18_snapshot(struct desktop *desktop)
{ (void)desktop; int fd=take_fd(); fake_surface(fd,1024,1,0); return fd; }
static int g18_request(int i,unsigned op,const uint32_t *words,unsigned count,const char *text,int fd)
{
    struct desk_client *c=&d.clients[i]; struct ciuki_message m;
    unsigned n=text?(unsigned)strlen(text):0;
    v3msg(&m,op,count*4+n,c->v3_id,c->v3_generation);
    for (unsigned k=0;k<count;k++) desk_put32(m.data+16+k*4,words[k]);
    if(n)memcpy(m.data+16+count*4,text,n);
    if(fd>=0){m.fd_count=1;m.fds[0]=fd;}
    return desk_v3_receive(&d,v3_root,&m);
}
static void row_g18_tray(unsigned m)
{
    row_name="G18 tray and flash"; wm_begin(m,1);
    d.sh.cfg.layout_switch=1; /* G12 and G18 share the tray. */
    desk_damage_add(&d.damage,(struct desk_box){0,0,(int)d.width,(int)d.height},d.width,d.height);
    d.drop_snapshot=g18_snapshot;
    int i=wm_v3_window(0,-1,60,100,260,180); assert(i>=0);
    int j=wm_v3_window(0,-1,340,120,180,140); assert(j>=0);
    step("two Win32 windows"); int focus=d.focus;
    /* fx-exeicons DESK_V3_WINDOW_ICON: a 32 px icon also gives the 16 px one
     * (2x2 average) until the program sends its own; size 0 drops both. The
     * window ends without icons, so this row's frames stay as they were. */
    unsigned icon_fd=(unsigned)take_fd(); fake_surface((int)icon_fd,32,32,0xff2040c0u);
    uint32_t big[]={32},small[]={16},none[]={0};
    assert(!g18_request(i,DESK_V3_WINDOW_ICON,big,1,NULL,(int)icon_fd));
    assert(d.clients[i].v3_icon_mask==7 && d.clients[i].v3_icon32[0]==0xff2040c0u && d.clients[i].v3_icon16[255]==0xff2040c0u);
    assert(desk_shell_window_icon(&d,i,16)==d.clients[i].v3_icon16 && desk_shell_window_icon(&d,i,32)==d.clients[i].v3_icon32);
    icon_fd=(unsigned)take_fd(); fake_surface((int)icon_fd,16,16,0xff10e010u);
    assert(!g18_request(i,DESK_V3_WINDOW_ICON,small,1,NULL,(int)icon_fd));
    assert(d.clients[i].v3_icon_mask==3 && d.clients[i].v3_icon16[0]==0xff10e010u && d.clients[i].v3_icon32[0]==0xff2040c0u);
    assert(!g18_request(i,DESK_V3_WINDOW_ICON,none,1,NULL,-1));
    assert(!d.clients[i].v3_icon_mask && desk_shell_window_icon(&d,i,16)!=d.clients[i].v3_icon16);
    uint32_t flag[]={1};
    assert(!g18_request(i,DESK_V3_FLASH,flag,1,NULL,-1));
    assert(d.clients[i].flash && d.focus==focus); step("background task flashes without activation");
    assert(!g18_request(i,DESK_V3_FLASH,flag,1,NULL,-1)); step("flash toggles back");
    assert(!d.clients[i].flash);
    assert(!g18_request(i,DESK_V3_FLASH,flag,1,NULL,-1)); step("flash while minimized follows task button");
    desk_minimize(&d,i); step("flashing window minimized");
    flag[0]=0; assert(!g18_request(i,DESK_V3_FLASH,flag,1,NULL,-1)); step("FALSE restores appearance");
    unsigned fd=(unsigned)take_fd(); fake_surface((int)fd,16,16,0xff00a040u);
    uint32_t add[]={0,123,7,0x450,6};
    assert(!g18_request(i,DESK_V3_TRAY_ICON,add,5,"Player",(int)fd));
    assert(d.sh.program_tray[0].window==d.clients[i].window_id && d.sh.program_tray[0].pixels[0]==0xff00a040u);
    step("program icon enters tray and shifts layout");
    struct desk_box language=desk_shell_tray_icon_box(&d,SH_TRAY_LANGUAGE);
    struct desk_box program=desk_shell_tray_box(&d);
    assert(program.x+SH_TRAY_MARGIN+SH_TRAY_ICON_SIZE<=language.x);
    uint32_t mod[]={1,123,4,0,5};
    assert(!g18_request(i,DESK_V3_TRAY_ICON,mod,5,"Ready",-1));
    assert(d.sh.program_tray[0].callback==0x450 && d.sh.program_tray[0].pixels[0]==0xff00a040u);
    step("tooltip modification keeps callback and pixels");
    struct desk_box tray=desk_shell_tray_box(&d);
    pointer_to(tray.x+SH_TRAY_MARGIN+4,tray.y+6); turn(800000000ull); step("program tooltip");
    assert(d.sh.hover_target==256);
    click(d.mouse_x,d.mouse_y,CIUKI_BUTTON_RIGHT); step("program tray right click"); assert(d.focus==focus);
    uint32_t ball[]={123,5,4,10000|(2u<<16)};
    assert(!g18_request(i,DESK_V3_TRAY_BALLOON,ball,4,"TitleText",-1));
    assert(!g18_request(i,DESK_V3_TRAY_BALLOON,ball,4,"OtherBody",-1));
    assert(d.sh.program_balloon_count==2);
    turn(16666667ull); step("first queued program balloon");
    assert(d.sh.notification.active && d.sh.balloon_window==d.clients[i].window_id && d.sh.program_balloon_count==1);
    assert(d.sh.notification.icon==DU_DLG_ICON_WARNING);
    turn(11000000000ull); step("first program balloon times out");
    turn(16666667ull); step("second queued program balloon");
    assert(d.sh.notification.active && !strcmp(d.sh.notification.title,"Other"));
    uint32_t del[]={2,123,0,0,0}; assert(!g18_request(i,DESK_V3_TRAY_ICON,del,5,NULL,-1));
    assert(!d.sh.notification.active && !d.sh.program_tray[0].window); step("delete cancels owner balloon");
    assert(g18_request(i,DESK_V3_TRAY_ICON,mod,5,"Ready",-1)==-ENOENT);
    /* Same numeric ID in another window is a separate owner. */
    uint32_t add_blank[]={0,123,5,0x450,6};
    assert(!g18_request(i,DESK_V3_TRAY_ICON,add_blank,5,"Player",-1));
    assert(!g18_request(j,DESK_V3_TRAY_ICON,add_blank,5,"Player",-1)); step("same ID independent windows");
    uint32_t count[]={1}; assert(!g18_request(j,DESK_ACCEPT_FILES,count,1,NULL,-1)); assert(d.clients[j].accept_files);
    struct fake_app *files=app_open("files",200,100,60,320);
    assert(files_drag_begin(files,1)); struct desk_box target=desk_content(&d.clients[j]);
    pointer_to(target.x+20,target.y+20); step("Files drag over Win32 accepting window");
    assert(d.sh.dnd.kind==SH_DROP_APP && d.sh.dnd.effect==FM_EFFECT_COPY);
    press(CIUKI_BUTTON_LEFT,0); step("Win32 DROP_FILES routed in v3 envelope");
    struct ciuki_message msg; assert(!g18_request(i,DESK_CLOSE,NULL,0,NULL,-1)); step("window close cleans its own icon");
    assert(!d.sh.program_tray[0].window && d.sh.program_tray[1].window==d.clients[j].window_id);
    /* A blocked transport keeps sender ownership; send or peer teardown
     * releases it exactly once. The kernel supplies a receiver duplicate. */
    int snapshot=g18_snapshot(&d);
    desk_v3_message(&msg,DESK_DROP_FILES,12,0,d.clients[j].v3_id,d.clients[j].v3_generation);
    desk_put32(msg.data+16,99); msg.fd_count=1; msg.fds[0]=snapshot;
    desk_queue(&d,j,&msg); fake_block=1; desk_flush(&d,v3_root); assert(!fake_fd_free(snapshot));
    fake_block=0; desk_flush(&d,v3_root); assert(fake_fd_free(snapshot));
    snapshot=g18_snapshot(&d); msg.fds[0]=snapshot; desk_queue(&d,j,&msg);
    desk_drop_client(&d,v3_root); assert(fake_fd_free(snapshot)); step("dead peer releases queued drop descriptor");
    /* Native and DOS shortcut Run state is consumed on first CREATE. */
    assert(!desk_task_launch(&d,"calc",NULL)); struct fake_app *native=last_app;
    d.clients[native->slot].initial_show=7; app_step(native); app_step(native);
    assert(d.clients[native->slot].minimized && !d.clients[native->slot].initial_show); step("native shortcut starts minimized");
    assert(!desk_task_launch(&d,"calc",NULL)); native=last_app;
    d.clients[native->slot].initial_show=3; app_step(native); app_step(native);
    assert(d.clients[native->slot].zoomed && !d.clients[native->slot].initial_show); step("native shortcut starts maximized");
    wm_end();
}
#endif
