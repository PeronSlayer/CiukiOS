/* F2-76: ordinary (non-gate) desktop, mounted USB, actual launch/input paths.
 * Included by shell_launch_test.c so the syscall and spawn seams are shared.
 * SPDX-License-Identifier: MIT */
#include "../../../apps/desktop/keymap.h"
#include "autoplay_wait.h"
static int empty_media_list(void *ctx,const char *path,fm_emit emit,void *user)
{ (void)ctx; (void)path; (void)emit; (void)user; return 0; }
static const struct fm_vfs empty_media_fs={.list=empty_media_list};
static int no_media_preference(struct desktop *d,const char *id,const char *label,int action)
{ (void)d; (void)id; (void)label; assert(action==-1); return -1; }
static void input_edge(struct desktop *d,int code,int down)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=down,.monotonic_ns=d->now_ns};
    desk_input(d,&e);
}
static void input_tap(struct desktop *d,int code)
{ input_edge(d,code,1); input_edge(d,code,0); }
static void assert_launch_empty(const struct desktop *d)
{
    assert(!d->launch_arg);
    for (unsigned i=0;i<6;i++) assert(!d->launch_argv[i]);
}
static void input_text(struct desktop *d,const char *text)
{
    for (;*text;text++) {
        int shift; uint16_t code=dosbox_key_for(*text,&shift); assert(code);
        if (shift) input_edge(d,0x2a,1);
        input_tap(d,code);
        if (shift) input_edge(d,0x2a,0);
    }
}
static void test_launch_input(void)
{
    static struct desktop d;
    uint8_t elf[52]={0x7f,'E','L','F',1,1,1}; elf[16]=2; elf[18]=3; elf[20]=1; elf[40]=52;
    fake_reset(); desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d);
    assert(!d.sh.gate);
    d.launch=launch_program; d.authority_fd=-1;
    d.autoplay_fs=&empty_media_fs; d.autoplay_preference=no_media_preference;
    assert(d.sh.autoplay.workspace);
    assert(!desk_autoplay_prepare(&d));
    d.pixels=calloc(1024*768,4); assert(d.pixels);
    assert(!desk_redraw(&d) && d.presents);
    desk_program_add(&d,"dosbox",elf,sizeof(elf)); desk_program_add(&d,"control",elf,sizeof(elf));
    media_available=1;
    media_fixture=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_fixture)};
    assert(media_read(&d));
    media_fixture=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_fixture),.removable_mask=1u<<4};
    media_fixture.type[4]=CIUKI_MEDIA_USB; media_fixture.generation[4]=77;
    media_volumes[4]=(struct ciuki_media_volume){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_volumes[4]),
        .drive=4,.generation=77,.flags=CIUKI_MEDIA_REMOVABLE};
    strcpy(media_volumes[4].id,"fat-12345678-1234");
    assert(media_read(&d) && d.sh.notification.active && d.sh.autoplay.waiting==(1u<<4));
    unsigned before=dosbox_spawns;
    startup_launch(&d,"dosbox"); assert(d.action>=0);
    assert(launch_pending(&d)>0 && dosbox_spawns==before+1); assert_launch_empty(&d);
    /* The window has sent its first frame; no gate/scenario pacing is active. */
    int box=0; d.clients[box].window=1; d.clients[box].window_id=++d.next_window_id;
    d.clients[box].frame=desk_frame(640,400,20,52); d.clients[box].awaiting_frame=0;
    d.clients[box].version=DESK_SHELL_VERSION; desk_focus(&d,box);
    desk_flush(&d,box);
    unsigned sent=fake_sent, controls=control_spawns;
    for (unsigned tick=0;tick<3;tick++) {
        d.now_ns+=100000000ull; desk_shell_tick(&d); shell_service(&d); desk_flush(&d,box);
    }
    assert(dosbox_spawns==before+1 && control_spawns==controls && !d.sh.want_run[0] && d.action<0);
    assert(d.focus==box && !d.sh.autoplay.open && d.sh.autoplay.waiting==(1u<<4));
    for (unsigned i=sent;i<fake_sent;i++) {
        struct desk_packet p; assert(desk_parse(&fake_messages[i],0,NULL,&p)); assert(p.opcode!=DESK_KEY);
    }
    /* A late media arrival must not cancel an in-progress Start-menu command. */
    for (int i=0;i<DESK_CLIENTS;i++) if (d.clients[i].internal==DESK_INTERNAL_AUTOPLAY) desk_drop_client(&d,i);
    media_volumes[4].generation++; media_fixture.generation[4]++;
    input_edge(&d,0x1d,1); input_tap(&d,DU_KEY_ESC); input_edge(&d,0x1d,0);
    assert(d.menu_open && d.sh.levels==1);
    assert(media_read(&d)); shell_service(&d);
    assert(d.menu_open && d.sh.levels==1);
    assert(d.sh.autoplay.waiting==(1u<<4) && !d.sh.autoplay.open);
    /* Ctrl+Esc selects Programs. Four Down presses reach Run; the old media
     * fixture's five reach Shut Down. */
    for (unsigned i=0;i<4;i++) input_tap(&d,DU_KEY_DOWN);
    assert(d.sh.menus[0].items[d.sh.menus[0].hot].action==SH_ACT_RUN);
    input_tap(&d,DU_KEY_ENTER);
    assert(d.focus>=0 && d.clients[d.focus].internal==DESK_INTERNAL_RUN);
    desk_flush(&d,box);
    sent=fake_sent;
    input_text(&d,"control --panel=sounds"); shell_service(&d);
    assert(d.focus>=0 && d.clients[d.focus].internal==DESK_INTERNAL_RUN && control_spawns==controls);
    expect_control_arg="--panel=sounds";
    input_tap(&d,DU_KEY_ENTER); shell_service(&d); desk_flush(&d,box);
    assert(control_spawns==controls+1 && d.sh.autoplay.waiting==(1u<<4)); assert_launch_empty(&d);
    for (unsigned i=sent;i<fake_sent;i++) {
        struct desk_packet p; assert(desk_parse(&fake_messages[i],0,NULL,&p));
        if (fake_destinations[i]==d.clients[box].channel && p.opcode==DESK_KEY) assert(!p.event.value);
    }
    expect_control_arg=NULL;
    /* A subsequent startup/menu launch is bare even with both stale payload
     * forms populated. A shorter vector must also overwrite its entire tail. */
    d.launch_arg="--panel=sounds"; d.launch_argv[5]="control --panel=sounds";
    desk_menu_open(&d,SH_MENU_PROGRAMS,0,0);
    int row=item_at(&d.sh.menus[0],"DOS Box"); assert(row>=0);
    while (d.sh.menus[0].hot!=row) input_tap(&d,DU_KEY_DOWN);
    input_tap(&d,DU_KEY_ENTER); assert(d.action>=0 && launch_pending(&d)>0); assert_launch_empty(&d);
    d.launch_arg="--panel=sounds"; d.launch_argv[5]="control --panel=sounds";
    const char *command[]={"E:\\GAME\\RUN.COM"}; expect_dosbox[0]=command[0];
    assert(desk_task_launch_args(&d,"dosbox",command,1)>0); assert_launch_empty(&d);
    fail_spawn=EIO;
    assert(desk_task_launch_args(&d,"dosbox",command,1)==-EIO); assert_launch_empty(&d);
    fail_spawn=0; expect_dosbox[0]=NULL;
    assert(desk_task_launch(&d,"dosbox",NULL)>0); assert_launch_empty(&d);
    d.launch_arg="--panel=sounds"; d.launch_argv[5]=command[0];
    assert(desk_task_launch_args(&d,"dosbox",NULL,1)==-EINVAL); assert_launch_empty(&d);
    d.launch_arg="--panel=sounds"; d.launch_argv[5]=command[0];
    assert(desk_task_launch(&d,"missing",NULL)==-ENOENT); assert_launch_empty(&d);
    /* Constructing/navigating settings menus and running service turns cannot
     * launch their selected applet until an explicit Enter/click activates it. */
    for (unsigned i=0;i<DESK_CLIENTS;i++) d.clients[i].awaiting_frame=0;
    controls=control_spawns;
    desk_menu_open(&d,SH_MENU_SETTINGS,0,0); input_tap(&d,DU_KEY_DOWN);
    for (unsigned i=0;i<3;i++) { desk_shell_tick(&d); shell_service(&d); }
    assert(control_spawns==controls && d.sh.levels);
    input_tap(&d,DU_KEY_ENTER); assert(control_spawns==controls+1); assert_launch_empty(&d);
    d.sh.menu_owner=CP_SOUNDS; desk_menu_open(&d,SH_MENU_TRAY_SETTINGS,0,0);
    input_tap(&d,DU_KEY_DOWN); shell_service(&d); assert(control_spawns==controls+1);
    expect_control_arg="--panel=sounds"; input_tap(&d,DU_KEY_ENTER);
    assert(control_spawns==controls+2); assert_launch_empty(&d); expect_control_arg=NULL;
    /* A completed frame is not permission to take input from the DOS window.
     * Pending prompts resume when a user click returns focus to the desktop. */
    for (unsigned i=0;i<DESK_CLIENTS;i++) d.clients[i].awaiting_frame=0;
    shell_service(&d); assert(d.focus==box && !d.sh.autoplay.open && d.sh.autoplay.waiting);
    d.mouse_x=1000; d.mouse_y=100;
    struct ciuki_input_event click={.type=CIUKI_INPUT_BUTTON,.code=CIUKI_BUTTON_LEFT,.value=1};
    desk_input(&d,&click); click.value=0; desk_input(&d,&click); assert(d.focus<0);
    shell_service(&d); autoplay_drain(&d); assert(d.sh.autoplay.open && !d.sh.autoplay.waiting);
    assert(dosbox_spawns==before+5); /* startup, bare startup, command, failure, bare */
    media_available=0; memset(media_volumes,0,sizeof(media_volumes));
    desk_menu_close(&d); desk_render_release(&d); drop_all(&d); fake_reset();
    desk_autoplay_stop(&d); free(d.pixels);
    free(d.sh.wall); free(d.sh.autoplay.workspace);
    puts("PASS F2-76 idle USB launch: exact bare dosbox argv, no unsolicited command or settings launch");
    puts("PASS F2-76 media race: Start, Run and focused DOS retain input; only explicit activation launches Sounds; AutoPlay waits for desktop focus");
    puts("PASS F2-76 launch isolation: startup/menu, Run, short command vector, spawn/lookup/validation failures clear both payload forms");
}
