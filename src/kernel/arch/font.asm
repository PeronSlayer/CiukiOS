; Inconsolata bitmap (OFL 1.1), regular style of assets/fonts/native/INCONSOL.CFN:
; 95 advance widths followed by 95 glyphs of 16 little-endian 16-bit rows.
; Licence text: assets/fonts/LICENSES.TXT.
; SPDX-License-Identifier: OFL-1.1
section .rodata
global font_cfn_regular
font_cfn_regular:
    incbin "assets/fonts/native/INCONSOL.CFN", 64, 95 + 95 * 32

; f0-55 boot splash tagline: CiukiOS Sans (OFL 1.1; Liberation Sans renamed),
; regular style of assets/fonts/native/CIUKIOS.CFN in the same layout.
global font_cfn_sans
font_cfn_sans:
    incbin "assets/fonts/native/CIUKIOS.CFN", 64, 95 + 95 * 32
