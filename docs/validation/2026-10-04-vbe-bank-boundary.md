# VBE banked read overrun during compositor fallback

Runtime 2 hit a GPF at `VC_FB_COPY`'s `REP MOVSD` while switching to
640x480x16. Intel's Software Developer's Manual lists `#GP` for `MOVS` when a
memory operand is outside its segment limit. The fault was in the copy loop,
but the request size was wrong upstream.

The compositor fallback divided `ui_comp_capacity` by the row byte count to
choose how many rows to copy into its scratch band. `ui_comp_capacity` is a
word (`dw`) and held 61440 bytes; the affected 16-bit row was 1222 bytes, so
the safe count was 50. A 32-bit load from this word also read the adjacent
`ui_comp_try_paras` field. The fallback then capped the resulting count at 64,
requesting 64 rows and overrunning the scratch allocation. The compositor
load now zero-extends the declared word, matching the other capacity reads in
the file. No VBE bank-copy workaround is needed; splitting a malformed request
at the scratch segment boundary would only allow the 16-bit offset to wrap and
corrupt the buffer.

Sources:

- [Intel SDM, Volume 2B: MOVS/MOVSB/MOVSW/MOVSD/MOVSQ](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf)
- [Intel Software Developer's Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)

## Retest

The focused UI-4 runtime passed both banked-mode recovery cases: the 32-bit
preview timed out and restored the desktop, and the 16-bit preview was
cancelled with Escape and restored the desktop. The final runtime passed the
full reboot flow, including persistence of the 640x480 display profile and
About startup preference. The display-mode and reboot results are recorded in
[`final-runtime/results.json`](../../build/tests/release-0.8.3-2026-10-04/final-runtime/results.json).
