/* SPDX-License-Identifier: MIT
 * Files controller and libdeskui rendering against the fake image tree.
 * Replays the files-qemu-t23 script in client coordinates, checks the
 * checkpoints and filesystem outcome, and writes deterministic frames. */
#include "fake_vfs.h"
#include "../../../apps/desktop/media_notice.h"
#include "../../../apps/files/app.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

static void raw_files_event(struct du_ui *u,const struct du_event *e,void *arg)
{ files_event(u,e,arg); }
#include "files_wait.h"

#define WIDTH 760
#define HEIGHT 520
static uint32_t *pixels;
static struct du_ui ui;
static struct files_app app;
static const char *out_dir, *portrait_path;
static uint64_t clock_ns = 1000000000ull;
static int checkpoints[8], checkpoint_count, closed;

static void dump(const char *name)
{
    du_render(&ui);
    uint32_t *partial=malloc(WIDTH*HEIGHT*4); assert(partial); memcpy(partial,pixels,WIDTH*HEIGHT*4);
    ui.dirty=1; du_render(&ui);
    if (memcmp(partial,pixels,WIDTH*HEIGHT*4)) { fprintf(stderr,"Partial frame differs: %s\n",name); }
    assert(!memcmp(partial,pixels,WIDTH*HEIGHT*4)); free(partial);
    assert(pixels[WIDTH * HEIGHT] == 0xdeadbeefu);
    if (!out_dir) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/files-%s.xrgb", out_dir, name);
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(pixels, 4, WIDTH * HEIGHT, f) == WIDTH * HEIGHT);
    fclose(f);
}
static void event(int type, int code, int value, int value2)
{
    clock_ns += 20000000ull;
    struct ciuki_input_event e = {.type = (uint32_t)type, .code = code, .value = value, .value2 = value2, .monotonic_ns = clock_ns};
    du_input(&ui, &e);
    files_test_wait(&app);
}
static void motion(int x, int y) { event(CIUKI_INPUT_MOTION, 0, x, y); }
static void press(int b) { event(CIUKI_INPUT_BUTTON, b, 1, 0); }
static void release(int b) { event(CIUKI_INPUT_BUTTON, b, 0, 0); }
static void click(int x, int y) { motion(x, y); press(1); release(1); clock_ns += 600000000ull; }
static void rclick(int x, int y) { motion(x, y); press(2); release(2); clock_ns += 600000000ull; }
static void key(int code) { event(CIUKI_INPUT_KEY, code, 1, 0); event(CIUKI_INPUT_KEY, code, 0, 0); }
static void chord(int mod, int code) { event(CIUKI_INPUT_KEY, mod, 1, 0); key(code); event(CIUKI_INPUT_KEY, mod, 0, 0); }
static void tick(void) { for (int i = 0; i < 200 && app.job_active; i++) { clock_ns += 100000000ull; du_tick(&ui, clock_ns); files_test_wait(&app); struct timespec pause={0,1000000}; nanosleep(&pause,NULL); } }
static void on_checkpoint(struct files_app *a, int stage) { (void)a; checkpoints[checkpoint_count++] = stage; }
static void on_close(struct files_app *a) { (void)a; closed = 1; }
static int control_spawned;
static int control_spawn(struct files_app *a, const char *program, char *const argv[])
{
    (void)a; assert(!strcmp(program, "/Programs/control") && !strcmp(argv[0], "control") &&
                    !strcmp(argv[1], "--panel=home") && !argv[2]);
    control_spawned++; return 77;
}
static int spawned;
static char launched[80], launch_path[FM_PATH];
static int on_spawn(struct files_app *a, const char *program, char *const argv[])
{
    (void)a; spawned++; snprintf(launched,sizeof(launched),"%s",program);
    snprintf(launch_path,sizeof(launch_path),"%s",argv[1]?argv[1]:"");
    return !strcmp(program,"/Programs/hello") || !strcmp(program,"/Programs/paint") || !strcmp(program,"/Programs/viewer") || !strcmp(program,"/Programs/music") ? 77 : -2;
}
static int portrait(uint32_t *p)
{
    if (!portrait_path) return -1;
    FILE *f = fopen(portrait_path, "rb"); if (!f) return -1;
    size_t n = fread(p, 4, 256 * 256, f); fclose(f);
    return n == 256 * 256 ? 0 : -1;
}
static int list_row_y(struct du_widget *list, unsigned index) { struct du_rect r = du_list_label(list, index); return r.y + r.h / 2; }
static int list_find(const char *name)
{ for (unsigned i = 0; i < app.model.count; i++) if (!strcmp(app.model.entries[i].name, name)) return (int)i; return -1; }

static void type(const char *text)
{
    static const unsigned char letters[] = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c
    };
    for (; *text; text++) {
        unsigned c = (unsigned char)*text, code; int shift = 0;
        if (c >= 'A' && c <= 'Z') { shift = 1; c += 'a' - 'A'; }
        if (c >= 'a' && c <= 'z') code = letters[c - 'a'];
        else if (c == ':') { shift = 1; code = 0x27; }
        else if (c == '\\') code = 0x2b;
        else if (c == '/') code = 0x35;
        else if (c == ' ') code = 0x39;
        else { assert(!"unmapped character"); code = 0; }
        if (shift) chord(DU_KEY_LSHIFT, (int)code); else key((int)code);
    }
}
static void address(const char *text)
{ chord(DU_KEY_LALT, 0x20); type(text); key(DU_KEY_ENTER); }
static void activate(int id)
{ struct du_event e = {.type = DU_EV_ACTIVATE, .id = id}; files_event(&ui, &e, &app); }

