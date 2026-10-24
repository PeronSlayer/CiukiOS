/* G22 production recorder model, UI and WAV round trip. SPDX-License-Identifier: MIT */
#include <unistd.h>
#include "recorder.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned starts, stops, source, gain, writes, reads;
static int input_error, read_once, output_error;
static int start(void *c,unsigned s,unsigned g)
{ (void)c; starts++; source=s; gain=g; return input_error; }
static int capture(void *c,int16_t *pcm,unsigned frames)
{
    (void)c; assert(frames<=128); reads++;
    if (input_error) return input_error;
    if (read_once) return -EAGAIN;
    read_once=1;
    for (unsigned i=0;i<frames;i++) { pcm[i*2]=(int16_t)i; pcm[i*2+1]=(int16_t)-(int)i; }
    return (int)frames;
}
static int output(void *c,const int16_t *pcm,unsigned frames)
{
    (void)c; if (!frames) { assert(!pcm); return 0; }
    if (output_error) return output_error;
    unsigned n=frames>3 ? 3 : frames; writes+=n; return (int)n;
}
static int stop(void *c) { (void)c; stops++; return 0; }
void du_window_close(struct du_window *w) { (void)w; }
void du_window_close_reply(struct du_window *w,int later) { (void)w; (void)later; }
static void event(struct recorder_app *a,enum du_event_type type,int id,int index,const char *text)
{ struct du_event e={.type=type,.id=id,.index=index,.text=text}; recorder_event(a->ui,&e,a); }
static void click(struct recorder_app *a,int id) { event(a,DU_EV_ACTIVATE,id,0,NULL); }
int main(int argc,char **argv)
{
    assert(argc==2); static struct du_ui ui; static uint32_t pixels[REC_W*REC_H];
    struct recorder_app a={0}; struct recorder_backend backend={start,capture,output,stop,NULL};
    du_init(&ui,pixels,REC_W,REC_H); recorder_attach(&a,&ui,NULL,&backend);
    du_render(&ui);
    assert(du_get(&ui,REC_PLAY)->disabled);
    click(&a,REC_TRIM_BEFORE); /* empty editor is safe */
    input_error=-19; click(&a,REC_RECORD); assert(!a.recording && !a.frames && strstr(a.status,"failed"));
    input_error=0; click(&a,REC_SOURCE); event(&a,DU_EV_CHANGE,REC_GAIN,50,NULL);
    click(&a,REC_RECORD); assert(a.recording && source==1 && gain==50 && starts==2);
    recorder_pump(&a); assert(a.frames==128 && a.dirty && a.position==128 && reads==2);
    du_render(&ui);
    assert(a.pcm[2]==1 && a.pcm[3]==-1);
    click(&a,REC_REVERSE); assert(a.pcm[0]==0); /* editing locked while recording */
    input_error=-EIO; recorder_pump(&a); assert(!a.recording && a.frames==128 && strstr(a.status,"retained"));
    input_error=0;
    click(&a,REC_REVERSE); assert(a.pcm[0]==127 && a.pcm[1]==-127);
    a.pcm[0]=32767; a.pcm[1]=-32768; click(&a,REC_LOUDER);
    assert(a.pcm[0]==32767 && a.pcm[1]==-32768);
    click(&a,REC_QUIETER); assert(a.pcm[0]==26213 && a.pcm[1]==-26214);
    event(&a,DU_EV_CHANGE,REC_POSITION,500,NULL); assert(a.position==64);
    click(&a,REC_TRIM_BEFORE); assert(a.frames==64 && !a.position);
    event(&a,DU_EV_CHANGE,REC_POSITION,500,NULL); click(&a,REC_TRIM_AFTER); assert(a.frames==32 && a.position==32);
    char path[512]; snprintf(path,sizeof(path),"%s/recording.wav",argv[1]);
    assert(!recorder_save(&a,path) && !a.dirty);
    int16_t sample=a.pcm[0];
    du_init(&ui,pixels,REC_W,REC_H);
    struct recorder_app loaded={0}; recorder_attach(&loaded,&ui,NULL,&backend);
    assert(!recorder_open(&loaded,path) && loaded.frames==32 && loaded.pcm[0]==sample);
    assert(recorder_open(&loaded,"/not-present.wav")==-ENOENT && loaded.frames==32);
    loaded.dirty=1; assert(recorder_save(&loaded,"/no-such-folder/record.wav")<0 && loaded.dirty);
    assert(!recorder_close(&loaded) && loaded.closing && ui.dialog.id==REC_D_CLOSE);
    du_dialog_close(&ui); event(&loaded,DU_EV_DIALOG,REC_D_CLOSE,2,NULL); assert(!loaded.closing && loaded.dirty);
    click(&loaded,REC_NEW); assert(loaded.frames==32);
    loaded.dirty=0; loaded.position=0; click(&loaded,REC_PLAY);
    output_error=-EINTR; recorder_pump(&loaded); assert(!loaded.position && loaded.playing);
    output_error=0;
    for (unsigned i=0;i<20 && loaded.playing;i++) recorder_pump(&loaded);
    assert(!loaded.playing && writes==32 && loaded.position==32);
    click(&loaded,REC_PLAY); output_error=-EIO; recorder_pump(&loaded); assert(!loaded.playing && strstr(loaded.status,"failed"));
    assert(stops>2);
    loaded.frames=loaded.position=0; loaded.dirty=1;
    assert(!recorder_save(&loaded,path) && !loaded.dirty);
    assert(!recorder_open(&loaded,path) && !loaded.frames);
    FILE *bad=fopen(path,"wb"); assert(bad); fputs("not a WAV",bad); fclose(bad);
    assert(recorder_open(&loaded,path)==-EINVAL && !loaded.frames);
    recorder_destroy(&a); recorder_destroy(&loaded); du_dialog_close(&ui); unlink(path);
    puts("recorder: PASS capture failure/EAGAIN, retained data, bounded reads, edit lock, effects/clipping, trim, WAV round trip, save failure, unsaved close, partial writes/drain");
}
