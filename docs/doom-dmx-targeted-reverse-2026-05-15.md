# DOOM DMX targeted reverse report - 2026-05-15

## Scope

Target binary:

- `third_party/Doom/DOOM.EXE`
- SHA256: `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`
- Embedded LE payload offset: `0x25214`
- Local extracted payload: `build/rev/doom_part_25214.exe`

This report only covers the DOOM/DMX sound path. It does not retest generic
SB16, IRQ, DMA, AdLib, config, or Stage1 DPMI behavior.

## Method

Used targeted binary instrumentation on temporary build artifacts only:

- patched copied `DOOM.EXE` binaries under `build/rev/`
- injected patched binaries into `build/full/ciukios-full.img`
- did not modify `third_party/Doom/DOOM.EXE`
- did not commit patched DOOM binaries

The reliable marker method was a reversible `EB FE` hang probe at specific
protected-mode code addresses. If DOOM stopped after `S_Init` and before
`HU_Init`, the patched address was proven reached.

The earlier direct serial-marker attempt was rejected because DOS/4GW segment
layout made simple DS-based string reads unsafe. The hang probes were kept as
the trusted evidence.

## Static map

Important virtual addresses inside the extracted LE payload:

- `0x2B700`: `I_StartupSound`-like routine
- `0x2B800`: call to DMX init routine
- `0x29520`: `DMX_Init`-like routine
- `0x4F520`: `S_Init`-like routine
- `0x4F533`: call to `I_SetChannels`
- `0x2B860`: `I_SetChannels`
- `0x26240`: `WAV_PlayMode` wrapper
- `0x214E0`: SB/FX backend init candidate reached through `WAV_PlayMode`
- `0x21721`: SB16 DMA start command path, around DSP command `0xC6`
- `0x21260`: candidate SB IRQ/service routine

Relevant reverse facts:

- `I_SetChannels` loads `0x2B11` (`11025`) and calls `0x26240`.
- `0x26240` dispatches through the selected DMX backend vtable at `[0x1150]+8`.
- `DMX_Init` sets the selected backend through `0x26260`.
- The SB16 backend path contains direct DSP/DMA programming, including DSP
  command `0x41` for sample rate and `0xC6` for SB16 DMA playback.

## Runtime evidence

Baseline SB16 lane still reaches:

- `I_StartupSound`
- `calling DMX_Init`
- `S_Init`
- `HU_Init`
- `runtime_stable=PASS`

Targeted hang probes:

- Patch at `0x2B860` (`I_SetChannels`) stops after `S_Init` and before `HU_Init`.
- Patch at `0x26240` (`WAV_PlayMode` wrapper) stops after `S_Init` and before
  `HU_Init`.
- Patch at `0x214E0` (SB/FX backend init candidate) stops after `S_Init` and
  before `HU_Init`.
- Patch at `0x21721` (SB16 DSP DMA start path) stops after `S_Init` and before
  `HU_Init`.
- Patch at `0x21260` (candidate SB IRQ/service routine) does **not** stop DOOM;
  DOOM still reaches `HU_Init`.

## Requested checkpoints

1. After `DMX_Init`: reached. Existing SB16 logs reach `S_Init`; focused logs
   reach past `calling DMX_Init`.
2. After `I_SetChannels`: reached in normal binary, because patching
   `I_SetChannels` entry stops before `HU_Init`, while the unpatched binary
   reaches `HU_Init`.
3. Entering `WAV_PlayMode`: reached. Patch at `0x26240` stops before `HU_Init`.
4. Leaving `WAV_PlayMode`: reached in normal binary by inference; unpatched DOOM
   reaches `HU_Init` after `S_Init`. No numeric return value captured; public
   references treat `WAV_PlayMode` as void.
5. Entering `FX_Init`: reached at SB/FX backend candidate `0x214E0`.
6. Leaving `FX_Init`: reached in normal binary by inference; unpatched DOOM
   reaches `HU_Init`.
7. First `S_StartSound`: not proven in this lane. The SB16 startup path stalls
   before stable visual gameplay, so no gameplay SFX event is reached in the
   observed automation window.
8. First `SFX_PlayPatch`: not proven.
9. First `FX_PlayRaw`: not proven.
10. SB DMA start from inside DOOM/DMX: reached. Patch at `0x21721` stops before
    `HU_Init`, proving the SB16 DMA start path is executed during sound setup.
11. SB IRQ/callback returning into DOOM/DMX: not proven. Patch at candidate
    handler `0x21260` does not stop execution; DOOM still reaches `HU_Init`.

## First proven missing marker

The first proven missing marker is:

**SB IRQ/callback returning into DOOM/DMX.**

`WAV_PlayMode`, the SB/FX backend, and SB16 DMA start are now proven reached.
The current evidence points past setup and toward the callback/interrupt/service
return path used by DMX after starting SB16 DMA.

## Most likely diagnosis

DOOM/DMX starts the SB16 DMA path, but the expected DMX SB interrupt/service
callback path is not proven to return into the engine.

This is narrower than previous hypotheses:

- not raw SB16
- not protected-mode IRQ/DMA in general
- not `I_SetChannels`
- not `WAV_PlayMode`
- not SB16 DMA start

The likely fault is now one of:

- the actual IRQ vector/service routine is not the candidate at `0x21260`
- DOS/4GW vector/passup registration is different from the candidate path
- DMX expects a callback/service dispatch condition that is not being satisfied
- the IRQ fires but is routed to a different thunk or swallowed before the
  candidate code

## Next single minimal test

Do not patch Stage1.

Next test:

**Locate the actual SB IRQ vector installation in DOOM/DMX and patch that exact
installed handler or callback target, not the guessed `0x21260` candidate.**

Suggested anchors:

- calls around `0x23AA0`, used near SB IRQ setup
- calls around `0x238F0`, used with IRQ number setup
- calls around `0x20E40`, used in SB detection/setup
- writes to callback pointer `[0x584]`
- DPMI/vector/passup-related calls near the SB setup region

Acceptance for next test:

- A hang/marker at the actual installed handler fires after the `0x21721` DMA
  start point.
- If it does not fire, inspect PIC/DSP ack state after DOOM's DMA start rather
  than retesting helper probes.

## IRQ install follow-up

One focused follow-up was run against the SB IRQ install path.

Static result:

- SB setup calls `0x23AA0` at `0x217AA`.
- Input IRQ comes from `[0x2638C]`, observed config/default is IRQ7.
- `0x23AA0` maps IRQ7 to protected-mode interrupt vector `0x0F` (`irq + 8`).
- The SB callback/service pointer passed in `EDX` at the install call is
  `0x11260`.
- The DPMI set-vector helper is `0x50DF0`.

Runtime probe:

- Temporary patch: `EB FE` at VA `0x11260`.
- Full-file patch offset: `0x43274`.
- Result: DOOM still reaches `HU_Init`; the `0x11260` callback was not entered
  during the observed startup/DMA lane.

Conclusion:

- Installed IRQ vector number: `0x0F`.
- Recoverable callback target from SB install call: `0x11260`.
- Marker observed: NO.
- DMA start observed: YES from prior `0x21721` proof.
- IRQ handler/callback entered: NO for `0x11260`.
- Next single action: instrument the `0x50DF0` DPMI set-vector helper call at
  `0x23BAA` / `0x23C64` to capture the exact offset:selector actually written
  for vector `0x0F`; do not guess another handler address.

## IRQ vector target capture

Focused vector-log patch:

- Patched only the IRQ<=7 set-vector call at VA `0x23C64`.
- Full-file patch offset: `0x55C78`.
- Temporary stub cave: VA `0x12019`.
- Stub printed `EAX=vector`, `CX=selector`, `EBX=offset`, then called the
  original helper `0x50DF0`.

Observed serial output:

