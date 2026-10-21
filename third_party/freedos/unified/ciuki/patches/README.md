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

`../build.json` pins the date `Oct 21 2026`, time `00:00:00`, UTC epoch
1792540800 (the existing f3-55 build-input date), patch hash and both changed
file hashes. The builder passes `CIUKI_BUILD_DATE` and `CIUKI_BUILD_TIME` on
the compiler command line using Open Watcom's documented `-dNAME="text"`
syntax. It verifies exact patch context without fuzz, all 181 source hashes
before/after the patch and after the build. The native compiler probe checks
the resulting date/time strings before make runs.

The external `MAKE` setting also supplies these macros through wmake's
command-line `CFLAGST` override: `sys/makefile` does not consume `ALLCFLAGS`.
That override retains the exact upstream Watcom flags and Linux `.obj`
suffix. The two date sites therefore receive the same manifest values.

Research: [K2043 release](https://github.com/FDOS/kernel/releases/tag/ke2043),
[Open Watcom 1.9 release](https://github.com/open-watcom/open-watcom-1.9/releases/tag/ow1.9),
[getting started](https://open-watcom.github.io/open-watcom-1.9/c_readme.html),
[compiler preprocessor options](https://open-watcom.github.io/open-watcom-1.9/cguide.html#Preprocessor),
[wmake command-line macro precedence](https://open-watcom.github.io/open-watcom-1.9/tools.html#Open_Watcom_Make_Command_Line_Format).
The checked local inputs are the release archives, never master sources.

The checker compares the raw rebuilt kernel against the pinned vendor member,
masking only the unique compile date in its FreeDOS banner. Missing/ambiguous
banners, different offsets/sizes and any other changed bytes fail. The pinned
vendor member is packed (UPX marker at offset 85, no visible compile date).
No unpacking or replacement reference is authorised here: this prerequisite
currently blocks pristine qualification. The separate two-date check compares
raw rebuilt outputs under input mtimes 946684800 and 1609459200. It cannot
substitute for the pristine check.
