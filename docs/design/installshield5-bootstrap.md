# InstallShield 5 native bootstrap

The bootstrap is a native PE32 helper admitted by the common kernel
`w32_spawn` path. Both native desktop launches and Win32 CreateProcess
(including WinExec and ShellExecute) reach that path. The helper uses the
ordinary Win32 file/process APIs, then starts the original engine through
NTDLL's existing PE loader. `sdk/win16/ne` remains a structural NE reader;
no NE instructions or Win16 imports execute here.
The helper reserves a normal 1 MiB application stack and initially commits
64 KiB for its bounded metadata/Huffman workspaces, so all frames stay
inside the initial commit without CRT stack probes.

Admission requires a bounded MZ/NE Windows application header, SETUP.INS
and an Intel/Alpha/MIPS/PowerPC `_INST32*.EX_` sibling. The Intel archive
must be present for execution on CiukiOS. Other NE applications retain the
existing invalid-image refusal. `_ISDEL.EXE` (or Startup/IsdelName) uses
the same admission and the native cleanup operation. A copied `_ISDEL.EXE`
also qualifies with a Windows `_delis43.ini` record containing file/main,
exeostype=1 and both alt fields (bounded to 4 KiB). The helper validates
every deletion target again; a filename alone never admits an NE image.

## Research and format

Primary implementation reference: [Deark, installshld.c,
`do_instarch_member` and `de_run_is_instarch`](https://github.com/jsummers/deark/blob/master/modules/installshld.c).
This is an installer-engine archive, distinct from InstallShield cabinets
and InstallShield Z archives. It has an 82-byte header, signature
`2a ab 79 d8 00 01`, and a little-endian member count at 78. Each member
contains u32 compressed offset, u32 compressed size, u32 expanded size,
eight reserved bytes, u16 first-name length and bytes, then u16 second-name
length and bytes. Offsets are absolute. The second names are the runtime
names: INSTALL.EXE becomes a unique `_INSnnnn._MP`, zdata.dll becomes
ZDataI51.dll and WUTL95i.DLL becomes _WUTL951.DLL. BOOT16.EXE is unused.
Validate every directory entry's ranges, names and overlaps, and the three
used members' expanded sizes and DCL end markers before publishing output.
The format has no checksum. Bounds are 16 members, 63-byte leaf names,
16 MiB input files and 8 MiB per expanded member; larger variants fail.

The coding reference is [Mark Adler's zlib 1.3.1 contrib/blast](https://github.com/madler/zlib/tree/v1.3.1/contrib/blast),
locally available in build/downloads/zlib/zlib-1.3.1.tar.gz. DCL has a
literal coding flag (0 or 1), a dictionary extra-bit count (4–6), fixed
canonical Huffman alphabets, overlapping backward matches, and length 519
as the terminator. It is unrelated to PKZIP method 6. Our bounded decoder
adapts blast's fixed tables and canonical decoding; it has explicit error
returns, per-call tables and no setjmp, global initialization or libc.
The original blast license is retained alongside it.

[Revenera's Setup command-line reference](https://docs.revenera.com/installshield/helplibrary/IHelpSetup_EXECmdLine.htm)
documents silent `-s`, response `-f1` and log `-f2`. The vendor's
[Professional 5.x migration notes (User Guide, p. 408)](https://docs.revenera.com/installshield25helplib/pdf/InstallShield2019_UserGuide.pdf)
explicitly distinguish old `-SMS` (keep the launcher alive) and old `-z`
(skip the memory check above 256 MiB) from later semantics. Thus the modern
`-z` script-argument meaning must not be projected back onto IS5. These
references cannot establish IS5's private handoff. Preserve user arguments
without interpreting or claiming to implement the install script's switches.
The private format below comes from the lead's original-media Wine trace
in fx-is5boot.

[Microsoft KB 896458](https://learn.microsoft.com/en-us/troubleshoot/windows-client/application-management/x64-windows-not-support-16-bit-programs)
describes recognition/replacement of selected 16-bit installers on x64.
[otvdm's upstream README](https://github.com/otya128/winevdm) describes both
that replacement mechanism and its general Win16 emulator. This change
uses the narrow replacement approach, not a general emulator. Community
IS5 32-bit replacements exist, but their binaries are not vendored.
[Wine bug 4159](https://www.winehq.org/pipermail/wine-bugs/2006-January/024437.html)
reports a regression spinning at InstallShield startup; [Wine bug
46984](https://bugs.winehq.org/show_bug.cgi?id=46984) concerns an engine
installation error. Neither establishes IS5 private handoff bytes or proves
that replacing a stub completes an installation. Keep bootstrap and
engine-runtime evidence separate. The contemporary
[32-bit replacement instructions](https://community.pcgamingwiki.com/files/file/364-installshield-5-32-bit-generic-installer/)
are corroboration only, not a format specification or vendored input.

## Handoff and lifetime

The helper reads SETUP.INI Startup/IsdelName, AppName, CmdLine,
EnableLangDlg and Mif/Type; SETUP.LID Languages/Default; and system.ini
Boot/Shell. It selects the Intel archive. Its required engine outputs are
written under GetTempPathA with exclusive engine-name reservation. Fixed
handoff/DLL names require one active bootstrap; an exclusive Windows
lock file serializes their lifetime through engine exit.

`%TEMP%\_INS0432.INI` is a raw argument line, with no section, key, newline
or trailing NUL. The reference is
`  -fD:\SETUP.INS  -z1 -cx -x<TEMP>\ -x1D:\ -q10010`.
Append Startup/CmdLine and the caller's argument tail in order. `%WINDIR%`
contains `_isenv31.ini` (environment entries joined with single spaces,
no NUL/newline), `_iserr31.ini` ([SharedErrors], 7068/7063/7064/7110), and
`_delis43.ini` ([fileN], main=engine, exeostype=1, alt0=ZDataI51.dll,
alt1=_WUTL951.DLL). Error text is taken from NE string resources, first
the bootstrap and then its `_SETUP.DLL`, with English fallback text.
On the mounted reference disc all four error IDs are in `_SETUP.DLL`;
none is present in SETUP.EXE. The local test checks presence without
recording their commercial text.

Start the engine with an explicitly empty command line and the source
setup directory as current directory. Wait for its exit, return its exit
status and remove owned temporary bootstrap files. A failed process wait
retains those files for later cleanup. Native _ISDEL waits for the same
handoff lock, reads its cleanup list, waits for matching live engine
processes and retries sharing failures; it only deletes validated engine/DLL
paths in TEMP. No general-purpose deletion instruction is accepted from an
arbitrary INI. The helper remains alive through the engine lifetime even
without -SMS, so waited CreateProcess users observe engine completion.

## Validation boundary

Synthetic tests generate their own archive directory and literal-only DCL
streams, exercise malformed metadata/coding and exact handoff bytes.
The original-media check reads the mounted CD, decodes in memory and
asserts the measured expanded sizes (547840, 53760, 46592).
The local H1 test exposes the CD read-only and runs the authored helper
and original engine using the production PE loader and core DLLs.
Commercial input and decoded bytes must not persist in fixtures, image
payloads or diagnostic logs. Local H1 scratch outputs are removed on exit.
No QEMU or canonical image build is part of this implementer validation.
