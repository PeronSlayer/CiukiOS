# CiukiOS hardware identification names

`pci-ids.json` and `usb-ids.json` contain bus ID pairs and ASCII device names
of at most 64 characters. Labels never select drivers or authorize hardware
access. The Control Panel loads each table lazily from `/System/hwids/`.
No Windows driver binary or INF file is copied into the repository or image.

## Reproduction and snapshots (f2-123)

Generated on 2026-10-23 from the owner's read-only **Ultimate Windows Driver
Pack (for vista,xp,98,95,me,2000,2003)** collection (909 case-insensitive INF
files), plus the public PCI and USB ID repositories. Run:

```sh
python3 scripts/build_hwids.py /path/to/INF-collection
```

`CIUKIOS_INF_DIR` can supply the positional argument. `--output-dir`,
`--pci-ids` and `--usb-ids` override paths. The generator requires exact pinned
snapshots (default `build/downloads/pci.ids` and `build/downloads/usb.ids`):

| Database | Original version | SHA-256 |
| --- | --- | --- |
| PCI | 2026.10.01 | `4f020f031d1f2d011b2a656a191519a056cb9744229551aed2cf7f90dfc2e18a` |
| USB | 2026.06.26 | `34b2b2289508a866cf38b96e54b37ac52c0ab7ce8361a56116af24e3fd094421` |

The original snapshots were copied from this workstation's hwdata package.
Direct upstream downloads failed DNS resolution in the implementation sandbox.
Do not substitute an unpinned current download; snapshot changes require an
explicit generator pin update. The raw databases stay ignored under
`build/downloads/`. [Notices](../../docs/licenses/hwids-NOTICES.txt) credit
both upstreams and hwdata; the notices and GPL text are installed in the image.

## Selection and precedence

The parser resolves neutral `[Strings]` first, then stable sorted locale
sections, handles UTF-16 and continuations, and accepts literal, quoted,
token and mixed descriptions in Manufacturer model sections and decorations.
Minimal INFs without Manufacturer may use `[Models]`. IDs in registry,
Strings, ControlFlags or install sections do not count. Duplicates choose the
shortest name, then case-insensitive lexical order, then bytewise order.

Scope is the requested classes: display, multimedia, network, storage,
bridges, serial bus/USB, input and communication. Recognized INF setup
classes are enumerated in `scripts/build_hwids.py`; System is mixed and only
explicit bridge/SMBus/USB/LPC/AGP/root-port function descriptions or individually
scoped pairs survive. Bluetooth is communication; biometric and smart-card
readers are input. Unknown
classes are omitted unless the pair is explicitly classified.

Neither public database maps individual device pairs to device classes.
`retro-scope.json` therefore lists individual public-source pairs by class,
covering the display-family contract, the baseline sample and explicit common
legacy peripherals. No vendor wildcard or description keyword guesses expand
the public tables. This is a bounded naming catalogue, not an exhaustive census
of every historical peripheral. New public pairs require an explicit class.

INF names win for every included pair, including scoped System devices.
Next come `native-pci-ids.json` reference labels (six native NICs plus i740 PCI
and QEMU/Bochs VGA). Public database names fill remaining scoped gaps.
The AudioPCI names formerly maintained by hand are now reproducibly supplied
from pci.ids. Native NIC provenance remains in `docs/validation/f6-08-nic-drivers.md`;
new reference-label sources are in the notices.

The class restriction and Models-only parsing intentionally remove unrelated
and non-model rows from the older table. A current validation record and
coverage counts are in `docs/validation/f2-123-hardware-names.md`.

## Format and size

The JSON stays flat, sorted, unescaped ASCII compatible with the existing
Control Panel scanner. Its combined 400 KiB ceiling remains enforced before
writing either output; neither runtime memory limits nor loaders change.
The two resident text allocations together cost file sizes plus two trailing
NUL bytes (excluding allocator and stdio overhead). This is an allocation
calculation, not a measurement of the full Control Panel process.
