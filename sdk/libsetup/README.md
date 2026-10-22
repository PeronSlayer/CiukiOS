# libsetup

The frozen application interface is `include/setup_txn.h`; `setup_inf.h` is
owned by f9-03. One process owns one transaction. A plan belongs to the most
recent successful prepare in that process and can be executed once; all
answer/report/log objects must remain alive throughout execution. Prepare
performs no destination writes. libdisk owns the lease, formatter and final
MBR publication. Required source errors have Retry/Cancel, with five retries.

Production translation units: `txn.c`, `manifest.c`, `native.c`,
`native_media.c`. Archive them as `libsetup.a` and link `-lsetup -ldisk`
alongside the shared answer validation/catalogue implementation from f9-03.
The native media module uses the existing `ciuki_devctl` and `ciuki_power`.
`internal.h` is a private platform/testing boundary, not an installed SDK
header. `setup_restart` remains private pending a frozen-interface amendment.

Native installation is currently refused with ENOTSUP during preflight.
The native VFS cluster/attribute access and shared configuration/RTC writers
required by the directive are absent from this tree. A host backend is not
evidence of those missing native facilities. See
[the integration record](../../docs/validation/f9-04-libsetup.md).

Run host tests serially:

```sh
nice -n 19 bash scripts/test/host_fs_tests.sh --libsetup
```

This uses the filesystem runner's lock and existing ASan/UBSan policy. The
private harness supplies >200 synthetic files on the CD side and production
FAT/VFS/libdisk on the destination side. It checks the archive link boundary,
raw verification, attributes, long names, mtools/fsck, durable phase markers,
crash cuts, retries, cancellation and restart timing. Configuration persistence
has an explicitly identified stand-in, pending the shared native writers.

With a pre-existing SDK available read-only, also compile the production
archive with its target flags:

```sh
nice -n 19 python3 tests/host/libsetup_target_compile.py --sdk /path/to/ciuki-sdk
```

This does not call the SDK wrapper (which would write its own work/temp).
No SDK, kernel, image or ISO is built.