- `[VEC 08 0170:0015B1F0]`
- `[VEC 0A 0170:0015B240]`
- `[VEC 0B 0170:0015B270]`
- `[VEC 0C 0170:0015B2A0]`
- `[VEC 0D 0170:0015B2D0]`
- `[VEC 0F 0170:0015B330]`
- `[VEC 0F 0170:0015B330]`

Conclusion:

- INT31 helper address requested by the reverse task: `0x50DF0` set-vector
  helper path, reached through the IRQ installer.
- Installed vector: `0x0F`.
- Installed selector: `0x0170`.
- Installed offset: `0x0015B330`.
- Computed VA/file offset: not safely derivable from the current LE object map;
  this appears to be a DOS/4GW generated selector:offset thunk target, not the
  direct DMX callback VA.
- Equals `0x11260`: NO.
- DMA start observed: YES from prior `0x21721` proof.
- IRQ handler entered: not tested in this run; this run only captured the
  installed target.
- Next single action: resolve selector `0x0170` base or instrument the
  `0170:0015B330` thunk target with DOS/4GW-aware debugging/selector capture,
  then patch that exact target.

## IRQ selector/thunk entry probe

Focused selector-resolution patch:

- Patched only the IRQ<=7 set-vector call at VA `0x23C64`.
- Full-file patch offset: `0x55C78`.
- Temporary stub cave: VA `0x12019`.
- Stub called DPMI `INT 31h AX=0006` for selector base and used `LSL` for the
  selector limit before replacing only vector `0x0F` with a temporary CS-local
  IRQ7 marker/hang stub.

Observed serial output:

- `08 0170:0015B1F0 00000000 FFFFFFFF`
- `0A 0170:0015B240 00000000 FFFFFFFF`
- `0B 0170:0015B270 00000000 FFFFFFFF`
- `0C 0170:0015B2A0 00000000 FFFFFFFF`
- `0D 0170:0015B2D0 00000000 FFFFFFFF`
- `0F 0170:0015B330 00000000 FFFFFFFF`

Runtime result:

- DOOM/DOS4GW raised exception `05h` at `0170:000120FC` before `HU_Init`.
- The marker string `[IRQ7_ENTER]` was not observed.
- The final image was rebuilt cleanly after the temporary patch.

Conclusion:

- Vector: `0x0F`.
- Raw installed target: `0170:0015B330`.
- Selector base: `0x00000000`.
- Selector limit: `0xFFFFFFFF`.
- Computed linear target: `0x0015B330`.
- Mapping classification: flat DOS/4GW protected-mode selector, not safely
  mapped to a persisted DOOM LE file offset from the current static object map.
- Mapped LE VA/full-file offset: not safe.
- Patch location tested: temporary replacement handler at `0170:000120FC`.
- Marker/hang observed: NO.
- DMA start observed: YES from prior `0x21721` proof.
- IRQ/thunk entered: NO proof in this run; replacing the vector with a
  serial-printing handler caused a DOS/4GW bounds exception before the game
  reached the previous runtime point.
- Next single action: disassemble/runtime-dump the flat linear bytes at
  `0x0015B330` or add a non-printing memory flag at that exact flat target;
  do not serial-print from the IRQ handler path.

## IRQ target byte dump and non-printing probe

Focused single-lane probe:

- Patched only the IRQ<=7 set-vector call at VA `0x23C64`.
- Full-file patch offset: `0x55C78`.
- Temporary stub cave: VA `0x12019`.
- Stub dumped 64 bytes from the captured vector target `0170:0015B330` from the
  safe vector-install context, then replaced vector `0x0F` with a non-printing
  wrapper.
- The wrapper did not use serial output; it only attempted to increment a
  local memory flag and loop.

Bytes at `0x0015B330`:

```text
60 1E 06 0F A0 0F A8 89 E5 FC E8 A9 DA 02 00 B8
07 00 00 00 E8 97 01 00 00 0F A9 0F A1 07 1F 61
CF 8D 80 00 00 00 00 8D 92 00 00 00 00 8D 40 00
60 1E 06 0F A0 0F A8 89 E5 FC E8 79 DA 02 00 B8
```

Disassembly/classification:

```text
0015B330  60                pusha
0015B331  1E                push ds
0015B332  06                push es
0015B333  0FA0              push fs
0015B335  0FA8              push gs
0015B337  89E5              mov ebp,esp
0015B339  FC                cld
0015B33A  E8A9DA0200        call 0x188de8
0015B33F  B807000000        mov eax,0x7
0015B344  E897010000        call 0x15b4e0
0015B349  0FA9              pop gs
0015B34B  0FA1              pop fs
0015B34D  07                pop es
0015B34E  1F                pop ds
0015B34F  61                popa
0015B350  CF                iret
```

Conclusion:

- Classification: real generated DOS/4GW protected-mode IRQ thunk, not invalid
  or unmapped memory.
- Probe memory address: local cave flag at `0x000120CF`.
- IRQ target entered: NO proof; DOOM/DOS4GW raised exception `06h` at
  `0170:00012128` before the prior useful runtime window.
- DMA start evidence reused: YES, prior `0x21721` proof.
- Exception/crash: YES, invalid opcode `06h`, followed by DOS/4GW transfer
  stack overflow on interrupt `0Dh`.
- Final restore: `make build-full` rebuilt the image from the unpatched
  packaged `DOOM.EXE`.
- Next single action: do not replace the vector target; instead patch the real
  generated thunk shape by recreating its prologue in the wrapper and tail-call
  `0x188de8`/`0x15B4E0`, or dump DOS/4GW's thunk generator to identify the
  safe insertion point before the thunk is executed.

## External breakpoint proof for IRQ thunk entry

Focused non-invasive lane:

- Rebuilt the full image with `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`.
- Did not modify `DOOM.EXE`.
- Did not modify the generated thunk.
- Ran the existing DOS taxonomy launch with QEMU's GDB stub enabled.
- Attached GDB and set a hardware execution breakpoint at linear
  `0x0015B330`.

GDB evidence:

```text
Hardware assisted breakpoint 1 at 0x15b330
Breakpoint 1, 0x0015b330 in ?? ()
BREAK_HIT
cs             0x170
eip            0x15b330
eflags         0x10046             [ RF IOPL=0 ZF PF ]
```

Conclusion:

