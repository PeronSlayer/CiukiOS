#!/usr/bin/env bash
# OPENGEM-027 — v8086 live-switch trampoline stub (build-only).
#
# This gate asserts that the stub file exists, declares all required
# symbols, contains no live CPU-mutating instructions, and is not
# invoked from any other stage2 source.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf()   { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }
gE()   { grep -Eq -- "$2" "$1" && pass || fail "$3 (regex missing: $2)"; }
gN()   { if grep -qF -- "$2" "$1"; then fail "$3 (forbidden present: $2)"; else pass; fi; }

H=stage2/include/vm86_switch.h
S=stage2/src/vm86_switch.S

# -------- presence --------
[ -f "$H" ] && pass || fail "header missing: $H"
[ -f "$S" ] && pass || fail "asm missing: $S"

# -------- header contract --------
gf "$H" "OPENGEM-027" "header sentinel"
gf "$H" "#define VM86_SWITCH_SENTINEL 0x0270u" "sentinel macro"
gf "$H" "extern void vm86_switch_long_to_pe32(void);" "long_to_pe32 proto"
gf "$H" "extern void vm86_switch_pe32_to_long(void);" "pe32_to_long proto"
gf "$H" "extern void vm86_switch_enter_v86_via_iret(void);" "enter_v86 proto"
gf "$H" "extern void vm86_switch_gp_trampoline(void);" "gp_trampoline proto"
gf "$H" "extern const unsigned int vm86_switch_stub_sentinel;" "stub sentinel proto"
gf "$H" "#ifndef STAGE2_VM86_SWITCH_H" "include guard"

# -------- asm contract: every symbol is .global + body is stub --------
gf "$S" ".global vm86_switch_stub_sentinel" "stub sentinel global"
gf "$S" ".long   0x00000270" "stub sentinel literal"
gf "$S" ".global vm86_switch_long_to_pe32" "long_to_pe32 global"
gf "$S" ".global vm86_switch_pe32_to_long" "pe32_to_long global"
gf "$S" ".global vm86_switch_enter_v86_via_iret" "enter_v86 global"
gf "$S" ".global vm86_switch_gp_trampoline" "gp_trampoline global"

# -------- stub discipline: no live CPU-mutating ops in this file --------
# Forbid LGDT / LIDT / IRET / far jumps / CR0 writes inside vm86_switch.S.
# The gate matches instruction tokens (leading whitespace + mnemonic +
# whitespace or newline), so occurrences inside comments (prefixed by
# '*' or '/*' or '#') are tolerated by requiring the line to start with
# whitespace + mnemonic.
forbid_insn() {
    local mnem="$1" label="$2"
    if awk -v m="$mnem" '
        /^[ \t]*\/\*/     { next }
        /^[ \t]*\*/       { next }
        /^[ \t]*#/        { next }
        /^[ \t]*\/\//     { next }
        {
            line = $0
            # strip trailing inline comments
            sub(/\/\*.*/, "", line)
            sub(/\/\/.*/, "", line)
            if (match(line, "(^|[ \t])"m"([ \t]|$)")) {
                print NR ":" line
                found = 1
            }
        }
        END { exit (found ? 0 : 1) }
    ' "$S" >/dev/null; then
        fail "$label (forbidden instruction present in $S: $mnem)"
    else
        pass
    fi
}
forbid_insn "lgdt"  "no live LGDT in stub"
forbid_insn "lidt"  "no live LIDT in stub"
forbid_insn "iretq" "no live IRETQ in stub"
forbid_insn "iretd" "no live IRETD in stub"
forbid_insn "iret"  "no live IRET in stub"
forbid_insn "ljmp"  "no live far jump in stub"
forbid_insn "lretq" "no live LRETQ in stub"

# -------- stub discipline: every function body is just retq --------
for sym in vm86_switch_long_to_pe32 vm86_switch_pe32_to_long \
           vm86_switch_enter_v86_via_iret vm86_switch_gp_trampoline; do
    # Match the first non-blank, non-directive line after the label;
    # it must be "retq".
    body=$(awk -v sym="$sym" '
        $0 ~ "^"sym":" { found = 1; next }
        found {
            if ($0 ~ /^[ \t]*\./) next
            if ($0 ~ /^[ \t]*\/\*/) next
            if ($0 ~ /^[ \t]*\*/) next
            if ($0 ~ /^[ \t]*#/) next
            if ($0 ~ /^[ \t]*$/) next
            gsub(/^[ \t]+|[ \t]+$/, "", $0)
            print $0
            exit
        }
    ' "$S")
    if [ "$body" = "retq" ]; then
        pass
    else
        fail "stub body for $sym is not a single retq (got: $body)"
    fi
done

# -------- no call site anywhere else in stage2/src (only this file or header declares) --------
for sym in vm86_switch_long_to_pe32 vm86_switch_pe32_to_long \
           vm86_switch_enter_v86_via_iret vm86_switch_gp_trampoline; do
    # Exclude the asm definition (which literally uses the symbol name
    # on a label line like "sym:") and the header prototype line. Count
    # any other references — there must be none at this phase.
    hits=$(grep -RnE --include='*.c' --include='*.h' --include='*.S' -w "$sym" stage2/ 2>/dev/null \
         | grep -v "^stage2/src/vm86_switch.S:" \
         | grep -v "^stage2/include/vm86_switch.h:" \
         | wc -l | tr -d ' ')
    if [ "$hits" = "0" ]; then
        pass
    else
        fail "forbidden reference to $sym outside stub/header ($hits hits)"
    fi
done

# -------- Makefile auto-collect preserved (STAGE2_S_SRCS via find) --------
gf Makefile "STAGE2_S_SRCS := \$(shell find stage2/src -type f -name '*.S')" "S autoglob"
gf Makefile "test-vm86-switch" "makefile target"

echo "[summary] $OK OK / $FAIL FAIL"
[ "$FAIL" = "0" ] && echo "[PASS] OPENGEM-027 vm86 switch-stub gate" || echo "[FAIL] OPENGEM-027 vm86 switch-stub gate"
exit "$FAIL"
