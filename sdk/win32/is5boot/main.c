/* Native replacement for IS5's NE launcher/cleanup pair.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shell.h>
#include "private.h"
extern char *W32API GetCommandLineA(void);
extern char *W32API GetEnvironmentStringsA(void);
extern BOOL W32API FreeEnvironmentStringsA(char *);
extern DWORD W32API GetWindowsDirectoryA(char *,DWORD);
extern DWORD W32API GetShortPathNameA(const char *,char *,DWORD);
extern BOOL W32API GetExitCodeProcess(HANDLE,DWORD *);
extern void W32API ExitProcess(DWORD);
struct process_entry {
    DWORD size,usage,pid;
    uintptr_t heap;
    DWORD module,threads,parent;
    LONG priority;
    DWORD flags;
    char exe[260];
};
extern HANDLE W32API CreateToolhelp32Snapshot(DWORD,DWORD);
extern BOOL W32API Process32First(HANDLE,struct process_entry *);
extern BOOL W32API Process32Next(HANDLE,struct process_entry *);
extern HANDLE W32API OpenProcess(DWORD,BOOL,DWORD);
static unsigned len(const char *s){unsigned n=0;while(s[n])n++;return n;}
static int equal(const char *a,const char *b){while(*a && ((*a|32)==(*b|32))){a++;b++;}return *a==*b;}
static void copy(char *a,const char *b){while((*a++=*b++));}
static const char *base(const char *p){const char *b=p;while(*p){if(*p=='/' || *p=='\\')b=p+1;p++;}return b;}
static void *allocate(unsigned n){return HeapAlloc(GetProcessHeap(),0,n);}
static void release(void *p){if(p)HeapFree(GetProcessHeap(),0,p);}
static uint8_t *read_all(const char *path,DWORD *size) {
    HANDLE h=CreateFileA(path,0x80000000,3,0,3,0,0);
    if(h==(HANDLE)(uintptr_t)-1)return 0;
    DWORD high=0,n=GetFileSize(h,&high),got=0;uint8_t *p=0;
    if(!high && n && n<=16u*1024*1024){p=allocate(n);if(p && (!ReadFile(h,p,n,&got,0) || got!=n)){release(p);p=0;}}
    CloseHandle(h);if(p)*size=n;return p;
}
static int write_handle(HANDLE h,const void *p,DWORD n) {
    DWORD done=0;return WriteFile(h,p,n,&done,0) && done==n && FlushFileBuffers(h);
}
static int write_new(const char *path,const void *p,DWORD n) {
    HANDLE h=CreateFileA(path,0x40000000,0,0,1,0,0);
    if(h==(HANDLE)(uintptr_t)-1)return 0;
    int ok=write_handle(h,p,n);CloseHandle(h);if(!ok)DeleteFileA(path);return ok;
}
static int write_text(const char *dir,const char *name,const char *text) {
    char path[IS5_PATH];if(!is5_join(path,dir,name))return 0;
    HANDLE h=CreateFileA(path,0x40000000,0,0,2,0,0);
    if(h==(HANDLE)(uintptr_t)-1)return 0;
    int ok=write_handle(h,text,len(text));CloseHandle(h);return ok;
}
static int profile(const char *path,const char *section,const char *key,const char *fallback,char *out,unsigned cap) {
    return GetPrivateProfileStringA(section,key,fallback,out,cap,path)<cap-1;
}
static const unsigned error_ids[]={7068,7063,7064,7110};
static const char *const fallback_errors[]={"Setup is unable to find the installation engine.",
    "Setup is unable to create a temporary file.","Setup is unable to start the installation engine.",
    "Setup is unable to decompress the installation files."};
struct state {
    char source[IS5_PATH],temp[IS5_PATH],windows[IS5_PATH],engine[IS5_PATH],dll[2][IS5_PATH];
    char errors[4][512],app[256],isdel[IS5_NAME];
    unsigned owned[3];
    /* fx-hlis5: the step that failed and its Win32 error, for the record. */
    const char *step;DWORD error;
};
static int failed(struct state *s,const char *step){s->step=step;s->error=GetLastError();return 0;}
/* fx-hlis5: the 16-bit stub only ever sees short (8.3) spellings: its own
 * module path and, on Windows NT, TEMP/TMP, which the system keeps in their
 * short form for compatibility (Raymond Chen, "Why does the system convert
 * TEMP to a short file name?", The Old New Thing 2004-12-24; Windows XP's
 * %TEMP% is C:\DOCUME~1\<user>\LOCALS~1\Temp). The engine's argument
 * line separates its operands with spaces: a profile TEMP below "Local
 * Settings" made the engine extract to "...\Local\". The directory keeps
 * its long spelling when it has no short form. */
