/* SPDX-License-Identifier: MIT
 * Exercise the actual min/max and plain-text workers with deterministic time. */
#include <assert.h>
#include "html/layout.c"
#include "text/textplain.c"
extern unsigned test_clock_reads,test_expire_read;
void amendment_layout_checks(struct content *document)
{
    html_content *html=(html_content *)document;
    uint64_t start=ciuki_layout_begin();
    test_expire_read=test_clock_reads+1;
    layout_minmax_block(html->layout,html->font_func,html);
    assert(ciuki_layout_expired());
    assert(!ciuki_layout_end(start,false));
    assert(ciuki_layout_max_ns>=75000000u);
    test_expire_read=0;
    textplain_content text={0};
    char data[4097];memset(data,'x',sizeof(data)-1);data[sizeof(data)-1]=0;
    text.utf8_data=data;text.utf8_data_size=sizeof(data)-1;
    start=ciuki_layout_begin();
    /* entry check + font pre/post + loop: expire in the bounded UTF-8 walk. */
    test_expire_read=test_clock_reads+5;
    assert(!ciuki_text_reformat(&text.base,640,480));
    assert(ciuki_layout_expired() && !text.physical_line_count);
    assert(!ciuki_layout_end(start,false));
    free(text.physical_line);
    test_expire_read=0;
    puts("NetSurf layout: min/max and plain-text workers unwind at deadlines");fflush(stdout);
}
