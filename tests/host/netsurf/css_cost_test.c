/* SPDX-License-Identifier: MIT
 * Exercise libcss's real selector-chain merge, including the >16-class fallback.
 * A second compilation can include the unmodified pinned source for timings. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "allocation_fail.h"
#ifndef CSS_SELECTION_SOURCE
#define CSS_SELECTION_SOURCE "select/select.c"
#endif
#include CSS_SELECTION_SOURCE
static css_error resolve(void *pw,const char *base,lwc_string *rel,lwc_string **abs)
{ (void)pw;(void)base;*abs=lwc_string_ref(rel);return CSS_OK; }
static css_error has_name(void *pw,void *node,const css_qname *name,bool *match)
{ (void)pw;(void)node;(void)name;*match=true;return CSS_OK; }
static css_error has_class(void *pw,void *node,lwc_string *name,bool *match)
{ (void)pw;(void)node;(void)name;*match=true;return CSS_OK; }
static css_error parent(void *pw,void *node,void **out)
{ (void)pw;(void)node;*out=NULL;return CSS_OK; }
static css_error name_failure(void *pw,void *node,css_qname *out)
{ (void)pw;(void)node;(void)out;return CSS_NOMEM; }
static uint64_t now(void)
{ struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec; }
int main(void)
{
    css_stylesheet_params params={.params_version=CSS_STYLESHEET_PARAMS_VERSION_1,
        .level=CSS_LEVEL_21,.charset="UTF-8",.url="file:fx24b",.resolve=resolve};
    css_stylesheet *sheet=NULL;assert(css_stylesheet_create(&params,&sheet)==CSS_OK);
    const char *text="div {color:#111111} .c0 {color:#123456} .c16 {color:#abcdef}";
    css_error e=css_stylesheet_append_data(sheet,(const uint8_t *)text,strlen(text));assert(e==CSS_OK || e==CSS_NEEDDATA);
    assert(css_stylesheet_data_done(sheet)==CSS_OK);
    css_select_ctx *ctx=NULL;assert(css_select_ctx_create(&ctx)==CSS_OK);
    css_select_state *state=calloc(1,sizeof(*state));assert(state);
    css_select_handler handler={.handler_version=CSS_SELECT_HANDLER_VERSION_1,.node_has_name=has_name,.node_has_class=has_class};
    css_media media={.type=CSS_MEDIA_SCREEN};css_unit_ctx units={0};
    css_bloom bloom[CSS_BLOOM_SIZE]={0};
    struct css_node_data node_data={.bloom=bloom};
    state->handler=&handler;state->node_data=&node_data;state->media=&media;state->unit_ctx=&units;
    assert(lwc_intern_string("div",3,&state->element.name)==lwc_error_ok);
    lwc_string *classes[128];
    for(unsigned i=0;i<128;i++) { char name[12];int n=snprintf(name,sizeof(name),"c%u",i);assert(lwc_intern_string(name,n,&classes[i])==lwc_error_ok); }
    state->classes=classes;assert(css__computed_style_create(&state->computed)==CSS_OK);
    css_select_results results={0};results.styles[CSS_PSEUDO_ELEMENT_NONE]=state->computed;
    state->results=&results;
    static const unsigned counts[]={0,1,16,17,128};
    for(unsigned at=0;at<sizeof(counts)/sizeof(*counts);at++) {
        unsigned count=counts[at];state->n_classes=count;
        assert(match_selectors_in_sheet(ctx,sheet,state)==CSS_OK);
        allocation_count=0;uint64_t start=now();
        for(unsigned i=0;i<10000;i++) {
            memset(state->props,0,sizeof(state->props));
            assert(match_selectors_in_sheet(ctx,sheet,state)==CSS_OK);
        }
        uint64_t elapsed=now()-start;
#ifdef CSS_BASELINE
        unsigned expected=count?10000:0;
#else
        unsigned expected=count>16?10000:0;
#endif
        assert(allocation_count==expected);
        css_color color;assert(css_computed_color(state->computed,&color)==CSS_COLOR_COLOR);
        assert(color==(count>16?0xffabcdefu:count?0xff123456u:0xff111111u));
        printf("PASS FX-24B CSS: classes=%u selections=10000 class_cursor_allocations=%u ns=%llu\n",count,allocation_count,(unsigned long long)elapsed);
        if(count>16) {
            allocation_count=0;fail_allocation=1;
            assert(match_selectors_in_sheet(ctx,sheet,state)==CSS_NOMEM);fail_allocation=0;
        }
    }
    css_computed_style_destroy(state->computed);lwc_string_unref(state->element.name);free(state);
    for(unsigned i=0;i<128;i++)lwc_string_unref(classes[i]);
    assert(css_select_ctx_destroy(ctx)==CSS_OK);assert(css_stylesheet_destroy(sheet)==CSS_OK);
#ifndef CSS_BASELINE
    /* The i686 stack fix retains scratch in the context, including after
     * initialization failures. No partially initialized result escapes. */
    assert(css_select_ctx_create(&ctx)==CSS_OK);
    handler.parent_node=parent;css_select_results *out=NULL;
    allocation_count=0;fail_allocation=1;
    assert(css_select_style(ctx,&handler,&units,&media,NULL,&handler,NULL,&out)==CSS_NOMEM);
    assert(!ctx->spare_state && !out);
    allocation_count=0;fail_allocation=2;
    assert(css_select_style(ctx,&handler,&units,&media,NULL,&handler,NULL,&out)==CSS_NOMEM);
    css_select_state *cached=ctx->spare_state;assert(cached && !out);
    for(unsigned failure=1;failure<=2;failure++) {
        allocation_count=0;fail_allocation=failure;
        assert(css_select_style(ctx,&handler,&units,&media,NULL,&handler,NULL,&out)==CSS_NOMEM);
        assert(ctx->spare_state==cached && !out);
    }
    fail_allocation=0;handler.node_name=name_failure;
    for(unsigned i=0;i<2;i++) {
        assert(css_select_style(ctx,&handler,&units,&media,NULL,&handler,NULL,&out)==CSS_NOMEM);
        assert(ctx->spare_state==cached && !out);
    }
    assert(css_select_ctx_destroy(ctx)==CSS_OK);
    puts("PASS FX-24B CSS: reusable heap scratch survives allocation failures and is owned by the context");
#endif
    fflush(stdout);return 0;
}
