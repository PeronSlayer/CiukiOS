#!/usr/bin/env bash
# Wine 10.0 PE experiment, upper audit and authored builtins; no OS images.
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
source_dir="$repo/build/external/wine-10.0"
output_dir="$repo/build/host/win32/wine-10.0"
case ${1:-} in
  --help|-h)
    cat <<'EOF'
Usage: build-wine-dlls.sh [SOURCE_DIRECTORY [OUTPUT_DIRECTORY]]
       build-wine-dlls.sh --codecs [OUTPUT_DIRECTORY]
       build-wine-dlls.sh --installer-builtins [OUTPUT_DIRECTORY]
       build-wine-dlls.sh --upper-audit PE_DIRECTORY OUTPUT_JSON
       build-wine-dlls.sh --upper-source-audit PE_DIRECTORY OUTPUT_JSON
--codecs builds the original P5 implementations; it needs no Wine tree.
--installer-builtins builds the original DirectSetup identity DLLs.
--upper-audit checks actual eager/delayed imports and recursive PE forwards.
--upper-source-audit compares pinned source candidates with actual core PEs.
The latter is discovery evidence, not proof of linked imports or runtime loads.
Uses the hash-pinned Wine 10.0 sources, clang/lld, llvm-ar, llvm-dlltool,
and the explicitly pinned host wrc. All builds run sequentially at nice 19.
Outputs: tools/{winebuild,winegcc}, version.dll, version-pe.json, hashes.json.
This reproduces the small PE experiment, not a full Wine build or ABI gate.
Fetch the archive in config/wine-pin.json outside the sandbox and verify its
SHA-256 before extraction; the script additionally checks every audited file.
EOF
    exit 0 ;;
esac
case ${1:-} in
  --installer-builtins)
    [[ $# -le 2 ]] || { echo 'Too many arguments' >&2; exit 2; }
    exec nice -n 19 python3 "$repo/scripts/win32/upper_dlls.py" build-builtins \
        "${2:-$repo/build/host/win32/upper/pe}"
    ;;
  --upper-audit|--upper-source-audit)
    [[ $# -eq 3 ]] || { echo 'Expected PE_DIRECTORY OUTPUT_JSON' >&2; exit 2; }
    command=audit
    [[ $1 != --upper-source-audit ]] || command=source-audit
    exec nice -n 19 python3 "$repo/scripts/win32/upper_dlls.py" "$command" "$2" "$3"
    ;;
esac
if [[ ${1:-} == --codecs ]]; then
    [[ $# -le 2 ]] || { echo 'Too many arguments' >&2; exit 2; }
    exec nice -n 19 python3 "$repo/scripts/win32/codecs.py" "${2:-$repo/build/host/win32/codecs/pe}"
fi
source_dir=${1:-$source_dir}
output_dir=${2:-$output_dir}
[[ $# -le 2 ]] || { echo 'Too many arguments' >&2; exit 2; }
[[ -d $source_dir ]] || {
    echo "Pinned Wine 10.0 source tree unavailable: $source_dir" >&2
    echo 'Stage the pinned release archive or complete tagged sources; installed Wine is not a substitute.' >&2
    exit 2
}
source_dir=$(cd -- "$source_dir" && pwd)
mkdir -p -- "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd)
[[ $output_dir != "$source_dir" && $output_dir != "$source_dir/"* ]] || { echo 'Use an out-of-tree build' >&2; exit 2; }

run() { nice -n 19 "$@"; }
run python3 "$repo/scripts/win32/wine_audit.py" verify-source "$source_dir"
run python3 - "$repo/config/wine-pin.json" <<'PY'
import hashlib, json, pathlib, shutil, sys
pin = json.loads(pathlib.Path(sys.argv[1]).read_text())
for name, info in pin['tools'].items():
    path = shutil.which(name)
    if not path:
        sys.exit('Missing pinned tool: ' + name)
    if hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest() != info['sha256']:
        sys.exit('Tool hash differs from experiment pin: ' + name + '; lead must requalify it')
PY

mkdir -p -- "$output_dir/include" "$output_dir/tools"
# A small Linux host-tool bootstrap, not a replacement for upstream configure.
# These are the configuration macros actually used by winebuild/winegcc here.
cat > "$output_dir/include/config.h" <<'EOF'
#ifndef __WINE_CONFIG_H
#define __WINE_CONFIG_H
#define PACKAGE_VERSION "10.0"
#define EXEEXT ""
#define HAVE_SIGPROCMASK 1
#define CC "clang"
#define CXX "clang++"
#define CPP "clang -E"
#define LD "ld.lld"
#define LIBDIR "/usr/lib"
#define INCLUDEDIR "/usr/include"
#endif
EOF

# Relative input paths keep assert/__FILE__ paths stable across checkouts.
cd -- "$source_dir"
run clang -O2 -I"$output_dir/include" -Iinclude tools/winebuild/*.c -o "$output_dir/tools/winebuild"
run clang -O2 -I"$output_dir/include" -Iinclude tools/winegcc/winegcc.c tools/winegcc/utils.c -o "$output_dir/tools/winegcc"

pe_flags=(--target=i686-windows --no-default-config -march=pentiumpro
          -mno-sse -mno-sse2 -O2 -D__WINE_PE_BUILD -D__WINESRC__ -D_UCRT
          -fno-builtin -fshort-wchar -Wno-pragma-pack -Iinclude/msvcrt -Iinclude)
run clang "${pe_flags[@]}" -c dlls/version/version.c -o "$output_dir/version.o"
for unit in debug dll_main crt_dllmain; do
    run clang "${pe_flags[@]}" -c "dlls/winecrt0/$unit.c" -o "$output_dir/$unit.o"
done
run llvm-ar rcs "$output_dir/libwinecrt0.a" "$output_dir/debug.o" "$output_dir/dll_main.o" "$output_dir/crt_dllmain.o"
for dll in kernelbase ucrtbase kernel32 ntdll; do
    run "$output_dir/tools/winebuild" -b i686-windows --implib \
        -E "dlls/$dll/$dll.spec" -o "$output_dir/lib$dll.a"
done
# llvm-rc cannot parse the adjacent string-literal BLOCK in wine_common_ver.rc.
# This host's wrc is pinned separately; --nostdinc prevents 11.18 headers leaking in.
run wrc --nostdinc -Iinclude -o "$output_dir/version.res" dlls/version/version.rc
export WINEBUILD="$output_dir/tools/winebuild"
run "$output_dir/tools/winegcc" -b i686-windows -mno-cygwin -shared \
    -Wl,--wine-builtin -Wl,-timestamp:0 -L"$output_dir" \
    dlls/version/version.spec "$output_dir/version.o" "$output_dir/version.res" \
    -lkernelbase -lucrtbase -lkernel32 -lntdll -o "$output_dir/version.dll"
run llvm-readobj --file-headers --coff-imports --coff-exports "$output_dir/version.dll" > "$output_dir/version-readobj.txt"
run python3 "$repo/scripts/win32/wine_audit.py" pe "$output_dir/version.dll" "$output_dir/version-pe.json"
run python3 - "$output_dir" <<'PY'
import hashlib, json, pathlib, sys
out = pathlib.Path(sys.argv[1])
files = ['tools/winebuild', 'tools/winegcc', 'version.dll']
hashes = {name: hashlib.sha256((out / name).read_bytes()).hexdigest() for name in files}
(out / 'hashes.json').write_text(json.dumps(hashes, indent=2, sort_keys=True) + '\n')
print(json.dumps(hashes, indent=2))
PY
