/* SPDX-License-Identifier: MIT -- G8 against the real VFS jobs and HKCR server. */
#include "fake_vfs.h"
#include "../../../apps/files/operations.h"
#include "../../../apps/w32srv/registry.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct wr_server registry;
static unsigned calls;
static uint32_t exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used)
{ size_t count=0; calls++; uint32_t e=wr_dispatch(&registry,42,in,n,out,cap,&count); *used=(uint32_t)count; return e; }
static void baseline(void)
{
    fake_reset(); fake_image_tree(); fake_create("/work",FM_DIR); fake_create("/dest",FM_DIR);
    fake_create("/Documents/Desktop",FM_DIR); fake_create("/Documents/SendTo",FM_DIR);
    fake_file("/work/a.txt",23,11); fake_file("/work/b.txt",51,23);
}
static void finish(struct fm_job *j)
{
    for(unsigned i=0;j->state==FM_RUNNING || j->state==FM_REPLACE;i++) {
        assert(i<30000);
        if(j->state==FM_REPLACE) fm_job_answer(j,FM_YES); else fm_job_step(j);
    }
    if(j->state!=FM_DONE) fprintf(stderr,"job failed error=%d state=%d\n",j->error,j->state);
    assert(j->state==FM_DONE); assert(!j->undo_error);
}
static void undo(struct fm_job *j)
{ fm_job_destroy(j); int e=fm_undo_start(j,&fake_vfs); if(e)fprintf(stderr,"undo start=%d\n",e); assert(!e); finish(j); fm_job_destroy(j); assert(fake_lookup(FM_UNDO_FILE)<0); }
static void undo_tests(void)
{
    baseline(); struct fm_model *m=calloc(1,sizeof(*m)); struct fm_job *j=calloc(1,sizeof(*j)); assert(m && j);
    fm_init(m,&fake_vfs); assert(!fm_navigate(m,"/work")); assert(fm_select_name(m,"a.txt",0)>=0);
    assert(!fm_rename_undo(m,m->active,"new.txt")); assert(fake_lookup("/work/new.txt")>=0); undo(j); assert(fake_lookup("/work/a.txt")>=0);
    assert(!fm_refresh(m)); fm_select_name(m,"a.txt",0); assert(!fm_rename_undo(m,m->active,"A.TXT")); undo(j);
    char *paths[]={"/work/a.txt","/work/b.txt"};
    assert(!fm_job_start(j,&fake_vfs,FM_COPY,paths,2,"/dest")); j->record_undo=1; finish(j); undo(j);
    assert(fake_lookup("/dest/a.txt")<0 && fake_lookup("/work/a.txt")>=0 && fake_lookup("/dest/b.txt")<0);
    assert(!fm_job_start(j,&fake_vfs,FM_MOVE,paths,2,"/dest")); j->record_undo=1; finish(j); undo(j);
    assert(fake_lookup("/work/a.txt")>=0 && fake_lookup("/work/b.txt")>=0 && fake_lookup("/dest/a.txt")<0);
    struct rb_policy policy; rb_policy_default(&policy);
    assert(!rb_delete_job(j,&fake_vfs,paths,2,&policy,0,0)); j->record_undo=1; finish(j);
    assert(fake_lookup("/work/a.txt")<0); undo(j); assert(fake_lookup("/work/a.txt")>=0 && fake_lookup("/work/b.txt")>=0);
    assert(!fm_job_start(j,&fake_vfs,FM_COPY,paths,1,"/dest")); j->record_undo=1; finish(j); fm_job_destroy(j);
    fake_nodes[fake_lookup("/dest/a.txt")].data[0]^=1;
    assert(fm_undo_start(j,&fake_vfs)==-EBUSY); assert(fake_lookup("/dest/a.txt")>=0);
    /* An overwrite invalidates Undo; it cannot erase the prior destination. */
    assert(!fm_job_start(j,&fake_vfs,FM_COPY,paths,1,"/dest")); j->record_undo=1; finish(j); fm_job_destroy(j);
    assert(fm_undo_start(j,&fake_vfs)==-ENOENT);
    /* A folder inverse checks its descendants, not just the folder mtime. */
    fake_create("/work/tree",FM_DIR); fake_file("/work/tree/child",8,1); char *tree[]={"/work/tree"};
    assert(!fm_job_start(j,&fake_vfs,FM_COPY,tree,1,"/dest")); j->record_undo=1; finish(j); fm_job_destroy(j);
    fake_file("/dest/tree/new",2,1); assert(fm_undo_start(j,&fake_vfs)==-EBUSY);
    /* Permanent delete is explicitly not offered as an undoable delete. */
    assert(!rb_delete_job(j,&fake_vfs,paths,1,&policy,0,RB_PERMANENT)); j->record_undo=1; finish(j); fm_job_destroy(j);
    assert(fm_undo_start(j,&fake_vfs)==-ENOENT);
    fm_destroy(m); free(m); free(j); fake_assert_clean();
    puts("PASS G8 Undo: rename/case, multi-copy/move/recycled delete, changed copies/descendants, replacement and permanent-delete safety");
}
static void link_tests(void)
{
    baseline(); struct fm_model *m=calloc(1,sizeof(*m)); struct desk_launch_record *r=calloc(1,sizeof(*r)),*decoded=calloc(1,sizeof(*decoded)); assert(m && r && decoded);
    fm_init(m,&fake_vfs); assert(!fm_navigate(m,"/work"));
    *r=(struct desk_launch_record){.label="Ciuki document",.target="/work/a.txt",.show=1};
    char path[FM_PATH]; assert(!fm_shortcut(m,r,path)); assert(!fm_link_read(&fake_vfs,path,decoded)); assert(!strcmp(decoded->target,r->target));
    assert(!fm_shortcut(m,r,path) && strstr(path,"(2).lnk"));
    strcpy(r->label,"Ciuki: a/b"); assert(!fm_shortcut(m,r,path)); assert(strstr(path,"Ciuki_ a_b.lnk"));
    char *paths[]={"/work/a.txt","/work/b.txt"}; assert(!fm_link_selection(m,paths,2));
    char manifest[FM_PATH]; char **selected=NULL; unsigned count=0;
    assert(!fm_shell_selection_write(&fake_vfs,paths,2,manifest));
    assert(!fm_shell_selection_read(&fake_vfs,manifest,&selected,&count) && count==2);
    assert(!strcmp(selected[0],paths[0]) && !strcmp(selected[1],paths[1])); for (unsigned i=0;i<count;i++) free(selected[i]); free(selected);
    assert(!fake_vfs.unlink(NULL,manifest));
    assert(!fm_bitmap(m,"white.bmp")); int bmp=fake_lookup("/work/white.bmp"); assert(bmp>=0 && fake_nodes[bmp].meta.size==58 && fake_nodes[bmp].data[54]==255);
    uint8_t bytes[512]={0}; static const uint8_t clsid[]={1,0x14,2,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    desk_link_put32(bytes,76); memcpy(bytes+4,clsid,16); desk_link_put32(bytes+20,2); desk_link_put32(bytes+60,1);
    uint8_t *info=bytes+76; desk_link_put32(info,60); desk_link_put32(info+4,28); desk_link_put32(info+8,1); desk_link_put32(info+16,28); desk_link_put32(info+24,52);
    strcpy((char *)info+28,"C:\\work\\a.txt"); strcpy((char *)info+52,"a.txt");
    assert(!fm_link_decode(bytes,136,"/work/win.lnk",decoded)); assert(!strcmp(decoded->target,"C:\\work\\a.txt"));
    for(unsigned n=0;n<76;n++) assert(fm_link_decode(bytes,n,"/work/win.lnk",decoded)<0);
    desk_link_put32(info,0xffffffffu); assert(fm_link_decode(bytes,136,"/work/win.lnk",decoded)<0);
    memset(bytes+76,0,sizeof(bytes)-76); desk_link_put32(bytes+20,8|128); bytes[76]=5;
    const char *relative="a.txt"; for(unsigned i=0;i<5;i++) bytes[78+i*2]=(uint8_t)relative[i];
    assert(!fm_link_decode(bytes,88,"/work/win.lnk",decoded)); assert(!strcmp(decoded->target,"/work/a.txt"));
    bytes[78]=0; bytes[79]=0xd8; assert(fm_link_decode(bytes,88,"/work/win.lnk",decoded)<0);
    /* Cycle depth and unsupported shell namespace links are ordinary errors. */
    strcpy(r->target,"/work/cycle.lnk"); uint8_t native[DESK_LINK_BYTES]; unsigned n=desk_link_encode(r,native,sizeof(native));
    assert(!fm_write_new(&fake_vfs,r->target,native,n)); assert(fm_launch_resolve(&fake_vfs,r->target,NULL,decoded)==-EINVAL);
    fm_destroy(m); free(m); free(r); free(decoded); fake_assert_clean();
    puts("PASS G8 shortcuts: CIUKLNK3 round trips, unique names, selection, BMP, Windows LinkInfo/Unicode relative links, malformed data and cycles");
}
static void association_tests(const char *directory)
{
    struct wr_io io; wr_posix_io(&io,directory); assert(wr_init(&registry,&io,0)); fm_registry_exchange=exchange;
    baseline(); assert(!fm_associate("/work/a.txt","/Programs/ciuknote"));
    struct desk_launch_record builtin;
    assert(!fm_association("/work/picture.bmp",NULL,&builtin) && !strcmp(builtin.target,"/Programs/viewer"));
    assert(!fm_association("/work/sound.wav",NULL,&builtin) && !strcmp(builtin.target,"/Programs/music"));
    assert(!fm_registry_set(".only","","OnlyEdit"));
    assert(!fm_registry_set("OnlyEdit\\shell\\edit\\command","","\"/Programs/ciuknote\" \"%1\""));
    assert(!fm_association("/work/document.only",NULL,&builtin) && !strcmp(builtin.target,"/Programs/ciuknote"));
    char value[300]; assert(!fm_registry_get(".txt","",value,sizeof(value)) && !strcmp(value,"CiukiOS.txt"));
    struct desk_launch_record *r=calloc(1,sizeof(*r)); assert(r);
    assert(!fm_launch_resolve(&fake_vfs,"/work/a.txt",NULL,r)); assert(!strcmp(r->target,"/Programs/ciuknote") && !strcmp(r->arguments,"\"/work/a.txt\""));
    assert(!fm_associate("/work/a.txt","C:\\Windows\\Other.exe"));
    assert(!fm_launch_resolve(&fake_vfs,"/work/a.txt",NULL,r) && !strcmp(r->target,"/Windows/Other.exe"));
    struct fm_filetype *t=calloc(1,sizeof(*t)); assert(t); strcpy(t->extension,".txt"); assert(!fm_filetype_get(t));
    strcpy(t->description,"Ciuki text"); strcpy(t->icon,"C:\\Windows\\Other.exe,1"); strcpy(t->verb,"edit");
    strcpy(t->commands[1],"\"/Programs/paint\" \"%1\""); strcpy(t->commands[2],"\"/Programs/ciuknote\" /p \"%1\""); t->edit_flags=0x10000;
    assert(!fm_filetype_set(t)); memset(t,0,sizeof(*t)); strcpy(t->extension,".txt"); assert(!fm_filetype_get(t));
    assert(!strcmp(t->description,"Ciuki text") && !strcmp(t->verb,"edit") && t->edit_flags==0x10000 && strstr(t->icon,",1"));
    assert(!fm_launch_resolve(&fake_vfs,"/work/a.txt",NULL,r) && !strcmp(r->target,"/Programs/paint"));
    assert(!fm_association("/work/a.txt","print",r) && strstr(r->arguments,"/p"));
    /* Same server, a different Win32 client modifies the same HKCR bytes. */
    struct wr_key *software=wr_child(&registry,registry.roots[0],"Software"),*classes=wr_child(&registry,software,"Classes");
    struct wr_key *cls=wr_child(&registry,classes,"CiukiOS.txt"),*shell=wr_child(&registry,cls,"shell"),*edit=wr_child(&registry,shell,"edit"),*command=wr_child(&registry,edit,"command");
    assert(command); uint8_t text[128]={0}; const char *newcommand="\"/Programs/viewer\" \"%1\"";
    for(unsigned i=0;i<strlen(newcommand);i++)text[i*2]=(uint8_t)newcommand[i];
    assert(!wr_put(&registry,command,"",1,text,(uint32_t)(strlen(newcommand)+1)*2));
    assert(!fm_launch_resolve(&fake_vfs,"/work/a.txt",NULL,r) && !strcmp(r->target,"/Programs/viewer"));
    assert(!fm_registry_set("CiukiOS.txt\\shell\\edit\\command","","bad %q"));
    assert(fm_launch_resolve(&fake_vfs,"/work/a.txt",NULL,r)<0); /* no catalogue bypass */
    assert(!fm_registry_set(".xyz\\ShellNew","NullFile",""));
    struct fm_model *m=calloc(1,sizeof(*m)); assert(m); fm_init(m,&fake_vfs); assert(!fm_navigate(m,"/work"));
    char path[FM_PATH]; assert(!fm_shellnew(m,".xyz",path)); assert(fake_nodes[fake_lookup(path)].meta.size==0);
    assert(!fm_registry_set(".abc\\ShellNew","FileName","C:\\work\\b.txt"));
    assert(!fm_shellnew(m,".abc",path)); assert(fake_nodes[fake_lookup(path)].meta.size==51);
    char name[300]; assert(!fm_registry_enum("",0,name,sizeof(name)) && name[0]);
    assert(!registry.handles); assert(calls>40);
    fm_destroy(m); free(m); free(t); free(r); wr_destroy(&registry); fm_registry_exchange=NULL; fake_assert_clean();
    puts("PASS G8 HKCR: production service, shared native/Win32 data, Always use, verbs/default/icon/description/EditFlags, ShellNew templates and NullFile");
}
static void launch_tests(void)
{
    baseline(); struct desk_launch_record *r=calloc(1,sizeof(*r)); assert(r);
    const char *names[]={"/work/prog.exe","/work/prog.com","/work/prog.bat","/work/native"};
    for(unsigned i=0;i<4;i++) {
        int node=fake_file(names[i],4,0); if(i==3) memcpy(fake_nodes[node].data,"\177ELF",4);
        assert(!fm_launch_resolve(&fake_vfs,names[i],NULL,r)); assert(!strcmp(r->target,names[i]));
    }
    assert(fm_launch_resolve(&fake_vfs,"/dest",NULL,r)==1 && !strcmp(r->target,"/Programs/files"));
    assert(!fm_launch_resolve(&fake_vfs,"https://example.com/ciuki",NULL,r) && !strcmp(r->target,"/Programs/netsurf"));
    uint8_t pif[369]={0}; strcpy((char *)pif+36,"C:\\work\\prog.com"); strcpy((char *)pif+101,"C:\\work"); strcpy((char *)pif+165,"/test");
    assert(!fm_write_new(&fake_vfs,"/work/test.pif",pif,sizeof(pif)));
    assert(!fm_launch_resolve(&fake_vfs,"/work/test.pif",NULL,r)); assert(!strcmp(r->target,"/work/prog.com") && !strcmp(r->arguments,"/test") && !strcmp(r->directory,"/work"));
    uint8_t extended[520]={0}; memcpy(extended,pif,sizeof(pif)); strcpy((char *)extended+36,"prog.com");
    memcpy(extended+369,"MICROSOFT PIFEX",15); extended[385]=0x87; extended[386]=1;
    memcpy(extended+391,"WINDOWS 386",11); extended[407]=255; extended[408]=255;
    extended[409]=0x9d; extended[410]=1; extended[411]=104;
    strcpy((char *)extended+453,"/enhanced");
    assert(!fm_write_new(&fake_vfs,"/work/enhanced.pif",extended,sizeof(extended)));
    assert(!fm_launch_resolve(&fake_vfs,"/work/enhanced.pif",NULL,r) && !strcmp(r->target,"/work/prog.com") && !strcmp(r->arguments,"/enhanced"));
    assert(fm_launch_resolve(&fake_vfs,"/missing",NULL,r)==-ENOENT);
    char args[100]="\"a b\" one\\two \"c\\\"d\"",*argv[4]; int n=fm_launch_arguments(args,argv,4);
    assert(n==3 && !strcmp(argv[0],"a b") && !strcmp(argv[1],"one\\two") && !strcmp(argv[2],"c\"d"));
    strcpy(args,"\"unfinished"); assert(fm_launch_arguments(args,argv,4)==-EINVAL);
    free(r); fake_assert_clean(); puts("PASS G8 shared launcher: native ELF, EXE/COM/BAT/PIF, folder, URL, arguments and typed failures");
}
int main(int argc,char **argv)
{ setvbuf(stdout,NULL,_IONBF,0); assert(argc==2); undo_tests(); link_tests(); association_tests(argv[1]); launch_tests(); fake_reset(); return 0; }
