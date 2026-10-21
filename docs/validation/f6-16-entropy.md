# f6-16 entropy: Amendment 3 hardware procedure

Status (2026-10-21): **pending hardware qualification**. Host fixtures and
synthetic clocks cannot qualify T23 entropy. No hardware image, serial capture
or NIST assessment was produced by this amendment's implementer.

## Research and accepted pinned behavior

[jitterentropy-library v3.7.0 CHANGES.md](https://github.com/smuellerDD/jitterentropy-library/blob/v3.7.0/CHANGES.md)
records removal of loop-count shuffling and replacement of SHA3-256 output
generation with XDRBG-256. Amendment 3 accepts the pinned SHAKE-256/XDRBG
conditioner; SHA3-256 remains in the noise workload. No upstream algorithm,
health threshold, oversampling rate or archive pin changes in this amendment.
The scheduler yield contributes to combined-noise timing deltas.

[SP 800-90B](https://csrc.nist.gov/pubs/sp/800/90/b/final) distinguishes
sequential runtime and restart evidence from online health tests. The
[pinned upstream assessment instructions](https://github.com/smuellerDD/jitterentropy-library/blob/v3.7.0/tests/raw-entropy/README.md)
describe the runtime estimate and the separate restart analysis. The
[NIST assessment implementation](https://github.com/usnistgov/SP800-90B_EntropyAssessment/blob/master/cpp/non_iid_main.cpp)
accepts one sample per byte and at least one million entries. Use initial
raw-source estimation (`-i`), all bits (`-a`), eight bits per sample below.
Record the exact NIST tool commit and build/compiler before running it.

## Lead procedure: T23 capture at 38400

1. Review/integrate the amendment and run the complete host gate and canonical
   build outside the sandbox. Record the image SHA-256, compiler flags, pinned
   jitterentropy archive SHA-256, T23 CPU/memory configuration, boot entropy
   cost, source/health/reseed records and the effective UART configuration.
2. Use the normal image and add the exact token `entropyraw=1` to `BOOT.CFG`.
   Keep the DRBG enabled; no special image or entropy-credit override is used.
   Boot collection still has its existing 5 s/131,072-round cap and emits no
   raw stream. Runtime capture requires successful startup and an entropy task.
3. On the receiving host, start an 8N1 serial capture at **38400** before T23
   boot. The following commands are examples for the lead to run, not evidence
   that this procedure has been executed. Select the actual receiver port;
   keep captures in ignored local evidence storage, never in tracked files:

   ```bash
   mkdir -p legacy/local/f6-16
   capture_port=/dev/ttyUSB0
   stty -F "$capture_port" 38400 raw -echo cs8 -cstopb -parenb -ixon -ixoff -crtscts clocal
   cat "$capture_port" > legacy/local/f6-16/t23-serial.log
   ```

   Capture through `[entropy] raw complete samples=1048576 bits=8`, then stop
   the receiver with Ctrl-C. Allow minutes: hex encoding and framing exceed
   two million wire bytes. Preserve other boot/entropy records in the log.
   A stopped/unavailable record, a failed health record, missing completion,
   or a missing/repeated/malformed data frame means an incomplete run.
4. Decode the low byte of each GCD-normalized combined-noise timing delta.
   Records retain zeros and rejected/stuck inputs, before conditioning.
   This strict example rejects gaps, duplicates, bad lengths and incomplete
   files before writing the one-byte-per-sample corpus:

   ```bash
   python3 - <<'PY'
   from pathlib import Path
   import re
   folder = Path('legacy/local/f6-16')
   samples = bytearray()
   complete = False
   for row in (folder / 't23-serial.log').read_bytes().splitlines():
       if not row.startswith(b'[entropy] raw '):
           continue
       match = re.fullmatch(rb'\[entropy\] raw index=(\d+) count=(\d+) data=([0-9a-f]+)', row)
       if match:
           assert not complete, 'data after completion'
           index, count = map(int, match.group(1, 2))
           assert index == len(samples), 'gap or duplicate'
           assert 1 <= count <= 64 and len(match[3]) == 2 * count, 'bad frame'
           samples.extend(bytes.fromhex(match[3].decode('ascii')))
           assert len(samples) <= 1048576, 'sample overflow'
       else:
           assert row == b'[entropy] raw complete samples=1048576 bits=8', row
           assert not complete and len(samples) == 1048576, 'bad completion'
           complete = True
   assert complete and len(samples) == 1048576, 'incomplete capture'
   (folder / 't23-raw.bin').write_bytes(samples)
   print('validated', len(samples), 'ordered samples')
   PY
   sha256sum legacy/local/f6-16/t23-serial.log legacy/local/f6-16/t23-raw.bin
   ```

5. Run the official NIST tool, keeping the exact version, command, stdout,
   stderr, exit status and input SHA-256. From the repository directory, with
   the built `ea_non_iid` on PATH:

   ```bash
   ea_non_iid --version > legacy/local/f6-16/nist-version.txt 2>&1
   nice -n 19 ea_non_iid -i -a -v legacy/local/f6-16/t23-raw.bin 8 > legacy/local/f6-16/t23-non-iid.txt 2>&1
   ```

   Assess the smaller of H_original and eight times H_bitstring against the
   pinned source's 1/OSR raw-delta assumption (OSR at least 3), including its
   safety factor and health-test model. Do not increase Ciuki's one-bit-per-
   delivered-output-byte credit from this measurement. Record the numerical
   result and lead conclusion in a new validation record linked to the image.
6. Remove `entropyraw=1` for normal use. Confirm that the normal boot emits
   no raw records and that collection pauses at full fresh credit. Preserve
   the image hash and a concise result in tracked validation; keep full local
   captures out of the repository.

## Limits and remaining evidence

The hook buffers at most one bounded attempt (131,072 bytes; 33 PMM pages) and
publishes frames only after fresh credit is filled (or one read when already
full). Serial backpressure/draining uses the
existing queue and UART IRQ without holding the entropy pool mutex; native
DRBG requests and reseeds remain active. Collection continues at full credit
only while the explicit capture quota remains. Storage is erased/freed after
completion, serial failure, or collector retirement. Nothing streams by default.

Combined deltas span consecutive calls: scheduler work and serial drain waits
between batches affect the next delta. The low-byte corpus describes this
instrumented runtime, not an unperturbed source or startup noise components.
UART submission tickets cannot prove remote receipt; the parser verifies the
received framing, and the physical capture still needs review. Exposed raw
data does not establish TLS qualification. A single million-sample runtime
capture does not replace the separate SP 800-90B restart dataset/assessment
or other supported-machine qualification. Those remain pending with the lead,
along with T23 startup/calibration accuracy, memory admission and boot cost.
