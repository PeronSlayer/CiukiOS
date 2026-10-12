# Developing native CiukiOS applications

Classic computing, one woof at a time.

CiukIDE and Tiny C Compiler let you write and build native C applications on
CiukiOS. Open Start > Programs > Developer > CiukIDE. The same static SDK also
supports host builds. This guide is installed as
`/Documents/Developer/Guide.txt`; sample projects are in
`/Documents/Developer/Samples`. The public API reference is generated from the
headers as `docs/developer-api.md`, and installed in `Developer/API` as one
text file per header. Open these text files with CiukNote through Files.

## The native program model

A native application is a static ELF32 i386 executable. Each process has its
own address space, heap, stack, file descriptors and working directory. The
kernel admits page-separated executable/read-only/writable segments starting
at 0x00400000, with a non-executable stack. The SDK's `crt0.o` starts the C
runtime, receives argc/argv/environment and auxv, initializes newlib's main
reentrancy record and pthread state using the kernel's GS thread block,
runs constructors, calls main, then runs exit callbacks and destructors.
Return an exit code from main; do not replace the CRT or issue Linux syscalls.

The C ABI is ILP32: int, long and pointers are 32 bits; long long is 64 bits.
Floating point uses x87, including 12-byte long double. No SSE/MMX instructions
are needed. Compiler TLS (`__thread` or `_Thread_local`), dynamic linking,
shared libraries and PIE executables are excluded. Use pthread keys for
thread-local application data. A process cannot give itself desktop display,
input or termination authority by changing argv.

The POSIX subset includes ordinary files and directories, open/read/write,
positional I/O, seek, rename, unlink, mkdir, stat, descriptor duplication,
message channels and nonblocking fcntl, clocks, sleep, signals, waitpid, process creation
through `ciuki_spawn`, mmap, pthreads and synchronization. It uses pinned
newlib 4.5.0.20241231, with full stdio, allocation, strings, character
classification, integer conversion, locale/time facilities and libm. File
operations remain subject to mounted filesystem capabilities and permissions.
Fork, exec, POSIX pipe, dlopen, sockets/networking, poll/select and unsupported device
operations are excluded or return ENOSYS/EOPNOTSUPP. `system` and `popen` do
not provide a native shell. Consult `docs/sdk.md`, the installed headers and
their error returns instead of assuming every POSIX facility is implemented.

## Compile on CiukiOS

The installed compiler is `/Programs/tcc`. Its default SDK is `/System/sdk`:
`include` contains newlib and `ciuki/*.h`, `deskui.h` and toolkit headers;
`lib` contains crt0, libc, libm, libpthread, libciuki, libcompiler,
libdeskui and libtcc1. `tcc-include` contains TCC's compiler-specific headers.
`ciuki.ld` and `abi.ld` in `lib` are the host SDK linker scripts. TCC's
built-in linker implements their static segment layout directly; it does not
interpret the full GNU SECTIONS script.

From a launcher or native program that can supply argv, the following command
produces an executable in the current folder:

```sh
/Programs/tcc -o hello hello.c -ldeskui
```

CiukIDE supplies argv without a shell. The desktop Run dialog can invoke tcc
with a small argument list; use absolute source/output paths there since its
working directory is not the project folder. Compiler output goes to the
console outside CiukIDE. To execute a graphical application, use CiukIDE's
Run command so the desktop supplies its private channel at fd 3. Executables
outside /Programs are launched from project folders under /Documents.

TCC 0.9.27 implements C99 with upstream limitations (including no complex or
imaginary arithmetic); it is not a full optimizing GCC/Clang replacement.
The port supports C compilation, preprocessing (`-E`), objects (`-c`), static
archives (`-ar`), multiple sources, includes (`-I`), defines (`-D`) and static
libraries (`-l`). Place libraries after sources, as with a traditional linker.
The runtime archives are rescanned to resolve their mutual dependencies.
There is one i386 target: omit `-m32`/`-m64`. `-shared`, `-run`, alternate
image layouts, compiler TLS and dynamic dependencies are rejected. Bounds
instrumentation and runtime backtraces are unavailable. Use `-g` for compiler
source diagnostics; there is no IDE debugger in this version.

