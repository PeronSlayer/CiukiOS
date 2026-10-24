/* Real profile transactions, independently stale browser processes. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sdk/netsurf/port/profile.h"
#include "apps/control/internet_history.h"
static int contains(const char *path,const char *needle)
{
    FILE *f=fopen(path,"r");if(!f)return 0;
    char *line=malloc(32768);assert(line);int found=0;while(fgets(line,32768,f))if(strstr(line,needle))found=1;
    free(line);fclose(f);return found;
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);assert(argc==2 && !ciuki_profile_init(argv[1]));
    int barrier[2];assert(!pipe(barrier));pid_t children[2];
    for(unsigned i=0;i<2;i++) {
        children[i]=fork();assert(children[i]>=0);
        if(!children[i]) {
            close(barrier[1]);char go;assert(read(barrier[0],&go,1)==1);close(barrier[0]);
            for(unsigned n=0;n<8;n++) {
                char url[128];snprintf(url,sizeof(url),"https://window-%u.example/page-%u",i,n);
                assert(!ciuki_profile_visit(url) && !ciuki_profile_history_save());
            }
            ciuki_profile_fini();_exit(0);
        }
    }
    close(barrier[0]);assert(write(barrier[1],"go",2)==2);close(barrier[1]);
    for(unsigned i=0;i<2;i++) { int status;assert(waitpid(children[i],&status,0)==children[i] && WIFEXITED(status) && !WEXITSTATUS(status)); }
    assert(!ciuki_profile_history_save());
    for(unsigned i=0;i<2;i++)for(unsigned n=0;n<8;n++) {
        char url[128];snprintf(url,sizeof(url),"https://window-%u.example/page-%u",i,n);assert(contains(ciuki_profile_path("History.html"),url));
    }
    pid_t child=fork();assert(child>=0);
    if(!child) { struct internet_profile_lock lock;assert(!internet_profile_lock(argv[1],&lock));_exit(0); }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    struct internet_profile_lock lock;assert(!internet_profile_lock(argv[1],&lock));internet_profile_unlock(&lock);
    assert(!ciuki_profile_visit("https://old-pending.example/") && !internet_history_clear(argv[1]));
    assert(!ciuki_profile_history_save() && !contains(ciuki_profile_path("History.html"),"old-pending"));
    assert(!ciuki_profile_visit("https://new-after-clear.example/") && !ciuki_profile_history_save());
    assert(contains(ciuki_profile_path("History.html"),"new-after-clear") && !contains(ciuki_profile_path("History.html"),"window-"));
    ciuki_profile_history_days(0);assert(!ciuki_profile_history_save());
    assert(!contains(ciuki_profile_path("History.html"),"href="));ciuki_profile_fini();
    puts("PASS G26B two stale browser processes merge 16 visits; dead-owner lock recovery; cross-window clear cannot resurrect pending history; zero-day policy");return 0;
}
