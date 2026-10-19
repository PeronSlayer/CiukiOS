# FluidR3 GM banks

Frank Wen's Fluid (R3), copyright 2000–2002, 2008, MIT. Preserve
`LICENSE-FluidR3.txt` and `AUTHORS-FluidR3.txt` with both banks.

The pinned source archive is
[fluid-soundfont_3.1.orig.tar.gz](https://deb.debian.org/debian/pool/main/f/fluid-soundfont/fluid-soundfont_3.1.orig.tar.gz),
134,835,922 bytes, SHA-256
`2621acaa1c78e4abdb24bdd163230cc577e61276936d6aa6e3180582142f0343`.
The hash is independently recorded in the
[Buildroot package introduction](https://lists.buildroot.org/pipermail/buildroot/2019-December/258213.html).
The original full GM bank is 148,398,306 bytes, SHA-256
`74594e8f4250680adf590507a306655a299935343583256f3b722c48a1bc1cb0`,
recorded by the [archive mirror's LFS object](https://github.com/Jacalz/fluid-soundfont/blob/master/original-files/FluidR3_GM.sf2).
The mirror retains [Wen's licence and authors](https://github.com/Jacalz/fluid-soundfont/tree/master/original-files).

Stage the archive in `build/downloads/`, then run:

```sh
nice -n 19 python3 assets/audio/soundfonts/prepare.py --archive build/downloads/fluid-soundfont_3.1.orig.tar.gz
```

The offline tool verifies both source hashes, retains all 128 GM programs
and the percussion bank, and produces `FluidR3_Ciuki.sf2` plus an exact
inventory. The reduced derivative box-filters and decimates S16 samples by
eight, scaling sample rates, loops and fine address generators. Nonzero
coarse offsets fail explicitly. This lowers the bandwidth to roughly an
eighth of the original; listening qualification is required. It is not a
claim of perceptual equivalence to the full bank.

**Current checkout:** the archive and binary banks were unavailable in the
implementation sandbox; there is no fabricated reduced-bank digest or
claim of a completed FluidR3 coverage/listening check. `inventory.json`
is written only after generating and validating the actual bytes. The image
installer fails closed when the inventory or a bank is missing/mismatched.
The source archive belongs in the download cache, not in this directory.
