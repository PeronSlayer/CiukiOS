/* Production native observer over the real registry directory/facades.
 * SPDX-License-Identifier: MIT */
#include "host.h"
#include <w32/server.h>
#include "../../../../apps/desktop/installer_fixture.h"
#include "../../../../src/probes/win32_installers/controller.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int damaged;
extern uint32_t w32r_exchange(const void *,uint32_t,void *,uint32_t,uint32_t *);
uint32_t desk_w32_exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used){
    uint32_t e=w32r_exchange(in,n,out,cap,used);if(damaged && *used)(*used)--;return e;
}
extern LONG RegDeleteKeyA(uint32_t,const char *);
extern LONG RegDeleteValueA(uint32_t,const char *);
extern LONG RegOpenKeyA(uint32_t,const char *,uint32_t *);
extern LONG RegCloseKey(uint32_t);
int main(int argc,char **argv){
    assert(argc==2);host_init(argv[1]);assert(desk_installer_ready());
    assert(!desk_installer_registry(1));
    const char *names[]={"DisplayName","UninstallString","DisplayVersion","Publisher","InstallLocation"};
    const char *values[]={"Ciuki Installer Fixture","\"" INSTALLER_DIR "\\uninstall.exe\"","1.0","CiukiOS",INSTALLER_DIR};
    for(unsigned i=0;i<5;i++)host_registry(INSTALLER_UNINSTALL,names[i],values[i]);
    assert(!desk_installer_registry(2));
    assert(desk_installer_registry(1));
    assert(desk_installer_registry(3));
    assert(desk_installer_registry(4));
    host_registry(INSTALLER_RUNONCE,"CiukiInstallerFixture","\"" INSTALLER_DIR "\\child.exe\" /restart-child");
    assert(!desk_installer_registry(3));
    assert(!desk_installer_registry(4));
    assert(desk_installer_registry(5)); /* not consumed at completion */
    host_registry(INSTALLER_UNINSTALL,"Publisher","incorrect");assert(desk_installer_registry(2));
    host_registry(INSTALLER_UNINSTALL,"Publisher","CiukiOS");
    damaged=1;assert(desk_installer_registry(3));damaged=0;
    uint32_t key;assert(!RegOpenKeyA(W32R_HKLM,INSTALLER_RUNONCE,&key));assert(!RegDeleteValueA(key,"CiukiInstallerFixture"));assert(!RegCloseKey(key));
    assert(!desk_installer_registry(5));
    assert(desk_installer_registry(4)); /* prematurely consumed before handoff */
    assert(!RegDeleteKeyA(W32R_HKLM,INSTALLER_UNINSTALL));assert(!desk_installer_registry(8));
    /* An empty Uninstall key still is a stale installation entry. */
    host_registry(INSTALLER_UNINSTALL,"temporary","value");assert(!RegOpenKeyA(W32R_HKLM,INSTALLER_UNINSTALL,&key));
    assert(!RegDeleteValueA(key,"temporary") && !RegCloseKey(key));assert(desk_installer_registry(8));
    assert(!RegDeleteKeyA(W32R_HKLM,INSTALLER_UNINSTALL));
    host_finish();puts("PASS native registry observer install/cancel/restart/uninstall, stale empty key and damaged RPC rejection");return 0;
}