- Debug method used: QEMU gdbstub + GDB hardware execution breakpoint.
- Breakpoint address: `0x0015B330`.
- Breakpoint hit: YES.
- CS:EIP when hit: `0170:0015B330`.
- DMA start evidence reused: YES, prior `0x21721` proof.
- Exception/crash: NO in this lane; taxonomy reached `runtime_stable=PASS`.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS and packaged
  `::APPS/DOOM/DOOM.EXE` SHA256 restored to
  `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.
- Next single action: place the next non-invasive breakpoint on the thunk
  call-out target `0x00188DE8`, then on the later callback target reached via
  `0x0015B4E0`, to prove whether control reaches the DMX/SB service and where
  it returns or stalls.

## External breakpoint proof for IRQ thunk dispatch calls

Focused non-invasive lane:

- Rebuilt the full image with `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`.
- Did not modify `DOOM.EXE`.
- Did not modify the generated thunk.
- Ran the existing DOS taxonomy launch with QEMU's GDB stub enabled.
- Attached GDB and set hardware execution breakpoints at `0x0015B330`,
  `0x00188DE8`, and `0x0015B4E0`.

Observed hit order:

1. `0x00188DE8`
2. `0x0015B4E0`

`0x0015B330` was not observed in this specific multi-breakpoint run, but it was
already proven in the prior single-breakpoint lane.

Register snapshot at `0x00188DE8`:

```text
CS:EIP 0170:00188DE8
EAX=00000000 EBX=0000001E ECX=000003E8 EDX=00000004
ESI=001B8208 EDI=001B81E0 EBP=000064F0 ESP=000064EC
EFLAGS=00000046 [ IOPL=0 ZF PF ]
```

Register snapshot at `0x0015B4E0`:

```text
CS:EIP 0170:0015B4E0
EAX=00000000 EBX=0000001E ECX=000003E8 EDX=00000004
ESI=001B8208 EDI=001B81E0 EBP=000064F0 ESP=000064EC
EFLAGS=00000046 [ IOPL=0 ZF PF ]
```

Conclusion:

- Hit `0x0015B330`: NO in this lane; YES in prior lane.
- Hit `0x00188DE8`: YES.
- Hit `0x0015B4E0`: YES.
- Return from `0x00188DE8`: YES, inferred because execution reached the next
  thunk call target `0x0015B4E0`.
- Return from `0x0015B4E0`: no exception was observed and taxonomy remained
  stable after GDB detached; direct single-step return was not captured.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward.
- Next single action: break after `0x0015B4E0` returns at thunk address
  `0x0015B349` and/or step into `0x0015B4E0` to identify the final DMX/SB
  callback target reached from the DOS/4GW dispatcher.

## External breakpoint proof for thunk-owned `0x15B4E0` call

Focused non-invasive lane:

- Rebuilt the full image with `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`.
- Did not modify `DOOM.EXE`.
- Did not modify the generated thunk.
- Ran the existing DOS taxonomy launch with QEMU's GDB stub enabled.
- Attached GDB and used enabled/disabled hardware breakpoints to follow only
  the `0x15B330` thunk-owned path.

Observed sequence:

1. `0x0015B330` thunk entry hit.
2. `0x0015B33F` hit after `call 0x188DE8` returned.
3. `mov eax,7` executed.
4. `0x0015B344` hit immediately before the thunk-owned `call 0x15B4E0`.
5. `0x0015B4E0` hit from that thunk-owned call.
6. `0x0015B349` hit after `0x15B4E0` returned.

Key register evidence:

```text
THUNK_HIT:
CS:EIP=0170:0015B330 EAX=000000F2 EBX=000000C8 ECX=000000F2 EDX=0000022C
ESI=001B8208 EDI=00000007 EBP=001B81EC ESP=00006520 EFLAGS=00010046

RETURNED_188DE8_AT_MOV_EAX_7:
CS:EIP=0170:0015B33F EAX=000000F2 EBX=000000C8 ECX=000000F2 EDX=0000022C
ESI=001B8208 EDI=00000007 EBP=000064F0 ESP=000064F0 EFLAGS=00000046

BEFORE_CALL_15B4E0:
CS:EIP=0170:0015B344 EAX=00000007 EBX=000000C8 ECX=000000F2 EDX=0000022C
ESI=001B8208 EDI=00000007 EBP=000064F0 ESP=000064F0 EFLAGS=00000046

AT_15B4E0_ENTRY:
CS:EIP=0170:0015B4E0 EAX=00000007 EBX=000000C8 ECX=000000F2 EDX=0000022C
ESI=001B8208 EDI=00000007 EBP=000064F0 ESP=000064EC EFLAGS=00000046

RETURNED_FROM_15B4E0:
CS:EIP=0170:0015B349 EAX=00000020 EBX=000000C8 ECX=000000F2 EDX=0000022C
ESI=001B8208 EDI=00000007 EBP=000064F0 ESP=000064F0 EFLAGS=00000046
```

Conclusion:

- Thunk hit: YES.
- Stepped call `0x188DE8`: YES.
- Returned from `0x188DE8`: YES.
- EAX before call `0x15B4E0`: `0x00000007`.
- Entered `0x15B4E0` from thunk: YES.
- EAX at `0x15B4E0` entry: `0x00000007`.
- Returned from `0x15B4E0`: YES, to `0x0015B349`.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward.
- Next single action: step into/disassemble `0x15B4E0` with `EAX=7` and locate
  the actual DMX/SB callback it dispatches to; the IRQ thunk itself is no
  longer the blocker.

## External GDB trace inside `0x15B4E0`

Focused non-invasive lane:

- Rebuilt the full image with `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`.
- Did not modify `DOOM.EXE`.
- Did not modify the generated thunk.
- Ran the existing DOS taxonomy launch with QEMU's GDB stub enabled.
- Followed the thunk-owned path into `0x15B4E0` with `EAX=7`.

Dispatcher entry:

```text
CS:EIP=0170:0015B4E0 EAX=00000007 EBX=000000C8 ECX=000000F2 EDX=0000022C
ESI=001B8208 EDI=00000007 EBP=000064F0 ESP=000064EC EFLAGS=00000046
```

Relevant disassembly:

```text
0015B4E0  push ebx/ecx/edx/esi/edi/ebp
0015B4E6  mov eax,ebp
0015B4E8  cli
0015B4E9  mov ebp,edx
0015B4ED  lea 0(,edx,8),eax
0015B4F4  add edx,eax
0015B4F6  cmpl $0,0x1B71AC(,eax,4)
0015B4FE  je 0x15B609
0015B504  cmp $7,ebp
0015B509  mov $0x20,edx
0015B510  out al,(dx)
0015B513  in (dx),al
0015B516  je 0x15B61B
0015B51C  mov 0x1B73E8,ebx
```

Runtime path facts:

- At entry, `EAX=7`.
- `0x15B4E0` copies the interrupt number into `EBP`/`EDX`.
- It computes `EAX = irq * 9`, so for IRQ7 `EAX=0x3F`.
- First table/index source observed:
  `0x1B71AC + (0x3F * 4) = 0x001B72A8`.
- The table check was non-zero; execution did not jump to `0x15B609`.
- IRQ7-specific PIC poll executed via port `0x20`, command `0x0B`.
- PIC ISR read returned `AL=0x80`, so bit 7 was set and execution continued.
- Step trace reached `0x15B52F`; no indirect call/jump was reached inside the
  28-instruction trace window.

Conclusion:

- Entered `0x15B4E0` from IRQ thunk: YES.
- EAX at entry: `0x00000007`.
- First table/index source: `0x001B72A8`.
- First indirect call/jump address: not reached in this trace window.
- Resolved callback target: not yet captured.
- Callback target reached: NO proof.
- Returned from callback: NO proof.
- Returned from `0x15B4E0`: not captured in this lane.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward.
- Next single action: in one GDB lane, read `*(uint32_t*)0x001B72A8`, continue
  stepping from `0x15B52F` until the first indirect call/jump, and break on the
  resolved callback target if the table entry is executable.

## IRQ7 dispatcher slot and callback target

Focused non-invasive lane:

- Rebuilt the full image with `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`.
- Did not modify `DOOM.EXE`.
- Did not modify the generated thunk.
- Ran the existing DOS taxonomy launch with QEMU's GDB stub enabled.
- Followed the thunk-owned path into `0x15B4E0`, read the IRQ7 table slot and
  adjacent slots, then stepped from `0x15B52F` to the first indirect call.

Adjacent slot before, 9 dwords at `0x001B7284`:

```text
00000000 00000000 00000000 00000000
00000000 00000000 0015B330 0D090170
00800000
```

IRQ7 table slot, 9 dwords at `0x001B72A8`:

```text
00159820 00000002 00000000 00000000
00000080 00000000 0015B360 00000170
00000000
```

Adjacent slot after, 9 dwords at `0x001B72CC`:

```text
00000000 00000000 00000000 00000000
00000000 00000000 0015B390 00000170
00000000
```

Resolved dispatch:

```text
0015B584  mov 0x1B71AC(%eax),%ebx   ; EBX = 0x00159820
0015B58A  mov %ebp,%eax             ; EAX = 7
0015B59A  lss 0x8(%edx),%esp
0015B59E  call *%ebx
00159820  mov %eax,0x1B5374
```

Callback entry registers:

```text
CS:EIP=0170:00159820 EAX=00000007 EBX=00159820 ECX=0021D200 EDX=0021D1F0
ESI=001B8208 EDI=00000003 EBP=00000007 ESP=0021D1EC EFLAGS=00000206
```

Return evidence:

```text
0015B624  ret
0015B349  pop %gs
```

Conclusion:

- First indirect instruction: `0x0015B59E: call *%ebx`.
- Resolved target: `0x00159820`.
- Target reached: YES.
- Target CS:EIP: `0170:00159820`.
- Callback returns: YES, execution returned through the dispatcher cleanup.
- `0x15B4E0` returns: YES, execution reached thunk return address `0x15B349`.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward and packaged
  `DOOM.EXE` SHA256 remained
  `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.
