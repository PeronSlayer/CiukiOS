# f2-101c — Display command semantics specification (part C prerequisite; no code)

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning high, `--search`. **Worktree:** `wt/f2-101c` from `main`. Specification only: **no change under `src/`, `apps/` or `sdk/`**.
- **Source:** `docs/design/display-driver-model.md`, Amendment 1, item 7. Part C (MONO, INVERT, KEYED, SCALED, LINE, clip lists, colour ARGB cursor, batches and fences, system-memory targets) needs a written semantic specification before any code.

## Required

Write `docs/design/display-command-semantics.md`, a new file, so it does not conflict with f2-101 part A, which edits the contract. For each command it defines:
- exact pixel semantics at 16, 24 and 32 bpp (rounding, colour conversion, alpha mode for the colour cursor);
- aliasing and overlap rules (which commands forbid source/destination overlap, as Haiku's scaled blit does);
- clip-list union behaviour, which must never double-apply XOR;
- the line endpoint and diagonal rule (compare the Windows 9x/GDI convention of excluding the last pixel with X11 zero-width lines, and pick one with its reason);
- the 1 bpp bit order and pattern origin;
- the filtering definition for SCALED;
- error codes and the guarantee of no pixel change when refused.

It also defines the batch wire format:
- header, version, record layout and reserved fields;
- count and byte limits and grant checks;
- staging of faultable payloads outside display locks;
- completed-prefix semantics, fence ids and their wait/status/cancel rules;
- the lock lifetime across presenter releases (drawing claim and in-flight state);
- interleaving with other clients.

Finally it gives golden-test vectors: a table of small cases with expected pixels, usable by the later code directive.

Cite the primary references: Haiku `Accelerant.h`; X.org EXA; Linux `fb.h`, `cfbimgblt.c` and `cfbfillrect.c`; Microsoft GDI documentation for raster operations, lines and DrawFocusRect; and the Windows 95 DDK display minidriver pages (archived).

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Write `build/codex/f2-101c-report.md`.
