# Pinned VSBHDA/DBOPL source

This directory preserves the complete, unmodified VSBHDA source archive at
commit `75fa4bbfea70cbcc0c40d1212f04952ff8abbf16`. `UPSTREAM.json` records the
canonical codeload URL and SHA-256. The upstream GPL version 2 text is copied
verbatim as `LICENSE`; the complete archive retains every per-file notice.

CiukiOS uses only `src/DBOPL.CPP`, `src/DBOPL.H` and `src/CONFIG.HPP`, through
the separate `guest_opl_dbopl.cpp` adapter (model tests) and `session_opl.cpp`
(built into `CVSESSION.DLL` by `scripts/build_vm_session.sh`). DBOPL's files identify the DOSBox
Team and permit redistribution under GPL version 2 or later. A binary linked
with that adapter and DBOPL must be distributed under compatible GPL terms
with corresponding source. `CVSESSION.DLL` links DBOPL and is therefore
distributed under GPLv2 (CiukiOS's license) with its complete source in this
repository. The standalone `guest_peripherals.c` device model does not contain
or link DBOPL.

The complete upstream archive also retains its small helper binaries and test
audio assets. CiukiOS does not extract or link those files. The verification
script checks the archive hash, extracts only the three named DBOPL source
files into an ignored build directory, compiles them, and records their hashes
in its report. No physical audio driver from VSBHDA is used by a guest session.
