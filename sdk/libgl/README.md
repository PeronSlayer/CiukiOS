# TinyGL for native CiukiOS programs

CiukiOS uses TinyGL by Fabrice Bellard, Gek (DMHSW), and C-Chads.
The retained [licence](../../licenses/TinyGL-LICENSE.txt) requires this
acknowledgment in the product and documentation. The SDK installs that notice
in `licenses/`; the image ships it in `/system/licenses`. Graphics Demo also
displays the acknowledgment. TinyGL is a software OpenGL subset, with no full
OpenGL 1.1 conformance claim.

`vendor/src` and `vendor/include` come from the supplied C-Chads snapshot.
`upstream.json` records original file hashes and the archive hash; `port.json`
records the reviewed local configuration and fixes. Altered files carry a
CiukiOS port note. No example, media, platform glue or prebuilt binary is used.
The portable surface adapter lives outside the vendor tree.

Include `<GL/gl.h>` and `<ciuki/gl.h>`, link with `-lgl` (and `-lm` outside the
SDK wrapper). Call `ciuki_gl_bind(pixels, width, height, stride)` before GL
commands, `ciuki_gl_finish()` before consuming visible pixels, and
`ciuki_gl_close()` on exit. Stride is in pixels. The adapter owns the depth
buffer; pixels belong to the caller. Aligned packed targets need no colour
copy. Other targets use private padded rows. One thread and one context per
process are supported; target replacement retains GL state. Render into
private pixels until libdeskui acknowledges any submitted shared surface.

The offline recipe in `build.py` is used by `sdk/build_sdk.py` and host tests.
It verifies every imported/ported source before compiling sequentially into
`libgl.a`. Headers are installed into the SDK sysroot. See
[the design and measurement record](../../docs/design/opengl-software.md).
