/* SPDX-License-Identifier: MIT */
#include "sound_storage.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

static void load_file(struct sound_scheme *s, const char *path)
{
    if (!path) return;
    FILE *f = fopen(path,"rb"); if (!f) return;
    char *text = malloc(SOUND_TEXT_MAX+1);
    if (text) {
        size_t n = fread(text,1,SOUND_TEXT_MAX+1,f);
        if (!ferror(f)) (void)sound_parse(s,text,n);
        free(text);
    }
    fclose(f);
}
int sound_load_at(struct sound_scheme *s, const char *cfg, const char *legacy, const char *directory)
{
    sound_defaults(s); load_file(s,cfg); load_file(s,legacy);
    struct reg_root *r = reg_open_root_at("Desktop",directory); if (!r) return -1;
    int enabled; int32_t volume;
    if (!reg_get_bool(r,"Desktop\\Sounds","Enabled",&enabled)) s->enabled = enabled;
    if (!reg_get_int32(r,"Desktop\\Sounds","Volume",&volume) && volume >= 0 && volume <= 100) s->volume = volume;
    for (unsigned i = 0; i < SOUND_EVENTS; i++) {
        char key[128], value[SOUND_NAME_MAX], path[128]; size_t n = sizeof(value);
        snprintf(key,sizeof(key),"Desktop\\Sounds\\%s",sound_keys[i]);
        if (reg_get_string(r,key,"File",value,&n)) continue;
        /* Validate the raw value: serializing it to cfg text would let embedded
         * newlines masquerade as additional preferences. Empty means disabled. */
        struct sound_scheme next = *s; memcpy(next.file[i],value,n);
        int result = sound_path(&next,i,path,sizeof(path));
        if (!result || result == -ENOENT) *s = next;
    }
    reg_close_root(r); return 0;
}
int sound_save_at(const struct sound_scheme *s, const char *directory)
{
    char temp[256], *check = malloc(SOUND_TEXT_MAX);
    if (!check) return -1;
    size_t valid = sound_format(s,check,SOUND_TEXT_MAX); free(check);
    if (!valid) { errno = EINVAL; return -1; }
    int n = snprintf(temp,sizeof(temp),"%s/sounds.cpl",directory);
    if (n < 0 || (size_t)n >= sizeof(temp)) { errno = ENAMETOOLONG; return -1; }
    if (mkdir(directory,0755) && errno != EEXIST) return -1;
    FILE *f = fopen(temp,"wb"); if (!f) return -1;
    int error = 0;
    if (fprintf(f,"[Desktop\\Sounds]\nEnabled=%s\nVolume=%d\n",s->enabled ? "true" : "false",s->volume) < 0) error = EIO;
    for (unsigned i = 0; i < SOUND_EVENTS && !error; i++)
        if (fprintf(f,"[Desktop\\Sounds\\%s]\nFile=\"%s\"\n",sound_keys[i],s->file[i]) < 0) error = EIO;
    if (fflush(f) || fsync(fileno(f))) error = errno ? errno : EIO;
    if (fclose(f) && !error) error = errno;
    if (!error) {
        struct reg_root *r = reg_open_root_at("Desktop",directory);
        if (!r) error = errno;
        else { if (reg_import(r,temp)) error = errno; reg_close_root(r); }
    }
    unlink(temp);
    if (error) { errno = error; return -1; }
    return 0;
}

