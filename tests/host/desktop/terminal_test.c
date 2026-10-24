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
static int native_run(struct terminal *t,int n,char **a)
{ assert(n==2 && !strcmp(a[0],"sample") && !strcmp(a[1],"two words"));term_write(t,"native child output\n");return 7; }
static int native_registry(struct terminal *t,int n,char **a)
{ assert(n==6 && !strcmp(a[1],"ADD") && !strcmp(a[4],"STRING") && !strcmp(a[5],"hello world"));term_write(t,"registry request\n");return 0; }
static int network_command(struct terminal *t,int n,char **a)
{ assert(n==2 && !strcmp(a[0],"PING") && !strcmp(a[1],"127.0.0.1"));term_write(t,"raw ICMP provider\n");return 0; }
static int kill(void *ctx,uint32_t pid) { (void)ctx; kills++; killed=pid; return pid==1 ? -EPERM : 0; }
static int power(void *ctx,unsigned action) { (void)ctx; powers++; powered=action; return 0; }
static time_t now(void *ctx) { (void)ctx; return 1791892800; }
static void quit(void *ctx) { (void)ctx; exits++; }
static void pump(void *ctx) { (void)ctx; if(cancel_after && ++pumps==cancel_after)at_chord(&ui,DU_KEY_LCTRL,0x2e); }
/* G27 fakes: attribute bits on the fake nodes, volume labels and drive space. */
static char labels[26][33]={"","","CIUKI"}; static int label_sets;
static int attributes(void *ctx,const char *path,uint32_t value)
{ (void)ctx; int n=fake_lookup(path); if(n<0)return -ENOENT; fake_nodes[n].meta.attributes=value; return 0; }
static int label_get(void *ctx,unsigned drive,char out[33]) { (void)ctx; if(drive>=26)return -EINVAL; strcpy(out,labels[drive]); return 0; }
static int label_set(void *ctx,unsigned drive,const char *label) { (void)ctx; if(drive>=26)return -EINVAL; strcpy(labels[drive],label); label_sets++; return 0; }
static int space(void *ctx,unsigned drive,uint64_t *total,uint64_t *free_bytes) { (void)ctx; if(drive>=26)return -EINVAL; *total=1000000; *free_bytes=400000; return 0; }
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
/* The output of one command line exactly as the grid receives it. */
static char *run(const char *line,int expected)
{
    int error; char *out=term_capture(&app,line,&error);
    if(error!=expected || !out) { fprintf(stderr,"run [%s]: error=%d expected=%d output=[%s]\n",line,error,expected,out ? out : "NULL"); abort(); }
    fake_assert_clean(); return out;
}
static void exact(const char *line,int expected,const char *want)
{
    char *out=run(line,expected);
    if(strcmp(out,want)) { fprintf(stderr,"exact [%s]: got [%s] want [%s]\n",line,out,want); abort(); }
    free(out);
}
static void contains(const char *line,int expected,const char *part,int present)
{
    char *out=run(line,expected);
    if((strstr(out,part)!=NULL)!=present) { fprintf(stderr,"contains [%s]: [%s] %s [%s]\n",line,part,present ? "missing from" : "found in",out); abort(); }
    free(out);
}
static const char *contents(const char *path)
{ static char buffer[4096]; int n=fake_lookup(path); assert(n>=0); size_t size=(size_t)fake_nodes[n].meta.size; assert(size<sizeof(buffer)); memcpy(buffer,fake_nodes[n].data,size); buffer[size]=0; return buffer; }
static void scripting(void)
{
    command("CD /Documents",0,NULL);
    /* Environment: SET, %VAR%, unknown names, deletion. */
    exact("SET GREETING=hello world",0,""); exact("ECHO %GREETING%",0,"hello world\n"); exact("echo %greeting%!",0,"hello world!\n");
    contains("SET",0,"GREETING=hello world",1); exact("SET GREETING",0,"GREETING=hello world\n");
    exact("ECHO %NOPE%",0,"%NOPE%\n"); command("SET NOPE",-ENOENT,"not defined");
    exact("SET GREETING=",0,""); exact("ECHO %GREETING%",0,"%GREETING%\n"); exact("ECHO 100%",0,"100%\n");
    exact("SET A=1",0,""); exact("SET B=%A%2",0,""); exact("ECHO %B% %ERRORLEVEL%",0,"12 0\n");
    /* Redirection. */
    exact("ECHO one > s_out.txt",0,""); assert(!strcmp(contents("/Documents/s_out.txt"),"one\n"));
    exact("ECHO two >> s_out.txt",0,""); assert(!strcmp(contents("/Documents/s_out.txt"),"one\ntwo\n"));
    exact("ECHO three > s_out.txt",0,""); assert(!strcmp(contents("/Documents/s_out.txt"),"three\n"));
    exact("ECHO more>>s_out.txt",0,""); assert(!strcmp(contents("/Documents/s_out.txt"),"three\nmore\n"));
    exact("ECHO \"a > b\"",0,"a > b\n");
    exact("TYPE < s_out.txt",0,"three\nmore\n"); exact("ECHO ignored < s_out.txt",0,"ignored\n");
    exact("ECHO x > sub\\s_deep.txt",0,""); assert(!strcmp(contents("/Documents/sub/s_deep.txt"),"x\n"));
    command("ECHO bad > ",-EINVAL,"Syntax error"); command("TYPE < missing.txt",-ENOENT,"missing.txt: No such file");
    command("ECHO x > sub",-EISDIR,"Is a directory");
    { int n=fake_lookup("/Documents/s_out.txt"); assert(n>=0); }
    /* Pipes and the filters. */
    file("/Documents/s_list.txt","pear\napple\nfig\nbanana\n");
    exact("TYPE s_list.txt | SORT",0,"apple\nbanana\nfig\npear\n"); exact("TYPE s_list.txt | SORT /R",0,"pear\nfig\nbanana\napple\n");
    exact("TYPE s_list.txt | FIND \"an\"",0,"banana\n"); exact("FIND /C \"a\" s_list.txt",0,"3\n");
    exact("FIND /V /I \"A\" s_list.txt",0,"fig\n"); exact("TYPE s_list.txt | SORT | FIND /N \"fig\"",0,"[3]fig\n");
    exact("SORT < s_list.txt",0,"apple\nbanana\nfig\npear\n"); exact("SORT s_list.txt",0,"apple\nbanana\nfig\npear\n");
    exact("TYPE s_list.txt | SORT > s_sorted.txt",0,""); assert(!strcmp(contents("/Documents/s_sorted.txt"),"apple\nbanana\nfig\npear\n"));
    exact("TYPE s_list.txt | MORE",0,"pear\napple\nfig\nbanana\n"); exact("MORE s_list.txt",0,"pear\napple\nfig\nbanana\n");
    exact("ECHO a b c | FIND \"b\"",0,"a b c\n"); exact("ECHO one | ECHO two",0,"two\n");
    contains("DIR | FIND \"s_list\"",0,"s_list.txt",1); contains("DIR | FIND \"s_list\"",0,"s_sorted",0);
    command("FIND \"x\"",-EINVAL,"no input"); command("SORT",-EINVAL,"no input"); command("FIND",-EINVAL,"missing");
    command("FIND /Q \"x\" s_list.txt",-EINVAL,NULL); command("TYPE nothing | SORT",0,"No such file");   /* the failing first stage's message becomes data */
    exact("ECHO \"a|b\"",0,"a|b\n");
    /* Batch files. */
    file("/Documents/s_b.bat","ECHO called %1\r\nEXIT /B 3\r\nECHO never\r\n");
    file("/Documents/s_a.bat","@ECHO OFF\r\nREM a comment\r\n:: another\r\nSET X=%1\r\nIF \"%X%\"==\"go\" GOTO yes\r\nECHO no\r\nGOTO end\r\n:yes\r\n"
         "ECHO going %1 %2 [%*]\r\nFOR %%f IN (alpha beta) DO ECHO item %%f\r\nIF EXIST s_list.txt ECHO exists\r\nIF NOT EXIST nothing.txt ECHO missing\r\n"
         "IF EXIST s_*.txt ECHO globbed\r\nCALL s_b.bat from_a\r\nIF ERRORLEVEL 3 ECHO level3\r\nIF ERRORLEVEL 4 ECHO level4\r\nIF NOT ERRORLEVEL 4 ECHO below4\r\n"
         "ECHO 100%%\r\n:end\r\nECHO done\r\n");
    exact("s_a go two",0,"going go two [go two]\nitem alpha\nitem beta\nexists\nmissing\nglobbed\ncalled from_a\nlevel3\nbelow4\n100%\ndone\n");
    exact("s_a.bat stop",0,"no\ndone\n"); exact("s_a",0,"no\ndone\n"); exact("CALL s_a go",0,"going go [go]\nitem alpha\nitem beta\nexists\nmissing\nglobbed\ncalled from_a\nlevel3\nbelow4\n100%\ndone\n");
    exact("ECHO %ERRORLEVEL%",0,"0\n");
    char *out=run("s_a go",0); free(out);
    file("/Documents/s_c.bat","ECHO hi\r\nECHO OFF\r\nECHO quiet\r\nECHO ON\r\n@ECHO shown\r\nECHO %0\r\n");
    exact("s_c",0,"C:\\Documents>ECHO hi\nhi\nC:\\Documents>ECHO OFF\nquiet\nshown\nC:\\Documents>ECHO C:\\Documents\\s_c.bat\nC:\\Documents\\s_c.bat\n");
    file("/Documents/s_shift.bat","@ECHO OFF\r\nECHO %1 %2\r\nSHIFT\r\nECHO %1 %2\r\nSHIFT\r\nECHO [%1] %*\r\n");
    exact("s_shift a b c d",0,"a b\nb c\n[c] c d\n");
    file("/Documents/s_for.bat","@ECHO OFF\r\nFOR %%a IN (*.bat) DO IF %%a==s_b.bat ECHO found %%a\r\nFOR %%x IN (1 2 3) DO ECHO n%%x\r\nFOR %%y IN (nothing*.zzz) DO ECHO none\r\n");
    exact("s_for",0,"found s_b.bat\nn1\nn2\nn3\n");
    file("/Documents/s_redir.bat","@ECHO OFF\r\nECHO from batch > s_made.txt\r\nTYPE s_made.txt | FIND \"batch\"\r\nPAUSE\r\n");
    exact("s_redir",0,"from batch\nPress any key to continue . . .\n"); assert(!strcmp(contents("/Documents/s_made.txt"),"from batch\n"));
    file("/Documents/s_loop.bat","@ECHO OFF\r\n:again\r\nGOTO again\r\n");
    contains("s_loop",-EIO,"ran too long",1);
    file("/Documents/s_rec.bat","@ECHO OFF\r\nCALL s_rec.bat\r\n");
    contains("s_rec",-EIO,"nested too deeply",1);
    file("/Documents/s_label.bat","@ECHO OFF\r\nGOTO nowhere\r\nECHO skipped\r\n");
    contains("s_label",-ENOENT,"Label not found - nowhere",1);
    file("/Documents/s_eof.bat","@ECHO OFF\r\nECHO before\r\nGOTO :EOF\r\nECHO after\r\n");
    exact("s_eof",0,"before\n");
    command("nosuch.bat",-ENOENT,"nosuch.bat"); command("GOTO x",-EINVAL,"only in a batch"); command("SHIFT",-EINVAL,NULL);
    command("CALL",-EINVAL,NULL); command("CALL nosuch",-ENOENT,NULL); command("IF",-EINVAL,NULL); command("IF EXIST",-EINVAL,NULL);
    exact("IF EXIST s_list.txt ECHO yes",0,"yes\n"); exact("IF NOT EXIST s_list.txt ECHO yes",0,""); exact("IF a==a ECHO same",0,"same\n"); exact("IF a==b ECHO same",0,"");
    exact("IF \"a b\"==\"a b\" ECHO quoted",0,"quoted\n"); exact("IF NOT a==b ECHO differs",0,"differs\n"); exact("IF a == a ECHO spaced",0,"spaced\n");
    exact("FOR %v IN (x y) DO ECHO [%v]",0,"[x]\n[y]\n"); command("FOR %v IN x DO ECHO",-EINVAL,NULL);
    exact("ECHO ok",0,"ok\n"); exact("ECHO %ERRORLEVEL%",0,"0\n"); term_execute(&app,"TYPE missing"); exact("ECHO %ERRORLEVEL%",0,"2\n");
    command("IF ERRORLEVEL x ECHO",-EINVAL,NULL);
    /* New commands. */
    contains("ATTRIB s_list.txt",0,"s_list.txt",1); contains("ATTRIB s_list.txt",0,"   C:\\Documents\\s_list.txt",1);
    exact("ATTRIB +R +H s_list.txt",0,"    HR     C:\\Documents\\s_list.txt\n");
    assert((fake_nodes[fake_lookup("/Documents/s_list.txt")].meta.attributes&3)==3);
    exact("ATTRIB -R s_list.txt",0,"    H      C:\\Documents\\s_list.txt\n"); exact("ATTRIB +A -H s_list.txt",0,"A          C:\\Documents\\s_list.txt\n");
    exact("ATTRIB -A s_list.txt",0,"           C:\\Documents\\s_list.txt\n");
    command("ATTRIB +X s_list.txt",-EINVAL,NULL); command("ATTRIB a b",-EINVAL,NULL); command("ATTRIB nothing.txt",-ENOENT,NULL);
    contains("ATTRIB +R sub",0,"folders keep their attributes",1);
    contains("ATTRIB",0,"one.txt",1);
    exact("VOL",0," Volume in drive C is CIUKI\n"); exact("VOL D:",0," Volume in drive D has no label\n");
    exact("LABEL D: DATA DISK",0,""); exact("VOL D:",0," Volume in drive D is DATA DISK\n"); assert(label_sets==1);
    exact("LABEL",0," Volume in drive C is CIUKI\n"); command("LABEL Z: x",0,NULL); command("VOL 1",-EINVAL,NULL);
    contains("CHKDSK",0,"bytes total disk space",1); contains("CHKDSK",0,"user files",1); contains("CHKDSK",0,"400000 bytes available",1);
    contains("CHKDSK D:",0,"Volume DATA DISK",1); contains("CHKDSK /F",-EINVAL,"nothing is repaired",1); command("CHKDSK X:",-ENOENT,NULL);
    contains("IPCONFIG",0,"IP Address. . . . . . . . . : 0.0.0.0",1); contains("IPCONFIG",0,"Connected",1); contains("IPCONFIG /ALL",0,"52-00-00-00-00-AB",1);
    contains("IPCONFIG /ALL",0,"rtl8139",1); command("IPCONFIG /X",-EINVAL,NULL);
    contains("PING 10.0.0.1",-ENOSYS,"error",1); contains("FORMAT A:",-ENOSYS,"error",1);
    contains("HELP",0,"ATTRIB",1); contains("HELP",0,"IPCONFIG",1);
    /* Interrupting a long redirection and the capture limits stay clean. */
    fake_assert_clean();
}
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
    command("RUN hello",-ENOSYS,NULL); command("REG",-ENOSYS,NULL); command("mystery",-ENOSYS,NULL);
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
    struct term_host host={.fs=fake_vfs,.attributes=attributes,.label_get=label_get,.label_set=label_set,.space=space,.net=net,.mixer=mixer,.volume=volume,.processes=processes,.display=display,.launch=launch,.kill=kill,.power=power,.now=now,.pump=pump,.quit=quit};
    du_init(&ui,pixels,TERM_WIDTH,TERM_HEIGHT); assert(!term_attach(&app,&ui,&host)); assert(app.columns==80 && app.rows==25);
    app.host.disktool=disktool;
    command("BACKUP FULL set.cbk one.txt",0,NULL); command("RESTORE set.cbk ORIGINAL OLDER",0,NULL); command("FDISK 1",0,NULL);
    assert(disktool_calls==3);
    builtins(); system_commands(); scripting(); lua_and_ui();
    app.host.run=native_run;app.host.registry=native_registry;app.host.network_command=network_command;
    command("RUN sample \"two words\"",7,"native child output");assert(app.errorlevel==7);
    command("REG ADD System\\Tools Message STRING \"hello world\"",0,"registry request");
    command("PING 127.0.0.1",0,"raw ICMP provider");
    /* Deterministic review scene: the production fixed-cell painter, accents,
     * descenders, cursor and selection. Independent of previous command output. */
    term_execute(&app,"CLS"); term_write(&app,"CiukiOS Terminal\nClassic computing, one woof at a time.\nAVATAR To Wa 0123456789 gjpqy\n");
    strcpy(app.input,"ECHO hello"); app.anchor=app.cursor=strlen(app.input); du_render(&ui);
    const char *frame=getenv("CIUKI_TERMINAL_FRAME");
    if(frame) { FILE *f=fopen(frame,"wb"); assert(f); assert(fwrite(pixels,sizeof(*pixels),TERM_WIDTH*TERM_HEIGHT,f)==TERM_WIDTH*TERM_HEIGHT); assert(!fclose(f)); }
    term_detach(&app); fake_assert_clean(); du_release(&ui);
    puts("terminal: PASS built-ins, aliases, VFS, nodes, Lua, interruption, history, completion, selection, resize, scrollback"); return 0;
}
