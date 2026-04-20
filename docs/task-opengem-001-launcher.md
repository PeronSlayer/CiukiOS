# Task: OPENGEM-001 - OpenGEM Launcher Integration (Phase 1)

## Objective
Implement Phase 1 of the OpenGEM UX roadmap: validate OpenGEM runtime is functional, create smoke test, document entry points, and integrate launcher button in desktop scene.

## Scope In
1. Verify OpenGEM runtime bundle files and entry points (GEM.BAT, DESKTOP.APP, etc.)
2. Create smoke test harness that validates OpenGEM boot/launch/exit
3. Add serial debug markers for troubleshooting
4. Document runtime structure and fallback scenarios
5. Add "Open OpenGEM" launcher button/icon in desktop scene UI
6. Wire ALT+O shortcut (or similar) to launch OpenGEM
7. Implement desktop → OpenGEM transition with state save/restore
8. Update documentation and handoff

## Scope Out
1. Advanced OpenGEM configuration or customization
2. App discovery/file browser integration (Phase 3)
3. Mouse/keyboard intensive testing (Phase 4)
4. DOOM binary integration (Phase 5)

## Requirements

### Functional
1. `opengem` shell command must launch OpenGEM reliably
2. OpenGEM launcher window must be visually discoverable
3. Return from OpenGEM to shell must preserve state
4. Fallback behavior documented if OpenGEM missing

### Testing
1. Smoke test: `bash scripts/test_opengem_smoke.sh` PASS
   - Validates runtime structure
   - Boots OpenGEM in timeout-controlled manner
   - Checks for launcher visibility marker
2. Desktop launcher test: verify button appears and responds to click
3. No regressions: all existing tests remain PASS

### Documentation
1. Document `third_party/freedos/runtime/OPENGEM/*` structure
2. Document entry points and boot sequence
3. Create handoff with implementation details and next steps

### Constraints
1. No version bump (baseline CiukiOS Alpha v0.8.7)
2. No breaking changes to shell/desktop/video ABI
3. Work on dedicated branch `feature/opengem-001-launcher`
4. Only merge to main if user explicitly approves

## Implementation Steps

### Step 1: Analyze Runtime Structure
- Inspect `third_party/freedos/runtime/OPENGEM/` layout
- Identify GEM.BAT, DESKTOP.APP, and required files
- Document dependencies and boot requirements
- Create reference doc: `docs/opengem-runtime-structure.md`

### Step 2: Create Smoke Test Script
- File: `scripts/test_opengem_smoke.sh`
- Validates runtime files exist and are readable
- Boots OpenGEM with timeout protection (30-60s)
- Checks for launcher presence marker in serial output or framebuffer
- Reports PASS/FAIL
- Integrates into Makefile as `make test-opengem-smoke`

### Step 3: Add Serial Debug Markers
- Add stage2 log: "OpenGEM: boot sequence starting"
- Add mark: "OpenGEM: launcher window initialized"
- Add mark: "OpenGEM: exit detected, returning to shell"
- Ensure markers appear in serial console for test validation

### Step 4: Desktop Scene Integration
- Locate desktop UI code in `stage2/src/ui.c` or `stage2/src/desktop.c`
- Add launcher icon/button for OpenGEM (visual area: dock or bottom bar)
- Button click event → launch_opengem()
- Wire ALT+O keyboard shortcut to same function
- Save desktop state before launch, restore after return

### Step 5: Implement Launch Function
- Function: `shell_run_opengem_interactive(boot_info, handoff)`
- Locate OpenGEM entry point in FAT (GEM.BAT or DESKTOP.APP)
- Save current shell state (current directory, environment)
- Call shell_run() with OpenGEM entry
- Capture exit reason
- Restore shell state and return to desktop

### Step 6: Update Tests
- Add `make test-opengem-smoke` target
- Verify `make test-stage2` still PASS
- Verify `make test-gui-desktop` still PASS
- Add regression check: desktop launcher button still responds to ESC/ALT+G+Q

### Step 7: Documentation and Handoff
- Update `documentation.md` with OpenGEM section
- Create handoff: `docs/handoffs/2026-04-19-opengem-001-launcher.md`
- Document runtime structure, state transitions, fallback scenarios
- List files touched, decisions made, validation performed

## Definition of Done
1. ✅ OpenGEM runtime structure documented
2. ✅ `bash scripts/test_opengem_smoke.sh` PASS
3. ✅ Desktop scene includes OpenGEM launcher button
4. ✅ ALT+O (or similar) launches OpenGEM reliably
5. ✅ State save/restore working (desktop ↔ OpenGEM transitions)
6. ✅ Serial debug markers present for troubleshooting
7. ✅ All existing tests remain PASS (no regressions)
8. ✅ Handoff and documentation complete
9. ✅ Code review ready (branch pushed, no uncommitted changes)

## Output Deliverables
1. **Code Changes**
   - Desktop UI launcher button and keyboard shortcut
   - OpenGEM launch function in shell runtime
   - Smoke test script
   - Serial debug markers in boot sequence

2. **Test Artifacts**
   - `scripts/test_opengem_smoke.sh` (executable)
   - Makefile target: `make test-opengem-smoke`
   - Validation in CI pipeline

3. **Documentation**
   - `docs/opengem-runtime-structure.md` (new)
   - Updated `documentation.md`
   - Handoff file: `docs/handoffs/2026-04-19-opengem-001-launcher.md`

4. **Git State**
   - Branch: `feature/opengem-001-launcher`
   - All changes staged and ready for merge approval
   - No uncommitted changes

## Risk Mitigation
- **If OpenGEM not in runtime:** Add conditional check; fallback to error message
- **If launcher button breaks desktop:** Separate visual layer or use existing desktop slot
- **If state corruption:** Implement explicit save/restore with checksum validation
- **If timeout in test:** Adjust timeout value and add retries

## Testing Matrix

| Test | Expected Outcome | Gate |
|------|------------------|------|
| Runtime structure exists | Files found in FAT | PASS |
| OpenGEM boot sequence | Launcher window appears or serial marker received | PASS |
| Desktop button visible | Button renders on desktop scene | PASS |
| ALT+O launches OpenGEM | OpenGEM window appears | PASS |
| Return to shell | Desktop state restored | PASS |
| `make test-stage2` | All checks pass | PASS |
| `make test-gui-desktop` | All checks pass (no regressions) | PASS |
| Smoke test script | Exit code 0 (PASS) | PASS |

## Timeline
- Analysis + test harness: 1-2 hours
- Desktop UI integration: 1-2 hours
- Validation + documentation: 1 hour

**Total: 3-5 hours focused work**

## References
- Roadmap: `docs/roadmap-opengem-ux.md`
- FreeDOS policy: `docs/freedos-integration-policy.md`
- Symbiotic architecture: `docs/freedos-symbiotic-architecture.md`
- Main roadmap: `Roadmap.md`, phase 4
- CLAUDE.md: collaboration workflow
