/* Optional retained-graphics DOS utility runner. Only utilities that use it
 * link this module; its buffers must not consume every CAPP data segment. */
#include "app.h"

int app_helper(const char *command)
{
    static const char wrapper[] = "\\SYSTEM\\GUIEXEC.COM";
    static u8 tail[128], fcb[16];
    static u16 params[7];
    struct regs r;
    int n = str_len(command);
    if (n > 125) return -1;
    tail[0] = (u8)n;
    mem_copy(tail + 1, command, n); tail[n + 1] = 13;
    params[0] = 0; params[1] = (u16)tail; params[2] = app_seg();
    params[3] = params[5] = (u16)fcb; params[4] = params[6] = app_seg();
    mem_set(&r, 0, sizeof r);
    r.ax = 0x4B00; r.dx = (u16)wrapper; r.bx = (u16)params;
    r.ds = r.es = app_seg();
    if (intr(0x21, &r)) return -(int)r.ax;
    r.ax = 0x4D00;
    intr(0x21, &r);
    return r.ax & 255;
}
