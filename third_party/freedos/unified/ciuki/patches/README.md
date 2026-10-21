# Patch 0001: reproducible date

The lead authorised this patch in f3-55 Amendment 1 (2026-10-21) after the
pinned native Open Watcom 1.9 compiler rejected redefinition of its predefined
`__DATE__` macro with E1100. The kernel is **unmodified except patch 0001
(reproducible date)**. The untouched nested source archive and its ledger
remain the upstream authority.

`0001-reproducible-date.patch` SHA-256:
`816cca4f5bd4542013073cbd60246b78e9342db200262cb6a941aafd543c8045`.

The only two timestamp uses in the pinned K2043 archive are
`hdr/version.h:48` (kernel banner) and `sys/sys.c:989` (SYS banner). Both use
`CIUKI_BUILD_DATE` after this patch and fall back to upstream `__DATE__` when
the macro is absent. There is no `__TIME__` or `__TIMESTAMP__` use in this
archive. No executable logic or other string changes. Source line endings
are preserved. FreeCOM changes belong to f3-58.

`../build.json` pins the K2043 release date `May 14 2021`, time `00:00:00`,
UTC epoch 1620950400, patch hash and both changed file hashes. The date is
confirmed by `APPINFO/KERNEL.LSM` in the pinned kernel package. Amendment 3
passes the two build macros through the generated external forced include
`ciuki-build-date.h`, using only the space-free relative command-line token
`-fi=../ciuki-build-date.h`. It never redefines `__DATE__` or `__TIME__`.
The exact header is part of the build record and corresponding-source bundle.
The native compiler probe uses the same option and SYS working directory.
All 181 source hashes and both external inputs are checked after make.

The external `MAKE` setting also supplies the forced include through wmake's
command-line `CFLAGST` override: `sys/makefile` does not consume `ALLCFLAGS`.
That override retains the exact upstream Watcom flags and Linux `.obj`
suffix. The two date sites therefore receive the same fixed manifest values.

Research: [K2043 release](https://github.com/FDOS/kernel/releases/tag/ke2043),
[Open Watcom 1.9 release](https://github.com/open-watcom/open-watcom-1.9/releases/tag/ow1.9),
[getting started](https://open-watcom.github.io/open-watcom-1.9/c_readme.html),
[compiler forced-include option](https://open-watcom.github.io/open-watcom-1.9/cguide.html),
[wmake command-line macro precedence](https://open-watcom.github.io/open-watcom-1.9/tools.html#Open_Watcom_Make_Command_Line_Format).
The checked local inputs are the release archives, never master sources.

Amendment 3 supersedes the masked vendor byte comparison: UPX 5.2.1 rejects
the flattened SYS wrapper with `NotPackedException`; no wrapper reconstruction
is required. UPX's unused pin and unpack tooling are removed. Acceptance is
byte-identical raw rebuilds under different observed host dates, roots and
input mtimes, plus 100% identical results in the unchanged f3-48 differential
battery (lead-run). A host qualification does not claim behavioural success.
The original vendor hash and size accompany the raw rebuild's size and Wlink
section layout. Input mtimes 946684800 and 1609459200 are separate from both
the fixed release date and the observed host clock.
