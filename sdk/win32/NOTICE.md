# CiukiOS Win32 core notices

Copyright 2026 CiukiOS contributors. The original code in this directory and
the f5-05 host adapter/stub generator is licensed under LGPL-2.1-or-later.
See [COPYING.LIB](COPYING.LIB) for the complete licence. It is provided without
warranty. Retain the source, copyright, licence and modification notices when
redistributing it; provide relinking/source access as the licence requires.

Microsoft's public PE/ABI documentation, upstream Wine's public layouts and
ordinal specifications, and Linux's syscall documentation were consulted as
behavioural references. No Wine implementation or conformance-test source was
copied. The self-authored PE comparison fixture contains no vendor game code.
Linux UAPI headers used by the host adapter carry the Linux syscall exception;
they are not included in the freestanding PE DLL builds. The research links and
implementation decisions are recorded in
`docs/design/win32-pe-host-evidence.md`.

The generated placeholder DLLs have project version **0.0.0.0**, including an
actual VERSIONINFO resource. These are not qualified Windows 98 compatibility
version values. In particular, ordinal 2 of winmm is an opaque NONAME stub with
no invented public identity or calling convention.

The audio facade contains TinySoundFont v0.9, by Bernhard Schelling, based
on SFZero by Steve Folta, under the MIT licence in `audio/tsf/LICENSE`.
Upstream `tsf.h` Git blob: `a81f25d5ca2e210720d646dec2dbfaeb119acb09`.
Local changes borrow shared S16 samples, use defined offsetof expressions
and stream callback signatures, and steal the oldest active voice after
release-first exhaustion. The surrounding audio facade is original code.
The image must retain this licence as `/system/licenses/TinySoundFont.txt`.
FluidR3 bank notices are separate; see `assets/audio/soundfonts/README.md`.
