/* SPDX-License-Identifier: MIT
 * Exact same transfer presentation for Files and the shell's toolkit host. */
#ifndef CIUKI_FILES_TRANSFER_UI_H
#define CIUKI_FILES_TRANSFER_UI_H
#include "transfer.h"
#include "../libdeskui/deskui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static inline void fm_progress_dialog(struct du_ui *ui, int id, enum fm_job_kind kind,
    const char *destination, struct transfer_view *v, uint64_t started_at)
{
    const char *title=kind==FM_COPY ? "Copying..." : kind==FM_MOVE ? "Moving..." : kind==FM_DELETE ? "Deleting..." : "Properties";
    if (v->preparing) {
        du_progress(ui,id,title,"Preparing the selected items...",0,100); return;
    }
    v->elapsed=job_now()-started_at;

    char name[201], text[DU_DIALOG_TEXT];
    snprintf(name,sizeof(name),"%.200s",fm_basename(v->source));
    fm_parent(v->source,v->source); fm_display_path(v->source,v->source);
    snprintf(v->target,sizeof(v->target),"%s",destination);
    fm_display_path(v->target,v->target);
    unsigned percent=v->total ? (unsigned)((v->bytes > v->total ? v->total : v->bytes)*100/v->total) : 0;
    if (percent==100 && v->state!=FM_DONE) percent=99;
    if (kind==FM_PROPERTIES) snprintf(text,sizeof(text),"Counting: %.200s\n%llu files, %llu folders",name,(unsigned long long)v->files,(unsigned long long)v->folders);
    else if (kind==FM_DELETE) snprintf(text,sizeof(text),"%s: %.70s\nFrom: %.70s\nItems: %u of %u\nElapsed: %llu s",v->counting ? "Counting" : "Deleting",name,v->source,v->items,v->count,(unsigned long long)(v->elapsed/1000000000ull));
    else {
        char rate[100]="Rate: calculating...   Remaining: calculating...";
        if (v->rate) snprintf(rate,sizeof(rate),"Rate: %llu KB/s   Remaining: %llu s",(unsigned long long)(v->rate/1024),(unsigned long long)((v->total>v->bytes ? v->total-v->bytes : 0)/v->rate));
        snprintf(text,sizeof(text),"%s: %.65s\nFrom: %.65s\nTo: %.65s\nBytes: %llu of %llu (%u%%)\nFile: %llu of %llu bytes\nItems: %u of %u   Elapsed: %llu s\n%s",v->counting ? "Counting" : kind==FM_COPY ? "Copying" : "Moving",name,v->source,v->target,(unsigned long long)v->bytes,(unsigned long long)v->total,percent,(unsigned long long)v->file_bytes,(unsigned long long)v->file_total,v->items,v->count,(unsigned long long)(v->elapsed/1000000000ull),rate);
    }
    unsigned value=kind==FM_DELETE ? v->items : percent, total=kind==FM_DELETE ? v->count : 100;
    du_progress(ui,id,title,text,value,total);
}
static inline void fm_dialog_size(char *out, size_t n, uint64_t bytes)
{
    static const char *const units[] = {"KB", "MB", "GB", "TB"};
    if (bytes < 1024) { snprintf(out, n, "%llu bytes", (unsigned long long)bytes); return; }
    uint64_t scaled = bytes; unsigned u = 0;
    while (scaled >= 1024 * 1024 && u < 3) { scaled /= 1024; u++; }
    uint64_t hundredths = scaled * 100 / 1024;
    if (hundredths < 1000) snprintf(out, n, "%llu.%02llu %s", (unsigned long long)(hundredths / 100), (unsigned long long)(hundredths % 100), units[u]);
    else if (hundredths < 10000) snprintf(out, n, "%llu.%llu %s", (unsigned long long)(hundredths / 100), (unsigned long long)(hundredths % 100 / 10), units[u]);
    else snprintf(out, n, "%llu %s", (unsigned long long)(hundredths / 100), units[u]);
}

static inline void fm_dialog_time(char *out, size_t n, int64_t stamp)
{
    out[0]=0; time_t when=(time_t)stamp; struct tm t;
    if (stamp>0 && gmtime_r(&when,&t)) strftime(out,n,"%Y-%m-%d %H:%M",&t);
}
static inline void fm_replace_dialog(struct du_ui *ui, int id, const struct transfer_view *v)
{
    static const char *const buttons[]={"&Yes","Yes to &All","&No","Cancel"};
    char text[DU_DIALOG_TEXT], a1[48], a2[48], t1[24], t2[24];
    const char *name = fm_basename(v->target);
    if (v->prompt_dir)
        snprintf(text, sizeof(text), "This folder already contains a folder named '%.120s'.\n\n"
                 "If files in the existing folder have the same names as files in\n"
                 "the folder you are copying, you will be asked before each one\n"
                 "is replaced. Do you still want to copy or move the folder?", name);
    else {
        fm_dialog_size(a1, sizeof(a1), v->existing.size); fm_dialog_size(a2, sizeof(a2), v->meta.size);
        fm_dialog_time(t1, sizeof(t1), v->existing.modified); fm_dialog_time(t2, sizeof(t2), v->meta.modified);
        snprintf(text, sizeof(text), "This folder already contains a file named '%.120s'.\n\n"
                 "Would you like to replace the existing file\n\t%s, modified %s\n\nwith this one?\n\t%s, modified %s",
                 name, a1, t1[0] ? t1 : "-", a2, t2[0] ? t2 : "-");
    }
    du_message(ui, id, v->prompt_dir ? "Confirm Folder Replace" : "Confirm File Replace", text,
               DU_DLG_ICON_QUESTION, buttons, 4, 0, 3);
}
#endif
