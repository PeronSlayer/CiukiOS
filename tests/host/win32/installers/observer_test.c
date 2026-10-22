/* Observer grammar and false-positive rejection, not installer execution.
 * SPDX-License-Identifier: MIT */
#include "../../../../src/probes/win32_installers/controller.h"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
static struct installer_observation facts[]={
    {0,0,0,1}, {IF_BASE|IF_DIRECTORY,IF_CONTENT,0,0},
    {IF_BASE|IF_DIRECTORY|IF_REMOVE|IF_INI,IF_CONTENT|IF_REMOVE|IF_INI,0,0},
    {IF_BASE|IF_DIRECTORY|IF_REMOVE|IF_INI|IF_JOURNAL,IF_CONTENT|IF_REMOVE|IF_INI|IF_JOURNAL,0,0},
    {IF_BASE|IF_DIRECTORY|IF_RESTARTED|IF_BACKUP,IF_CONTENT|IF_RESTARTED|IF_BACKUP,0,0},
    {IF_COPY|IF_INI|IF_BACKUP|IF_RESTARTED,IF_COPY|IF_INI|IF_BACKUP|IF_RESTARTED,0,0},
    {IF_COPY|IF_INI|IF_BACKUP|IF_RESTARTED|IF_JOURNAL,IF_COPY|IF_INI|IF_BACKUP|IF_RESTARTED|IF_JOURNAL,0,0},
    {IF_BACKUP|IF_RESTARTED,IF_BACKUP|IF_RESTARTED,0,0}
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
    for(unsigned changed=0;changed<8;changed++)for(unsigned bit=1;bit<=IF_DIRECTORY;bit<<=1){
        if(!(facts[changed].files&bit))continue;
        for(unsigned content=0;content<2;content++){
            if(content && !(facts[changed].contents&bit))continue;
            struct installer_result bad={0};
            for(unsigned i=0;i<8;i++){
                struct installer_observation o=facts[i];
                if(i==changed){if(content)o.contents&=~bit;else o.files&=~bit;}
                assert(installer_observe(&bad,i+1,&o));
            }
            assert(bad.errors[0]|bad.errors[1]|bad.errors[2]|bad.errors[3]);
        }
    }
    for(unsigned progress=1;progress<=2;progress++){
        struct installer_result advanced={0};
        for(unsigned i=0;i<8;i++){
            struct installer_observation o=facts[i];
            if(i==3 || i==6){
                unsigned removed=i==3?IF_REMOVE:IF_COPY;
                o.files&=~removed;o.contents&=~removed;o.contents|=IF_LINE_APPLIED;
                if(progress==2){
                    o.files=(o.files&~IF_INI)|IF_BACKUP;
                    o.contents=(o.contents&~IF_INI)|IF_BACKUP|IF_RETIRED;
                }
            }
            assert(installer_observe(&advanced,i+1,&o));
        }
        assert(advanced.complete && !(advanced.errors[0]|advanced.errors[1]|advanced.errors[2]|advanced.errors[3]));
    }
    installer_publish(&good,emit);
    puts("PASS observer order, corrupt manifest, stale copies, registry error, journal and delayed deletion rejection");return 0;
}
