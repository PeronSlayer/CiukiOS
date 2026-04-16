#!/bin/bash
# FreeDOS Pipeline Validation Harness
# Ensures FreeDOS import/build artifacts are present and consistent.
# Exit code: 0 if all checks pass, non-zero on any failure.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
FREEDOS_RUNTIME="${PROJECT_ROOT}/third_party/freedos/runtime"
FREEDOS_MANIFEST="${PROJECT_ROOT}/third_party/freedos/manifest.csv"
RUNTIME_MANIFEST="${PROJECT_ROOT}/third_party/freedos/runtime-manifest.csv"
UPSTREAM_LOCK="${PROJECT_ROOT}/third_party/freedos/upstreams.lock"

FAILED=0

print_check() {
    local name="$1"
    local status="$2"
    echo "[validate] $name ... $status"
}

# ===== Check 1: Manifest exists =====
if [ ! -f "$FREEDOS_MANIFEST" ]; then
    print_check "manifest.csv exists" "FAIL"
    FAILED=$((FAILED + 1))
else
    print_check "manifest.csv exists" "PASS"
fi

# ===== Check 2: Runtime directory exists =====
if [ ! -d "$FREEDOS_RUNTIME" ]; then
    print_check "runtime directory exists" "FAIL"
    FAILED=$((FAILED + 1))
else
    print_check "runtime directory exists" "PASS"
fi

# ===== Check 3: Manifest is well-formed =====
if [ -f "$FREEDOS_MANIFEST" ]; then
    # Basic CSV format check: count columns in header
    header_cols=$(head -1 "$FREEDOS_MANIFEST" | awk -F',' '{print NF}')

    # Check each data line has same column count (skip empty lines)
    malformed=0
    while IFS=',' read -r component file_name required imported sha256 source_path license notes restofline; do
        [ -z "$component" ] && continue
        # Re-count the line cols to verify integrity
        line_cols=$(printf "%s" "$component,$file_name,$required,$imported,$sha256,$source_path,$license,$notes" | awk -F',' '{print NF}')
        if [ "$line_cols" -lt 8 ]; then
            malformed=$((malformed + 1))
        fi
    done < <(tail -n +2 "$FREEDOS_MANIFEST")

    if [ "$malformed" -eq 0 ]; then
        print_check "manifest CSV format valid" "PASS"
    else
        print_check "manifest CSV format valid" "FAIL (malformed rows)"
        FAILED=$((FAILED + 1))
    fi
else
    print_check "manifest exists (for format check)" "SKIP"
fi

# ===== Check 4: Required files are present =====
if [ -f "$FREEDOS_MANIFEST" ]; then
    # Reset malformed count for required file checks
    required_missing=0
    while IFS=',' read -r component file_name required imported sha256 source_path license notes restofline; do
        [ -z "$file_name" ] && continue

        if [ "$required" = "yes" ]; then
            if [ ! -f "$FREEDOS_RUNTIME/$file_name" ]; then
                print_check "required file $file_name present" "FAIL"
                required_missing=$((required_missing + 1))
            else
                print_check "required file $file_name present" "PASS"
            fi
        fi
    done < <(tail -n +2 "$FREEDOS_MANIFEST")

    if [ "$required_missing" -gt 0 ]; then
        FAILED=$((FAILED + 1))
    fi
fi

# ===== Check 5: freecom sources available (optional) =====
FREECOM_SOURCES="${PROJECT_ROOT}/third_party/freedos/sources/freecom"
if [ -d "$FREECOM_SOURCES/.git" ]; then
    print_check "freecom git repo available" "PASS"
else
    print_check "freecom git repo available" "SKIP (optional)"
fi

