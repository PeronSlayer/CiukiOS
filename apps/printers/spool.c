/* SPDX-License-Identifier: MIT
 * Single desktop-owned background print service. */
#include "../libprint/print.h"
#include <signal.h>
#include <time.h>
static volatile sig_atomic_t running=1;
static void stop(int sig){(void)sig;running=0;}
int main(void)
{
    signal(SIGTERM,stop);signal(SIGINT,stop);
    if(cp_init(NULL) || cp_service_lock(NULL))return 1;
    while(running){int r=cp_service_step(NULL);struct timespec delay={.tv_sec=0,.tv_nsec=r>0?1000000:1000000000};nanosleep(&delay,NULL);}
    cp_service_unlock(NULL);return 0;
}
