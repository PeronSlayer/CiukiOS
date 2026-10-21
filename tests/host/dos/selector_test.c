/* Production parser, not a Python duplicate. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <string.h>
#include "../../../src/kernel/probes/selector.h"
int main(void) {
    struct probe_selection s, guard;
    memset(&guard,0xa7,sizeof(guard));
    const char *good="f3:dos-conformance run=1234aBcD guest=freedos";
    assert(probes_parse_selector(good,strlen(good),0,&s));
    assert(s.phase==3 && !strcmp(s.run,"1234aBcD") && (s.flags&PROBE_DOS_FREEDOS));
    good="f3:dos-conformance run=12345678 guest=ciukidos";
    assert(probes_parse_selector(good,strlen(good),0,&s) && !s.flags);
    good="f3:dos-conformance run=12345678 guest=ciukidos kernel=unified";
    assert(probes_parse_selector(good,strlen(good),0,&s) && s.flags==PROBE_DOS_UNIFIED);
    const char *bad[]={"f3:dos-conformance run=12345678", "f3:dos-conformance run=12345678 guest=other",
        "f3:dos-conformance run=12345678 guest=freedos guest=ciukidos", "f3:dos-conformance run=12345678 guest=freedos ",
        "f3:dos-conformance run=12345678 guest=freedos x=1", "f3:dos-conformance run=1234567g guest=freedos",
        "f3:dos-conformance run=12345678 guest=freedos kernel=unified",
        "f3:dos-conformance run=12345678 guest=ciukidos kernel=master"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        s=guard; assert(!probes_parse_selector(bad[i],strlen(bad[i]),0,&s));
        assert(!memcmp(&s,&guard,sizeof(s)));
    }
    assert(!probes_parse_selector(0,8,0,&s));
    assert(!probes_parse_selector(good,strlen(good),0,0));
}
