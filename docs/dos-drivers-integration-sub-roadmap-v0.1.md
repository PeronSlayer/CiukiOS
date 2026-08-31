# DOS Drivers Integration Sub-Roadmap v0.1

## Status

Driver packaging and controlled activation evidence exist. Driver integration is not yet productized runtime policy.

The previous statement that MSCDEX is blocked at child exit 0x11 is obsolete. A later focused evidence run recorded successful MSCDEX child completion and:

    Drive A: = Driver QCDROM1 unit 0

This is useful closure evidence for the previously blocked redirector path. It is not a product PASS because A: is the wrong mapping for the active drive policy, the result is not part of the current aggregate full/full-CD gate, and activation still depends on the helper/evidence path.

## Proven Boundaries

1. Full-image packaging places helpers and optional local driver payloads under SYSTEM/DRIVERS.
2. DRVLOAD.COM provides deterministic begin/try/done markers and fail-open behavior.
3. The native SYS loader reaches QCDROM.SYS strategy/interrupt INIT.
4. QCDROM detects the QEMU optical device and registers QCDROM1.
5. Focused historical evidence shows MSCDEX can complete and publish a redirector mapping.

No statement above implies automatic boot activation, correct drive-letter assignment, full-CD product integration, or broad third-party driver compatibility.

## Remaining Productization Work

### 1. Freeze the activation contract

Document:

1. manual helper versus automatic runtime activation policy
2. deterministic driver load order
3. required versus optional driver behavior
4. missing, invalid, duplicate, and partially initialized driver handling
5. ownership of device chain, List-of-Lists, CDS, handles, and IOCTL forwarding

Keep boot-time autoload disabled until CIUKIDOS/module ownership and failure behavior are explicit.

### 2. Correct drive mapping

1. Preserve A: and B: for floppy semantics.
2. Preserve C: for the full boot volume and D: for the current full-CD live profile unless a later approved allocator replaces this policy.
3. Assign the CD redirector deterministically without overwriting an existing logical drive.
4. Verify current/default drive, CDS, List-of-Lists, and shell behavior after activation.

The historical A: result remains PARTIAL until this gate is met.

### 3. Add a current validation matrix

Mandatory scenarios:

1. valid QCDROM plus valid MSCDEX
2. missing SYS driver
3. corrupt SYS header
4. INIT failure or no optical device
5. missing MSCDEX
6. duplicate activation
7. mixed valid and invalid drivers
8. full-CD D: boot with activation and shell recovery

Every scenario requires a fresh log, explicit PASS/FAIL result, final drive map, and proof that shell/runtime state remains usable.

### 4. Move policy out of Stage1

Driver activation and redirector policy should live in CIUKIDOS, a runtime module, or explicit COM helpers. Stage1 should retain only boot-critical media access.

## Completion Gate

The driver integration cycle is complete when:

1. activation ownership and ordering are documented
2. correct drive letters are reproducible on full and full-CD
3. all eight mandatory scenarios have explicit results
4. no invalid driver blocks boot or corrupts the device chain
5. current DRVLOAD, runtime, shell, full-CD, and aggregate gates are green on the same checkout
6. the DOS compatibility matrix records the productized workflow separately from historical evidence

## Immediate Next Action

Turn the historical MSCDEX success into a current deterministic lane, then fix the A: mapping through the general CDS/List-of-Lists drive policy before considering automatic activation.