- Next single action: step into callback `0x00159820` to determine whether it
  acknowledges the SB DSP interrupt, advances the DMX DMA/mixer state, or
  returns without feeding the SFX pipeline.

## IRQ7 callback `0x159820` first invocation

Focused non-invasive lane:

- Rebuilt the full image with `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`.
- Did not modify `DOOM.EXE`.
- Did not modify the generated thunk or dispatcher.
- Ran the existing DOS taxonomy launch with QEMU's GDB stub enabled.
- Followed the thunk-owned path through `0x15B59E: call *%ebx` into
  `0x00159820`.

Callback entry:

```text
CS:EIP=0170:00159820 EAX=00000007 EBX=00159820 ECX=0021D200 EDX=0021D1F0
ESI=001B8208 EDI=00000003 EBP=00000007 ESP=0021D1EC EFLAGS=00000206
```

Callback disassembly:

```text
00159820  mov %eax,0x1B5374
00159825  lea 0x0(%eax),%eax
0015982B  lea 0x0(%edx),%edx
0015982E  mov %ebx,%ebx
00159830  xor %eax,%eax
00159832  ret
```

Observed callback behavior:

- Writes state: `0x1B5374 = 7`.
- Clears return value: `EAX = 0`.
- Returns immediately.
- No `in` from SB ACK ports `0x22E` or `0x22F` in this callback body.
- No deeper call inside this callback body.
- No direct PIC EOI inside the callback body.
- PIC EOI still occurs later in the DOS/4GW dispatcher at
  `0x15B61D: out %al,(%dx)` with `EDX=0x20`, `AL=0x20`.

Conclusion:

- Callback `0x159820` reached: YES.
- Reads SB ACK port `0x22E` or `0x22F`: NO.
- Sends PIC EOI: NO inside callback; YES in dispatcher after callback.
- Calls deeper target(s): NO in this first callback invocation.
- Deeper target address(es): none from `0x159820`.
- Obvious DMX/SB state write(s): `0x1B5374 = 7`, then callback returns `0`.
- Callback returns: YES.
- `0x15B4E0` returns: YES, prior and current dispatcher trace reaches cleanup.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward and packaged
  `DOOM.EXE` SHA256 remained
  `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.
- Next single action: identify why the registered IRQ7 callback is the trivial
  status stub `0x159820` instead of the SB DSP ACK/mixer service; trace the
  registration path that writes the IRQ7 slot at `0x1B72A8`.

## IRQ7 callback slot registration watchpoint

Focused non-invasive lane:

- Rebuilt the full image with `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`.
- Did not modify `DOOM.EXE`.
- Did not modify the generated thunk, dispatcher, or Stage1.
- Ran the existing DOS taxonomy launch with QEMU's GDB stub enabled.
- Set a hardware watchpoint on `*(uint32_t*)0x001B72A8`.

Watchpoint results:

1. First write:

```text
CS:EIP=0170:0015BB1A
writer instruction just executed: 0015BB17  mov %edx,0xC(%esi)
EDX=00159820 ESI=001B729C EBP=00000007 EBX=00000002
slot: 00159820 00000000 00000000 00000000 00000000 00000000 0015B360 00000170 00000000
```

2. Second write:

```text
CS:EIP=0170:0015BB1A
writer instruction just executed: 0015BB17  mov %edx,0xC(%esi)
EDX=00159260 ESI=001B729C EBP=00000007 EBX=00000002 ECX=00000008
slot: 00159260 00000002 00000000 00000000 00000080 00000000 0015B360 00000170 00000000
```

Nearby registration code:

```text
0015BAFA  test $1,%bl
0015BAFD  sete %al
0015BB00  add %ebp,%esi
0015BB04  lea 0(,%esi,4),%esi
0015BB0D  add $0x1B71A0,%esi
0015BB13  pushf
0015BB14  pop %eax
0015BB15  cli
0015BB16  inc %ebx
0015BB17  mov %edx,0xC(%esi)
0015BB1C  mov %ebx,0x10(%esi)
```

Conclusion:

- Watchpoint on `0x1B72A8` fired: YES.
- Number of writes observed: 2.
- First writer CS:EIP: `0170:0015BB1A`, caused by previous instruction
  `0x15BB17`.
- First written value: `0x00159820`.
- Final written value: `0x00159260`.
- Full final IRQ7 slot:
  `00159260 00000002 00000000 00000000 00000080 00000000 0015B360 00000170 00000000`.
- Is `0x159820` default DOS/4GW stub: UNKNOWN, but it is the first registered
  placeholder/stub callback.
- Is a real DMX/SB callback ever written: YES, candidate `0x00159260` replaces
  `0x159820` before the lane completes.
- Candidate callback address: `0x00159260`.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward and packaged
  `DOOM.EXE` SHA256 remained
  `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.
- Next single action: break on the first IRQ7 dispatch after the second write
  and verify whether `0x00159260` is the callback actually used for the SB DMA
  completion IRQ; if it is, step `0x159260` for SB DSP ACK/mixer behavior.

## IRQ7 post-registration dispatch attempt

Focused non-invasive lane:

- Rebuilt the full image with `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`.
- Did not modify `DOOM.EXE`.
- Did not modify the generated thunk, dispatcher, or Stage1.
- Ran the existing DOS taxonomy launch with QEMU's GDB stub enabled.
- Watched `0x001B72A8` until the final write to `0x00159260`, then enabled
  breakpoints for the next thunk/dispatcher/callback path:
  `0x0015B330`, `0x0015B59E`, `0x00159260`, and `0x0015B349`.

Observed:

```text
SLOT_WRITE #1 VALUE=0x159820
SLOT_WRITE #2 VALUE=0x159260
FINAL_SLOT_159260
```

No subsequent breakpoint hit was observed at:

- `0x0015B330`
- `0x0015B59E`
- `0x00159260`

Conclusion:

- Final slot write to `0x159260` observed: YES.
- First IRQ after final write hit thunk `0x15B330`: NO in this observation
  window.
- Reached `call *%ebx` at `0x15B59E`: NO.
- EBX at call: not available.
- Callback `0x159260` reached: NO.
- Callback disassembly summary: not captured in this lane.
- Reads SB ACK `0x22E`/`0x22F`: not observed.
- Sends PIC EOI inside callback: not observed.
- Deeper calls: not observed.
- State writes: final slot state only:
  `00159260 00000002 00000000 00000000 00000080 00000000 0015B360 00000170 00000000`.
