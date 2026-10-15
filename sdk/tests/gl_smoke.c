/* Static link/ABI smoke; guest execution belongs to the lead. SPDX-License-Identifier: MIT */
#include <GL/gl.h>
#include <ciuki/gl.h>
#include <stdio.h>
static uint32_t pixels[64*48];
int main(void)
{
    if (ciuki_gl_bind(pixels,64,48,64)) return 1;
    glClearColor(1,0,0,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    ciuki_gl_finish();
    int failed=pixels[24*64+32]!=0xff0000 || glGetError()!=GL_NO_ERROR;
    ciuki_gl_close();puts(failed ? "GL smoke FAIL" : "GL smoke PASS");return failed;
}
