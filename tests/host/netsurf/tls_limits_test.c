/* Exercise the certificate-capture proxy independently of parser errors, so
 * malformed ASN.1 cannot mask a missing byte/depth bound.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "sdk/netsurf/port/tls.c"
bool ciuki_host_clock_rtc(void) { return true; }
int main(void)
{
    for(unsigned kind=0;kind<2;kind++){
        enum ciuki_tls_error error;
        struct ciuki_tls *t=ciuki_tls_create("fixture.example",NULL,0,&error);assert(t && !error);
        chain_start(&t->proxy.vtable,"fixture.example");
        if(!kind){
            unsigned char *bytes=calloc(1,32769);assert(bytes);
            cert_start(&t->proxy.vtable,32769);cert_append(&t->proxy.vtable,bytes,32769);cert_end(&t->proxy.vtable);free(bytes);
            assert(!t->der_bytes && !t->depth);
        } else {
            for(unsigned i=0;i<11;i++){cert_start(&t->proxy.vtable,1);cert_append(&t->proxy.vtable,(const unsigned char *)"x",1);cert_end(&t->proxy.vtable);}
            assert(t->depth==10 && t->der_bytes==10);
        }
        assert(t->cert_overflow && chain_end(&t->proxy.vtable)==BR_ERR_X509_LIMIT_EXCEEDED);
        ciuki_tls_free(t);
    }
    puts("PASS certificate capture 32 KiB/10-depth overflow overrides malformed ASN.1");fflush(stdout);return 0;
}
