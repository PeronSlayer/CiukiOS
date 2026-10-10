# Ciuki desktop (f2-08)

An ordinary SDK-built ring-3 compositor. The supervisor owns launch, exclusive
grants and the desktop's separate process group. See
[the desktop guide](../../docs/desktop.md) for wire layout, launch arguments,
limits, input recovery, research, evidence and f2-09 integration requirements.

Build the existing SDK first, then run:

```sh
python3 -B apps/desktop/build_desktop.py
python3 -B -m unittest discover -s tests/host/desktop -v
```

The production `make desktop` target uses the existing capped build recipe.
The implementer uses the direct Python command under the directive's explicit
sandbox exception. No download, QEMU, systemd or Git command is used by this
recipe. Scratch lives on disk inside the worktree. The manifest records SDK,
source, ELF and portrait hashes; stale outputs fail image payload validation.

`protocol.h` is shared with `apps/demo`. **Lead note:** install this header as
`ciuki/desktop.h` through an SDK directive; f2-08 does not allow edits to `sdk/`.
The approved original remains `assets/brand/ciuki-logo.png`. The converter
neither redraws nor restyles it. Generated pixels belong in build output, not
in this source directory. The Ciuki portrait remains a separate project asset;
MIT notices on the code do not relicense it or the GPL ABI header.