TCC is LGPL; `/System/sdk/TCC.TXT` contains its notice and full licence.
libtcc1 has upstream's additional linking permission. Compiler sources,
patches, pins and build recipe are in `/System/sdk/tcc-source` and in the
repository's `third_party/tcc` and `apps/tcc`. The SDK, newlib, libdeskui and
the Ciuki portrait have separate notices; read `sdk/README.md` for the
existing public ABI header licensing boundary.

## Your first Hello window project

1. Open CiukIDE from Start > Programs > Developer.
2. Choose Project > New Project: Hello window. Accept
   `/Documents/HelloWindow`, or enter another unused direct child folder of
   /Documents. CiukIDE creates main.c and project.cfg from the shipped sample.
   An existing folder is never overwritten.
3. Select main.c in the project list and edit the greeting. The editor uses
   CiukNote's UTF-8 editing model, selection, undo, clipboard, scrollbars and
   a 61,440-byte document limit. C keywords, comments, strings, numbers and
   preprocessor lines have distinct colours; the gutter shows source lines.
4. Press F7 or Build. CiukIDE saves the active file, reloads project.cfg and
   starts /Programs/tcc as a child. Both stdout and stderr are drained while
   the child runs. The lower panes show diagnostics and compiler output.
5. A successful build enables F5/Run. Press it; the desktop opens Hello Ciuki.
   Click Woof! to change the label. Close the sample window normally.
6. Introduce a syntax error and press F7. Select the resulting diagnostic to
   open its source file and jump to the reported line. Correct it and build
   again. Run remains disabled after failed or stale builds.

Use Project > New Project: Hello console for a printf-based program. Its
output appears in the system console when launched; it has no desktop
window. Build diagnostics always remain in CiukIDE. The compiler exits after
each build. Closing CiukIDE saves the active document and terminates/reaps an
active compiler child. A failed save leaves the editor open with an error.
Open another project with Open Project and its absolute folder path. Source
files open from the project list; switching files saves the previous buffer.
Save also uses Ctrl+S. Edit supplies Ctrl+Z/X/C/V/A. F1 opens this guide.

## Project configuration

A project is a folder containing project.cfg. Example:

```ini
sources=main.c widgets.c
output=my-app
libraries=deskui m
```

Select project.cfg in the file list, edit it, save, then Build to reload it.
`sources` and `output` are required; `libraries` may be empty or omitted.
Source paths end in .c and are relative to the folder. Subfolders are allowed
for sources and output if they already exist. Each path component uses ASCII
letters, digits, dot, underscore or hyphen; spaces, absolute paths, empty
components, `.` and `..` are rejected. Libraries are names such as deskui,
m or pthread (an optional -l prefix is accepted). This format does not execute
shell commands, response files or arbitrary compiler options. Keys may occur
once; unknown keys are errors. Output paths cannot alias a source or project.cfg,
including case differences on FAT32. Full-line # comments and LF/CRLF are supported.
Limits are 4 KiB config, 16 source files, eight libraries, 159-byte source
paths and a 63-byte relative output name. Complete project paths must fit
the desktop launch protocol's 160-byte argument. New folders are direct
children of /Documents. Run is available only after a successful build of
the current editor revision; editing during a build invalidates that result.
Each new build removes its previous output first, preventing stale execution.

Compiler output is limited to 16 KiB in the visible pane and 64 diagnostic
entries. CiukIDE continues draining excess bytes to avoid blocking the compiler and
marks truncation in the status bar. Diagnostics longer than 511 bytes are
omitted from the navigable list; the compiler's exit code still controls
success. Only project source files can be opened by a diagnostic; diagnostics
for SDK headers remain visible as text. Failed launches report the desktop's
error and leave the successful build available to retry.

