/* Startup selection precedes all EXE/DLL callbacks. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shims.h>
int w32_shim_start(struct w32_loader *l,const char *exe){
    const struct w32_services *s=l->services;
    w32_fill(&w32_shims,0,sizeof(w32_shims));
    struct w32_shim_identity id={.exe=exe};
    if(!s->fingerprint)return 1; /* structural adapters without a kernel identity */
    if(!s->fingerprint(s->opaque,&id))return 0;
    struct w32_shim_match selected={0},override={0};
    struct w32_file f={0};
    if(s->open(s->opaque,"C:\\System\\Win32\\SHIMS.INI",&f)){
        if(!w32_shim_parse(f.bytes,f.size,&id,&selected))w32_diag("warning","shims","invalid-database");
        s->close(s->opaque,&f);
    }
    if(s->compat_database){
        uint8_t *text=s->alloc(s->opaque,W32_SHIM_TEXT_MAX);uint32_t bytes=0;
        if(!text)return 0;
        int result=s->compat_database(s->opaque,text,W32_SHIM_TEXT_MAX,&bytes);
        if(result>0){if(!w32_shim_parse(text,bytes,&id,&override))w32_diag("warning","shims","invalid-override");}
        else if(result<0)w32_diag("warning","shims","override-unavailable");
        s->free(s->opaque,text,W32_SHIM_TEXT_MAX);
    }
    if(selected.conflicts||override.conflicts||(selected.rank&&override.rank))w32_diag("warning","shims","match-conflict");
    w32_shims=override.rank?override.settings:selected.settings;
    if(w32_shims.mask&(W32_SHIM_BIT(W32_SH_DDRAW_555)|W32_SHIM_BIT(W32_SH_DDRAW_SCALE)|W32_SHIM_BIT(W32_SH_DDRAW_WINDOWED)))
        w32_diag("warning","ddraw.dll","shim-hook-unavailable");
    return !s->configure||s->configure(s->opaque,&w32_shims,&id);
}
void w32_report_exception(struct w32_exception *r,struct w32_context *c){
    const struct w32_services *s=w32_get_services();
    /* Corrupted loader lists must not fault again while reporting a fault.
     * The kernel resolves the copied EIP through safe user reads. */
    if(s&&s->report)s->report(s->opaque,W32_REPORT_CRASH,r->ExceptionCode,c,"unknown",0);
    else w32_diag("crash","exception","unhandled");
}
void w32_report_refusal(uint32_t reason,const char *name,uint32_t code){
    const struct w32_services *s=w32_get_services();
    if(s&&s->report)s->report(s->opaque,W32_REPORT_REFUSE,reason,0,name,code);
}
