/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_FILES_MODEL_H
#define CIUKI_FILES_MODEL_H
#include <stddef.h>
#include <stdint.h>
#define FM_PATH 1040
#define FM_NAME 766
#define FM_ENTRIES 4096
#define FM_HISTORY 32
#define FM_DEPTH 32
#define FM_CHUNK 16384
enum fm_type { FM_FILE,FM_DIR,FM_OTHER };
struct fm_meta { enum fm_type type; uint64_t size,ino; uint32_t dev,mode; int64_t modified; };
struct fm_entry { char name[FM_NAME]; struct fm_meta meta; unsigned selected; };
typedef int (*fm_emit)(const char *,const struct fm_meta *,void *);
/* Every error is -errno; reads/writes return a positive short count or zero. */
struct fm_vfs {
    void *ctx;
    int (*canonical)(void *,const char *,char *);
    int (*list)(void *,const char *,fm_emit,void *);
    int (*stat_path)(void *,const char *,struct fm_meta *);
    int (*mkdir)(void *,const char *);
    int (*rename)(void *,const char *,const char *);
    int (*unlink)(void *,const char *);
    int (*rmdir)(void *,const char *);
    int (*open)(void *,const char *,int); /* 0 read, 1 exclusive create */
    int (*read)(void *,int,void *,unsigned);
    int (*write)(void *,int,const void *,unsigned);
    int (*sync)(void *,int);
    int (*close)(void *,int);
    int (*metadata)(void *,const char *,const struct fm_meta *); /* optional */
};
struct fm_model {
    struct fm_vfs fs; char path[FM_PATH],history[FM_HISTORY][FM_PATH]; unsigned history_count,history_at;
    struct fm_entry *entries; unsigned count,capacity,active,anchor,sort; int descending;
    char find[FM_NAME]; size_t find_length; uint64_t find_deadline;
    char **clipboard; unsigned clipboard_count; int cut;
};
enum fm_job_kind { FM_COPY,FM_MOVE,FM_DELETE,FM_PROPERTIES };
enum fm_job_state { FM_IDLE,FM_RUNNING,FM_REPLACE,FM_DONE,FM_CANCELLED,FM_FAILED };
enum fm_replace { FM_YES,FM_YES_ALL,FM_NO,FM_CANCEL };
struct fm_frame {
    char source[FM_PATH],target[FM_PATH]; struct fm_meta meta;
    struct fm_entry *entries; unsigned count,next; int phase,approved,exists;
};
struct fm_job {
    struct fm_vfs fs; enum fm_job_kind kind; enum fm_job_state state;
    char **sources; unsigned count,next,depth,completed,skipped,metadata_warnings;
    char destination[FM_PATH],temporary[FM_PATH]; unsigned serial;
    struct fm_frame frames[FM_DEPTH];
    int input,output,error,cleanup_error,yes_all,cancel,skip_seen;
    unsigned used,written; unsigned char buffer[FM_CHUNK];
    uint64_t bytes,files,folders;
};
int fm_join(char *,const char *,const char *);
int fm_beneath(const char *,const char *);
const char *fm_basename(const char *);
const char *fm_type_name(const struct fm_entry *);
void fm_init(struct fm_model *,const struct fm_vfs *);
void fm_destroy(struct fm_model *);
int fm_navigate(struct fm_model *,const char *);
int fm_refresh(struct fm_model *);
int fm_history(struct fm_model *,int);
int fm_up(struct fm_model *);
void fm_sort(struct fm_model *,unsigned,int);
void fm_select(struct fm_model *,unsigned,unsigned);
void fm_select_all(struct fm_model *,int invert);
int fm_find(struct fm_model *,unsigned,uint64_t);
int fm_clipboard(struct fm_model *,int cut);
int fm_create(struct fm_model *,const char *,int directory);
int fm_rename(struct fm_model *,unsigned,const char *);
int fm_job_start(struct fm_job *,const struct fm_vfs *,enum fm_job_kind,char *const *,unsigned,const char *);
int fm_job_step(struct fm_job *);
void fm_job_answer(struct fm_job *,enum fm_replace);
void fm_job_cancel(struct fm_job *);
void fm_job_destroy(struct fm_job *);
const struct fm_vfs *fm_native_vfs(void);
#endif
