# f6-07 local driver package import

The implementation follows [ND11](ndis-miniport-host-contract.md). Research
compared `scripts/build_hwids.py` with Microsoft's
[Manufacturer](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-manufacturer-section),
[TargetOSVersion](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/combining-platform-extensions-with-operating-system-versions),
[Services](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-ddinstall-services-section),
[AddService](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addservice-directive),
[CopyFiles](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-copyfiles-directive),
[AddReg](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addreg-directive)
and [configuration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndisreadconfiguration)
documentation, plus the [general INF syntax](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/general-syntax-rules-for-inf-files)
for quoted fields and string substitution. The local Resource Kit, Appendix C, `wrkappc_1.htm`, describes
device-ID selection and ordered section entries. That Win98 description does
not authorize NT miniports; the NT 5.x contract supplies the target policy.

Decisions: retain INF comments, continuations, case-insensitive section names
and neutral Strings precedence, but follow the Manufacturer/models/install/
associated-service chain. Prefer complete subsystem/revision IDs and the closest
applicable NT x86 minimum version. Keep distinct INFs as explicit user choices.
Include/Needs is bounded and cycle checked; unresolved dependencies, unsupported
registry operations, executable helpers and compressed sources are refused.
Only HKR string, DWORD and binary values enter the private adapter/service tree;
this is separate from the Win32 registry. Ndi parameter metadata is retained and
validated defaults are materialized. No vendor setup command runs.

Packages use an exclusive staging directory beside their final directory.
Only a completely copied, hash-verified package with its typed registry and
versioned manifest can be renamed into place. Existing packages are never
overwritten; removal precedes replacement explicitly. An interrupted staging
directory is not an installed manifest. Filesystem error rollback is supported;
this does not claim power-loss atomicity beyond the native FAT rename/flush
guarantees. The native filesystem has no symbolic links; host adapters reject
links at every path component. All input and dependency sizes are bounded.

The lead-owned production qualification table is empty. Imports therefore
remain disabled with an explicit reason. An installed manifest cannot grant
qualification or replace a native owner. Runtime activation still requires the
existing exact compiled qualification, containment and ownership checks.

The private `ndis_qualification.package_seal` pins the entire versioned
manifest, including parameters and all dependency hashes. The gate checks this
compiled authority before `ndis_native_prepare` allocates a task/image and
again before `ndis_native_bind` changes ownership. It checks the present BDF,
revision/subsystem, exact image hash, registry bytes and every dependency. A
native adapter, including one retained after quarantine, takes precedence.
Changing an enable flag or recomputing a manifest checksum never qualifies it.

Importer bounds are eight INFs, 128 KiB per INF, 1024 entries per INF, 511
characters per logical line, 16 dependency levels, 32 files (4 MiB each), and
96 typed registry values (256 bytes each). Scans stop at 4096 directory entries,
eight directory levels and 32 candidate INFs. Paths are at most 255 bytes and
each filename is at most 63. Unsupported extensions/flags, NT decorations
with product/suite constraints, multi-string/expand-string registry types,
additional service binaries, helper directives and compressed files are
refused rather than approximated. File-list pages show every declared file.

Successful removal drops the committed directory name first, then deletes its
bounded inventory. Failed cleanup retains a disabled tombstone with its
manifest; retry finishes it. An empty tombstone left by a failed final rmdir
can also be removed on retry. Staging recovery requires an exact matching
transaction inventory. There is no append-only installation ledger.
