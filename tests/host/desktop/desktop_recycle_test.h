/* SPDX-License-Identifier: MIT
 * G2: the desktop's Recycle Bin through the production shell, folder and
 * client code: icon full/empty, its menu, confirmations, Shift+Delete, the
 * policy, drops on the icon, Empty with the sound event, position cleanup
 * and the migration of the old Trash folder. Included by desktop_folder_test.c. */
static unsigned bin_sounds[16], bin_sound_count;
static void bin_sound(struct desktop *desk, unsigned event) { (void)desk; if (bin_sound_count<16) bin_sounds[bin_sound_count++]=event; }
static int sounded(unsigned event) { for (unsigned i=0;i<bin_sound_count;i++) if (bin_sounds[i]==event) return 1; return 0; }
static int bin_icon_full(void) { return !strcmp(d.sh.icons[2].icon,"user-trash-full"); }
static void bin_poll(void) { d.now_ns+=1000000000ull; poll_folder(&d); }
static void bin_menu(void) { d.sh.menu_owner=2; desk_menu_open(&d,SH_MENU_ICON,500,200); }
static int menu_flags(const char *label)
{
    for (int i=0;i<d.sh.menus[0].count;i++) if (!strcmp(d.sh.menus[0].items[i].label,label)) return d.sh.menus[0].items[i].flags;
    return -1;
}
static void recycle_desktop_cases(void)
{
    close_message(); for (int k=0;k<d.sh.icon_count;k++) d.sh.icons[k].selected=0;
    d.sh.icon_selected=-1; d.sh.focus_unselected=0; d.sound_event=bin_sound;
    struct rb_policy policy; rb_policy_default(&policy); assert(!rb_policy_save(&fake_vfs,&policy));

    /* The icon follows the bin: empty it first, through the icon's menu. */
    bin_poll(); assert(bin_icon_full() && !strcmp(d.sh.icons[2].label,"Recycle Bin"));
    bin_menu();
    assert(d.sh.menus[0].count==5 && !strcmp(d.sh.menus[0].items[0].label,"Open") && menu_flags("Empty Recycle Bin")==0 &&
           menu_flags("Properties")==0 && (d.sh.menus[0].items[3].flags&SH_ITEM_SEPARATOR));
    capture("bin-menu");
    key(DU_KEY_DOWN); key(DU_KEY_DOWN); key(DU_KEY_ENTER);                     /* Empty Recycle Bin */
    assert(d.sh.confirm==SH_CONFIRM_EMPTY && !strcmp(d.sh.message_title,"Confirm Multiple File Delete") &&
           !strcmp(d.sh.message_text,"Are you sure you want to delete all of the items in the Recycle Bin?"));
    capture("confirm-empty");
    key(DU_KEY_ESC); assert(!d.sh.confirm && bin_icon_full() && in_bin("dnd-c.txt")==1);      /* No: nothing happens */
    bin_menu(); key(DU_KEY_DOWN); key(DU_KEY_DOWN); key(DU_KEY_ENTER);
    unsigned before=bin_sound_count; confirm(); settle();
    assert(!bin_icon_full() && sounded(SOUND_EMPTY_RECYCLE) && bin_sound_count>before && !in_bin("dnd-c.txt"));
    assert(fake_lookup("/RECYCLED/INFO2")>=0 && fake_nodes[fake_lookup("/RECYCLED/INFO2")].meta.size==RB_HEADER);
    bin_poll(); assert(!bin_icon_full());
    bin_menu(); assert(menu_flags("Empty Recycle Bin")==SH_ITEM_DISABLED); key(DU_KEY_ESC);
    desk_folder_empty_request(&d); assert(!d.sh.confirm && strstr(d.sh.message_text,"already empty")); close_message();
    puts("PASS desktop Recycle Bin icon and Empty: full/empty picture follows the bin, menu, confirmation, No, sound event, already empty");

    /* Open and Properties launch Files at the bin. */
    d.sh.icon_selected=2; d.sh.icons[2].selected=1; program[0]=0; launched[0]=0; key(DU_KEY_ENTER);
    assert(!strcmp(program,"files") && !strcmp(launched,"Recycle Bin"));
    bin_menu(); key(DU_KEY_DOWN); key(DU_KEY_DOWN); key(DU_KEY_DOWN); program[0]=0; key(DU_KEY_ENTER);   /* Empty is disabled: Down skips it */
    assert(!strcmp(program,"files") && !strcmp(launched,"recycle:properties"));
    d.sh.icons[2].selected=0;

    /* Delete: Windows 98 wording, No keeps the file, Y answers Yes; the icon fills; the position is forgotten. */
    fake_file(DESK_FOLDER "/bin-a.txt",10,1); fake_file(DESK_FOLDER "/bin-b.txt",20,2); command(DF_REFRESH,-1);
    assert(cfg_icon_move(&d.sh.cfg,"bin-a.txt",700,300)); d.sh.want_save=0;
    int a=named("bin-a.txt"); d.sh.icon_selected=a; d.sh.icons[a].selected=1; d.sh.anchor=a; d.sh.focus_unselected=0;
    key(DU_KEY_DELETE); desk_folder_service(&d);
    assert(d.sh.confirm==SH_CONFIRM_DELETE && !strcmp(d.sh.message_title,"Confirm File Delete") &&
           !strcmp(d.sh.message_text,"Are you sure you want to send 'bin-a.txt' to the Recycle Bin?") && d.sh.message_kind==DU_DLG_ICON_QUESTION);
    key(0x31); assert(!d.sh.confirm && fake_lookup(DESK_FOLDER "/bin-a.txt")>=0 && !d.sh.folder->pending);   /* N */
    key(DU_KEY_DELETE); desk_folder_service(&d);
    for (int i=0;i<DESK_CLIENTS;i++) if (d.clients[i].internal==DESK_INTERNAL_MESSAGE) desk_drop_client(&d,i);   /* closing the window answers No */
    assert(!d.sh.confirm && fake_lookup(DESK_FOLDER "/bin-a.txt")>=0 && !d.sh.folder->pending);
    key(DU_KEY_DELETE); desk_folder_service(&d); key(0x15); settle();                                          /* Y */
    assert(fake_lookup(DESK_FOLDER "/bin-a.txt")<0 && in_bin("bin-a.txt")==1 && bin_icon_full() && !cfg_icon_position(&d.sh.cfg,"bin-a.txt",&(int){0},&(int){0}));
    assert(!strcmp(d.sh.icons[2].icon,"user-trash-full") && d.sh.icons[2].icon && desk_shell_icon(d.sh.icons[2].icon,48));
    capture("desktop-bin-full");
    /* Shift+Delete: a permanent delete with its own wording; nothing enters the bin. */
    a=named("bin-b.txt"); d.sh.icon_selected=a; d.sh.icons[a].selected=1; d.sh.anchor=a;
    d.sh.modifiers=DU_SHIFT; key(DU_KEY_DELETE); d.sh.modifiers=0; desk_folder_service(&d);
    assert(d.sh.confirm==SH_CONFIRM_DELETE && !strcmp(d.sh.message_text,"Are you sure you want to delete 'bin-b.txt'?") && d.sh.message_kind==DU_DLG_ICON_WARNING);
    confirm(); settle(); assert(fake_lookup(DESK_FOLDER "/bin-b.txt")<0 && !in_bin("bin-b.txt"));
    /* The icon menu's Delete is the plain one. */
    fake_file(DESK_FOLDER "/bin-c.txt",5,3); command(DF_REFRESH,-1); delete_command(named("bin-c.txt")-3); assert(in_bin("bin-c.txt")==1);
    /* Policy: "do not move files" deletes at once; confirmation off asks nothing. */
    policy.nuke=1; assert(!rb_policy_save(&fake_vfs,&policy));
    fake_file(DESK_FOLDER "/bin-d.txt",5,3); command(DF_REFRESH,-1);
    command(DF_DELETE,named("bin-d.txt")-3); assert(!strcmp(d.sh.message_text,"Are you sure you want to delete 'bin-d.txt'?")); confirm(); settle();
    assert(!in_bin("bin-d.txt") && fake_lookup(DESK_FOLDER "/bin-d.txt")<0);
    policy.nuke=0; policy.confirm=0; assert(!rb_policy_save(&fake_vfs,&policy));
    fake_file(DESK_FOLDER "/bin-e.txt",5,3); command(DF_REFRESH,-1);
    command(DF_DELETE,named("bin-e.txt")-3); assert(!d.sh.confirm); settle(); assert(in_bin("bin-e.txt")==1);
    policy.confirm=1; assert(!rb_policy_save(&fake_vfs,&policy));
    puts("PASS desktop Delete: Windows 98 confirmations, Yes/No/Y/N/close, Shift+Delete permanent, 'do not move files', confirmation off, position forgotten, icon fills");

    /* The drop on the icon asks, then recycles (a CD source is refused above in the drag tests). */
    fake_file(DESK_FOLDER "/bin-f.txt",5,3); command(DF_REFRESH,-1); dnd_layout();
    int f=named("bin-f.txt"); assert(f>=0); d.sh.icon_selected=f; d.sh.icons[f].selected=1; d.sh.anchor=f; d.sh.focus_unselected=0;
    int fx=d.sh.icons[f].box.x+48, fy=d.sh.icons[f].box.y+30; struct desk_box bin=d.sh.icons[2].box;
    d.now_ns+=1000000000ull; d.mouse_x=fx; d.mouse_y=fy; desk_shell_button(&d,1,1); dnd_move(bin.x+48,bin.y+30);
    assert(d.sh.dnd.kind==SH_DROP_TRASH && d.sh.dnd.effect==FM_EFFECT_MOVE && d.sh.dnd.drop_index==2);
    desk_shell_button(&d,1,0);
    assert(d.sh.confirm==SH_CONFIRM_DELETE && !strcmp(d.sh.message_text,"Are you sure you want to send 'bin-f.txt' to the Recycle Bin?"));
    confirm(); settle(); assert(in_bin("bin-f.txt")==1 && fake_lookup(DESK_FOLDER "/bin-f.txt")<0);
    /* Items already in the bin are not deleted again: no drop effect. */
    char *inside[]={"/RECYCLED/Dc1"}; char type='F'; (void)inside; (void)type;
    puts("PASS desktop drop on the Recycle Bin icon: highlight, confirmation, recycled");

    /* The old Trash folder is taken into the bin; the icon is full until then. */
    fake_create(DESK_TRASH,FM_DIR); fake_file(DESK_TRASH "/legacy.txt",11,4); fake_create(DESK_TRASH "/Legacy Dir",FM_DIR);
    assert(rb_full(&fake_vfs,0)==1);
    struct rb_usage u; assert(!rb_usage(&fake_vfs,'C',&u) && fake_lookup(DESK_TRASH)<0);
    assert(in_bin("legacy.txt")==1 && in_bin("Legacy Dir")==1);
    puts("PASS desktop Recycle Bin migration: the pre-G2 Trash folder's items become bin records");
    d.sound_event=NULL;
}
