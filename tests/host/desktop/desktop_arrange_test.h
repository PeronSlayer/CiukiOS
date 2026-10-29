/* SPDX-License-Identifier: MIT
 * fx-desk-arrange: real menus, filesystem operations, input and settings reload. */
static void arrange_pick(int action, int argument)
{
    desk_menu_open(&d,SH_MENU_DESKTOP,500,200);
    int row=-1;
    for (int i=0;i<d.sh.menus[0].count;i++) if (d.sh.menus[0].items[i].arg==SH_MENU_ARRANGE && d.sh.menus[0].items[i].action==SH_ACT_SUBMENU) row=i;
    assert(row>=0); d.sh.menus[0].hot=row; key(DU_KEY_ENTER);
    assert(d.sh.levels==2 && d.sh.menus[1].kind==SH_MENU_ARRANGE);
    row=-1;
    for (int i=0;i<d.sh.menus[1].count;i++) if (d.sh.menus[1].items[i].action==action && d.sh.menus[1].items[i].arg==argument) row=i;
    assert(row>=0); d.sh.menus[1].hot=row; key(DU_KEY_ENTER);
}
static int arrange_before(const char *a, const char *b)
{
    int x=named(a),y=named(b); assert(x>=3 && y>=3);
    struct desk_box p=d.sh.icons[x].box,q=d.sh.icons[y].box;
    return p.x<q.x || (p.x==q.x && p.y<q.y);
}
static void arrange_grid(void)
{
    int rows=((int)d.height-DESK_BAR-4-(DESK_BAR+10))/d.sh.cfg.icon_spacing_y;
    if (rows<1) rows=1;
    for (int i=0;i<d.sh.icon_count;i++) {
        struct desk_box b=d.sh.icons[i].box;
        assert(b.x>=8 && b.y>=DESK_BAR+10 && (b.x-8)%b.w==0 && (b.y-DESK_BAR-10)%b.h==0);
        int rank=((b.x-8)/b.w)*rows+(b.y-DESK_BAR-10)/b.h;
        assert(rank<d.sh.icon_count);
        for (int j=0;j<i;j++) assert(b.x!=d.sh.icons[j].box.x || b.y!=d.sh.icons[j].box.y);
    }
    int network=named("Network Neighborhood"); assert(network>=3);
    const int system[]={0,1,network,2};
    for (int i=0;i<4;i++) {
        struct desk_box b=d.sh.icons[system[i]].box;
        assert(b.x==8+(i/rows)*b.w && b.y==DESK_BAR+10+(i%rows)*b.h);
    }
}
static void arrange_cases(void)
{
    close_message(); d.focus=-1;
    char too_long[766]; memset(too_long,'x',765); too_long[765]=0;
    assert(!cfg_icon_label_set(&d.sh.cfg,too_long,"refused"));
    int a=fake_file(DESK_FOLDER "/arr-alpha.txt",70,1);
    int b=fake_file(DESK_FOLDER "/arr-beta.bmp",2,1);
    int z=fake_file(DESK_FOLDER "/arr-Zulu.bin",20,1);
    fake_nodes[a].meta.modified=10; fake_nodes[b].meta.modified=30; fake_nodes[z].meta.modified=20;
    assert(!d.v3_links); d.v3_links=calloc(2,sizeof(*d.v3_links)); assert(d.v3_links); d.v3_link_count=2;
    for (unsigned i=0;i<2;i++) {
        d.v3_links[i].desktop=1; d.v3_links[i].launch.show=1;
        strcpy(d.v3_links[i].launch.target,"/Documents");
        snprintf(d.v3_links[i].source,sizeof(d.v3_links[i].source),"/Windows/Desktop/arr-%u.lnk",i);
    }
    strcpy(d.v3_links[0].launch.label,"arr-Installed A"); d.v3_links[0].source_size=100; d.v3_links[0].source_modified=40;
    strcpy(d.v3_links[1].launch.label,"arr-Installed Z"); d.v3_links[1].source_size=1; d.v3_links[1].source_modified=5;
    command(DF_REFRESH,-1);
    /* Old manual and migration coordinates must not defeat explicit sorting. */
    assert(cfg_icon_move_at(&d.sh.cfg,"arr-beta.bmp",d.width,d.height,800,450));
    assert(cfg_icon_move(&d.sh.cfg,"arr-alpha.txt",700,500));
    arrange_pick(SH_ACT_ICON_SORT,0); assert(!d.sh.cfg.icon_auto_arrange);
    assert(arrange_before("arr-alpha.txt","arr-beta.bmp") && arrange_before("arr-beta.bmp","arr-Zulu.bin"));
    assert(arrange_before("arr-Installed A","arr-Installed Z"));
    arrange_grid();
    arrange_pick(SH_ACT_ICON_SORT,1);
    assert(arrange_before("arr-Zulu.bin","arr-beta.bmp") && arrange_before("arr-beta.bmp","arr-alpha.txt"));
    assert(arrange_before("arr-Installed A","arr-alpha.txt"));
    arrange_pick(SH_ACT_ICON_SORT,2);
    assert(arrange_before("arr-beta.bmp","arr-Zulu.bin") && arrange_before("arr-Zulu.bin","arr-alpha.txt"));
    assert(arrange_before("arr-Installed Z","arr-beta.bmp") && arrange_before("arr-alpha.txt","arr-Installed A"));
    arrange_pick(SH_ACT_ICON_SORT,3);
    assert(arrange_before("arr-alpha.txt","arr-Zulu.bin") && arrange_before("arr-Zulu.bin","arr-beta.bmp"));
    assert(arrange_before("arr-Installed Z","arr-alpha.txt") && arrange_before("arr-beta.bmp","arr-Installed A"));
    arrange_grid();
    /* Packed positions survive a serialized reload while Auto Arrange is off. */
    static char text[CFG_TEXT_MAX]; static struct ciuki_settings parsed;
    struct desk_box before=d.sh.icons[named("arr-beta.bmp")].box;
    size_t bytes=cfg_format(&d.sh.cfg,text,sizeof(text)); assert(bytes);
    cfg_defaults(&parsed); assert(!cfg_parse(&parsed,text,bytes)); d.sh.cfg=parsed; desk_folder_changed(&d);
    assert(d.sh.cfg.icon_sort==3 && !d.sh.cfg.icon_auto_arrange);
    assert(!memcmp(&before,&d.sh.icons[named("arr-beta.bmp")].box,sizeof(before)));
    arrange_pick(SH_ACT_ICON_AUTO,0); assert(d.sh.cfg.icon_auto_arrange); arrange_grid();
    desk_menu_open(&d,SH_MENU_ARRANGE,500,200);
    int checked=0; for (int i=0;i<d.sh.menus[0].count;i++) if (d.sh.menus[0].items[i].action==SH_ACT_ICON_AUTO) checked=!!(d.sh.menus[0].items[i].flags&SH_ITEM_CHECKED);
    assert(checked); key(DU_KEY_ESC);
    fake_file(DESK_FOLDER "/arr-added.txt",9,1); command(DF_REFRESH,-1); arrange_grid();
    arrange_pick(SH_ACT_ICON_SORT,0);
    int k=named("arr-added.txt"); assert(k>=3);
    desk_folder_rename(&d,k); strcpy(d.sh.folder->edit,"arr-renamed.txt"); key(DU_KEY_ENTER); desk_folder_service(&d);
    assert(named("arr-renamed.txt")>=3 && !strcmp(d.sh.icons[d.sh.icon_selected].label,"arr-renamed.txt")); arrange_grid();
    command(DF_DELETE,named("arr-renamed.txt")-3); confirm(); settle();
    assert(named("arr-renamed.txt")<0); arrange_grid();
    arrange_pick(SH_ACT_ICON_SORT,3);
    fake_file("/Documents/arr-drop.dat",4,1); char *paths[]={"/Documents/arr-drop.dat"};
    assert(!desk_folder_transfer(&d,paths,1,DESK_FOLDER,FM_EFFECT_COPY,0,1,800,500)); settle();
    assert(named("arr-drop.dat")>=3 && d.sh.cfg.icon_auto_arrange); arrange_grid();
    for (int i=0;i<d.sh.icon_count;i++) d.sh.icons[i].selected=0;
    d.sh.icon_selected=0; d.sh.icons[0].selected=1; before=d.sh.icons[0].box;
    d.mouse_x=before.x+48; d.mouse_y=before.y+25; d.now_ns+=1000000000ull;
    desk_shell_button(&d,1,1); dnd_move(950,690); assert(d.sh.dnd.active); desk_shell_button(&d,1,0);
    assert(d.sh.cfg.icon_auto_arrange && !memcmp(&before,&d.sh.icons[0].box,sizeof(before))); arrange_grid();
    bytes=cfg_format(&d.sh.cfg,text,sizeof(text)); assert(bytes);
    cfg_defaults(&parsed); assert(!cfg_parse(&parsed,text,bytes)); d.sh.cfg=parsed;
    d.width=800; d.height=600; desk_folder_changed(&d); arrange_grid();
    d.width=1024; d.height=768; desk_folder_changed(&d); arrange_grid();
    assert(d.sh.cfg.icon_auto_arrange && d.sh.cfg.icon_sort==3);
    before=d.sh.icons[named("arr-beta.bmp")].box;
    arrange_pick(SH_ACT_ICON_AUTO,0); assert(!d.sh.cfg.icon_auto_arrange);
    bytes=cfg_format(&d.sh.cfg,text,sizeof(text)); assert(bytes);
    cfg_defaults(&parsed); assert(!cfg_parse(&parsed,text,bytes)); d.sh.cfg=parsed; desk_folder_changed(&d);
    assert(!d.sh.cfg.icon_auto_arrange && !memcmp(&before,&d.sh.icons[named("arr-beta.bmp")].box,sizeof(before)));
    arrange_pick(SH_ACT_ICON_AUTO,0); assert(d.sh.cfg.icon_auto_arrange);
    /* Rename changes presentation, never the launch target or fixed order. */
    d.sh.icon_selected=0; key(DU_KEY_F2); cache_equivalent();
    key(0x11); assert(!strcmp(d.sh.icon_edit,"w")); cache_equivalent(); key(DU_KEY_ESC);
    assert(!strcmp(d.sh.icons[0].label,"My Computer"));
    d.sh.icon_selected=0; key(DU_KEY_F2); assert(d.sh.icon_editing==0);
    strcpy(d.sh.icon_edit,"Workstation"); key(DU_KEY_ENTER); assert(!strcmp(d.sh.icons[0].label,"Workstation"));
    d.sh.icon_selected=0; key(DU_KEY_F2); strcpy(d.sh.icon_edit,"Workstation");
    d.mouse_x=950; d.mouse_y=690; desk_shell_button(&d,1,1); desk_shell_button(&d,1,0);
    assert(d.sh.icon_editing<0 && !strcmp(d.sh.icons[0].label,"Workstation"));
    g19_context(1,"Rename"); assert(d.sh.icon_editing==1); strcpy(d.sh.icon_edit,"My work"); key(DU_KEY_ENTER);
    desk_menu_open(&d,SH_MENU_DOCUMENTS,500,200);
    int found=0; for (int i=0;i<d.sh.menus[0].count;i++) if (!strcmp(d.sh.menus[0].items[i].label,"My work")) found=1;
    assert(found); key(DU_KEY_ESC);
    k=named("Network Neighborhood"); g19_context(k,"Rename"); strcpy(d.sh.icon_edit,"Our network"); key(DU_KEY_ENTER);
    assert(!strcmp(d.sh.icons[k].argument,"--panel=network"));
    g19_context(k,"Open"); assert(!strcmp(program,"control") && !strcmp(launched,"--panel=network"));
    bytes=cfg_format(&d.sh.cfg,text,sizeof(text)); assert(bytes);
    cfg_defaults(&parsed); assert(!cfg_parse(&parsed,text,bytes)); d.sh.cfg=parsed; desk_folder_changed(&d);
    assert(!strcmp(d.sh.icons[0].label,"Workstation") && !strcmp(d.sh.icons[1].label,"My work") && named("Our network")>=3);
    for (int i=0;i<2;i++) { d.sh.icon_selected=i; key(DU_KEY_F2); key(DU_KEY_DELETE); key(DU_KEY_ENTER); }
    k=named("Our network"); g19_context(k,"Rename"); key(DU_KEY_DELETE); key(DU_KEY_ENTER);
    assert(!strcmp(d.sh.icons[0].label,"My Computer") && !strcmp(d.sh.icons[1].label,"My Documents")); arrange_grid();
    d.sh.icon_selected=2; key(DU_KEY_F2); assert(d.sh.icon_editing<0);
    d.sh.menu_owner=2; desk_menu_open(&d,SH_MENU_ICON,500,200);
    assert(menu_flags("Rename")==SH_ITEM_DISABLED); key(DU_KEY_ESC);
    /* A new resolution with too little settings space refuses atomically. */
    parsed=d.sh.cfg;
    size_t used=strlen(d.sh.cfg.extra),limit=sizeof(d.sh.cfg.extra)-64;
    assert(used+5<limit); memcpy(d.sh.cfg.extra+used,"pad=",4);
    memset(d.sh.cfg.extra+used+4,'x',limit-used-5);
    d.sh.cfg.extra[limit-1]='\n'; d.sh.cfg.extra[limit]=0;
    static struct ciuki_settings full; full=d.sh.cfg;
    d.width=900; d.height=700; arrange_pick(SH_ACT_ICON_SORT,2);
    assert(!memcmp(&full,&d.sh.cfg,sizeof(full)) && strstr(d.sh.message_text,"previous arrangement"));
    close_message(); d.sh.cfg=parsed; d.width=1024; d.height=768; desk_folder_changed(&d);
    free(d.v3_links); d.v3_links=NULL; d.v3_link_count=0;
    puts("PASS fx-desk-arrange: four menu sorts, saved positions, fixed system order, Auto Arrange add/rename/delete/drop, check mark, reload/resolution, system rename/reset/other views and Recycle Bin denial");
}
