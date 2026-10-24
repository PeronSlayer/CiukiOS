/* SPDX-License-Identifier: MIT */
#include "recorder.h"
#include <ciuki/audio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
struct recorder_main { struct du_window window; struct du_ui ui; struct recorder_app app; int input,output; };
static int start(void *ctx,unsigned source,unsigned gain)
{
    struct recorder_main *m=ctx;
    m->input=open("/dev/audio",O_RDONLY|O_CLOEXEC);
    if (m->input<0) return -errno;
    struct ciuki_audio_capture c={.version=1,.size=sizeof(c),.source=source,.gain=gain,
        .rate=REC_RATE,.channels=2,.format=CIUKI_AUDIO_S16LE};
    if (!ciuki_devctl(m->input,AUDIO_CAPTURE_START,&c,sizeof(c))) return 0;
    int e=-errno; close(m->input); m->input=-1; return e;
}
static int capture(void *ctx,int16_t *pcm,unsigned frames)
{
    struct recorder_main *m=ctx;
    struct ciuki_audio_capture_data c={.version=1,.size=sizeof(c),.frames=frames};
    if (ciuki_devctl(m->input,AUDIO_CAPTURE_READ,&c,sizeof(c))) return -errno;
    for (unsigned i=0;i<c.captured*2;i++) pcm[i]=c.pcm[i];
    return (int)c.captured;
}
static int playback(void *ctx,const int16_t *pcm,unsigned frames)
{
    struct recorder_main *m=ctx;
    if (m->output<0) {
        m->output=open("/dev/audio",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
        if (m->output<0) return -errno;
        struct ciuki_audio_format f={.version=1,.size=sizeof(f),.rate=REC_RATE,.format=CIUKI_AUDIO_S16LE,.channels=2};
        if (ciuki_devctl(m->output,AUDIO_SET_FORMAT,&f,sizeof(f))) {
            int e=-errno; close(m->output); m->output=-1; return e;
        }
    }
    if (!frames) {
        struct ciuki_audio_command c={.version=1,.size=sizeof(c)};
        return ciuki_devctl(m->output,AUDIO_DRAIN,&c,sizeof(c)) ? -errno : 0;
    }
    ssize_t n=write(m->output,pcm,frames*4); return n<0 ? -errno : (int)n/4;
}
static int stop(void *ctx)
{
    struct recorder_main *m=ctx; int error=0;
    if (m->input>=0) {
        struct ciuki_audio_capture c={.version=1,.size=sizeof(c)};
        if (ciuki_devctl(m->input,AUDIO_CAPTURE_STOP,&c,sizeof(c))) error=-errno;
        close(m->input); m->input=-1;
    }
    if (m->output>=0) { close(m->output); m->output=-1; }
    return error;
}
static int closing(struct du_window *w,void *ctx)
{ (void)w; return recorder_close(&((struct recorder_main *)ctx)->app); }
int main(int argc,char **argv)
{
    int channel,test; const char *path;
    if (acc_args(argc,argv,"recorder",&channel,&test,&path)) return 2;
    struct recorder_main *m=calloc(1,sizeof(*m)); if (!m) return 1;
    m->input=m->output=-1;
    if (du_window_open(&m->window,&m->ui,channel,REC_W,REC_H,"Sound Recorder")) { free(m); return 1; }
    struct recorder_backend backend={start,capture,playback,stop,m};
    recorder_attach(&m->app,&m->ui,&m->window,&backend);
    m->window.close_request=closing; m->window.user=m;
    if (path && recorder_open(&m->app,path)) {
        du_message(&m->ui,1700,"Sound Recorder","Cannot open this PCM WAV file.",DU_DLG_ICON_ERROR,NULL,0,0,0);
    }
    du_timer_start(&m->ui,REC_TICK,10000000ull,1);
    if (test) du_window_move(&m->window,24,48);
    int result=du_run(&m->window,NULL,NULL);
    recorder_destroy(&m->app); du_window_destroy(&m->window); free(m);
    return result<0 ? 1 : 0;
}
