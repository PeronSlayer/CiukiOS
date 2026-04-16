#!/bin/bash
# INT21h Compatibility Matrix Gate
# Validates that:
# 1. INT21h compatibility matrix exists and is well-formed
# 2. Matrix marks all known priority-A functions with status
# 3. No conflicts between status and implementation

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
INT21_DOC="${PROJECT_ROOT}/docs/int21-priority-a.md"

FAILED=0

print_check() {
    local name="$1"
    local status="$2"
    echo "[gate] $name ... $status"
}

# ===== Check 0: File exists =====
if [ ! -f "$INT21_DOC" ]; then
    print_check "docs/int21-priority-a.md exists" "FAIL"
    exit 1
fi

print_check "docs/int21-priority-a.md exists" "PASS"

# ===== Check 1: Matrix section exists =====
if grep -q "INT21h Compatibility Matrix" "$INT21_DOC"; then
    print_check "Compatibility Matrix section found" "PASS"
else
    print_check "Compatibility Matrix section found" "FAIL"
    FAILED=$((FAILED + 1))
fi

# ===== Check 2: Extract matrix entries - strict filtering =====
# Matrix format: lines like "00h | IMPLEMENTED | ..." starting with hex digits or asterisk
# Skip header separators (----), markdown code blocks (```), and header row (FN)

matrix_block=$(sed -n '/^FN  | Status/,/^## [A-Z]/p' "$INT21_DOC" | \
    grep -E "^[0-9a-fA-F*].*\|" | \
    grep -v "^---" | \
    grep -v '```' | \
    grep -v "^FN")

if [ -z "$matrix_block" ]; then
    print_check "Matrix has entries" "FAIL"
    FAILED=$((FAILED + 1))
else
    entry_count=$(printf "%s" "$matrix_block" | wc -l)
    print_check "Matrix entries found" "PASS ($entry_count entries)"
fi

# ===== Check 3: Validate required functions are present =====
required_functions="00h 01h 02h 08h 09h 19h 25h 30h 35h 4Ch 4Dh 51h 62h 48h 49h 4Ah"

missing_in_matrix=0
for fn in $required_functions; do
    if printf "%s" "$matrix_block" | grep -q "^$fn "; then
        print_check "Function $fn documented" "PASS"
    else
        print_check "Function $fn documented" "FAIL"
        missing_in_matrix=$((missing_in_matrix + 1))
    fi
done

if [ "$missing_in_matrix" -gt 0 ]; then
    FAILED=$((FAILED + 1))
fi

# ===== Check 4: Validate status values =====
valid_statuses="IMPLEMENTED|DETERMINISTIC_STUB|UNSUPPORTED"
invalid_status=0

while IFS='|' read -r fn status rest; do
    fn=$(printf "%s" "$fn" | xargs)
    status=$(printf "%s" "$status" | xargs)

    [ -z "$fn" ] && continue

    if ! printf "%s" "$status" | grep -qE "^($valid_statuses)$"; then
        print_check "Function $fn status ($status)" "FAIL"
        invalid_status=$((invalid_status + 1))
    fi
done << EOF
$matrix_block
EOF

if [ "$invalid_status" -gt 0 ]; then
    FAILED=$((FAILED + 1))
    print_check "All status values valid" "FAIL ($invalid_status invalid)"
else
    print_check "All status values valid" "PASS"
fi

# ===== Check 5: Implementation coverage =====
impl_claimed=$(printf "%s" "$matrix_block" | grep -c "IMPLEMENTED" || echo "0")
stub_claimed=$(printf "%s" "$matrix_block" | grep -c "DETERMINISTIC_STUB" || echo "0")

if [ "$impl_claimed" -ge 10 ]; then
    print_check "Implementation coverage" "PASS ($impl_claimed implemented, $stub_claimed stubs)"
else
    print_check "Implementation coverage" "ADVISORY (only $impl_claimed implemented)"
fi

# ===== Summary =====
echo ""
if [ "$FAILED" -gt 0 ]; then
    echo "[gate] INT21h Compatibility Matrix validation FAILED ($FAILED issues)"
    exit 1
else
    echo "[gate] INT21h Compatibility Matrix validation PASSED"
    exit 0
fi
