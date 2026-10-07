# T23 LCD panel-size detection research

Research for the ThinkPad T23 graphics path, 2026-10-07. This note concerns
the internal LCD's native panel size. It does not identify the maximum mode of
an external monitor.

## Evidence

The upstream X.Org `xf86-video-savage` 2.4.1 source implements
`SavageGetPanelInfo()` in `src/savage_driver.c` (function begins near line
976). For the Savage mobile family, including SuperSavage, it reads CRTC
register CR6B and sequencer registers SR61, SR66, SR69 and SR6E. It decodes:

```text
panel_width  = (SR61 + ((SR66 & 0x02) << 7) + 1) * 8
panel_height =  SR69 + ((SR6E & 0x70) << 4) + 1
```

CR6B bit 1 is named `ActiveLCD` (`0x02`). The driver logs the decoded size,
but only saves it as the panel dimensions and constrains display modes when
that bit says the LCD is active. The source groups `S3_SUPERSAVAGE` with
`S3_SAVAGE_MX` as the mobile family and calls this path for that family unless
CRT-only operation was requested.

The source's VGA MMIO aliases are sequencer index/data at BAR0 offsets
`0x83C4/0x83C5` and color CRTC index/data at `0x83D4/0x83D5`. Its mapping code
uses BAR0 plus offset zero for SuperSavage MMIO, a 512 KiB MMIO range, and
BAR1 for framebuffer VRAM. On this machine, physical PCI discovery reports
vendor/device `5333:8C2E`, a SuperSavage IX/C variant. These facts make the
panel registers a plausible read-only native report source after verifying
the PCI identity and BAR resource.

IBM's T20/T21/T22/T23 Hardware Maintenance Manual lists T23 configurations
with 13.3- or 14.1-inch XGA panels and 14.1-inch SXGA+ panels at 1400x1050.
Its LCD FRU table separately lists 14.1-inch XGA panels and 14.1-inch SXGA+
panels. IBM's T23 Service and Troubleshooting Guide states the LCD supports
up to 1400x1050, depending on model. Thus the machine family includes multiple
panel sizes; neither the product family nor the highest enumerated BIOS mode
proves which panel is fitted in this unit.

## Recommendation

Add a host-side `native LCD panel` report only when the PCI identity and BAR0
resource are valid and CR6B reports `ActiveLCD`. Read the four sequencer
values and CR6B from the X.Org-verified VGA MMIO aliases, validate plausible
dimensions and a stable repeated snapshot, and return unavailable when the
LCD is inactive, the registers cannot be read reliably, or the values fail
sanity checks. Preserve and restore the indexed-register selectors; do not
change panel timing, display mode, or extended-register lock state to obtain
the report. A safe query that cannot read the current state should return
unknown rather than unlocking or programming the device.

Use the result as the internal panel's native-size ceiling. Select only the
largest already validated VBE mode that fits within the detected dimensions.
Do not synthesize a new timing from the panel size: the current BIOS mode list,
VBE descriptor, successful mode set, and 4F03 access-path readback remain
required. If VBE does not expose a native-resolution mode, keep the best
validated lower mode. If the internal LCD is inactive, do not apply its panel
size as a limit to an external display.

## Primary sources and provenance

- [X.Org xf86-video-savage 2.4.1 upstream release archive](https://xorg.freedesktop.org/archive/individual/driver/xf86-video-savage-2.4.1.tar.xz), also listed in the [official X.Org driver archive](https://xorg.freedesktop.org/archive/individual/driver/). Relevant files: `src/savage_driver.c` around lines 976-1054 and 2012-2056; `src/savage_regs.h` around lines 20 and 64-112; `SavageMapMem()` around lines 2930-2970. The inspected archive is retained under ignored `build/external/savage-research/`; SHA-256 `674191160c61982199c582233c5f1df5163a09f3ecbcb3d5bb5079b31a7d10af`.
- [Lenovo-hosted T20/T21/T22/T23 HMM record](https://think.lenovo.com.cn/htmls/manual/detail_12506626371563870.html), whose attachment is the IBM Hardware Maintenance Manual. The inspected PDF was downloaded from that attachment; SHA-256 `347aa5abf1b4000bef8b8c76ee6b2c9dcf735c6703f6bdf18d786568b06f73ca`. Product overview: printed page 41; LCD FRU lists: printed pages 157-158.
- [IBM/Lenovo ThinkPad T23 Service and Troubleshooting Guide](https://download.lenovo.com/pccbbs/mobiles_pdf/t23tsguiden.pdf), display feature listing in chapter 1 (LCD up to 1400x1050, model-dependent).