- Callback returns: NO proof.
- Dispatcher returns: NO post-final-dispatch proof.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward and packaged
  `DOOM.EXE` SHA256 remained
  `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.
- Next single action: trigger an actual in-game SFX event after final slot
  registration while GDB breakpoints on `0x15B330`, `0x15B59E`, and `0x159260`
  are armed; current startup lane observes registration but no post-final IRQ.

## Post-final-slot SFX trigger attempt

Focused non-invasive lane:

- Rebuilt the full image with `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`.
- Did not modify `DOOM.EXE`.
- Did not modify the generated thunk, dispatcher, or Stage1.
- Ran the existing DOS taxonomy launch with QEMU's GDB stub enabled.
- Watched `0x001B72A8` until final write to `0x00159260`, then armed
  breakpoints on `0x0015B330`, `0x0015B59E`, `0x00159260`, and `0x0015B349`.
- Sent HMP `sendkey ctrl` after launch delay as the attempted fire/SFX input.

Observed:

```text
SLOT_WRITE #1 VALUE=0x159820
SLOT_WRITE #2 VALUE=0x159260
FINAL_SLOT_159260
```

Post-final dispatcher calls were observed, but all were non-IRQ7:

```text
POST_FINAL_CALL_EBX_15B59E #1..#5
EAX=0 EBX=0015D210 EBP=0 ESP=0021D1F0
```

No breakpoint hit was observed at `0x00159260`.

Conclusion:

- Final slot write to `0x159260` observed: YES.
- Input/SFX trigger used: HMP `sendkey ctrl`.
- Post-trigger IRQ thunk `0x15B330` hit: not separately observed.
- Reached `0x15B59E`: YES, but only for dispatcher index `EBP=0`.
- EBX at call: `0x0015D210`, not `0x00159260`.
- Callback `0x159260` reached: NO.
- Callback disassembly/action summary: not captured in this lane.
- Reads SB ACK `0x22E`/`0x22F`: NO.
- Sends PIC EOI inside callback: NO.
- Deeper calls: NO proof for `0x159260`.
- Deeper targets: only unrelated dispatcher target `0x0015D210`.
- State writes: final IRQ7 slot remains
  `00159260 00000002 00000000 00000000 00000080 00000000 0015B360 00000170 00000000`.
- Callback returns: NO proof for `0x159260`.
- Dispatcher returns: not captured for IRQ7 path.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward and packaged
  `DOOM.EXE` SHA256 remained
  `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.
- Next single action: add a deterministic visual/input lane that reaches
  gameplay and fires after `HU_Init`, with a screenshot or menu/gameplay gate,
  then reuse the same GDB breakpoints; current `ctrl` send did not produce an
  IRQ7/SB callback dispatch.

## Post-final-slot menu/fire gated SFX attempt

Focused non-invasive lane:

- Rebuilt the full image with `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`.
- Did not modify `DOOM.EXE`.
- Did not modify the generated thunk, dispatcher, or Stage1.
- Ran the existing DOS taxonomy launch with QEMU's GDB stub enabled.
- Watched `0x001B72A8` until final write to `0x00159260`, then armed
  breakpoints on `0x0015B330`, `0x0015B59E`, `0x00159260`, and `0x0015B349`.
- After `HU_Init`/interactive gate, sent HMP input sequence:
  `esc`, `down`, `ret`, `down`, `ret`, `ctrl`.

Observed:

```text
SLOT_WRITE #1 VALUE=0x159820
SLOT_WRITE #2 VALUE=0x159260
FINAL_SLOT_159260
```

No post-final breakpoint hit was observed at:

- `0x0015B330`
- `0x0015B59E` with IRQ7 / `EBX=0x159260`
- `0x00159260`

Conclusion:

- Final slot write to `0x159260` observed: YES.
- Input/SFX trigger used: HMP `esc`, `down`, `ret`, `down`, `ret`, `ctrl`.
- Post-trigger IRQ thunk `0x15B330` hit: NO.
- Reached `0x15B59E`: NO for IRQ7/`0x159260`.
- EBX at call: not available for IRQ7.
- Callback `0x159260` reached: NO.
- Callback disassembly/action summary: not captured.
- Reads SB ACK `0x22E`/`0x22F`: NO.
- Sends PIC EOI inside callback: NO.
- Deeper calls: NO.
- Deeper targets: none observed.
- State writes: final IRQ7 slot remains
  `00159260 00000002 00000000 00000000 00000080 00000000 0015B360 00000170 00000000`.
- Callback returns: NO proof.
- Dispatcher returns: NO IRQ7 proof.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward and packaged
  `DOOM.EXE` SHA256 remained
  `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.
- Next single action: stop relying on blind HMP input; add a visual screenshot
  gate or breakpoint on first `S_StartSound`/`SFX_PlayPatch` equivalent, then
  arm IRQ7 breakpoints immediately after a proven SFX request.

## Post-final-slot SFX request/DMA gate

Focused non-invasive lane:

- Started from a clean `DOOM.EXE` and a temporary `sb16-sfx` full image.
- Did not modify `DOOM.EXE`.
- Did not modify the generated IRQ thunk, dispatcher, or Stage1.
- Reused the proven downstream sound/DMA gate at `0x00121721` because no
  already-proven local addresses for `S_StartSound`, `SFX_PlayPatch`, or
  `FX_PlayRaw` were available in the current notes/build logs.
- Watched `0x001B72A8` until the final write to `0x00159260`, then armed
  breakpoints on `0x00121721`, `0x0015B330`, `0x0015B59E`, and `0x00159260`.

Observed:

```text
SLOT_WRITE #1 VALUE=0x159820
SLOT_WRITE #2 VALUE=0x159260
FINAL_SLOT_159260
```

No post-final breakpoint hit was observed at:

- `0x00121721`
- `0x0015B330`
- `0x0015B59E`
- `0x00159260`

Conclusion:

- Final slot `0x159260` registered: YES.
- `S_StartSound` reached after final registration: UNKNOWN; no proven address
  available in this lane.
- `SFX_PlayPatch` reached after final registration: UNKNOWN; no proven address
  available in this lane.
- `FX_PlayRaw` reached after final registration: UNKNOWN; no proven address
  available in this lane.
- DMA start `0x21721` reached after final registration: NO.
- IRQ thunk `0x15B330` reached after DMA: NO.
- Dispatcher `0x15B59E` reached for IRQ7: NO.
- EBX at dispatcher call: not available.
- Callback `0x159260` reached: NO.
- SB ACK `0x22E`/`0x22F`: NO.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward and packaged
  `DOOM.EXE` SHA256 remained
  `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.
- Next single action: locate `S_StartSound`, `SFX_PlayPatch`, or `FX_PlayRaw`
  statically/by call graph before more HMP input attempts; the downstream
  `0x21721`/IRQ gates prove the current lane did not issue a post-final SB DMA
  playback request.

## Static SFX path candidates and post-final breakpoint lane

Static reverse used the extracted payload mapping `file_off = VA + 0xCE00`.
Runtime GDB addresses use load base `0x12C200`, recovered from the live
`S_START` string/reference area.

Candidate path:

- `S_StartSound` wrapper: VA `0x4F370`, runtime `0x17B570`.
- `S_StartSoundAtVolume` equivalent: VA `0x4F130`, runtime `0x17B330`.
- `I_StartSound` / DMX adapter: VA `0x2B2B0`, runtime `0x1574B0`.
- `SFX_PlayPatch` equivalent: VA `0x25680`, runtime `0x151880`.
- `FX_PlayRaw`-like lower play/register path: VA `0x25AE0`, runtime
  `0x151CE0`.
- DMA arm / SB16 backend function: VA `0x214E0`, runtime `0x14D6E0`.
- SB16 DMA command site: VA `0x21721`, runtime `0x14D921`.

Evidence:

- `0x4F370` loads `[0x75CA0]` as default volume and calls `0x4F130`.
- `0x4F130` validates SFX id/channel state and calls `0x2B2B0` at `0x4F34B`.
- `0x2B2B0` forwards sample pointer/params and calls `0x25680`.
- `0x25680` dispatches by patch header word and calls `0x25AE0` for type
  `1/2` patches.
- `0x214E0` contains the SB16 backend setup/DMA arm path; `0x21721` is the
  DSP command `0xC6` site.

Focused GDB lane after final callback registration:

