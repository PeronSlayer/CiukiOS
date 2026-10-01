/* CiukiOS software OpenGL proof: TinyGL renders in DOS extended memory,
 * then this program presents the 16-bit image through VGA mode 13h. */
#include <conio.h>
#include <i86.h>
#include <GL/gl.h>
#include "ciukgl.h"

#define WIDTH 320
#define HEIGHT 200

static void video_mode(int mode)
{
    union REGS r;
    r.w.ax = mode;
    int386(0x10, &r, &r);
}

static void palette(void)
{
    int i;
    outp(0x3C8, 0);
    for (i = 0; i < 256; ++i) {
        outp(0x3C9, ((i >> 5) & 7) * 63 / 7);
        outp(0x3C9, ((i >> 2) & 7) * 63 / 7);
        outp(0x3C9, (i & 3) * 63 / 3);
    }
}

static void serial(const char *s)
{
    while (*s) {
        while (!(inp(0x3FD) & 0x20)) { }
        outp(0x3F8, *s++);
    }
}

int main(void)
{
    ciukgl_context *context = ciukgl_create(WIDTH, HEIGHT);
    const unsigned short *pixels;
    volatile unsigned char *vga = (volatile unsigned char *)0xA0000UL;
    int x, y;
    if (!context) return 2;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -0.625, 0.625, 1.0, 10.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -3.0f);
    glClearColor(0.04f, 0.08f, 0.20f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glShadeModel(GL_SMOOTH);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.1f, 0.1f); glVertex3f(-1.1f, -0.8f, 0.0f);
    glColor3f(0.1f, 1.0f, 0.1f); glVertex3f(1.1f, -0.8f, 0.0f);
    glColor3f(0.1f, 0.2f, 1.0f); glVertex3f(0.0f, 0.9f, 0.0f);
    glEnd();
    pixels = ciukgl_pixels(context);

    video_mode(0x13);
    palette();
    for (y = 0; y < HEIGHT; ++y)
        for (x = 0; x < WIDTH; ++x) {
            unsigned short pixel = pixels[y * WIDTH + x];
            vga[y * WIDTH + x] = (unsigned char)
                (((pixel >> 13) << 5) | (((pixel >> 8) & 7) << 2) | ((pixel >> 3) & 3));
        }
    serial("[CIUKGL] FRAME READY\r\n");
    while (getch() != 27) { }   /* the launch key may still be queued */
    video_mode(3);
    ciukgl_destroy(context);
    return 0;
}