static int removal_error, removal_calls;
static struct ciuki_media_state media_state;
static int read_media(struct ciuki_media_state *s) { *s=media_state; return 0; }
static int remove_media(unsigned d,uint32_t gen)
{
    assert((d==4 && gen==77) || (d==3 && gen==88)); removal_calls++;
    if (removal_error) return removal_error;
    fake_nodes[fake_lookup(d==3 ? "/mnt/d" : "/mnt/e")].live=0; media_state.removable_mask &= ~(1u<<d); media_state.type[d]=d==3 ? 0 : CIUKI_MEDIA_SAFE_REMOVED; return 0;
}
static int blockers(unsigned drive,uint32_t generation,struct ciuki_media_blockers *b)
{
    assert(drive==4 && generation==77);
    *b=(struct ciuki_media_blockers){.count=2,.programs={{42,"ciuknote"},{43,"terminal"}}}; return 0;
}
static void removable_ui_tests(void)
{
    fake_image_tree(); fake_create("/mnt/e",FM_DIR);
    memset(&app,0,sizeof(app));du_init(&ui,pixels,WIDTH,HEIGHT);
    app.media_read=read_media;app.media_remove=remove_media;app.media_blockers=blockers;
    media_state=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_state),.removable_mask=1u<<4};media_state.generation[4]=77;media_state.type[4]=CIUKI_MEDIA_USB;
    assert(!files_setup(&app,&ui,&fake_vfs,FM_COMPUTER));
    int row=list_find("E:");assert(row>=0);fm_select(&app.model,(unsigned)row,0);files_update(&app);
    assert(!strcmp(app.rows[row].text[2],"Removable Drive") && app.rows[row].icon16 && app.rows[row].icon32);
    struct du_event e={.type=DU_EV_CONTEXT,.id=107,.index=row,.x=300,.y=200};files_event(&ui,&e,&app);
    int action=0;for(unsigned i=0;i<app.popup_count;i++)if(app.popup[i].label && strstr(app.popup[i].label,"Safely"))action=app.popup[i].id;
    assert(action);du_popup(&ui,NULL,0,0,0,0);
    removal_error=-EBUSY;activate(action);assert(removal_calls==1 && ui.dialog.kind && media_state.removable_mask);
    assert(strstr(ui.dialog.text,"ciuknote (42)") && strstr(ui.dialog.text,"terminal (43)"));
    key(DU_KEY_ENTER);removal_error=0;activate(action);
    assert(removal_calls==2 && !app.model.path[0] && list_find("E:")<0 && strstr(app.note,"safe to unplug"));
    files_destroy(&app);fake_assert_clean();
    fake_image_tree(); memset(&app,0,sizeof(app));du_init(&ui,pixels,WIDTH,HEIGHT);
    app.media_read=read_media;app.media_remove=remove_media;app.media_blockers=blockers;
    media_state=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_state)};
    assert(!files_setup(&app,&ui,&fake_vfs,FM_COMPUTER));
    assert(list_find("D:")<0); fake_create("/mnt/d",FM_DIR);
    media_state.removable_mask=1u<<3;media_state.generation[3]=88;media_state.type[3]=CIUKI_MEDIA_OPTICAL;
    files_update(&app); /* unrelated UI activity must not consume the change */
    e=(struct du_event){.type=DU_EV_TICK,.id=2};files_event(&ui,&e,&app);
    row=list_find("D:");assert(row>=0 && !strcmp(app.rows[row].text[2],"CD-ROM Drive") && app.rows[row].icon16 && app.rows[row].icon32);
    assert(!files_navigate(&app,"/mnt/d"));
    fake_nodes[fake_lookup("/mnt/d")].live=0;media_state.removable_mask=0;media_state.generation[3]=0;media_state.type[3]=0;
    files_event(&ui,&e,&app);assert(!app.model.path[0] && list_find("D:")<0);
    fake_create("/mnt/d",FM_DIR);media_state.removable_mask=1u<<3;media_state.generation[3]=88;media_state.type[3]=CIUKI_MEDIA_OPTICAL;
    files_event(&ui,&e,&app);row=list_find("D:");assert(row>=0);fm_select(&app.model,(unsigned)row,0);files_update(&app);
    struct du_event context={.type=DU_EV_CONTEXT,.id=107,.index=row,.x=300,.y=200};files_event(&ui,&context,&app);
    int eject=0;for(unsigned i=0;i<app.popup_count;i++)if(app.popup[i].label && strstr(app.popup[i].label,"Eject"))eject=app.popup[i].id;
    assert(eject==action);du_popup(&ui,NULL,0,0,0,0);
    removal_error=0;activate(action);assert(list_find("D:")<0 && strstr(app.note,"disc has been ejected"));
    files_destroy(&app);fake_assert_clean();
    puts("PASS Files removable Computer view: E: Tango icon/type, context action, busy error, generation, safe detach and refresh");
    puts("PASS Files optical Computer view: runtime insertion/removal, D: CD-ROM icon/type, expired folder returns to Computer, reinsertion and eject text");
}
static void external_removal_tests(void)
{
    /* Same generation after safe removal: a tray action and a separate Files
     * controller must both expire a browser inside a nested directory. */
    for (int other_files=0;other_files<2;other_files++) {
        fake_image_tree();fake_create("/mnt/e",FM_DIR);fake_create("/mnt/e/sub",FM_DIR);fake_file("/mnt/e/sub/stale.txt",3,0);
        memset(&app,0,sizeof(app));du_init(&ui,pixels,WIDTH,HEIGHT);
        app.media_read=read_media;
        media_state=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_state),.removable_mask=1u<<4};
        media_state.generation[4]=77;media_state.type[4]=CIUKI_MEDIA_USB;
        assert(!files_setup(&app,&ui,&fake_vfs,"/mnt/e/sub"));
        assert(list_find("stale.txt")>=0);
        if (other_files) {
            struct files_app *other=calloc(1,sizeof(*other));struct du_ui *other_ui=calloc(1,sizeof(*other_ui));assert(other && other_ui);
            du_init(other_ui,pixels,WIDTH,HEIGHT);other->media_read=read_media;other->media_remove=remove_media;
            assert(!files_setup(other,other_ui,&fake_vfs,"/mnt/e"));
            int action=0;
            for (unsigned i=0;i<other->menus[0].count;i++)
                if (strstr(other->file_items[i].label ? other->file_items[i].label : "","Safely")) action=other->file_items[i].id;
            assert(action);
            struct du_event remove={.type=DU_EV_ACTIVATE,.id=action};files_event(other_ui,&remove,other);
            assert(!other->model.path[0]);files_destroy(other);free(other_ui);free(other);
        } else {
            /* Tray/kernel observation: absent mask with retained success gen. */
            assert(!remove_media(4,77));
        }
        assert(media_state.generation[4]==77 && !(media_state.removable_mask & (1u<<4)));
        /* The old directory can even still be in a caller's namespace cache. */
        struct du_event poll={.type=DU_EV_TICK,.id=2};files_event(&ui,&poll,&app);
        assert(!app.model.path[0] && list_find("stale.txt")<0 && list_find("E:")<0);
        files_event(&ui,&poll,&app);assert(!app.model.path[0] && list_find("stale.txt")<0);
        files_destroy(&app);fake_assert_clean();
    }
    puts("PASS Files external removal: tray and second Files instance, retained generation, nested stale listing cleared to Computer");
}
static unsigned label_calls[26], key_calls[26];
static struct files_label_key label_keys[26];
static int label_error;
static int label_source(unsigned d,struct files_label_key *key)
{ key_calls[d]++;*key=label_keys[d];return label_error; }
static int label_read(unsigned d,char out[33])
{ label_calls[d]++;snprintf(out,33,"Drive %u label %lld",d,(long long)label_keys[d].modified);return 0; }
static void label_cache_tests(void)
{
    fake_image_tree();fake_create("/mnt/e",FM_DIR);fake_create("/mnt/f",FM_DIR);
    memset(&app,0,sizeof(app));du_init(&ui,pixels,WIDTH,HEIGHT);
    memset(label_calls,0,sizeof(label_calls));memset(key_calls,0,sizeof(key_calls));memset(label_keys,0,sizeof(label_keys));
    app.media_read=read_media;app.media_remove=remove_media;app.media_label=label_read;app.media_label_key=label_source;
    media_state=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_state),.removable_mask=(1u<<4)|(1u<<5)};
    media_state.generation[4]=77;media_state.generation[5]=78;media_state.type[4]=media_state.type[5]=CIUKI_MEDIA_USB;
    assert(!files_setup(&app,&ui,&fake_vfs,FM_COMPUTER));
    assert(label_calls[4]==1 && label_calls[5]==1);
    int row=list_find("E:");assert(row>=0 && strstr(app.rows[row].text[0],"Drive 4 label 0"));
    fm_select(&app.model,(unsigned)row,0);
    for (unsigned i=0;i<8;i++) { files_update(&app);files_layout(&app); }
    assert(label_calls[4]==1 && label_calls[5]==1 && key_calls[4]==1 && key_calls[5]==1);
    assert(strstr(app.removal_label,"Drive 4 label 0"));
    assert(!files_navigate(&app,"/mnt/e"));
    for (unsigned i=0;i<8;i++) files_update(&app);
    assert(label_calls[4]==1 && key_calls[4]==1);
    struct du_event poll={.type=DU_EV_TICK,.id=2};
    ui.dirty=0;
    for (unsigned i=0;i<4;i++) files_event(&ui,&poll,&app);
    assert(!ui.dirty); /* Unchanged worker observations do not repaint. */
    assert(label_calls[4]==1 && label_calls[5]==1); /* metadata only */
    label_keys[4].modified=1;files_event(&ui,&poll,&app);
    assert(label_calls[4]==2 && label_calls[5]==1 && strstr(app.removal_label,"Drive 4 label 1"));
    strcpy(label_keys[5].volume,"New volume label");files_event(&ui,&poll,&app);
    assert(label_calls[4]==2 && label_calls[5]==2);
    label_error=-EIO;label_keys[4].modified=2;files_event(&ui,&poll,&app);
    assert(label_calls[4]==2 && strstr(app.removal_label,"Drive 4 label 1"));
    label_error=0;files_event(&ui,&poll,&app);assert(label_calls[4]==3);
    media_state.generation[4]++;files_event(&ui,&poll,&app);
    assert(label_calls[4]==4 && label_calls[5]==2 && !app.model.path[0]);
    media_state.removable_mask &= ~(1u<<4);files_event(&ui,&poll,&app);
    assert(!app.drive_labels[4].valid && label_calls[4]==4);
    media_state.removable_mask |= 1u<<4;files_event(&ui,&poll,&app);
    assert(label_calls[4]==5 && label_calls[5]==2);
    key(DU_KEY_F5);assert(label_calls[4]==6 && label_calls[5]==3);
    files_destroy(&app);fake_assert_clean();
    puts("PASS Files labels: cached UI/selection/layout, unchanged polls, per-drive generation, volume/AUTORUN metadata changes, retry, absence/reinsert and F5");
}
static void blocker_dialog_tests(void)
{
    for (unsigned theme=0;theme<(unsigned)desk_theme_count();theme++) for (int small=0;small<2;small++) for (int long_names=0;long_names<2;long_names++) {
        desk_theme_use(desk_theme_at(theme)->name);
        int width=small ? 620 : WIDTH, height=small ? 380 : HEIGHT;
        du_init(&ui,pixels,width,height);
        struct ciuki_media_blockers b={.count=long_names ? 64 : 30};
        for (unsigned i=0;i<b.count;i++) {
            b.programs[i].pid=i+1;
            snprintf(b.programs[i].name,sizeof(b.programs[i].name),long_names ? "WWWWWWWWWWWWWWWWWWWWWWWWWWW%02u" : "p%u",i);
        }
        char text[1024];struct du_rect budget=desk_media_dialog_body(width,height);
        desk_media_blocker_text(text,sizeof(text),&b,budget);
        assert(desk_media_text_fits(text,budget));
        const char *tail=strstr(text,"\n... and ");assert(tail);
        unsigned shown=0,remaining=0;
        for (const char *s=text;*s;s++) if (*s=='(') shown++;
        assert(sscanf(tail,"\n... and %u more programs.",&remaining)==1 && shown+remaining==b.count && shown);
        du_message(&ui,204,"Drive is in use",text,DU_DLG_ICON_WARNING,NULL,0,0,0);
        struct du_dialog *dialog=&ui.dialog;
        struct du_rect actual={0,0,dialog->box.w-78,dialog->items[0].box.y-18-(dialog->box.y+DESK_FRAME_TITLE+16)};
        assert(desk_media_text_fits(dialog->text,actual));
        assert(!strcmp(dialog->text,text));du_render(&ui);du_dialog_close(&ui);
    }
    desk_theme_use("platinum");
    puts("PASS Files blocker dialog rows: many short/long wrapped names, complete visible remainder, three themes, 620x380 and 760x520");
}
static void computer_ui_tests(void)
{
    fake_image_tree(); memset(&app, 0, sizeof(app));
    du_init(&ui, pixels, WIDTH, HEIGHT); app.portrait = portrait; app.spawn = control_spawn;
    assert(!files_setup(&app, &ui, &fake_vfs, NULL));
    assert(!app.model.path[0] && app.model.count == 1 && app.place_count == 7);
    assert(!strcmp(du_get(&ui, 100)->text, "Computer"));
    fm_select(&app.model, 0, 0); files_update(&app);
    assert(app.rows[0].icon16 && app.rows[0].icon32 && !strcmp(app.rows[0].text[2], "Disk Drive"));
    assert(du_get(&ui, 3)->disabled && du_get(&ui, 4)->disabled && du_get(&ui, 7)->disabled && du_get(&ui, 8)->disabled);
    int operations = fake_operations;
    const int forbidden[] = {4, 5, 6, 7, 8, 11, 12, 13, 14, 32};
    for (unsigned i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) activate(forbidden[i]);
    assert(fake_operations == operations && !app.job_active && !ui.dialog.kind && app.rename_index < 0);
    rclick(du_get(&ui, 107)->box.x + 30, list_row_y(du_get(&ui, 107), 0));
    for (unsigned i = 0; i < app.popup_count; i++)
        if (app.popup[i].id == 7 || app.popup[i].id == 14 || app.popup[i].id == 8) assert(app.popup[i].flags & DU_ITEM_DISABLED);
    key(DU_KEY_ESC); key(DU_KEY_ENTER);
    assert(!strcmp(app.model.path, "/") && !strcmp(du_get(&ui, 100)->text, "C:\\"));
    key(DU_KEY_BACKSPACE); assert(!app.model.path[0]);
    address("C:\\Programs\\"); assert(!strcmp(app.model.path, "/Programs"));
    assert(!strcmp(du_get(&ui, 100)->text, "C:\\Programs") && strstr(du_get(&ui, 108)->text, "C:\\Programs"));
    address("/Documents/"); assert(!strcmp(app.model.path, "/Documents"));
    struct du_widget *places = du_get(&ui, 103);
    for (unsigned i = 0; i < app.place_count; i++) {
        struct du_rect r = du_list_label(places, i);
        click(r.x + r.w / 2, r.y + r.h / 2);
        if (!strcmp(app.places[i].path, "control:")) {
            assert(control_spawned == 1 && !du_dialog_id(&ui));
        } else assert(fm_same_path(app.model.path, app.places[i].path));
        assert(app.place_items[i].icon16);
    }
    assert(control_spawned == 1); activate(302); assert(control_spawned == 2);
    fake_create("/mnt/d", FM_DIR); fake_create("/mnt/d/Games", FM_DIR); fake_create("/mnt/d/Games/tmp", FM_DIR);
    assert(!files_navigate(&app, "Computer") && app.model.count == 2);
    fm_select_none(&app.model); files_update(&app); dump("computer");
    fm_select(&app.model, 1, 0); files_update(&app); du_focus(&ui, 107); key(DU_KEY_ENTER);
    assert(!strcmp(app.model.path, "/mnt/d") && !strcmp(du_get(&ui, 100)->text, "D:\\"));
    address("D:\\Games"); assert(!strcmp(app.model.path, "/mnt/d/Games") && list_find("tmp") >= 0);
    activate(12); assert(app.rename_index >= 0); type("Created"); key(DU_KEY_ENTER);
    assert(fake_lookup("/mnt/d/Games/Created") >= 0);
    chord(DU_KEY_LALT, DU_KEY_ENTER); tick(); assert(strstr(ui.dialog.text, "D:\\Games")); key(DU_KEY_ENTER);
    key(DU_KEY_DELETE); key(DU_KEY_ENTER); tick(); assert(fake_lookup("/mnt/d/Games/Created") < 0);
    address("C:\\"); assert(app.model.count == 4);
    for (unsigned i = 0; i < app.model.count; i++) assert(!fm_hidden("/", app.model.entries[i].name));
    assert(!strcmp(app.tree[0].name, "Computer") && !strcmp(app.tree[1].name, "C:") && app.tree[1].depth == 1);
    for (unsigned i = 0; i < app.tree_count; i++) if (app.tree[i].depth == 2) assert(!fm_hidden("/", app.tree[i].name));
    fake_create("/.config", FM_DIR); activate(25); assert(list_find(".config") >= 0);
    int dot=0; for (unsigned i = 0; i < app.tree_count; i++) dot |= !strcmp(app.tree[i].name, ".config");
    assert(dot); fake_nodes[fake_lookup("/.config")].live=0; activate(25);
    dump("c-drive");
    fake_nodes[fake_lookup("/Games")].live = 0; activate(25);
    assert(app.place_count == 6);
    files_destroy(&app); fake_assert_clean();
    puts("PASS Files Computer UI: drive open/Up, protected commands/menus, typed POSIX/drive addresses, Places, D: create/rename/properties/delete, hidden tree, optional Games");
}

