#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build/tests"
OUT_BIN="$BUILD_DIR/mz_probe"
CORPUS_DIR="$PROJECT_DIR/third_party/freedos/runtime"
MIN_PARSED="${CIUKIOS_MZ_MIN_PARSED:-5}"
MAX_PARSE_FAILED="${CIUKIOS_MZ_MAX_PARSE_FAILED:-4}"

if [[ ! -d "$CORPUS_DIR" ]]; then
    echo "[FAIL] corpus directory not found: $CORPUS_DIR" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"

clang -std=c11 -Wall -Wextra -I"$PROJECT_DIR/stage2/include" \
    "$PROJECT_DIR/stage2/tests/mz_probe.c" \
    "$PROJECT_DIR/stage2/src/dos_mz.c" \
    -o "$OUT_BIN"

total_exe=0
mz_signature=0
parsed_ok=0
parse_failed=0
non_mz=0

while IFS= read -r exe_path; do
    total_exe=$((total_exe + 1))

    set +e
    "$OUT_BIN" "$exe_path"
    rc=$?
    set -e

    if [[ $rc -eq 0 ]]; then
        parsed_ok=$((parsed_ok + 1))
        mz_signature=$((mz_signature + 1))
        continue
    fi

    if [[ $rc -eq 2 ]]; then
        non_mz=$((non_mz + 1))
        echo "[probe] SKIP non-MZ: $exe_path"
    elif [[ $rc -eq 1 ]]; then
        parse_failed=$((parse_failed + 1))
        mz_signature=$((mz_signature + 1))
        echo "[probe] FAIL parse: $exe_path" >&2
    else
        echo "[probe] FAIL read/error: $exe_path (rc=$rc)" >&2
        exit 1
    fi

done < <(find "$CORPUS_DIR" -type f -iname '*.exe' | sort)

echo "[summary] exe_total=$total_exe mz_signature=$mz_signature parsed_ok=$parsed_ok non_mz=$non_mz parse_failed=$parse_failed"

if [[ $total_exe -eq 0 ]]; then
    echo "[FAIL] no EXE files found in corpus" >&2
    exit 1
fi

if [[ $parse_failed -ne 0 ]]; then
    if [[ $parse_failed -gt $MAX_PARSE_FAILED ]]; then
        echo "[FAIL] MZ parse failures ($parse_failed) exceed threshold ($MAX_PARSE_FAILED)" >&2
        exit 1
    fi
    echo "[WARN] MZ parse failures within threshold ($parse_failed <= $MAX_PARSE_FAILED)"
fi

if [[ $mz_signature -lt $MIN_PARSED ]]; then
    echo "[FAIL] MZ-signature files ($mz_signature) below minimum threshold ($MIN_PARSED)" >&2
    exit 1
fi

if [[ $parsed_ok -lt $MIN_PARSED ]]; then
    echo "[FAIL] parsed MZ files ($parsed_ok) below minimum threshold ($MIN_PARSED)" >&2
    exit 1
fi

echo "[PASS] MZ runtime corpus harness"
