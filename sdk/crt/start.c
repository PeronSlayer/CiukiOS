#include <ciuki/runtime.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
extern int main(int,char **,char **);
extern char **environ;
extern void (*__preinit_array_start[])(void),(*__preinit_array_end[])(void);
extern void (*__init_array_start[])(void),(*__init_array_end[])(void);
extern void (*__fini_array_start[])(void),(*__fini_array_end[])(void);
static struct _reent main_reent;
struct _reent *__getreent(void) {
    struct _reent *r; __asm__ volatile("movl %%gs:%c1,%0":"=r"(r):"i"(__builtin_offsetof(struct ciuki_tcb,reent))); return r;
}
/* fx-autorun: one console line per record for every native program. The
 * kernel publishes each write() to the console as its own "[console] " line
 * (src/kernel/proc/devnodes.c), and newlib opens stderr unbuffered
 * (findfp.c stderr_init: __SRW|__SNBF), so vfprintf wrote each conversion
 * separately and one fprintf record arrived split over several serial
 * lines (fx-acc1, acceptance run f63df5ab). ISO C 7.21.3p7 requires only
 * that stderr is not fully buffered: it becomes line buffered, flushed at
 * each newline, before input on a line-buffered or unbuffered stream
 * (newlib __srefill) and at exit. stdout, fully buffered by newlib unless
 * isatty(), is line buffered as well when it is a character device (any
 * terminal is one): the CiukiOS console/serial log (isatty is 0 by
 * contract, posix-subset.md), a COM/LPT port or /dev/null; pipes and files
 * stay fully buffered. Only the mode bits change here: the buffer is still
 * allocated lazily by __smakebuf_r on the first write, which keeps a preset
 * __SLBF (the state stdout_init gives stdout on ports without fcntl). A
 * program's own setvbuf replaces this choice. errno is zero at startup
 * (ISO C 7.5p3), so the probe's errno is not kept. */
static void stdio_line_buffering(void) {
    int saved=errno;
    stderr->_flags=(short)((stderr->_flags&~__SNBF)|__SLBF);
    struct stat s;
    if (!fstat(1,&s) && S_ISCHR(s.st_mode)) stdout->_flags=(short)(stdout->_flags|__SLBF);
    errno=saved;
}
static void finish(void) {
    for (void (**p)(void)=__fini_array_end;p!=__fini_array_start;) (*--p)();
}
void __ciuki_start(uint32_t *stack) {
    int argc=*stack; char **argv=(char **)(stack+1);
    environ=argv+argc+1;
    char **end=environ; while (*end) ++end;
    uint32_t *aux=(uint32_t *)(end+1); struct ciuki_tcb *t=NULL;
    for (;aux[0]!=AT_NULL;aux+=2) if(aux[0]==AT_CIUKI_TLS) t=(void *)(uintptr_t)aux[1];
    if (!t || t!=ciuki_tcb()) { CU_CALL(EXIT,127,0,0,0,0,0); __builtin_trap(); }
    _REENT_INIT_PTR(&main_reent);
    t->reent=CU_PTR(&main_reent);
    __ciuki_pthread_main();
    extern void __sinit(struct _reent *);
    __sinit(&main_reent);
    stdio_line_buffering();
    atexit(finish);
    for(void (**p)(void)=__preinit_array_start;p!=__preinit_array_end;++p) (*p)();
    for(void (**p)(void)=__init_array_start;p!=__init_array_end;++p) (*p)();
    exit(main(argc,argv,environ));
}
void _exit(int status) { CU_CALL(EXIT,(unsigned)status & 255,0,0,0,0,0); __builtin_trap(); }
void _Exit(int status) { _exit(status); }