- Final callback `0x159260` registered: YES.
- `S_StartSound` wrapper `0x17B570` hit after final registration: NO.
- `S_StartSoundAtVolume` `0x17B330` hit after final registration: NO.
- `I_StartSound` adapter `0x1574B0` hit after final registration: NO.
- `SFX_PlayPatch` equivalent `0x151880` hit after final registration: NO.
- `FX_PlayRaw` equivalent `0x151CE0` hit after final registration: NO.
- DMA arm `0x14D6E0` hit after final registration: NO.
- DMA command `0x14D921` hit after final registration: NO.
- IRQ gate skipped by evidence: no post-final DMA/SFX request occurred.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward with default
  `pcspeaker-sfx`.
- Next single action: trigger or force a known DOOM-side SFX callsite after
  final registration; the current startup lane installs SB callback but never
  requests post-final playback.

## Forced SFX call attempt after final registration

Focused GDB lane:

- Started from a clean `DOOM.EXE` and temporary `sb16-sfx` full image.
- Watched `0x001B72A8` until final write to `0x00159260`.
- Armed breakpoints for the candidate SFX path and IRQ gate.
- Attempted controlled GDB call after final registration:
  `eax=0`, `edx=1`, `ebx=127`, target `0x0017B330`
  (`S_StartSoundAtVolume` candidate).

Observed:

```text
SLOT_WRITE #1 VALUE=0x159820
SLOT_WRITE #2 VALUE=0x159260
FINAL_SLOT_159260
FORCE_S_START_AT_VOLUME eax=0 edx=1 ebx=127 target=0x17b330
```

GDB aborted evaluation because execution stopped inside the function called
from GDB. DOOM then returned to shell with DOS/4GW exception `06h` at
`170:00000220B`, so this forced-call method is not a safe proof lane.

Conclusion:

- Final callback `0x159260` registered: YES.
- Trigger method used: controlled temporary GDB `call` to `0x17B330`.
- `S_StartSound` `0x17B570` hit: NO.
- `S_StartSoundAtVolume` `0x17B330` hit: attempted by GDB call, but breakpoint
  chain did not produce a safe trace.
- `I_StartSound` adapter `0x1574B0` hit: NO.
- `SFX_PlayPatch` `0x151880` hit: NO.
- `FX_PlayRaw` `0x151CE0` hit: NO.
- DMA arm `0x14D6E0` hit: NO.
- DMA cmd `0x14D921` hit: NO.
- IRQ gate: not reopened; no DMA proof.
- Exception/crash: YES, DOS/4GW exception `06h`.
- Runtime stable: FAIL, app returned to shell.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward with default
  `pcspeaker-sfx`.
- Next single action: use a real in-code callsite instead of GDB `call`; best
  first targets are menu/UI callsites such as VA `0x4CCA7` / runtime
  `0x178EA7` or VA `0x4D242` / runtime `0x179442`, both setting `edx=0x52`
  and `eax=0` before calling `S_StartSound`.

## Real callsite menu/UI trigger attempt

Focused GDB lane:

- Started from a clean `DOOM.EXE` and temporary `sb16-sfx` full image.
- Watched `0x001B72A8` until final write to `0x00159260`.
- After final registration, armed breakpoints on:
  `0x178EA7`, `0x17B570`, `0x17B330`, `0x1574B0`, `0x151880`,
  `0x151CE0`, `0x14D6E0`, `0x14D921`, `0x15B330`, `0x15B59E`,
  `0x159260`.
- Sent a minimal menu-oriented HMP sequence after the final slot was observed:
  `esc`, `down`, `ret`.

Observed:

```text
SLOT_WRITE #1 VALUE=0x159820
SLOT_WRITE #2 VALUE=0x159260
FINAL_SLOT_159260
```

No post-final breakpoint hit was observed.

Conclusion:

- Final callback `0x159260` registered: YES.
- Trigger method used: HMP `esc`, `down`, `ret`.
- Real callsite `0x178EA7` hit: NO.
- Callsite reaches `S_StartSound`/`S_StartSoundAtVolume`: NO.
- `S_StartSound` `0x17B570` hit: NO.
- `S_StartSoundAtVolume` `0x17B330` hit: NO.
- `I_StartSound` `0x1574B0` hit: NO.
- `SFX_PlayPatch` `0x151880` hit: NO.
- `FX_PlayRaw` `0x151CE0` hit: NO.
- DMA arm `0x14D6E0` hit: NO.
- DMA cmd `0x14D921` hit: NO.
- IRQ gate: not reopened; no DMA proof.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward with default
  `pcspeaker-sfx`.
- Next single action: stop relying on HMP menu navigation in this SB16 lane;
  either identify the exact interactive/menu state visually, or use a
  non-invasive breakpoint on a known menu input handler before the
  `0x4CCA7`/`0x4D242` callsites.

## Menu handler/ticker probe

Focused GDB lane:

- Started from a clean `DOOM.EXE` and temporary `sb16-sfx` full image.
- Watched `0x001B72A8` until final write to `0x00159260`.
- After final registration, armed breakpoints on:
  `0x179390` (`0x4D190` menu/ticker candidate),
  `0x178EA7`, `0x179442`, `0x17B570`, `0x17B330`, `0x1574B0`,
  `0x151880`, `0x151CE0`, `0x14D6E0`, `0x14D921`.
- Sent HMP `esc`, `down`, `ret`.

Observed:

```text
SLOT_WRITE #1 VALUE=0x159820
SLOT_WRITE #2 VALUE=0x159260
FINAL_SLOT_159260
```

No post-final breakpoint hit was observed, including the menu/ticker candidate.

Conclusion:

- Final callback `0x159260` registered: YES.
- Input/menu handler identified: PARTIAL; static candidate `0x179390`
  encloses real callsite `0x179442`.
- Handler address: `0x179390`.
- HMP input reaches handler: NO.
- Real UI/menu callsite `0x178EA7` hit: NO.
- Other callsite hit: NO (`0x179442` not hit).
- `S_StartSound` `0x17B570` hit: NO.
- `S_StartSoundAtVolume` `0x17B330` hit: NO.
- `I_StartSound` `0x1574B0` hit: NO.
- `SFX_PlayPatch` `0x151880` hit: NO.
- `FX_PlayRaw` `0x151CE0` hit: NO.
- DMA `0x14D921` hit: NO.
- Exact reason for no `S_StartSound`: HMP input did not reach the identified
  menu/ticker handler in the SB16 lane.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward with default
  `pcspeaker-sfx`.
- Next single action: obtain a visual/manual gate for the real interactive
  state, or trace the keyboard event ingestion layer below the menu handler;
  current HMP sendkey is not a proven path to DOOM menu input here.

## HMP keyboard delivery probe

Focused GDB lane:

- Started from a clean `DOOM.EXE` and temporary `sb16-sfx` full image.
- Watched `0x001B72A8` until final write to `0x00159260`.
- After final registration, armed breakpoints on:
  DOS/4GW dispatcher `0x15B4E0` filtered for `EAX=1` / IRQ1,
  menu/input handler candidate `0x179390`,
  callsites `0x178EA7`/`0x179442`,
  `S_StartSound` `0x17B570`, and `S_StartSoundAtVolume` `0x17B330`.
- Sent HMP `esc`, `down`, `ret`.

Observed:

```text
SLOT_WRITE #1 VALUE=0x159820
SLOT_WRITE #2 VALUE=0x159260
FINAL_SLOT_159260
```

No post-final breakpoint hit was observed.

Conclusion:

- Final callback `0x159260` registered: YES.
- HMP reaches keyboard IRQ/BIOS path: NO proof; no IRQ1 hit at DOS/4GW
  dispatcher `0x15B4E0` with `EAX=1`.
- HMP reaches DOOM input handler `0x179390`: NO.
- Where input is lost: before the proven DOS/4GW protected-mode IRQ dispatcher
  path, or outside the observed keyboard path; current HMP sendkey is not a
  valid DOOM input trigger in this SB16 lane.
