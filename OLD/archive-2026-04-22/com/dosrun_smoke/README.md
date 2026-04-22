# CIUKSMK.COM

Tiny deterministic DOS smoke payload used by `SR-DOSRUN-001` tests.

## Source and Reproducibility
1. Source file: `com/dosrun_smoke/ciuksmk.c`
2. Build target: `make all` (produces `build/CIUKSMK.COM`)
3. Runtime image copy: `run_ciukios.sh` copies `build/CIUKSMK.COM` into `A:\\EFI\\CIUKIOS\\CIUKSMK.COM`

## Runtime Contract
1. Prints a single deterministic line.
2. Terminates via `INT 21h AH=4Ch` with return code `0x2A`.

## Licensing
Self-built project source; no third-party binary redistribution.
