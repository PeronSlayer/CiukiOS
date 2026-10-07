/* CiukiOS music player. Decoding is streamed by MEDIAWORK.EXE; playback is
 * sent to the shell's AC'97 queue in 2048-frame slices. */
#include "app.h"
#include "player_audio.h"
#include "mediawork.h"
#define PLAYER_PATH_MAX 260

#define PLAYER_CHUNK_FRAMES 2048
#define PLAYER_RING_FRAMES 16384UL
static char path[PLAYER_PATH_MAX],track[48],status[72],pending_path[PLAYER_PATH_MAX];
static char playlist[32][PLAYER_PATH_MAX];
static int playlist_count,playlist_index,start_after_close;
static struct media_header media_reply;
static struct app_audio_packet audio;
static int16_t pcm[PLAYER_CHUNK_FRAMES*2];
static int worker_pending,worker_op,audio_open,playing,paused,eos,closing;
static int seek_deferred,close_requested;
static u16 poll_repaint_ticks;
static int poll_repaint_pending;
static u32 total_frames,pcm_frames,pcm_offset,played_frames,queued_frames,deferred_seek_frame;
static u16 volume=80;
static u16 log_tick;
static char logged_state[16];

static void set_status(const char *s)
{
    if(str_cmp(status,s))poll_repaint_pending=1;
    str_ncopy(status,s,sizeof status);
}
/* The player ABI stores a u32 frame count. Compute floor(value*100/maximum)
 * without forming the overflowing product: compare against ceil(maximum*p/100)
 * using quotient/remainder, with a seven-step search over p in [0,100]. */
static u32 percent_u32(u32 value,u32 maximum)
{
    u32 q,r,lo=0,hi=100;
    if(!maximum)return 0;
    if(value>=maximum)return 100;
    q=maximum/100UL;r=maximum%100UL;
    while(lo<hi){
        u32 p=(lo+hi+1)/2;
        u32 threshold=q*p+(r*p+99)/100UL;
        if(value>=threshold)lo=p;else hi=p-1;
    }
    return lo;
}
/* num/den is a clamped viewport fraction, so this split keeps both products
 * in range even when value is a full-width frame count. */
