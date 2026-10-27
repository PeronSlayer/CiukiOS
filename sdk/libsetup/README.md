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

The version 2 native adapter uses capability-scoped FAT descriptor queries,
full attributes, raw cluster verification, and the shared Control Panel
configuration codecs. Configuration targets the published destination prefix;
prepare only reads media and enumerates disks. The SDK recipe builds the real
archive. The fake remains exclusive to host wizard tests.

The additional archive members are `apps/control/configuration.c` and
`src/kernel/net/config.c` (the f2-100 NETWORK.INI codec), and
`apps/desktop/settings_io.c` (Control Panel's destination-aware durable
writer). Setup calls the writer directly and starts no settings worker. Link desktop settings
and theme, libreg and libdisk as the application recipe does. The checked RTC
write happens during P4. IPX/lobby persistence remains deferred by the lead.

Account provisioning (sec-user-02, user-security-contract.md 3.6/5.1): the
configuration step stages the application's origin-2 seed (salted verifiers
only, `setup_answers.account_seed`) as `System/Security/provision.seed` with an
UNPROVISIONED `bootstrap.state` bound to the formatted volume
(`sdk/private/provision.h`: partition LBA << 32 | BPB serial), creates the
`Windows/Profiles` container, flushes and reads both back before the volume is
sealed. Prepare refuses answers without that seed or with a seed for another
name, and a CD manifest carrying any file below `System/Security`. Private
profile roots are installed by OWNER on the first installed boot.

Run host tests serially:

```sh
nice -n 19 bash scripts/test/host_fs_tests.sh --libsetup
```

This uses the filesystem runner's lock and existing ASan/UBSan policy. The
private harness supplies >200 synthetic files on the CD side and production
FAT/VFS/libdisk on the destination side. It checks the archive link boundary,
raw verification, attributes, long names, mtools/fsck, durable phase markers,
crash cuts, retries, cancellation and restart timing. Production native adapters and configuration codecs are exercised through
renamed POSIX entry points over the file-backed VFS fixture.

With a pre-existing SDK available read-only, also compile the production
archive with its target flags:

```sh
nice -n 19 python3 tests/host/libsetup_target_compile.py --sdk /path/to/ciuki-sdk
```

Add `--link-setup` to compile and inspect the complete production wizard
against the real archives (the existing SDK and libdeskui stay read-only).

This does not call the SDK wrapper (which would write its own work/temp).
No SDK, kernel, image or ISO is built.
