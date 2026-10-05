/* Host harness for the production WEBSTYLE.APP state machine. Not a DOS ABI
 * substitute; it checks the packed request, bounded parser and cascade. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define CIUKIOS_APP_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define EV_OPEN 1
#define EV_POLL 6
#define EV_CLOSE 7

struct ws_local_ref { const void *ptr; };
static struct ws_local_ref local_refs[8];
static u16 host_local_offset(const void *p)
{
    unsigned i;
    for(i=0;i<8;i++) if(local_refs[i].ptr==p) return (u16)(0x1000u+i*0x1000u);
    for(i=0;i<8;i++) if(!local_refs[i].ptr) { local_refs[i].ptr=p;return (u16)(0x1000u+i*0x1000u); }
    return 0;
}
#define WEBSTYLE_LOCAL_OFFSET(p) host_local_offset((const void *)(p))
u16 app_seg(void);
void far_copy(u16 ds,u16 doff,u16 ss,u16 soff,u16 n);
#include "../../src/apps/webstyle.c"

static u8 request_segment[65536];
static unsigned checks, failures;
u16 app_seg(void) { return 1; }
void far_copy(u16 ds,u16 doff,u16 ss,u16 soff,u16 n)
{
    u8 *dst=0; const u8 *src=0; unsigned i;
    if(ds==1) for(i=0;i<8;i++) if(local_refs[i].ptr&&doff==(u16)(0x1000u+i*0x1000u)) dst=(u8 *)local_refs[i].ptr;
    if(ss==1) for(i=0;i<8;i++) if(local_refs[i].ptr&&soff==(u16)(0x1000u+i*0x1000u)) src=(const u8 *)local_refs[i].ptr;
    if(ds==2&&(unsigned)doff+n<=sizeof request_segment) dst=request_segment+doff;
    if(ss==2&&(unsigned)soff+n<=sizeof request_segment) src=request_segment+soff;
    if(!dst||!src) { ++failures;return; }
    memmove(dst,src,n);
}

static int expect(int ok,const char *name)
{
    ++checks; if(!ok) { ++failures;fprintf(stderr,"FAIL: %s\n",name);return 0; } return 1;
}
static u16 transact(struct webstyle_request *r)
{
    memcpy(request_segment,r,sizeof *r);
    app_event(EV_POLL,2,0,0);
    memcpy(r,request_segment,sizeof *r);
    return r->status;
}
static int feed(const char *css,u16 chunk)
{
    struct webstyle_request r; u16 n; u32 off=0,total=(u32)strlen(css); int status=WEBSTYLE_STATUS_OK;
    while(off<total) {
        n=(u16)((total-off)>chunk?chunk:(total-off));
        memset(&r,0,sizeof r);r.abi_version=WEBSTYLE_ABI_VERSION;r.struct_bytes=sizeof r;
        r.op=WEBSTYLE_OP_FEED;r.css_bytes=n;memcpy(r.css,css+off,n);
        status=transact(&r);off+=n;if(status!=WEBSTYLE_STATUS_OK) return status;
    }
    return status;
}
static int operation(struct webstyle_request *r,u16 op)
{
    r->op=op;r->abi_version=WEBSTYLE_ABI_VERSION;r->struct_bytes=sizeof *r;return transact(r);
}
static int reset_worker(void)
{
    struct webstyle_request r;memset(&r,0,sizeof r);r.abi_version=WEBSTYLE_ABI_VERSION;r.struct_bytes=sizeof r;
    r.op=WEBSTYLE_OP_RESET;return operation(&r,WEBSTYLE_OP_RESET);
}
static int enter(struct webstyle_request *r,const char *tag,const char *id,const char *classes,const char *inline_style,u16 flags)
{
    memset(r,0,sizeof *r);r->abi_version=WEBSTYLE_ABI_VERSION;r->struct_bytes=sizeof *r;r->op=WEBSTYLE_OP_ENTER;r->flags=flags;
    strncpy(r->tag,tag,sizeof r->tag-1);if(id)strncpy(r->id,id,sizeof r->id-1);
    if(classes)strncpy(r->classes,classes,sizeof r->classes-1);
    if(inline_style)strncpy(r->inline_style,inline_style,sizeof r->inline_style-1);
    return transact(r);
}
static int apply(struct webstyle_request *r,const char *tag,const char *id,const char *classes)
{
    memset(r,0,sizeof *r);r->abi_version=WEBSTYLE_ABI_VERSION;r->struct_bytes=sizeof *r;r->op=WEBSTYLE_OP_APPLY;
    strncpy(r->tag,tag,sizeof r->tag-1);if(id)strncpy(r->id,id,sizeof r->id-1);
    if(classes)strncpy(r->classes,classes,sizeof r->classes-1);
    return transact(r);
}
static int leave_tag(const char *tag)
{
    struct webstyle_request r;memset(&r,0,sizeof r);r.abi_version=WEBSTYLE_ABI_VERSION;r.struct_bytes=sizeof r;r.op=WEBSTYLE_OP_LEAVE;
    if(tag)strncpy(r.tag,tag,sizeof r.tag-1);return transact(&r);
}

int main(void)
{
    static const char css[]=
        "/* split comment */ p { color: red; margin-left: 1px; font-weight: normal }\n"
        ".note { font-weight: bold; color: green }\n"
        "#target { background-color: #abcdef; color: #ff00ff }\n"
        "div p { margin-left: 2px }\n"
        "div > p.note { color: #0000ff !important; margin-left: 3px }\n"
        "section p { margin-right: 2px } section > p { margin-right: 4px }\n"
        "h1, h2 { font-style: italic }\n"
        "div { color: #123456; font-weight: bold } span { color: inherit }\n";
    struct webstyle_request r; unsigned i,j;
    memset(request_segment,0,sizeof request_segment); app_event(EV_OPEN,0,0,0);
    expect(reset_worker()==WEBSTYLE_STATUS_OK,"RESET accepted");
    /* Deliberately split selectors, comments and declarations across feeds. */
    expect(feed(css,7)==WEBSTYLE_STATUS_OK,"fragmented stylesheet FEED accepted");
    memset(&r,0,sizeof r);expect(operation(&r,WEBSTYLE_OP_SHEET_END)==WEBSTYLE_STATUS_OK,"SHEET_END accepted");

    expect(enter(&r,"div",0,"parent",0,0)==WEBSTYLE_STATUS_OK,"ENTER parent accepted");
    expect(r.style.color==0x123456&&r.style.font_weight==WEBSTYLE_WEIGHT_BOLD,"parent declarations applied");
    expect(enter(&r,"p","target","note","color: green; background-color: #00ff00",0)==WEBSTYLE_STATUS_OK,"ENTER child accepted");
    expect(r.style.color==0x0000ff,"important declaration beats ID and inline normal declarations");
    expect(r.style.margin[3]==3,"child selector wins for direct child spacing");
    expect(r.style.background_color==0x00ff00,"inline style wins normal cascade by specificity");
    expect(r.style.font_weight==WEBSTYLE_WEIGHT_BOLD,"class specificity beats tag rule");
    expect(leave_tag("div")==WEBSTYLE_STATUS_OK,"LEAVE unwinds matching element and malformed descendants");
    expect(apply(&r,"p",0,"note")==WEBSTYLE_STATUS_OK,"APPLY works without pushing");
    expect(r.style.margin[3]==1,"LEAVE removed former ancestor selectors");

    expect(enter(&r,"section",0,0,0,0)==WEBSTYLE_STATUS_OK,"ENTER section accepted");
    expect(enter(&r,"p",0,0,0,0)==WEBSTYLE_STATUS_OK,"ENTER direct paragraph accepted");
    expect(r.style.margin[1]==4,"child selector matches immediate parent");
    expect(leave_tag("section")==WEBSTYLE_STATUS_OK,"LEAVE section unwinds subtree");
    expect(enter(&r,"section",0,0,0,0)==WEBSTYLE_STATUS_OK,"ENTER second section accepted");
    expect(enter(&r,"div",0,0,0,0)==WEBSTYLE_STATUS_OK,"ENTER intermediate element accepted");
    expect(enter(&r,"p",0,0,0,0)==WEBSTYLE_STATUS_OK,"ENTER nested paragraph accepted");
    expect(r.style.margin[1]==2,"descendant selector matches through intermediate element");
    expect(leave_tag(0)==WEBSTYLE_STATUS_OK,"LEAVE top element accepted");
    expect(enter(&r,"span",0,0,0,1)==WEBSTYLE_STATUS_OK,"void ENTER computes without pushing");
    expect(r.style.color==0x123456,"explicit inherit uses computed parent value");
    expect(apply(&r,"h2",0,0)==WEBSTYLE_STATUS_OK,"grouped selector query accepted");
    expect(r.style.font_style==WEBSTYLE_FONT_ITALIC,"comma selector list is expanded");

    expect(reset_worker()==WEBSTYLE_STATUS_OK,"RESET clears rules and tree");
    memset(&r,0,sizeof r);r.abi_version=WEBSTYLE_ABI_VERSION;r.struct_bytes=sizeof r;r.op=WEBSTYLE_OP_FEED;
    r.css_bytes=WEBSTYLE_CHUNK_MAX;
    for(j=0;j<WEBSTYLE_CHUNK_MAX;j+=4) memcpy(r.css+j,"/**/",4);
    for(i=0;i<32;i++) expect(transact(&r)==WEBSTYLE_STATUS_OK,"bounded stylesheet comment chunk accepted");
    r.css_bytes=1;r.css[0]=' ';
    expect(transact(&r)==WEBSTYLE_STATUS_LIMIT,"32 KiB stylesheet limit is reported");

    expect(reset_worker()==WEBSTYLE_STATUS_OK,"RESET before rule-limit check accepted");
    {
        static char rule_buf[32];
        for(i=0;i<256;i++) {
            snprintf(rule_buf,sizeof rule_buf,"r%u{color:red}\n",i);
            expect(feed(rule_buf,(u16)strlen(rule_buf))==WEBSTYLE_STATUS_OK,"rule accepted");
        }
        snprintf(rule_buf,sizeof rule_buf,"r%u{color:red}\n",256);
        expect(feed(rule_buf,(u16)strlen(rule_buf))==WEBSTYLE_STATUS_LIMIT,"256-rule limit is reported");
    }

    expect(reset_worker()==WEBSTYLE_STATUS_OK,"RESET before at-rule and shorthand check accepted");
    expect(feed("\n@media (max-width:640px) { .hidden { display: none } }\n"
                "p { background: #00ff00; -webkit-box-shadow: 0 1px 2px; color: #123456 }\n",64)==WEBSTYLE_STATUS_OK,"at-rule and supported properties fed");
    memset(&r,0,sizeof r);expect(operation(&r,WEBSTYLE_OP_SHEET_END)==WEBSTYLE_STATUS_OK,"at-rule parsed cleanly to SHEET_END");
    expect(enter(&r,"p",0,0,0,0)==WEBSTYLE_STATUS_OK,"ENTER p accepted");
    expect(r.style.background_color==0x00ff00,"background shorthand sets background_color");
    expect(r.style.color==0x123456,"color applied despite unsupported properties");

    expect(reset_worker()==WEBSTYLE_STATUS_OK,"RESET before syntax check accepted");
    expect(feed("p { color:red",128)==WEBSTYLE_STATUS_OK,"incomplete CSS feed buffered");
    memset(&r,0,sizeof r);expect(operation(&r,WEBSTYLE_OP_SHEET_END)==WEBSTYLE_STATUS_ERROR,"unterminated rule is rejected");
    app_event(EV_CLOSE,0,0,0);
    if(failures) fprintf(stderr,"webstyle host checks: %u checks, %u failures\n",checks,failures);
    else printf("webstyle host checks: PASS (%u assertions)\n",checks);
    return failures?1:0;
}