- Visual/manual gate needed: YES.
- `0x178EA7` hit: NO.
- `0x179442` hit: NO.
- `S_StartSound` hit: NO.
- `S_StartSoundAtVolume` hit: NO.
- Exception/crash: NO.
- Runtime stable: PASS.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward with default
  `pcspeaker-sfx`.
- Next single action: use a visual/manual interactive gate or a lower-level
  keyboard injection path proven to raise IRQ1 before continuing SFX tracing.

## Visual/manual input gate attempt

Focused visual GDB lane:

- Started from a clean `DOOM.EXE` and temporary `sb16-sfx` full image.
- Launched visual QEMU with gdbstub.
- Used real keyboard input in the QEMU window to launch DOOM from the shell.
- Watched `0x001B72A8` until final write to `0x00159260`.
- After final registration, breakpoints were armed on:
  `0x179390`, `0x178EA7`, `0x179442`, `0x17B570`, `0x17B330`,
  `0x1574B0`, `0x151880`, `0x151CE0`, `0x14D921`, `0x15B330`,
  `0x15B59E`, `0x159260`.

Observed:

```text
SLOT_WRITE #1 VALUE=0x159820
SLOT_WRITE #2 VALUE=0x159260
FINAL_SLOT_159260
```

No post-final breakpoint hit was observed before DOOM returned to the shell
with DOS/4GW exception `06h` at `170:0000693C`.

Conclusion:

- Final callback `0x159260` registered: YES.
- Manual input reaches DOOM handler `0x179390`: NO.
- `0x178EA7` hit: NO.
- `0x179442` hit: NO.
- `S_StartSound` `0x17B570` hit: NO.
- `S_StartSoundAtVolume` `0x17B330` hit: NO.
- `I_StartSound` `0x1574B0` hit: NO.
- `SFX_PlayPatch` `0x151880` hit: NO.
- `FX_PlayRaw` `0x151CE0` hit: NO.
- DMA cmd `0x14D921` hit: NO.
- IRQ gate: not reopened; no DMA proof.
- Exception/crash: YES, DOS/4GW exception `06h` at `170:0000693C`.
- Runtime stable: FAIL, app returned to shell.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward with default
  `pcspeaker-sfx`.
- Next single action: investigate the new post-final crash site
  `0170:0000693C`; current visual input proves shell keyboard delivery works,
  but DOOM crashes before reaching menu/input/SFX handlers in the SB16 lane.

## Crash triage at `0170:0000693C`

Focused crash lane:

- Started from a clean `DOOM.EXE` and temporary `sb16-sfx` full image.
- Used visual QEMU with gdbstub.
- Used only final IRQ7 slot watchpoint plus a breakpoint at linear `0x693C`.
- No audio/input/SFX breakpoints were armed.

Observed:

```text
SLOT_WRITE #1 VALUE=0x159820
SLOT_WRITE #2 VALUE=0x159260
FINAL_SLOT_159260
HIT_CRASH_SITE_693C
CS:EIP 0170:0000693C
EFLAGS 0x86
EAX 0x0000EEFF EBX 0x413006FE ECX 0x00000C40 EDX 0x00000170
ESI 0x00006174 EDI 0x00000102 EBP 0x0000616A ESP 0x0000612D
SS 0x00C8
```

Bytes at `0x6900`:

```text
30 41 f3 06 28 69 00 00 58 fe ff 06 0a 20 ae 18
aa 0b ff 06 30 51 fe 06 30 b9 fe 06 00 b9 fe 06
30 b9 fe 06 2c 00 00 00 e4 ff ff 06 44 bb fe 06
30 41 f3 06 30 41 fd 06 00 00 0a 00 ff ff ff ff
ac 86 0f 00 00 00 0a 00 b9 f8 ff 06 00 00 ff 06
10 00 00 00 ac 86 0f 00 a8 69 00 00 c6 86 00 00
```

Disassembly around `0x6920` is nonsensical data-as-code and reaches invalid
opcode at `0x693C`.

DOS/4GW printed `SS base=0x116330`; using `SS:ESP = 0x116330 + 0x612D =
0x11C45D`, the exception frame area includes:

```text
0x11c450: 00693c00 00017000 01008600 ...
0x11c4d0: ... 15b56f00 10017000 ...
```

`0x15B56F` is inside the DOS/4GW IRQ dispatcher path:

```text
0x15b566 lea 0x0(,%ebp,8),%eax
0x15b56d sti
0x15b56e cld
0x15b56f add %ebp,%eax
...
0x15b59e call *%ebx
...
0x15b61d out %al,(%dx)
0x15b624 ret
```

Conclusion:

- Exception reproduced: YES.
- CS:EIP: `0170:0000693C`.
- Bytes at `0x693C`: `ff ff ff ff ...`, invalid opcode/data.
- Stack dump: captured with correct `SS` base-derived linear address.
- Mapping/classification: not DOOM LE image code; `0x693C` is low flat
  DOS/4GW/runtime/data area being executed as code.
- Likely caller/return source: exception frame/stack points back near DOS/4GW
  IRQ dispatcher `0x15B56F`, not a DOOM SFX function.
- Happens with minimal breakpoints: YES.
- Happens without audio/input breakpoints: YES; only final slot watchpoint and
  crash-site breakpoint were armed.
- `DOOM.EXE` modified: NO.
- Image restored/clean: YES; `make build-full` PASS afterward with default
  `pcspeaker-sfx`.
- Next single action: trace the dispatcher path around `0x15B56F` / `0x15B59E`
  at the post-final crash window and identify which interrupt/callback returns
  or jumps into low data at `0x693C`.

## Dispatcher trace to `0170:0000693C`

Focused QEMU/GDB dispatcher lane:

