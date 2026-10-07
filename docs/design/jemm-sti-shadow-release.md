# Releasing Jemm's synthetic STI single step

The Intel architecture uses TF to request a debug exception after an
instruction; STI inhibits interrupt delivery through the following
instruction. See the [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
Vol. 2 STI and Vol. 3 debug exceptions. The pinned
[Jemm exception return path](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/src/JEMM32.ASM)
reflects an unclaimed V86 debug exception to the guest's INT 1.

CiukiOS's `cvirq.inc` adaptation implements the STI boundary by setting TF
and recording `CvShadow=1` plus the original guest TF in `CvShadowTF`.
`CvDebug` removes this synthetic TF only while the profile is active.
However, `CvLeave` calls the return-to-V86 poll, which can park the VMM
after the last decoder VM exits. Its disable path then clears `CvActive`
without consuming an outstanding synthetic TF. The next debug exception
falls through to the guest rather than completing the monitor's shadow.

The switch capture `music-switch-runtime2` restores a valid master frame
at shell RETF with EFlags `33393` (TF set). A separate failing capture
contains successive `svc_` epilogue instruction offsets `0245`–`024A`
in About's stack, consistent with repeated debug exceptions; this stack
observation alone does not prove the writer.

Decision, recorded before implementation: on actual profile disable,
clear TF only when `CvShadow` is pending and `CvShadowTF` says it was
introduced by the monitor. Preserve a genuine guest TF. Reset the shadow
metadata before clearing `CvActive`. This closes the synthetic flag's
lifetime for all release paths, without changing allocation or scheduler
policy. Runtime music EOF/close/reopen and V86 interrupt tests qualify the
production binaries only when the exact patched Jemm and CVSESSION files are
installed in the VM directory and verified by hash.

Focused regression plan: extract the cleanup instructions directly from the
release patch, assemble them with JWasm, and execute those exact instructions in
Unicorn against a synthetic pending STI shadow and a genuine guest TF. This
checks that profile release clears only monitor-owned TF and clears the shadow
metadata in both cases. It does not model the complete Jemm V86 exit path, so a
runtime VM-exit gate remains necessary for end-to-end qualification.

The focused fixture passed on 2026-10-07 with:
`uv run --no-project --with unicorn python scripts/test_jemm_sti_shadow_release.py`.
It executed the extracted cleanup block for synthetic TF, genuine TF during a
pending shadow, and genuine TF with no pending shadow. This is instruction-level
evidence only. The corrected QEMU production run at
`build/full/t23-vbe-fix/music-shadow-runtime2/report.json` passed all four audio
formats, six worker sessions and ends, EOF, seek, close/reopen, and an audio
chirp rising from 330 Hz to 370 Hz. Its `disk.img` contains verified
`VM/JEMMEX.EXE` SHA-256
`01ba4421c3f908b965df9151de95123eaf68d3320e48c412d243568ca39a7b31`,
`VM/JEMM386.EXE` SHA-256
`2b38684adf5122242174ebc17bfd6732adfd74c44915498fa749e50dabce477f`, and
`VM/CVSESS.DLL` SHA-256
`932a04f7027cbd3c55670afbb7c246b8c8af510dc7ce30756c1cb68104cac47f`.
The earlier `music-shadow-runtime1` placed Jemm outside the VM directory and
did not exercise this fix. Runtime2 is emulator evidence only and makes no
physical-hardware claim.
