/* Publisher/rejection dry-run; these values are synthetic, never guest evidence. MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../../src/probes/win32_core/controller.h"
static unsigned records;
static void emit(const char *probe,const char *event,const char *format,...)
{
    va_list args;printf("%s %s ",probe,event);
    va_start(args,format);vprintf(format,args);va_end(args);putchar('\n');records++;
}
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);
    assert(argc==2);unsigned id=(unsigned)strtoul(argv[1],0,10);
    assert(id>0 && id<W32_FIXTURE_COUNT);
    struct w32_core_result r={.magic=W32_CORE_MAGIC,.version=1,.done=1,.fixture=id};
    unsigned restarts=id==W32_FIXTURE_REGISTRY ? 2u : 0u;
    r.request=r.acknowledged=restarts;
    const struct w32_core_schema *s=&w32_core_schemas[id];
    for(unsigned i=0;i<s->rows;i++) {
        r.row[i].checked=1;
        for(unsigned j=0;j<s->row[i].fields;j++)r.row[i].value[j]=s->row[i].field[j].expected;
    }
    assert(w32_core_valid(&r,id) && w32_core_pass(&r,id,0));
    assert(!w32_core_pass(&r,id,1));
    r.control_error=5;assert(!w32_core_pass(&r,id,0));r.control_error=0;
    r.first_error=42;assert(!w32_core_pass(&r,id,0));r.first_error=0;
    r.request=1;assert(!w32_core_pass(&r,id,0));r.request=restarts;
    if(restarts) {
        r.request=r.acknowledged=0;assert(!w32_core_pass(&r,id,0));
        r.request=r.acknowledged=1;assert(!w32_core_pass(&r,id,0));
        r.request=r.acknowledged=restarts;
    }
    w32_core_publish(&r,id,emit);assert(records==s->rows);
    for(unsigned i=0;i<s->rows;i++) {
        r.row[i].checked=0;assert(!w32_core_pass(&r,id,0));r.row[i].checked=1;
        r.row[i].checked=2;assert(!w32_core_pass(&r,id,0));r.row[i].checked=1;
        r.row[i].errors=1;assert(!w32_core_pass(&r,id,0));r.row[i].errors=0;
        r.unsupported=1u<<i;assert(!w32_core_pass(&r,id,0));r.unsupported=0;
        for(unsigned j=0;j<s->row[i].fields;j++) {
            const struct w32_core_field *f=&s->row[i].field[j];
            r.row[i].value[j]=f->compare>0 ? f->expected-1 : f->expected+1;
            assert(!w32_core_pass(&r,id,0));r.row[i].value[j]=f->expected;
        }
    }
    r.magic=0;assert(!w32_core_valid(&r,id));r.magic=W32_CORE_MAGIC;
    r.version=2;assert(!w32_core_valid(&r,id));r.version=1;
    r.done=0;assert(!w32_core_valid(&r,id));r.done=2;assert(!w32_core_valid(&r,id));r.done=1;
    r.release=1;assert(!w32_core_valid(&r,id));r.release=0;
    r.fixture=W32_FIXTURE_COUNT;assert(!w32_core_valid(&r,id));r.fixture=id;
    r.unsupported=1u<<31;assert(!w32_core_valid(&r,id));r.unsupported=0;
    assert(!w32_core_valid(&r,0) && !w32_core_valid(&r,W32_FIXTURE_COUNT));
    return 0;
}