static void short_spelling(char *directory) {
    char shorter[IS5_PATH];unsigned n=len(directory),slash=n && (directory[n-1]=='\\' || directory[n-1]=='/');
    DWORD m=GetShortPathNameA(directory,shorter,sizeof(shorter)-1);
    if(!m || m>=sizeof(shorter)-1)return;
    if(slash && shorter[m-1]!='\\'){shorter[m++]='\\';shorter[m]=0;}
    copy(directory,shorter);
}
static int delete_wait(const char *path) {
    for(unsigned i=0;i<100;i++) {
        if(DeleteFileA(path) || GetLastError()==2 || GetLastError()==3)return 1;
        if(GetLastError()!=32 && GetLastError()!=5)return 0;
        Sleep(100);
    }
    return 0;
}
static void remove_owned(struct state *s) {
    if(s->owned[0])delete_wait(s->engine);
    for(unsigned i=0;i<2;i++)if(s->owned[i+1])delete_wait(s->dll[i]);
}
static int wait_engine(const char *name) {
    HANDLE snapshot=CreateToolhelp32Snapshot(2,0);
    if(snapshot==(HANDLE)(uintptr_t)-1)return 0;
    struct process_entry e;e.size=sizeof(e);int ok=1;
    for(BOOL more=Process32First(snapshot,&e);more;more=Process32Next(snapshot,&e)) {
        if(!equal(base(e.exe),base(name)))continue;
        HANDLE process=OpenProcess(0x100000,0,e.pid);
        if(!process){if(GetLastError()!=87)ok=0;continue;}
        if(WaitForSingleObject(process,0xffffffffu)!=0)ok=0;
        CloseHandle(process);
    }
    if(GetLastError()!=18)ok=0; /* ERROR_NO_MORE_FILES, not an enumeration error. */
    CloseHandle(snapshot);return ok;
}
/* A cleanup record must name one TEMP engine and its two exact support DLLs.
 * Reject traversal, alternate drives and unowned file types. */
