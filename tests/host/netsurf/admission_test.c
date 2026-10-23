/* SPDX-License-Identifier: MIT
 * fx-14: the binding admits ordinary pages against the memory budget
 * (sdk/netsurf/port/docmem.h) and refuses only an exceeded budget or
 * excessive nesting, before allocating. Arguments: page files that must be
 * admitted (tests/host/netsurf/pages.py), then "--" and pages that must be
 * refused for memory under the CIUKI_NETSURF_FREE_BYTES of the run. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/allocator_interface.h>
#include "allocation_fail.h"
#include "parser.c"
static void quiet(uint32_t severity,void *context,const char *message,...)
{ (void)severity;(void)context;(void)message; }
static dom_hubbub_parser *new_parser(dom_document **document)
{
    dom_hubbub_parser *p=NULL;
    dom_hubbub_parser_params params={.enc="UTF-8",.fix_enc=true,.enable_script=false,.msg=quiet};
    assert(dom_hubbub_parser_create(&params,&p,document)==DOM_HUBBUB_OK && p);
    return p;
}
static char *slurp(const char *path,size_t *size)
{
    FILE *f=fopen(path,"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>=0);rewind(f);
    char *data=malloc((size_t)n+1);assert(data);
    assert(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);data[n]=0;*size=(size_t)n;
    return data;
}
/* NetSurf feeds the parser in network/file slices; 16 KiB like the fetcher. */
static dom_hubbub_error feed(dom_hubbub_parser *p,const char *data,size_t size)
{
    dom_hubbub_error err=DOM_HUBBUB_OK;
    for(size_t at=0;at<size && err==DOM_HUBBUB_OK;at+=16384)
        err=dom_hubbub_parser_parse_chunk(p,(const uint8_t *)data+at,size-at<16384?size-at:16384);
    return err==DOM_HUBBUB_OK?dom_hubbub_parser_completed(p):err;
}
static unsigned count_nodes(dom_node_internal *n,unsigned *script_children)
{
    unsigned count=0;
    for(dom_node_internal *c=n->first_child;c;c=c->next) {
        if(c->type==DOM_ELEMENT_NODE && c->name && !strcasecmp(dom_string_data(c->name),"script") && c->first_child)
            ++*script_children;
        count+=1+count_nodes(c,script_children);
    }
    return count;
}
static void admit(const char *path)
{
    size_t size;char *data=slurp(path,&size);
    size_t before=__sanitizer_get_current_allocated_bytes();
    dom_document *document=NULL;dom_hubbub_parser *p=new_parser(&document);
    assert(feed(p,data,size)==DOM_HUBBUB_OK && !p->ciuki_rejected && !dom_hubbub_parser_ciuki_reason(p));
    size_t charged=p->ciuki_charged,budget=p->ciuki_budget;
    dom_hubbub_parser_destroy(p);
    /* The DOM that stays after parsing (host x86-64: wider than i686). */
    size_t live=__sanitizer_get_current_allocated_bytes()-before;
    unsigned scripts=0,nodes=count_nodes((dom_node_internal *)document,&scripts);
    printf("admitted %s bytes=%zu nodes=%u charged=%zu live_dom=%zu live_per_node=%zu budget=%zu\n",
           path,size,nodes,charged,live,nodes?live/nodes:0,budget);
    fflush(stdout);
    assert(!scripts);              /* script text was dropped */
    assert(live<=charged);         /* the ledger is conservative */
    assert(charged<=budget);
    dom_node_unref(document);free(data);
}
static void refuse(const char *path)
{
    size_t size;char *data=slurp(path,&size);
    dom_document *document=NULL;dom_hubbub_parser *p=new_parser(&document);
    assert(feed(p,data,size)==DOM_HUBBUB_NOMEM);
    assert(dom_hubbub_parser_ciuki_reason(p)==CIUKI_DOC_MEMORY && p->ciuki_charged<=p->ciuki_budget);
    printf("refused %s bytes=%zu charged=%zu budget=%zu\n",path,size,p->ciuki_charged,p->ciuki_budget);
    dom_hubbub_parser_destroy(p);dom_node_unref(document);free(data);
}
static void parse_text(const char *text,dom_hubbub_error expected,int reason)
{
    dom_document *document=NULL;dom_hubbub_parser *p=new_parser(&document);
    assert(feed(p,text,strlen(text))==expected);
    assert(dom_hubbub_parser_ciuki_reason(p)==reason);
    dom_hubbub_parser_destroy(p);dom_node_unref(document);
}
static char *nest(unsigned depth)
{
    char *s=malloc(depth*11+16);assert(s);char *at=s;
    for(unsigned i=0;i<depth;i++)at+=sprintf(at,"<div>");
    at+=sprintf(at,"x");
    for(unsigned i=0;i<depth;i++)at+=sprintf(at,"</div>");
    return s;
}
int main(int argc,char **argv)
{
    int i=1;
    for(;i<argc && strcmp(argv[i],"--");i++)admit(argv[i]);
    for(i++;i<argc;i++)refuse(argv[i]);
    if(getenv("CIUKI_NETSURF_FREE_BYTES"))return 0;  /* the refusal run only */
    /* No per-chunk cap: one 256 KiB text node and one 64 KiB attribute. */
    size_t big=256*1024;char *text=malloc(big+64);assert(text);
    strcpy(text,"<p>");memset(text+3,'x',big);strcpy(text+3+big,"</p>");
    parse_text(text,DOM_HUBBUB_OK,0);
    strcpy(text,"<p title=\"");memset(text+10,'a',65536);strcpy(text+10+65536,"\">x</p>");
    parse_text(text,DOM_HUBBUB_OK,0);
    free(text);
    /* Nesting: 120 levels pass, 200 are refused with their own reason. */
    char *deep=nest(120);parse_text(deep,DOM_HUBBUB_OK,0);free(deep);
    deep=nest(200);parse_text(deep,DOM_HUBBUB_NOMEM,CIUKI_DOC_DEPTH);free(deep);
    dom_node_internal chain[CIUKI_DOM_DEPTH+2]={0};
    for(unsigned n=1;n<CIUKI_DOM_DEPTH+2;n++)chain[n].parent=&chain[n-1];
    assert(ciuki_attach(&chain[CIUKI_DOM_DEPTH-2],&chain[CIUKI_DOM_DEPTH+1]));
    assert(!ciuki_attach(&chain[CIUKI_DOM_DEPTH],&chain[CIUKI_DOM_DEPTH+1]));
    dom_document *doc=NULL;dom_hubbub_parser *p=new_parser(&doc);
    assert(p->ciuki_budget==CIUKI_DOC_MODEL_FREE_BYTES/CIUKI_DOC_FREE_SHARE);
    /* Admission rejects before attempting even the first heap allocation. */
    unsigned before=allocation_count;
    assert(p->ciuki_charged==CIUKI_DOM_DOCUMENT_BYTES);
    p->ciuki_charged=p->ciuki_budget-CIUKI_DOM_NODE_BYTES;void *node=(void *)1;
    hubbub_string one={(const uint8_t *)"x",1};
    fail_allocation=allocation_count+1;
    assert(create_text(p,&one,&node)==HUBBUB_NOMEM && !node && allocation_count==before);
    assert(dom_hubbub_parser_ciuki_reason(p)==CIUKI_DOC_MEMORY);
    fail_allocation=0;p->ciuki_charged=0;p->ciuki_rejected=false;p->ciuki_reason=0;
    /* Exercise allocation rollback in the admitted text-construction path. */
    allocation_count=0;
    assert(create_text(p,&one,&node)==HUBBUB_OK);dom_node_unref(node);
    unsigned allocations=allocation_count;
    for(unsigned n=1;n<=allocations;n++) {
        allocation_count=0;fail_allocation=n;node=NULL;p->ciuki_charged=0;
        hubbub_error err=create_text(p,&one,&node);
        fail_allocation=0;
        if(err==HUBBUB_OK)dom_node_unref(node);else assert(!node);
    }
    dom_hubbub_parser_destroy(p);dom_node_unref(doc);
    puts("NetSurf construction: ordinary pages admitted against the memory budget, script text dropped, "
         "oversized pages refused for memory, depth-before-attach and text OOM sweep passed");
    fflush(stdout);
}
