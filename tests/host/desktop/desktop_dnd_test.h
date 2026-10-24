/* SPDX-License-Identifier: MIT
 * fx-19: desktop multi-selection, rubber band, keyboard selection and the
 * shell drag session (desktop and Files sources; desktop, folder icon,
 * Trash, Files, WM_DROPFILES and taskbar targets) through the production
 * shell, folder, input and client code. Included by desktop_folder_test.c. */
static void dnd_click(int x, int y, unsigned modifiers, int button)
{
    d.sh.modifiers=modifiers; d.mouse_x=x; d.mouse_y=y; d.now_ns+=1000000000ull;
    desk_shell_button(&d,button,1); desk_shell_button(&d,button,0); d.sh.modifiers=0;
}
static void dnd_at(const char *name, int *x, int *y)
{ int k=named(name); assert(k>=0); *x=d.sh.icons[k].box.x+48; *y=d.sh.icons[k].box.y+30; }
static int dnd_marked(const char *name) { int k=named(name); assert(k>=0); return desk_shell_icon_marked(&d,k); }
static int dnd_count(void) { int n=0; for (int k=0;k<d.sh.icon_count;k++) n+=desk_shell_icon_marked(&d,k); return n; }
static void dnd_move(int x, int y) { d.mouse_x=x; d.mouse_y=y; desk_shell_motion(&d); }
static void dnd_layout(void)
{
    static const char *names[]={"dnd-a.txt","dnd-b.txt","dnd-c.txt","DndFolder"};
    static const int xy[][2]={{300,200},{400,200},{300,300},{600,200}};
    for (unsigned i=0;i<4;i++) assert(cfg_icon_move(&d.sh.cfg,names[i],xy[i][0],xy[i][1]));
    command(DF_REFRESH,-1); desk_folder_changed(&d);
    for (unsigned i=0;i<4;i++) { int k=named(names[i]); assert(k>=0 && d.sh.icons[k].box.x==xy[i][0]); }
}
static void dnd_files(void)
{
    static const char *names[]={DESK_FOLDER "/dnd-a.txt",DESK_FOLDER "/dnd-b.txt",DESK_FOLDER "/dnd-c.txt"};
    for (unsigned i=0;i<3;i++) if (fake_lookup(names[i])<0) fake_file(names[i],10+i,(unsigned char)i);
    if (fake_lookup(DESK_FOLDER "/DndFolder")<0) fake_create(DESK_FOLDER "/DndFolder",FM_DIR);
    dnd_layout();
}
static int last_message(int client, unsigned opcode, struct desk_packet *p)
{
    desk_flush(&d,client);
    for (int i=(int)fake_sent-1;i>=0;i--) if (fake_messages[i].data[1]==opcode) { assert(desk_parse(&fake_messages[i],0,NULL,p)); return 1; }
    return 0;
}
static void fx19_desktop_cases(void)
{
    close_message(); for (int k=0;k<d.sh.icon_count;k++) d.sh.icons[k].selected=0;
    d.sh.icon_selected=-1; d.sh.focus_unselected=0;
    dnd_files();
    int ax,ay,bx,by,cx,cy,fx,fy; dnd_at("dnd-a.txt",&ax,&ay); dnd_at("dnd-b.txt",&bx,&by); dnd_at("dnd-c.txt",&cx,&cy); dnd_at("DndFolder",&fx,&fy);
    /* Click, Ctrl+click adds, Ctrl+click on a selected icon toggles it at release, Shift+click selects the rectangle. */
    dnd_click(ax,ay,0,1); assert(dnd_count()==1 && dnd_marked("dnd-a.txt"));
    dnd_click(bx,by,DU_CTRL,1); assert(dnd_count()==2 && dnd_marked("dnd-b.txt"));
    dnd_click(ax,ay,DU_CTRL,1); assert(dnd_count()==1 && !dnd_marked("dnd-a.txt") && d.sh.icon_selected==named("dnd-a.txt"));
    dnd_click(bx,by,0,1); dnd_click(cx,cy,DU_SHIFT,1);
    assert(dnd_marked("dnd-a.txt") && dnd_marked("dnd-b.txt") && dnd_marked("dnd-c.txt") && !dnd_marked("DndFolder"));
    /* A press on a selected icon keeps the selection; the click applies at release. */
    d.now_ns+=1000000000ull; d.mouse_x=ax; d.mouse_y=ay; desk_shell_button(&d,1,1); assert(dnd_count()==3);
    desk_shell_button(&d,1,0); assert(dnd_count()==1 && dnd_marked("dnd-a.txt"));
    puts("PASS fx-19 desktop selection: click, Ctrl toggle (deferred on a selected icon), Shift rectangle from the anchor, press keeps the selection");

    /* Rubber band from empty space: plain replaces, Ctrl toggles, Shift adds. */
    d.now_ns+=1000000000ull; d.mouse_x=290; d.mouse_y=470; desk_shell_button(&d,1,1); assert(d.sh.band && dnd_count()==0);
    dnd_move(300,350); dnd_move(350,250);
    assert(dnd_marked("dnd-a.txt") && dnd_marked("dnd-c.txt") && !dnd_marked("dnd-b.txt") && !dnd_marked("DndFolder"));
    capture("desktop-fx19-marquee"); cache_equivalent();
    desk_shell_button(&d,1,0); assert(!d.sh.band && dnd_count()==2);
    d.sh.modifiers=DU_CTRL; d.now_ns+=1000000000ull; d.mouse_x=290; d.mouse_y=470; desk_shell_button(&d,1,1); dnd_move(350,250); desk_shell_button(&d,1,0);
    assert(dnd_count()==0);
    dnd_click(bx,by,0,1);
    d.sh.modifiers=DU_SHIFT; d.now_ns+=1000000000ull; d.mouse_x=290; d.mouse_y=470; desk_shell_button(&d,1,1); dnd_move(350,250); desk_shell_button(&d,1,0);
    d.sh.modifiers=0; assert(dnd_count()==3 && dnd_marked("dnd-b.txt"));
    dnd_click(700,500,0,1); assert(dnd_count()==0);
    puts("PASS fx-19 desktop rubber band: dotted XOR band, plain/Ctrl toggle/Shift add, empty click clears, cached layer matches a fresh render");

    /* Keyboard: Ctrl+A, arrows, Shift+arrows, Ctrl+arrows and Ctrl+Space. */
    d.sh.icon_selected=named("dnd-a.txt"); d.sh.focus_unselected=0; d.sh.anchor=d.sh.icon_selected;
    desk_shell_key(&d,0x1d,1); key(0x1e); desk_shell_key(&d,0x1d,0); assert(dnd_count()==d.sh.icon_count);
    key(DU_KEY_RIGHT); assert(dnd_count()==1 && dnd_marked("dnd-b.txt"));
    desk_shell_key(&d,0x2a,1); key(DU_KEY_LEFT); desk_shell_key(&d,0x2a,0); assert(dnd_marked("dnd-a.txt") && dnd_marked("dnd-b.txt") && dnd_count()==2);
    desk_shell_key(&d,0x1d,1); key(DU_KEY_DOWN); assert(d.sh.icon_selected==named("dnd-c.txt") && !dnd_marked("dnd-c.txt"));
    key(DU_KEY_SPACE); desk_shell_key(&d,0x1d,0); assert(dnd_marked("dnd-c.txt") && dnd_count()==3);
    puts("PASS fx-19 desktop keyboard selection: Ctrl+A, arrows, Shift+arrows, Ctrl+arrows, Ctrl+Space");

    /* Delete acts on the whole selection: one Recycle Bin job, one confirmation. */
    dnd_click(ax,ay,0,1); dnd_click(bx,by,DU_CTRL,1); key(DU_KEY_DELETE); desk_folder_service(&d);
    assert(!strcmp(d.sh.message_title,"Confirm Multiple File Delete") && strstr(d.sh.message_text,"send these 2 items to the Recycle Bin")); confirm(); settle();
    assert(fake_lookup(DESK_FOLDER "/dnd-a.txt")<0 && fake_lookup(DESK_FOLDER "/dnd-b.txt")<0);
    assert(in_bin("dnd-a.txt")==1 && in_bin("dnd-b.txt")==1);
    puts("PASS fx-19 desktop multi-item Delete: one Recycle Bin job and one confirmation for the whole selection");

    /* Drag a multi-selection onto a folder icon: same drive moves; Ctrl copies; the image and cursor follow. */
    dnd_files(); dnd_at("dnd-a.txt",&ax,&ay); dnd_at("dnd-b.txt",&bx,&by); dnd_at("DndFolder",&fx,&fy);
    dnd_click(ax,ay,0,1); dnd_click(bx,by,DU_CTRL,1);
    d.now_ns+=1000000000ull; d.mouse_x=ax; d.mouse_y=ay; desk_shell_button(&d,1,1);
    dnd_move(ax+3,ay+3); assert(!d.sh.dnd.active);
    dnd_move(fx,fy); assert(d.sh.dnd.active && d.sh.dnd.source<0 && d.sh.dnd.count==2 && d.sh.dnd.kind==SH_DROP_ICON);
    desk_cursor_update(&d); assert(d.sh.dnd.effect==FM_EFFECT_MOVE && d.cursor_shape==DESK_CURSOR_DRAG_MOVE && d.sh.dnd.drop_index==named("DndFolder"));
    capture("desktop-fx19-drag");
    desk_shell_key(&d,0x1d,1); desk_cursor_update(&d); assert(d.sh.dnd.effect==FM_EFFECT_COPY && d.cursor_shape==DESK_CURSOR_DRAG_COPY);
    desk_shell_key(&d,0x1d,0); desk_shell_key(&d,0x38,1); desk_cursor_update(&d);
    assert(d.sh.dnd.effect==FM_EFFECT_LINK && d.cursor_shape==DESK_CURSOR_DRAG_LINK);   /* G8 native shortcuts */
    desk_shell_key(&d,0x38,0);
    {   char **paths; char *types; unsigned n; assert(!fm_drag_load(&fake_vfs,&paths,&types,&n) && n==2 && types[0]=='F'); fm_drag_free(paths,types,n); }
    desk_shell_button(&d,1,0); assert(!d.sh.dnd.active); settle();
    assert(fake_lookup(DESK_FOLDER "/DndFolder/dnd-a.txt")>=0 && fake_lookup(DESK_FOLDER "/DndFolder/dnd-b.txt")>=0 &&
           fake_lookup(DESK_FOLDER "/dnd-a.txt")<0);
    /* Onto the Trash icon: delete (move to Trash); Ctrl is refused there. */
    dnd_files(); dnd_at("dnd-c.txt",&cx,&cy); dnd_click(cx,cy,0,1);
    struct desk_box trash=d.sh.icons[2].box;
    d.now_ns+=1000000000ull; d.mouse_x=cx; d.mouse_y=cy; desk_shell_button(&d,1,1); dnd_move(trash.x+48,trash.y+30);
    assert(d.sh.dnd.kind==SH_DROP_TRASH && d.sh.dnd.effect==FM_EFFECT_MOVE);
    desk_shell_key(&d,0x1d,1); assert(d.sh.dnd.effect==FM_EFFECT_NONE); desk_shell_key(&d,0x1d,0);
    desk_shell_button(&d,1,0); assert(fake_lookup(DESK_FOLDER "/dnd-c.txt")>=0 && d.sh.confirm); confirm(); settle();
    assert(fake_lookup(DESK_FOLDER "/dnd-c.txt")<0 && in_bin("dnd-c.txt")==1);
    puts("PASS fx-19 desktop drag: SM_CXDRAG, multi-item move into a folder icon, Ctrl copy cursor, Alt/link cursor, Recycle Bin target, data object");

    /* Free space repositions the dragged icons by the pointer offset; Esc cancels. */
    dnd_files(); dnd_at("dnd-a.txt",&ax,&ay); dnd_at("dnd-b.txt",&bx,&by);
    dnd_click(ax,ay,0,1); dnd_click(bx,by,DU_CTRL,1);
    d.now_ns+=1000000000ull; d.mouse_x=ax; d.mouse_y=ay; desk_shell_button(&d,1,1); dnd_move(ax+40,ay+260);
    assert(d.sh.dnd.kind==SH_DROP_DESKTOP && d.sh.dnd.effect==FM_EFFECT_MOVE);
    key(DU_KEY_ESC); assert(!d.sh.dnd.active); desk_shell_button(&d,1,0);
    assert(d.sh.icons[named("dnd-a.txt")].box.x==300);
    d.now_ns+=1000000000ull; d.mouse_x=ax; d.mouse_y=ay; desk_shell_button(&d,1,1); dnd_move(ax+40,ay+260); desk_shell_button(&d,1,0);
    int x,y; assert(cfg_icon_position(&d.sh.cfg,"dnd-a.txt",&x,&y) && x==340 && y==460);
    assert(cfg_icon_position(&d.sh.cfg,"dnd-b.txt",&x,&y) && x==440 && y==460);
    /* Ctrl over free space copies beside the originals (Copy of). */
    dnd_layout(); dnd_at("dnd-a.txt",&ax,&ay); dnd_click(ax,ay,0,1);
    d.now_ns+=1000000000ull; d.mouse_x=ax; d.mouse_y=ay; desk_shell_button(&d,1,1); dnd_move(ax+300,ay+250);
    desk_shell_key(&d,0x1d,1); assert(d.sh.dnd.effect==FM_EFFECT_COPY); desk_shell_button(&d,1,0); desk_shell_key(&d,0x1d,0); settle();
    assert(fake_lookup(DESK_FOLDER "/Copy of dnd-a.txt")>=0);
    puts("PASS fx-19 desktop reposition: offsets kept and saved, Esc cancels, Ctrl copies beside the originals");

    /* Right-drag ends in the Move/Copy/Shortcut/Cancel menu; the default is the plain effect. */
    dnd_files(); dnd_at("dnd-c.txt",&bx,&by); dnd_at("DndFolder",&fx,&fy);
    d.now_ns+=1000000000ull; d.mouse_x=bx; d.mouse_y=by; desk_shell_button(&d,CIUKI_BUTTON_RIGHT,1); assert(!d.sh.levels);
    dnd_move(fx,fy); assert(d.sh.dnd.active && d.sh.dnd.button==CIUKI_BUTTON_RIGHT);
    desk_shell_button(&d,CIUKI_BUTTON_RIGHT,0);
    assert(d.sh.dnd.menu && d.sh.levels==1 && d.sh.menus[0].kind==SH_MENU_DRAG && d.sh.menus[0].count==5);
    assert(!strcmp(d.sh.menus[0].items[0].label,"Move Here") && (d.sh.menus[0].items[0].flags&SH_ITEM_DEFAULT));
    assert(!strcmp(d.sh.menus[0].items[1].label,"Copy Here") && !(d.sh.menus[0].items[1].flags&SH_ITEM_DISABLED));
    assert(!(d.sh.menus[0].items[2].flags&SH_ITEM_DISABLED));
    capture("desktop-fx19-drag-menu");
    key(DU_KEY_DOWN); key(DU_KEY_DOWN); key(DU_KEY_ENTER);
    assert(!d.sh.dnd.active); settle();
    assert(fake_lookup(DESK_FOLDER "/DndFolder/dnd-c.txt")>=0 && fake_lookup(DESK_FOLDER "/dnd-c.txt")>=0);
    /* Escape on the menu is Cancel; a right click without a drag shows the icon menu at release. */
    dnd_layout(); dnd_at("dnd-b.txt",&bx,&by); dnd_at("DndFolder",&fx,&fy);
    d.now_ns+=1000000000ull; d.mouse_x=bx; d.mouse_y=by; desk_shell_button(&d,CIUKI_BUTTON_RIGHT,1); dnd_move(fx,fy);
    desk_shell_button(&d,CIUKI_BUTTON_RIGHT,0); key(DU_KEY_ESC); assert(!d.sh.dnd.active && !d.sh.levels);
    dnd_click(bx,by,0,CIUKI_BUTTON_RIGHT); assert(d.sh.levels==1 && d.sh.menus[0].kind==SH_MENU_ICON); key(DU_KEY_ESC);
    puts("PASS fx-19 desktop right-drag: menu with bold default, Copy Here, Shortcut enabled, Esc cancel, release-time icon menu");

    /* Desktop to a Files window: DRAG_OVER with client coordinates, the target's DRAG_STATUS sets the cursor, DRAG_DROP carries the effect. */
    fake_channel(30); int files=desk_add_client(&d,30); assert(files>=0);
    struct desk_client *c=&d.clients[files]; c->hello=1; c->window=1; c->version=2; c->minor=1; c->pid=77;
    c->program=DESK_APP_FILES; c->frame=(struct desk_box){560,380,400,300}; strcpy(c->title,"Files");
    struct desk_box content=desk_content(c);
    dnd_layout(); dnd_at("dnd-a.txt",&ax,&ay); dnd_click(ax,ay,0,1);
    d.now_ns+=1000000000ull; d.mouse_x=ax; d.mouse_y=ay; desk_shell_button(&d,1,1); dnd_move(content.x+50,content.y+60);
    struct desk_packet p; assert(d.sh.dnd.kind==SH_DROP_FILES && last_message(files,DESK_DRAG_OVER,&p));
    assert(p.serial==d.sh.dnd.serial && p.flags==0 && p.x==50 && p.y==60 && p.index==0);
    desk_cursor_update(&d); assert(d.cursor_shape==DESK_CURSOR_FORBIDDEN);   /* until the target answers */
    struct ciuki_message msg; desk_message(&msg,DESK_DRAG_STATUS,16); msg.data[0]=2; msg.data[2]=1; msg.sender_pid=77;
    desk_put32(msg.data+4,d.sh.dnd.serial); desk_put32(msg.data+8,FM_EFFECT_MOVE); desk_put32(msg.data+12,FM_EFFECT_COPY|FM_EFFECT_MOVE);
    desk_receive(&d,files,&msg); desk_cursor_update(&d); assert(d.sh.dnd.effect==FM_EFFECT_MOVE && d.cursor_shape==DESK_CURSOR_DRAG_MOVE);
    d.now_ns+=150000000ull; unsigned sent=fake_sent; desk_shell_tick(&d); desk_flush(&d,files); assert(fake_sent>sent);   /* DragOver repeat */
    desk_shell_button(&d,1,0); assert(last_message(files,DESK_DRAG_DROP,&p) && p.flags==FM_EFFECT_MOVE && p.x==50 && p.y==60);
    /* Leaving a Files target sends the leave phase. */
    dnd_click(ax,ay,0,1); d.now_ns+=1000000000ull; d.mouse_x=ax; d.mouse_y=ay; desk_shell_button(&d,1,1);
    dnd_move(content.x+50,content.y+60); dnd_move(700,300); assert(last_message(files,DESK_DRAG_OVER,&p) && p.flags==1);
    key(DU_KEY_ESC);
    puts("PASS fx-19 desktop to Files: DRAG_OVER client coordinates, DRAG_STATUS effect and cursor, 100 ms repeat, DRAG_DROP effect, leave");

    /* Files window as the source: DRAG_BEGIN only with the press routed to it; the shell owns the pointer; drop on the desktop. */
    fake_file("/Documents/FromFiles.txt",12,5);
    { char *paths[]={"/Documents/FromFiles.txt"}; char type='F'; assert(!fm_drag_save(&fake_vfs,paths,&type,1)); }
    desk_message(&msg,DESK_DRAG_BEGIN,12); msg.data[0]=2; msg.data[2]=1; msg.sender_pid=77;
    desk_put32(msg.data+4,1); desk_put32(msg.data+8,1);
    d.focus=-1; d.routed_buttons=0; desk_receive(&d,files,&msg); assert(!d.sh.dnd.active);
    d.focus=files; d.routed_buttons=1; d.buttons=1; d.mouse_x=content.x+20; d.mouse_y=content.y+20;
    desk_receive(&d,files,&msg); assert(d.sh.dnd.active && d.sh.dnd.source==files);
    struct ciuki_input_event motion={.type=CIUKI_INPUT_MOTION,.value=700-d.mouse_x,.value2=320-d.mouse_y};
    sent=fake_sent; desk_input(&d,&motion); desk_flush(&d,files);
    for (unsigned i=sent;i<fake_sent;i++) assert(fake_messages[i].data[1]!=DESK_MOTION);
    assert(d.sh.dnd.kind==SH_DROP_DESKTOP && d.sh.dnd.effect==FM_EFFECT_MOVE && d.cursor_shape==DESK_CURSOR_DRAG_MOVE);
    struct ciuki_input_event up={.type=CIUKI_INPUT_BUTTON,.code=CIUKI_BUTTON_LEFT,.value=0};
    sent=fake_sent; desk_input(&d,&up); desk_flush(&d,files);
    int ended=0, routed=0;
    for (unsigned i=sent;i<fake_sent;i++) {
        if (fake_messages[i].data[1]==DESK_DRAG_END) { assert(desk_parse(&fake_messages[i],0,NULL,&p) && p.flags==FM_EFFECT_MOVE); ended=1; }
        if (fake_messages[i].data[1]==DESK_BUTTON) routed=1;
    }
    assert(ended && routed && !d.sh.dnd.active); settle();
    assert(fake_lookup(DESK_FOLDER "/FromFiles.txt")>=0 && fake_lookup("/Documents/FromFiles.txt")<0);
    assert(cfg_icon_position(&d.sh.cfg,"FromFiles.txt",&x,&y) && x==700-48 && y==320-24);
    puts("PASS fx-19 Files source: DRAG_BEGIN authority, shell-owned pointer (no client motion), desktop drop at the pointer, DRAG_END effect and routed release");

    /* WM_DROPFILES applications: ACCEPT_FILES, the copy cursor, DROP_FILES and activation. */
    fake_channel(31); int note=desk_add_client(&d,31); assert(note>=0);
    struct desk_client *n=&d.clients[note]; n->hello=1; n->window=1; n->version=2; n->minor=1; n->pid=78; n->frame=(struct desk_box){120,420,300,200};
    strcpy(n->title,"CiukNote");
    desk_message(&msg,DESK_ACCEPT_FILES,8); msg.data[0]=2; msg.data[2]=1; msg.sender_pid=78; desk_put32(msg.data+4,1); desk_receive(&d,note,&msg);
    assert(n->accept_files);
    struct desk_box nc=desk_content(n);
    dnd_layout(); dnd_at("dnd-a.txt",&ax,&ay); dnd_click(ax,ay,0,1);
    d.now_ns+=1000000000ull; d.mouse_x=ax; d.mouse_y=ay; desk_shell_button(&d,1,1); dnd_move(nc.x+10,nc.y+15);
    desk_cursor_update(&d); assert(d.sh.dnd.kind==SH_DROP_APP && d.cursor_shape==DESK_CURSOR_DRAG_COPY);
    desk_shell_button(&d,1,0); assert(last_message(note,DESK_DROP_FILES,&p) && p.x==10 && p.y==15 && d.focus==note);
    assert(fake_lookup(DESK_FOLDER "/dnd-a.txt")>=0);   /* the application opens it; nothing moves */
    puts("PASS fx-19 WM_DROPFILES target: DragAcceptFiles, copy cursor, DROP_FILES client coordinates, activation, no transfer");

    /* Taskbar: resting on a button brings its window forward; dropping there explains why it cannot. */
    desk_minimize(&d,note); assert(d.clients[note].minimized);
    struct desk_box button=desk_task_button(&d,note); assert(button.w);
    dnd_click(ax,ay,0,1); d.now_ns+=1000000000ull; d.mouse_x=ax; d.mouse_y=ay; desk_shell_button(&d,1,1);
    dnd_move(button.x+10,button.y+button.h/2); assert(d.sh.dnd.kind==SH_DROP_TASK && d.sh.dnd.hover_task==note);
    d.now_ns+=1100000000ull; desk_shell_tick(&d); assert(!d.clients[note].minimized && d.focus==note);
    desk_shell_button(&d,1,0); assert(strstr(d.sh.message_text,"button on the taskbar")); close_message();
    puts("PASS fx-19 taskbar during a drag: hover restores the window after one second, a drop explains the rule");

    /* Strict packet shapes for the new opcodes. */
    desk_message(&msg,DESK_DRAG_BEGIN,12); msg.data[0]=2; desk_put32(msg.data+4,3); desk_put32(msg.data+8,1); assert(!desk_parse(&msg,1,NULL,&p));
    desk_put32(msg.data+4,1); desk_put32(msg.data+8,0); assert(!desk_parse(&msg,1,NULL,&p));
    desk_put32(msg.data+8,4097); assert(!desk_parse(&msg,1,NULL,&p)); desk_put32(msg.data+8,2); assert(desk_parse(&msg,1,NULL,&p) && !desk_parse(&msg,0,NULL,&p));
    desk_message(&msg,DESK_DRAG_STATUS,16); msg.data[0]=2; desk_put32(msg.data+4,5); desk_put32(msg.data+8,3); assert(!desk_parse(&msg,1,NULL,&p));
    desk_put32(msg.data+8,2); desk_put32(msg.data+12,1); assert(!desk_parse(&msg,1,NULL,&p));
    desk_put32(msg.data+12,3); assert(desk_parse(&msg,1,NULL,&p));
    desk_message(&msg,DESK_ACCEPT_FILES,8); msg.data[0]=2; desk_put32(msg.data+4,2); assert(!desk_parse(&msg,1,NULL,&p));
    desk_message(&msg,DESK_DRAG_DROP,20); msg.data[0]=2; desk_put32(msg.data+4,1); desk_put32(msg.data+16,0); assert(!desk_parse(&msg,0,NULL,&p));
    desk_put32(msg.data+16,1); assert(desk_parse(&msg,0,NULL,&p) && !desk_parse(&msg,1,NULL,&p));
    desk_drop_client(&d,note); desk_drop_client(&d,files);
    puts("PASS fx-19 protocol: DRAG_BEGIN/STATUS/DROP/ACCEPT_FILES shapes, direction and ranges");
}
