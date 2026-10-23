/* SPDX-License-Identifier: MIT
 * fx-14: compiled once, in libdom's hubbub binding (bindings/hubbub/parser.c),
 * so the browser and the binding's host tests share one definition. */
#include <stdio.h>
#include "sdk/netsurf/port/docmem_query.h"
static size_t ciuki_doc_cached;
size_t ciuki_doc_budget_refresh(void)
{
    ciuki_doc_cached=ciuki_docmem_query()/CIUKI_DOC_FREE_SHARE;
    return ciuki_doc_cached;
}
size_t ciuki_doc_budget(void) { return ciuki_doc_cached?ciuki_doc_cached:ciuki_doc_budget_refresh(); }
size_t ciuki_doc_box_limit(void) { return ciuki_doc_budget()/CIUKI_BOX_BYTES; }
const char *ciuki_doc_message(int reason)
{
    switch (reason) {
    case CIUKI_DOC_MEMORY: return "This page is too large for the memory available to the browser.";
    case CIUKI_DOC_DEPTH: return "This page nests its elements more deeply than the browser supports.";
    case CIUKI_DOC_COLUMNS: return "This page has a table with more columns than the browser supports.";
    default: return NULL;
    }
}
void ciuki_doc_report(int reason,size_t charged,size_t budget)
{
    static const char *const cause[]={"admitted","page too large for available memory","nesting depth","table columns"};
    fprintf(stderr,"NetSurf structure rejected: %s (charged=%lu budget=%lu).\n",
            cause[reason>=0 && reason<=CIUKI_DOC_COLUMNS?reason:0],(unsigned long)charged,(unsigned long)budget);
}
