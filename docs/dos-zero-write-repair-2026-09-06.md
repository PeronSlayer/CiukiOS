# DOS zero-count writes: allocation repair

`INT 21h/AH=40h, CX=0` previously changed only the directory size. Extending
a 9468-byte file to 16124 bytes still left three 4096-byte clusters allocated:
the on-disk file advertised 16124 bytes but only 12288 bytes could be read.
Truncation likewise retained all the old clusters. This is independent of the
separately investigated Windows program-group failure.

The zero-count branch now resizes the FAT chain before updating the size. It
allocates additional clusters, frees a truncated tail (including truncation to
zero), and rolls back newly allocated clusters when the volume fills during
extension. The shared growth allocator stops at the volume's last data cluster
instead of searching past the data area. Existing bytes and the current file
position remain unchanged. Newly extended bytes have unspecified content, as
per the [DOS AH=40h contract](https://fd.lod.bz/rbil/interrup/dos_kernel/2140.html).

## Actual installed-HDD qualification

`scripts/qemu_test_write_zero.py` boots a copy of the installed HDD without a CD.
The guest fixture uses DOS open, seek, zero-write, close, reopen, read and write
calls. It copies every resulting byte into a separate file. A separate host
FAT16 parser checks the physical chains, preserved data, complete copies, both
FAT copies and the net allocation/free count after QEMU closes the disk.

| Operation | Chain before | Chain after | Guest read/copy |
|---|---:|---:|---:|
| 9468 → 16124 bytes | 3 | 4 | 16124 bytes |
| 9468 → 70013 bytes | 3 | 18 | 70013 bytes |
| 70013 → 4097 bytes | 18 | 2 | 4097 bytes |
| 70013 → 8192 bytes | 18 | 2 | 8192 bytes |
| 70013 → 0 bytes | 18 | 0 | 0 bytes |
| Empty → 12345 bytes | 0 | 4 | 12345 bytes |

The test checks preserved CX/DX and the unchanged seek position. With a test
volume containing only two free clusters, extending 9468 → 70013 bytes returns
DOS error 5, preserves the original three-cluster chain and all 9468 bytes,
and leaves exactly the same two clusters free. No partial extension leaks.

Evidence directory: `build/full/ui-multiwindow-2026-09-06/`:

- `cx0-before/results.json`: old-kernel short-allocation failure reproduced.
- `cx0-after-2/results.json`: six resize/read/copy cases pass.
- `cx0-full-after/results.json`: full-volume rollback passes.

The qualified candidate is `cx0-kernel.sys`, SHA-256
`bf53f4a018c084744114373515ff3319780e8972287f2fcda59ed8572244b7fe`.
It is an installed-image candidate using the exact volume geometry and LBA 63;
the release builder must rebuild with its own matching geometry. The unrelated
Windows MCB/PSP repair is being integrated separately. These runs do not claim
physical ThinkPad qualification or arbitrary power-failure atomicity.
