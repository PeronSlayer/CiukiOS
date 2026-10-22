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
static void write_config(const void *text, size_t n) {
    char path[128]; assert(!dosbox_setting_path(2,path,sizeof(path)));
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(text,1,n,f)==n); assert(!fclose(f));
}
int main(void) {
    assert(dosbox_game("\\DOOM\\DOOM.EXE -config CIUKI.CFG")==2);
    assert(!dosbox_game("DIR"));
    assert(!mkdir(DOSBOX_SETTINGS_DIR,0755) || errno==EEXIST);
    for (unsigned i=0;i<4;i++) assert(dosbox_setting_load(i)==CIUKI_DOSVM_UNIFIED);
    const char *invalid[]={"DOS kernel=ciukidos\nDOS memory=locked\n", "DOS kernel=freedos-1.3\nDOS memory=locked\n",
        "DOS kernel=master\n", "DOS kernel=unified\nDOS memory=locked\nextra\n", "DOS kernel=unified", ""};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
        write_config(invalid[i],strlen(invalid[i]));
        assert(dosbox_setting_load(2)==CIUKI_DOSVM_UNIFIED && dosbox_memory_load(2)==CIUKI_DOSVM_MEMORY_PAGED);
        assert(dosbox_kernel_value(invalid[i])==CIUKI_DOSVM_UNIFIED);
    }
    const char locked[]="DOS kernel=unified\nDOS memory=locked\n";
    write_config(locked,sizeof(locked)-1);
    assert(dosbox_setting_load(2)==CIUKI_DOSVM_UNIFIED && dosbox_memory_load(2)==CIUKI_DOSVM_MEMORY_LOCKED);
    write_config(locked,sizeof(locked)); /* embedded NUL is not EOF */
    assert(dosbox_memory_load(2)==CIUKI_DOSVM_MEMORY_PAGED);
    char huge[128]; memset(huge,'x',sizeof(huge)); memcpy(huge,locked,sizeof(locked)-1);
    write_config(huge,sizeof(huge)); assert(dosbox_memory_load(2)==CIUKI_DOSVM_MEMORY_PAGED);
    char path[128]; assert(!dosbox_setting_path(2,path,sizeof(path))); assert(!unlink(path));
    assert(dosbox_setting_path(0,path,sizeof(path))<0 && dosbox_setting_path(4,path,sizeof(path))<0);
    puts("PASS unified-only read-only configuration parser: retired/malformed/oversized/NUL files default, locked accepted");
    return 0;
}
