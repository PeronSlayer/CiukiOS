# F5-12B2 CD audio transport

Research checked 2026-10-20, before continuing the partial implementation.

Sources: [MMC-3 revision 10g](https://www.13thmonkey.org/documentation/SCSI/mmc3r10g.pdf),
[SFF-8020i mirror](https://www.jmargolin.com/project/SFF8020i.pdf),
[Microsoft MCI_STATUS](https://learn.microsoft.com/en-us/windows/win32/multimedia/mci-status),
[MCI_SET](https://learn.microsoft.com/en-us/windows/win32/multimedia/mci-set),
[MCI_PLAY](https://learn.microsoft.com/en-us/windows/win32/multimedia/mci-play),
[CD track example](https://learn.microsoft.com/en-us/windows/win32/multimedia/playing-a-compact-disc-track),
[id Software Quake II CD client](https://github.com/id-Software/Quake-2/blob/master/win32/cd_win.c).
The SFF mirror is labelled revision 2.6 on its cover but retains revision 2.5
page footers; command/table references refer to that exact document.

Repository comparison: F4 owns task-file programming, channel serialization,
PIO phase checks, deadlines and quarantine in `ata.c`. CD commands extend its
packet whitelist and use that path. No separate register protocol is introduced.
The previous first-track-data readiness test prevented CD-DA-only media from
being exposed; readiness now caches format-0 TOC and mounting separately requires
a data first track. No filesystem probe is made for an audio first track.

| Packet | Encoding and source | Decision |
| --- | --- | --- |
| MODE SENSE(10), 5A | page 2A; MMC-3 6.3.11 table 361; SFF 10.8.6.4 | Decode page-relative bytes 4/5, not mode-header-relative bits; absent page means unknown capability. |
| READ TOC, 43 | format 0, LBA; MMC-3 5.23 tables 230–233; SFF 10.8.19 | Cache track number, ADR/CONTROL, start and AA lead-out per generation. CONTROL bit 2 marks data. |
| READ CD, BE | sector type 001b, byte 9=10h; MMC-3 5.17 tables 147/148/153/154 | 2352-byte S16LE stereo sectors, 1–27 sectors per packet. Refuse a range intersecting data before issuing any read. |
| PLAY AUDIO MSF, 47 | MMC-3 5.13; SFF 10.8.9 | Absolute MSF = LBA+150, exclusive end. Analog fallback only. |
| PAUSE/RESUME, 4B | MMC-3 5.10; SFF 10.8.7 | Byte 8 bit 0 selects resume. |
| STOP PLAY/SCAN, 4E | MMC-3 5.37 | Explicitly stop paused or playing analog audio. |
| READ SUB-CHANNEL, 42 | SubQ, format 1, LBA; MMC-3 5.22 tables 216–220; SFF 10.8.18 | Drive position applies only to analog playback. |
| START STOP UNIT, 1B | SFF 10.8.25 table 155 | Existing F4 LoEj/Start path for tray close; existing eject preflight for mounted media. |

MMC-3 table 154 specifies the L-low/L-high/R-low/R-high order. Without stream
accuracy, loss of streaming cannot be repaired by blindly reissuing a READ CD:
section 5.17 describes uncertain alignment. Therefore the primary reader does
not retry failed extraction. It switches to analog at the last consumed sector
only on a recoverable digital failure and an Audio Play capable drive. Media
change/no-medium failures terminate with MCI_NOTIFY_FAILURE instead.

MCI exposes continuous track numbers, per-track status length in MSF even with
TMSF selected, per-track start positions in the selected format, and the three
CD time formats. The Quake II client queries type and length then combines the
MSF length with a TMSF track number. This is a required compatibility case.

Guest boundary: [QEMU 9.2 ide-cd implementation](https://github.com/qemu/qemu/blob/v9.2.0/hw/ide/atapi.c)
returns data-track TOCs for block images; the suite uses the existing xorriso
fixture backend and explicit secondary-slave ide-cd frontend from F4. A CUE/BIN
is a host fixture and authored-disc mastering input, not an ISO masquerading as
a Red Book image. The new kernel-observed `case=cdaudio` result is separate from
the inherited F5-12B `case=media` waveform/sequencer/AVI acceptance predicate.
The latter still requires the earlier work's guest exercise integration by the
lead; it is deliberately retained, not reported as passing by the CD fixture.

Per-path state machines and native-to-MCI error mapping are part of the
[approved-boundary implementation notes](../design/f4-audio-media-contract.md#cd-audio-transport-f5-12b2-lead-amendment-2026-10-20).
No target CPU, audible timing, analog routing, or real-disc qualification is
claimed from host execution. All exact commands and results belong in
`build/codex/f5-12b2-report.md`.
