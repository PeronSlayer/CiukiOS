/* SPDX-License-Identifier: MIT */
#include "../../../apps/accessories/help.h"
#include "../../../apps/desktop/help_request.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check_document(const char *path,int format)
{
    struct hf_document *d=NULL; int e=hf_open_file(&d,path);
    if (e) fprintf(stderr,"open %s: %d\n",path,e);
    assert(!e && d && d->format==(unsigned)format && d->count==2);
    int first=hf_keyword(d,"Overview",0),second=hf_context(d,42);
    assert(first>=0 && second>=0 && first!=second);
    assert(hf_context(d,9999)==-ENOENT && hf_keyword(d,"missing",0)==-ENOENT);
    assert(!hf_page_load(d,(unsigned)first) && !hf_page_load(d,(unsigned)second));
    assert(strstr(d->pages[second].text,format==1 ? "Second topic" : "Related destination"));
    assert(d->pages[first].link_count);
    if (format==1) {
        assert(strstr(d->pages[first].text,"An authored phrase for the Help parser."));
        assert(hf_hash(d,hf_context_hash("second"))==second);
        assert(hf_macro(d,"JI(\"\",\"second\")",(unsigned)first)==second);
        assert(hf_macro(d,"JumpContext(\"\",42)",(unsigned)first)==second);
        assert(hf_macro(d,"Contents();Next()",(unsigned)first)==second);
        assert(hf_macro(d,"Prev()",(unsigned)second)==first);
        assert(hf_macro(d,"KLink(\"Overview\")",(unsigned)second)==first);
        assert(hf_macro(d,"PopupContext(\"\",42)",(unsigned)first)==second);
        char macro[96]; snprintf(macro,sizeof(macro),"JumpHash(\"\",%u)",hf_context_hash("second"));
        assert(hf_macro(d,macro,(unsigned)first)==second);
        assert(hf_macro(d,"ExecFile(\"rm\")",(unsigned)first)==-ENOSYS);
        assert(hf_macro(d,"JI(\"other.hlp\",\"first\")",0)==-ENOSYS);
        assert(hf_macro(d,"JI(\"\",\"unknown\")",0)==-ENOENT);
        assert(hf_macro(d,"Contents() garbage",0)==-EINVAL);
    } else {
        assert(!strcmp(d->pages[first].title,"Welcome page"));
        assert(d->toc_count==3 && d->toc[0].page<0 && d->toc[1].depth==1 && d->toc[2].depth==1);
        assert(strstr(d->pages[first].text,"Authored & readable.") && !strstr(d->pages[first].text,"hidden script"));
        assert(d->pages[first].links[0].end>d->pages[first].links[0].start);
    }
    /* Truncations, oversized expansion and deterministic mutations are all
     * untrusted input. Even an accepted mutation must remain bounds-safe. */
    for (size_t n=0;n<d->input_size;n+=d->input_size/13+1) {
        struct hf_document *bad=NULL; e=hf_open(&bad,d->input,n); if (!e) hf_close(bad);
    }
    unsigned char *copy=malloc(d->input_size); assert(copy);
    for (unsigned i=0;i<120;i++) {
        memcpy(copy,d->input,d->input_size); size_t pos=(i*7919u+7)%d->input_size;
        copy[pos]^=(unsigned char)(i+1);
        struct hf_document *bad=NULL; e=hf_open(&bad,copy,d->input_size);
        if (!e) { for (unsigned k=0;k<bad->count;k++) (void)hf_page_load(bad,k); hf_close(bad); }
    }
    free(copy); hf_close(d);
}
static void request_codec(void)
{
    struct hf_document *oversize=NULL;
    assert(hf_open(&oversize,NULL,HF_INPUT_LIMIT+1)==-EFBIG && !oversize);
    char out[161]; struct desk_help_request r;
    assert(desk_help_encode(out,1,1,42,"/Program Files/Example/test.hlp",""));
    assert(desk_help_decode(out,&r) && r.format==1 && r.command==1 && r.context==42 && !strcmp(r.path,"/Program Files/Example/test.hlp"));
    assert(desk_help_encode(out,2,0,0,"/test.chm","sub/second.htm#anchor"));
    assert(desk_help_decode(out,&r) && !strcmp(r.query,"sub/second.htm#anchor"));
    assert(desk_help_encode(out,1,2,0,"",""));
    assert(!desk_help_decode("hlp:1:4294967296:1:a",&r));
    assert(!desk_help_decode("hlp:1:42:9000:x",&r));
    assert(!desk_help_decode("hlp:1:0:2:x",&r));
    assert(!desk_help_decode("chm:0:0:1:a\n",&r));
    assert(!desk_help_decode("hhm:0:0:1:a",&r));
    char long_name[162]; memset(long_name,'a',161); long_name[161]=0;
    assert(!desk_help_encode(out,1,1,0,long_name,""));
}
static void frame(struct du_ui *u)
{
    static unsigned number; number++;
    du_render(u);
    size_t n=(size_t)u->canvas.width*u->canvas.height*4; uint32_t *expected=malloc(n); assert(expected);
    memcpy(expected,u->canvas.pixels,n); u->dirty=1; du_render(u);
    if (memcmp(expected,u->canvas.pixels,n)) {
        struct help_app *a=u->user;
        for (size_t i=0;i<n/4;i++) if (expected[i]!=u->canvas.pixels[i]) { fprintf(stderr,"frame %u topic %d wizard %d step %d differs at %zu,%zu: %x / %x\n",number,a->topic,a->wizard,a->wizard_step,i%(size_t)u->canvas.width,i/(size_t)u->canvas.width,expected[i],u->canvas.pixels[i]); break; }
        abort();
    }
    free(expected);
}
static void user_interface(const char *path)
{
    struct help_app *a=calloc(1,sizeof(*a)); struct du_ui *u=calloc(1,sizeof(*u));
    uint32_t *pixels=calloc(HELP_W*HELP_H,4); assert(a && u && pixels);
    du_init(u,pixels,HELP_W,HELP_H); assert(!help_attach(a,u)); frame(u);
    int first=a->topic;
    assert(!help_follow(a,"topic:nosound")); assert(a->topic==help_topic_find("nosound")); frame(u);
    help_back(a); assert(a->topic==first); frame(u);
    for (unsigned kind=0;help_wizard_topic(kind);kind++) {
        help_wizard_begin(a,(int)kind); assert(a->wizard && a->wizard_step==0); frame(u);
        assert(du_get(u,H_WIZ_NEXT)->disabled);
        help_wizard_action(a,H_WIZ_NO); help_wizard_action(a,H_WIZ_NEXT); assert(a->wizard_step==2); frame(u);
        help_wizard_action(a,H_WIZ_BACK); assert(a->wizard_step==0); frame(u);
        help_wizard_action(a,H_WIZ_YES); help_wizard_action(a,H_WIZ_NEXT); assert(a->wizard_step==1); frame(u);
        help_wizard_action(a,H_WIZ_NO); help_wizard_action(a,H_WIZ_NEXT); assert(a->wizard_step==4); frame(u);
        help_wizard_action(a,H_WIZ_CANCEL); assert(!a->wizard); frame(u);
        help_wizard_begin(a,(int)kind); help_wizard_action(a,H_WIZ_YES); help_wizard_action(a,H_WIZ_NEXT);
        help_wizard_action(a,H_WIZ_YES); help_wizard_action(a,H_WIZ_NEXT); assert(a->wizard_step==3);
        help_wizard_action(a,H_WIZ_NEXT); assert(!a->wizard); frame(u);
    }
    assert(!help_open_argument(a,path)); assert(a->topic==-2 && a->document); frame(u);
    unsigned rows=a->row_count;
    struct du_event toggle={.type=DU_EV_CHANGE,.id=H_TREE,.index=0}; help_event(u,&toggle,a);
    assert(a->row_count<rows); frame(u); help_event(u,&toggle,a); assert(a->row_count==rows); frame(u);
    int source=(int)a->external_page;
    assert(!help_follow(a,"sub/second.htm#target") && (int)a->external_page!=source && a->ta.cursor>0); frame(u);
    assert(help_follow(a,"#missing")==-ENOENT);
    assert(!help_follow(a,"../first.htm") && (int)a->external_page==source); frame(u);
    assert(help_follow(a,"https://example.invalid/")==-ENOSYS);
    assert(help_follow(a,"../../escape.htm")==-EINVAL);
    assert(help_follow(a,"missing.htm")==-ENOENT);
    frame(u);
    help_show_topic(a,help_topic_find("help")); frame(u); help_back(a); assert(a->topic==-2); frame(u);
    help_tab(a,HELP_TAB_INDEX); frame(u);
    du_set_text(du_get(u,H_SEARCH_EDIT),"destination"); help_search_run(a); assert(a->hit_count==1); frame(u);
    char argument[161];
    assert(desk_help_encode(argument,2,15,42,path,""));
    assert(!help_request_current(a,argument)); assert((int)a->external_page!=source); frame(u);
    assert(desk_help_encode(argument,2,2,0,path,"Overview")); assert(!help_request_current(a,argument));
    assert(du_get(u,H_TABS)->active==HELP_TAB_INDEX && !strcmp(du_get(u,H_INDEX_EDIT)->text,"Overview")); frame(u);
    int page=hf_find(a->document,"sub/second.htm"); assert(page>=0);
    char *long_text=malloc(HF_TEXT_LIMIT+31); assert(long_text); memset(long_text,'x',HF_TEXT_LIMIT+30);
    assert(!hf_page_text(a->document,(unsigned)page,long_text,HF_TEXT_LIMIT+30)); free(long_text);
    assert(!help_document_show(a,(unsigned)page) && a->ta.len==HF_TEXT_LIMIT); frame(u);
    assert(!help_follow(a,"part:60000") && a->external_offset==60000 && a->ta.len==30); frame(u);
    assert(!help_follow(a,"part:0") && !a->external_offset); frame(u);
    help_detach(a); free(pixels); free(u); free(a);
}
int main(int argc,char **argv)
{
    if (argc==3 && !strcmp(argv[1],"--real")) {
        struct hf_document *d=NULL; int e=hf_open_file(&d,argv[2]);
        if (e) { fprintf(stderr,"real help open error %d\n",e); return 1; }
        unsigned loaded=0,links=0;
        for (unsigned i=0;i<d->count;i++) { e=hf_page_load(d,i); if (e) { fprintf(stderr,"real help page %u error %d\n",i,e); hf_close(d); return 1; } loaded++; links+=d->pages[i].link_count; }
        printf("PASS owner-local help format=%u topics=%u keywords=%u contexts=%u loaded=%u links=%u\n",d->format,d->count,d->keyword_count,d->context_count,loaded,links);
        hf_close(d); fflush(stdout); return 0;
    }
    assert(argc==2); request_codec();
    const char *names[]={"phrases.hlp","phrases40.hlp","compressed.hlp","stored.chm","lzx.chm"};
    char path[512];
    for (unsigned i=0;i<5;i++) { snprintf(path,sizeof(path),"%s/%s",argv[1],names[i]); check_document(path,i<3 ? 1 : 2); }
    snprintf(path,sizeof(path),"%s/lzx.chm",argv[1]); user_interface(path);
    puts("PASS G15B formats, phrases, contexts, keywords, macros, LZX, links, wizards and frame equivalence"); fflush(stdout); return 0;
}
