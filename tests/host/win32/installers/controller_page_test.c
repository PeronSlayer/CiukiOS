/* Native coordinator protocol only; never installer execution evidence. MIT */
#include "desktop.h"
#include <ciuki/spawn.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
static unsigned copy_fault,copy_reads,copy_writes,copy_removed,copy_closed;
static int copy_open(const char *path,int flags,...){
    if(flags==O_RDONLY){assert(strstr(path,"/Program Files/"));return 101;}
    assert(strstr(path,"/WINDOWS/TEMP/") && (flags&O_EXCL));return copy_fault==6?-1:102;
}
static ssize_t copy_read(int fd,void *bytes,size_t cap){
    assert(fd==101 && cap>=4);if(copy_fault==1)return -1;
    if(copy_reads++)return 0;memcpy(bytes,"data",4);return 4;
}
static ssize_t copy_write(int fd,const void *bytes,size_t n){
    (void)bytes;assert(fd==102 && n);copy_writes++;
    if(copy_fault==2 || (copy_fault==5 && copy_writes>1))return -1;
    return copy_fault==5?1:(ssize_t)n;
}
static int copy_fsync(int fd){assert(fd==102);return copy_fault==3?-1:0;}
static int copy_close(int fd){assert(fd==101 || fd==102);copy_closed++;return fd==102 && copy_fault==4?-1:0;}
static int copy_unlink(const char *path){assert(!strcmp(path,"/WINDOWS/TEMP/CiukiInstallerFixture-uninstall.exe"));copy_removed++;return 0;}
#define open copy_open
#define read copy_read
#define write copy_write
#define fsync copy_fsync
#define close copy_close
#define unlink copy_unlink
#include "../../../../apps/desktop/installer_fixture.c"
#undef open
#undef read
#undef write
#undef fsync
#undef close
#undef unlink
static unsigned reports;
void desk_v3_fixture_report(const char *text){
    assert(result && result->magic==INSTALLER_CHECKPOINT_MAGIC);
    assert(strstr(text,"case=w32-installer "));
    if(reports++==0)assert(result->stage==2 && result->child==71 && !result->exit_code &&
                          !result->registry_errors && result->ui_keys==3);
    else assert(result->stage==99 && result->exit_code==~0u && result->registry_errors==1);
}
unsigned desk_installer_registry(unsigned stage){(void)stage;return 0;}
int desk_installer_ready(void){return 0;}
int ciuki_win32_shell_restart(uint32_t pid,uint32_t flags){(void)pid;(void)flags;return 0;}
int ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *fds,uint32_t n,uint32_t flags)
{(void)path;(void)argv;(void)env;(void)fds;(void)n;(void)flags;return -1;}
void desk_focus(struct desktop *d,int index){(void)d;(void)index;}
void desk_input(struct desktop *d,const struct ciuki_input_event *event){(void)d;(void)event;}
int main(void){
    for(copy_fault=0;copy_fault<=6;copy_fault++){
        copy_reads=copy_writes=copy_removed=copy_closed=0;
        assert(copy_uninstaller()==(copy_fault?-1:0));
        assert(copy_removed==(copy_fault && copy_fault!=6?1u:0u));
        assert(copy_closed==(copy_fault==6?1u:2u));
    }
    void *wanted=(void *)(uintptr_t)INSTALLER_RESULT_VA;
    void *page=mmap(wanted,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(page==wanted); /* host model of the probe's private ua_map_at */
    static struct desktop desktop;
    memset(page,0xa5,4096);
    assert(!desk_installer_fixture_begin(&desktop) && desk_installer_fixture_active());
    assert(!result->magic && !result->stage && !result->child);
    assert(desk_installer_fixture_begin(&desktop)==-EBUSY);
    install.pid=71;install.ui_keys=3;checkpoint(2,0,0);
    desktop.now_ns=15000000000ull;desk_installer_fixture_tick(&desktop);
    assert(reports==2 && !desk_installer_fixture_active() && !result);
    assert(!desk_installer_fixture_begin(&desktop) && !result->magic);finish();
    assert(!munmap(page,4096));
    puts("PASS native installer checkpoint page, busy refusal, synchronous report and timeout cleanup");return 0;
}
