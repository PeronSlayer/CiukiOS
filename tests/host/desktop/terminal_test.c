/* SPDX-License-Identifier: MIT */
#include "../../../apps/terminal/terminal.h"
#include "fake_vfs.h"
#include "acc_test.h"
#include <errno.h>
static struct terminal app;
static struct du_ui ui;
static uint32_t pixels[TERM_WIDTH*TERM_HEIGHT], resized[800*600];
static void selection_frame(const char *name)
{
    const char *prefix=getenv("CIUKI_TERMINAL_SELECTION"); if(!prefix)return;
    char path[1200]; snprintf(path,sizeof(path),"%s-%s.xrgb",prefix,name); du_render(&ui);
    FILE *f=fopen(path,"wb"); assert(f && fwrite(pixels,4,TERM_WIDTH*TERM_HEIGHT,f)==TERM_WIDTH*TERM_HEIGHT && !fclose(f));
}
static unsigned disktool_calls;
static int disktool(void *ctx,int n,char **a)
{ (void)ctx; assert(n>=1 && (fm_same_path(a[0],"BACKUP") || fm_same_path(a[0],"RESTORE") || fm_same_path(a[0],"FDISK"))); disktool_calls++; return 0; }
static int device_error, cancel_after, pumps, launches, kills, powers, exits;
static char launched[64], argument[FM_PATH];
static uint32_t killed, powered;
static struct ciuki_mixer_master master={.version=1,.size=32,.left=65,.right=60};
static int net(void *ctx,struct ciuki_net_state *s)
{ (void)ctx; if(device_error)return device_error; s->present=1; strcpy(s->driver,"rtl8139"); s->mac[0]=0x52; s->mac[5]=0xab; s->link=CIUKI_NET_LINK_UP; s->state=CIUKI_NET_RUNNING; strcpy(s->reason,"carrier"); return 0; }
static int mixer(void *ctx,struct ciuki_mixer_master *s) { (void)ctx; if(device_error)return device_error; *s=master; return 0; }
static int volume(void *ctx,uint32_t l,uint32_t r,uint32_t mute) { (void)ctx; master.left=l; master.right=r; master.mute=mute; return 0; }
static int processes(void *ctx,void *buffer,unsigned size)
{
    (void)ctx; assert(size>=144); if(device_error)return device_error;
    struct ciuki_proc_system s={.version=1,.size=64,.records=1,.processes=1,.usable_pages=32768,.free_pages=22000,.kheap_pages=12,.kheap_bytes=23000};
    struct ciuki_proc_record r={.version=1,.size=80,.pid=42,.ppid=2,.state=CIUKI_PROC_LIVE,.resident_pages=77}; strcpy(r.name,"ciuknote");
    memcpy(buffer,&s,sizeof(s)); memcpy((char *)buffer+sizeof(s),&r,sizeof(r)); return 1;
}
static int display(void *ctx,struct ciuki_display_info *s)
{ (void)ctx; if(device_error)return device_error; s->width=1024; s->height=768; s->bpp=32; s->mode_count=1;
  s->modes[0]=(struct ciuki_display_mode){.id=0x118,.width=1024,.height=768,.bpp=32,.current=1}; return 0; }
