/* Shared USER/dialog harness pixel-format bridge. */
#include <w32/gdi32.h>
#include <w32/pixelformat.h>
int host_pixel_format(DWORD dc, int set) {
  return set ? SetPixelFormat(dc,set,0) : GetPixelFormat(dc);
}
