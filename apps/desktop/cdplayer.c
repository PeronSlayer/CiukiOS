/* Win32 CD Player offered by AutoPlay, through the public MCI interface.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/user32.h>
#include "../../sdk/win32/audio/mci.h"
#include "cdplayer_model.h"
extern char *W32API GetCommandLineA(void);
extern HANDLE W32API GetModuleHandleA(const char *);
extern void W32API ExitProcess(uint32_t);
extern uint32_t W32API mciSendStringA(const char *, char *, uint32_t, uintptr_t);
extern BOOL W32API mciGetErrorStringA(uint32_t, char *, uint32_t);
extern int W32API MessageBoxA(HWND, const char *, const char *, UINT);
extern DWORD W32API GetPrivateProfileStringA(const char *,const char *,const char *,char *,DWORD,const char *);
extern BOOL W32API WritePrivateProfileSectionA(const char *,const char *,const char *);
extern int W32API GetWindowTextA(HWND,char *,int);
static HWND display,track_list,repeat_button,random_button,artist_edit,title_edit,name_edit;
static char database_section[8192];
static struct cdp_player cd;
static void db_get(void *ctx,const char *section,const char *key,char *out,unsigned cap)
{ (void)ctx; GetPrivateProfileStringA(section,key,"",out,cap,"C:\\System\\cdplayer.ini"); }
static int db_put(void *ctx,const char *section,const char *text)
{ (void)ctx; return WritePrivateProfileSectionA(section,text,"C:\\System\\cdplayer.ini"); }
static const struct cdp_database database={0,db_get,db_put};
static uint32_t send(void *ctx,const char *s,char *out,uint32_t cap)
{ return mciSendStringA(s,out,cap,(uintptr_t)ctx); }
static void error(HWND h,uint32_t e)
{
    if (e) { char text[160]; if (!mciGetErrorStringA(e,text,sizeof(text))) cdp_text(text,"The CD operation failed."); MessageBoxA(h,text,"CD Player",0x10); }
}
static void command(HWND h,const char *s) { error(h,send(h,s,0,0)); }
static void selection(void) {
    SendMessageA(track_list,0x186,cd.track ? cd.track-1 : (WPARAM)-1,0);
    SetWindowTextA(name_edit,cd.names[cd.track]);
}
static void play(HWND h) { error(h,cdp_play(&cd)); selection(); }
static void show_tracks(void)
{
    SendMessageA(track_list,0x184,0,0); /* LB_RESETCONTENT */
    for (unsigned i=1;i<=cd.tracks;i++) {
        char text[64]; cdp_label(&cd,i,text); SendMessageA(track_list,0x180,0,(LPARAM)text);
    }
    selection();
}
static void refresh(HWND h)
{
    uint32_t e=cdp_load(&cd);
    if (!e) cdp_database_load(&cd,&database);
    SetWindowTextA(artist_edit,cd.artist); SetWindowTextA(title_edit,cd.title);
    show_tracks(); error(h,e);
}
static LRESULT W32API window_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp)
{
    if (msg==0x111) {
        switch (wp&0xffff) {
        case 1: play(h); break;
        case 2: command(h,"pause cd"); break;
        case 3: command(h,"resume cd"); break;
        case 4: command(h,"stop cd"); break;
        case 5: if (cdp_advance(&cd,-1,0)) selection(); break;
        case 6: if (cdp_advance(&cd,1,0)) selection(); break;
        case 7: command(h,"set cd door open"); break;
        case 8:
            if ((wp>>16)==1) { /* LBN_SELCHANGE */
                LRESULT row=SendMessageA(track_list,0x188,0,0);
                if (row>=0 && (unsigned)row<cd.tracks && cd.audio[row+1]) cd.track=(unsigned)row+1;
                selection();
            }
            break;
        case 9: cd.repeat^=1; SetWindowTextA(repeat_button,cd.repeat ? "Repeat: On" : "Repeat: Off"); break;
        case 10: cd.random^=1; SetWindowTextA(random_button,cd.random ? "Random: On" : "Random: Off"); break;
        case 11: refresh(h); break;
        case 15:
            if (!cd.track) break;
            GetWindowTextA(artist_edit,cd.artist,sizeof(cd.artist));
            GetWindowTextA(title_edit,cd.title,sizeof(cd.title));
            GetWindowTextA(name_edit,cd.names[cd.track],sizeof(cd.names[cd.track]));
            if (!cdp_database_save(&cd,&database,database_section,sizeof(database_section)))
                MessageBoxA(h,"The disc names could not be saved. Your edits are still in this window.","CD Player",0x10);
            show_tracks(); break;
        }
        return 0;
    }
    if (msg==MM_MCINOTIFY && wp==MCI_NOTIFY_SUCCESSFUL) {
        if (cdp_advance(&cd,1,1)) play(h);
        return 0;
    }
    if (msg==0x113) {
        char position[64]={0},text[160];
        if (!send(h,"status cd position",position,sizeof(position))) {
            char *s=cdp_track(cdp_text(text,"Track "),cd.track);
            s=cdp_text(s,"    "); s=cdp_text(s,position); s=cdp_text(s," / ");
            cdp_text(s,cd.length[cd.track]); SetWindowTextA(display,text);
        }
        return 0;
    }
    if (msg==WM_CLOSE) { command(h,"close cd"); DestroyWindow(h); return 0; }
    if (msg==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h,msg,wp,lp);
}
void W32API entry(void)
{
    HANDLE instance=GetModuleHandleA(0);
    WNDCLASSA wc={0}; wc.lpfnWndProc=window_proc; wc.hInstance=instance;
    wc.hbrBackground=(HBRUSH)(uintptr_t)16; wc.lpszClassName="CiukiCDPlayer";
    if (!RegisterClassA(&wc)) ExitProcess(1);
    HWND h=CreateWindowExA(0,wc.lpszClassName,"CD Player",0x00ca0000u,100,100,480,470,0,0,instance,0);
    if (!h) ExitProcess(1);
    char open[]="open cdaudio!@0 alias cd";
    const char *s=GetCommandLineA();
    for (;*s;s++) if (s[0]=='-' && s[1]=='-' && s[2]=='s' && s[3]=='l' && s[4]=='o' && s[5]=='t' && s[6]=='=' && s[7]>='0' && s[7]<='3') { open[14]=s[7]; break; }
    uint32_t e=mciSendStringA(open,0,0,0);
    if (e) { error(h,e); ExitProcess(e); }
    command(h,"set cd time format tmsf");
    cd.command=send; cd.ctx=h; cd.seed=1;
    display=CreateWindowExA(0,"STATIC","Audio CD",0x50000000u,12,12,448,24,h,0,instance,0);
    track_list=CreateWindowExA(0,"LISTBOX","",0x50a10001u,12,44,448,144,h,(HANDLE)(uintptr_t)8,instance,0);
    const char *labels[]={"Play","Pause","Resume","Stop","Previous","Next","Eject"};
    for (unsigned i=0;i<7;i++) CreateWindowExA(0,"BUTTON",labels[i],0x50010000u,8+(int)i*66,200,64,26,h,(HANDLE)(uintptr_t)(i+1),instance,0);
    repeat_button=CreateWindowExA(0,"BUTTON","Repeat: Off",0x50010000u,12,240,132,26,h,(HANDLE)(uintptr_t)9,instance,0);
    random_button=CreateWindowExA(0,"BUTTON","Random: Off",0x50010000u,160,240,132,26,h,(HANDLE)(uintptr_t)10,instance,0);
    CreateWindowExA(0,"BUTTON","Refresh disc",0x50010000u,308,240,144,26,h,(HANDLE)(uintptr_t)11,instance,0);
    const char *fields[]={"Artist:","Title:","Track name:"};
    HWND *edits[]={&artist_edit,&title_edit,&name_edit};
    for (unsigned i=0;i<3;i++) {
        CreateWindowExA(0,"STATIC",fields[i],0x50000000u,12,282+(int)i*30,84,22,h,0,instance,0);
        *edits[i]=CreateWindowExA(0x200,"EDIT","",0x50010080u,100,280+(int)i*30,350,24,h,(HANDLE)(uintptr_t)(12+i),instance,0);
        SendMessageA(*edits[i],0xc5,i==2 ? 31 : 63,0); /* EM_SETLIMITTEXT */
    }
    CreateWindowExA(0,"BUTTON","Save disc names",0x50010000u,272,376,178,26,h,(HANDLE)(uintptr_t)15,instance,0);
    refresh(h); ShowWindow(h,1); SetTimer(h,1,250,0); if (cd.track) play(h);
    MSG m; while (GetMessageA(&m,0,0,0)>0) { TranslateMessage(&m); DispatchMessageA(&m); }
    ExitProcess(0);
}
