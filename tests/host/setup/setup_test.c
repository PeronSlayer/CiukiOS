/* SPDX-License-Identifier: MIT */
#include "../../../apps/setup/setup.h"
#include "../../../sdk/libsetup/fake/fake.h"
#include "../../../apps/hwreport/textframe.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void native_tests(const uint32_t *portrait);
void link_tests(void);
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
static unsigned fail_allocation, allocation;
void *__wrap_malloc(size_t n) { if(fail_allocation && ++allocation==fail_allocation)return NULL;return __real_malloc(n); }
void *__wrap_calloc(size_t n,size_t s) { if(fail_allocation && ++allocation==fail_allocation)return NULL;return __real_calloc(n,s); }
static struct st_app a;
static struct du_ui ui;
static uint32_t pixels[ST_W*ST_H], portrait[256*256];
static const char *folder;
static const char valid[]="[Setup]\nVersion=1\nAcceptLicences=yes\nEraseDisk=yes\nDisk=auto\nReinstall=no\nComponents=dos,windows,netsurf,games,devtools,soundfont,sources\nUserName=Ciuki\nComputerName=CIUKI-PC\nKeyboard=us\nTimeZone=+01:00\nClock=local\nNetwork=dhcp\nIPX=yes\nRestart=wait\n";
struct fixture {
    struct st_disk disks[4]; int disks_n;
    struct st_volume volumes[4]; int volumes_n;
    const char *cd_inf, *usb_inf;
    unsigned reads, writes, enumerations, dialogs, frames;
    int refusal, no_nic, media, reboot, eject, cancel_on_copy;
    /* ACPI power button: kernel request count, power-off calls/failure and
     * how the scripted user answers Setup's power dialog. */
    uint32_t power_requests; int power_off, power_fail, power_on_copy, power_confirm, power_moved, hold_power, hold_dialogs;
    unsigned power_dialogs, copy_power_dialogs, power_kept; int no_error_frame;
    uint64_t now;
    /* report_app fake: result and reason to return, the text it produces, calls. */
    int report_result, no_report_hook; unsigned report_calls; const char *report_reason, *report_text;
    char records[8192], written[1024], last_path[128], first_path[128];
} f;
static void tap(unsigned code){st_key(&a,code,1);st_key(&a,code,0);}
static uint64_t mono(void *c){return ((struct fixture *)c)->now;}
static int64_t rtc(void *c){(void)c;return 1792713600;}
static int mem(void *c,uint64_t *k){(void)c;*k=128*1024;return 0;}
static int fb(void *c,unsigned *w,unsigned *h,unsigned *b){(void)c;*w=640;*h=480;*b=32;return f.refusal==ST_STOP_VIDEO ? -1:0;}
static int dev(void *c,struct ciuki_devices_info *d){(void)c;strcpy(d->cpu_brand,"Generic i686 CPU");d->count=1;d->devices[0].class_id=CIUKI_DEVICE_NETWORK;d->devices[0].state=f.no_nic?CIUKI_DEVICE_ABSENT:CIUKI_DEVICE_READY;strcpy(d->devices[0].name,"Generic PCI Ethernet");return 0;}
static int display(void *c,struct ciuki_display_info *d){(void)c;d->width=640;d->height=480;d->bpp=32;d->vbe_version=0x200;strcpy(d->product,"Generic software display");return 0;}
static int disks(void *c,struct st_disk *out,unsigned cap){(void)c;assert(cap>=4);f.enumerations++;memcpy(out,f.disks,sizeof(f.disks));return f.disks_n;}
static int volumes(void *c,struct st_volume *out,unsigned cap){(void)c;assert(cap>=4);memcpy(out,f.volumes,sizeof(f.volumes));return f.volumes_n;}
static int cd(void *c,struct st_cd *out){(void)c;*out=(struct st_cd){.present=1,.ready=f.refusal!=ST_STOP_NO_CD};strcpy(out->model,"Generic ATAPI CD");return 0;}
static int sizes(void *c,struct st_size *out){(void)c;for(unsigned i=0;i<SETUP_COMP_COUNT;i++)out[i]=(struct st_size){.known=1,.bytes=1024*1024*(i+1),.files=10*(i+1)};return 0;}
static int release(void *c,char *out,size_t cap){(void)c;snprintf(out,cap,"%064d",1);return 0;}
static int read_file(void *c,const char *path,char *out,size_t cap,size_t *length){
    (void)c;f.reads++;const char *text=NULL;
    if(!strcmp(path,"D:\\SETUP.INF"))text=f.cd_inf;
    if(!strcmp(path,"F:\\SETUP.INF"))text=f.usb_inf;
    if(!strcmp(path,"C:\\system\\licenses\\GPL.TXT"))text="GNU General Public License version 2\nOrigin: Free Software Foundation\n";
    if(!strcmp(path,"D:\\LICENSES\\INDEX.TXT"))text="L0001.TXT  aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  123  payload:/system/licenses/GPL.TXT\n";
    if(!strcmp(path,"C:\\SYSTEM\\BUILD.TXT"))text="schema=1\nsource_commit=aaaa\nbuild_date=2026-10-23T00:00:00Z\n";
    if(!strcmp(path,"D:\\LICENSES\\SOURCES.TXT"))text="Corresponding source archives: \\SOURCES\\CIUKIOS.ZIP\n";
    if(!text)return -ENOENT;
    *length=strlen(text);if(*length>cap)return -EFBIG;memcpy(out,text,*length);return 0;
}
static int write_file(void *c,const char *path,const char *text,size_t length){
    (void)c;assert(path[0]=='F' && path[1]==':');f.writes++;
    snprintf(f.last_path,sizeof(f.last_path),"%s",path);if(f.writes==1)snprintf(f.first_path,sizeof(f.first_path),"%s",path);
    if(length>=sizeof(f.written))length=sizeof(f.written)-1;memcpy(f.written,text,length);f.written[length]=0;return 0;
}
static int make_dir(void *c,const char *path){(void)c;assert(!strncmp(path,"F:\\CIUKIOS-RESCUE-",18));return 0;}
static int list(void *c,const char *path,char (*out)[64],unsigned cap){(void)c;(void)path;assert(cap);strcpy(out[0],"GPL.TXT");return 1;}
static int report_app(void *c,char *buffer,size_t cap,size_t *length,char reason[24]){
    (void)c;f.report_calls++;assert(cap>=HR_LINK_MAX);
    if(f.report_result){snprintf(reason,24,"%s",f.report_reason);return f.report_result;}
    size_t n=strlen(f.report_text);memcpy(buffer,f.report_text,n);*length=n;return 0;
}
static void record(void *c,const char *line){(void)c;size_t k=strlen(f.records);snprintf(f.records+k,sizeof(f.records)-k,"%s\n",line);}
static int eject(void *c){(void)c;f.eject++;return 0;}
static int media(void *c){(void)c;return f.media;}
static int reboot(void *c){(void)c;f.reboot++;return 0;}
static int power_off(void *c){(void)c;f.power_off++;return f.power_fail?-EIO:0;}
static int power_status(void *c,struct ciuki_power_status *s){
    (void)c;assert(s->version==CIUKI_POWER_VERSION&&s->size==sizeof(*s));
    s->provider=CIUKI_POWER_PROVIDER_ACPI;s->shutdown_requests=f.power_requests;return 0;
}
static char letter(void *c){(void)c;return 'E';}
static void frame(const char *name){
    st_render(&a);char path[1024];snprintf(path,sizeof(path),"%s/%s.frame",folder,name);
    FILE *fp=fopen(path,"wb");assert(fp);assert(fwrite(pixels,4,ST_W*ST_H,fp)==ST_W*ST_H);assert(!fclose(fp));
}
static void present(void *c,const uint32_t *p,int w,int h){(void)c;(void)p;assert(w==ST_W&&h==ST_H);f.frames++;}
/* Dialog input is delivered through the ordinary toolkit route. Every
 * retry dialog is captured and checked before sending its default key. */
