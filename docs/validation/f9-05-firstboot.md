# First boot and rescue implementation contract

The wizard is part of the desktop, before its ordinary session. It uses the
existing display/input grants and approved portrait; no new supervisor authority
or kernel interface is needed. Missing/done state takes only a bounded read.

Research before implementation:

- [POSIX fsync](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fsync.html)
  specifies completion/error reporting for synchronized file data. The endpoint
  returned HTTP 403 in this session; the repository's existing fsync-based Setup
  durability contract is retained. Append whole LF records, commit each result,
  and do not advance after a commit error. Ignore an unfinished trailing line.
- [RFC 2131](https://www.rfc-editor.org/rfc/rfc2131), section 4.4: DHCP discovery
  and renewal belong to the network service. The wizard observes its snapshot;
  ten seconds is a UI deadline, not a new DHCP timeout or an Internet test.
- [OpenBSD getentropy](https://man.openbsd.org/getentropy.2): request one byte,
  below the 256-byte ceiling. CiukiOS's documented EAGAIN means not ready;
  getentropy(NULL, 0) cannot test readiness. Never use the indefinite wait helper.
- [X.Org Savage acceleration source](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c)
  programs the engine. That work stays in the kernel; the wizard only queries
  generic capabilities/quarantine. The existing nonempty attempt marker is
  observed, never replaced by a second marker or a BOOT.CFG override.
- Windows 98 Resource Kit, local read-only `wrkc27_3.htm`, Microsoft System
  Information: inventory combines devices, drivers and resources. Use existing
  generic inventories and f9-07, without machine-specific assumptions.

Repository findings: f9-04 writes LF-separated SETUP.STATE under 4 KiB and
permits first-boot appends. games are registered by catalogue identity in the
shell and enumerated by f2-87's cp_program_list; it has no registration writer.
Control Panel Display has no acceleration retry action in this source state.
The f9-07 application and its new inventory ABI have not been merged here.
The manifest producer belongs to parallel f9-04b, so its integration must be
coordinated rather than edited concurrently.