static int cleanup_list(struct state *s) {
    char ini[IS5_PATH],sections[4096];if(!is5_join(ini,s->windows,"_delis43.ini"))return 1;
    DWORD n=GetPrivateProfileSectionNamesA(sections,sizeof(sections),ini);
    if(n>=sizeof(sections)-2)return 1;
    for(const char *section=sections;*section;section+=len(section)+1) {
        if(len(section)<5 || section[0]!='f' || section[1]!='i' || section[2]!='l' || section[3]!='e')return 1;
        for(unsigned i=4;section[i];i++)if(section[i]<'0' || section[i]>'9')return 1;
        char main[IS5_PATH],alt[2][IS5_PATH],type[16];
        if(!profile(ini,section,"main","",main,sizeof(main)) ||
           !profile(ini,section,"exeostype","",type,sizeof(type)) || !equal(type,"1"))return 1;
        const char *b=base(main);unsigned prefix=(unsigned)(b-main),t=len(s->temp);
        if(prefix!=t || len(b)!=12 || b[0]!='_' || (b[1]|32)!='i' || (b[2]|32)!='n' || (b[3]|32)!='s' || !equal(b+8,"._MP"))return 1;
        for(unsigned i=4;i<8;i++)if(b[i]<'0' || b[i]>'9')return 1;
        for(unsigned i=0;i<t;i++)if((main[i]|32)!=(s->temp[i]|32))return 1;
        if(!profile(ini,section,"alt0","",alt[0],IS5_PATH) || !equal(alt[0],s->dll[0]) ||
           !profile(ini,section,"alt1","",alt[1],IS5_PATH) || !equal(alt[1],s->dll[1]))return 1;
        if(!wait_engine(main) || !delete_wait(main) || !delete_wait(alt[0]) || !delete_wait(alt[1]))return 1;
        if(!WritePrivateProfileStringA(section,0,0,ini))return 1;
    }
    return 0;
}
static int cleanup(struct state *s) {
    char path[IS5_PATH];if(!is5_join(path,s->windows,"_is5boot.lck"))return 1;
    HANDLE lock;
    for(;;) {
        lock=CreateFileA(path,0x40000000,0,0,4,0,0);
        if(lock!=(HANDLE)(uintptr_t)-1)break;
        if(GetLastError()!=32)return 1;
        Sleep(100);
    }
    int result=cleanup_list(s);CloseHandle(lock);return result;
}
static int run(struct state *s,const char *stub,const char *user,unsigned *failure) {
    DWORD bytes=0;uint8_t *image=read_all(stub,&bytes);
    if(!image || !is5_ne(image,bytes)){release(image);return 193;}
    for(unsigned i=0;i<4;i++) {
        copy(s->errors[i],fallback_errors[i]);
        is5_string(image,bytes,error_ids[i],s->errors[i],sizeof(s->errors[i]));
    }
    release(image);
    unsigned prefix=(unsigned)(base(stub)-stub);
    if(!prefix || prefix>=IS5_PATH)return 193;
    for(unsigned i=0;i<prefix;i++)s->source[i]=stub[i];s->source[prefix]=0;
    DWORD n=GetTempPathA(sizeof(s->temp),s->temp);
    if(!n || n>=sizeof(s->temp) || !GetWindowsDirectoryA(s->windows,sizeof(s->windows))){failed(s,"temp-path");return 1;}
    short_spelling(s->temp);short_spelling(s->source);
    if(!is5_join(s->dll[0],s->temp,"ZDataI51.dll") || !is5_join(s->dll[1],s->temp,"_WUTL951.DLL")){failed(s,"temp-path");return 1;}
    char path[IS5_PATH],cmd[4096],ignored[256];
    if(is5_join(path,s->source,"_SETUP.DLL")) {
        image=read_all(path,&bytes);
        if(image)for(unsigned i=0;i<4;i++)is5_string(image,bytes,error_ids[i],s->errors[i],sizeof(s->errors[i]));
        release(image);
    }
    if(!is5_join(path,s->source,"SETUP.INI") ||
       !profile(path,"Startup","AppName","Setup",s->app,sizeof(s->app)) ||
       !profile(path,"Startup","IsdelName","_ISDEL.EXE",s->isdel,sizeof(s->isdel)) ||
       !profile(path,"Startup","CmdLine","",cmd,sizeof(cmd)) ||
       !profile(path,"Startup","EnableLangDlg","Y",ignored,sizeof(ignored)) ||
       !profile(path,"Mif","Type","",ignored,sizeof(ignored))){failed(s,"setup-ini");return 1;}
    if(equal(base(stub),base(s->isdel))){int r=cleanup(s);if(r)failed(s,"cleanup");return r;}
    if(!is5_join(path,s->source,"SETUP.LID") ||
       !profile(path,"Languages","Default","0009",ignored,sizeof(ignored))){failed(s,"setup-lid");return 1;}
    if(!is5_join(path,s->windows,"system.ini") ||
       !profile(path,"Boot","Shell","explorer.exe",ignored,sizeof(ignored))){failed(s,"system-ini");return 1;}
    if(!is5_join(path,s->source,"_INST32I.EX_")){failed(s,"archive");return 1;}
    uint8_t *archive=read_all(path,&bytes);struct is5_archive a;
    if(!archive || !is5_directory(archive,bytes,&a)){failed(s,"archive");release(archive);*failure=0;return 1;}
    /* Decode all three members first: malformed input never publishes an engine. */
    uint8_t *decoded[3]={0};unsigned selected[3]={0},sizes[3]={0};int ok=1;
    for(unsigned i=0;i<a.count && ok;i++) {
        struct is5_member *m=a.member+i;
        int which=equal(m->original,"INSTALL.EXE")?0:equal(m->original,"zdata.dll")?1:equal(m->original,"WUTL95i.DLL")?2:-1;
        if(which<0)continue;
        if(selected[which]++){ok=0;break;}
        decoded[which]=allocate(m->expanded);sizes[which]=m->expanded;
        if(!decoded[which] || !is5_dcl(archive+m->offset,m->packed,decoded[which],m->expanded))ok=0;
    }
    release(archive);
    for(unsigned i=0;i<3;i++)if(!selected[i])ok=0;
    if(!ok)failed(s,"decode");
    *failure=3;
    HANDLE lock=(HANDLE)(uintptr_t)-1;char lockpath[IS5_PATH];
    if(ok && is5_join(lockpath,s->windows,"_is5boot.lck")) {
        /* OPEN_ALWAYS + share-none serializes even if a prior helper crashed. */
        lock=CreateFileA(lockpath,0x40000000,0,0,4,0,0);ok=lock!=(HANDLE)(uintptr_t)-1;
        if(!ok)failed(s,"lock");
    }else if(ok)ok=failed(s,"lock");
    if(ok) {
        *failure=1;ok=0;
        unsigned start=GetTickCount()%10000;
        for(unsigned i=0;i<10000;i++) {
            unsigned value=(start+i)%10000;char name[]="_INS0000._MP";
            for(int j=7;j>=4;j--){name[j]=(char)('0'+value%10);value/=10;}
            if(!is5_join(s->engine,s->temp,name))break;
            HANDLE h=CreateFileA(s->engine,0x40000000,0,0,1,0,0);
            if(h==(HANDLE)(uintptr_t)-1){if(GetLastError()==80 || GetLastError()==183)continue;failed(s,"engine-file");break;}
            s->owned[0]=1;ok=write_handle(h,decoded[0],sizes[0]);if(!ok)failed(s,"engine-file");CloseHandle(h);break;
        }
        if(!ok && !s->step)failed(s,"engine-file");
    }
    for(unsigned i=0;i<2 && ok;i++) {
        ok=write_new(s->dll[i],decoded[i+1],sizes[i+1]);if(ok)s->owned[i+1]=1;else failed(s,"support-file");
    }
    for(unsigned i=0;i<3;i++)release(decoded[i]);
    char *text=allocate(65536);char *env=0;
    if(!text && ok)ok=failed(s,"memory");
    if(ok && !(is5_arguments(text,65536,s->source,s->temp,cmd,user) && write_text(s->temp,"_INS0432.INI",text)))ok=failed(s,"handoff");
    if(ok) {
        env=GetEnvironmentStringsA();unsigned count=0;
        if(env){while(count<32767 && (env[count] || env[count+1]))count++;count+=2;}
        ok=env && count<=32768 && is5_environment(text,65536,env,count) && write_text(s->windows,"_isenv31.ini",text);
        if(!ok)failed(s,"handoff");
    }
    if(env)FreeEnvironmentStringsA(env);
    if(ok) {
        copy(text,"[SharedErrors]\r\n");
        for(unsigned i=0;i<4;i++) {
            char key[]="0000=";unsigned v=error_ids[i];for(int j=3;j>=0;j--){key[j]=(char)('0'+v%10);v/=10;}
            unsigned at=len(text);copy(text+at,key);at=len(text);copy(text+at,s->errors[i]);copy(text+len(text),"\r\n");
        }
        ok=write_text(s->windows,"_iserr31.ini",text) && is5_cleanup_text(text,65536,s->engine,s->dll[0],s->dll[1]) &&
            write_text(s->windows,"_delis43.ini",text);
        if(!ok)failed(s,"handoff");
    }
    release(text);
    int result=1,may_delete=1;
    if(ok) {
        *failure=2;
        struct shell_startup startup={.size=sizeof(startup)};struct shell_process child={0};char empty[1]={0};
        if(CreateProcessA(s->engine,empty,0,0,0,0,0,s->source,&startup,&child)) {
            DWORD code=1;
            if(WaitForSingleObject(child.process,0xffffffffu)==0) {
                if(GetExitCodeProcess(child.process,&code)){result=(int)code;*failure=4;}
            } else may_delete=0; /* Retain the cleanup list on a failed wait. */
            CloseHandle(child.thread);CloseHandle(child.process);
            /* A nonzero installer exit is its own result, not a stub error. */
        } else failed(s,"engine-start");
    }
    if(may_delete)remove_owned(s);
    if(lock!=(HANDLE)(uintptr_t)-1)CloseHandle(lock);
    return result;
}
/* fx-hlis5: a start failure is recorded ("[win32] warning check=is5-<step>-
 * <error> module=is5boot.exe") and shown. The caller's STARTUPINFO belongs to
 * the stub: Half-Life's AUTORUN.EXE starts SETUP.EXE with SW_HIDE, and the
 * first-ShowWindow override hid this unowned captioned box, leaving an
 * invisible modal loop. Windows' first-ShowWindow heuristic excludes
 * system-modal windows so that critical messages are not hidden (Raymond
 * Chen, "When does STARTF_USESHOWWINDOW override the parameter passed to
 * ShowWindow()?", The Old New Thing 2010-03-01, rule two item 5; user32
 * u32_startup_show): the box is MB_SYSTEMMODAL | MB_ICONHAND. */
