#include <ciuki/runtime.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
extern int main(int,char **,char **);
extern char **environ;
extern void (*__preinit_array_start[])(void),(*__preinit_array_end[])(void);
extern void (*__init_array_start[])(void),(*__init_array_end[])(void);
extern void (*__fini_array_start[])(void),(*__fini_array_end[])(void);
static struct _reent main_reent;
struct _reent *__getreent(void) {
    struct _reent *r; __asm__ volatile("movl %%gs:%c1,%0":"=r"(r):"i"(__builtin_offsetof(struct ciuki_tcb,reent))); return r;
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
    atexit(finish);
    for(void (**p)(void)=__preinit_array_start;p!=__preinit_array_end;++p) (*p)();
    for(void (**p)(void)=__init_array_start;p!=__init_array_end;++p) (*p)();
    exit(main(argc,argv,environ));
}
void _exit(int status) { CU_CALL(EXIT,(unsigned)status & 255,0,0,0,0,0); __builtin_trap(); }
void _Exit(int status) { _exit(status); }
