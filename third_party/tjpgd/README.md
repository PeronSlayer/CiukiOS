# TJpgDec

This directory contains the official TJpgDec R0.03 source by ChaN, downloaded
from the upstream archive at <https://elm-chan.org/fsw/tjpgd/arc/tjpgd3.zip>.
The downloaded archive SHA-256 is
`052fe3efbc9a8be29f31597ad009c5b51a4f6905878eb28569e0ab3d46d0c013`.
The decompressor is compiled into `WEBIMG.APP` from `src/apps/webimg.c`.

Upstream notice retained from `tjpgd.c`:

> Copyright (C) 2021, ChaN, all right reserved.
>
> The TJpgDec module is a free software that opened for education, research and
> commercial developments under license policy of following terms.
>
> The TJpgDec module is a free software and there is NO WARRANTY.
> No restriction on use. You can use, modify and redistribute it for personal,
> non-profit or commercial products UNDER YOUR RESPONSIBILITY.
> Redistributions of source code must retain the above copyright notice.

The upstream patch1 from <https://elm-chan.org/fsw/tjpgd/patches.html> is
preserved in `tjpgd.c` for grayscale output when `JD_FASTDECODE >= 1`.
CiukiOS configures RGB888 (`JD_FORMAT == 0`) and `JD_FASTDECODE == 0`, so the
affected branch is inactive in this build.

CiukiOS adds `jd_step` to the upstream header/source. It retains the MCU cursor
and restart counters in `JDEC`, and decodes at most one MCU per call for the
browser's cooperative polling. The original `jd_decomp` API remains present.
The embedded source also replaces upstream `memset` calls with an equivalent
private byte-fill routine (the CAPP has no C runtime) and converts the fixed
IDCT/YCbCr decimal coefficients to the same truncated integer constants, which
avoids floating-point runtime imports on Open Watcom 16-bit builds.