static int scheme_name(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (!n || n >= SOUND_SCHEME_NAME) return 0;
    char lower[SOUND_SCHEME_NAME];
    for (size_t i=0;i<=n;i++) lower[i]=(char)(s[i]>='A' && s[i]<='Z' ? s[i]+32 : s[i]);
    if (!strcmp(lower,"default") || !strcmp(lower,"no sounds") || !strcmp(lower,"custom")) return 0;
    for (size_t i=0;i<n;i++) if (!((s[i]>='a' && s[i]<='z') ||
        (s[i]>='A' && s[i]<='Z') || (s[i]>='0' && s[i]<='9') || s[i]=='_' || s[i]=='-')) return 0;
    return 1;
}
int sound_scheme_list_at(const char *directory, char names[][SOUND_SCHEME_NAME], unsigned capacity)
{
    DIR *d=opendir(directory);
    if (!d) return errno==ENOENT ? 0 : -errno;
    unsigned count=0; struct dirent *entry;
    while ((entry=readdir(d))) {
        size_t n=strlen(entry->d_name);
        if (n<5 || n-4>=SOUND_SCHEME_NAME || strcmp(entry->d_name+n-4,".cfg")) continue;
        char name[SOUND_SCHEME_NAME]; memcpy(name,entry->d_name,n-4); name[n-4]=0;
        if (!scheme_name(name)) continue;
        if (count==capacity) { closedir(d); return -ENOSPC; }
        snprintf(names[count++],SOUND_SCHEME_NAME,"%s",name);
    }
    closedir(d);
    /* Stable menu order independent of FAT directory allocation. */
    for (unsigned i=1;i<count;i++) for (unsigned j=i;j && strcmp(names[j-1],names[j])>0;j--) {
        char swap[SOUND_SCHEME_NAME]; memcpy(swap,names[j],sizeof(swap));
        memcpy(names[j],names[j-1],sizeof(swap)); memcpy(names[j-1],swap,sizeof(swap));
    }
    return (int)count;
}
int sound_scheme_at(const char *directory, unsigned operation, const char *name, struct sound_scheme *s)
{
    if (!scheme_name(name) || operation>SOUND_SCHEME_DELETE) return -EINVAL;
    char path[256], temp[256];
    int n=snprintf(path,sizeof(path),"%s/%s.cfg",directory,name);
    int t=snprintf(temp,sizeof(temp),"%s/.%s.%ld.tmp",directory,name,(long)getpid());
    if (n<0 || (size_t)n>=sizeof(path) || t<0 || (size_t)t>=sizeof(temp)) return -ENAMETOOLONG;
    if (operation==SOUND_SCHEME_DELETE) return unlink(path) ? -errno : 0;
    struct stat existing;
    if (operation==SOUND_SCHEME_SAVE && stat(path,&existing)) {
        if (errno!=ENOENT) return -errno;
        char names[SOUND_SCHEMES_MAX][SOUND_SCHEME_NAME];
        int count=sound_scheme_list_at(directory,names,SOUND_SCHEMES_MAX);
        if (count<0) return count;
        if (count==(int)SOUND_SCHEMES_MAX) return -ENOSPC;
    }
    char *text=malloc(SOUND_TEXT_MAX+1); if (!text) return -ENOMEM;
    int error=0;
    if (operation==SOUND_SCHEME_LOAD) {
        FILE *f=fopen(path,"rb");
        if (!f) error=-errno;
        else {
            size_t bytes=fread(text,1,SOUND_TEXT_MAX+1,f);
            if (ferror(f)) error=-EIO;
            else { struct sound_scheme next; sound_defaults(&next);
                error=sound_parse(&next,text,bytes); if (!error) *s=next; }
            fclose(f);
        }
    } else {
        size_t bytes=sound_format(s,text,SOUND_TEXT_MAX);
        if (!bytes) error=-EINVAL;
        if (!error && mkdir(directory,0755) && errno!=EEXIST) error=-errno;
        FILE *f=error ? NULL : fopen(temp,"wbx");
        if (!error && !f) error=-errno;
        if (f) {
            if (fwrite(text,1,bytes,f)!=bytes || fflush(f) || fsync(fileno(f))) error=-EIO;
            if (fclose(f)) error=-EIO;
            if (!error && rename(temp,path)) error=-errno;
            if (error) unlink(temp);
        }
    }
    free(text); return error;
}
int sound_validate_file(const char *path)
{
    struct sound_scheme s; sound_defaults(&s);
    if (sound_assign(&s,0,path)) return -EINVAL;
    FILE *f=fopen(path,"rb"); if (!f) return -errno;
    void *data=malloc(SOUND_FILE_MAX+1); int error=-ENOMEM;
    if (data) {
        size_t n=fread(data,1,SOUND_FILE_MAX+1,f); struct sound_clip clip;
        error=ferror(f) ? -EIO : sound_wav(data,n,&clip); free(data);
    }
    fclose(f); return error;
}
