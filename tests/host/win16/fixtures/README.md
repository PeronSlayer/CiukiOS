# f5-19a synthetic NE fixtures

`corpus.json` is the source manifest for every case: `model` describes the
NE image, `mutations` inject faults after a valid body is laid out, `rules`
names the contract obligations and `expect` is the handwritten oracle (parse
rule, load policy, imports by ordinal/name, exports, segment and resource
metadata, expanded bytes, hand-computed relocation patches, relocation
refusal). `evidence` states that execution is pending: these are structural
host fixtures, never run as 16-bit code.

`scripts/win16/fixtures.py` lays the bytes out deliberately, including forms
a linker would never emit. `hashes.json` pins the SHA-256 of every generated
image; `census.txt` is the committed text census of the corpus. Generated
images live in `build/host/win16/current`; tests build into temporary
directories under `build/host/win16` and remove them.

Run from the repository root, one suite at a time:

```sh
nice -n 19 python3 -B scripts/win16/fixtures.py --output build/host/win16/current \
    --census tests/host/win16/fixtures/census.txt
ASAN_OPTIONS=detect_leaks=0 nice -n 19 python3 -B -m unittest discover -s tests/host/win16 -t .
```

Regenerate `hashes.json` (copy it from the output directory) and
`census.txt` only together with a reviewed change to the generator or the
manifests. Real Open Watcom 1.9 fixtures are pinned pending download in the
contract appendix "f5-19a inputs", section C.
