/* Observer grammar and false-positive rejection, not installer execution.
 * SPDX-License-Identifier: MIT */
#include "../../../../src/probes/win32_installers/controller.h"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
static struct installer_observation facts[]={
    {0,0,0,1}, {IF_BASE|IF_DIRECTORY,IF_CONTENT,0,0},
    {IF_BASE|IF_DIRECTORY|IF_REMOVE|IF_INI,IF_CONTENT|IF_INI,0,0},
    {IF_JOURNAL,0,0,0}, {IF_BASE|IF_DIRECTORY|IF_RESTARTED|IF_BACKUP,IF_CONTENT,0,0},
    {IF_COPY|IF_INI|IF_BACKUP,IF_INI,0,0}, {IF_JOURNAL,0,0,0}, {IF_BACKUP,0,0,0}
};
static void emit(const char *probe,const char *event,const char *format,...){
    printf("RECORD %s %s ",probe,event);va_list args;va_start(args,format);vprintf(format,args);va_end(args);puts("");
}
int main(void){
    struct installer_result good={0};
    for(unsigned i=0;i<8;i++)assert(installer_observe(&good,i+1,&facts[i]));
    assert(good.complete && !(good.errors[0]|good.errors[1]|good.errors[2]|good.errors[3]));
    assert(!installer_observe(&good,8,&facts[7]));
    for(unsigned changed=0;changed<8;changed++){
        struct installer_result bad={0};
        for(unsigned i=0;i<8;i++){
            struct installer_observation o=facts[i];if(i==changed)o.registry_errors=1;
            assert(installer_observe(&bad,i+1,&o));
        }
        assert(bad.errors[0]|bad.errors[1]|bad.errors[2]|bad.errors[3]);
    }
    for(unsigned mutation=0;mutation<6;mutation++){
        struct installer_result bad={0};
        for(unsigned i=0;i<8;i++){
            struct installer_observation o=facts[i];
            if(mutation==0 && i==0)o.files|=IF_PAYLOAD;
            if(mutation==1 && i==1)o.contents&=~IF_DESKTOP;
            if(mutation==2 && i==2)o.contents&=~IF_INI;
            if(mutation==3 && i==3)o.files&=~IF_JOURNAL;
            if(mutation==4 && i==4)o.files|=IF_REMOVE;
            if(mutation==5 && i==7)o.files|=IF_COPY;
            assert(installer_observe(&bad,i+1,&o));
        }
        assert(bad.errors[0]|bad.errors[1]|bad.errors[2]|bad.errors[3]);
    }
    installer_publish(&good,emit);
    puts("PASS observer order, corrupt manifest, stale copies, registry error, journal and delayed deletion rejection");return 0;
}
