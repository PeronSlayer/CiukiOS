/* Kernel-owned file observations, at synchronous native desktop checkpoints.
 * Registry observations come from that authenticated desktop's separate
 * read-only w32srv queries, never from the installer or application stdout.
 * SPDX-License-Identifier: GPL-2.0-only */
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
    if(n<40 || memcmp(bytes,"CIUKLNK3",8) || bytes[36] || bytes[37] || bytes[38] || bytes[39])return false;
    unsigned at=40;uint32_t lengths[5];
    for(unsigned i=0;i<5;i++){
        unsigned p=16+i*4;lengths[i]=(uint32_t)bytes[p]|(uint32_t)bytes[p+1]<<8|(uint32_t)bytes[p+2]<<16|(uint32_t)bytes[p+3]<<24;
        if(lengths[i]>n-at)return false;at+=lengths[i];
    }
    return at==n && installer_text(bytes+40+lengths[0],lengths[1],INSTALLER_DIR "\\payload.txt");
}
static bool installer_report_stage(unsigned stage,unsigned code,unsigned registry_errors)
{
    struct installer_observation o={.exit_code=code,.registry_errors=registry_errors};
    static uint8_t bytes[4096];unsigned count;
    for(unsigned i=0;i<ARRAY_SIZE(installer_paths);i++){
        struct fat_entry entry;int e=vfs_stat(&installer_files,installer_paths[i],&entry);
        if(e){if(e!=-FS_ENOENT && e!=-FS_ENOTDIR)o.registry_errors++;continue;}
        o.files|=1u<<i;
        if(i==0 || i==3 || i==4 || i==8){
            if(installer_bytes(installer_paths[i],bytes,sizeof(bytes),&count)){o.registry_errors++;continue;}
            bool valid=i==0?installer_text(bytes,count,INSTALLER_PAYLOAD):i==3 || i==4?installer_link(bytes,count):false;
            if(i==8){
                const char *target=stage<=5?"C:\\WINDOWS\\TEMP\\CiukiInstallerFixture.remove":"C:\\WINDOWS\\TEMP\\CiukiInstallerFixture-uninstall.exe";
                /* INI whitespace/line endings are the profile implementation's;
                 * require the exact operand inside the rename section. */
                valid=false;
                for(unsigned at=0;at+9<count;at++)if(!memcmp(bytes+at,"[rename]",8)){
                    for(unsigned p=at+8;p+4<count;p++)if(!memcmp(bytes+p,"NUL=",4)){
                        unsigned len=(unsigned)strlen(target);if(len<=count-p-4 && !memcmp(bytes+p+4,target,len) &&
                           (p+4+len==count || bytes[p+4+len]=='\r' || bytes[p+4+len]=='\n'))valid=true;
                    }
                }
            }
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
    if(!native_live(server) || native_reports.survivor_stage!=2 ||
       !native_command(server,GATE_SNAPSHOT,&generation) || !native_command(server,GATE_INSTALLER,&generation))goto cleanup;
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