static int input(void *c,struct ciuki_input_event *out,unsigned cap){
    (void)c;assert(cap>=1);f.now+=60000000;
    if(ui.dialog.kind){
        if(f.hold_dialogs)return 0;
        if(ui.dialog.id==ST_DLG_POWER){
            f.power_dialogs++;
            if(f.hold_power)return 0;
            if(a.executing){
                assert(strstr(ui.dialog.text,"installation is incomplete")&&strstr(ui.dialog.text,"Exit Setup and power off?"));
                if(!f.copy_power_dialogs++)frame("power-during-copy");
                /* The warning defaults to continuing: confirming takes a move. */
                if(f.power_confirm&&!f.power_moved){f.power_moved=1;*out=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=DU_KEY_LEFT,.value=1};return 1;}
            }
            *out=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=DU_KEY_ENTER,.value=1};return 1;
        }
        if(ui.dialog.id==ST_DLG_ERROR){
            f.dialogs++;if(a.power_pending)f.power_kept++;
            if(a.error.attempt){assert(strstr(ui.dialog.text,"Attempt"));assert(ui.dialog.count==(a.error.retryable && a.error.attempt<5?2u:1u));}
            if(!f.no_error_frame)frame(ui.dialog.count==2?"error-retry":"error-cancel");
        }
        *out=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=DU_KEY_ENTER,.value=1};return 1;
    }
    if(f.power_on_copy && a.page==ST_PAGE_COPY && a.progress.phase==SETUP_PHASE_COPY){f.power_on_copy=0;f.power_requests++;return 0;}
    if(f.cancel_on_copy && a.page==ST_PAGE_COPY && a.progress.phase==SETUP_PHASE_COPY){
        f.cancel_on_copy=0;st_action(&a,ST_ID_CANCEL);
        /* Cancellation defaults to Continue: move to Cancel first. */
        *out=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=DU_KEY_LEFT,.value=1};return 1;
    }
    return 0;
}
static void idle(void *c,uint64_t ns){(void)c;f.now+=ns;}
static void entered(void *c){(void)c;assert(a.page==ST_PAGE_COPY&&a.execute_calls==1&&a.plan_ok);assert(setup_fake.prepare_calls==1);}
static void fixture(void){
    memset(&f,0,sizeof(f));setup_fake_reset();f.now=1000000000;f.media=1;
    f.disks_n=1;f.disks[0]=(struct st_disk){.present=1,.sectors=16ull*1024*1024,.sector_size=512,.installable=1,.part_count=2};
    strcpy(f.disks[0].model,"GENERIC ATA");strcpy(f.disks[0].serial,"DISK-0001");
    f.disks[0].parts[0]=(struct st_part){.type=0x0c,.start=2048,.count=4*1024*1024,.letter='E'};strcpy(f.disks[0].parts[0].label,"OLD-DATA");
    f.disks[0].parts[1]=(struct st_part){.type=0x07,.start=4196352,.count=4*1024*1024,.letter='G'};strcpy(f.disks[0].parts[1].label,"ARCHIVE");
    f.volumes_n=2;f.volumes[0]=(struct st_volume){.letter='G',.usb=0,.writable=0};strcpy(f.volumes[0].fs,"NTFS");
    f.volumes[1]=(struct st_volume){.letter='F',.usb=1,.writable=1};strcpy(f.volumes[1].fs,"FAT32");
    setup_fake.plan.disk.sectors=f.disks[0].sectors;strcpy(setup_fake.plan.disk.model,f.disks[0].model);strcpy(setup_fake.plan.disk.serial,f.disks[0].serial);
    memset(setup_fake.plan.disk.table_fingerprint,0xab,32);setup_fake.plan.erased_partitions=2;setup_fake.plan.erased_total=2;
    for(unsigned i=0;i<2;i++){setup_fake.plan.erased[i].type=f.disks[0].parts[i].type;setup_fake.plan.erased[i].start=f.disks[0].parts[i].start;setup_fake.plan.erased[i].count=f.disks[0].parts[i].count;strcpy(setup_fake.plan.erased[i].label,f.disks[0].parts[i].label);}
    setup_fake.plan.partition_start=2048;setup_fake.plan.partition_count=f.disks[0].sectors-2048;setup_fake.plan.required_bytes=101*1024*1024;setup_fake.plan.payload_bytes=36*1024*1024;
    setup_fake.plan.overhead_bytes=1024*1024;setup_fake.plan.reserve_bytes=64*1024*1024;setup_fake.plan.free_after_bytes=7ull*1024*1024*1024;
    memset(setup_fake.plan.source_image_sha,'1',64);memset(setup_fake.plan.source_iso_sha,'2',64);
    setup_fake.entered=entered;
}
static void init(void){
    struct st_host h={.ctx=&f,.monotonic=mono,.realtime=rtc,.input=input,.idle=idle,.present=present,.devices=dev,.memory=mem,.display=display,.framebuffer=fb,.disks=disks,.volumes=volumes,.cd=cd,.catalogue=sizes,.release=release,.read_file=read_file,.write_file=write_file,.make_dir=make_dir,.list_dir=list,.report_app=f.no_report_hook?NULL:report_app,.record=record,.eject=eject,.media_present=media,.reboot=reboot,.power_off=power_off,.power_status=power_status,.dest_letter=letter,.portrait=portrait};
    du_init(&ui,pixels,ST_W,ST_H);ui.focused_window=1;assert(!st_init(&a,&ui,&h));
}
static void no_writes(void){assert(!setup_fake.execute_calls&&!setup_fake.mutations&&!f.writes);}
static void focus(int id){for(unsigned i=0;i<ui.count+1;i++){if(ui.focus>=0&&ui.widgets[ui.focus].id==id)return;tap(DU_KEY_TAB);}assert(!"missing keyboard focus");}
static void type(const char *s){
    static const char keys[]="qwertyuiop";static const char keys2[]="asdfghjkl";static const char keys3[]="zxcvbnm";
    for(;*s;s++){const char *p=strchr(keys,*s);unsigned k=p?0x10+(unsigned)(p-keys):0;
        if(!k&&(p=strchr(keys2,*s)))k=0x1e + (unsigned)(p-keys2);
        if(!k&&(p=strchr(keys3,*s)))k=0x2c+(unsigned)(p-keys3);assert(k);tap(k);}
}
static void summary(void){
    tap(DU_KEY_F10);assert(a.page==ST_PAGE_LICENCE);no_writes();
    focus(ST_ID_ACCEPT);tap(DU_KEY_SPACE);tap(DU_KEY_F10);assert(a.page==ST_PAGE_HARDWARE);no_writes();
    tap(DU_KEY_F10);assert(a.page==ST_PAGE_DISK);frame("disk-two-partitions");
    assert(strstr(a.pool,"GENERIC ATA"));
    struct ciuki_input_event mouse={.type=CIUKI_INPUT_MOTION,.value=a.view.box.x+a.view.box.w-8,.value2=a.view.box.y+a.view.box.h-8};st_input_event(&a,&mouse);
    mouse=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};st_input_event(&a,&mouse);mouse.value=0;st_input_event(&a,&mouse);assert(a.view.scroll>0);
    mouse=(struct ciuki_input_event){.type=CIUKI_INPUT_WHEEL,.value=-1};int wheel_before=a.view.scroll;st_input_event(&a,&mouse);assert(a.view.scroll>wheel_before);tap(DU_KEY_HOME);
    int old_scroll=a.view.scroll;tap(DU_KEY_END);assert(a.view.scroll>old_scroll);frame("disk-warning-scrolled");tap(DU_KEY_HOME);
    assert(du_get(&ui,ST_ID_FREE)->disabled&&du_get(&ui,ST_ID_ADVANCED)->disabled);no_writes();
    tap(DU_KEY_F10);assert(a.page==ST_PAGE_COMPONENTS);tap(DU_KEY_F10);assert(a.page==ST_PAGE_USER);
    type("ciuki");tap(DU_KEY_TAB);type("ciukipc");tap(DU_KEY_F10);assert(a.page==ST_PAGE_NETWORK);no_writes();
    tap(DU_KEY_F10);assert(a.page==ST_PAGE_SUMMARY&&a.plan_ok);assert(setup_fake.prepare_calls==1);no_writes();frame("summary");
}
static void parser(void){
    struct setup_answers ans;struct setup_inf_status st;
    assert(!setup_inf_parse(valid,strlen(valid),&ans,&st));assert(!setup_inf_complete(&ans,&st));assert(ans.components==SETUP_COMP_ALL);
    static const struct {const char *text,*reason,*key;} bad[]={
        {"[Other]\nVersion=1\n",SETUP_ST_SECTION,"Other"},{"[Setup]\nVersion=2\n",SETUP_ST_VERSION,"Version"},
        {"[Setup]\nVersion=1\nBogus=x\n",SETUP_ST_UNKNOWN_KEY,"Bogus"},{"[Setup]\nVersion=1\nversion=1\n",SETUP_ST_DUPLICATE,"Version"},
        {"[Setup]\n",SETUP_ST_MISSING,"Version"},{"[Setup]\nVersion=1\nKeyboard=zz\n",SETUP_ST_VALUE,"Keyboard"},
        {"[Setup]\nVersion=1\nComponents=games\n",SETUP_ST_DEPENDENCY,"Components"},
        {"[Setup]\nVersion=1\nRestart=maybe\n",SETUP_ST_VALUE,"Restart"},
        {"[Setup]\nVersion=1\nTimeZone=+15:00\n",SETUP_ST_VALUE,"TimeZone"},
        {"[Setup]\nVersion=1\nNetwork=static:192.168.0.0/24,,\n",SETUP_ST_VALUE,"Network"},
        {"[Setup]\nVersion=1\nComputerName=1234\n",SETUP_ST_VALUE,"ComputerName"},
        {"[Setup]\nVersion=1\nUserName=\n",SETUP_ST_VALUE,"UserName"},
        {"[Setup]\nVersion=1\nAcceptLicences=no\n",SETUP_ST_VALUE,"AcceptLicences"},
        {"[Setup]\nVersion=1\n[Setup]\n",SETUP_ST_DUPLICATE,"[Setup]"}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){assert(setup_inf_parse(bad[i].text,strlen(bad[i].text),&ans,&st)<0);assert(!strcmp(st.reason,bad[i].reason)&&!strcmp(st.key,bad[i].key));}
    assert(!setup_inf_parse("[setup] ; comment\nversion=1\n",strlen("[setup] ; comment\nversion=1\n"),&ans,&st));assert(setup_inf_complete(&ans,&st)<0&&!strcmp(st.key,"AcceptLicences"));
    struct setup_inf_disk d[4]={{.present=1,.occupied=1}};strcpy(d[0].model,"GENERIC ATA");strcpy(d[0].serial,"DISK-0001");
    assert(!setup_inf_parse(valid,strlen(valid),&ans,&st));assert(!setup_inf_disk_rules(&ans,d,"sha",&st));
    d[1].present=1;assert(setup_inf_disk_rules(&ans,d,"sha",&st)<0&&!strcmp(st.reason,SETUP_ST_AMBIGUOUS));d[1].present=0;
    ans.disk=SETUP_DISK_MATCH;strcpy(ans.disk_model,"other");assert(setup_inf_disk_rules(&ans,d,"sha",&st)<0&&!strcmp(st.reason,SETUP_ST_NOT_FOUND));
    strcpy(ans.disk_model,d[0].model);strcpy(ans.disk_serial,"wrong");assert(setup_inf_disk_rules(&ans,d,"sha",&st)<0);strcpy(ans.disk_serial,d[0].serial);assert(!setup_inf_disk_rules(&ans,d,"sha",&st));
    ans.erase_disk=0;assert(setup_inf_disk_rules(&ans,d,"sha",&st)<0&&!strcmp(st.reason,SETUP_ST_CONSENT));ans.erase_disk=1;
    d[0].installed=1;strcpy(d[0].release,"sha");assert(setup_inf_disk_rules(&ans,d,"sha",&st)<0&&!strcmp(st.reason,SETUP_ST_INSTALLED));ans.reinstall=1;assert(!setup_inf_disk_rules(&ans,d,"sha",&st));
    const char *const policies[]={"eject","wait","force"};for(unsigned i=0;i<3;i++){char text[100];snprintf(text,sizeof(text),"[Setup]\nVersion=1\nRestart=%s\n",policies[i]);assert(!setup_inf_parse(text,strlen(text),&ans,&st));assert((unsigned)ans.restart==i);}
    assert(setup_valid_user("Ciuki")&&!setup_valid_user(" Ciuki")&&!setup_valid_computer("-PC")&&!setup_valid_computer("123"));
    uint8_t ip[4],gw[4]={192,168,1,1},dns[4]={1,1,1,1};assert(!setup_parse_ipv4("192.168.1.2",ip)&&setup_static_valid(ip,24,gw,dns));assert(setup_parse_ipv4("192.168.01.2",ip)<0);
    char ring[SETUP_LOG_BYTES+1];struct setup_log log={0};memset(ring,'x',sizeof(ring)-1);ring[sizeof(ring)-1]=0;setup_log_append(&log,ring);assert(log.dropped==1&&log.length==SETUP_LOG_BYTES);assert(setup_log_copy(&log,ring,sizeof(ring))==SETUP_LOG_BYTES);
    const char *const required[]={"AcceptLicences","Disk","Components","UserName","ComputerName","Keyboard","TimeZone","Network","IPX"};
    for(unsigned i=0;i<sizeof(required)/sizeof(required[0]);i++) {
        char input_text[2048],key[64];snprintf(input_text,sizeof(input_text),"%s",valid);snprintf(key,sizeof(key),"\n%s=",required[i]);
        char *start=strstr(input_text,key);if(start)start++;char *end=start?strchr(start,'\n'):NULL;assert(start&&end);memmove(start,end+1,strlen(end+1)+1);
        assert(!setup_inf_parse(input_text,strlen(input_text),&ans,&st));assert(setup_inf_complete(&ans,&st)<0&&!strcmp(st.reason,SETUP_ST_MISSING)&&!strcmp(st.key,required[i]));
    }
    char oversized[SETUP_INF_MAX_BYTES+1];memset(oversized,'x',sizeof(oversized));assert(setup_inf_parse(oversized,sizeof(oversized),&ans,&st)<0&&!strcmp(st.reason,SETUP_ST_TOO_LARGE));
    assert(st_text_wrap(NULL,DESK_FONT_UI,(struct du_rect){0,0,30,0},15,"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",0)>15);
    puts("PASS strict INF, disk rules, validation, log ring");
}
static void renders(void){
    fixture();init();strcpy(a.answers.user_name,"Ciuki");strcpy(a.answers.computer_name,"CIUKI-PC");
    for(int p=0;p<ST_PAGES;p++){
        if(p==ST_PAGE_SUMMARY){a.plan=setup_fake.plan;a.plan.answers=a.answers;a.plan.components=a.answers.components;}
        if(p==ST_PAGE_COPY){a.progress=(struct setup_progress){.phase=SETUP_PHASE_COPY,.bytes_done=4*1024*1024,.bytes_total=8*1024*1024,.files_done=64,.files_total=128,.elapsed_ms=2000,.eta_ms=2000,.throughput_kbps=2048};strcpy(a.step,"SYSTEM/test");}
        if(p==ST_PAGE_RESTART){a.dest_letter='E';a.progress.files_total=128;}
        if(p==ST_PAGE_STATUS){strcpy(a.answers.source,"F:\\SETUP.INF");strcpy(a.status.reason,SETUP_ST_MISSING);strcpy(a.status.key,"UserName");}
        if(p==ST_PAGE_FAILED){a.failed_class=SETUP_ERR_DEST_FLUSH;a.error=(struct setup_error){.cls=SETUP_ERR_DEST_FLUSH,.phase=SETUP_PHASE_COPY,.attempt=1};a.error_valid=1;}
        if(p==ST_PAGE_TEXT)assert(!st_text_load(&a,"Hardware report",NULL,a.report,a.report_length));
        st_goto(&a,(enum st_page)p);frame(st_page_name(a.page));
        int bottom=st_page_height(&a,a.page);assert(bottom<=390);
        for(unsigned i=0;i<ui.count;i++){struct du_rect b=ui.widgets[i].box;assert(b.x>=0&&b.y>=0&&b.x+b.w<=ST_W&&b.y+b.h<=ST_H);}
        tap(DU_KEY_F1);assert(ui.dialog.kind);tap(DU_KEY_ESC);assert(!ui.dialog.kind);
        if(p==ST_PAGE_COPY)a.executing=1;
        tap(0x3d);assert(ui.dialog.kind);tap(DU_KEY_ESC);assert(!ui.dialog.kind);
        a.executing=0;
        tap(DU_KEY_ESC);if(ui.dialog.kind)tap(DU_KEY_ESC);
        assert(!setup_fake.execute_calls);
    }
    st_goto(&a,ST_PAGE_LICENCE);du_get(&ui,ST_ID_ACCEPT)->value=1;a.answers.accept_licences=1;st_page_build(&a);frame("licence-accepted");
    a.answers.components=SETUP_COMP_SYSTEM;st_goto(&a,ST_PAGE_COMPONENTS);frame("components-disabled");
    st_goto(&a,ST_PAGE_NETWORK);a.net_manual=1;st_page_build(&a);frame("network-manual");
    f.no_nic=1;st_inventory(&a);st_goto(&a,ST_PAGE_NETWORK);frame("network-none");
    st_goto(&a,ST_PAGE_DISK);
    a.disks[0].part_count=16;
    for(unsigned i=0;i<16;i++){a.disks[0].parts[i]=(struct st_part){.type=12,.start=2048+4096*i,.count=4096};snprintf(a.disks[0].parts[i].label,12,"VOLUME%02u",i);}
    st_page_build(&a);assert(a.view.count>=18);tap(DU_KEY_END);frame("disk-sixteen-partitions");
    assert(strstr(a.view.lines[a.view.count-2].text,"VOLUME15"));
    st_free(&a);
}
static void interactive(void){
    fixture();init();summary();
    setup_fake_progress(SETUP_PHASE_PARTITION,"Partition",1,1,100);setup_fake_progress(SETUP_PHASE_COPY,"SYSTEM/test",4096,8192,1000);setup_fake_progress(SETUP_PHASE_COMPLETE,"Complete",8192,8192,2000);
    tap(DU_KEY_F10);assert(a.page==ST_PAGE_RESTART&&setup_fake.execute_calls==1);assert(strstr(f.records,"status=complete")&&strstr(f.records,"letter=E"));
    a.answers.restart=SETUP_RESTART_WAIT;tap(DU_KEY_F10);assert(f.eject==1&&!f.reboot);f.now+=100000000000ull;st_turn(&a);assert(!f.reboot);f.media=0;st_turn(&a);assert(f.reboot==1);st_free(&a);
    fixture();init();tap(DU_KEY_TAB);int focused=ui.focus;st_key(&a,DU_KEY_LSHIFT,1);tap(DU_KEY_TAB);st_key(&a,DU_KEY_LSHIFT,0);assert(ui.focus!=focused);
    tap(0x3d);assert(ui.dialog.id==ST_DLG_EXIT);tap(DU_KEY_ESC);assert(!ui.dialog.kind);no_writes();st_free(&a);
    for(int refusal=ST_STOP_VIDEO;refusal<=ST_STOP_DISK;refusal++){fixture();f.refusal=refusal;if(refusal==ST_STOP_NO_DISK)f.disks_n=0;if(refusal==ST_STOP_DISK)f.disks[0].installable=0;init();assert(a.page==ST_PAGE_STOP&&(int)a.stop==refusal);char name[64];snprintf(name,sizeof(name),"hardware-refusal-%d",refusal);frame(name);no_writes();tap(DU_KEY_F10);assert(f.reboot==1);no_writes();st_free(&a);}
    fixture();init();st_action(&a,ST_ID_RESCUE);assert(a.page==ST_PAGE_RESCUE);tap(DU_KEY_ESC);assert(a.page==ST_PAGE_WELCOME);no_writes();
    assert(strstr(a.report,"source_commit=aaaa"));st_action(&a,ST_ID_REPORT);assert(a.page==ST_PAGE_TEXT);tap(DU_KEY_ESC);assert(a.page==ST_PAGE_WELCOME);no_writes();st_free(&a);
    fixture();init();summary();tap(DU_KEY_ESC);assert(a.page==ST_PAGE_NETWORK&&!a.plan_ok);no_writes();st_free(&a);
    fixture();init();summary();a.disks[0].part_count=24;setup_fake.plan.erased_partitions=16;setup_fake.plan.erased_total=24;
    st_goto(&a,ST_PAGE_NETWORK);tap(DU_KEY_F10);assert(a.page==ST_PAGE_SUMMARY&&a.plan_ok);
    int omitted=0;for(unsigned i=0;i<a.view.count;i++)if(strstr(a.view.lines[i].text,"and 8 more"))omitted=1;
    assert(omitted);no_writes();st_free(&a);
    for(unsigned policy=SETUP_RESTART_EJECT;policy<=SETUP_RESTART_FORCE;policy++) {
        fixture();init();a.answers.restart=(enum setup_restart)policy;st_goto(&a,ST_PAGE_RESTART);tap(DU_KEY_F10);
        f.now+=61000000000ull;st_turn(&a);
        if(policy==SETUP_RESTART_WAIT){assert(!f.reboot);f.media=0;st_turn(&a);}
        assert(f.reboot==1);st_free(&a);
    }
    puts("PASS keyboard-only completion, refusal no-write paths, all restart policies");
}
static void errors(void){
    const enum setup_error_class classes[]={SETUP_ERR_SOURCE_READ,SETUP_ERR_MEDIA_REMOVED,SETUP_ERR_SOURCE_MISMATCH,SETUP_ERR_DEST_FLUSH,SETUP_ERR_DEST_CHANGED,SETUP_ERR_MEMORY};
    for(unsigned k=0;k<sizeof(classes)/sizeof(classes[0]);k++){
        fixture();init();summary();
        setup_fake_progress(SETUP_PHASE_COPY,"SYSTEM/test",4096,8192,1000);
        if(classes[k]==SETUP_ERR_SOURCE_READ){for(unsigned i=1;i<=5;i++)setup_fake_error(classes[k],i,true);}
        else if(classes[k]==SETUP_ERR_MEDIA_REMOVED){setup_fake_error(classes[k],1,true);setup_fake_progress(SETUP_PHASE_COMPLETE,"Matching disc",8192,8192,2000);}
        else setup_fake_error(classes[k],1,false);
        tap(DU_KEY_F10);assert(setup_fake.execute_calls==1);
        assert(a.page==(classes[k]==SETUP_ERR_MEDIA_REMOVED?ST_PAGE_RESTART:ST_PAGE_FAILED));
        if(classes[k]==SETUP_ERR_SOURCE_READ)assert(setup_fake.errors==5&&setup_fake.retries==4&&f.dialogs==5);
        st_free(&a);
    }
    fixture();init();summary();f.cancel_on_copy=1;setup_fake_progress(SETUP_PHASE_COPY,"SYSTEM/test",4096,8192,1000);setup_fake_progress(SETUP_PHASE_COPY,"SYSTEM/next",8192,16384,2000);setup_fake_progress(SETUP_PHASE_COPY,"SYSTEM/last",16384,16384,3000);tap(DU_KEY_F10);assert(a.page==ST_PAGE_FAILED&&a.failed_class==SETUP_ERR_CANCELLED);assert(setup_fake.cancels==1);st_free(&a);
    puts("PASS retry exhaustion, matching media, nonretryable errors, cancel");
}
static void copy_steps(unsigned n){for(unsigned i=0;i<n;i++)setup_fake_progress(SETUP_PHASE_COPY,"SYSTEM/test",4096*(i+1),4096*n,1000*(i+1));setup_fake_progress(SETUP_PHASE_COMPLETE,"Complete",4096*n,4096*n,1000*(n+1));}
static void power_button(void){
    /* Before the copy: Setup's own dialog; Resume keeps Setup, the default
     * powers off through call 73 OFF. Nothing is written to the disk. */
    fixture();init();assert(a.page==ST_PAGE_WELCOME);st_turn(&a);assert(!ui.dialog.kind);
    f.power_requests=1;f.hold_power=1;f.now+=ST_POWER_POLL_NS;st_turn(&a);
    assert(ui.dialog.kind&&ui.dialog.id==ST_DLG_POWER&&!strcmp(ui.dialog.title,"Exit Setup and power off?"));
    assert(strstr(ui.dialog.text,"Nothing has been written to the disk"));frame("power-before-copy");
    assert(strstr(f.records,"[setup] power button request=1 page=welcome"));
    tap(DU_KEY_ESC);assert(!ui.dialog.kind&&!f.power_off&&!a.quit&&strstr(f.records,"power button declined"));
    for(unsigned i=0;i<5;i++){f.now+=ST_POWER_POLL_NS;st_turn(&a);}assert(!ui.dialog.kind); /* one request, one dialog */
    f.power_requests=2;f.now+=ST_POWER_POLL_NS;st_turn(&a);assert(ui.dialog.id==ST_DLG_POWER);
    tap(DU_KEY_ENTER);assert(f.power_off==1&&a.quit&&a.power_requested&&strstr(f.records,"[setup] power off source=button page=welcome"));
    no_writes();st_free(&a);

    /* Another dialog is never replaced: the request waits for it. */
    fixture();init();tap(0x3d);assert(ui.dialog.id==ST_DLG_EXIT);
    f.power_requests=7;f.hold_dialogs=1;f.now+=ST_POWER_POLL_NS;st_turn(&a);assert(ui.dialog.id==ST_DLG_EXIT&&a.power_pending);
    tap(DU_KEY_ESC);f.now+=ST_POWER_POLL_NS;st_turn(&a);assert(ui.dialog.id==ST_DLG_POWER&&!a.power_pending);
    f.power_fail=1;tap(DU_KEY_ENTER);assert(f.power_off==1&&!a.quit&&ui.dialog.kind&&strstr(ui.dialog.text,"Shut down is unavailable"));
    no_writes();st_free(&a);

    /* During the copy: an incomplete-installation warning that defaults to
     * continuing. Enter keeps installing. */
    fixture();init();summary();copy_steps(12);f.power_on_copy=1;
    tap(DU_KEY_F10);assert(f.copy_power_dialogs==1&&!f.power_off&&a.page==ST_PAGE_RESTART&&setup_fake.execute_calls==1&&!setup_fake.cancels);
    assert(strstr(f.records,"page=copy")&&strstr(f.records,"status=complete"));st_free(&a);
    /* Explicit confirmation cancels libsetup at its next boundary, then powers off. */
    fixture();init();summary();copy_steps(12);f.power_on_copy=1;f.power_confirm=1;
    tap(DU_KEY_F10);assert(f.copy_power_dialogs==2&&f.power_moved&&setup_fake.cancels==1&&a.failed_class==SETUP_ERR_CANCELLED);
    assert(f.power_off==1&&a.quit&&strstr(f.records,"confirmed during copy")&&strstr(f.records,"power off source=button page=failed"));st_free(&a);
    /* Unanswered when the copy ends: asked again with the post-copy text,
     * and the restart countdown waits for the answer. */
    fixture();init();summary();copy_steps(12);f.power_on_copy=1;f.hold_power=1;
    tap(DU_KEY_F10);assert(a.page==ST_PAGE_RESTART&&f.copy_power_dialogs==0&&f.power_dialogs);
    st_turn(&a);assert(ui.dialog.id==ST_DLG_POWER&&strstr(ui.dialog.text,"If Setup did not complete"));
    f.now+=61000000000ull;st_turn(&a);assert(!f.reboot&&!f.eject&&ui.dialog.id==ST_DLG_POWER);
    tap(DU_KEY_ENTER);assert(f.power_off==1&&a.quit&&!f.reboot);st_free(&a);
    /* A copy error takes the dialog slot; the power request is kept and
     * asked again afterwards, never lost. */
    fixture();init();summary();for(unsigned i=0;i<8;i++)setup_fake_progress(SETUP_PHASE_COPY,"SYSTEM/test",4096*(i+1),65536,1000*(i+1));
    setup_fake_error(SETUP_ERR_SOURCE_READ,1,true);copy_steps(4);f.power_on_copy=1;f.hold_power=1;f.no_error_frame=1;
    tap(DU_KEY_F10);assert(f.power_kept==1&&setup_fake.retries==1&&a.page==ST_PAGE_RESTART&&!f.power_off);
    st_turn(&a);assert(ui.dialog.id==ST_DLG_POWER);tap(DU_KEY_ESC);assert(!ui.dialog.kind&&!f.power_off);st_free(&a);
    puts("PASS power button: Setup dialog, call-73 power off before the copy, incomplete-copy warning with explicit confirmation, no dialog replacement (a copy error keeps the request), restart countdown pause");
}
/* f9-07 amendment 1: the rescue page runs the f9-07 program headless, shows
 * the text it returns, and saves it as CIUKIREP.TXT next to HARDWARE.TXT; any
 * failure falls back to Setup's own report; the other pages keep the own report. */
