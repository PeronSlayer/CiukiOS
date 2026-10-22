/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#define DOSBOX_SETTINGS_DIR "build/host/vm/settings"
#include <unistd.h>
#undef _SC_PAGESIZE
#undef _SC_OPEN_MAX
#undef _SC_ARG_MAX
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include "../../../apps/dosbox/network_settings.h"
#include <stdlib.h>
static unsigned checks;
#define CHECK(x) do { checks++; if(!(x)) { fprintf(stderr,"FAIL %u: %s\n",__LINE__,#x); exit(1); } } while(0)
int main(void)
{
    char path[144]; CHECK(!dosbox_network_path(1,path,sizeof(path))); (void)unlink(path);
    CHECK(dosbox_network_load(1)==0 && !strcmp(dosbox_network_label(1),"IPX (LAN)"));
    CHECK(!mkdir(DOSBOX_SETTINGS_DIR,0755) || errno==EEXIST);
    FILE *f=fopen(path,"wb"); CHECK(f); CHECK(fputs("network=ipx\n",f)>=0); CHECK(!fclose(f));
    CHECK(dosbox_network_load(1)==1);
    char config[144]; CHECK(!dosbox_setting_path(1,config,sizeof(config)));
    f=fopen(config,"wb"); CHECK(f); CHECK(fputs("DOS kernel=unified\nDOS memory=locked\n",f)>=0); CHECK(!fclose(f));
    CHECK(dosbox_network_load(1)==1 && dosbox_setting_load(1)==CIUKI_DOSVM_UNIFIED && dosbox_memory_load(1)==1);
    f=fopen(path,"wb"); CHECK(f); CHECK(fputs("network=ipx\nextra",f)>=0); CHECK(!fclose(f));
    CHECK(dosbox_network_load(1)==0);
    CHECK(dosbox_network_path(0,path,sizeof(path))<0);
    CHECK(!dosbox_network_path(1,path,sizeof(path)));
    struct ciuki_dosvm_command c={0}; CHECK(dosvm_launch_network(&c)==0);
    dosvm_launch_network_set(&c,1); CHECK(dosvm_launch_network(&c)==1);
    CHECK(!c.launch.kernel && !c.launch.memory && !c.launch.reserved[1]);
    CHECK(!unlink(path)); CHECK(!dosbox_setting_path(1,path,sizeof(path))); CHECK(!unlink(path));
    CHECK(!rmdir(DOSBOX_SETTINGS_DIR));
    printf("settings: %u checks passed\n",checks); fflush(stdout); return 0;
}
