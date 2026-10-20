/* Pinned system-face names exposed in the writable Windows fonts folder.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_W32_FONTS_H
#define CIUKI_W32_FONTS_H
/* Use the VFS case folding at the call site; aliases are resolved first. */
static inline bool w32_system_font_name(const char *name) {
  static const char *const names[]={
    "LiberationSans-Regular.ttf","LiberationSans-Bold.ttf",
    "LiberationSans-Italic.ttf","LiberationSans-BoldItalic.ttf",
    "LiberationSerif-Regular.ttf","LiberationSerif-Bold.ttf",
    "LiberationSerif-Italic.ttf","LiberationSerif-BoldItalic.ttf",
    "LiberationMono-Regular.ttf","LiberationMono-Bold.ttf",
    "LiberationMono-Italic.ttf","LiberationMono-BoldItalic.ttf"};
  for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++)if(path_equal(name,names[i]))return true;
  return false;
}
#endif
