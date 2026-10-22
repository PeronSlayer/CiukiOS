/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/ldr.h>
void w32_diag(const char *a,const char *b,const char *c) { (void)a; (void)b; (void)c; }
const struct w32_services *w32_get_services(void) { return 0; }
int user_accepts(const void *bytes,uint32_t size) {
    struct w32_pe pe; struct w32_error error={0};
    return w32_pe_parse(&pe,bytes,size,&error);
}
