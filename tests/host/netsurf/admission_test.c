/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
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
static void parse_limit(const char *unit,unsigned count,const char *end)
{
    dom_document *document=NULL;dom_hubbub_parser *p=new_parser(&document);
    dom_hubbub_error err=DOM_HUBBUB_OK;
    for(unsigned i=0;i<count && err==DOM_HUBBUB_OK;i++)
        err=dom_hubbub_parser_parse_chunk(p,(const uint8_t *)unit,strlen(unit));
    if(err==DOM_HUBBUB_OK)err=dom_hubbub_parser_parse_chunk(p,(const uint8_t *)end,strlen(end));
    if(err==DOM_HUBBUB_OK)err=dom_hubbub_parser_completed(p);
    assert(err!=DOM_HUBBUB_OK);
    assert(p->ciuki_nodes<=8192 && p->ciuki_text<=131072);
    dom_hubbub_parser_destroy(p);dom_node_unref(document);
}
int main(void)
{
    parse_limit("<div>",20000,"x");
    parse_limit("<p>x</p>",10000,"");
    parse_limit("xxxxxxxxxxxxxxxx",10000,"");
    char attribute[8300],doctype[8400];
    memset(attribute,'a',sizeof(attribute));memcpy(attribute,"<p ",3);
    strcpy(attribute+sizeof(attribute)-5,"=x>");
    parse_limit(attribute,1,"");
    memset(doctype,'x',sizeof(doctype));memcpy(doctype,"<!DOCTYPE html PUBLIC \"",23);
    strcpy(doctype+sizeof(doctype)-6,"\" \"\">");
    parse_limit(doctype,1,"");
    dom_node_internal chain[66]={0};
    for(unsigned i=1;i<66;i++)chain[i].parent=&chain[i-1];
    assert(!ciuki_attach(&chain[64],&chain[65]));
    dom_document *doc=NULL;dom_hubbub_parser *p=new_parser(&doc);
    /* Admission rejects before attempting even the first heap allocation. */
    unsigned before=allocation_count;
    p->ciuki_nodes=8192;void *node=(void *)1;
    hubbub_string text={(const uint8_t *)"x",1};
    fail_allocation=allocation_count+1;
    assert(create_text(p,&text,&node)==HUBBUB_NOMEM && !node && allocation_count==before);
    fail_allocation=0;p->ciuki_nodes=0;p->ciuki_text=0;p->ciuki_rejected=false;
    /* Exercise allocation rollback in the admitted text-construction path. */
    allocation_count=0;
    assert(create_text(p,&text,&node)==HUBBUB_OK);dom_node_unref(node);
    unsigned allocations=allocation_count;
    for(unsigned i=1;i<=allocations;i++) {
        allocation_count=0;fail_allocation=i;node=NULL;
        p->ciuki_nodes=0;p->ciuki_text=0;
        hubbub_error err=create_text(p,&text,&node);
        fail_allocation=0;
        if(err==HUBBUB_OK)dom_node_unref(node);else assert(!node);
    }
    dom_hubbub_parser_destroy(p);dom_node_unref(doc);
    puts("NetSurf construction: deep/wide/text parser admission, depth-before-attach and text OOM sweep passed");
    fflush(stdout);
}