static void report(struct state *s,unsigned failure,int result) {
    char reason[96];unsigned at=4;copy(reason,"is5-");
    const char *step=s->step?s->step:result==193?"image":"start";
    for(unsigned i=0;step[i] && at<60;i++)reason[at++]=step[i];
    reason[at++]='-';
    char digits[12];unsigned n=0;DWORD e=s->error;do{digits[n++]=(char)('0'+e%10);e/=10;}while(e && n<10);
    while(n)reason[at++]=digits[--n];
    reason[at]=0;
    w32_diag("warning","is5boot.exe",reason);
    MessageBoxA(0,s->errors[failure][0]?s->errors[failure]:fallback_errors[failure],s->app[0]?s->app:"Setup",0x1010);
}
void W32API entry(void) {
    /* Kernel redirects ImagePath, while CommandLine starts with the original
     * NE path. H1 uses the same convention for its kernel admission adapter. */
    const char *command=GetCommandLineA(),*p=command;char stub[IS5_PATH];unsigned n=0;
    while(*p==' ' || *p=='\t')p++;
    int quoted=*p=='"';if(quoted)p++;
    while(*p && (quoted?*p!='"':*p!=' ' && *p!='\t')){if(n+1==sizeof(stub))ExitProcess(206);stub[n++]=*p++;}
    stub[n]=0;
    struct state *s=allocate(sizeof(*s));if(!s)ExitProcess(8);
    for(unsigned i=0;i<sizeof(*s);i++)((uint8_t *)s)[i]=0;
    unsigned failure=1;int result=run(s,stub,is5_tail(command),&failure);
    if(result && failure<4)report(s,failure,result);
    release(s);ExitProcess((DWORD)result);
}
