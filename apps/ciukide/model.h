/* SPDX-License-Identifier: MIT */
#ifndef CIUKIDE_MODEL_H
#define CIUKIDE_MODEL_H
#include <stddef.h>
#define IDE_SOURCES 16
#define IDE_LIBRARIES 8
#define IDE_NAME 160
#define IDE_ERRORS 64
#define IDE_LINE 512
#define IDE_LOG 16384
#define IDE_CFG 4096
struct ide_project {
    char sources[IDE_SOURCES][IDE_NAME], libraries[IDE_LIBRARIES][64], output[64];
    unsigned source_count, library_count;
};
struct ide_error { char file[IDE_NAME], message[256]; unsigned line, column; int warning; };
enum ide_state { IDE_IDLE, IDE_BUILDING, IDE_READY, IDE_FAILED, IDE_LAUNCHING, IDE_RUNNING };
struct ide_model {
    struct ide_project project;
    enum ide_state state;
    unsigned revision, build_revision, errors, warnings, dropped;
    int built, exit_code;
    struct ide_error error[IDE_ERRORS];
    char partial[2][IDE_LINE]; size_t partial_len[2]; int overflow[2];
    char log[IDE_LOG + 1]; size_t log_len; int log_truncated;
};
/* Strict bounded project data; names are relative paths, never commands. */
int ide_project_parse(struct ide_project *, const char *, size_t);
int ide_relative_path(const char *);
int ide_diagnostic(const char *, struct ide_error *);
void ide_changed(struct ide_model *);
int ide_build_begin(struct ide_model *);
void ide_output(struct ide_model *, unsigned stream, const char *, size_t);
void ide_output_end(struct ide_model *, unsigned stream);
void ide_build_end(struct ide_model *, int code);
int ide_run_begin(struct ide_model *);
void ide_run_result(struct ide_model *, int pid_or_error);
/* Lexical classes for the textarea's C presentation; comments span lines. */
enum ide_syntax { IDE_C_TEXT, IDE_C_KEYWORD, IDE_C_STRING, IDE_C_COMMENT, IDE_C_NUMBER, IDE_C_PREPROCESSOR };
void ide_highlight(const char *, size_t, unsigned char *classes);
#endif
