#!/usr/bin/env bash
set -euo pipefail
out="${1:-build/tests/win32-probes}"
mkdir -p "$out"
llvm-dlltool -m i386 -k -d src/probes/win32/kernel32.def -l "$out/kernel32.lib"
llvm-dlltool -m i386 -k -d src/probes/win32/advapi32.def -l "$out/advapi32.lib"
for kind in HELLO SETUP; do
    args=()
    if [[ "$kind" == SETUP ]]; then args+=(/DPROBE_INSTALL=1); fi
    clang-cl --target=i686-pc-windows-msvc /nologo /c /GS- /O1 /Zl \
        /clang:-fno-builtin "${args[@]}" \
        /Fo"$out/$kind.obj" src/probes/win32/probe.c
    lld-link /nologo /machine:x86 /subsystem:console,4.0 /osversion:4.0 \
        /entry:mainCRTStartup \
        /nodefaultlib /out:"$out/$kind.EXE" "$out/$kind.obj" \
        "$out/kernel32.lib" "$out/advapi32.lib"
done
file "$out/HELLO.EXE" "$out/SETUP.EXE"