static u32 scale_fraction_u32(u32 value,u32 num,u32 den)
{
    u32 q=value/den,r=value%den;
    return q*num+(r*num)/den;
}
static void log_state(const char *s)
{
    if(str_cmp(logged_state,s)){
        str_ncopy(logged_state,s,sizeof logged_state);
        app_log("[PLAYER] state",s);
        log_tick=HOST.ticks;
    }
}
static void log_progress(int force)
{
    char detail[48];
    if(!force&&(u16)(HOST.ticks-log_tick)<18)return;
    log_tick=HOST.ticks;
    str_copy(detail,"played=");fmt_u32(detail+str_len(detail),played_frames);
    str_cat(detail," queued=");fmt_u32(detail+str_len(detail),queued_frames);
    str_cat(detail," total=");fmt_u32(detail+str_len(detail),total_frames);
    app_log("[PLAYER] progress",detail);
}
static void display_name(void)
{
    const char *p=path,*base=path;int n;
    while(*p){if(*p=='\\'||*p=='/')base=p+1;++p;}
    n=str_len(base);if(n>=sizeof track)n=sizeof track-1;mem_copy(track,base,(u16)n);track[n]=0;
}
static int audio_extension(const char *name)
{
    int n=str_len(name);const char *e;
    if(n>=6&&name[n-5]=='.'&&(name[n-4]=='f'||name[n-4]=='F')&&(name[n-3]=='l'||name[n-3]=='L')&&(name[n-2]=='a'||name[n-2]=='A')&&(name[n-1]=='c'||name[n-1]=='C'))return 1;
    if(n<5||name[n-4]!='.')return 0;e=name+n-3;
    if((e[0]=='w'||e[0]=='W')&&(e[1]=='a'||e[1]=='A')&&(e[2]=='v'||e[2]=='V'))return 1;
    if((e[0]=='m'||e[0]=='M')&&(e[1]=='p'||e[1]=='P')&&(e[2]=='3'))return 1;
    if((e[0]=='o'||e[0]=='O')&&(e[1]=='g'||e[1]=='G')&&(e[2]=='g'||e[2]=='G'))return 1;
    return 0;
}
static void playlist_scan(void)
{
    struct dir_ent e;char dir[PLAYER_PATH_MAX],pattern[PLAYER_PATH_MAX];int n,rc;
    playlist_count=playlist_index=0;
    str_ncopy(dir,path,sizeof dir);n=str_len(dir);
    while(n&&dir[n-1]!='\\'&&dir[n-1]!='/')--n;
    if(!n){dir[0]='\\';dir[1]=0;n=1;}else dir[n]=0;
    if(n+3>=sizeof pattern)return;
    str_ncopy(pattern,dir,sizeof pattern);str_cat(pattern,"*.*");
    if(dir_first(pattern,0,&e)<0)return;
    do{
        if(!(e.attr&A_DIR)&&audio_extension(e.name)&&playlist_count<32&&
           str_len(dir)+str_len(e.name)<sizeof playlist[0]){
            str_ncopy(playlist[playlist_count],dir,sizeof playlist[0]);
            str_cat(playlist[playlist_count],e.name);
            if(!str_cmp(playlist[playlist_count],path))playlist_index=playlist_count;
            ++playlist_count;
        }
        rc=dir_next(&e);
    }while(rc==0);
    dir_close(&e);
}
static void audio_packet_init(void)
{
    mem_set(&audio,0,sizeof audio);audio.bytes=sizeof audio;audio.sample_rate=48000;audio.volume=volume;
}
static int call_audio(int op)
{
    audio_packet_init();return app_audio(op,&audio);
}
static void worker_fail(void)
{
    worker_pending=0;seek_deferred=0;set_status("Decoder worker failed.");
    log_state("Error");
    if(audio_open){call_audio(APP_AUDIO_CLOSE);audio_open=0;}
    playing=0;closing=1;start_after_close=0;mediawork_close();
}
static int worker_submit_decode(void)
{
    if(worker_pending||eos)return 1;
    if(!mediawork_decode()){worker_fail();return 0;}
    worker_pending=1;worker_op=MEDIA_DECODE;return 1;
}
static void poll_worker(void)
{
    int rc;
    if(!worker_pending)return;
    rc=mediawork_poll(&media_reply);
    if(!rc)return;
    worker_pending=0;
    if(rc<0){worker_fail();return;}
    if(seek_deferred){
        u32 target=deferred_seek_frame;
        seek_deferred=0;
        if(!mediawork_seek(target)){worker_fail();return;}
        worker_pending=1;worker_op=MEDIA_SEEK;return;
    }
    if(worker_op==MEDIA_OPEN){
        total_frames=media_reply.total_frames;
        audio_packet_init();audio.total_frames=total_frames;
        if(app_audio(APP_AUDIO_OPEN,&audio)!=APP_AUDIO_STATUS_OK){worker_fail();return;}
        audio_open=1;playing=1;paused=0;set_status("Playing");log_state("Playing");log_progress(1);worker_submit_decode();
    }else if(worker_op==MEDIA_DECODE){
        pcm_frames=media_reply.output_frames;pcm_offset=0;
        if(!pcm_frames){eos=1;set_status("End of track");}
    }else if(worker_op==MEDIA_SEEK){
        pcm_frames=pcm_offset=0;eos=0;set_status(paused?"Paused":"Playing");worker_submit_decode();
    }
}
static void poll_audio(void)
{
    if(!audio_open)return;
    if(call_audio(APP_AUDIO_POLL)!=APP_AUDIO_STATUS_OK){worker_fail();return;}
    played_frames=audio.played_frames;queued_frames=audio.queued_frames;
    log_progress(0);
    if((audio.flags&APP_AUDIO_F_EOS)&&!queued_frames)eos=1;
}
static void pump_pcm(void)
{
    u32 remain,free_frames,n;u16 bytes;
    if(!audio_open||paused||!pcm_frames)return;
    free_frames=queued_frames>=PLAYER_RING_FRAMES?0:PLAYER_RING_FRAMES-queued_frames;
    if(!free_frames)return;
    remain=pcm_frames-pcm_offset;n=remain;
    if(n>PLAYER_CHUNK_FRAMES)n=PLAYER_CHUNK_FRAMES;
    if(n>free_frames)n=free_frames;
    if(!n)return;
    bytes=(u16)(n*4);
    if(!mediawork_read(pcm_offset*4UL,pcm,bytes)){worker_fail();return;}
    audio_packet_init();audio.data_seg=app_seg();audio.data_off=(u16)pcm;audio.frames=(u16)n;
    { int rc=app_audio(APP_AUDIO_WRITE,&audio);
      if(rc==5||audio.status==5)return;
      if(rc!=0){worker_fail();return;}
      if(audio.frames){pcm_offset+=audio.frames;queued_frames+=audio.frames;}
    }
    if(pcm_offset>=pcm_frames){pcm_frames=pcm_offset=0;worker_submit_decode();}
}
static void poll(void)
{
    poll_worker();poll_audio();pump_pcm();
    if(eos&&audio_open){
        audio_packet_init();audio.flags=APP_AUDIO_F_EOS;
        if(app_audio(APP_AUDIO_POLL,&audio)!=APP_AUDIO_STATUS_OK){worker_fail();return;}
        if(!audio.queued_frames){call_audio(APP_AUDIO_CLOSE);audio_open=0;playing=0;closing=1;set_status("End of track");log_state("End");log_progress(1);mediawork_close();}
    }
}
static void start_track(const char *name)
{
    if(audio_open){call_audio(APP_AUDIO_CLOSE);audio_open=0;}
    if(worker_pending||closing){set_status("Please wait for the current operation.");return;}
    str_ncopy(path,name,sizeof path);display_name();playlist_scan();worker_pending=worker_op=0;seek_deferred=0;
    app_log("[PLAYER] open path",path);
    pcm_frames=pcm_offset=played_frames=queued_frames=total_frames=0;eos=0;
    if(!mediawork_start(path)){set_status("Could not start the decoder.");log_state("Error");return;}
    worker_pending=1;worker_op=MEDIA_OPEN;set_status("Opening audio file...");
}
static void seek_to(u32 frame)
{
    if(!total_frames||!audio_open)return;
    if(frame>total_frames)frame=total_frames;
    if(worker_pending){
        /* The decoder mailbox permits one command at a time. Flush audio now,
         * remember the newest target, then seek as soon as its reply arrives. */
        deferred_seek_frame=frame;seek_deferred=1;
        audio_packet_init();audio.played_frames=frame;
        audio.flags=paused?APP_AUDIO_F_PAUSED:0;
        if(app_audio(APP_AUDIO_SEEK,&audio)!=APP_AUDIO_STATUS_OK){worker_fail();return;}
        pcm_frames=pcm_offset=0;eos=0;played_frames=frame;queued_frames=0;
        set_status("Seeking...");
        {char detail[32];str_copy(detail,"frame=");fmt_u32(detail+str_len(detail),frame);app_log("[PLAYER] seek",detail);log_progress(1);}
        return;
    }
    audio_packet_init();audio.played_frames=frame; /* SEEK input: target output frame. */
    audio.flags=paused?APP_AUDIO_F_PAUSED:0;
    if(!mediawork_seek(frame)){worker_fail();return;}
    worker_pending=1;worker_op=MEDIA_SEEK;
    if(app_audio(APP_AUDIO_SEEK,&audio)!=APP_AUDIO_STATUS_OK){worker_fail();return;}
    pcm_frames=pcm_offset=0;eos=0;
    played_frames=frame;queued_frames=0;
    {char detail[32];str_copy(detail,"frame=");fmt_u32(detail+str_len(detail),frame);app_log("[PLAYER] seek",detail);log_progress(1);}
}
static void paint(void)
{
    int x=HOST.x+4,y=HOST.y+TITLE_H,w=HOST.w-8,h=HOST.h-TITLE_H-4;
    u32 pct=percent_u32(played_frames,total_frames);
    ui_rect(x,y,w,h,C_FACE);
    ui_bevel(x+12,y+12,w-24,92,C_TITLE);
    ui_text(x+26,y+25,track[0]?track:"No track selected",C_PAPER|BOLD);
    ui_text(x+26,y+49,status,C_PAPER);
    if(total_frames){char time[32];u32 sec=played_frames/48000UL,total=total_frames/48000UL;
        str_copy(time,"Elapsed ");fmt_u32(time+str_len(time),sec/60);str_cat(time,":");fmt_u32(time+str_len(time),sec%60);
        str_cat(time," / ");fmt_u32(time+str_len(time),total/60);str_cat(time,":");fmt_u32(time+str_len(time),total%60);
        ui_text(x+26,y+72,time,C_PAPER);
    }
    ui_button(x+18,y+122,58,28,"Prev",7);
    ui_button(x+80,y+122,84,28,paused?"Play":"Pause",1);
    ui_button(x+168,y+122,58,28,"Stop",2);
    ui_button(x+230,y+122,58,28,"Next",8);
    ui_button(x+294,y+122,64,28,"Files...",3);
    ui_button(x+w-116,y+122,42,28,"- Vol",4);
    ui_button(x+w-70,y+122,60,28,"+ Vol",5);
    ui_inset(x+18,y+166,w-36,13);ui_rect(x+20,y+168,(int)((w-40)*pct/100),9,C_GREEN);
    ui_hit(x+18,y+164,w-36,17,6);
    ui_text(x+18,y+192,"WAV  MP3  OGG Vorbis  FLAC",C_INK);
    ui_text(x+18,y+210,"Click the timeline to seek; volume is 0-100%.",C_SHADOW);
}
int app_event(int ev,int a,int b,int c)
{
    if(ev==EV_OPEN){close_requested=0;str_copy(app_title,"CiukiOS Music Player");HDR_WIDTH=600;HDR_HEIGHT=300;mem_set(path,0,sizeof path);mem_set(track,0,sizeof track);set_status("Open an audio file from Files.");
        if(APP_ARG[0])start_track(APP_ARG);return 0;}
    if(ev==EV_PAINT){paint();return 0;}
    if(ev==EV_POLL){
        /* A stopped worker is expected during teardown, not a decode error. */
        if(!closing)poll();
        if(closing&&mediawork_close()){
            closing=0;
            if(close_requested){
                close_requested=0;
                app_window_cmd(WIN_PLAYER,2);
                return 0;
            }
            if(start_after_close){char next[PLAYER_PATH_MAX];str_ncopy(next,pending_path,sizeof next);start_after_close=0;start_track(next);}
        }
        if(poll_repaint_pending){poll_repaint_pending=0;return 1;}
        if(++poll_repaint_ticks>=3){poll_repaint_ticks=0;if(audio_open||worker_pending||closing)return 1;}
        return 0;
    }
    if(ev==EV_ACTION){
        if(a==1&&audio_open){audio_packet_init();audio.flags=paused?0:APP_AUDIO_F_PAUSED;if(app_audio(APP_AUDIO_PAUSE,&audio)==APP_AUDIO_STATUS_OK){paused=!paused;set_status(paused?"Paused":"Playing");log_state(paused?"Paused":"Playing");log_progress(1);}}
        else if(a==2){if(audio_open){call_audio(APP_AUDIO_CLOSE);audio_open=0;}closing=1;mediawork_close();playing=0;eos=0;worker_pending=0;seek_deferred=0;set_status("Stopped");log_state("Stopped");log_progress(1);}
        else if(a==3)app_open(WIN_FILES,path);
        else if(a==4||a==5){if(a==4&&volume>=10)volume-=10;else if(a==5&&volume<=90)volume+=10;audio_packet_init();audio.volume=volume;if(audio_open&&app_audio(APP_AUDIO_VOLUME,&audio)!=APP_AUDIO_STATUS_OK)set_status("Volume update failed.");{char detail[16];str_copy(detail,"level=");fmt_u32(detail+str_len(detail),volume);app_log("[PLAYER] volume",detail);}}
        else if(a==6&&total_frames){int px=HOST.mx-(HOST.x+22),width=HOST.w-52;u32 target;if(width<=0)return 0;if(px<0)px=0;if(px>width)px=width;target=scale_fraction_u32(total_frames,(u32)px,(u32)width);seek_to(target);}
        else if((a==7||a==8)&&playlist_count>1){playlist_index=(playlist_index+(a==7?playlist_count-1:1))%playlist_count;str_ncopy(pending_path,playlist[playlist_index],sizeof pending_path);start_after_close=1;if(audio_open){call_audio(APP_AUDIO_CLOSE);audio_open=0;}closing=1;mediawork_close();}
        ui_repaint();return 0;
    }
    if(ev==EV_CLOSE){if(audio_open){call_audio(APP_AUDIO_CLOSE);audio_open=0;}closing=1;playing=0;start_after_close=0;seek_deferred=0;if(!mediawork_close()){close_requested=1;return 1;}closing=close_requested=0;return 0;}
    if(ev==EV_SUSPEND){if(audio_open){audio_packet_init();audio.flags=APP_AUDIO_F_PAUSED;app_audio(APP_AUDIO_PAUSE,&audio);paused=1;}return 1;}
    (void)b;(void)c;return 0;
}
