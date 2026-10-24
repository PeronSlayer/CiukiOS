# NetSurf 3.11 native port plan

Date: 2026-10-21. Directive: [f6-13](../directives/f6-13-netsurf-port-plan.md).
Status: dependency audit and proposed implementation contract for lead review.
This document does not close the [F6 browser gate](network-contract.md#test-architecture-and-acceptance-gates).
All budgets, implementation changes and future tests below are proposals, not
measurements or completed browser functionality. The inspected directive has
21 lines; it contains no separate amendment or lead-note appendix.

## Baseline and evidence

Use the owner's selected NetSurf 3.11, GPL-2.0-only browser, native framebuffer
frontend, CiukiOS socket fetcher, BearSSL 0.6 and existing SDK newlib. Keep
`file:`, `data:`, `resource:` and `about:`. Disable JavaScript for M1–M4.
Do not port libcurl or OpenSSL. The framebuffer backend must create a normal
v3 desktop window, never take the display or NIC directly.

The [upstream release page](https://www.netsurf-browser.org/downloads/source/)
identifies 3.11; the [developer documentation](https://www.netsurf-browser.org/documentation/develop)
points to source-tree build instructions. The pinned archive takes precedence
over older web instructions and current upstream HEAD. Inspected input:

- `config/netsurf.json`: `build/downloads/netsurf/netsurf-all-3.11.tar.gz`,
  **9,080,781 bytes**, SHA-256
  `4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b`.
- [Full-source release](https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz).
  All upstream paths below are relative to its `netsurf-all-3.11/` directory,
  not files installed in this repository. Inspected with `tar -tzf` and
  `tar -xOzf` into `/tmp/ciuki-f6-13-audit/` only; no target compilation.
- Repository boundaries: [F2 subset](posix-subset.md),
  [f6-10 approved numbering](../directives/f6-10-lwip-sys-arch-sockets.md),
  `sdk/libciuki/{posix,time,signals,excluded}.c`, `sdk/newlib/recipe.json`,
  `sdk/sysroot-overlay/include/ciuki/font.h`, `apps/libdeskui/{deskui,v3}.h`,
  `apps/desktop/protocol.h`, `sdk/bin/ciuki-cc` and SDK dependency builders.
- The [newlib OS interface manual](https://sourceware.org/newlib/libc.html#Syscalls)
  distinguishes libc facilities from OS hooks; the local 4.5.0.20241231
  archive contains `newlib/libc/posix/regcomp.c`, `regexec.c`, `regerror.c`
  and `regfree.c`. Source availability still requires a selected SDK link
  check; it does not establish every POSIX interface as implemented.
- This worktree has calls through 77 in `src/kernel/include/ciuki/abi.h` and
  no public socket/poll/resolver SDK headers. f6-10 is a prerequisite, not an
  available implementation here. Its approved extension is **78–90**, not
  the older **75–87** table still present in `network-contract.md`.

### Complete archive inventory

Sizes are the sum of uncompressed regular-file payload bytes, including tests,
fixtures and documentation; they are neither installed sizes nor runtime RAM.
Versions are from component Makefiles. Each library uses the shared GNU make
buildsystem, C compiler, archiver and target-only pkg-config metadata unless
specified otherwise. MIT means the component's `COPYING`, except the per-file
notices of libpencil/librufl and `libutf8proc/LICENSE.md`.

| Component / version | Files | Bytes | License / first-port use | Dependencies and host-generation requirements |
| --- | ---: | ---: | --- | --- |
| netsurf 3.11 | 1,572 | 17,738,083 | GPL-2.0-only; core + framebuffer | libcss, libdom/hubbub, parserutils, wapcaplet, nsutils, utf8proc, zlib, nsfb; Perl testament; host C tools `split-messages` (-lz), `xxd`, `convert_font`, `convert_image` (-lpng); optional nslog/nspsl/images |
| libcss 0.9.2 | 333 | 3,543,777 | MIT; CSS | parserutils + wapcaplet; host property parser generator; Python 3 select generator; Perl test harness |
| libhubbub 0.3.8 | 145 | 9,818,377 | MIT; HTML parser | parserutils; gperf element lookup; Perl entity generation, Python trie tooling for regeneration, Perl tests |
| libdom 0.4.2 | 3,445 | 8,814,278 | MIT library; HTML DOM | parserutils + wapcaplet + hubbub binding; XML bindings optional (libxml2/Expat), off; Perl/XML::XPath belongs to DOM test transformation, not target runtime |
| libparserutils 0.2.5 | 101 | 448,584 | MIT; parsing/encoding | built-in codecs or iconv filter; Perl mapping/alias generators and tests; generated tables already supplied |
| libwapcaplet 0.4.3 | 12 | 114,895 | MIT; interned strings | C only; optional host test framework, no runtime OS dependency |
| libnsutils 0.1.1 | 17 | 38,501 | MIT; time/base64/I/O | C; monotonic-clock branch needs CiukiOS selection; do not inherit Linux `-lrt` |
| libutf8proc 2.4.0-1 | 35 | 1,839,455 | MIT + included Unicode data notice; IDNA/Unicode | C and supplied Unicode tables; data regeneration is a separate host operation |
| libnsgif 1.0.0 | 303 | 1,191,044 | MIT; M3 GIF | callback-owned bitmap allocator; Perl test tools; no external image library |
| libnsbmp 0.1.7 | 1,697 | 2,892,646 | MIT; M3 BMP/ICO | callback-owned bitmap allocator; no external image library |
| libsvgtiny 0.1.8 | 2,642 | 8,728,179 | MIT library; SVG off initially | libdom, wapcaplet, libm; gperf colors; enabling it requires qualifying the XML parser binding too |
| librosprite 0.1.3 | 21 | 316,301 | MIT; RISC OS sprites off | C stream callbacks; optional palette conversion tool |
| libnsfb 0.2.2 | 52 | 541,037 | MIT; software plots + new CiukiOS surface | C; disable all host-detected SDL/XCB/VNC/Wayland backends, retain RAM backend for host tests |
| buildsystem 1.10 | 30 | 139,673 | MIT; host only, LLVM helper has separate `llvm/LICENSE.TXT` | GNU make, shell, sed, Perl, pkg-config; static-library rules, no target runtime |
| nsgenbind 0.9 | 54 | 619,076 | MIT; host JS binding generator, omitted with JS off | **Actual spelling is nsgenbind**, not nsgenbin; C, flex, bison; no reason to build/run it on CiukiOS |
| libnslog 0.1.3 | 19 | 190,843 | MIT; recommend bounded diagnostics | allocation/string/formatting only; included by umbrella Makefile, not mentioned in directive's component list |
| libnspsl 0.1.7 | 15 | 2,854,806 | MIT code + **MPL-2.0 public suffix data**; enable for cookies | supplied `src/psl.inc`; regeneration Perl modules `Net::IDN::Punycode`, `Tie::IxHash`; keep data notice/source provenance |
| libpencil 0.0.5 | 9 | 31,430 | MIT per-file; excluded | RISC OS Draw export/OS interfaces; not a framebuffer dependency |
| librufl 0.1.0 | 44 | 268,040 | MIT per-file; excluded | RISC OS font/OS interfaces; not a framebuffer dependency |

The four root regular files (`.gitmodules`, `ChangeLog.md`, `Makefile`,
`README.md`) add 95,665 bytes. There are **19 component directories**, rather
than the directive's 15 names. Test/data notices are not relicensed by a
library's MIT notice: ship only the necessary resources plus their own notices.
In particular, public suffix data and Unicode data need separate attribution.
Retain NetSurf GPL source/patch availability and notices for linked libraries,
newlib's per-file licenses, SDK MIT, FreeType FTL and the separately licensed
Ciuki portrait. This is a packaging inventory, not a legal determination.

### Source-level libc/POSIX audit

Evidence is lexical `rg` search of archive C source streams and bounded reads
of the actual branch/Makefile, rather than an `nm` claim. A source match can
be a generator, unused backend or debug code. No objects were built; exact
**linked undefined symbols** must be frozen with `nm -u` on the selected
objects and a no-undefined SDK link in the coding directive. No build was
performed merely to manufacture that evidence.

The following inventory names the observed interfaces relevant to the port;
C allocation/string/stdio/ctype/math are newlib facilities, not new kernel
syscalls. `assert` is often a macro and debug diagnostics may compile out.

| Consumer | Observed C/library calls and POSIX boundary | Source evidence / disposition |
| --- | --- | --- |
| libcss | `malloc, calloc, realloc, free, memcmp, memcpy, memmove, memset, strchr, strcmp, strdup, strlen, strncasecmp, abs`, assertions/stdio; parser generator uses files/exit | `src/parse`, `src/select`, property generator; no thread, dynamic-loader, locale or readiness calls observed |
| libhubbub | `malloc, realloc, free, memchr, memcmp, memmove, memset, strlen, strncmp, strncasecmp, strncpy, fprintf, printf, abort`, assertions | `src/{tokeniser,treebuilder}`; parser callbacks, no OS I/O loop |
| libdom | `malloc, calloc, realloc, free, memcpy, memcmp, memset, strlen, strcmp, strncmp, strchr, strdup, strtol, strtoul, snprintf, sprintf`, stdio/assertions; `time(NULL)` in event initialization | `src/events/event.c`, `src/core`, `bindings/hubbub`; file-reading helpers and optional XML backends are separate from in-memory HTML binding |
| libparserutils | `malloc, realloc, free, memcpy, memmove, memset, strlen, strncmp, tolower, bsearch`, assertions; optional `iconv_open, iconv, iconv_close` | `src/input/filter.c`, `src/charset`; `WITHOUT_ICONV_FILTER` switches to supplied codec path |
| libwapcaplet | `malloc, free, memset`, assertions | `src/libwapcaplet.c`; no file/time/thread/locale/readiness dependency observed |
| libnsutils | `malloc, free`, assertions; `clock_gettime`, fallback `gettimeofday`; `read, write, pread, pwrite, lseek, ftruncate` | `src/time.c`, `src/unistd.c`; compile CiukiOS monotonic branch and native positioned I/O |
| libutf8proc | `malloc, realloc, free` and supplied data | `src/utf8proc.c`; Unicode conversion does not remove NetSurf's iconv calls |
| libnsgif | `malloc, calloc, realloc, free, memcpy, memset, strncmp`, assertions | `src/gif.c`; supplied bitmap callback API; no socket/file/thread/locale calls observed |
| libnsbmp | `malloc, calloc, free, memset`, assertions | `src/libnsbmp.c`; bitmap callbacks; no socket/file/thread/locale calls observed |
| libsvgtiny | allocation/memory/string/stdio/conversion; `atof, sscanf, acos, ceil, cos, fabs, fmod, sin, sqrt` | `src/svgtiny*.c`; libm/newlib available; optional XML dependency excluded initially |
| librosprite | `malloc, free, memcpy, fread, fopen, fclose`, diagnostic `printf, exit`, assertions | `src/librosprite.c`; disable format in browser, preserve audit |
| libnsfb | allocation/memory/string/assertions, scalar software plotters | `src/{plot,surface/ram.c}`; external backends add select, mmap/shm, sockets and GUI libraries and are excluded |
| libnslog | `malloc, calloc, free, strcmp, strncmp, strdup, strlen, snprintf, sprintf, vsnprintf`, assertions | `src/log.c` and component sources; no thread/dlopen dependency observed; bounded log sink required |
| libnspsl | `strlen`, generated static suffix lookup | `src/nspsl.c`, `src/psl.inc`; suffix data stays local, no runtime update/download |
| buildsystem/nsgenbind | host file/stdio/process/tool facilities | no target link; flex/bison and shell/preprocessor use are host requirements only |
| libpencil/librufl | C allocation/strings/files plus RISC OS SWIs/font/Draw/Wimp APIs | excluded; do not stub RISC OS APIs to pretend framebuffer compatibility |
| NetSurf core/framebuffer | allocation, string/ctype, stdio, sort, numeric conversion, libm, `setjmp/longjmp`; iconv, file/time/signal/inet functions below | `netsurf/utils`, `desktop`, `content`, framebuffer frontend; port OS feature detection explicitly |

The core's explicit OS-facing surface to account for is:

- File and persistence: `open, close, read, write, lseek, stat, fstat, mkdir,
  rmdir, unlink, rename, remove, opendir, readdir, closedir, dirfd, pread,
  pwrite, ftruncate`; `fopen/fclose/fread/fwrite/fseek/ftell/fgets/fputs/fgetc/
  fputc/fflush/feof` through newlib. `access`, `realpath`, `scandir`,
  `fstatat`, `unlinkat` are feature/port decisions, not assumed F2 services.
- `content/fetchers/file/file.c` selects **file-backed MAP_SHARED mmap** when
  `HAVE_MMAP` is set. F2's anonymous mapping support does not satisfy this.
  Use the upstream stdio fallback with size limits, then bounded streaming if
  needed. `data:` base64 decoding also needs decoded-size admission.
- Time: `time, gettimeofday, gmtime/localtime` and reentrant variants,
  `mktime, strftime, strptime`; cache/cookie expiry uses wall time; scheduler,
  DNS/TCP/TLS deadlines must use `clock_gettime(CLOCK_MONOTONIC)`.
  `utils/time.c` assumes `%s` in strftime/strptime; select its numeric fallback
  on CiukiOS rather than infer that conversion from the presence of newlib.
- Locale/charset: `setlocale(LC_ALL, "")` in `desktop/netsurf.c`; choose `C`
  explicitly. `utils/utf8.c` unconditionally includes `iconv.h` and calls
  `iconv_open/iconv/iconv_close`, even with parserutils' filter disabled.
- Environment/process-local support: `getenv`, `atexit` and exit/abort paths;
  regex helpers use `regcomp, regexec, regerror, regfree`. Require real newlib
  regex behavior for history/import handling: NetSurf's `!HAVE_REGEX`
  fallback reports successful compilation and always returns no match, which
  is unsuitable as a working port. Do not use its copy-only realpath fallback
  as canonicalization either.
- Readiness: `content/fetchers.h`, `content/fetch.h`, `utils/inet.h` expose
  `fd_set`; `fetch_fdset()` polls fetchers and reschedules them. That is an
  upstream interface, not evidence that CiukiOS implements `select()`.
- Signals/information: `signal(SIGPIPE, SIG_IGN)` in `desktop/netsurf.c`,
  `uname` in logging/user-agent; use existing SDK signal/utsname adapters.
  No runtime pthread or dlopen/dlsym/dlclose call observed in the selected
  core/library sources. Other frontends and tests do not define our needs.
- zlib is **mandatory even without PNG or gzip HTTP**: `netsurf/Makefile:150`
  adds `-lz`; `utils/messages.c` uses `gzopen/gzclose` and inflate; user-agent
  support also includes zlib. Do not describe it as only an M3 image option.

| Area | Available in CiukiOS (this checkout) | Missing / prerequisite | To stub or adapt locally | To avoid |
| --- | --- | --- | --- | --- |
| C/newlib | scalar C, allocation, stdio/math, reentrancy and compiler helpers; regex sources in pinned newlib | selected target-link inventory remains unmeasured, including regex exports | retain upstream guarded string/scandir fallbacks where SDK lacks them | fake regex/realpath success, Linux headers/libs, permissive implicit declarations |
| Files | `sdk/libciuki/posix.c` file/dir/rename/fsync adapters | fstatat/unlinkat not in F2 surface; access/realpath not established by wrapper scan | undef `HAVE_FSTATAT/HAVE_UNLINKAT/HAVE_SCANDIR`; upstream path-based/scandir fallback; Ciuki file path adapter using getcwd/stat (no symlinks) | file-backed mmap, permissions/symlinks assumptions, fake successful durability |
| Time | `sdk/libciuki/time.c` real/monotonic clocks, UTC calendar | trusted wall-time validity/provenance for TLS | select CiukiOS monotonic branch in nsutils; numeric epoch fallback; errors propagated | wall time for deadlines, fake `_POSIX_TIMERS` conformance |
| Locale/encoding | C/POSIX locale; parserutils internal codecs; utf8proc tables | usable iconv not established; SDK recipe disables multibyte and does not enable iconv | preferred M1: real NetSurf conversion adapter over parserutils codecs; local UTF-8; reject unsupported encoding with NSERROR | identity-copy iconv stub; assuming `LIBICONV_PLUG=YES` implements iconv |
| Threads | SDK pthread subset, mutex/cond/TLS; automatically linked | cancellation-safe resolver worker integration | one bounded DNS worker using pthread_create/join and mutex/cond; no core calls off UI thread | pthread_cancel, fork/exec, shell launch, plugin loaders |
| Network | approved f6-10 contract only | headers + native wrappers 78–90, O_NONBLOCK, SO_ERROR, resolve4 and poll must land first | local fd_set representation/translation at fetcher boundary, use poll for sockets | advertising select() or IPv6 support, libc socket success stubs |
| Signals | supported SDK signals including SIGPIPE; signal/sigaction | no extra signal required | ignore SIGPIPE consistently; EINTR stays observable | timer signals and busy retry loops |
| TLS | vendored BearSSL, pinned CA data | cryptographic entropy; trustworthy clock; user-mode build/qualification | fail closed on unavailable prerequisites; explicit certificate error page | time/MAC/PID-seeded randomness, certificate bypass, TLS downgrade |
| Optional desktop operations | window/input/surfaces; shared text clipboard | frontend v3 binding and bookmarks UI | unsupported PDF/printing/external URL handlers return explicit unsupported/error | no-op success for networking, certificate checking, saving or clipboard |

Preferred encoding choice requires a real adapter, not an iconv ABI shim that
silently copies bytes. Parserutils internal codec set is narrower than arbitrary
iconv. Its README lists UTF-8, native-endian UTF-16, ISO-8859-n, Windows-125n
and US-ASCII; verify concrete codec aliases/tables, not arbitrary values of n.
Before M1, freeze the exact required encodings from the lead's private
fixtures, exercise split input/output and invalid sequences, and document
unsupported encodings. If that set exceeds the supplied codecs, the lead must
pin a compatible iconv implementation (including version, bytes, hash, license
and memory cost) or approve an SDK newlib iconv extension. Disabling the
parserutils iconv filter alone does not complete this work.

## Images, resources and fonts

For M1/M2, set `NETSURF_USE_GIF=NO`, `BMP=NO`, `PNG=NO`, `JPEG=NO`,
`JPEGXL=NO`, `WEBP=NO`, `NSSVG=NO`, `ROSPRITE=NO` (each has the
`NETSURF_USE_` prefix). Render alternative text/placeholders and test those
cases. M3 first enables the already pinned GIF/BMP/ICO decoders; PNG/JPEG
remain disabled until their separate pins pass the same import qualification.
This deliberately uses the directive's permission to disable missing formats
for the first milestone; there is no fabricated PNG/JPEG pin or source size.

| Item | Requirement / license | Size and pin status / proposed action |
| --- | --- | --- |
| GIF | libnsgif MIT, bitmap callbacks | 1,191,044 archive payload bytes above; M3 |
| BMP/ICO | libnsbmp MIT, bitmap callbacks | 2,892,646 payload bytes above; M3 |
| zlib | mandatory message decompression + gzip HTTP; zlib license | absent from NetSurf archive; proposed [zlib 1.3.2](https://zlib.net/), upstream tar.gz size **1468K (rounded)**, published SHA-256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`; acquire exact byte count and verify before adoption; no downloaded/qualified pin in this task |
| PNG | libpng plus zlib; libpng license/retained notices | absent; disabled M1/M2; [upstream libpng 1.6.59](https://www.libpng.org/pub/png/libpng.html) advertises tar.gz **1,589,315 bytes**, SHA-256 `86a3e4b501f7f50e392c4e456ea158893e9a595b1c63eb88c7bb9f6cf8772dad`; candidate for host tools/M3, not locally verified/adopted |
| JPEG | libjpeg C API; IJG notice/acknowledgment (or separately audited alternative) | absent; disabled M1/M2; [IJG source directory](https://ijg.org/files/) lists `jpegsrc.v10.tar.gz` at **1,074 KB (rounded)**; candidate only, exact bytes/hash and compatibility review remain before M3 adoption |
| SVG/RISC OS sprite/WebP/JXL | extra parser/codec costs | off M1–M4 unless separately directed; supplied SVG/sprite code is audited, not automatically selected |

**Superseded by fx-24 (2026-10-23):** PNG, JPEG, WebP and SVG are enabled
as in upstream's framebuffer front end; see *Content handlers, rendering and
cost (fx-24)* below for the pins, the handler table and the rendering fixes.

Host tools are a separate dependency graph. The pinned framebuffer Makefile
always generates embedded toolbar/pointer/throbber arrays via
`tools/convert_image.c` using **host libpng**, independent of
`NETSURF_USE_PNG`. `tools/split-messages.c` always links **host zlib**. Either
pin/record these host libraries and tool binaries, or replace the asset input
pipeline with deterministic conversions of approved project assets. Runtime
PNG being off removes neither host requirement. Keep generated C out of tracked
upstream sources. Host tool versions and libraries enter the cache key.

The internal bitmap font is **NetSurf framebuffer's**, not a font system in
libnsfb: `font_internal.c` includes generated `font-ns-sans.h` from
`frontends/framebuffer/res/fonts/glyph_data`, generated by host `convert_font`.
`NETSURF_FB_FONTLIB=internal` is the existing minimal route and offers limited
sizes/glyph coverage. Do not mistake this for the SDK scalable font bridge.

Prefer a new `NETSURF_FB_FONTLIB=ciuki` adapter for final acceptance, using
`ciuki/font.h` and linking `-lfont -lfreetype` from the existing SDK. Implement
NetSurf's layout width/position/split callbacks and glyph painting using
`ciuki_font_measure`, metrics/lookup/raster/paint and explicit byte lengths;
translate CSS sizes to 26.6 units and compose grayscale coverage through nsfb
plotters with a shared baseline. This reuses the existing trusted system font
policy and avoids bundling a second FreeType or upstream hard-coded Windows
font names. `font_freetype.c` calls FT_* directly; merely choosing `freetype`
does not route it through `ciuki_font_*`. Add a font selector rather than
pretending the direct backend is already the bridge.

Use packaged system fonts and existing fallback mapping; no downloaded
`@font-face` files for this milestone. Keep per-face cache caps (128 glyphs,
256 KiB) within a total browser font allowance, not multiplied without limit.
Host tests must compare width, hit-testing/splits and paint advances for
ASCII/UTF-8, missing glyphs, baseline, scale and clipping. Internal bitmap mode
may bootstrap M1 only if the lead accepts its coverage limitations; M1 evidence
must identify the actual backend. Retain exact approved Ciuki icon conversions
and Tango chrome; do not use upstream mascot art to replace CiukiOS identity.

## CiukiOS fetcher

### Boundary and upstream API

Choose a new `content/fetchers/ciuki.c` with `http` and `https` registrations.
The curl fetcher already integrates HTTP, decoding, cookies and certificates,
but importing curl would add another portability/TLS backend layer. A native
fetcher keeps the ABI boundary small and lets BearSSL handle TLS directly. The
cost is owning an HTTP parser and negative tests; this is not a trivial curl
wrapper replacement. Upstream evidence: `content/fetchers/curl.c`,
`content/fetchers.h`, `content/fetch.h`, `content/fetch.c` and
`content/llcache.c` from the pinned archive.

Implement all nine `fetcher_operation_table` entries with their actual types:

| Operation | Signature / contract |
| --- | --- |
| initialise | `bool (lwc_string *scheme)`; shared state reference-counted for two schemes |
| acceptable | `bool (const struct nsurl *url)`; only accepted scheme/host/port forms |
| setup | `void *(struct fetch *parent_fetch, struct nsurl *url, bool only_2xx, bool downgrade_tls, const char *post_urlenc, const struct fetch_multipart_data *post_multipart, const char **headers)`; own copies/references; validate all sizes |
| start | `bool (void *fetch)`; admit bounded job, return immediately; no blocking DNS/connect on UI |
| abort | `void (void *fetch)`; mark cancelled, detach I/O, ignore stale completions |
| free | `void (void *fetch)`; exactly-once ownership release; safe after callback cancellation |
| poll | `void (lwc_string *scheme)`; bounded progress over jobs, no duplicate work when both schemes polled |
| fdset | `int (lwc_string *scheme, fd_set *read_set, fd_set *write_set, fd_set *error_set)`; max fd/-1; preserve upstream semantics and translate readiness to native poll |
| finalise | `void (lwc_string *scheme)`; close jobs/pool, stop resolver safely on last scheme |

Register via `fetcher_add()` (which takes the interned scheme reference) from
`fetcher_init()` in `content/fetch.c`; change `content/fetchers/Makefile` source
selection. `NETSURF_USE_CURL=NO` exists upstream, but does not itself install a
replacement. Keep the non-network fetchers and ensure JS-off does not activate
a JavaScript fetcher. The enclosing core calls must stay on its UI thread:

- `fetch_set_http_code`, `fetch_send_callback`, `fetch_remove_from_queues`,
  `fetch_free`; `fetch_set_cookie` handles context/policy for Set-Cookie.
- Read request cookie text with `urldb_get_cookie(url, true)` like curl;
  authentication, if enabled, uses `urldb_get_auth_details`, never a separate
  persistent cookie/password database.
- Deliver raw response header lines as `FETCH_HEADER`, decoded body slices as
  `FETCH_DATA`, optional `FETCH_PROGRESS`; deliver **one** terminal outcome:
  `FETCH_FINISHED`, `FETCH_NOTMODIFIED`, `FETCH_REDIRECT`, `FETCH_AUTH`,
  `FETCH_TIMEDOUT`, `FETCH_ERROR`, `FETCH_CERT_ERR` or `FETCH_SSL_ERR`.
  `FETCH_CERTS` carries a correctly owned `cert_chain` for HTTPS information.
  Check for abort after **every callback**, including headers, before touching
  state again. Respect NetSurf queue removal/free ordering from curl/file.

The new network code uses public SDK wrappers, not hard-coded syscall assembly:
`socket=78`, `bind=79`, `connect=80`, `listen=81`, `accept=82`, `sendto=83`,
`recvfrom=84`, `shutdown=85`, `sockname=86`, `sockopt=87`, `poll=88`,
`resolve4=89`, `netctl=90`. Browser client needs socket/connect/send/recv,
shutdown/close, fcntl O_NONBLOCK, SO_ERROR, poll and resolve4; it needs no
listen/accept/netctl or network-configuration authority. Structures, errno and
byte order come from the qualified f6-10 SDK. DNS uses one IPv4 A result from
resolve4 (or getaddrinfo wrapping it), not an embedded resolver. IDNA processing
occurs in user mode before bounded ASCII names reach resolve4. IPv6 and
HTTP/2/3 are outside this plan.

Preserve the upstream fd_set API using a port-local bounded set/FD_* adapter
and map sets to native `pollfd` records in the event pump. Do not add a public
select() compatibility promise solely for this port. `fetch_fdset()` already
advances jobs before collecting sets and can reschedule polling to one second;
calling it without adapting the wakeup/scheduler path would cause stalls.
Include desktop channel events through the readiness mechanism f6-10 actually
qualifies; otherwise bound nonblocking desktop drains and socket poll waits to
10 ms. Keep scheduler due-times and worker completion wakeups in that bound.

DNS is an interruptible but potentially blocking ABI operation. Use one bounded
resolver worker with job-generation tokens, copied input/results and no NetSurf
callbacks. Cancellation invalidates the token and frees UI resources; a late
answer cannot resurrect a fetch or fd. Worker interruption/join/exit deadlines
must be tested against f6-10. Never block the UI joining an unresolved worker or
spawn unlimited workers after timeouts. All TCP/TLS state is driven from the UI
pump with nonblocking sockets and bounded bytes/steps per turn.

### HTTP/1.1 behavior

Implement according to [RFC 9112 framing](https://www.rfc-editor.org/rfc/rfc9112.html)
and [RFC 9110 semantics](https://www.rfc-editor.org/rfc/rfc9110.html), with these
proposed initial limits and policies:

- GET/HEAD and bounded URL-encoded form POST; preserve the setup API's multipart
  argument, but return an explicit unsupported-form error until bounded upload
  streaming is directed/tested. Validate URL/request/header bytes, reject CR/LF
  injection, emit origin-form target and Host including nondefault port.
- Handle partial status/headers, interim 1xx, Content-Length, chunked data with
  extensions/trailers, and EOF-delimited responses. Reject invalid/conflicting
  lengths, overflowing chunks, unsupported transfer codings and truncated bodies.
  Conflicting TE/CL is a local fetch error; never reuse that connection.
- Reuse only completely framed/drained successful connections with matching
  origin (scheme, normalized host, port), successful TLS policy and no EOF/error.
  No pipelining. Start with four total active fetches, two per origin, two idle
  keep-alive handles and 15 s idle expiry. Retry stale pooled connections at
  most once for an idempotent request before body delivery, never replay a POST.
- Let llcache follow `FETCH_REDIRECT` using Location; don't independently follow
  inside the fetcher. Check the existing redirect hop limit, propose ten maximum;
  resolve relative locations, strip cross-origin authorization, retain appropriate
  301/302/303 versus 307/308 method handling and stop loops/unsupported schemes.
  No silent HTTPS-to-HTTP downgrade; user-visible error unless separately approved.
- Send `Accept-Encoding: gzip` only after streaming zlib decoding passes tests.
  Transfer dechunking precedes content decoding. Decoded slices go to llcache;
  preserve real raw response metadata and test Content-Encoding/Length handling
  against curl behavior. Bound compressed and expanded bytes independently and
  verify gzip checksum/end-of-stream. Unsupported encodings fail explicitly.
- Proposed caps: URL 4 KiB, total headers 32 KiB, line 8 KiB, 128 fields,
  decoded document/body 8 MiB, input/output slices 16 KiB. Reject excess with
  a local fetch/content error, no panic. Bound `data:` and `file:` similarly.
  Use checked arithmetic for chunk size, Content-Length and decoded dimensions.
- Monotonic proposed deadlines: DNS/connect 10 s each, handshake 20 s, idle body
  15 s, total fetch 60 s. Close cancelled/error sockets; resume EINTR/EAGAIN
  through the event loop. HTTP can remain usable offline with local content.

Cookies stay in NetSurf's URL database. M1/M2 can operate with cookies disabled;
M3 enables them with libnspsl and tests host/domain/path/expiry/Secure/HttpOnly,
public-suffix rejection and redirects; [RFC 6265](https://www.rfc-editor.org/rfc/rfc6265.html)
is the primary protocol reference. Qualify actual NetSurf 3.11 behavior rather
than claim current browser cookie conformance. M4 persists cookies only under
an explicit user setting, with bounded files and visible save errors. Auth
credentials must not leak into serial logs or cross-origin redirects.

### HTTPS and prerequisite failures

BearSSL is pinned locally at 0.6 (`third_party/bearssl/README.ciuki`), MIT,
upstream archive SHA-256
`6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`.
Use its [generic engine API](https://bearssl.org/api1.html) with the SDK socket
pump, not blocking `br_sslio` callbacks. Initialize `br_ssl_client_init_full`,
set buffers/versions, inject qualified entropy and reset with the hostname/SNI.
Consume/ack record and application buffers via `br_ssl_engine_*buf/*ack` while
respecting `BR_SSL_SENDREC/RECVREC/SENDAPP/RECVAPP/CLOSED` and error state.

The [SSL API reference](https://bearssl.org/apidoc/bearssl__ssl_8h.html) and
vendored headers define `BR_SSL_BUFSIZE_BIDI` as **33,178 bytes** per active
connection (16,709 input + 16,469 output), before contexts/certificate storage.
Keep contexts at stable addresses. Propose TLS 1.2 only, scalar i31/constant-time
portable algorithms for i686 without SSE; no CPU-unsupported hardware crypto
paths. Restrict to qualified forward-secret AEAD suites; never honor upstream
`downgrade_tls` as permission to lower verification or protocol policy.
[Supported protocol limits](https://bearssl.org/support.html) must be visible:
BearSSL 0.6 supplies no TLS 1.3, HTTP/2 or general modern-site guarantee.

Use the 2026-09-25 `third_party/cacert/CACERT.PEM`, **188,900 bytes**, verified
SHA-256 `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`.
Its [curl/Mozilla provenance and MPL-2.0](https://curl.se/docs/caextract.html)
are retained; do not fetch a moving CA file during the build. Compile a
separately hashed trust-anchor table with a pinned host tool, or use bounded
BearSSL PEM/X.509 decoding at startup. The vendored subset has no `brssl` tool;
acquiring/building a host generator is another recorded dependency, not a
command assumed to exist. Keep the original bundle and license in resources.

[BearSSL X.509 documentation](https://bearssl.org/x509.html) defines minimal
chain/name/date validation and its limitations. Use trust anchors, hostname
validation and explicit `br_x509_minimal_set_time` conversion from validated
system CLOCK_REALTIME (checked days since year 0 plus seconds-of-day). Reject
untrusted roots, mismatches, expired/future certificates, malformed chains and
unsupported algorithms. Minimal validation does not supply revocation checking;
M2 must state that limit and cannot claim full browser PKI policy.
HTTPS numeric-IP URLs require correctly qualified IP SAN matching; reject them
as unsupported until that is supplied, rather than treating an IP as a DNS
hostname or bypassing name checks. Include that rejection in negative tests.

**M2 is blocked until entropy and wall time are qualified.**
`sdk/libciuki/excluded.c` implements `getentropy` and `_getentropy_r` as ENOSYS.
BearSSL's host OS random adapters do not provide a CiukiOS entropy source.
The lead must approve a trusted kernel entropy facility/interface with seed
quality and failure tests; do not substitute PRNG seeds from clock, PID, MAC,
network timing or bundled constants. Likewise, an available `time()` wrapper
is not evidence that the RTC is set correctly: require a validated clock and
visible correction path, fail HTTPS closed if unavailable, and keep monotonic
I/O timers independent. No certificate exception/continue button is proposed.

## Framebuffer frontend and desktop integration

Reuse NetSurf layout/rendering and framebuffer software plotters, with a new
libnsfb `ciuki` surface. Source API is `libnsfb/src/surface.h`:
`nsfb_surface_rtns_t` has `defaults, initialise, finalise, geometry, parameters,
input, claim, update, cursor`. Register it using `NSFB_SURFACE_DEF` and a
new `nsfb_type_e`, included by `src/surface/Makefile`. Registration uses ELF
constructors: verify SDK init-array execution and archive retention; NetSurf's
framebuffer Makefile currently whole-archives libnsfb, so do not assume a
constructor survives arbitrary dead stripping.

Implement the window over `du3_connect/du3_request/du3_next/du3_ui_progress`
and the byte-encoded desktop v3 protocol. Existing `du_window_*` glue primarily
uses v1/v2 envelopes; it is not automatically a v3 backend. Reuse libdeskui's
widget/drawing/keymap code inside the new v3 binding rather than embedding
another window protocol or silently downgrading the directive to v2.

- Handshake and CREATE with bounded UTF-8 title, window id/generation tracking;
  create/map a Ciuki XRGB8888 surface and validate width, height, pitch, size,
  format and overflow via `ciuki_surface_info/map`. Use nsfb's matching byte
  order after red/blue/stride fixture tests; do not assume ABGR defaults match.
- Render into one owned back buffer, copy clipped damage to mapped surface,
  send bounded DAMAGE, and honor presentation ownership. `claim/update` must
  union and clip rectangles, not repeatedly repaint the entire screen.
- CONFIGURE/RESIZE is transactional: allocate/map one replacement, send fd
  attachment with configure serial; retain old storage until the protocol
  confirms swap. Ignore stale generations and handle resize OOM by keeping
  the usable old surface. Shutdown/disconnect closes all descriptors/maps.
- Translate KEY/MOTION/BUTTON and focus/capture-loss into nsfb/fbtk input.
  Preserve modifiers, navigation keys, text codepoints, repeat, wheel and
  client/content coordinate offsets; reconcile pressed state on loss. No
  direct PS/2 reads. Route toolbar events once; content sees only its viewport.
- Keep PONG tied to UI progress; a DNS thread cannot conceal a stalled renderer.
  Drain input, scheduler/fetch progress and redraw work in bounded turns.
  The current framebuffer loop is `schedule_run -> fbtk_event -> fbtk_redraw`;
  adapt its wait to socket/desktop readiness and the scheduler deadline.
- Clipboard uses NetSurf `gui_clipboard_table.get/set` mapped to
  `du_clipboard_get/du_clipboard_set_n`; get returns a new core-owned UTF-8
  allocation, set copies borrowed data, validates length and retains the old
  clipboard on failure. Protocol v3 currently has no clipboard opcode: the
  shared text clipboard helper is the boundary, not an invented v3 message.
- Use libdeskui for window chrome/buttons/text entry where practical. Retain
  fbtk for browser content/scrolling, with one event/layout owner per region.
  Add URL entry (Enter), Back/Forward, Reload/Stop and loading/status/errors;
  keyboard Tab/Shift-Tab, Enter, arrows/PageUp/PageDown, Ctrl+L/R, Esc,
  Alt+Left/Right, copy/paste, focus indication and disabled-button states.
- Use NetSurf local history navigation and hotlist/bookmark models; M4 adds
  visible bookmark/history actions and persistent load/save. Upstream's
  framebuffer GUI does not by itself satisfy a complete bookmarks/history UI.
  Use a writable per-user directory chosen by the lead, proposed
  `/home/default/.netsurf/{Choices,Hotlist,History,Cookies}`. Resources stay
  read-only under proposed `/system/netsurf`; never expand `~` implicitly.
  Bound file sizes/counts, use temp + fsync + rename where supported, preserve
  old data on failure and test ENOSPC/read-only/corruption/restart.

Set **`NETSURF_USE_DUKTAPE=NO`** at build time and
**`enable_javascript:false`** in Choices. The former selects
`content/handlers/javascript/none`, the latter alone merely disables execution
at runtime while still compiling Duktape. Remove nsgenbind/WebIDL generation
from the umbrella recipe when this switch is off. No JavaScript compatibility
or fallback execution is part of these milestones.

## Proposed reproducible SDK build recipe

This is a recipe for the subsequent coding directive, not commands executed
or a claim that the unmodified tarball cross-builds today.

1. Validate the archive length/hash against `config/netsurf.json` on every
   invocation, including cache hits. Future implementation extracts only
   validated safe regular paths into `build/external/netsurf-all-3.11` and
   applies a hashed ordered patch set there. Nothing downloads during build.
   This task has extracted only text into scratch.
2. Mirror `sdk/freetype/build.py` and `sdk/libgl/build.py` provenance: hash
   archive, every patch, SDK manifest/ABI, compiler/archiver/linker, flags,
   feature choices, host tools/libraries, fonts, CA bundle/table generator,
   resources and license manifest. Recheck input and cached output hashes;
   changed/missing input invalidates cache, not a fallback to a host library.
3. Separate host tools from target libraries. Set `BUILD` to host tuple,
   `HOST=i686-unknown-elf`, `CC=<installed-sdk>/bin/ciuki-cc`, pinned LLVM
   `AR/RANLIB`, and `BUILD_CC/BUILD_CFLAGS/BUILD_LDFLAGS` to recorded host
   tools/host dependencies. Set both upstream spellings **PKGCONFIG**
   (libraries) and **PKG_CONFIG** (browser) to target metadata only; clear
   inherited CPATH/LIBRARY_PATH/PKG_CONFIG_PATH and set PKG_CONFIG_LIBDIR.
   Never let host SDL/png/freetype/curl .pc files leak into target detection.
4. Patch buildsystem tuple/tool detection for the SDK wrapper: do not infer a
   target compiler by executing its output or append forbidden `-march`,
   `-msse`, `-shared`, PIC/PIE or `-pthread` flags. The SDK already fixes i686
   ABI and automatically links libpthread/newlib/libm/compiler support.
   `COMPONENT_TYPE=lib-static`, no dlopen/shared imports, no `-lrt/-ldl`.
   Use the wrapper for the final ELF link and preserve its map/no-undefined
   checks. No host CRT/libc or post-link conversion to a Linux executable.
5. Build shared buildsystem metadata for host use. Build static target
   libwapcaplet and parserutils, then libcss/libhubbub, then libdom's hubbub
   binding, nsutils/utf8proc, nslog/nspsl, nsfb with only Ciuki surface.
   Keep image libraries for M3. XML bindings/SVG/sprites/libpencil/librufl
   stay off. Build required host tools separately; gperf and any Perl/Python
   generators run on host, never via the target wrapper. Pin generator
   versions or retain verified release-generated tables; no opportunistic
   regeneration of PSL/Unicode from the Internet. The umbrella Makefile
   currently builds optional libraries and nsgenbind unconditionally; replace
   its orchestration with the explicit dependency graph.
6. Build NetSurf with `TARGET=framebuffer`, `NETSURF_FB_FRONTEND=ciuki`,
   proposed `NETSURF_FB_FONTLIB=ciuki`, `NETSURF_USE_CURL=NO`,
   `NETSURF_USE_DUKTAPE=NO`, `NETSURF_USE_UTF8PROC=YES`,
   `NETSURF_USE_NSLOG=YES`, `NETSURF_USE_NSPSL=YES`,
   `NETSURF_USE_HARU_PDF=NO` and explicit NO image settings above.
   No AUTO option may depend on developer-installed packages. Adapt iconv,
   inet/fd_set and utils/config.h feature assumptions as audited. Set
   `NETSURF_USE_LIBICONV_PLUG` only for an actual chosen conversion backend;
   neither YES nor NO is an implementation. Link separately qualified zlib,
   libfont/FreeType and, from M2, BearSSL user library.
7. Split library staging prefix from runtime paths. Install executable as
   **`/Programs/netsurf`**, following the executable-file convention in
   `scripts/build_image.py`, and resources under proposed
   **`/system/netsurf`**, including filtered English Messages,
   CSS defaults, HTML error/about templates, MIME information, cursor/chrome
   assets if used, fonts/references, pinned CA bundle/derived anchors and
   licenses. Override `NETSURF_FRAMEBUFFER_BIN`,
   `NETSURF_FRAMEBUFFER_RESOURCES`, `NETSURF_FB_RESPATH/FONTPATH` rather than
   leave `/opt/netsurf`, `/usr/share/fonts`, current directory or host HOME
   paths. Upstream builds `EXETARGET=nsfb` and installs it as `netsurf-fb`;
   the packaging adapter must explicitly rename that verified ELF to
   `/Programs/netsurf`, rather than expect a prefix change to rename it.
   Add the desktop launcher through the existing program catalogue
   only after M4; it points to that executable with the approved Ciuki icon.
8. Pin source-date epoch, `LC_ALL=C`, `TZ=UTC`, stable source ordering and
   deterministic archives; apply SDK prefix maps to generated objects.
   Replace checkout-dependent git testament/build timestamps with pinned
   release/patch identities, record host tool versions and generated file
   hashes. Two isolated clean builds must produce identical executable and
   resource hashes. Wall-clock build duration is evidence, not binary input.

Patch groups to review before coding: (a) make/toolchain/feature selection,
(b) charset/time/file portability, (c) fetcher + HTTP/BearSSL adapters,
(d) libnsfb Ciuki surface and retained constructors, (e) v3 event/frontend/font
bridge + persistence/resources. No new kernel panic path is needed by any
browser error or malformed input. This directive changes none of those files.

### Proposed T23/128 MiB admission target

Do not raise OS memory ceilings on the basis of source size. Start with **one
browser process, one window**, 32 MiB total resident charge target including
static segments, heap, committed stacks and browser-owned surfaces; reject
additional windows/oversized content rather than consume emergency reserves.
The 32 MiB number is a proposed application target, not an existing enforced
per-process kernel limit. At 1024x768, two 32-bit buffers cost exactly 6 MiB;
resize may temporarily need four (12 MiB). Reserve for that peak explicitly.

| Owner | Proposed maximum within 32 MiB total |
| --- | ---: |
| Linked text/rodata/data/BSS | 6 MiB (measure; may require lead revision) |
| Mapped surface + back buffer, including transactional resize peak | 12 MiB |
| Main stack + one resolver stack/reentrancy/TLS | 1.5 MiB; use default 1 MiB main and bounded worker reservation |
| All heap, including the following sublimits | 12.5 MiB |

Within the heap (not additional to it): NetSurf `memory_cache_size=2 MiB`
instead of its 12 MiB default; decoded image cache 2 MiB; total fonts 1 MiB;
network/TLS/pool/headers at most 1 MiB; remaining 6.5 MiB for DOM/CSS/layout,
resources, metadata and temporary work. Disable disk cache (`disc_cache_size=0`,
null `disc_cache_path`; upstream default is 1 GiB), cap active fetchers 4,
per-host 2 and idle handles 2 using the matching NetSurf options. Image pixel
and body admission must consult remaining charge, not just standalone limits.

A cache-size option is not an allocator limit. Native allocations, decoder
callbacks, data/file bodies, CA state, fonts, two-to-four surfaces and DOM must
be measured and admitted against a browser ledger. Propose single-image max
1024x1024/4 MiB with aggregate decoded-cache eviction; never infer a tiny GIF's
RAM from compressed size. Limit animation frames/time and discard on OOM.
Host OOM injection must leave an error page and working toolbar.

The lead must measure actual free/reserved/committed memory on the supported
128 MiB profile with desktop and network running before approving these caps.
Record ELF sections, heap high-water, stack reservation/commit/touched peak,
surface bytes, font/cache/network ledger, overall physical pressure and OOM
recovery; repeat at 256/512 MiB. Also measure input/stop latency and longest
layout/redraw slice: synchronous NetSurf HTML layout may require additional
yielding or document limits. Nonblocking networking alone proves no UI bound.
Proposed fixture acceptance is <=100 ms input/Stop response; accept or revise
with measured T23 evidence, never suppress genuine unresponsive-window state.

### Document admission against available memory (fx-14, 2026-10-23)

The f6-14 amendment-1 structural constants (8192 DOM nodes, 8192-byte text
chunks, 128 KiB of document text, 8192 boxes, depth 64, 64 table columns,
128 KiB of plain text) refused ordinary pages on the T23 (trial e831e685:
Google's home page and BBC News downloaded, then "parser admission limit"
and the out-of-memory page). They are replaced by ledgers charged against
the memory actually free (`sdk/netsurf/port/docmem.h`, patch 0008): CiukiOS
reads `free_pages` through call 70 (`ciuki_proc_list`, system record only);
host builds use a model of the 128 MiB baseline (56 MiB free while parsing).
The DOM ledger (libdom hubbub binding: 64 KiB per document, 352 bytes per
node or attribute plus its strings, measured on x86-64) and the box ledger
(352 bytes per box from the i686 `struct box`, plus its text; box creation,
layout line splits) may each use a quarter of free memory; plain text is
admitted as its bytes plus an upper bound of its line records. There is no
count, chunk or run cap below the budget. Script element text is dropped
(no JavaScript). DOM depth 128 and box depth 256 (verified under a 1 MiB
stack), colspan clamped to 1000 and tables wider than 1000 columns refused.
Refusals log `NetSurf structure rejected: <cause> (charged=… budget=…)` and
show an error page that names the cause ("This page is too large for the
memory available to the browser."). Images: one bitmap up to 1024x1024
pixels in any shape up to 4096 per side; the decoded-image ledger is free/8
clamped to 2–16 MiB; 1024 GIF frame records. Evidence:
`build/codex/fx-14-report.md`, `tests/host/netsurf` (`pages.py` fixtures).

### Layout time limits (f6-14 budget; lead decision 2026-10-23)

f6-14 accepted a 75 ms monotonic layout deadline provisionally ("revised by
directive if exceeded"); f6-20 exempted local content (`about:`, `resource:`,
`file:`) from it. **Lead decision 2026-10-23 (fx-11):** the document of a
top-level browser window gets a **10 s** layout limit
(`CIUKI_LAYOUT_DOCUMENT_LIMIT_NS`, `sdk/netsurf/port/budget.h`); frames,
iframes, embedded objects and every other remote span keep 75 ms, and a
nested span inherits its outer deadline. A page over its limit still shows
the existing error page ("needs more layout time than the browser allows for
web content"), with Try Again. Measured basis (T23, trial 423b803a): 58 ms of
layout work for the welcome page alone; real documents with 5–13 ms of host
layout correspond to about 0.6–1.7 s on the T23
(`docs/validation/fx-11-netsurf-fetch-layout.md`).

NetSurf runs in its own process, so a long layout freezes only its window.
While a layout runs the browser neither repaints nor handles input: the
desktop keeps showing the last presented frame and can still move the window,
and marks it "Not responding" after an unanswered 5 s PING
(`apps/desktop/client.c`), which is genuine and not suppressed. The keepalive
pump (`ciuki_ui_keepalive`, at most every 20 ms while layout code runs) reads
the desktop channel throughout, so the desktop never drops the client (it
drops only a client that read nothing for 5 s with a full queue); input past
64 held packets is dropped and counted (`input_overflows`), and held keys and
buttons are released afterwards like a focus loss. Held input is processed
when the layout ends: the bound on input latency is the layout time, at most
10 s for a top-level document and 75 ms for anything else.

### Content handlers, rendering and cost (fx-24, 2026-10-23)

Evidence: T23 trial of preview `f441c5bd` (private `legacy/local/physical/
f441c5bd/`): `NETSURF.LOG` `fetches=92 fetch_ok=90`, `remote_max_us=3567000`;
serial: the encyclopedia main page (268,370 bytes), its stylesheet
(`load.php`, 200) and its thumbnails (200) were fetched; the slow layout was
`pass=2 font_measures=1562 font_measure_us=632000 width_hits=1201
measured_bytes=20938`, while `[cpu] top=netsurf#65:442k302s7` (NetSurf had
44.2 % of the CPU, 30.2 points of it, i.e. 68 %, in the kernel). The host build lays out the same page with
the same counters (1562/1201/20938), so the author CSS *was* applied on the
T23; what was missing were the images, and the time went largely to the
kernel.

Upstream reference: `frontends/framebuffer/Makefile.defaults` of NetSurf 3.11
(`NETSURF_USE_JPEG := YES`; `PNG`, `GIF`, `BMP`, `WEBP`, `NSSVG`,
`ROSPRITE` `:= AUTO`, i.e. on when the library is present),
`content/handlers/image/image.c` (`image_init` registers BMP, GIF, ICO, JPEG,
JPEG XL, PNG, sprite, SVG, WebP when compiled), `frontends/framebuffer/
framebuffer.c` (the plotter table; `framebuffer_plot_path` logs "path
unimplemented"). Documentation: <https://www.netsurf-browser.org/documentation/>,
<https://www.netsurf-browser.org/developers/>. SVG rules followed by the path
plotter and the viewBox fix: SVG 1.1 painting (11.3 `fill-rule`,
<https://www.w3.org/TR/SVG11/painting.html>), paths (8.3.3 closepath,
<https://www.w3.org/TR/SVG11/paths.html>), coordinate systems (7.7 `viewBox`,
<https://www.w3.org/TR/SVG11/coords.html>).

| Content | Upstream framebuffer | Before fx-24 | fx-24 |
| --- | --- | --- | --- |
| `text/html` (libhubbub, libdom) | yes | yes | yes |
| `text/css` (libcss): `<link rel=stylesheet>`, `@import`, `<style>`, `style=` | yes | yes | yes (host test) |
| `text/plain` | yes | yes | yes |
| `image/gif` (libnsgif) | yes | yes | yes |
| `image/bmp`, `image/x-icon` (libnsbmp) | yes | yes | yes |
| `image/png` (libpng 1.6.59) | yes | **no** (f6-14 deferred) | yes |
| `image/jpeg` (libjpeg-turbo 3.1.4.1, C only) | yes | **no** | yes, DCT-scaled when larger than the bitmap admission |
| `image/webp` (libwebp 1.6.0 decoder) | yes | **no** | yes |
| `image/svg+xml` (libsvgtiny 0.1.8, Expat 2.9.0 XML binding) | yes, but paths not drawn | **no** | yes, paths drawn (non-zero fill, strokes, anti-aliased) |
| `image/jxl` (libjxl, C++) | when present | no | no (gap: no C++ runtime in the SDK) |
| RISC OS sprite | when present | no | no (not a web format) |

Fetching and attachment follow upstream unchanged: stylesheets from
`<link>` and `@import` are fetched by the HTML content and box conversion
waits for them; `<img>` is fetched speculatively when the element enters the
DOM and again by box construction (one shared low-level cache object);
background images through the computed style. A fetch whose media type no
handler accepts is aborted after its first slice, which is why the trial's
thumbnails ended `stage=done code=200` at about 12 KB.

Rendering defects fixed (patch 0009 and port files):

1. **Paths** (`sdk/netsurf/port/path.c`): NetSurf path commands are mapped
   through the SVG transform, Béziers flattened in device space, fills use
   SVG's default non-zero rule (SVG 1.1, 11.3), strokes are the union of one
   quadrilateral per segment plus round joins; each fill/stroke becomes an
   8-bit coverage mask (exact horizontally, 4 rows per pixel), clipped and
   blended with `nsfb_plot_glyph8`. `NS_TRANSPARENT` disables fill/stroke as
   in the GTK front end (svg.c leaves the style types unset).
2. **Clip outside the surface**: libnsfb refuses such a rectangle and
   `framebuffer_plot_clip` returned `NSERROR_INVALID`; the knockout plotter
   handed that error to the next flushing operation, so an unrelated SVG or
   bitmap redraw "failed" and was drawn as the pink broken-object box. The
   front end now treats it as an empty clip until the next clip, as cairo
   and GDI do.
3. **JPEG larger than the bitmap admission** (1024x1024 pixels, 4096 per
   side, fx-14): decoded at 1/2, 1/4 or 1/8 in the DCT domain instead of
   being refused (a broken-object box before).
4. **SVG with only a viewBox** (typical logos): libsvgtiny took the missing
   width/height from the viewport, which NetSurf gives as 0x0 before layout
   and width x 1 for block-level objects; the drawing was 1 px high and then
   stretched. The viewBox aspect ratio now fills in the missing size.
5. Upstream bug: the broken-object glyph length used `sizeof` of a pointer.
6. The decoded-bitmap cache is sized from the image ledger (3/4 of it)
   instead of a quarter of the 2 MiB `memory_cache_size` (512 KiB), which
   dropped every off-screen bitmap of an illustrated page.
7. Local-history thumbnails are not made: the CiukiOS chrome has no
   local-history view, and each history add/update cost a scaled redraw of
   the page and 46 KB of the image ledger.

Cost (the second half of the trial's complaint):

1. **Allocator and stdio locks were system calls.** newlib's `malloc`,
   `free` and stdio locks are SDK pthread mutexes, and
   `pthread_mutex_unlock` always issued `WAKE_WORD`. One article load makes
   about 2.7 million allocator calls; each became a kernel entry with an
   address-space walk (`ww_bind`). The unlock now wakes only a registered
   waiter, race-free under SMP by Dekker ordering (full-barrier release
   before reading the waiter count; full-barrier registration before the
   kernel compares the owner word). This applies to every native program.
2. **Heap growth**: newlib extends the heap in 4 KiB steps (`DEFAULT_TOP_PAD`
   0) and the kernel makes each `brk` extension a separate extent with its
   own memory object, walked linearly by faults and mapping calls. NetSurf
   now sets a 1 MiB top pad and a 4 MiB trim threshold (`mallopt`).
3. **Clock reads**: the layout deadline read `clock_gettime` (a system call)
   at every check (about 30,000 reads in one remote main-page layout pass,
   several times more for a long article); it now reads it on a span's
   first check and every 16th, never for an untimed layout without a
   desktop channel; font measures are timed with the
   time stamp counter.
4. **Font measuring**: libfont rasterised a glyph to learn its advance and
   scanned its 128-entry cache linearly. Measures now use a 512-entry
   metrics cache per face (same hinted load, no rendering: identical
   advances), Latin-1 code points skip the cmap search, bitmap lookups use
   bucket chains, and faces without a kern table skip the kerning call.

Measured on the host (snapshots of an encyclopedia main page and a long
article, 800x600, medians of five runs): first layout pass 6.3 → 4.0 ms and
38.6 → 22.9 ms, font measuring 3.9 → 1.9 ms and 17.7 → 8.5 ms; per load
0.98 M and 2.66 M allocator lock releases no longer enter the kernel; a
remote layout pass reads the clock ~1,400 instead of ~30,000 times. Style
selection (libcss) is now the largest CPU share of a page (~25 %). T23
estimate from the trial's own CPU accounting (user mode ≈ x87 the host):
that main-page layout ≈ 0.3 s instead of 3.6 s, its whole first display
≈ 4 s of CPU after the download. Details: `build/codex/fx-24-report.md`.
Remaining kernel item: `brk` growth creates one extent per call and extent
lists are walked linearly (`src/kernel/proc/uaddr.c`); NetSurf avoids it
with its 1 MiB pad, other programs do not.

### FX-24B: shared clipboard, large raster images and CSS selection

Expected behaviour and implementation contract (2026-10-24):

- Windows 98 applications exchange selected text through the system clipboard.
  The Resource Kit, Appendix G (`wrkappg_2.htm`, Clipboard shortcuts), and
  Microsoft's [clipboard operations](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-operations)
  require one shared store. Bind NetSurf's v3 desktop channel with
  `du_clip_bind`; retain G5's Unicode conversion and ownership protocol.
  Clipboard replies use window/generation zero, unlike window input. Preserve
  window events during a bounded synchronous RPC, close every attachment and
  unbind on shutdown. Page selection and toolbar editing use the same store.
- The [libpng manual](https://www.libpng.org/pub/png/libpng-manual.txt)
  describes progressive rows and raw Adam7 passes; the
  [PNG specification](https://www.w3.org/TR/png-3/) defines their sample lattice.
  Sample selected pixels directly from these rows, including interlaced PNG,
  without allocating a full-size intermediate image. Reuse that decoder for
  cache reconstruction. Keep the original content dimensions for layout.
- [GIF89a](https://www.w3.org/Graphics/GIF/spec-gif89a.txt) defines interlace,
  transparency and disposal. Retain libnsgif's LZW and animation state machine,
  sampling onto a reduced canvas; disposal and restore buffers use the same
  sample lattice. Do not allocate an original-size canvas for a large GIF.
- Google's [WebP decoder API](https://developers.google.com/speed/webp/docs/api)
  supports scaled decoding into external storage. Use it and charge decoder
  working allocations to the existing image ledger too. PNG library allocations
  must also be charged. The ledger remains 2–16 MiB, derived from free memory;
  no OS memory limit changes. Output stays within 4096 pixels per side and
  one megapixel, with smaller output when remaining ledger space requires it.
  Raster inputs above 65535 pixels per side are refused before row allocation.
- [CSS 2 selectors](https://www.w3.org/TR/CSS2/selector.html) and the pinned
  libcss 0.9.2 `src/select/select.c` define matching and cascade order. Remove
  the redundant pending-selector scan: selecting the next chain already
  detects exhaustion. Use a small stack array for class-chain cursors with a
  heap fallback for large class lists. Preserve specificity/source ordering,
  invalidation, pseudo classes, media matching and style sharing. Verify against
  upstream selection expectations and rendered fixtures; measure allocation
  savings and time separately from hardware qualification.
  The strict i686 check found upstream's 6200-byte `css_select_style` frame.
  Keep one reusable heap scratch state per selection context, released with the
  context; reentrant selection borrows a separate state. Results own no scratch
  memory. This keeps the production frame below the required 4 KiB limit.
  Failure cleanup before node-data attachment must leave the borrowed parent
  bloom filter (or static empty root filter) intact; the allocation/error-path
  fixture covers this ownership boundary too.

## Milestones and acceptance

Private site names/URLs and expected pages will be supplied by the lead at
coding time and remain under `legacy/local/`. Tracked tests use synthetic local
fixtures and public standards cases. The lead maps private site identifiers to
milestones without publishing the list. JS/TLS1.3-only sites may be unsupported;
record that result rather than count an error page as a successful browser test.

| Milestone | Deliverable and host tests required in its coding directive | Evidence the lead runs |
| --- | --- | --- |
| M1: local + HTTP | Exact SDK link/import inventory; file/data/resource/about; HTML4/CSS2 fixture subset and UTF-8/required legacy encodings; Ciuki window, font widths, keyboard navigation and URL/back/forward/reload/stop. Host parser tests for fragmented HTTP/chunks/length conflicts/redirects/partial sends/EOF; file no-mmap fallback, MIME/resources, data bounds; fd-set/poll scheduling, DNS failure/offline/timeout/cancel/reentrant callback/OOM; mock v3 surface/input/font integration. Render fixtures specify expected supported features, not all HTML4/CSS2 conformance. | Canonical image + SHA-256; QEMU local HTTP/DNS fixture and missing-NIC/offline/failure/cancellation, UI responsiveness; T23 e100 same fixture and memory/stack/surface measurements at 128 MiB. Record actual HTML/CSS limits. |
| M2: HTTPS | BearSSL native engine with same HTTP path; CA pin/anchor equivalence; TLS loopback mock/server handshake fragmented at every boundary, partial I/O/cancellation; trusted and untrusted root, bad host, expired/not-yet-valid/malformed chain, missing entropy, missing/invalid clock, truncated TLS, protocol mismatch; no downgrade/exception; worker cleanup and bounded allocations. | Lead TLS fixture on canonical QEMU networking, negative outcomes, serial/disk logs and hash; T23 successful TLS1.2 handshake and transfer timings/entropy/time provenance. M2 cannot pass until its external prerequisites land. |
| M3: images + cookies | Pinned GIF/BMP/ICO; optional separately pinned PNG/JPEG. Deterministic decoder pixels, transparency, animation limits, malformed/truncated/oversized and fail-every-allocation tests; gzip checksums/truncation/expansion cap; NetSurf cookie policy with PSL, Secure/path/domain/expiry/redirect isolation; cache eviction never exceeds budget. | Lead QEMU controlled image/cookie/gzip fixtures then private fixture mapping; T23 decode/render/scroll performance, high-water and OOM recovery. Explicit list of enabled formats and unsupported sites. |
| M4: persistence + desktop | Bookmarks/hotlist + history UI/load/save; cookies opt-in; Choices; clipboard, focus/capture-loss, DPI/resize/rapid configure/stale generation, close/disconnect fd/map cleanup; ENOSPC/read-only/corrupt persistence and atomic replacement tests; deterministic two-build/resource/license manifest comparisons; desktop launch arguments and identity. | Lead canonical desktop-launch/persistence/restart QEMU run, short authorized boot smoke when needed, then T23 final integration with serial/disk evidence; previous F6 gates remain green. No intermediate hardware image unless owner requests. |

For each coding milestone, host suites run **one at a time with `nice -n 19`**;
use repository sanitizer settings unchanged, including leak detection wherever
enabled. Include native ABI/layout and existing network/libdeskui/font/SDK
regressions as selected by the lead; browser test command paths do not exist
yet and must be named in the coding directive. This document-only directive
names no executable host suite and forbids builds: the checks actually run are
reported separately in `build/codex/f6-13-report.md`.

Only the lead runs canonical builds, QEMU and hardware evidence, under the
repository resource/test policies. Do not use a second image, compile-time
test variant or public Internet dependence. Evidence is bound to source/SDK/
config and image SHA-256, fixture version and runtime settings. F6-04 closure
requires launch, failure/offline/cancel responsiveness and the measured 128 MiB
footprint, plus M2's qualified TLS/store/time/entropy if HTTPS is claimed.
Rendering one local file is not that closure.

## Open decisions for the lead

1. Approve native HTTP fetcher/BearSSL/libnsfb v3 design, the port-local fd_set
   bridge and one bounded DNS worker; land/qualify f6-10 and correct its older
   network-contract numbering before browser code depends on it.
2. Pin mandatory **zlib** (target and host) and host libpng for framebuffer
   asset conversion, or direct an audited replacement conversion pipeline.
   Record release URL, exact bytes/hash, license and tool versions; decide
   PNG/JPEG timing/pins. These absent pins are blockers, not build fallbacks.
3. Choose charset scope/backend: proposed parserutils adapter and explicitly
   supported encodings versus a separately pinned iconv/SDK extension. Freeze
   private fixture needs without adding private names to tracked documents.
4. Approve the SDK font adapter (`FONTLIB=ciuki`) for M1, or allow a bitmap-only
   bootstrap with documented coverage; identify redistributable system fonts.
5. Supply/qualify cryptographic entropy and wall-time validity interface/policy
   before M2; accept TLS1.2-only and no revocation checking or choose a separate
   future TLS stack directive. No approval here permits weaker certificate checks.
6. Approve HTTP caps/timeouts/keep-alive/redirect policy and explicit multipart,
   proxy, external-handler and advanced-web exclusions; decide whether any form
   uploads/authentication beyond bounded URL-encoded POST are required for beta.
7. Approve or revise the 32 MiB resident target and sublimits after baseline
   measurement; choose UI responsiveness bound and how to gate long synchronous
   layout. No OS memory limit change is authorized by this plan.
8. Confirm `/Programs/netsurf` + `/system/netsurf` resources layout, writable profile
   path, bookmark/history/cookie retention caps/settings, desktop launch/catalogue
   integration and approved icon conversions. Keep user-visible copy English.
9. Supply private site acceptance mapping at code time; write implementation
   directives with exact host commands and lead-run QEMU/T23 checks. Review the
   license/resource inventory, especially MPL PSL/CA data and font/asset notices.

No implementation, image build, runtime qualification, commit or diary change
is part of f6-13. The next action is lead review/decisions and a written coding
directive with the above prerequisites resolved.

## G26 user tools extension

[Network user tools](network-user-tools.md) records the later implemented
extension: streamed downloads with exclusive staging and Save/cancel; explicit
New Window/Ctrl+N through the existing desktop launcher; pinned libpng and
static stb JPEG content handlers sharing the image ledger; Internet Properties
(home, HTTP proxy and memory cache). Each process still owns one framebuffer
surface. G26B adds URL-only `_blank` routing, concurrent profile transactions, HTTPS
CONNECT with Basic proxy authentication, active download progress/cancel and
upstream Duktape/nsgenbind with memory admission. Script text is retained and
charged against the fx-14 DOM budget. Integration retains G31 visible-page
Ctrl+P printing and Ctrl+Shift+P setup; browser printing through G31 and
default-program association integration remain open. G26 PNG/JPEG handlers
register after upstream image handlers while fx-24 GIF/BMP/ICO/WebP/SVG remain
enabled. Host evidence and limits are in `build/codex/g26-report.md` and
`build/codex/g26b-report.md`.