- Started from clean `DOOM.EXE` hash
  `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.
- Used terminal/visual QEMU input, not HMP input.
- Armed only dispatcher/callback/crash breakpoints around `0x15B4E0`,
  `0x15B56F`, `0x15B59E`, `0x159820`, `0x159260`, and `0x693C`.
- Did not patch `DOOM.EXE` or the DOS/4GW thunk.

Observed:

```text
DISP_COUNT=24
CALL_COUNT=23
CB_159820_COUNT=3
CB_159260_COUNT=0
CRASH_693C
CS:EIP 0170:0000693C
EAX=0000EEFF EBX=00000080 ECX=00000C40 EDX=00000170
ESI=00006174 EDI=00000102 EBP=0000616A ESP=0000612C
EFLAGS=00000086 SS=00C8
```

Last repeated dispatcher path before the crash was event/slot `0`, not IRQ7:

```text
0x15B59E: call *%ebx
EAX=0
EBX=0x0015D210
EBP=0
ESP=0x0021D1F0
```

Slot `0` at `0x1B71AC`:

```text
0015D210 00000001 00000000 00000000
000000B8 00000000 0015B210 00000170
00000000
```

Stack frame before the last observed slot-0 indirect call:

```text
0x21D1F0: 0000620C 000000C8 0021D1F0 00000178 ...
```

IRQ/event observations:

- Slot `4` reached placeholder callback `0x159820` once.
- IRQ7/slot `7` reached placeholder callback `0x159820` twice before final
  SB callback registration.
- Final SB callback `0x159260` was not reached in this crash trace.
- The crash therefore does not come from the final SB callback body.

Conclusion:

- Crash reproduced: YES.
- Last dispatcher event before crash: event/slot `0`.
- Slot address used: `0x1B71AC`.
- Indirect branch/call: `0x15B59E: call *%ebx`.
- EBX target at indirect call: `0x15D210`.
- First bad control transfer: not directly captured yet; execution later lands
  at low flat `0x693C`, which is data/garbage bytes (`ff ff ff ff ...`).
- Source of `0x693C`: likely corrupted/stale DOS/4GW event-0 stack/control
  frame or return path, not the IRQ7 dispatcher table entry.
- Likely cause: event-0 callback target `0x15D210` or its stack restore/return
  path around `0x15B56F`/`0x15B59E` sends control into low runtime data.
- `DOOM.EXE` modified: NO.
- Image clean: YES; default image was rebuilt afterward with `make build-full`
  PASS.
- Next single action: step into event-0 target `0x15D210` and its return path;
  do not continue SFX/IRQ7 tracing until this dispatcher crash is closed.

## Event-0 callback `0x15D210` step

Focused QEMU/GDB event-0 lane:

- Broke at `0x15B59E`, `0x15D210`, `0x15B5A0`, `0x15B624`,
  `0x15B206`, and `0x693C`.
- Filtered dispatcher calls for slot `0` where `EBP=0` and `EBX=0x15D210`.
- Did not patch `DOOM.EXE` or DOS/4GW code.

Slot-0 call evidence:

```text
CALL0 #1 eip=15b59e eax=0 ebx=15d210 ebp=0 esp=21d1f0 ss=178
SLOT0:
0015D210 00000001 00000000 00000000
000000B8 00000000 0015B210 00000170
00000000
```

`0x15D210` entry:

```text
CB0_ENTRY #1 eip=15d210 eax=0 ebx=15d210 ecx=21d200
edx=21d1f0 ebp=0 esp=21d1ec ss=178
```

Disassembly summary:

- `0x15D210` is a DOS/4GW timer/task scheduler callback.
- It increments tick/state at `0x1B7820`.
- It scans scheduled task entries under `0x1B76A0`.
- If a task is due, it calls the task function through `0x15D266: call *(%edx)`.
- It restores registers and exits through `0x15D2FC: ret`.

Observed normal return:

```text
RET_FROM_CB0 #1 eip=15b5a0 eax=0 ebx=15d210 ebp=0 esp=21d1f0 ss=178
DISPATCHER_RET #1 eip=15b624 eax=0 ebx=1 ebp=6228 esp=6224 ss=c8
RET_STACK_SSBASE:
0015B206 00000020 00000000 00000000 ...
RETURNED_TO_15B206 eip=15b206 esp=6228 ss=c8
```

The same normal pattern repeated thousands of times:

- `0x15D210` reached: YES.
- `0x15D210` returned normally to `0x15B5A0`: YES.
- Dispatcher `ret` at `0x15B624` returned to `0x15B206`: YES.
- Direct return target `0x693C`: NO in this stepped lane.

Task-call evidence inside `0x15D210`:

```text
0x15D266: call *(%edx)

task entry 0x1B76C0 -> target 0x15DD20
task entry 0x1B76A0 -> target 0x162360
```

Conclusion:

- `0x15D210` itself is not a direct bad jump to `0x693C`.
- Slot `0` contents are not obviously stale; the slot points to a live
  DOS/4GW scheduler callback.
- The bad control flow is likely below the scheduler task callbacks or in the
  outer wrapper/exception-return state after `0x15B206`, not in the immediate
  `0x15D210 -> ret -> 0x15B5A0 -> 0x15B624` path.
- `DOOM.EXE` modified: NO.
- Image clean: YES.
- Next single action: step the two scheduled task targets reached from
  `0x15D266`, especially `0x162360`, and capture whether either corrupts the
  return/exception frame that later produces `0170:0000693C`.

## Scheduled task `0x162360` step

Focused QEMU/GDB task lane:

- Broke at scheduler indirect call `0x15D266`, task target `0x162360`,
  scheduler return site `0x15D268`, wrapper return site `0x15B206`, and
  crash site `0x693C`.
- Filtered `0x15D266` for task entries where `*(EDX) == 0x162360`.
- Did not patch `DOOM.EXE` or DOS/4GW code.

Scheduler call evidence:

```text
SCHED_CALL_162360 edx=1b76a0 esp=21d1d4 ebp=0 eax=4 ebx=1 ecx=4 ss=178
TASK_ENTRY:
00162360 00000023 00000004 00000000
00000000 00000004 00000000 00010001
```

Stack before task:

```text
0x21d1d4: 00000000 00000003 00007d65 0021d1f0
0x21d1e4: 0021d200 0015d210 0015b5a0 0000620c
0x21d1f4: 000000c8 0021d1f0 00000178 00000000
```

Disassembly summary:

```text
0x162360: push edx
0x162361: mov 0x1b813c,edx
0x162367: inc edx
0x162368: xor eax,eax
0x16236a: mov edx,0x1b813c
0x162370: pop edx
0x162371: ret
```

Return evidence:

```text
RET_FROM_TASK_162360 #1 eip=15d268 eax=0 ebx=1 ecx=4 edx=1b76a0
ebp=0 esp=21d1d4 ss=178

RETURNED_15B206 #4 eip=15b206 eax=0 ebx=0 ebp=6228 esp=6228 ss=c8
hit162=1 ret162=1
```

Conclusion:

- `0x162360` reached from `0x15D266`: YES.
- It returns normally to `0x15D268`: YES.
- It only increments `0x1B813C` and clears `EAX`.
- Stack after `0x162360`: unchanged for the scheduler frame.
- Scheduler/task table after return: unchanged for the task pointer/metadata.
- Reaches `0x15B206` after return: YES.
- `0x693C` reached after this first task invocation: NO.
- Likely cause: not `0x162360`; next target is the other scheduler task
  `0x15DD20`, or the outer wrapper frame after many scheduler ticks.
- `DOOM.EXE` modified: NO.
- Image clean: YES.

## Scheduled task `0x15DD20` step

Focused QEMU/GDB task lane:

- Broke at scheduler indirect call `0x15D266`, task target `0x15DD20`,
  scheduler return site `0x15D268`, wrapper return site `0x15B206`, and
  crash site `0x693C`.
- Filtered `0x15D266` for task entries where `*(EDX) == 0x15DD20`.
- Did not patch `DOOM.EXE` or DOS/4GW code.

Scheduler call evidence:

```text
SCHED_CALL_15DD20 edx=1b76c0 esp=21d1d4 ebp=0 eax=1 ebx=0 ecx=0 ss=178
TASK_ENTRY:
0015DD20 0000008C 00000001 00000000
00000000 00000001 00000001 00010001
```

Stack before task:

```text
0x21d1d4: 00000000 00000003 00000001 0021d1f0
0x21d1e4: 0021d200 0015d210 0015b5a0 0000620c
0x21d1f4: 000000c8 0021d1f0 00000178 00000000
```

Disassembly summary:

- `0x15DD20` is the DOS/4GW/DMX PC speaker/PIT service callback.
- It reads/writes PC speaker gate port `0x61`.
- It programs PIT channel 2 through ports `0x43` and `0x42`.
- It updates state at `0x1B785C`, `0x1B7870`, and `0x1B7874`.
- It returns through normal `ret`, with no indirect call in the first invoked
  body.

Return evidence:

```text
RET_FROM_TASK_15DD20 #1 eip=15d268 eax=1 ebx=0 ecx=0 edx=1b76c0
ebp=0 esp=21d1d4 ss=178

RETURNED_15B206 #1 eip=15b206 eax=0 ebx=1 ebp=6228 esp=6228 ss=c8
hit15=1 ret15=1
```

Conclusion:

- `0x15DD20` reached from `0x15D266`: YES.
- It returns normally to `0x15D268`: YES.
- Stack after `0x15DD20`: unchanged for the scheduler frame.
- Scheduler/task table after return: unchanged for the task pointer/metadata.
- Reaches `0x15B206` after return: YES.
- `0x693C` reached after this first task invocation: NO.
- Likely cause: not the first invocation of `0x15DD20`; both observed
  scheduler tasks are clean in isolation. The remaining suspect is the outer
  DOS/4GW interrupt/exception return frame after repeated event-0 ticks, or a
  later specific scheduler iteration rather than either task's basic body.
- `DOOM.EXE` modified: NO.
- Image clean: YES.
