/* Bounded PCM recorder/editor. SPDX-License-Identifier: MIT */
#include <unistd.h>
#include "recorder.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void show(struct recorder_app *a)
{
    struct du_widget *w=du_get(a->ui,REC_STATUS);
    char text[256]; snprintf(text,sizeof(text),"%s  %u.%02u / %u.%02u s%s",a->status,
        a->position/REC_RATE,a->position%REC_RATE/480,a->frames/REC_RATE,a->frames%REC_RATE/480,
        a->dirty ? "  (unsaved)" : "");
    if (w && strcmp(w->text,text)) { du_set_text(w,text); du_invalidate(a->ui,w->box); }
    for (unsigned i=0;i<a->ui->count;i++) {
        w=&a->ui->widgets[i]; int disabled=0;
        if (w->id==REC_RECORD) disabled=a->recording || a->playing || a->frames==REC_FRAMES;
        if (w->id==REC_PLAY) disabled=a->recording || a->playing || !a->frames;
        if (w->id==REC_SOURCE || w->id==REC_GAIN || (w->id>=REC_SAVE && w->id<=REC_POSITION))
            disabled=a->recording || a->playing;
        if (w->disabled!=disabled) { w->disabled=(uint8_t)disabled; du_invalidate(a->ui,w->box); }
        if (w->id==REC_POSITION && a->ui->capture<0) {
            int value=a->frames ? (int)((uint64_t)a->position*1000/a->frames) : 0;
            if (w->value!=value) { w->value=value; du_invalidate(a->ui,w->box); }
        }
    }
}
static void stop(struct recorder_app *a)
{
    int e=a->backend.stop ? a->backend.stop(a->backend.ctx) : 0;
    a->recording=a->playing=0;
    snprintf(a->status,sizeof(a->status),e ? "Stop failed (error %d)" : "Stopped",e);
}
static void fail(struct recorder_app *a, int error)
{
    stop(a);
    const char *reason=error==-ENODEV ? "no audio device is available" :
        error==-EOPNOTSUPP ? "this audio driver does not support recording" :
        error==-EBUSY ? "the audio device is in use" :
        error==-ENOMEM ? "not enough memory" : "device error or capture overrun";
    snprintf(a->status,sizeof(a->status),"Audio failed: %s; captured data retained",reason);
}
void recorder_pump(struct recorder_app *a)
{
    /* At most 1024 frames per turn; the UI remains responsive to Stop. */
    if (a->recording) for (unsigned i=0;i<8 && a->recording;i++) {
        unsigned n=REC_FRAMES-a->frames; if (n>128) n=128;
        if (!n) { stop(a); break; }
        int r=a->backend.read ? a->backend.read(a->backend.ctx,a->pcm+a->frames*2,n) : -ENODEV;
        if (r==-EAGAIN || r==-EINTR) break;
        if (r<0 || (unsigned)r>n) { fail(a,r<0 ? r : -EIO); break; }
        if (!r) break;
        a->frames+=(unsigned)r; a->position=a->frames; a->dirty=1;
    }
    if (a->playing) {
        unsigned n=a->frames-a->position; if (n>1024) n=1024;
        if (!n) {
            int r=a->backend.write ? a->backend.write(a->backend.ctx,NULL,0) : 0;
            if (r<0 && r!=-EAGAIN && r!=-EINTR) fail(a,r);
            else if (!r) stop(a);
        }
        else {
            int r=a->backend.write ? a->backend.write(a->backend.ctx,a->pcm+a->position*2,n) : -ENODEV;
            if (r>=0 && (unsigned)r<=n) a->position+=(unsigned)r;
            else if (r!=-EAGAIN && r!=-EINTR) fail(a,r<0 ? r : -EIO);
        }
    }
    show(a);
}
static void put32(uint8_t *p,unsigned v)
{ for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static unsigned get32(const uint8_t *p)
{ return (unsigned)p[0] | (unsigned)p[1]<<8 | (unsigned)p[2]<<16 | (unsigned)p[3]<<24; }
int recorder_save(struct recorder_app *a,const char *path)
{
    if (a->recording || a->playing || !path || strlen(path)>=sizeof(a->path)) return -EINVAL;
    char temp[288]; snprintf(temp,sizeof(temp),"%s.%ld.rec-tmp",path,(long)getpid());
    FILE *f=fopen(temp,"wbx"); if (!f) return -errno;
    uint8_t h[44]={0}; memcpy(h,"RIFF",4); put32(h+4,36+a->frames*4);
    memcpy(h+8,"WAVEfmt ",8); put32(h+16,16); h[20]=1; h[22]=2;
    put32(h+24,REC_RATE); put32(h+28,REC_RATE*4); h[32]=4; h[34]=16;
    memcpy(h+36,"data",4); put32(h+40,a->frames*4);
    int error=0;
    if (fwrite(h,1,44,f)!=44 || (a->frames && fwrite(a->pcm,4,a->frames,f)!=a->frames) ||
        fflush(f) || fsync(fileno(f))) error=-EIO;
    if (fclose(f)) error=-EIO;
    if (!error && rename(temp,path)) error=-errno;
    if (error) unlink(temp);
    else { snprintf(a->path,sizeof(a->path),"%s",path); a->dirty=0; }
    return error;
}
int recorder_open(struct recorder_app *a,const char *path)
{
    if (!path || strlen(path)>=sizeof(a->path) || a->recording || a->playing) return -EINVAL;
    FILE *f=fopen(path,"rb"); if (!f) return -errno;
    uint8_t h[44]; unsigned frames=0; int error=-EINVAL,valid=0;
    if (fread(h,1,44,f)==44 && !memcmp(h,"RIFF",4) && !memcmp(h+8,"WAVEfmt ",8) &&
        get32(h+16)==16 && h[20]==1 && !h[21] && h[22]==2 && !h[23] &&
        get32(h+24)==REC_RATE && get32(h+28)==REC_RATE*4 && h[32]==4 && !h[33] &&
        h[34]==16 && !h[35] && !memcmp(h+36,"data",4) && get32(h+40)%4==0 &&
        get32(h+40)<=REC_FRAMES*4 && get32(h+4)==get32(h+40)+36) {
        frames=get32(h+40)/4; valid=1;
    }
    int16_t *pcm=frames ? malloc(REC_FRAMES*4) : NULL;
    if (frames && !pcm) error=-ENOMEM;
    if (valid && (!frames || (pcm && fread(pcm,4,frames,f)==frames)) && fgetc(f)==EOF && !ferror(f)) {
        free(a->pcm); a->pcm=pcm; pcm=NULL; a->frames=frames; a->position=0; a->dirty=0;
        snprintf(a->path,sizeof(a->path),"%s",path); error=0;
    }
    free(pcm); fclose(f); return error;
}
int recorder_close(struct recorder_app *a)
{
    stop(a);
    if (!a->dirty) return 1;
    a->closing=1;
    static const char *const buttons[]={"Save As...","Discard","Cancel"};
    du_message(a->ui,REC_D_CLOSE,"Sound Recorder","Save changes to the recording?",DU_DLG_ICON_QUESTION,buttons,3,0,2);
    return 0;
}
static void finish_close(struct recorder_app *a,int accept)
{
    a->closing=0;
    if (a->window) { if (accept) du_window_close(a->window); else du_window_close_reply(a->window,0); }
}
void recorder_event(struct du_ui *u,const struct du_event *e,void *ctx)
{
    struct recorder_app *a=ctx;
    if (e->type==DU_EV_TICK) { recorder_pump(a); return; }
    if (e->type==DU_EV_CHANGE && !a->recording && !a->playing) {
        unsigned v=e->index<0 ? 0 : (unsigned)e->index;
        if (e->id==REC_POSITION && v<=1000) a->position=(unsigned)((uint64_t)a->frames*v/1000);
        if (e->id==REC_GAIN && v<=100) a->gain=v;
    }
    if (e->type==DU_EV_DIALOG) {
        if (e->id==REC_D_CLOSE) {
            if (e->index==1) { a->dirty=0; finish_close(a,1); }
            else if (e->index==0) du_file_dialog(u,REC_D_SAVE,1,"Save PCM WAV","/Documents","Recording.wav",NULL,NULL);
            else finish_close(a,0);
        } else if (e->id==REC_D_SAVE || e->id==REC_D_OPEN) {
            int error=0;
            if (!e->index && e->text) error=e->id==REC_D_SAVE ? recorder_save(a,e->text) : recorder_open(a,e->text);
            snprintf(a->status,sizeof(a->status),error ? "File operation failed (error %d)" : "Ready",error);
            if (a->closing) finish_close(a,!error && !e->index);
        }
    }
    if (e->type==DU_EV_ACTIVATE) {
        if (e->id==REC_STOP) stop(a);
        else if (!a->recording && !a->playing) switch (e->id) {
        case REC_RECORD: {
            if (!a->pcm) a->pcm=malloc(REC_FRAMES*4);
            int error=!a->pcm ? -ENOMEM : a->frames==REC_FRAMES ? -ENOSPC :
                a->backend.start ? a->backend.start(a->backend.ctx,a->source,a->gain) : -ENODEV;
            if (error) fail(a,error);
            else { a->recording=1; snprintf(a->status,sizeof(a->status),"Recording (60 second limit)"); }
            break;
        }
        case REC_PLAY:
            if (a->frames) { if (a->position==a->frames) a->position=0; a->playing=1; strcpy(a->status,"Playing"); }
            break;
        case REC_SOURCE:
            a->source^=1; du_set_text(du_get(u,REC_SOURCE),a->source ? "Source: Line In" : "Source: Microphone");
            du_invalidate(u,du_get(u,REC_SOURCE)->box); break;
        case REC_SAVE: du_file_dialog(u,REC_D_SAVE,1,"Save PCM WAV","/Documents","Recording.wav",NULL,NULL); break;
        case REC_OPEN:
            if (a->dirty) strcpy(a->status,"Save the current recording before opening another.");
            else du_file_dialog(u,REC_D_OPEN,0,"Open PCM WAV (48000 Hz, stereo, 16-bit)","/Documents","",NULL,NULL);
            break;
        case REC_NEW:
            if (a->dirty) strcpy(a->status,"Save the current recording before starting a new one.");
            else { a->frames=a->position=0; a->path[0]=0; }
            break;
        case REC_REVERSE:
            for (unsigned i=0;i<a->frames/2;i++) for (unsigned ch=0;ch<2;ch++) {
                int16_t t=a->pcm[i*2+ch]; a->pcm[i*2+ch]=a->pcm[(a->frames-1-i)*2+ch]; a->pcm[(a->frames-1-i)*2+ch]=t;
            }
            a->dirty|=a->frames!=0; break;
        case REC_LOUDER: case REC_QUIETER:
            for (unsigned i=0;i<a->frames*2;i++) {
                int v=(int)a->pcm[i]*(e->id==REC_LOUDER ? 125 : 80)/100;
                a->pcm[i]=(int16_t)(v>32767 ? 32767 : v< -32768 ? -32768 : v);
            }
            a->dirty|=a->frames!=0; break;
        case REC_TRIM_BEFORE:
            if (!a->frames || !a->position) break;
            memmove(a->pcm,a->pcm+a->position*2,(a->frames-a->position)*4);
            a->frames-=a->position; a->position=0; a->dirty=1; break;
        case REC_TRIM_AFTER:
            if (a->position<a->frames) { a->frames=a->position; a->dirty=1; }
            break;
        }
    }
    show(a);
}
void recorder_attach(struct recorder_app *a,struct du_ui *u,struct du_window *w,const struct recorder_backend *backend)
{
    a->ui=u; a->window=w; a->backend=*backend; a->gain=100;
    u->event=recorder_event; u->user=a;
    const char *labels[]={"Record","Play","Stop","Save As...","Open...","New",
        "Reverse","Volume +25%","Volume -20%","Delete before","Delete after"};
    for (unsigned i=0;i<11;i++) du_add(u,DU_BUTTON,REC_RECORD+(int)i,
        (struct du_rect){12+(int)(i%4)*116,12+(int)(i/4)*32,110,26},labels[i]);
    du_add(u,DU_BUTTON,REC_SOURCE,(struct du_rect){12,116,210,26},"Source: Microphone");
    du_add(u,DU_LABEL,1699,(struct du_rect){244,116,112,20},"Input level:");
    struct du_widget *v=du_add(u,DU_TRACKBAR,REC_GAIN,(struct du_rect){350,114,110,24},"");
    du_trackbar_set(v,0,100,100,5,20,0);
    du_add(u,DU_LABEL,1698,(struct du_rect){12,152,448,20},"Position (editing and playback):");
    v=du_add(u,DU_TRACKBAR,REC_POSITION,(struct du_rect){12,176,448,24},"");
    du_trackbar_set(v,0,1000,0,10,100,0);
    du_add(u,DU_STATUS,REC_STATUS,(struct du_rect){0,REC_H-24,REC_W,24},"");
    strcpy(a->status,"Ready"); show(a);
}
void recorder_destroy(struct recorder_app *a)
{ stop(a); free(a->pcm); a->pcm=NULL; }