static int launch(void *ctx,const char *p,const char *a) { (void)ctx; launches++; snprintf(launched,sizeof(launched),"%s",p); snprintf(argument,sizeof(argument),"%s",a ? a : ""); return 0; }
static int kill(void *ctx,uint32_t pid) { (void)ctx; kills++; killed=pid; return pid==1 ? -EPERM : 0; }
static int power(void *ctx,unsigned action) { (void)ctx; powers++; powered=action; return 0; }
static time_t now(void *ctx) { (void)ctx; return 1791892800; }
static void quit(void *ctx) { (void)ctx; exits++; }
static void pump(void *ctx) { (void)ctx; if(cancel_after && ++pumps==cancel_after)at_chord(&ui,DU_KEY_LCTRL,0x2e); }
static void file(const char *path,const char *s)
{ int n=fake_file(path,(unsigned)strlen(s),0); memcpy(fake_nodes[n].data,s,strlen(s)); }
static void command(const char *s,int expected,const char *part)
{
    int error; char *out=term_capture(&app,s,&error);
    if(error!=expected || !out || (part && !strstr(out,part))) { fprintf(stderr,"command [%s]: error=%d expected=%d output=[%s]\n",s,error,expected,out ? out : "NULL"); abort(); }
    free(out); fake_assert_clean();
}
static void submit(const char *s) { assert(strlen(s)<sizeof(app.input)); strcpy(app.input,s); app.cursor=strlen(s); at_tap(&ui,DU_KEY_ENTER); }
static int output_has(const char *s)
{ for(unsigned i=0;i<app.count;i++)if(strstr(app.lines[(app.first+i)%TERM_SCROLLBACK],s))return 1; return strstr(app.pending,s)!=NULL; }
static void builtins(void)
{
    assert(!term_execute(&app,"EXIT") && exits==1); app.quit=0;
    command("HELP",0,"DIR/ls"); command("VER",0,"Classic computing"); command("ECHO hello world",0,"hello world\n");
    command("echo \"quoted path\"",0,"quoted path"); command("DIR",0,"one.txt"); command("ls *.TXT",0,"one.txt"); command("DIR *.*",0,"plain");
    command("CD",0,"C:\\Documents"); command("cd sub",0,NULL); assert(!strcmp(app.cwd,"/Documents/sub"));
    command("CD ..",0,NULL); command("cd C:\\Documents\\sub\\..",0,NULL); assert(!strcmp(app.cwd,"/Documents"));
    command("CD Computer",0,NULL); command("DIR",0,"D:"); command("D:",0,NULL); assert(!strcmp(app.cwd,"/mnt/d"));
    command("cd ..",0,NULL); assert(!strcmp(app.cwd,"/mnt/d")); command("C:",0,NULL); command("cd ..",0,NULL); assert(!strcmp(app.cwd,"/"));
    command("CD /Documents",0,NULL); command("cd C:Documents",-EINVAL,NULL); command("cd one.txt",-ENOTDIR,NULL);
    command("TYPE one.txt",0,"first\n"); command("cat \"space name.txt\"",0,"spaces\n");
    command("MD newdir",0,NULL); command("mkdir other",0,NULL); command("COPY *.txt newdir",0,NULL);
    assert(fake_lookup("/Documents/newdir/one.txt")>=0 && fake_lookup("/Documents/newdir/space name.txt")>=0);
    command("cp one.txt renamed.txt",0,NULL); assert(fake_lookup("/Documents/renamed.txt")>=0);
    command("COPY one.txt renamed.txt",-EEXIST,NULL); command("COPY one.txt newdir",-EEXIST,NULL);
    command("MOVE renamed.txt other",0,NULL); command("mv other/renamed.txt moved.txt",0,NULL);
    command("REN moved.txt final.txt",0,NULL); command("REN final.txt one.txt",-EEXIST,NULL);
    command("MOVE final.txt D:\\other-name.txt",0,NULL); assert(fake_lookup("/mnt/d/other-name.txt")>=0 && fake_lookup("/Documents/final.txt")<0);
    command("rm newdir/*.txt",0,NULL); command("RD newdir",0,NULL); command("rmdir other",0,NULL);
    command("DEL plain",0,NULL); command("RD sub",-ENOTEMPTY,NULL); command("DEL sub",-EISDIR,NULL);
    command("COPY sub cloned",0,NULL); assert(fake_lookup("/Documents/cloned/child")>=0);
    command("MOVE cloned D:\\cloned",0,NULL); assert(fake_lookup("/mnt/d/cloned/child")>=0 && fake_lookup("/Documents/cloned")<0);
    command("TYPE missing",-ENOENT,NULL); command("COPY one.txt",-EINVAL,NULL); command("ECHO \"bad",-EINVAL,NULL);
    command("RUN hello",0,"not available in this release"); command("REG",0,"reserved"); command("mystery",-ENOSYS,NULL);
    command("START ciuknote \"space name.txt\"",0,"Launch requested"); assert(launches==1 && !strcmp(launched,"ciuknote") && !strcmp(argument,"/Documents/space name.txt"));
    command("START /Programs/tasks",0,NULL); command("START ../tasks",-EINVAL,NULL);
    command("clear",0,NULL); assert(app.count>0); /* captures do not clear the visible grid */
    assert(!term_execute(&app,"CLS") && !app.count); term_write(&app,"a\n"); assert(!term_execute(&app,"clear") && !app.count);
    fake_short_io=1; command("COPY one.txt short.txt",0,NULL); command("TYPE short.txt",0,"first\n"); fake_short_io=0;
    fake_countdown=3; command("COPY one.txt failed.txt",-EIO,NULL); fake_countdown=0; assert(fake_lookup("/Documents/failed.txt")<0);
    for(int fault=1;fault<=45;fault++) {
        fake_countdown=fault; int r=term_execute(&app,"COPY one.txt fault.txt"); fake_countdown=0;
        assert(r==0 || r==-EIO); assert(fake_lookup("/Documents/one.txt")>=0); fake_assert_clean();
        for(int i=0;i<FAKE_NODES;i++)if(fake_nodes[i].live)assert(!strstr(fake_nodes[i].path,".terminal-copy-"));
        int n=fake_lookup("/Documents/fault.txt"); if(n>=0) { assert(fake_nodes[n].meta.size==6 && !memcmp(fake_nodes[n].data,"first\n",6)); assert(!fake_vfs.unlink(NULL,"/Documents/fault.txt")); }
    }
    command("MD C:\\",-EPERM,NULL); command("RD D:",-EPERM,NULL); command("cp sub sub/nested",-EINVAL,NULL);
}
static void system_commands(void)
{
    command("NET",0,"rtl8139"); command("net",0,"52:00:00:00:00:ab"); command("NET",0,"Carrier: up");
    command("AUDIO",0,"L 65 R 60"); command("AUDIO volume 33",0,NULL); assert(master.left==33 && master.right==33);
    command("AUDIO mute",0,NULL); assert(master.mute); command("AUDIO unmute",0,NULL); assert(!master.mute);
    command("AUDIO volume 101",-EINVAL,NULL); command("audio volume rubbish",-EINVAL,NULL);
    command("TASKS",0,"ciuknote"); command("TASKS KILL 42",0,NULL); assert(kills==1 && killed==42);
    command("TASKS KILL 1",-EPERM,NULL); command("TASKS KILL 0",-EINVAL,NULL); command("TASKS KILL -1",-EINVAL,NULL);
    command("MEM",0,"131072 KiB"); command("DISPLAY",0,"1024x768 32 bpp"); command("DISPLAY",0,"0118");
    command("DATE",0,"2026-10-13"); command("TIME",0,":");
    device_error=-ENODEV; command("NET",-ENODEV,NULL); command("AUDIO",-ENODEV,NULL); command("TASKS",-ENODEV,NULL); command("DISPLAY",-ENODEV,NULL); device_error=0;
    assert(!term_execute(&app,"SHUTDOWN") && !powers && du_dialog_id(&ui)==50); at_tap(&ui,DU_KEY_ESC); assert(!powers);
    assert(!term_execute(&app,"REBOOT")); ui.event(&ui,&(struct du_event){.type=DU_EV_DIALOG,.id=50,.index=0},ui.user); du_dialog_close(&ui);
    assert(powers==1 && powered==CIUKI_POWER_REBOOT);
    assert(!term_execute(&app,"SHUTDOWN")); ui.event(&ui,&(struct du_event){.type=DU_EV_DIALOG,.id=50,.index=0},ui.user); du_dialog_close(&ui);
    assert(powers==2 && powered==CIUKI_POWER_OFF); command("SHUTDOWN",-EPERM,NULL);
}
static void lua_and_ui(void)
{
    file("/Documents/test.lua","assert(arg[1]=='hello'); term.print(term.run('ECHO native')); print('script', 42)\n");
    /* A capture cannot nest term.run; use the visible application path. */
    assert(!term_execute(&app,"LUA test.lua hello")); assert(output_has("native") && output_has("script"));
    file("/Documents/auto.lua","term.print('auto file')\n"); assert(!term_execute(&app,"auto.lua")); assert(output_has("auto file"));
    file("/Documents/recurse.lua","local s, e=term.run('LUA auto.lua'); assert(e < 0); print('no recursion')");
    assert(!term_execute(&app,"LUA recurse.lua")); assert(output_has("no recursion"));
    submit("LUA"); assert(app.lua_mode); submit("x=40"); submit("x+2"); assert(output_has("42"));
    submit("for i=1,2 do"); assert(app.chunk_length); submit("term.print(i)"); submit("end"); assert(!app.chunk_length);
    submit("this is ???"); assert(output_has("Lua:"));
    cancel_after=2; pumps=0; submit("while true do end"); cancel_after=0; assert(app.interrupted && output_has("interrupted"));
    submit(".exit"); assert(!app.lua_mode);
    file("/Documents/loop.lua","while true do end"); cancel_after=3; pumps=0; submit("loop.lua"); cancel_after=0; assert(app.interrupted);
    assert(app.history_count>0 && fake_lookup("/Documents/.terminal_history")>=0);
    strcpy(app.input,"TYPE spa"); app.cursor=strlen(app.input); at_tap(&ui,DU_KEY_TAB); assert(!strcmp(app.input,"TYPE \"space name.txt\""));
    strcpy(app.input,"CD su"); app.cursor=strlen(app.input); at_tap(&ui,DU_KEY_TAB); assert(!strcmp(app.input,"CD sub/"));
    submit("ECHO recalled"); at_tap(&ui,DU_KEY_UP); assert(!strcmp(app.input,"ECHO recalled")); at_tap(&ui,DU_KEY_DOWN); assert(!app.input[0]);
    /* Actual pointer capture and Ctrl+C use production toolkit dispatch. */
    term_execute(&app,"CLS"); term_write(&app,"selection\n");
    at_move(&ui,4,5); struct ciuki_input_event e={.type=CIUKI_INPUT_BUTTON,.code=1,.value=1}; du_input(&ui,&e);
    at_move(&ui,4+9*TERM_CELL_W,5); e.value=0; du_input(&ui,&e); at_chord(&ui,DU_KEY_LCTRL,0x2e); assert(!strcmp(du_clipboard_get(),"selection"));
    du_clipboard_set("ECHO pasted"); at_chord(&ui,DU_KEY_LCTRL,0x2f); assert(!strcmp(app.input,"ECHO pasted"));
    at_tap(&ui,DU_KEY_ENTER); assert(output_has("pasted"));
    /* Command editing uses the same selection and clipboard rules as fields. */
    du_clipboard_set("alpha beta"); at_chord(&ui,DU_KEY_LCTRL,0x2f);
    at_chord(&ui,DU_KEY_LSHIFT,DU_KEY_HOME); assert(app.anchor==10 && app.cursor==0);
    selection_frame("command");
    at_chord(&ui,DU_KEY_LCTRL,0x2e); assert(!strcmp(du_clipboard_get(),"alpha beta"));
    at_tap(&ui,0x2c); assert(!strcmp(app.input,"z") && app.anchor==app.cursor);
    at_chord(&ui,DU_KEY_LCTRL,0x1e); at_chord(&ui,DU_KEY_LCTRL,0x2d); assert(!app.input[0]);
    at_chord(&ui,DU_KEY_LCTRL,0x2f); assert(!strcmp(app.input,"z")); at_tap(&ui,DU_KEY_ESC);
    term_execute(&app,"CLS"); term_write(&app,"alpha beta\n"); app.top=0; app.follow=0;
    ui.now+=1000000000ull; at_click(&ui,4+7*TERM_CELL_W,5);
    ui.now+=100000000ull; at_click(&ui,4+7*TERM_CELL_W,5);
    assert(app.selection_a==6 && app.selection_b==10);
    selection_frame("word");
    ui.now+=100000000ull; at_click(&ui,4+7*TERM_CELL_W,5);
    assert(app.selection_a==0 && app.selection_b==app.columns);
    selection_frame("line");
    at_chord(&ui,DU_KEY_LCTRL,0x2d); assert(output_has("alpha beta") && !strcmp(du_clipboard_get(),"alpha beta\n"));
    at_tap(&ui,DU_KEY_DELETE); assert(output_has("alpha beta"));
    at_chord(&ui,DU_KEY_LSHIFT,DU_KEY_UP); assert(app.selection_b==0);
    app.selection_a=app.selection_b=0; app.output_focus=0; app.follow=1;
    puts("terminal selection: PASS command Shift/Home/End, select-all, cut/copy/paste, replacement; output word/line clicks, keyboard extension and read-only cut/delete");
    for(unsigned i=0;i<1100;i++)term_printf(&app,"row %u\n",i);
    assert(app.count==TERM_SCROLLBACK); char row[TERM_LINE+1]; term_row(&app,0,row,sizeof(row)); assert(!strcmp(row,"row 100"));
    unsigned old_top=app.top; at_tap(&ui,DU_KEY_PGUP); assert(app.top<old_top && !app.follow); at_tap(&ui,DU_KEY_PGDN);
    du_render(&ui); uint32_t before=pixels[4*TERM_WIDTH+4]; assert(before==0x101820);
    unsigned count=app.count; du_resize(&ui,resized,800,600); assert(app.count==count && app.columns>80 && app.rows>25); du_render(&ui);
    du_resize(&ui,pixels,TERM_WIDTH,TERM_HEIGHT); assert(app.columns==80 && app.rows==25);
    term_execute(&app,"CLS"); char wide[201]; memset(wide,'a',200); wide[200]=0; term_write(&app,wide); term_write(&app,"\n"); assert(term_visual_rows(&app)==3);
    du_resize(&ui,resized,800,600); assert(term_visual_rows(&app)==2); du_resize(&ui,pixels,TERM_WIDTH,TERM_HEIGHT);
    assert(!term_history_save(&app));
    fake_countdown=3; assert(term_history_save(&app)==-EIO); fake_countdown=0; fake_assert_clean();
    assert(fake_lookup("/Documents/.terminal_history")>=0);
    unsigned history=app.history_count; term_detach(&app);
    struct term_host host={.fs=fake_vfs}; du_init(&ui,pixels,TERM_WIDTH,TERM_HEIGHT); assert(!term_attach(&app,&ui,&host)); assert(app.history_count==history);
    for(unsigned i=0;i<220;i++) { char s[32]; snprintf(s,sizeof(s),"ECHO history %u",i); submit(s); }
    assert(app.history_count==TERM_HISTORY && !strcmp(app.history[0],"ECHO history 20"));
    assert(!term_execute(&app,"EXIT") && app.quit);
}
int main(void)
{
    fake_reset(); assert(fake_create("/Documents",FM_DIR)>=0); assert(fake_create("/Documents/sub",FM_DIR)>=0);
    assert(fake_create("/mnt",FM_DIR)>=0); assert(fake_create("/mnt/d",FM_DIR)>=0); file("/Documents/one.txt","first\n"); file("/Documents/space name.txt","spaces\n"); file("/Documents/plain","plain"); file("/Documents/sub/child","child");
    struct term_host host={.fs=fake_vfs,.net=net,.mixer=mixer,.volume=volume,.processes=processes,.display=display,.launch=launch,.kill=kill,.power=power,.now=now,.pump=pump,.quit=quit};
    du_init(&ui,pixels,TERM_WIDTH,TERM_HEIGHT); assert(!term_attach(&app,&ui,&host)); assert(app.columns==80 && app.rows==25);
    builtins(); system_commands(); lua_and_ui();
    app.host.disktool=disktool;
    command("BACKUP FULL set.cbk one.txt",0,NULL); command("RESTORE set.cbk ORIGINAL OLDER",0,NULL); command("FDISK 1",0,NULL);
    assert(disktool_calls==3);
    /* Deterministic review scene: the production fixed-cell painter, accents,
     * descenders, cursor and selection. Independent of previous command output. */
    term_execute(&app,"CLS"); term_write(&app,"CiukiOS Terminal\nClassic computing, one woof at a time.\nAVATAR To Wa 0123456789 gjpqy\n");
    strcpy(app.input,"ECHO hello"); app.anchor=app.cursor=strlen(app.input); du_render(&ui);
    const char *frame=getenv("CIUKI_TERMINAL_FRAME");
    if(frame) { FILE *f=fopen(frame,"wb"); assert(f); assert(fwrite(pixels,sizeof(*pixels),TERM_WIDTH*TERM_HEIGHT,f)==TERM_WIDTH*TERM_HEIGHT); assert(!fclose(f)); }
    term_detach(&app); fake_assert_clean(); du_release(&ui);
    puts("terminal: PASS built-ins, aliases, VFS, nodes, Lua, interruption, history, completion, selection, resize, scrollback"); return 0;
}
