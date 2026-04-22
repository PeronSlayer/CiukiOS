# OpenGEM payload for full image

Put your launcher binary here with this exact name:

- `OPENGEM.COM`

Build command:

```bash
bash scripts/build_full.sh
```

If `assets/full/opengem/OPENGEM.COM` exists, it is injected into the FAT16 root of `build/full/ciukios-full.img` and can be launched from the CiukiOS shell with:

```text
opengem
```

Current layout constraint for full image packaging: `OPENGEM.COM` must be <= 512 bytes.
