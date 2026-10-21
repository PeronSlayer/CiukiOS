/* Kernel-owned file observations, at synchronous native desktop checkpoints.
 * Registry observations come from that authenticated desktop's separate
 * read-only w32srv queries, never from the installer or application stdout.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../../probes/win32_installers/identities.h"
static struct vfs_table installer_files;
static struct installer_result installer_result;
static const char *const installer_paths[]={
    "C:/Program Files/Ciuki Installer Fixture/payload.txt",
    "C:/Program Files/Ciuki Installer Fixture/child.exe",
    "C:/Program Files/Ciuki Installer Fixture/uninstall.exe",
    "C:/WINDOWS/Desktop/Ciuki Installer Fixture.lnk",
    "C:/WINDOWS/Start Menu/Programs/Ciuki Installer Fixture/Readme.lnk",
    "C:/WINDOWS/TEMP/CiukiInstallerFixture-uninstall.exe",
    "C:/WINDOWS/TEMP/CiukiInstallerFixture.remove",
    "C:/WINDOWS/TEMP/CiukiInstallerFixture.restarted",
    "C:/WINDOWS/WININIT.INI","C:/WINDOWS/WININIT.BAK","C:/WINDOWS/CIUKIRST.LOG",
    "C:/Program Files/Ciuki Installer Fixture"
};
static int installer_bytes(const char *path,uint8_t *bytes,unsigned cap,unsigned *count)
{
    int fd=vfs_open(&installer_files,path,VFS_READ,VFS_DENY_NONE,0);if(fd<0)return fd;
    size_t done=0;int e=vfs_read(&installer_files,fd,bytes,cap,&done);int close=vfs_close(&installer_files,fd);
    *count=(unsigned)done;return e?e:close;
}
static bool installer_text(const uint8_t *bytes,unsigned count,const char *text)
{return strlen(text)==count && !memcmp(bytes,text,count);}
static bool installer_link(const uint8_t *bytes,unsigned n)
{
    if(n<40 || memcmp(bytes,"CIUKLNK3",8) || bytes[8]!=1 || bytes[9] || bytes[10] || bytes[11] ||
       bytes[12] || bytes[13] || bytes[14] || bytes[15] || bytes[36] || bytes[37] || bytes[38] || bytes[39])return false;
    const char *fields[]={"Ciuki Installer Fixture",INSTALLER_DIR "\\payload.txt","",INSTALLER_DIR,INSTALLER_DIR "\\child.exe"};
    unsigned at=40;
    for(unsigned i=0;i<5;i++){
        unsigned p=16+i*4;uint32_t len=(uint32_t)bytes[p]|(uint32_t)bytes[p+1]<<8|(uint32_t)bytes[p+2]<<16|(uint32_t)bytes[p+3]<<24;
        if(len>n-at || !installer_text(bytes+at,len,fields[i]))return false;at+=len;
    }
    return at==n;
}
static bool installer_ini(const uint8_t *bytes,unsigned count,const char *target)
{
    bool rename=false;unsigned found=0;
    for(unsigned at=0;at<count;){
        unsigned start=at;while(at<count && bytes[at]!='\r' && bytes[at]!='\n')at++;
        unsigned end=at;while(at<count && (bytes[at]=='\r' || bytes[at]=='\n'))at++;
        while(start<end && (bytes[start]==' ' || bytes[start]=='\t'))start++;
        while(end>start && (bytes[end-1]==' ' || bytes[end-1]=='\t'))end--;
        if(start==end || bytes[start]==';' || bytes[start]=='#')continue;
        if(bytes[start]=='['){
            rename=end-start==8;
            const char *section="[rename]";
            for(unsigned i=0;rename && i<8;i++){
                uint8_t c=bytes[start+i];if(c>='A' && c<='Z')c+=32;
                if(c!=(uint8_t)section[i])rename=false;
            }
            continue;
        }
        if(!rename)continue;
        unsigned equal=start;while(equal<end && bytes[equal]!='=')equal++;
        unsigned key_end=equal;while(key_end>start && (bytes[key_end-1]==' ' || bytes[key_end-1]=='\t'))key_end--;
        if(equal==end || key_end-start!=3)return false;
        for(unsigned i=0;i<3;i++){
            uint8_t c=bytes[start+i];if(c>='a' && c<='z')c-=32;
            if(c!=(uint8_t)"NUL"[i])return false;
        }
        unsigned value=equal+1;while(value<end && (bytes[value]==' ' || bytes[value]=='\t'))value++;
        if(!installer_text(bytes+value,end-value,target))return false;
        found++;
    }
    return found==1;
}
static bool installer_executable(const char *path,unsigned size,uint32_t expected)
{
    int fd=vfs_open(&installer_files,path,VFS_READ,VFS_DENY_NONE,0);if(fd<0)return false;
    uint8_t bytes[1024];uint32_t hash=2166136261u;unsigned total=0;bool valid=true;
    for(;;){
        size_t n=0;if(vfs_read(&installer_files,fd,bytes,sizeof(bytes),&n)){valid=false;break;}
        if(!n)break;
        if(n>size-total){valid=false;break;}
        for(unsigned i=0;i<n;i++)hash=(hash^bytes[i])*16777619u;
        total+=(unsigned)n;
    }
    if(vfs_close(&installer_files,fd))valid=false;
    return valid && total==size && hash==expected;
}
static unsigned installer_journal(const uint8_t *bytes,unsigned count)
{
    if(installer_text(bytes,count,"CIUKIRST 1\nphase=1\nflags=2\nline=0\nhanded=0\n"))return IF_JOURNAL;
    if(installer_text(bytes,count,"CIUKIRST 1\nphase=1\nflags=2\nline=1\nhanded=0\n"))return IF_JOURNAL|IF_LINE_APPLIED;
    if(installer_text(bytes,count,"CIUKIRST 1\nphase=2\nflags=2\nline=1\nhanded=0\n"))return IF_JOURNAL|IF_LINE_APPLIED|IF_RETIRED;
    return 0;
}
static bool installer_report_stage(unsigned stage,unsigned code,unsigned registry_errors)
{
    struct installer_observation o={.exit_code=code,.registry_errors=registry_errors};
    static uint8_t bytes[4096];unsigned count;
    unsigned journal=0;
    if((stage==4 || stage==7) && !installer_bytes(installer_paths[10],bytes,sizeof(bytes),&count)){
        journal=installer_journal(bytes,count);
        o.contents|=journal&(IF_LINE_APPLIED|IF_RETIRED);
    }
    for(unsigned i=0;i<ARRAY_SIZE(installer_paths);i++){
        struct fat_entry entry;int e=vfs_stat(&installer_files,installer_paths[i],&entry);
        if(e){if(e!=-FS_ENOENT && e!=-FS_ENOTDIR)o.registry_errors++;continue;}
        o.files|=1u<<i;
        if(i==1 || i==2 || i==5){
            if(installer_executable(installer_paths[i],i==1?INSTALLER_CHILD_SIZE:INSTALLER_UNINSTALL_SIZE,
                                     i==1?INSTALLER_CHILD_FNV:INSTALLER_UNINSTALL_FNV))o.contents|=1u<<i;
        }else if(i!=11){
            if(installer_bytes(installer_paths[i],bytes,sizeof(bytes),&count)){o.registry_errors++;continue;}
            bool valid=false;
            if(i==0)valid=installer_text(bytes,count,INSTALLER_PAYLOAD);
            if(i==3 || i==4)valid=installer_link(bytes,count);
            if(i==6)valid=installer_text(bytes,count,"authored restart deletion\r\n");
            if(i==7)valid=installer_text(bytes,count,"child completed\r\n");
            if(i==8 || i==9){
                bool restart=i==8?stage<=5:stage<=7 && !(stage==7 && (o.contents&IF_RETIRED));
                const char *target=restart?"C:\\WINDOWS\\TEMP\\CiukiInstallerFixture.remove":"C:\\WINDOWS\\TEMP\\CiukiInstallerFixture-uninstall.exe";
                valid=installer_ini(bytes,count,target);
            }
            if(i==10)valid=installer_journal(bytes,count)!=0 &&
                ((stage!=4 && stage!=7) || installer_journal(bytes,count)==journal);
            if(valid)o.contents|=1u<<i;
        }
    }
    rec_emit("w32-installer-nsis","DATA","case=checkpoint stage=%u files=%x contents=%x registry_errors=%u exit=%u",stage,o.files,o.contents,o.registry_errors,code);
    return installer_observe(&installer_result,stage,&o)!=0;
}
#ifndef CIUKI_INSTALLER_OBSERVER_ONLY
static unsigned installer_errors(void)
{return installer_result.errors[0]+installer_result.errors[1]+installer_result.errors[2]+installer_result.errors[3];}
static int probe_w32_installer_nsis(void)
{
    const char *name="w32-installer-nsis";rec_emit(name,"BEGIN",0);
    const struct fb_device *display=fbdev_get();struct storage *storage=storage_get();
    if(!display->present || (g_boot.flags&CBI_F_SAFE_MODE) || !native_payload_present() ||
       storage_enable_write(storage,2) || vfs_table_init(&storage->vfs,&installer_files,2)){
        rec_emit(name,"END","status=FAIL reason=installer_setup");return 1;
    }
    struct desktop_ledger initial,final;struct proc_ledger before,after;
    desktop_snapshot(&initial);proc_snapshot(&before);
    struct process *owner=0,*server=0;uint32_t generation=0;bool pass=false;
    const char *failure="desktop_launch";
    memset(&native_reports,0,sizeof(native_reports));memset(&installer_result,0,sizeof(installer_result));
    if(proc_prepare(proc_supervisor(),&owner))goto finished;
    proc_publish(owner,0);
    if(supervisor_spawn_desktop_probe(&server)<0)goto cleanup;
    server->ppid=owner->pid;native_reports.active=true;native_reports.server=server->pid;native_reports.installer=true;
    uint64_t deadline=g_ticks+10000;
    while(native_live(server) && !native_reports.survivor_stage && g_ticks<deadline)task_sleep_ms(1);
    if(!native_live(server) || native_reports.survivor_stage!=2)goto cleanup;
    /* The unmodified pinned NSIS engine has no result protocol. The native
     * coordinator owns this isolated checkpoint page; manifests remain
     * independently read by the kernel. ua_map_at refuses existing extents. */
    failure="installer_result_page";
    if(ua_map_at(server->memory,INSTALLER_RESULT_VA,PAGE_SIZE,PROT_READ|PROT_WRITE,
                 PROT_READ|PROT_WRITE,UA_ANON,0) ||
       !native_command(server,GATE_SNAPSHOT,&generation) ||
       !native_command_argument(server,GATE_W32_FIXTURE,&generation,INSTALLER_FIXTURE_NAME))goto cleanup;
    deadline=g_ticks+150000;
    while(native_live(server) && !installer_result.complete && !installer_errors() && g_ticks<deadline)task_sleep_ms(1);
    pass=native_live(server) && installer_result.complete && !installer_errors();
    failure=native_reports.invalid?native_reports.invalid_check:!installer_result.complete?"installer_incomplete":"installer_manifest";
cleanup:
    native_reports.active=false;native_reports.installer=false;
    if(server){
        static struct ciuki_proc_record children[CIUKI_PROCESS_MAX];
        unsigned listed=proc_list_snapshot(children,CIUKI_PROCESS_MAX,0);
        for(unsigned i=0;i<listed;i++)if(children[i].ppid==server->pid)desktop_remove(server,proc_find(children[i].pid));
        desktop_remove(owner,server);
    }
    if(owner){proc_stop(owner,0,0);proc_collect();}
finished:
    vfs_table_destroy(&installer_files);desktop_snapshot(&final);proc_snapshot(&after);
    installer_publish(&installer_result,rec_emit);
    bool clean=desktop_ledgers_equal(&initial,&final) && native_proc_equal(&before,&after);
    rec_emit(name,"DATA","case=ledger clean=%u",clean);
    if(pass && !clean){pass=false;failure="cleanup_ledger";}
    if(pass)rec_emit(name,"END","status=PASS");else rec_emit(name,"END","status=FAIL reason=%s",failure);
    return pass?0:1;
}
#endif
