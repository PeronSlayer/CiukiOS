/* fx-hlis5: the session desktop creates the profile's Local Settings\Temp
 * (apps/desktop/profile.h profile_temp_ready_at), as Windows 2000/XP copy it
 * from Default User. Host directories stand for the profile volume.
 * SPDX-License-Identifier: MIT */
#include <sys/stat.h>
static int competing_mkdir(const char *path,mode_t mode);
#define mkdir competing_mkdir
#include "../../../apps/desktop/profile.h"
#undef mkdir
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
static const char *race_path;
static int race_directory, race_calls;
static int competing_mkdir(const char *path,mode_t mode)
{
    if(race_path && !strcmp(path,race_path)) {
        race_calls++;
        if(race_directory) assert(!mkdir(path,mode));
        else {
            int fd=open(path,O_CREAT|O_WRONLY|O_EXCL,0600);
            assert(fd>=0); assert(!close(fd));
        }
        errno=EEXIST; return -1;
    }
    return mkdir(path,mode);
}
static int directory(const char *path) { struct stat st; return !stat(path,&st) && S_ISDIR(st.st_mode); }
int main(int argc,char **argv)
{
    assert(argc==2);
    char root[256],temp[256],other[256],blocked[256];
    snprintf(root,sizeof(root),"%s/u000003e8",argv[1]);
    assert(!mkdir(root,0700));
    snprintf(temp,sizeof(temp),"%s/Local Settings/Temp",root);
    assert(!directory(temp));
    /* A fresh profile: both missing components are created, owner-only. */
    assert(!profile_temp_ready_at(root,temp) && directory(temp));
    struct stat st; assert(!stat(temp,&st) && (st.st_mode & 0777)==0700);
    /* Again at the next logon: nothing to do, still success. */
    assert(!profile_temp_ready_at(root,temp) && directory(temp));
    /* Never outside the committed root, never through "..". */
    snprintf(other,sizeof(other),"%s/u000003e9/Local Settings/Temp",argv[1]);
    errno=0; assert(profile_temp_ready_at(root,other)==-1 && errno==EINVAL);
    snprintf(other,sizeof(other),"%s/../escape/Temp",root);
    errno=0; assert(profile_temp_ready_at(root,other)==-1 && errno==EINVAL);
    errno=0; assert(profile_temp_ready_at(NULL,temp)==-1 && errno==EINVAL);
    errno=0; assert(profile_temp_ready_at(root,root)==-1 && errno==EINVAL);
    /* A file where a folder belongs is reported, not replaced. */
    snprintf(blocked,sizeof(blocked),"%s/Blocked",root);
    int fd=open(blocked,O_CREAT|O_WRONLY|O_EXCL,0600); assert(fd>=0); close(fd);
    snprintf(other,sizeof(other),"%s/Blocked/Temp",root);
    errno=0; assert(profile_temp_ready_at(root,other)==-1 && errno==ENOTDIR);
    /* Deterministic race: initial stat sees ENOENT, but another creator
     * wins at mkdir. Only a competing directory is usable TEMP. */
    assert(!rmdir(temp)); race_path=temp; race_directory=1;
    assert(!profile_temp_ready_at(root,temp) && directory(temp) && race_calls==1);
    assert(!rmdir(temp)); race_directory=0;
    errno=0; assert(profile_temp_ready_at(root,temp)==-1 && errno==ENOTDIR && race_calls==2);
    assert(!stat(temp,&st) && S_ISREG(st.st_mode));
    race_path=NULL;
    puts("PASS profile Local Settings\\Temp created below the committed root, idempotent, confined (fx-hlis5)");
    return 0;
}