/* Client-coordinate targets shared with the QEMU script (window at 4,36:
 * client origin 7,60). The Python test converts them to screen moves. */
static void script_coordinates(void)
{
    struct du_rect view = du_menu_title(&ui, 2), file = du_menu_title(&ui, 0);
    struct du_rect vbox = du_menu_box(&ui, app.view_items, 10, view.x, view.y + view.h);
    struct du_rect fbox = du_menu_box(&ui, app.file_items, 11, file.x, file.y + file.h);
    struct du_widget *places = du_get(&ui, 103);
    struct du_rect system = du_list_label(places, 3);
    printf("SCRIPT view_title=%d,%d list_item=%d,%d places_system=%d,%d file_title=%d,%d new_folder=%d,%d\n",
           view.x + view.w / 2, view.y + view.h / 2, vbox.x + 40, vbox.y + 3 + DU_MENU_ITEM_H + DU_MENU_ITEM_H / 2,
           system.x + system.w / 2, system.y + system.h / 2, file.x + file.w / 2, file.y + file.h / 2,
           fbox.x + 40, fbox.y + 3 + 2 * DU_MENU_ITEM_H + DU_MENU_SEP_H + DU_MENU_ITEM_H / 2);
}

/* ---- fx-19: drag sources, drop targets, rubber band and keyboard selection ---- */
static int drag_begins, drag_button, statuses; static unsigned drag_items, status_effect, status_allowed; static uint32_t status_serial;
static int on_begin(struct files_app *a, int button, unsigned count) { (void)a; drag_begins++; drag_button=button; drag_items=count; return 0; }
static void on_status(struct files_app *a, uint32_t serial, unsigned effect, unsigned allowed)
{ (void)a; statuses++; status_serial=serial; status_effect=effect; status_allowed=allowed; }
static struct du_widget *LIST(void) { return du_get(&ui,107); }
static struct du_widget *TREE(void) { return du_get(&ui,105); }
static void label_point(const char *name, int *x, int *y)
{
    int index=list_find(name); assert(index>=0);
    struct du_rect r=du_list_label(LIST(),(unsigned)index); *x=r.x+6; *y=r.y+r.h/2;
}
static unsigned selected_names(const char *a, const char *b, const char *c)
{
    unsigned n=fm_selected(&app.model,NULL);
    const char *want[3]={a,b,c};
    for (unsigned k=0;k<3;k++) if (want[k]) { int i=list_find(want[k]); assert(i>=0 && app.model.entries[i].selected); }
    return n;
}
static void drag_data(unsigned count, const char *first)
{
    char **paths; char *types; unsigned n;
    assert(!fm_drag_load(&fake_vfs,&paths,&types,&n) && n==count && !strcmp(paths[0],first));
    fm_drag_free(paths,types,n);
}
/* A drag over (x, y) answered with keys; returns the effect reported. */
static unsigned over(uint32_t serial, int x, int y, unsigned keys)
{ int before=statuses; files_drag_over(&app,serial,0,x,y,keys); assert(statuses==before+1 && status_serial==serial); return status_effect; }
static void save_drag(const char *path, char type)
{ char *paths[]={(char *)path}; assert(!fm_drag_save(&fake_vfs,paths,&type,1)); }
static void band(int x0, int y0, int x1, int y1, int mod)
{
    if (mod) event(CIUKI_INPUT_KEY,mod,1,0);
    motion(x0,y0); press(1); motion((x0+x1)/2,(y0+y1)/2); motion(x1,y1);
    if (mod) event(CIUKI_INPUT_KEY,mod,0,0);
}
static void file_drag_tests(void)
{
    fake_image_tree();
    fake_file("/Documents/Desktop/Source.txt",100,1); fake_file("/Documents/Desktop/Move.txt",200,2);
    fake_file("/Documents/Desktop/Third.txt",30,3); fake_create("/Documents/Target",FM_DIR);
    fake_create("/Documents/Target/Inner",FM_DIR); fake_create("/mnt",FM_DIR); fake_create("/mnt/e",FM_DIR);
    fake_file("/mnt/e/Stick.txt",40,4);
    memset(&app,0,sizeof(app)); du_init(&ui,pixels,WIDTH,HEIGHT);
    assert(!files_setup(&app,&ui,&fake_vfs,"/Documents/Desktop"));
    app.drag_begin=on_begin; app.drag_status=on_status;
    assert(LIST()->file_drag && LIST()->marquee && TREE()->file_drag);
    /* The shared clipboard is never the drag data object. */
    int x,y,x2,y2; label_point("Source.txt",&x,&y);
    /* Below SM_CXDRAG (4 px) nothing starts; past it the drag begins once. */
    motion(x,y); press(1); motion(x+4,y-4); assert(!drag_begins); motion(x+5,y); assert(drag_begins==1 && drag_button==1 && drag_items==1);
    motion(x+40,y+40); release(1); assert(drag_begins==1);
    drag_data(1,"/Documents/Desktop/Source.txt");
    assert(fake_lookup("/system/settings/files.clip")<0);
    /* Ctrl-click builds a selection; a press on a selected item keeps it for the drag. */
    clock_ns+=600000000ull; label_point("Move.txt",&x2,&y2);
    event(CIUKI_INPUT_KEY,DU_KEY_LCTRL,1,0); click(x2,y2); event(CIUKI_INPUT_KEY,DU_KEY_LCTRL,0,0);
    assert(selected_names("Source.txt","Move.txt",NULL)==2);
    motion(x,y); press(1); assert(selected_names("Source.txt","Move.txt",NULL)==2);
    motion(x+20,y+20); assert(drag_begins==2 && drag_items==2); release(1);
    drag_data(2,"/Documents/Desktop/Move.txt");
    /* The same press without a drag applies the click at release: only that item. */
    clock_ns+=600000000ull; motion(x,y); press(1); assert(fm_selected(&app.model,NULL)==2); release(1);
    assert(selected_names("Source.txt",NULL,NULL)==1);
    /* Ctrl on a selected item toggles it off at release (no drag). */
    clock_ns+=600000000ull; event(CIUKI_INPUT_KEY,DU_KEY_LCTRL,1,0); click(x2,y2); click(x,y); event(CIUKI_INPUT_KEY,DU_KEY_LCTRL,0,0);
    assert(selected_names("Move.txt",NULL,NULL)==1);
    /* Right-drag starts with button 2; a right click without a drag shows the menu at release. */
    clock_ns+=600000000ull; motion(x2,y2); press(2); assert(!ui.popup); motion(x2+10,y2+10); assert(drag_begins==3 && drag_button==2); release(2); assert(!ui.popup);
    clock_ns+=600000000ull; motion(x2,y2); press(2); assert(!ui.popup); release(2); assert(ui.popup); key(DU_KEY_ESC);
    puts("PASS fx-19 Files drag source: SM_CXDRAG threshold, multi-selection kept on press, deferred click/Ctrl toggle, right-drag and release-time context menu, data object separate from the clipboard");

    /* Drop target: same drive moves, Ctrl copies, Shift moves, Ctrl+Shift/Alt link is refused (no shortcuts). */
    assert(!files_navigate(&app,"/Documents"));
    save_drag("/Documents/Desktop/Source.txt",'F');
    int tx,ty; label_point("Target",&tx,&ty);
    assert(over(7,tx,ty,0)==FM_EFFECT_MOVE && status_allowed==(FM_EFFECT_COPY|FM_EFFECT_MOVE));
    assert(LIST()->drop_hot==list_find("Target")+1);
    dump("drop-target");
    assert(over(7,tx,ty,FM_KEY_CTRL)==FM_EFFECT_COPY && over(7,tx,ty,FM_KEY_SHIFT)==FM_EFFECT_MOVE);
    assert(over(7,tx,ty,FM_KEY_CTRL|FM_KEY_SHIFT)==FM_EFFECT_NONE && over(7,tx,ty,FM_KEY_ALT)==FM_EFFECT_NONE && !LIST()->drop_hot);
    struct du_rect body=du_list_body(LIST());
    assert(over(7,body.x+body.w-30,body.y+body.h-30,0)==FM_EFFECT_MOVE && !LIST()->drop_hot);   /* background: this folder */
    files_drag_over(&app,7,1,0,0,0); assert(status_effect==FM_EFFECT_NONE && !LIST()->drop_hot);
    files_drag_drop(&app,7,tx,ty,FM_EFFECT_COPY); tick();
    assert(fake_lookup("/Documents/Target/Source.txt")>=0 && fake_lookup("/Documents/Desktop/Source.txt")>=0);
    /* Another drive copies by default; Shift moves. */
    save_drag("/mnt/e/Stick.txt",'F');
    assert(over(8,tx,ty,0)==FM_EFFECT_COPY && over(8,tx,ty,FM_KEY_SHIFT)==FM_EFFECT_MOVE);
    files_drag_drop(&app,8,tx,ty,FM_EFFECT_MOVE); tick();
    assert(fake_lookup("/Documents/Target/Stick.txt")>=0 && fake_lookup("/mnt/e/Stick.txt")<0);
    /* A CD source is copy-only; a folder never enters itself; its own folder is a no-op move. */
    app.media.removable_mask=1u<<4; app.media.type[4]=CIUKI_MEDIA_OPTICAL;
    save_drag("/mnt/e/Other.txt",'F');
    assert(over(9,tx,ty,0)==FM_EFFECT_COPY && status_allowed==FM_EFFECT_COPY && over(9,tx,ty,FM_KEY_SHIFT)==FM_EFFECT_NONE);
    app.media.removable_mask=0; app.media.type[4]=0;
    save_drag("/Documents/Target",'D');
    assert(over(10,tx,ty,0)==FM_EFFECT_NONE && status_allowed==0);
    assert(over(10,body.x+body.w-30,body.y+body.h-30,0)==FM_EFFECT_NONE);   /* already in /Documents */
    assert(over(10,body.x+body.w-30,body.y+body.h-30,FM_KEY_CTRL)==FM_EFFECT_COPY);
    /* Drops onto the folder tree and Places use their folder. */
    save_drag("/Documents/Desktop/Move.txt",'F');
    int node=-1; for (unsigned i=0;i<app.tree_count;i++) if (!strcmp(app.tree[i].path,"/Documents/Target")) node=(int)i;
    assert(node>=0);
    struct du_rect nl=du_list_label(TREE(),(unsigned)node);
    assert(over(11,nl.x+4,nl.y+nl.h/2,0)==FM_EFFECT_MOVE && TREE()->drop_hot==node+1);
    files_drag_drop(&app,11,nl.x+4,nl.y+nl.h/2,FM_EFFECT_MOVE); tick();
    assert(fake_lookup("/Documents/Target/Move.txt")>=0 && fake_lookup("/Documents/Desktop/Move.txt")<0 && !TREE()->drop_hot);
    /* A stale or mismatched effect does nothing. */
    save_drag("/Documents/Desktop/Third.txt",'F');
    files_drag_drop(&app,12,tx,ty,FM_EFFECT_LINK); tick(); assert(fake_lookup("/Documents/Target/Third.txt")<0);
    /* Hovering a collapsed tree folder expands it after one second (Explorer). */
    node=-1; for (unsigned i=0;i<app.tree_count;i++) if (!strcmp(app.tree[i].path,"/Documents/Target")) node=(int)i;
    assert(node>=0 && app.tree[node].children && !app.tree[node].expanded);
    nl=du_list_label(TREE(),(unsigned)node);
    over(13,nl.x+4,nl.y+nl.h/2,0); ui.now+=600000000ull; over(13,nl.x+4,nl.y+nl.h/2,0); assert(!app.tree[node].expanded);
    ui.now+=500000000ull; over(13,nl.x+4,nl.y+nl.h/2,0); files_test_wait(&app);
    int expanded=0; for (unsigned i=0;i<app.tree_count;i++) expanded|=!strcmp(app.tree[i].path,"/Documents/Target/Inner"); assert(expanded);
    files_drag_over(&app,13,1,0,0,0);
    /* The source refreshes after a move performed elsewhere. */
    files_drag_end(&app,FM_EFFECT_MOVE); assert(app.drag_refresh_until>ui.now);
    puts("PASS fx-19 Files drop target: same/other drive default, Ctrl/Shift/Ctrl+Shift/Alt, CD copy-only, folder into itself, own folder no-op, list/tree/background targets and highlight, hover expansion, source refresh");

    /* Rubber band in every view: plain, Ctrl toggles, Shift adds. */
    for (int k=0;k<6;k++) { char name[32]; snprintf(name,sizeof(name),"/Documents/Band%d.txt",k); fake_file(name,10,(unsigned char)k); }
    assert(!files_navigate(&app,"/Documents"));
    enum du_view views[3]={DU_DETAILS,DU_LIST,DU_ICONS}; const char *frame[3]={"band-details","band-list","band-icons"};
    for (int v=0;v<3;v++) {
        LIST()->view=views[v]; LIST()->scroll=LIST()->hscroll=0; files_update(&app); du_render(&ui);
        int a=list_find("Band1.txt"), b=list_find("Band3.txt"); assert(a>=0 && b>=0);
        struct du_rect ra=du_list_label(LIST(),(unsigned)a), rb=du_list_label(LIST(),(unsigned)b);
        body=du_list_body(LIST());
        /* Start in empty space (right of the names in details/list, below the icons). */
        int sx=views[v]==DU_ICONS ? body.x+body.w-6 : body.x+body.w-8, sy=views[v]==DU_ICONS ? body.y+body.h-6 : ra.y+2;
        if (views[v]==DU_LIST) { sx=ra.x+ra.w+6; sy=ra.y+2; }
        clock_ns+=600000000ull;
        band(sx,sy,views[v]==DU_ICONS ? ra.x+2 : rb.x+4,views[v]==DU_ICONS ? ra.y+2 : rb.y+rb.h-2,0);
        assert(LIST()->band); unsigned n=fm_selected(&app.model,NULL);
        assert(n>=2 && app.model.entries[a].selected);
        if (views[v]!=DU_ICONS) assert(app.model.entries[b].selected);
        dump(frame[v]);
        release(1); assert(!LIST()->band && fm_selected(&app.model,NULL)==n);
        /* Ctrl band over the same items toggles them off; Shift band adds back. */
        clock_ns+=600000000ull;
        band(sx,sy,views[v]==DU_ICONS ? ra.x+2 : rb.x+4,views[v]==DU_ICONS ? ra.y+2 : rb.y+rb.h-2,DU_KEY_LCTRL); release(1);
        assert(fm_selected(&app.model,NULL)==0);
        int c=list_find("Band5.txt"); fm_select(&app.model,(unsigned)c,0); files_update(&app);
        clock_ns+=600000000ull;
        band(sx,sy,views[v]==DU_ICONS ? ra.x+2 : rb.x+4,views[v]==DU_ICONS ? ra.y+2 : rb.y+rb.h-2,DU_KEY_LSHIFT); release(1);
        assert(fm_selected(&app.model,NULL)==n+1 && app.model.entries[c].selected);
        /* A plain click in empty space clears the selection. */
        clock_ns+=600000000ull; click(sx,sy); assert(fm_selected(&app.model,NULL)==0);
    }
    LIST()->view=DU_DETAILS; files_update(&app);
    /* The band auto-scrolls past the bottom edge. */
    for (int k=0;k<30;k++) { char name[32]; snprintf(name,sizeof(name),"/Documents/More%02d.txt",k); fake_file(name,1,1); }
    assert(!files_navigate(&app,"/Documents"));
    body=du_list_body(LIST()); clock_ns+=600000000ull;
    motion(body.x+body.w-8,body.y+4); press(1); motion(body.x+30,body.y+body.h+20);
    int before=LIST()->scroll; clock_ns+=60000000ull; du_tick(&ui,clock_ns); clock_ns+=60000000ull; du_tick(&ui,clock_ns);
    assert(LIST()->scroll>before && LIST()->band); release(1);
    assert(fm_selected(&app.model,NULL)>(unsigned)(body.h/DU_ROW));
    puts("PASS fx-19 Files rubber band: details/list/icons, plain/Ctrl toggle/Shift add, empty click clears, auto-scroll past the edge, dotted band frames");

    /* Keyboard selection: Ctrl+A, Shift+arrows extend from the anchor, Ctrl+arrows move the focus, Ctrl+Space toggles. */
    du_focus(&ui,107); clock_ns+=600000000ull;
    chord(DU_KEY_LCTRL,0x1e); assert(fm_selected(&app.model,NULL)==app.model.count);
    key(DU_KEY_HOME); assert(fm_selected(&app.model,NULL)==1 && app.model.active==0);
    event(CIUKI_INPUT_KEY,DU_KEY_LSHIFT,1,0); key(DU_KEY_DOWN); key(DU_KEY_DOWN); event(CIUKI_INPUT_KEY,DU_KEY_LSHIFT,0,0);
    assert(fm_selected(&app.model,NULL)==3 && app.model.active==2);
    event(CIUKI_INPUT_KEY,DU_KEY_LCTRL,1,0); key(DU_KEY_DOWN); key(DU_KEY_DOWN); key(DU_KEY_SPACE); event(CIUKI_INPUT_KEY,DU_KEY_LCTRL,0,0);
    assert(fm_selected(&app.model,NULL)==4 && app.model.entries[4].selected && !app.model.entries[3].selected);
    puts("PASS fx-19 Files keyboard selection: Ctrl+A, Shift+arrows, Ctrl+arrows, Ctrl+Space");
    files_destroy(&app); fake_assert_clean();
}

