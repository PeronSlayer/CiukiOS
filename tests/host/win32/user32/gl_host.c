/* Production WGL entry points in the USER/GDI host fixture. No presenter
 * policy is mocked here; only the syscall transport is adapted.
 * SPDX-License-Identifier: MIT */
#include <w32/opengl32.h>
#include <ciuki/present_abi.h>
#include <assert.h>
#include <string.h>
extern DWORD w32r_gate(DWORD, DWORD, DWORD, DWORD);
int w32_strcmp(const char *a, const char *b) { return strcmp(a,b); }
int32_t gl_host_display_frame(struct ciuki_w32_display_args *a, const void *payload)
{
    (void)payload;
    return (int32_t)w32r_gate(CIUKI_SYS_W32,(DWORD)(uintptr_t)a,sizeof(*a),0);
}
static HGLRC context;
extern BOOL W32API w32_gl_swap(HDC);
BOOL ui_gl_frame(DWORD dc)
{
    if (!context) {
        PIXELFORMATDESCRIPTOR p={.nSize=sizeof(p),.nVersion=1,
            .dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,
            .cColorBits=32,.cDepthBits=16};
        int format=ChoosePixelFormat(dc,&p);
        assert(format && SetPixelFormat(dc,format,&p));
        context=wglCreateContext(dc);
        assert(context && wglMakeCurrent(dc,context));
    }
    w32_glClearColor(1,0,0,1);
    w32_glClear(GL_COLOR_BUFFER_BIT);
    return w32_gl_swap(dc);
}
void ui_gl_close(void)
{
    assert(wglMakeCurrent(0,0) && wglDeleteContext(context));
    context=0;
}
