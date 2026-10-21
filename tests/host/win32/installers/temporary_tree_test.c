/* Execute the actual extraction entry points at their reservation boundary.
 * API fault injection only: no emulated CAB or INF engine, no PE evidence.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <w32/shell.h>

enum { RESERVE_FAIL, DELETE_RETRY, DIRECTORY_COLLISION, FILE_COLLISION, EXTRACT_FAIL };
static unsigned scenario,reserved,created,foreign,deletes,removals,code;
static jmp_buf finished;
static const char tree[]="C:\\WINDOWS\\TEMP\\reserved.tmp";
DWORD W32API GetModuleFileNameA(HANDLE module,char *out,DWORD cap)
{(void)module;const char *name="C:\\Fixtures\\setup.exe";assert(cap>strlen(name));strcpy(out,name);return (DWORD)strlen(name);}
DWORD W32API GetTempPathA(DWORD cap,char *out)
{const char *name="C:\\WINDOWS\\TEMP\\";assert(cap>strlen(name));strcpy(out,name);return (DWORD)strlen(name);}
DWORD W32API GetTempFileNameA(const char *path,const char *prefix,DWORD unique,char *out)
{(void)path;(void)prefix;assert(!unique);strcpy(out,tree);reserved=scenario!=RESERVE_FAIL;return reserved;}
BOOL W32API DeleteFileA(const char *path)
{
    deletes++;
    assert(!strcmp(path,tree)); /* No recognised file in a foreign tree may be touched. */
    assert(reserved && !foreign);
    if(scenario==DELETE_RETRY && deletes==1)return 0;
    reserved=0;return 1;
}
BOOL W32API CreateDirectoryA(const char *path,SECURITY_ATTRIBUTES *security)
{
    assert(!strcmp(path,tree) && !security && !reserved && !created);
    if(scenario==DIRECTORY_COLLISION || scenario==FILE_COLLISION){foreign=1;return 0;}
    created=1;return 1;
}
DWORD W32API GetFileAttributesA(const char *path)
{
    if(foreign)return scenario==FILE_COLLISION?128:16;
    if(!strcmp(path,tree))return reserved?128:created?16:~0u;
    return ~0u;
}
BOOL W32API RemoveDirectoryA(const char *path)
{assert(!strcmp(path,tree) && created && !foreign);created=0;removals++;return 1;}
/* No engine executes: a successfully owned directory reaches the loader,
 * fails extraction, then must be removed by the real cleanup path. */
HANDLE W32API LoadLibraryExA(const char *name,HANDLE file,DWORD flags)
{(void)name;(void)file;(void)flags;return 0;}
void W32API ExitProcess(DWORD result){code=result;longjmp(finished,1);}

#define entry fixture_entry
#include FIXTURE_SOURCE
#undef entry

int main(void)
{
    for(scenario=RESERVE_FAIL;scenario<=EXTRACT_FAIL;scenario++){
        reserved=created=foreign=deletes=removals=0;code=~0u;
        if(!setjmp(finished)){fixture_entry();assert(0);}
        assert(code!=0 && !reserved && !created);
        assert(foreign==(scenario==DIRECTORY_COLLISION || scenario==FILE_COLLISION));
        assert(deletes==(scenario==RESERVE_FAIL?0u:scenario==DELETE_RETRY?2u:1u));
        assert(removals==(scenario==EXTRACT_FAIL?1u:0u));
    }
    puts("PASS actual fixture cleanup: reservation failure, delete retry, foreign directory/file collision, owned-tree extraction failure");
    return 0;
}