int main(int argc, char **argv)
{
    out_dir = argc > 1 ? argv[1] : NULL;
    portrait_path = argc > 2 ? argv[2] : NULL;
    pixels = calloc(WIDTH * HEIGHT + 1, 4); assert(pixels);
    pixels[WIDTH * HEIGHT] = 0xdeadbeefu;
    fake_image_tree();
    du_init(&ui, pixels, WIDTH, HEIGHT);
    app.test = 1; app.checkpoint = on_checkpoint; app.close = on_close; app.spawn = on_spawn; app.portrait = portrait;
    assert(!files_setup(&app, &ui, &fake_vfs, "/"));
    assert(!strcmp(app.model.path, "/") && app.model.count == 4);
    dump("root-details");
    script_coordinates();

    /* View > List through the menu bar (the QEMU script does the same). */
    struct du_rect view = du_menu_title(&ui, 2);
    click(view.x + view.w / 2, view.y + view.h / 2);
    assert(ui.menu == 2);
    dump("menu-view");
    struct du_rect vbox = du_menu_box(&ui, app.view_items, 10, view.x, view.y + view.h);
    click(vbox.x + 40, vbox.y + 3 + 17 + 8);
    assert(ui.menu < 0 && du_get(&ui, 107)->view == DU_LIST);

    /* Places > System: checkpoint 1. */
    struct du_widget *places = du_get(&ui, 103);
    struct du_rect system = du_list_label(places, 3);
    click(system.x + system.w / 2, system.y + system.h / 2);
    assert(fm_same_path(app.model.path, "/system") && checkpoint_count == 1 && checkpoints[0] == 1);
    dump("cp1-system");

    /* File > New Folder: checkpoint 2, inline rename open. */
    struct du_rect file = du_menu_title(&ui, 0);
    click(file.x + file.w / 2, file.y + file.h / 2);
    struct du_rect fbox = du_menu_box(&ui, app.file_items, 11, file.x, file.y + file.h);
    dump("menu-file");
    click(fbox.x + 40, fbox.y + 3 + 17 + 17 + 9 + 8);
    assert(checkpoint_count == 2 && app.rename_index >= 0 && fake_lookup("/system/New Folder") >= 0);
    dump("cp2-new-folder");

    /* Type "Ciuki" (Shift+C) and Enter: checkpoint 3. */
    chord(DU_KEY_LSHIFT, 0x2e); key(0x17); key(0x16); key(0x25); key(0x17); key(DU_KEY_ENTER);
    assert(checkpoint_count == 3 && fake_lookup("/system/Ciuki") >= 0 && fake_lookup("/system/New Folder") < 0);
    dump("cp3-renamed");

    /* b selects BOOT.CFG, Ctrl+C, c selects Ciuki, Enter opens, Ctrl+V. */
    key(0x30); assert(!strcmp(app.model.entries[app.model.active].name, "BOOT.CFG"));
    chord(DU_KEY_LCTRL, 0x2e); assert(app.model.clipboard_count == 1);
    key(0x2e); assert(!strcmp(app.model.entries[app.model.active].name, "Ciuki"));
    struct du_event open_batch={.type=DU_EV_OPEN,.id=107,.index=(int)app.model.active};
    raw_files_event(&ui,&open_batch,&app);assert(app.loading);
    struct du_event paste_batch={.type=DU_EV_ACTIVATE,.id=6};
    raw_files_event(&ui,&paste_batch,&app);assert(app.pending_command==6);
    files_test_wait(&app);
    assert(fm_same_path(app.model.path, "/system/Ciuki") && app.model.count == 0 && app.job_active);
    dump("copy-progress");
    tick();
    assert(!app.job_active && checkpoint_count == 4 && fake_lookup("/system/Ciuki/BOOT.CFG") >= 0);
    assert(app.model.count == 1 && app.model.entries[0].selected);
    dump("cp4-copied");

    /* Del, confirm with Enter: checkpoint 5. */
    key(DU_KEY_DELETE); assert(du_dialog_id(&ui));
    dump("confirm-delete");
    key(DU_KEY_ENTER); tick();
    assert(checkpoint_count == 5 && fake_lookup("/system/Ciuki/BOOT.CFG") < 0 && fake_lookup("/system/BOOT.CFG") >= 0);
    dump("cp5-deleted");
    for (int i = 0; i < 5; i++) assert(checkpoints[i] == i + 1);

    /* Backspace goes up and selects the folder that was left. */
    key(DU_KEY_BACKSPACE); assert(fm_same_path(app.model.path, "/system") && !strcmp(app.model.entries[app.model.active].name, "Ciuki"));

    /* Views, sorting, selection keys. */
    click(du_get(&ui, 9)->box.x + 10, du_get(&ui, 9)->box.y + 10);
    assert(du_get(&ui, 107)->view == DU_DETAILS);
    struct du_widget *list = du_get(&ui, 107);
    dump("system-details");
    int size_header_x = list->box.x + 2 + list->columns[0].width + 10;
    click(size_header_x, list->box.y + 8);
    assert(app.model.sort == 1 && !app.model.descending);
    click(size_header_x, list->box.y + 8);
    assert(app.model.sort == 1 && app.model.descending);
    dump("sort-size-desc");
    click(list->box.x + 2 + 10, list->box.y + 8);
    assert(app.model.sort == 0);
    chord(DU_KEY_LCTRL, 0x1e); assert(fm_selected(&app.model, NULL) == app.model.count);
    key(DU_KEY_HOME); assert(app.model.active == 0 && fm_selected(&app.model, NULL) == 1);
    chord(DU_KEY_LSHIFT, DU_KEY_DOWN); event(CIUKI_INPUT_KEY, DU_KEY_LSHIFT, 1, 0); key(DU_KEY_DOWN); event(CIUKI_INPUT_KEY, DU_KEY_LSHIFT, 0, 0);
    assert(fm_selected(&app.model, NULL) == 3);
    int vmm = list_find("VMM.ELF");
    event(CIUKI_INPUT_KEY, DU_KEY_LCTRL, 1, 0); click(list->box.x + 40, list_row_y(list, (unsigned)vmm)); event(CIUKI_INPUT_KEY, DU_KEY_LCTRL, 0, 0);
    assert(fm_selected(&app.model, NULL) == 4 && app.model.entries[vmm].selected);
    dump("multi-select");
    click(du_get(&ui, 9)->box.x + 10, du_get(&ui, 9)->box.y + 10);
    assert(list->view == DU_ICONS);
    dump("system-icons");
    rclick(list->box.x + list->box.w - 30, list->box.y + list->box.h - 30);
    assert(ui.popup && fm_selected(&app.model, NULL) == 0);
    dump("context-background");
    key(DU_KEY_ESC); assert(!ui.popup);

    /* Item context menu, Properties of a folder, then the replace prompt. */
    int assets = list_find("assets");
    struct du_rect ar = du_list_label(list, (unsigned)assets);
    rclick(ar.x + ar.w / 2, ar.y - 10);
    assert(ui.popup && app.model.entries[assets].selected);
    dump("context-item");
    key(DU_KEY_ESC);
    chord(DU_KEY_LALT, DU_KEY_ENTER); tick();
    assert(du_dialog_id(&ui));
    dump("properties");
    key(DU_KEY_ENTER); assert(!du_dialog_id(&ui));
    int boot = list_find("BOOT.CFG");
    click(du_list_label(list, (unsigned)boot).x + 4, du_list_label(list, (unsigned)boot).y + 4);
    chord(DU_KEY_LCTRL, 0x2e);
    fake_file("/system/Ciuki/BOOT.CFG", 5, 'z');
    int ciuki = list_find("Ciuki");
    struct du_rect cr = du_list_label(list, (unsigned)ciuki);
    rclick(cr.x + cr.w / 2, cr.y - 10);
    /* "Paste" in the folder's context menu pastes into it. */
    unsigned paste = 0; for (unsigned i = 0; i < ui.popup_count; i++) if (ui.popup[i].label && !strcmp(ui.popup[i].label, "&Paste")) paste = i;
    struct du_rect pbox = du_menu_box(&ui, ui.popup, ui.popup_count, ui.popup_x, ui.popup_y);
    int y = pbox.y + 3; for (unsigned i = 0; i < paste; i++) y += ui.popup[i].flags & DU_ITEM_SEPARATOR ? 9 : 17;
    click(pbox.x + 30, y + 8);
    tick();
    assert(du_dialog_id(&ui) && app.job.state == FM_REPLACE);
    dump("replace-prompt");
    key(DU_KEY_ENTER); tick();
    assert(!app.job_active && fake_nodes[fake_lookup("/system/Ciuki/BOOT.CFG")].meta.size == 16);

    /* Same-folder paste makes "Copy of", Rename via F2, error on a bad name. */
    chord(DU_KEY_LCTRL, 0x2f); tick();
    assert(fake_lookup("/system/Copy of BOOT.CFG") >= 0);
    key(DU_KEY_F2); assert(app.rename_index >= 0);
    chord(DU_KEY_LSHIFT, 0x35); key(DU_KEY_ENTER);  /* "?" is invalid */
    assert(du_dialog_id(&ui) && fake_lookup("/system/Copy of BOOT.CFG") >= 0);
    dump("rename-error");
    key(DU_KEY_ENTER);

    /* Tree: collapse the root, expand again; address bar navigation. */
    struct du_widget *tree = du_get(&ui, 105);
    dump("tree");
    chord(DU_KEY_LALT, 0x20); assert(du_focused(&ui) == 100);
    const char *programs = "/Programs";
    for (const char *p = programs; *p; p++) {
        static const unsigned char map[128] = {['/'] = 0x35, ['p'] = 0x19, ['r'] = 0x13,
            ['o'] = 0x18, ['g'] = 0x22, ['a'] = 0x1e, ['m'] = 0x32, ['s'] = 0x1f};
        if (*p >= 'A' && *p <= 'Z') chord(DU_KEY_LSHIFT, map[(unsigned char)(*p - 'A' + 'a')]);
        else key(map[(unsigned char)*p]);
    }
    key(DU_KEY_ENTER); assert(!strcmp(app.model.path, "/Programs") && app.model.count == 6);
    assert(tree->count >= 3);
    dump("programs-icons");
    int hello = list_find("hello");
    app.model.active = (unsigned)hello; fm_select(&app.model, (unsigned)hello, 0); files_update(&app);
    key(DU_KEY_ENTER); assert(spawned == 1);
    /* f2-32: the desktop's refusal of a LAUNCH request (CiukNote) is reported in a dialog. */
    files_launch_failed(&app, EPERM);
    assert(ui.dialog.kind == DU_DLG_MESSAGE && strstr(ui.dialog.text, "Cannot start the application:") && strstr(ui.dialog.text, "Access is denied"));
    key(DU_KEY_ENTER); assert(!ui.dialog.kind);
    /* Help and About dialogs. */
    key(DU_KEY_F1); dump("help"); key(DU_KEY_ESC);
    struct du_rect help = du_menu_title(&ui, 4); click(help.x + 5, help.y + 5);
    struct du_rect hbox = du_menu_box(&ui, app.help_items, 3, help.x, help.y + help.h);
    click(hbox.x + 20, hbox.y + 3 + 17 + 9 + 8);
    dump("about"); key(DU_KEY_ENTER);
    /* A close request during a job cancels it, cleans up, then closes. */
    chord(DU_KEY_LCTRL, 0x1e); chord(DU_KEY_LCTRL, 0x2e);
    assert(!files_navigate(&app, "/system/Ciuki")); chord(DU_KEY_LCTRL, 0x2f); assert(app.job_active);
    assert(!files_close_request(&app) && !closed);
    assert(strstr(ui.dialog.title,"Cancel Operation"));
    struct du_event confirm={.type=DU_EV_DIALOG,.id=ui.dialog.id,.index=0};
    du_dialog_close(&ui); files_event(&ui,&confirm,&app);
    tick(); assert(closed && !app.job_active && app.job.state == FM_CANCELLED);
    closed = 0;
    /* Alt+F4 closes. */
    chord(DU_KEY_LALT, DU_KEY_F4); assert(closed);
    files_destroy(&app);
    fake_assert_clean();
    computer_ui_tests();
    removable_ui_tests();
    external_removal_tests();
    label_cache_tests();
    blocker_dialog_tests();
    /* Default associations run through the actual Files controller. */
    fake_image_tree(); fake_file("/Programs/paint",100,1); fake_file("/Programs/viewer",100,1); fake_file("/Programs/music",100,1);
    fake_file("/Documents/Test.BMP",100,1); fake_file("/Documents/Other.PNG",100,1); fake_file("/Documents/Photo.jpg",100,1);
    du_init(&ui,pixels,WIDTH,HEIGHT); memset(&app,0,sizeof(app));
    assert(!files_setup(&app,&ui,&fake_vfs,"/Documents")); app.spawn=on_spawn;
    const char *names[]={"Test.BMP","Other.PNG","Photo.jpg"};
    for (unsigned i=0;i<3;i++) {
        int row=list_find(names[i]); assert(row>=0); app.model.active=(unsigned)row;
        fm_select(&app.model,(unsigned)row,0); files_update(&app); key(DU_KEY_ENTER);
        assert(!ui.dialog.kind && !strcmp(launched,"/Programs/viewer"));
        assert(strstr(launch_path,names[i]));
    }
    const char *audio[]={"Sound.WAV","Track.MP3","Song.OGG","List.M3U"};
    for (unsigned i=0;i<4;i++) {
        char path[FM_PATH]; snprintf(path,sizeof(path),"/Documents/%s",audio[i]); fake_file(path,100,1);
    }
    fake_create("/Documents/Music",FM_DIR); assert(!files_navigate(&app,"/Documents"));
    for (unsigned i=0;i<4;i++) {
        int row=list_find(audio[i]); assert(row>=0); fm_select(&app.model,(unsigned)row,0); files_update(&app); key(DU_KEY_ENTER);
        assert(!ui.dialog.kind && !strcmp(launched,"/Programs/music") && strstr(launch_path,audio[i]));
    }
    int folder=list_find("Music"); assert(folder>=0);
    struct du_event context={.type=DU_EV_CONTEXT,.id=107,.index=folder,.x=200,.y=200};
    files_event(&ui,&context,&app);
    int music_command=0;
    for (unsigned i=0;i<app.popup_count;i++) if (app.popup[i].label && strstr(app.popup[i].label,"Music Player")) music_command=app.popup[i].id;
    assert(music_command); du_popup(&ui,NULL,0,0,0,0);
    struct du_event play_folder={.type=DU_EV_ACTIVATE,.id=music_command}; files_event(&ui,&play_folder,&app);
    assert(!strcmp(launched,"/Programs/music") && !strcmp(launch_path,"/Documents/Music"));
    /* Desktop navigation remains distinct from the music context command. */
    assert(music_command!=303); fake_create("/Documents/Desktop",FM_DIR);
    activate(303); assert(!strcmp(app.model.path,"/Documents/Desktop"));
    files_destroy(&app); fake_assert_clean();
    file_drag_tests();
    free(pixels);
    puts("PASS Files UI: script checkpoints 1-5, views, sorting, selection, context menus, properties, replace, Copy of, rename errors, tree, address, open, help/about");
    return 0;
}
