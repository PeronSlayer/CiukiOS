# VGA direct-map and banked-window performance

## Primary references

- Intel, *64 and IA-32 Architectures Software Developer's Manual*, combined official PDF: <https://cdrdv2-public.intel.com/825743/325462-sdm-vol-1-2abcd-3abcd-4.pdf>. The paging chapters describe accessed/dirty state in cached translations and TLB invalidation after paging-structure changes. The implementation therefore must not clear PTE dirty bits while retaining a translation that can continue using the prior dirty state; harvest points that clear those bits are followed by a TLB flush.
- QEMU v9.2.0 `hw/display/vga.c`: <https://github.com/qemu/qemu/blob/v9.2.0/hw/display/vga.c>. The upstream VGA model selects aperture/memory map, chain-4/odd-even access, read plane, write mode and write-plane mask from VGA register state. This supports keeping target recomputation at the register writes that can affect those inputs and preserving register-index/data phase semantics.

## Decision

Avoid redundant HDPMI direct-plane work without changing the VGA access model. When a VGA register write computes the same direct plane and permissions already mapped, keep the current PTE dirty bits intact and return without harvesting or flushing. When the direct mapping changes, harvest dirty pages from the old writable plane, install the new mapping, and flush once. At frame-ready and timer-tick boundaries, harvest the current writable plane; clear its PTE dirty bits and flush only if at least one page was dirty. Only signal frame-ready for an actual change to CRTC start high/low data; tracking the two-byte pair as a single atomic event is deferred because writes can arrive in either order and there is no reliable pair boundary in this adapter.

In banked host framebuffer mode, validate the full 16-page source window before mutation, snapshot the original aperture PTEs once, and compare desired PTEs against the current entries. Write only changed mappings and reload CR3 only when at least one entry changes. On unmap, restore the exact saved PTE words and reload CR3 only when restoration changes an entry. Keep the existing shared-block and instance layouts unchanged.

## Measurement hooks

The existing `pm_port_writes`, `pm_direct`, `vga->changes`, and present timing fields remain available to correlate register traffic, active mapping, harvested pages and frame cost. `cvvid_shared` has no reserved bytes, so this change adds no counters or structure fields.

## Validation status

The host mapping tests passed with ASan/UBSan against the real VGA model. The full build and bounded new-game/two-VM runtime check passed; see [integrated validation](2026-10-03-independent-vm-clock.md). The runtime loop-counter measurement does not isolate a numerical speedup attributable to VGA mapping alone.
