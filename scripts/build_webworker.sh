#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DJGPP_ROOT="${CIUKIOS_DJGPP_ROOT:-$ROOT_DIR/build/external/djgpp}"
DJGPP_CC="${DJGPP_CC:-$DJGPP_ROOT/bin/i586-pc-msdosdjgpp-gcc}"
HOST_CC="${HOST_CC:-cc}"
if (( $# > 1 )); then
    echo "usage: $0 [output-directory]" >&2
    exit 2
fi
OUT_DIR="${1:-${CIUKIOS_WEBWORKER_OUT:-$ROOT_DIR/build/webworker}}"
GEN_DIR="$OUT_DIR/generated"
OBJ_DIR="$OUT_DIR/obj"

[[ -x "$DJGPP_CC" ]] || {
    echo "[build-webworker] ERROR: DJGPP compiler missing: $DJGPP_CC" >&2
    exit 1
}
command -v "$HOST_CC" >/dev/null 2>&1 || {
    echo "[build-webworker] ERROR: host C compiler missing: $HOST_CC" >&2
    exit 1
}

mkdir -p "$GEN_DIR" "$OBJ_DIR"

# MQuickJS generates both headers from the same 32-bit target description.
"$HOST_CC" -std=gnu99 -O2 \
    -I third_party/jsvendor \
    third_party/jsvendor/mquickjs_build.c \
    third_party/jsvendor/cutils.c \
    third_party/jsvendor/worker_js_stdlib.c \
    -lm -o "$GEN_DIR/mquickjs_build"
"$GEN_DIR/mquickjs_build" -m32 -a > "$GEN_DIR/mquickjs_atom.h"
"$GEN_DIR/mquickjs_build" -m32 > "$GEN_DIR/worker_js_stdlib.h"

common_flags=(
    -O2 -std=gnu99 -fno-strict-aliasing
    -I "$ROOT_DIR/src/web"
    -I "$ROOT_DIR/third_party/jsvendor"
    -I "$ROOT_DIR/third_party/bearssl/inc"
    -I "$ROOT_DIR/third_party/bearssl/src"
    -I "$GEN_DIR"
    -DBR_RDRAND=0 -DBR_USE_URANDOM=0
)

objects=()
object_index=0
compile_one() {
    local source="$1"
    shift
    local object="$OBJ_DIR/obj-${object_index}.o"
    object_index=$((object_index + 1))
    echo "[build-webworker] CC $source"
    "$DJGPP_CC" "${common_flags[@]}" "$@" -c "$source" -o "$object"
    objects+=("$object")
}

# The engine and its adapter share a custom class count. Other translation
# units, especially the independently vendored BearSSL files, do not.
for source in \
    src/web/worker.c \
    src/web/worker_ca.c \
    src/web/worker_rng.c \
    src/web/worker_tls.c \
    src/web/worker_x509_ip.c \
    src/web/worker_js.c \
    src/web/worker_page.c; do
    compile_one "$source"
done

for source in \
    third_party/jsvendor/mquickjs.c \
    third_party/jsvendor/dtoa.c \
    third_party/jsvendor/libm.c \
    third_party/jsvendor/cutils.c; do
    compile_one "$source" -include "$ROOT_DIR/src/web/worker_js_config.h"
done

bearssl_sources=()
while IFS= read -r -d '' source; do
    bearssl_sources+=("$source")
done < <(find third_party/bearssl/src -type f -name '*.c' -print0 | sort -z)
(( ${#bearssl_sources[@]} > 0 )) || {
    echo "[build-webworker] ERROR: no BearSSL C sources found" >&2
    exit 1
}
for source in "${bearssl_sources[@]}"; do
    compile_one "$source"
done

echo "[build-webworker] LINK $OUT_DIR/WEBWORK.EXE"
"$DJGPP_CC" -O2 "${objects[@]}" -lm -o "$OUT_DIR/WEBWORK.EXE"
echo "[build-webworker] done: $OUT_DIR/WEBWORK.EXE"