## A small libdeskui application

Include `<deskui.h>`. Parse the desktop's `--channel-fd=3` argument, allocate
`struct du_ui` and `struct du_window`, and call `du_window_open` with that
descriptor, content dimensions and an English title. Use `du_add` to add
labels, buttons and other widgets. Set `ui->event` for widget events; on
`DU_EV_ACTIVATE` update state and mark `ui->dirty=1`. Call `du_run` to dispatch
the channel, input, timers and painting. Finally call `du_window_destroy` and
free your allocations. The complete example is in Samples/HelloWindow/main.c.

libdeskui owns the HELLO/CREATE/CONFIGURE handshake, surface mapping, event
delivery and DAMAGE presentation. The desktop owns window chrome, movement,
close routing and process launch. One channel corresponds to one window;
dialogs are drawn inside that window. Programs without the desktop-owned
channel cannot open a graphical window merely by assuming fd 3 is valid.
Resizing uses `du_window_open_resizable` and a `ui->layout` callback. Draw
with the toolkit's integer canvas API and use the existing Tango icons and
Ciuki identity asset instead of introducing another system icon family.

For process creation include `<ciuki/spawn.h>`. `ciuki_spawn(path, argv, envp,
maps, count, flags)` returns a pid or -1 with errno. The ABI deliberately
inherits only fd 0-2 plus explicit mappings; mapped sources must be open,
inheritable and not FD_CLOEXEC. POSIX pipe currently returns ENOSYS.
CiukIDE therefore creates two `ciuki_channel_pair` pairs, maps the compiler's
endpoints to child fd 1/2, and closes its own copies of those endpoints after
spawn. The target TCC port frames its stdout/stderr bytes into channel
messages through a private stdio adapter. Ordinary console/file output still
uses the SDK's normal write hook. The IDE drains both message queues with
`ciuki_channel_recv(..., DONTWAIT)` and polls waitpid with WNOHANG. Finish
capture only after both channels reach EOF (receive returns zero). Handle
EINTR and EAGAIN. This adapter belongs to TCC; other applications retain the
ordinary newlib stdio behavior. Do not give compiler children the IDE's
desktop channel.

## Build the same project on the host

Use the canonical pinned SDK build first (the lead runs heavy builds in the
repository's capped scope). Application builds use one source state and no
build-time image variants:

```sh
make sdk SDK_ARGS='--jobs 1'
make desktop
make tcc
build/tools/ciuki-sdk/bin/ciuki-cc \
  -I build/apps/tcc/sdk/include \
  sdk/samples/HelloWindow/main.c \
  build/apps/desktop/libdeskui.a -o build/hello-window
build/apps/tcc/tcc-host -Bbuild/apps/tcc/sdk \
  -o build/hello-window-tcc sdk/samples/HelloWindow/main.c -ldeskui
```

The host SDK wrapper selects the pinned Clang/lld tools, CRT, ABI flags and
linker script. tcc-host is the same CiukiOS compiler implementation hosted on
Linux; `-B` points at the prepared SDK. Neither executable is a Linux program.
Host harnesses inspect ELF layouts and exercise application logic; runtime
qualification uses the lead's canonical image on QEMU and supported hardware.
Do not execute a target ELF directly as a Linux application: syscall numbers
and startup TLS are different.

Regenerate the reference after editing public header comments:

```sh
python3 scripts/gen_api_reference.py
python3 scripts/gen_api_reference.py --check
```

The generated file has no timestamps or absolute machine paths. Builds also
generate the installed per-header files from the same public header comments.
Host coverage is in `tests/host/test_native_development.py` and the existing
SDK/desktop harnesses. Guest and hardware qualification are recorded
separately; a host PASS is not a claim about runtime behavior on a T23.
