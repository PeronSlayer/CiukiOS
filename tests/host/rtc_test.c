/* Scripted CMOS, production decoder/timeout. SPDX-License-Identifier: GPL-2.0-only */
#include "../../src/kernel/fs/fs_port.h"
#include <stdio.h>
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"RTC line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
struct script { uint8_t regs[16]; unsigned begins, ends, pauses, reads, seconds; uint32_t now; bool held, stuck, rollover, frozen, broken; };
static void begin(void *p) { struct script *s=p; CHECK(!s->held); s->held=true; s->begins++; }
static void end(void *p) { struct script *s=p; CHECK(s->held); s->held=false; s->ends++; }
static uint8_t read_reg(void *p, uint8_t r) {
    struct script *s=p; CHECK(s->held && r<16 && r!=12); s->reads++;
    if (r==10 && s->stuck) return 0x80;
    if (!r && s->rollover && ++s->seconds==3) return 1;
    return s->regs[r];
}
static void write_reg(void *p,uint8_t r,uint8_t v) {
    struct script *s=p; CHECK(s->held && r<16 && r!=12 && (r==11 || (s->regs[11]&128)));
    if(!s->broken || r==11)s->regs[r]=v;
}
static uint32_t now(void *p) { return ((struct script *)p)->now; }
static bool pause_rtc(void *p) { struct script *s=p; CHECK(!s->held); s->pauses++; if (!s->frozen) s->now++; return true; }
int main(void) {
    struct fs_rtc_sample s={.second=0x59,.minute=0x58,.hour=0x23,.day=0x29,.month=2,.year=0x24,.control=2,.valid=0x80};
    uint16_t date,time; uint8_t tenth;
    CHECK(fs_rtc_decode(&s,&date,&time,&tenth));
    CHECK(date==((2024-1980)*512+2*32+29) && time==23*2048+58*32+29 && tenth==100);
    s.year=0x23; CHECK(!fs_rtc_decode(&s,&date,&time,&tenth)); s.year=0x24;
    s.hour=0x92; s.control=0; CHECK(fs_rtc_decode(&s,&date,&time,&tenth) && (time>>11)==12);
    s.hour=0x12; CHECK(fs_rtc_decode(&s,&date,&time,&tenth) && !(time>>11));
    s.hour=0x81; CHECK(fs_rtc_decode(&s,&date,&time,&tenth) && (time>>11)==13);
    s.hour=0; CHECK(!fs_rtc_decode(&s,&date,&time,&tenth));
    s=(struct fs_rtc_sample){.second=58,.minute=59,.hour=23,.day=31,.month=12,.year=99,.control=6,.valid=0x80};
    CHECK(fs_rtc_decode(&s,&date,&time,&tenth) && (date>>9)==19 && !tenth);
    s.year=0; s.day=29; s.month=2; CHECK(fs_rtc_decode(&s,&date,&time,&tenth));
    s.control=0x86; CHECK(!fs_rtc_decode(&s,&date,&time,&tenth)); s.control=6;
    s.valid=0; CHECK(!fs_rtc_decode(&s,&date,&time,&tenth)); s.valid=0x80;
    s.second=60; CHECK(!fs_rtc_decode(&s,&date,&time,&tenth));
    s.control=2; s.second=0x1a; CHECK(!fs_rtc_decode(&s,&date,&time,&tenth));
    struct script f={.regs={[0]=0,[2]=0x15,[4]=0x11,[7]=0x10,[8]=0x10,[9]=0x26,[11]=2,[13]=0x80}};
    const struct fs_rtc_ops ops={begin,end,read_reg,now,pause_rtc};
    CHECK(fs_rtc_read(&ops,&f,&date,&time,&tenth) && f.begins==1 && f.ends==1 && !f.pauses);
    f.rollover=true; f.seconds=0; CHECK(fs_rtc_read(&ops,&f,&date,&time,&tenth) && f.pauses==1);
    f.stuck=true; f.now=UINT32_MAX-40; unsigned before=f.pauses;
    CHECK(!fs_rtc_read(&ops,&f,&date,&time,&tenth) && f.pauses-before==100 && f.begins==f.ends);
    f.frozen=true; before=f.pauses;
    CHECK(!fs_rtc_read(&ops,&f,&date,&time,&tenth) && f.pauses-before==1024 && !f.held);
    const struct fs_rtc_writer writer={ops,write_reg};
    date=(2024-1980)*512+2*32+29;time=23*2048+58*32+29;
    for(unsigned mode=0;mode<4;mode++) {
        f=(struct script){.regs={[11]=(uint8_t)(0x40|(mode&1 ? 4 : 0)|(mode&2 ? 2 : 0)),[13]=128}};
        uint8_t control=f.regs[11];CHECK(fs_rtc_write(&writer,&f,date,time,100));
        CHECK(f.regs[11]==control && !f.held);
        uint16_t d,t;uint8_t sub;CHECK(fs_rtc_read(&ops,&f,&d,&t,&sub));
        CHECK(d==date && t==time && sub==100);
    }
    f.broken=true;CHECK(!fs_rtc_write(&writer,&f,date,time,0) && !(f.regs[11]&128));
    f.broken=false;f.stuck=true;f.now=0;CHECK(!fs_rtc_write(&writer,&f,date,time,0) && f.now==100);
        fs_set_calendar_clock(0); fs_timestamp(&date,&time,&tenth); CHECK(date==0x21 && !time && !tenth);
    printf("RTC: PASS (%u checks; BCD/binary, 12/24h, leap/pivot, UIP/seconds, timeout/wrap/stall, fallback)\n",checks);
    return 0;
}
