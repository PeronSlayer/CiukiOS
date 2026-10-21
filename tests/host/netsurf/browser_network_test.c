/* Real NetSurf core/frontend with only the socket OS boundary replaced by
 * f6-10's kernel host seam. SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
extern void fixture_begin(const char *,const char *,const char *),fixture_end(void);
extern unsigned fixture_requests(void),fixture_loops(void);
int __real_main(int,char **);
int __wrap_main(int argc,char **argv)
{
    fixture_begin("browser","-","-");
    int result=__real_main(argc,argv);
    printf("fixture_requests=%u fixture_loop_requests=%u\n",fixture_requests(),fixture_loops());
    fixture_end();return result;
}
