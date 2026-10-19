/* H51-06 production preference parser/persistence; disposable host directory.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#undef _SC_PAGESIZE
#undef _SC_OPEN_MAX
#undef _SC_ARG_MAX
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include "../../../apps/dosbox/settings.h"
#include <assert.h>
#include <stdlib.h>
int main(void) {
    assert(dosbox_game("\\DOOM\\DOOM.EXE -config CIUKI.CFG")==2);
    assert(!dosbox_game("\\DOOM\\DOOM.EXE -nosound"));
    assert(!dosbox_game("DIR"));
    for(unsigned game=1;game<=3;game++) assert(!dosbox_setting_load(game));
    assert(!dosbox_kernel_value(NULL) && !dosbox_kernel_value("bad"));
    assert(dosbox_kernel_value("freedos-1.3")==1);
    assert(!dosbox_setting_save(2,1));
    assert(dosbox_setting_load(2)==1 && !dosbox_setting_load(1) && !dosbox_setting_load(3));
    char path[128]; assert(!dosbox_setting_path(1,path,sizeof(path)));
    FILE *file=fopen(path,"wb"); assert(file);
    fputs("DOS kernel=freedos-1.3\nextra=1\n",file); fclose(file);
    assert(!dosbox_setting_load(1) && dosbox_setting_load(2)==1);
    file=fopen(path,"wb"); assert(file); fputs("DOS kernel=master\n",file); fclose(file);
    assert(!dosbox_setting_load(1));
    assert(dosbox_setting_save(0,1)==-1 && dosbox_setting_save(4,1)==-1 && dosbox_setting_save(1,2)==-1);
    assert(dosbox_setting_load(2)==1);
    assert(!dosbox_setting_save(2,0) && !dosbox_setting_load(2));
    /* Failed replacement preserves existing selection, reports failure and
     * removes only this writer's temporary file. */
    assert(!dosbox_setting_save(3,1));
    assert(!dosbox_setting_path(3,path,sizeof(path))); assert(!unlink(path));
    assert(!mkdir(path,0755));
    assert(dosbox_setting_save(3,0)==-1);
    assert(!rmdir(path));
    assert(!dosbox_setting_path(1,path,sizeof(path))); assert(!unlink(path));
    assert(!dosbox_setting_path(2,path,sizeof(path))); assert(!unlink(path));
    puts("H51-06 preferences PASS missing/invalid/independent/override/failed-save");
}
