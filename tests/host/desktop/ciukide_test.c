/* SPDX-License-Identifier: MIT
 * Production editor/project/job logic; only OS and file boundaries are fake. */
#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char cfg[]="sources=main.c other.c\noutput=hello\nlibraries=deskui\n";
static const char *streams[2]; static size_t positions[2];
static int exit_status, waited, closed, start_fail, read_error, would_block;
static int start(void *ctx,const char *compiler,char *const argv[],int fds[2])
{
    (void)ctx; assert(!strcmp(compiler,"/Programs/tcc"));
    assert(!strcmp(argv[1],"-o") && !strcmp(argv[2],"/Documents/Test/hello"));
    assert(!strcmp(argv[3],"/Documents/Test/main.c") && !strcmp(argv[4],"/Documents/Test/other.c"));
    assert(!strcmp(argv[5],"-ldeskui") && !argv[6]);
    if (start_fail) return -ENOENT;
    fds[0]=17; fds[1]=18; return 42;
}
static ssize_t take(void *ctx,int fd,char *out,size_t cap)
{
    (void)ctx; (void)cap; unsigned stream=(unsigned)(fd-17); assert(stream<2);
    if (read_error) { read_error=0; errno=EINTR; return -1; }
    if (would_block) { would_block=0; errno=EAGAIN; return -1; }
    size_t n=strlen(streams[stream]+positions[stream]); if (n>7) n=7;
    memcpy(out,streams[stream]+positions[stream],n); positions[stream]+=n; return (ssize_t)n;
}
static int reap(void *ctx,int pid,int *status)
{ (void)ctx; assert(pid==42); if (!waited++) return -EINTR; *status=exit_status; return pid; }
static void shut(void *ctx,int fd) { (void)ctx; assert(fd==17 || fd==18); ++closed; }
static const struct ide_process_ops process={start,take,reap,shut};
static void reset_io(const char *out,const char *error,int status)
{ streams[0]=out; streams[1]=error; positions[0]=positions[1]=0; exit_status=status; waited=closed=0; start_fail=0; read_error=would_block=1; }
static void parser(void)
{
    struct ide_project p={0}; assert(!ide_project_parse(&p,cfg,strlen(cfg)));
    assert(p.source_count==2 && p.library_count==1);
    const char *bad[]={"sources=main.c\noutput=../out", "sources=/main.c\noutput=out", "sources=-o\noutput=out",
        "sources=main.c\noutput=main.c", "sources=main.c\noutput=project.cfg", "sources=main.c\noutput=out\ncommand=rm",
        "sources=main.c\noutput=out\nsources=again.c", "sources=main.c\noutput=out\nlibraries=../c",
        "sources=main.c main.c\noutput=out", "sources=main.c\noutput=out\nlibraries=$(echo)",
        "sources=main.c\noutput=MAIN.C", "sources=main.c\noutput=PROJECT.CFG", "sources=main.c MAIN.c\noutput=out"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        struct ide_project before=p; assert(ide_project_parse(&p,bad[i],strlen(bad[i]))<0); assert(!memcmp(&p,&before,sizeof(p)));
    }
    const char crlf[]="# comment\r\n sources = dir/main.c\r\n output = out\r\n libraries = -ldeskui m\r\n";
    assert(!ide_project_parse(&p,crlf,strlen(crlf)) && p.library_count==2);
    assert(!ide_relative_path("a//b.c") && !ide_relative_path("a/./b.c") && !ide_relative_path("a/../b.c"));
    assert(ide_relative_path("sub/main.c"));
    char many[2048]="sources=";
    for (unsigned i=0;i<17;++i) { char name[24]; snprintf(name,sizeof(name),"file%u.c ",i); strcat(many,name); }
    strcat(many,"\noutput=out\n"); assert(ide_project_parse(&p,many,strlen(many))<0);
    assert(ide_project_parse(&p,"sources=main.c\0\noutput=x",24)<0);
}
static void diagnostics(void)
{
    struct ide_error e; assert(ide_diagnostic("main.c:12: error: expected ';'",&e) && e.line==12 && !e.warning);
    assert(ide_diagnostic("sub/other.c:2:7: warning: unused",&e) && e.column==7 && e.warning);
    assert(!ide_diagnostic("tcc: error: not found",&e));
    assert(!ide_diagnostic("main.c:999999999999999: error: overflow",&e));
    struct ide_model m={0}; assert(!ide_project_parse(&m.project,cfg,strlen(cfg)));
    assert(!ide_build_begin(&m));
    const char *a="main.c:2: error: bad\n", *b="other.c:4: warning: unused";
    for (size_t i=0;i<strlen(a);++i) ide_output(&m,0,a+i,1);
    ide_output(&m,1,b,strlen(b)); ide_output_end(&m,1);
    assert(m.errors==1 && m.warnings==1 && !strcmp(m.error[1].file,"other.c"));
    char excess[IDE_LINE+10]; memset(excess,'x',sizeof(excess)); ide_output(&m,0,excess,sizeof(excess)); ide_output(&m,0,"\n",1);
    for (unsigned i=0;i<100;++i) ide_output(&m,0,a,strlen(a));
    assert(m.errors+m.warnings==IDE_ERRORS && m.dropped>0);
    for (unsigned i=0;i<64;++i) ide_output(&m,0,excess,sizeof(excess));
    assert(m.log_truncated && m.log_len==IDE_LOG);
}
static void state_and_job(void)
{
    struct ide_model m={0}; struct ide_job j;
    assert(!ide_project_parse(&m.project,cfg,strlen(cfg)));
    assert(ide_run_begin(&m)<0);
    reset_io("compiled\n","other.c:3: warning: note\n",0);
    assert(!ide_job_start(&j,&m,&process,NULL,"/Programs/tcc","/Documents/Test"));
    assert(ide_build_begin(&m)==-EBUSY && ide_run_begin(&m)<0);
    for (unsigned i=0;j.pid && i<100;++i) ide_job_poll(&j);
    assert(!j.pid && closed==2 && m.state==IDE_READY && m.built && m.warnings==1);
    assert(!ide_run_begin(&m)); ide_run_result(&m,-ENOENT); assert(m.state==IDE_READY);
    assert(!ide_run_begin(&m)); ide_run_result(&m,52); assert(m.state==IDE_RUNNING);
    ide_changed(&m); assert(!m.built && ide_run_begin(&m)<0);
    reset_io("","main.c:2: error: bad",256);
    assert(!ide_job_start(&j,&m,&process,NULL,"/Programs/tcc","/Documents/Test"));
    for (unsigned i=0;j.pid && i<100;++i) ide_job_poll(&j);
    assert(m.state==IDE_FAILED && m.errors==1 && !m.built && m.exit_code==1);
    reset_io("","",0);
    assert(!ide_job_start(&j,&m,&process,NULL,"/Programs/tcc","/Documents/Test"));
    ide_changed(&m);
    for (unsigned i=0;j.pid && i<100;++i) ide_job_poll(&j);
    assert(m.state==IDE_FAILED && !m.built);
    start_fail=1; assert(ide_job_start(&j,&m,&process,NULL,"/Programs/tcc","/Documents/Test")==-ENOENT);
    assert(m.state==IDE_FAILED && !j.pid);
    reset_io("","",9); /* killed by signal */
    assert(!ide_job_start(&j,&m,&process,NULL,"/Programs/tcc","/Documents/Test"));
    for (unsigned i=0;j.pid && i<100;++i) ide_job_poll(&j);
    assert(m.state==IDE_FAILED && m.exit_code==137);
}
static int saved,fail_save;
static int load(void *ctx,const char *path,char *out,size_t cap,size_t *n)
{
    (void)ctx; const char *text=strstr(path,"project.cfg") ? cfg : !strcmp(path,"/Documents/Test/other.c") ? "int second;\nline 2\n" : "int main(void) { return 0; }\n";
    *n=strlen(text); assert(*n<cap); memcpy(out,text,*n); return 0;
}
static int save(void *ctx,const char *path,const char *text,size_t n)
{ (void)ctx; (void)text; (void)n; assert(strstr(path,"/Documents/Test/")); ++saved; return fail_save ? -EIO : 0; }
static const struct cn_fs fs={NULL,NULL,NULL,load,save};
static void editor(void)
{
    struct ide_app *a=calloc(1,sizeof(*a)); struct du_ui *u=calloc(1,sizeof(*u));
    uint32_t *pixels=calloc(640*440,4); assert(a && u && pixels);
    du_init(u,pixels,640,440); a->fs=&fs; a->process=&process;
    assert(!ide_attach(a,u)); assert(!ide_open_project(a,"/Documents/Test"));
    assert(ide_open_project(a,"/Documents/..")==-EINVAL && ide_open_project(a,"/Documents/Test/.")==-EINVAL);
    assert(a->editor.line_numbers && a->syntax[0]==IDE_C_KEYWORD);
    du_render(u);
    /* Icon toolbar with tooltips, captioned panes, tabbed status bar. */
    for (int id=IDE_OPEN;id<=IDE_GUIDE;++id) if (id!=IDE_NEW_WINDOW && id!=IDE_NEW_CONSOLE) {
        struct du_widget *b=du_get(u,id); assert(b->kind==DU_TOOLBUTTON && b->tip[0] && !b->text[0] && b->box.w>=28);
    }
    assert(!strcmp(du_get(u,IDE_HDR_PROJECT)->text,"Project") && !strcmp(du_get(u,IDE_HDR_EDITOR)->text,"Editor") &&
           !strcmp(du_get(u,IDE_HDR_ERRORS)->text,"Errors") && !strcmp(du_get(u,IDE_HDR_OUTPUT)->text,"Output"));
    assert(du_get(u,IDE_HDR_EDITOR)->box.y+17<=du_get(u,IDE_EDITOR)->box.y && du_get(u,IDE_HDR_OUTPUT)->box.x==du_get(u,IDE_OUTPUT)->box.x);
    { const char *st=du_get(u,IDE_STATUS)->text; int tabs=0; for (;*st;++st) tabs+=*st=='\t';
      assert(tabs==3 && strstr(du_get(u,IDE_STATUS)->text,"\tLn 1, Col 1\t0 errors, 0 warnings")); }
    struct du_widget *edit=du_get(u,IDE_EDITOR); assert(a->editor.cols<(unsigned)edit->box.w/DU_TA_CELL_W);
    assert(du_ta_hit(&a->editor,edit->box,edit->box.x+46,edit->box.y+4)==0);
    assert(!du_ta_insert(&a->editor,"/* change */",12,DU_TA_EDIT_TYPE));
    ide_event(u,&(struct du_event){.type=DU_EV_CHANGE,.id=IDE_EDITOR},a);
    assert(!a->model.built && a->syntax[0]==IDE_C_COMMENT);
    fail_save=1; assert(ide_save(a)==-EIO && a->editor.modified); fail_save=0;
    assert(!ide_save(a) && !a->editor.modified && saved==2);
    a->model.error[0]=(struct ide_error){.file="/Documents/Test/other.c",.line=2}; a->model.errors=1;
    ide_event(u,&(struct du_event){.type=DU_EV_OPEN,.id=IDE_ERROR_LIST,.index=0},a);
    unsigned line,col; du_ta_line_col(&a->editor,a->editor.cursor,&line,&col);
    assert(!strcmp(a->file,"other.c") && line==2);
    a->model.state=IDE_READY; a->model.built=1; a->editor.modified=1; fail_save=1;
    ide_event(u,&(struct du_event){.type=DU_EV_ACTIVATE,.id=IDE_BUILD},a);
    assert(!a->model.built && a->model.state==IDE_FAILED); fail_save=0;
    ide_detach(a); free(a); free(u); free(pixels);
}
static void syntax(void)
{
    const char s[]="/* comment\n still comment */\nint x=42; // tail\nchar *s=\"a\\\"b\";\n#define YES 1\n";
    unsigned char colours[sizeof(s)]; ide_highlight(s,strlen(s),colours);
    assert(colours[3]==IDE_C_COMMENT && colours[15]==IDE_C_COMMENT);
    assert(colours[strstr(s,"int x")-s]==IDE_C_KEYWORD);
    assert(colours[strstr(s,"42")-s]==IDE_C_NUMBER);
    assert(colours[strstr(s,"\"a")-s]==IDE_C_STRING);
    assert(colours[strstr(s,"#define")-s]==IDE_C_PREPROCESSOR);
}
int main(void)
{ parser(); diagnostics(); state_and_job(); syntax(); editor(); puts("CiukIDE parser, diagnostics, build/run, editor and gutter: PASS"); return 0; }
