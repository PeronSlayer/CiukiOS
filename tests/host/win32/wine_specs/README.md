# Offline Wine 10.0 audit inputs

These are unmodified `.spec` files, Makefiles, notices and the generated server
protocol header from Wine tag `wine-10.0`, commit
`b073859675060c9211fcbccfd90e4e87520dc2c2`. Their original paths are retained.
`config/wine-pin.json` records the SHA-256 and Git blob SHA-1 for each input.

Wine is LGPL-2.1-or-later; retain `LICENSE`, `COPYING.LIB` and `AUTHORS` here.
The optional musl notice retains its MIT and file-specific notices separately.
These snapshots enable offline consistency checks; they do not constitute the
complete corresponding source of a distributed DLL. No Wine or game binaries
are stored here. The complete release archive is still pending download.

Regenerate the source census using:

```sh
nice -n 19 python3 scripts/win32/wine_audit.py collect build/external/wine-10.0
nice -n 19 python3 scripts/win32/wine_audit.py render
nice -n 19 python3 scripts/win32/wine_audit.py check
```

The collector preserves existing PE/memory observations for the same Wine
commit; rerun `build-wine-dlls.sh` and compare its JSON/hashes whenever the build
recipe, compiler, source pin or intended DLL subset changes. Fresh experiments
must be reviewed and recorded separately from the source-only candidates.