# ===== Check 6: Runtime manifest reproducibility =====
if [ -f "$RUNTIME_MANIFEST" ]; then
    tmp_runtime_manifest="$(mktemp)"
    trap 'rm -f "$tmp_runtime_manifest"' EXIT

    bash "${PROJECT_ROOT}/scripts/generate_freedos_runtime_manifest.sh" "$tmp_runtime_manifest" >/dev/null

    if cmp -s "$RUNTIME_MANIFEST" "$tmp_runtime_manifest"; then
        print_check "runtime manifest reproducible" "PASS"
    else
        print_check "runtime manifest reproducible" "FAIL"
        FAILED=$((FAILED + 1))
    fi

    rm -f "$tmp_runtime_manifest"
    trap - EXIT
else
    print_check "runtime manifest present" "FAIL"
    FAILED=$((FAILED + 1))
fi

# ===== Check 7: Upstream lock presence =====
if [ -f "$UPSTREAM_LOCK" ]; then
    if grep -q '^freecom.repo=' "$UPSTREAM_LOCK" && grep -q '^freecom.commit=' "$UPSTREAM_LOCK"; then
        print_check "upstream lock file" "PASS"
    else
        print_check "upstream lock file" "FAIL"
        FAILED=$((FAILED + 1))
    fi
else
    print_check "upstream lock file" "FAIL"
    FAILED=$((FAILED + 1))
fi

# ===== Check 8: OpenGEM GUI payload (optional) =====
OPENGEM_RUNTIME="${FREEDOS_RUNTIME}/OPENGEM"
OPENGEM_REQUIRED="${CIUKIOS_REQUIRE_OPENGEM:-0}"

# OpenGEM launch candidates in priority order
opengem_find_entry() {
    local dir="$1"
    for cand in GEM.BAT GEM.EXE DESKTOP.APP OPENGEM.BAT OPENGEM.EXE; do
        hit=$(find "$dir" -maxdepth 3 -iname "$cand" -type f 2>/dev/null | head -n1)
        if [ -n "$hit" ]; then
            echo "$hit"
            return 0
        fi
    done
    return 1
}

if [ "$OPENGEM_REQUIRED" = "1" ]; then
    # Strict mode: OpenGEM payload and entry must be present
    if [ -d "$OPENGEM_RUNTIME" ]; then
        print_check "OpenGEM payload directory present (required)" "PASS"
    else
        print_check "OpenGEM payload directory present (required)" "FAIL"
        FAILED=$((FAILED + 1))
    fi

    entry=$(opengem_find_entry "$OPENGEM_RUNTIME" 2>/dev/null || true)
    if [ -n "$entry" ]; then
        print_check "OpenGEM runnable entry present (required)" "PASS ($(basename "$entry"))"
    else
        print_check "OpenGEM runnable entry present (required)" "FAIL"
        FAILED=$((FAILED + 1))
    fi

    # Verify manifest entries for opengem are imported
    if [ -f "$FREEDOS_MANIFEST" ]; then
        opengem_imported=$(grep "^opengem,GEM.BAT," "$FREEDOS_MANIFEST" 2>/dev/null | cut -d',' -f4)
        if [ "$opengem_imported" = "yes" ]; then
            print_check "OpenGEM manifest entry imported" "PASS"
        else
            print_check "OpenGEM manifest entry imported" "FAIL"
            FAILED=$((FAILED + 1))
        fi
    fi
else
    # Info mode: report presence without failing
    if [ -d "$OPENGEM_RUNTIME" ]; then
        entry=$(opengem_find_entry "$OPENGEM_RUNTIME" 2>/dev/null || true)
        if [ -n "$entry" ]; then
            print_check "OpenGEM GUI payload" "PASS (present, entry: $(basename "$entry"))"
        else
            print_check "OpenGEM GUI payload" "PASS (present, no runnable entry)"
        fi
    else
        print_check "OpenGEM GUI payload" "INFO (absent, optional)"
    fi
fi

# ===== Summary =====
echo ""
if [ "$FAILED" -gt 0 ]; then
    echo "[validate] FreeDOS pipeline validation FAILED ($FAILED issues)"
    exit 1
else
    echo "[validate] FreeDOS pipeline validation PASSED"
    exit 0
fi
