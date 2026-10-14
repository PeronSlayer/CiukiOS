/* SPDX-License-Identifier: MIT */
#include "sound_storage.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