static void rescue_report(void){
    static const char text[]="CiukiOS Hardware Report\nCIUKIREP.TXT\n\n[CPU]\nVendor: GenuineIntel\n";
    fixture();f.report_text=text;init();st_action(&a,ST_ID_RESCUE);assert(a.page==ST_PAGE_RESCUE);
    st_action(&a,ST_ID_REPORT);
    assert(f.report_calls==1&&a.page==ST_PAGE_TEXT&&a.back_page==ST_PAGE_RESCUE&&a.link_length==sizeof(text)-1);
    char want[96];snprintf(want,sizeof(want),"[rescue] action=hardware_report result=app bytes=%u",(unsigned)sizeof(text)-1);
    assert(strstr(f.records,want)&&!strstr(f.records,"fallback")&&!strstr(f.records,"hardware_view"));
    assert(strstr(a.text.text,"Vendor: GenuineIntel")&&!strstr(a.text.text,"hardware report\r"));
    no_writes();
    /* Save to USB: Setup's HARDWARE.TXT, then the received text as CIUKIREP.TXT. */
    char result[200];assert(!st_save_report(&a,result,sizeof(result)));
    assert(f.writes==2&&!strcmp(f.first_path,"F:\\HARDWARE.TXT")&&!strcmp(f.last_path,"F:\\CIUKIREP.TXT")&&!strcmp(f.written,text));
    assert(strstr(result,"CIUKIREP.TXT")&&strstr(result,"HARDWARE.TXT")&&strstr(f.records,"hardware_export result=ok files=2"));
    tap(DU_KEY_ESC);assert(a.page==ST_PAGE_RESCUE);
    /* A later failure drops the received text: a stale report is never saved. */
    f.report_result=-ETIMEDOUT;f.report_reason="timeout";f.writes=0;
    st_action(&a,ST_ID_REPORT);assert(a.page==ST_PAGE_TEXT&&!a.link_length&&strstr(f.records,"result=fallback reason=timeout"));
    assert(!st_save_report(&a,result,sizeof(result))&&f.writes==1&&!strcmp(f.last_path,"F:\\HARDWARE.TXT"));
    st_free(&a);
    static const struct{int result,hook;const char *text,*reason,*logged;}failures[]={
        {-ENOENT,1,"","missing","missing"},{-ECHILD,1,"","exit","exit"},{-EPROTO,1,"","frame","frame"},{-EPIPE,1,"","closed","closed"},
        {-EIO,1,"","","failed"},{0,1,"","","empty"},{0,0,"","","unavailable"}};
    for(unsigned i=0;i<sizeof(failures)/sizeof(*failures);i++){
        fixture();f.report_result=failures[i].result;f.report_reason=failures[i].reason;f.report_text=failures[i].text;f.no_report_hook=!failures[i].hook;init();
        st_action(&a,ST_ID_RESCUE);st_action(&a,ST_ID_REPORT);
        snprintf(want,sizeof(want),"[rescue] action=hardware_report result=fallback reason=%s",failures[i].logged);
        assert(strstr(f.records,want)&&!strstr(f.records,"result=app")&&f.report_calls==(failures[i].hook?1u:0u));
        assert(a.page==ST_PAGE_TEXT&&a.back_page==ST_PAGE_RESCUE&&!a.link_length&&strstr(a.text.text,"CiukiOS 1.0 Beta Setup"));
        no_writes();
        /* The fallback page can still save to the USB drive (one file), then returns to the rescue page. */
        assert(!st_save_report(&a,result,sizeof(result))&&f.writes==1&&strstr(f.records,"files=1"));
        tap(DU_KEY_ESC);assert(a.page==ST_PAGE_RESCUE);st_free(&a);
    }
    /* Welcome and stop pages never run the program: they show Setup's own report. */
    fixture();init();st_action(&a,ST_ID_REPORT);assert(a.page==ST_PAGE_TEXT&&!f.report_calls&&!strstr(f.records,"hardware_report")&&strstr(f.records,"hardware_view"));st_free(&a);
    puts("PASS rescue hardware report: received text shown and saved as CIUKIREP.TXT next to HARDWARE.TXT, stale text dropped, fallback reasons, own report on the other pages");
}
static void memory_failures(void){
    for(unsigned failure=1;failure<=4;failure++) {
        fixture();fail_allocation=failure;allocation=0;init();fail_allocation=0;
        assert(a.page==ST_PAGE_FAILED&&a.failed_class==SETUP_ERR_MEMORY);no_writes();
        assert(strstr(f.records,"status=stopped reason=memory key=start"));
        if(!a.file){char result[200];assert(st_rescue_copy(&a,result,sizeof(result))<0);no_writes();}
        st_free(&a);
    }
    fixture();init();summary();setup_fake.prepare_result=-SETUP_ERR_MEMORY;
    tap(DU_KEY_ESC);tap(DU_KEY_F10);assert(a.page==ST_PAGE_FAILED&&a.failed_class==SETUP_ERR_MEMORY);no_writes();st_free(&a);
    puts("PASS start allocation failures and preflight memory stop");
}
static void unattended(void){
    fixture();f.cd_inf=valid;f.usb_inf="invalid";init();assert(a.unattended&&a.page==ST_PAGE_WELCOME&&!strcmp(a.answers.source,"D:\\SETUP.INF"));
    for(unsigned i=0;i<12&&a.page!=ST_PAGE_RESTART;i++)st_turn(&a);assert(a.page==ST_PAGE_RESTART&&setup_fake.execute_calls==1&&!f.writes);assert(a.plan.answers.tz_minutes==60);st_free(&a);
    fixture();f.usb_inf="[Setup]\nVersion=1\nUnknown=yes\n";init();assert(a.page==ST_PAGE_STATUS);assert(strstr(f.records,"status=stopped reason=unknown_key key=Unknown"));assert(f.writes==1&&!strcmp(f.last_path,"F:\\SETUP.STS"));assert(!setup_fake.execute_calls);st_free(&a);
    fixture();f.usb_inf="[Setup]\nVersion=1\n";init();assert(a.page==ST_PAGE_STATUS&&!strcmp(a.status.reason,SETUP_ST_MISSING));st_free(&a);
    fixture();init();char result[200];assert(!st_rescue_copy(&a,result,sizeof(result)));assert(f.writes==2&&!setup_fake.execute_calls);assert(!st_save_report(&a,result,sizeof(result)));st_free(&a);
    fixture();char without_erase[2048];snprintf(without_erase,sizeof(without_erase),"%s",valid);char *erase=strstr(without_erase,"EraseDisk=yes\n");assert(erase);memmove(erase,erase+14,strlen(erase+14)+1);
    f.usb_inf=without_erase;f.disks[0].part_count=0;f.disks[0].occupied=1;init();assert(a.page==ST_PAGE_STATUS&&!strcmp(a.status.reason,SETUP_ST_CONSENT));assert(!setup_fake.execute_calls);st_free(&a);
    puts("PASS CD precedence, unattended completion/status, USB-only rescue");
}
int main(int argc,char **argv){setvbuf(stdout,NULL,_IOLBF,0);assert(argc==3);folder=argv[2];FILE *fp=fopen(argv[1],"rb");assert(fp);assert(fread(portrait,4,256*256,fp)==256*256);fclose(fp);parser();renders();interactive();errors();power_button();rescue_report();memory_failures();unattended();native_tests(portrait);link_tests();return 0;}
