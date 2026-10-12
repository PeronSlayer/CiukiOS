# Provisional system sounds

Interface Sounds 1.0 by **Kenney**, licensed **CC0 1.0 Universal**.
Source: https://kenney.nl/assets/interface-sounds (licence checked 2026-10-12).
Licence: https://creativecommons.org/publicdomain/zero/1.0/
Archive: https://kenney.nl/media/pages/assets/interface-sounds/fa43c1dd4d-1677589452/kenney_interface-sounds.zip
SHA-256: `f2193d072726d6758a5f7871b2dcc54dcce0d5c35c6f0a62f92549b327c81232`.

The original archive, individual Ogg files and upstream notice already reside
in `kenney-interface/`. `scripts/build_sounds.py` verifies the archive into
`build/downloads`, verifies each selected member, decodes with ffmpeg (no
metadata or dithering), and writes canonical mono 16-bit PCM WAV at 22050 Hz.
It records output hashes and the converter version; reproducibility is for
that converter version. The canonical image installs the WAVs and credit and
licence notices in `/system/sounds`, and `/system/settings/sounds.cfg`.
The installed set is less than 1 MiB. The owner will compose replacements.

The old `native/` raw-PCM files and converter remain for the legacy line;
the protected-mode desktop uses the new WAV set. See
`docs/design/system-sounds.md` for the player and research decisions.
