/* Required file-queue callback ownership for the authored INF caller.
 * The reused setupapi still performs every installation operation.
 * SPDX-License-Identifier: MIT */
typedef DWORD (W32API *inf_file_callback)(void *,DWORD,uintptr_t,uintptr_t);
typedef BOOL (W32API *inf_install)(HANDLE,HANDLE,const char *,DWORD,HANDLE,const char *,DWORD,
                                 inf_file_callback,void *,HANDLE,void *);
struct inf_queue_api {
    void *(W32API *init)(HANDLE);
    inf_file_callback callback;
    void (W32API *term)(void *);
};
enum { FIXTURE_SPINST_INIFILES=0x2, FIXTURE_SPINST_REGISTRY=0x4,
       FIXTURE_SPINST_FILES=0x10 };
static int install_sections(HANDLE hinf,const char *tree,inf_install install,const struct inf_queue_api *queue){
    void *context=queue->init(0);if(!context)return 27;
    const DWORD flags[]={FIXTURE_SPINST_FILES,FIXTURE_SPINST_REGISTRY,FIXTURE_SPINST_INIFILES};int error=0;
    for(unsigned i=0;i<3;i++){
        if(!install(0,hinf,"DefaultInstall",flags[i],(HANDLE)(uintptr_t)0x80000002u,tree,0,
                    flags[i]==FIXTURE_SPINST_FILES?queue->callback:0,flags[i]==FIXTURE_SPINST_FILES?context:0,0,0)){
            error=8+(int)i;break;
        }
    }
    queue->term(context);return error;
}
