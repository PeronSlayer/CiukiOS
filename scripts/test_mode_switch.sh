#!/usr/bin/env bash
# OPENGEM-044 Task A static gate: mode-switch engine scaffolding.
#
# Invariants enforced:
#   - Arm-gate default disarmed.
#   - Magic 0xC1D39440u / sentinel 0x0440u present.
#   - mode_switch_run_legacy_pm arm-checks BEFORE any state inspection.
#   - Engine is "not implemented" until the asm trampoline lands in
#     this same branch (explicit NOT_IMPLEMENTED return is required so
#     boot safety is preserved).
#   - No boot-path caller exists. The only in-tree consumer is the
#     probe inside mode_switch.c itself.
#   - No CR0/CR3/CR4/EFER/LGDT/LIDT/LTR writes are introduced in the
#     C scaffolding. The mode-switch asm (when it lands) will be the
#     only place allowed to touch those, inside a dedicated .S file.
#
# Safety: gate does NOT build or run stage2. It is a static text gate.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

H=stage2/include/mode_switch.h
C=stage2/src/mode_switch.c

# --- 1. Files exist --------------------------------------------------
test -f "$H" && pass || fail "header $H missing"
test -f "$C" && pass || fail "source $C missing"

# --- 2. Sentinels and magic ------------------------------------------
grep -q '#define MODE_SWITCH_ARM_MAGIC[[:space:]]*0xC1D39440u' "$H" \
    && pass || fail "MODE_SWITCH_ARM_MAGIC 0xC1D39440u missing"
grep -q '#define MODE_SWITCH_SENTINEL[[:space:]]*0x0440u' "$H" \
    && pass || fail "MODE_SWITCH_SENTINEL 0x0440u missing"
grep -q 'opengem_044_a_sentinel\[\] = "OPENGEM-044-A"' "$C" \
    && pass || fail "C sentinel OPENGEM-044-A missing"

# --- 3. Public API signatures ----------------------------------------
while IFS= read -r sig; do
    grep -qF "$sig" "$H" && pass || fail "header signature missing: $sig"
done <<'EOF'
int  mode_switch_run_legacy_pm(mode_switch_legacy_pm_body_fn body, void *user);
int  mode_switch_arm(uint32_t magic);
void mode_switch_disarm(void);
int  mode_switch_is_armed(void);
int  mode_switch_probe(void);
EOF

# --- 4. Arm flag defaults 0 -----------------------------------------
grep -qE '^static int s_mode_switch_armed = 0;' "$C" \
    && pass || fail "s_mode_switch_armed must default to 0"

# --- 5. Magic enforcement in arm -------------------------------------
awk '/^int mode_switch_arm\(/,/^}/' "$C" > /tmp/ms_arm.c
grep -q 'MODE_SWITCH_ARM_MAGIC' /tmp/ms_arm.c \
    && pass || fail "mode_switch_arm does not check MODE_SWITCH_ARM_MAGIC"
grep -q 's_mode_switch_armed = 1' /tmp/ms_arm.c \
    && pass || fail "mode_switch_arm does not set armed flag"
rm -f /tmp/ms_arm.c

# --- 6. run_legacy_pm arm-checks FIRST -------------------------------
awk '/^int mode_switch_run_legacy_pm\(/,/^}/' "$C" > /tmp/ms_run.c
# The first conditional must test s_mode_switch_armed.
head -n 10 /tmp/ms_run.c | grep -q 's_mode_switch_armed' \
    && pass || fail "run_legacy_pm must check armed flag FIRST"
grep -q 'MODE_SWITCH_ERR_NOT_ARMED' /tmp/ms_run.c \
    && pass || fail "run_legacy_pm must return NOT_ARMED when disarmed"
grep -q 'MODE_SWITCH_ERR_BAD_INPUT' /tmp/ms_run.c \
    && pass || fail "run_legacy_pm must reject NULL body"
grep -q 'MODE_SWITCH_ERR_NOT_IMPLEMENTED' /tmp/ms_run.c \
    && pass || fail "run_legacy_pm must return NOT_IMPLEMENTED until asm lands"
rm -f /tmp/ms_run.c

# --- 7. No forbidden mode-register writes in C scaffolding ----------
# The asm trampoline (when it ships in mode_switch_asm.S) is the only
# place allowed to touch these. The C scaffolding must be inert.
if grep -qE '\bmov[[:space:]]+%?cr[0-4]' "$C"; then
    fail "C file must not contain CR register writes"
else
    pass
fi
if grep -qE 'wrmsr|rdmsr' "$C"; then
    fail "C file must not contain MSR writes"
else
    pass
fi
if grep -qE '\blgdt\b|\blidt\b|\bltr\b|\blldt\b' "$C"; then
    fail "C file must not contain descriptor-register writes"
else
    pass
fi

# --- 8. Boot-path isolation ------------------------------------------
# No file outside mode_switch.{c,h} may reference mode_switch_* symbols
# (except its own probe gate). stage2 must not call these from init.
LEAKS=$(grep -RIlE '\bmode_switch_(arm|disarm|is_armed|run_legacy_pm|probe)\b' \
    stage2/ 2>/dev/null \
    | grep -vE '^stage2/src/mode_switch\.c$' \
    | grep -vE '^stage2/include/mode_switch\.h$' \
    || true)
if [ -z "$LEAKS" ]; then
    pass
else
    fail "boot-path leak: mode_switch symbols referenced from: $LEAKS"
fi

# --- 9. Probe covers disarmed, bad-magic, armed + NULL body, armed + body=pending ---
awk '/^int mode_switch_probe\(/,/^}/' "$C" > /tmp/ms_probe.c
grep -q 'MODE_SWITCH_ERR_NOT_ARMED'       /tmp/ms_probe.c \
    && pass || fail "probe missing NOT_ARMED case"
grep -q '0xDEADBEEFu'                      /tmp/ms_probe.c \
    && pass || fail "probe missing bad-magic case"
grep -q 'MODE_SWITCH_ERR_BAD_INPUT'        /tmp/ms_probe.c \
    && pass || fail "probe missing BAD_INPUT case"
grep -q 'MODE_SWITCH_ERR_NOT_IMPLEMENTED'  /tmp/ms_probe.c \
    && pass || fail "probe missing NOT_IMPLEMENTED case"
rm -f /tmp/ms_probe.c

# --- Report ----------------------------------------------------------
echo "[test-mode-switch] OK=$OK FAIL=$FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "[PASS] OPENGEM-044-A mode-switch scaffold gate"
    exit 0
else
    echo "[FAIL] OPENGEM-044-A mode-switch scaffold gate"
    exit 1
fi
