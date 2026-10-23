/* SPDX-License-Identifier: MIT
 * Append-only install record. A torn final line is ignored; the next append
 * starts with LF so a crash cannot turn two records into one valid result. */
#include "firstboot.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int fb_accel_outcome(unsigned caps,unsigned quarantined,int marker,char *text,size_t cap)
{
    int on=caps && !quarantined && !marker;
    snprintf(text,cap,on?"Hardware acceleration is on":"Software rendering is in use; you can retry from Control Panel > Display");
    return on?0:1;
}
const char *const fb_names[FB_STEPS]={"accel","sound","network","games","entropy","clock","summary"};
const char *const fb_results[4]={"pending","ok","skipped","failed"};
int fb_begin(struct firstboot *f,const struct fb_host *h)
{
    memset(f,0,sizeof(*f)); f->host=*h;
    char *text=malloc(FB_STATE_BYTES); size_t n=0;
    if(!text)return f->error=-ENOMEM;
    int r=h->read(h->ctx,text,FB_STATE_BYTES,&n);
    if(r==-ENOENT){free(text);return 0;}
    if(r){free(text);return f->error=r;}
    if(n>=FB_STATE_BYTES || memchr(text,0,n)){free(text);return f->error=-EFBIG;}
    int installed=0,pending=0;
    for(size_t at=0;at<n;){
        char *end=memchr(text+at,'\n',n-at);if(!end)break;
        *end=0;char *line=text+at;size_t len=(size_t)(end-line);
        if(len && line[len-1]=='\r')line[--len]=0;
        if(!strcmp(line,"phase=installed"))installed=1;
        else if(!strncmp(line,"phase=",6))installed=0;
        if(!strcmp(line,"firstboot=pending"))pending=1;
        else if(!strncmp(line,"firstboot=",10))pending=0;
        for(unsigned s=0;s<FB_STEPS;s++)for(unsigned k=FB_OK;k<=FB_FAILED;k++){
            char expected[64];snprintf(expected,sizeof(expected),"step=%s result=%s",fb_names[s],fb_results[k]);
            if(!strcmp(line,expected))f->result[s]=(enum fb_result)k;
        }
        at=(size_t)(end-text)+1;
    }
    free(text);
    if(!installed || !pending)return 0;
    f->active=1;
    for(unsigned i=0;i<FB_STEPS;i++)if(f->result[i])snprintf(f->detail[i],sizeof(f->detail[i]),"Recorded on a previous run.");
    while(f->step<FB_STEPS && f->result[f->step])f->step++;
    return 1;
}
static unsigned limit(unsigned s){return s==FB_SOUND||s==FB_NETWORK?10000:s==FB_ENTROPY?5000:1000;}
static int commit(struct firstboot *f,unsigned s,enum fb_result result,uint64_t elapsed)
{
    char line[160];int n=snprintf(line,sizeof(line),"\nstep=%s result=%s\n",fb_names[s],fb_results[result]);
    int r=f->host.append(f->host.ctx,line,(size_t)n);
    if(r){f->error=r;return r;}
    f->result[s]=result;
    snprintf(line,sizeof(line),"[firstboot] step=%s result=%s ms=%llu",fb_names[s],fb_results[result],(unsigned long long)elapsed);
    if(f->host.record)f->host.record(f->host.ctx,line);
    return 0;
}
void fb_tick(struct firstboot *f,uint64_t ms)
{
    if(!f->active || f->done || f->error)return;
    while(f->step<FB_STEPS && f->result[f->step])f->step++;
    if(f->step==FB_STEPS){
        const char line[]="\nfirstboot=done\n";
        int r=f->host.append(f->host.ctx,line,sizeof(line)-1);
        if(r)f->error=r;else {f->done=1;f->active=0;}
        return;
    }
    unsigned s=f->step;
    int start=!f->started;
    if(start){f->began=ms;f->polls=0;f->started=1;}
    if(s==FB_SUMMARY && !f->finish){
        snprintf(f->detail[s],sizeof(f->detail[s]),"Installed on C:. Records: C:\\SYSTEM\\SETUP.LOG and HARDWARE.TXT. Choose Finish to start the desktop.");
        return;
    }
    uint64_t elapsed=ms>=f->began?ms-f->began:0;
    unsigned bound=limit(s);
    /* One tick per 20 ms in production; finite polls also cover a dead clock. */
    int r=(s!=FB_SUMMARY && (elapsed>=bound || f->polls++>=bound/20+1))?-ETIMEDOUT:
        f->host.service(f->host.ctx,s,start,f->detail[s],sizeof(f->detail[s]));
    if(r==-EAGAIN)return;
    if(f->host.cancel)f->host.cancel(f->host.ctx,s);
    if(r==-ETIMEDOUT && s!=FB_ENTROPY)snprintf(f->detail[s],sizeof(f->detail[s]),s==FB_NETWORK?"No network; you can configure it later.":"Check timed out; you can try again from Control Panel.");
    if(r<0 && !f->detail[s][0])snprintf(f->detail[s],sizeof(f->detail[s]),"Check failed (error %d). You can configure this later.",-r);
    enum fb_result result=r==0?FB_OK:r==1 || (s==FB_NETWORK && r==-ETIMEDOUT)?FB_SKIPPED:FB_FAILED;
    if(!commit(f,s,result,elapsed)){f->step++;f->started=0;}
}
void fb_finish(struct firstboot *f){if(f->step==FB_SUMMARY)f->finish=1;}
void fb_action(struct firstboot *f,unsigned action)
{if(f->active && action<=FB_CHANGE_CLOCK && f->host.action)f->host.action(f->host.ctx,action);}
static void button(struct du_canvas *c,int x,int y,int w,const char *label,int focus)
{
    struct du_rect b={x,y,w,30};du_raised(c,b,0,0);du_text(c,x+8,y+7,label,0x182638);
    if(focus)du_focus_ring(c,(struct du_rect){x+3,y+3,w-6,24});
}
void fb_draw(const struct firstboot *f,struct du_canvas *c,const uint32_t *portrait)
{
    int ox=(c->width-640)/2,oy=(c->height-480)/2;
    du_fill(c,(struct du_rect){0,0,c->width,c->height},0x0a3d62);
    du_fill(c,(struct du_rect){ox,oy,640,480},0xf5f7fa);
    du_fill(c,(struct du_rect){ox,oy,164,480},0x1b2a5a);
    if(portrait)for(int y=0;y<132;y++)for(int x=0;x<132;x++){
        uint32_t p=portrait[(y*256/132)*256+x*256/132];
        if(p>>24)du_fill(c,(struct du_rect){ox+16+x,oy+32+y,1,1},p&0xffffff);
    }
    du_text(c,ox+20,oy+185,"CiukiOS",0xffffff);
    du_text_wrap(c,(struct du_rect){ox+16,oy+216,132,90},"Classic computing, one woof at a time.",0xffffff);
    du_text_font(c,DESK_FONT_BOLD,ox+184,oy+26,"Finishing your installation",0x182638);
    for(unsigned s=0;s<FB_STEPS;s++){
        char line[96];snprintf(line,sizeof(line),"%s   %s",fb_names[s],f->result[s]?fb_results[f->result[s]]:s==f->step?"checking...":"waiting");
        du_text(c,ox+188,oy+68+(int)s*28,line,f->result[s]==FB_FAILED?0x9c271d:0x182638);
    }
    unsigned show=f->step<FB_STEPS?f->step:FB_SUMMARY;
    if(show && !f->detail[show][0])show--;
    du_text_wrap(c,(struct du_rect){ox+184,oy+272,432,48},f->detail[FB_ACCEL],0x182638);
    du_text_wrap(c,(struct du_rect){ox+184,oy+322,432,52},f->detail[show],0x182638);
    if(f->error)du_text(c,ox+184,oy+376,"Cannot commit SETUP.STATE. Restart to resume.",0x9c271d);
    
    button(c,ox+184,oy+396,148,"Display settings",f->focus==0);
    button(c,ox+344,oy+396,134,"Change clock",f->focus==1);
    button(c,ox+492,oy+396,116,"Finish",f->focus==2);
    du_text(c,ox+184,oy+448,"Tab: choose   Enter: open   F10: Finish",0x45586b);
}
