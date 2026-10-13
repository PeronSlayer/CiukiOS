# F1-48 optical binding and runtime media

Research checked 2026-10-13 before implementation:

- [SFF-8020i revision 2.6](https://www.jmargolin.com/project/SFF8020i.pdf),
  §§5.2–5.5: PACKET PIO and completion reason; §10.6: reset/media-change unit
  attention and REQUEST SENSE; §10.8.19: TOC/session fields;
  §10.8.25: immediate start without tray loading; §10.8.26.1: TEST UNIT
  READY polling, prompt status, no-medium sense.
- [QEMU 9.2 ATAPI model](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/ide/atapi.c):
  the accepted fixture uses SFF session format in byte 9, not MMC byte 2.

Repository trace: ATA IDENTIFY PACKET publishes type 5 and a 12/16-byte packet
device. `ata_device_get(channel, unit)` admits identified, non-quarantined
devices independently of DMA. Slot is `channel * 2 + unit`, so the T23
secondary master is slot 2; this is not the C: drive index. ATAPI bypasses the
512-byte MBR/blkpart path and mounts its 2048-byte data session as ISO 9660.
The f1-44 DMA admission applies only to ATA IDENTIFY, so ATAPI stays on PIO.

The supplied T23 record reaches the optical readiness/mount sequence and fails
before a drive letter is published. A failure in initial `atapi_ready`, an ISO
mount's readiness/generation check, or its packet reads can produce this record.
Generation 2 proves a classified media condition. `reason=unavailable` excludes
the old path's explicit no-medium result; the old record omits the command and
sense, so it cannot distinguish reset unit attention from other not-ready
states. Reset unit attention is a specification-supported hypothesis,
not a confirmed hardware diagnosis. Previously boot made one readiness attempt
and there was no runtime optical service.

Decision: retry only TEST UNIT READY for bounded unit attention (maximum three
commands within the ordinary two-second deadline), never failed READ(10).
For NOT READY 02/04/02, issue one asynchronous START STOP UNIT (IMMED=1,
Start=1, LoEj=0) per generation; later TUR must prove readiness. An empty tray
is never loaded/closed automatically. Immediate start uses the ordinary
two-second packet deadline; explicit non-immediate eject/start keeps ten seconds.
Log command stages, completion status, sense and mount/detach decisions.
Retain identified empty drives and poll each every 1000 ms from a dedicated
device task, separate from FAT writeback. Start it only on ordinary boot, so
the existing explicit ide-cd probe and QMP fixture remain unchanged. A ready,
responding drive is detected and mounted within two seconds; drive spin-up,
hardware command timeouts and scheduler/controller contention can delay usable
media and are visible in records. No transport recovery or BIOS fallback.

The namespace lock serializes polling, publication and eject with filesystem
access, and the existing ATA mutex serializes packets with the ATA peer.
Generation changes invalidate names/cache, cwd and open handles before a new
ISO mount. Refused ISO media are attempted once per generation. Letters use
the next available VFS letter from D:; Joliet uses the existing parser.

`MEDIA_GET_STATE` version 2 appends 26 uint32 type values (none, USB, optical)
to the 120-byte version 1 prefix, for 224 bytes. Version 1 GET_STATE and removal
requests remain supported. Optical D: is included; optical safe removal means
eject after open-handle preflight. Files polls the copied state every 500 ms,
refreshes on changes, and shows the existing Tango optical icon and CD-ROM type.
