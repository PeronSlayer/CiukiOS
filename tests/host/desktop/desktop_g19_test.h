/* SPDX-License-Identifier: MIT
 * G19 continuation through the real shell, VFS, wizard and drag session. */
static int g19_external_count;
static struct desk_launch_record g19_external_record;
static int g19_external(struct desktop *desktop, const struct desk_launch_record *record)
{ (void)desktop; g19_external_count++; g19_external_record=*record; return 0; }
/* An installed G26C resolver remains authoritative even when it is busy. */
static int g19_busy_path(struct desktop *desktop, const char *path)
{
    (void)desktop; assert(!strcmp(path,"https://example.com/")); return -EBUSY;
}
static void g19_link(const char *name, const char *target)
{
    static struct desk_launch_record record;
    memset(&record,0,sizeof(record)); record.show=1;
    snprintf(record.label,sizeof(record.label),"%.63s",name);
    snprintf(record.target,sizeof(record.target),"%s",target);
    assert(!desk_folder_link_create(&d,DESK_FOLDER,name,&record));
}
static void g19_open(const char *name, const char *expected_program, const char *argument)
{
    int k=named(name); assert(k>=3); d.focus=-1; d.sh.icon_selected=k;
    for (int i=0;i<d.sh.icon_count;i++) d.sh.icons[i].selected=0;
    key(DU_KEY_ENTER);
    if (strcmp(program,expected_program) || strcmp(launched,argument))
        fprintf(stderr,"G19 open %s: got %s [%s], expected %s [%s]\n",name,program,launched,expected_program,argument);
    assert(!strcmp(program,expected_program) && !strcmp(launched,argument));
}
static void g19_context(int index, const char *item)
{
    d.sh.menu_owner=index; desk_menu_open(&d,SH_MENU_ICON,500,200);
    int row=-1; for (int i=0;i<d.sh.menus[0].count;i++) if (!strcmp(d.sh.menus[0].items[i].label,item)) row=i;
    assert(row>=0); d.sh.menus[0].hot=row; key(DU_KEY_ENTER);
}
static void g19_cases(void)
{
    close_message(); d.focus=-1;
    strcpy(d.programs[4].name,"control"); strcpy(d.programs[4].path,"/Programs/control");
    strcpy(d.programs[5].name,"netsurf"); strcpy(d.programs[5].path,"/Programs/netsurf"); d.program_count=6;
    command(DF_SHORTCUT,-1); assert(d.sh.folder->wizard==1); capture("g19-shortcut-target");
    du_clipboard_set("/Documents"); key(DU_KEY_LCTRL); /* press/release alone leaves no modifier */
    desk_shell_key(&d,DU_KEY_LCTRL,1); key(0x2f); desk_shell_key(&d,DU_KEY_LCTRL,0);
    assert(!strcmp(d.sh.folder->shortcut_target,"/Documents"));
    desk_shell_key(&d,DU_KEY_LCTRL,1); key(0x1e); desk_shell_key(&d,DU_KEY_LCTRL,0); key(DU_KEY_BACKSPACE);
    assert(!d.sh.folder->shortcut_target[0]);
    int window=d.sh.folder->wizard_window; assert(window>=0);
    desk_request_close(&d,window); assert(!d.sh.folder->wizard);
    /* Browse can enter drives and select folders without creating an object. */
    command(DF_SHORTCUT,-1); strcpy(d.sh.folder->shortcut_target,"/missing-g19-target");
    desk_folder_shortcut_action(&d,3); assert(d.sh.folder->wizard==1 && d.sh.folder->wizard_error[0]);
    desk_folder_shortcut_action(&d,1); assert(d.sh.folder->wizard==3);
    assert(d.sh.folder->picker && !d.sh.folder->picker->path[0] && d.sh.folder->picker->count); capture("g19-shortcut-browse");
    d.sh.folder->wizard_focus=0; strcpy(d.sh.folder->picker_path,"/Documents"); desk_folder_shortcut_action(&d,3);
    assert(!strcmp(d.sh.folder->picker->path,"/Documents"));
    assert(fm_select_name(d.sh.folder->picker,"Desktop",0)>=0); d.sh.folder->wizard_focus=1;
    desk_folder_shortcut_action(&d,3); assert(d.sh.folder->wizard==1 && !strcmp(d.sh.folder->shortcut_target,DESK_FOLDER));
    desk_folder_shortcut_action(&d,3); assert(d.sh.folder->wizard==2);
    capture("g19-shortcut-name"); strcpy(d.sh.folder->shortcut_name,"bad:name"); desk_folder_shortcut_action(&d,3); assert(d.sh.folder->wizard_error[0] && !d.sh.folder->command);
    strcpy(d.sh.folder->shortcut_name,"G19 folder"); desk_folder_shortcut_action(&d,3); desk_folder_service(&d);
    assert(!d.sh.folder->wizard && fake_lookup(DESK_FOLDER "/G19 folder.lnk")>=0);
    g19_open("G19 folder.lnk","files",DESK_FOLDER);
    command(DF_SHORTCUT,-1); strcpy(d.sh.folder->shortcut_target,"https://example.com/"); desk_folder_shortcut_action(&d,3);
    strcpy(d.sh.folder->shortcut_name,"G19 web"); desk_folder_shortcut_action(&d,3); desk_folder_service(&d);
    g19_open("G19 web.lnk","netsurf","https://example.com/");
    static struct desk_launch_record web;
    assert(!desk_folder_link_read(&d,DESK_FOLDER "/G19 web.lnk",&web));
    d.open_path=g19_busy_path; program[0]=launched[0]=0;
    assert(desk_shell_open_record(&d,&web,0)==-EBUSY && !program[0] && !launched[0]);
    d.open_path=NULL;
    /* Closing after Finish but before service cancels the queued create. */
    command(DF_SHORTCUT,-1); strcpy(d.sh.folder->shortcut_target,"/Documents"); desk_folder_shortcut_action(&d,3);
    strcpy(d.sh.folder->shortcut_name,"G19 cancelled"); desk_folder_shortcut_action(&d,3); desk_folder_shortcut_close(&d); desk_folder_service(&d);
    assert(fake_lookup(DESK_FOLDER "/G19 cancelled.lnk")<0);
    /* Duplicate Finish stays on the name page, preserving the existing link. */
    command(DF_SHORTCUT,-1); strcpy(d.sh.folder->shortcut_target,"/Documents"); desk_folder_shortcut_action(&d,3);
    strcpy(d.sh.folder->shortcut_name,"G19 web"); desk_folder_shortcut_action(&d,3); desk_folder_service(&d);
    assert(d.sh.folder->wizard==2 && d.sh.folder->wizard_error[0]); desk_folder_shortcut_close(&d);
    if (fake_lookup(DESK_FOLDER "/g19.txt")<0) fake_file(DESK_FOLDER "/g19.txt",11,7);
    if (fake_lookup(DESK_FOLDER "/g19.bmp")<0) fake_file(DESK_FOLDER "/g19.bmp",11,7);
    g19_link("G19 document.lnk",DESK_FOLDER "/g19.txt"); g19_link("G19 image.lnk",DESK_FOLDER "/g19.bmp");
    g19_link("G19 drive.lnk","C:\\"); g19_link("G19 native.lnk","/Programs/paint");
    g19_link("G19 nested.lnk",DESK_FOLDER "/G19 document.lnk");
    fake_short_io=1; command(DF_REFRESH,-1);
    g19_open("G19 document.lnk","ciuknote",DESK_FOLDER "/g19.txt");
    g19_open("G19 image.lnk","viewer",DESK_FOLDER "/g19.bmp");
    g19_open("G19 drive.lnk","files","/"); g19_open("G19 native.lnk","paint","");
    g19_open("G19 nested.lnk","ciuknote",DESK_FOLDER "/g19.txt"); fake_short_io=0;
    d.v3_launch=g19_external;
    const char *extensions[]={"exe","com","bat","pif"};
    for (unsigned i=0;i<4;i++) {
        char target[128],link[64]; snprintf(target,sizeof(target),DESK_FOLDER "/g19.%s",extensions[i]);
        snprintf(link,sizeof(link),"G19 %s.lnk",extensions[i]); fake_file(target,20,0); g19_link(link,target); command(DF_REFRESH,-1);
        int before=g19_external_count; for (int k=0;k<d.sh.icon_count;k++) d.sh.icons[k].selected=0; d.sh.focus_unselected=0; d.sh.icon_selected=named(link); d.focus=-1; key(DU_KEY_ENTER);
        assert(g19_external_count==before+1 && !strcmp(g19_external_record.target,target));
    }
    fake_file(DESK_FOLDER "/native-no-extension",8,0);
    int elf=fake_lookup(DESK_FOLDER "/native-no-extension"); memcpy(fake_nodes[elf].data,"\177ELF",4);
    g19_link("G19 ELF.lnk",DESK_FOLDER "/native-no-extension"); command(DF_REFRESH,-1);
    int prior=g19_external_count; for (int k=0;k<d.sh.icon_count;k++) d.sh.icons[k].selected=0; d.sh.focus_unselected=0; d.sh.icon_selected=named("G19 ELF.lnk"); d.focus=-1; key(DU_KEY_ENTER);
    assert(g19_external_count==prior+1 && !strcmp(g19_external_record.target,DESK_FOLDER "/native-no-extension"));
    fake_file("/mnt/e/g19.exe",8,0); memset(&g19_external_record,0,sizeof(g19_external_record));
    static struct desk_launch_record drive_record;
    drive_record.show=1; strcpy(drive_record.label,"Drive program"); strcpy(drive_record.target,"E:\\g19.exe");
    assert(!desk_shell_open_record(&d,&drive_record,0) && !strcmp(g19_external_record.target,"/mnt/e/g19.exe"));
    g19_link("G19 loop.lnk",DESK_FOLDER "/G19 loop.lnk");
    static struct desk_launch_record record;
    assert(!desk_folder_link_read(&d,DESK_FOLDER "/G19 loop.lnk",&record)); assert(desk_shell_open_record(&d,&record,0)<0);
    strcpy(record.target,DESK_FOLDER "/missing.txt"); assert(desk_shell_open_record(&d,&record,0)<0);
    assert(!desk_folder_link_read(&d,DESK_FOLDER "/G19 document.lnk",&record));
    int node=fake_lookup(DESK_FOLDER "/G19 document.lnk"); assert(node>=0);
    fake_nodes[node].data[0]='X'; assert(desk_folder_link_read(&d,DESK_FOLDER "/G19 document.lnk",&record)<0);
    /* Stable system identities survive user label overrides. */
    g19_context(0,"Properties"); assert(!strcmp(program,"control") && !strcmp(launched,"--panel=system"));
    g19_context(1,"Properties"); assert(!strcmp(program,"files") && !strcmp(launched,"--properties=/Documents"));
    assert(cfg_icon_label_set(&d.sh.cfg,"/Trash","My bin")); desk_folder_changed(&d);
    g19_context(2,"Properties"); assert(!strcmp(program,"files") && !strcmp(launched,"recycle:properties"));
    int network=named("Network Neighborhood"); assert(network>=3);
    g19_context(network,"Properties"); assert(!strcmp(program,"control") && !strcmp(launched,"--panel=network"));
    g19_context(network,"Open"); assert(!strcmp(program,"control"));
    /* Hide and show from the actual background menu, with a persisted flag. */
    for (unsigned visible=0;visible<2;visible++) {
        desk_menu_open(&d,SH_MENU_DESKTOP,500,200); int row=-1;
        for (int i=0;i<d.sh.menus[0].count;i++) if (d.sh.menus[0].items[i].action==SH_ACT_ICONS_SHOW) row=i;
        assert(row>=0); d.sh.menus[0].hot=row; key(DU_KEY_ENTER); assert(d.sh.cfg.desktop_icons==(int)visible); cache_equivalent();
    }
    /* Link drops create references and leave source bytes unchanged. */
    char *paths[]={DESK_FOLDER "/g19.txt"};
    assert(!desk_folder_transfer(&d,paths,1,DESK_FOLDER,FM_EFFECT_LINK,0,1,300,300));
    assert(fake_lookup(DESK_FOLDER "/Shortcut to g19.txt.lnk")>=0 && fake_lookup(paths[0])>=0);
    assert(!desk_folder_link_read(&d,DESK_FOLDER "/Shortcut to g19.txt.lnk",&record) && !strcmp(record.target,paths[0]));
    /* Drop a document onto an executable: launch with quoted arguments, no move. */
    assert(cfg_icon_move_at(&d.sh.cfg,"g19.txt",d.width,d.height,300,200));
    assert(cfg_icon_move_at(&d.sh.cfg,"g19.exe",d.width,d.height,600,200)); desk_shell_apply(&d);
    int ax,ay,bx,by; dnd_at("g19.txt",&ax,&ay); dnd_at("g19.exe",&bx,&by);
    dnd_click(ax,ay,0,1); d.now_ns+=1000000000ull; d.mouse_x=ax; d.mouse_y=ay; desk_shell_button(&d,1,1); dnd_move(bx,by);
    assert(d.sh.dnd.kind==SH_DROP_PROGRAM && d.sh.dnd.effect==FM_EFFECT_COPY);
    int before=g19_external_count; desk_shell_button(&d,1,0);
    assert(g19_external_count==before+1 && strstr(g19_external_record.arguments,"g19.txt") && fake_lookup(paths[0])>=0);
    d.v3_launch=NULL; cfg_icon_label_forget(&d.sh.cfg,"/Trash"); desk_folder_changed(&d);
    puts("PASS G19 continuation: wizard/Browse/cancel/errors, target classes/chains, properties after rename, visibility, link and executable drops");
}
