# Physical network IRQ ownership across DOS VMs

## Problem

Each V86 VM receives a private copy of the first megabyte of DOS memory. That
copy includes its IVT and resident packet-driver state. A physical NIC has one
receive ring and one interrupt line, so reflecting its IRQ into whichever VM
happens to be running can make a cloned packet-driver ISR consume a frame into
the wrong VM's private queue. The system VM's TCP stack then never sees the
frame, even though the host-side capture shows a valid reply.

The V86 scheduler's physical-device bridge routes keyboard IRQ1 and mouse
IRQ12. Other hardware IRQs follow the currently running VM's normal Jemm
reflection path. The background `/B` DPMI worker can yield into another V86 VM
while its protected-mode client remains active, so the network IRQ needs an
explicit owner across those VM switches.

## Ownership rule

The desktop's system VM (VM0) owns the physical packet driver and its receive
queue. Startup calls `startup_driver_services` before
`startup_vm_manager` in [shell.asm](../../src/com/shell.asm), so the initial
`NETSTART` can discover the IRQ bitmap before CVSESSION exists. It scans PCI
network-class devices and stores their assigned IRQs in the resident ICMPD
TSR. If CVSESSION is already loaded, NETSTART registers the bitmap directly.
Otherwise `VMSTART` reads the stored bitmap through ICMPD's private
`INT 61h AX=FE08h` query after loading CVSESSION, then registers it before any
VM can be forked. The service is global to the session and accepts only VM0's
registration. It rejects IRQ0–2 and IRQ12, reserved for the host timer/cascade
and the physical mouse broker. The registration bitmap is dynamic; no NE2000
IRQ number is compiled into the VMM.

When VM0 leaves at a safe scheduler boundary, CVSESSION saves the exact
registered bits from the physical PIC masks and masks only those bits. It
leaves the device request pending and sends no EOI. After another VM's virtual
PIC state is restored, CVSESSION reapplies the mask. When VM0 is selected,
CVSESSION restores the saved bits, so the NIC's pending request reaches VM0's
own packet-driver ISR. Existing physical-ISR and early-EOI guards prevent a
switch while a device handler still owns an in-service interrupt.

The PIC's request/mask behavior supports this choice: the Intel ICH7 datasheet
describes IRR tracking for edge and level inputs and the IMR masking requests;
QEMU's 8259 model similarly selects pending IRQs from `IRR & ~IMR`. QEMU's
NE2000 asserts its IRQ while enabled status bits remain set, and stores
received frames in its on-card ring. Masking while another VM runs therefore
keeps the request pending for the owner instead of acknowledging it in the
wrong VM. Sources: [Intel ICH7 datasheet](https://www.intel.com/content/dam/doc/datasheet/i-o-controller-hub-7-datasheet.pdf),
[QEMU 8259 model](https://github.com/qemu/qemu/blob/master/hw/intc/i8259.c),
[QEMU NE2000 model](https://qemu.googlesource.com/qemu/+/91d0d16b44c93fa82cf76ae12990ce3aa96096c9/hw/net/ne2000.c).

The standard Packet Driver API is not used to infer a hardware IRQ. Its
AH=10 `get_parameters` result has an `int_num` for a separate post-EOI
processing hook, not the NIC's hardware IRQ. The configured PCI IRQ line is
the source of truth for this supported boot path. See the
[PC/TCP Packet Driver Specification 1.09](https://groups.google.com/g/comp.os.msdos.programmer/c/oAgIvwIOVU8).

## Timing and limits

While a non-owner VM runs, packet delivery waits until VM0 runs again. A
regular VM slice is two PIT ticks (about 110 ms at the usual 18.2 Hz rate);
the `/B` worker's idle HLT path can yield sooner. The NIC ring absorbs frames
during that interval, but sustained traffic or long periods without VM0 can
still overrun it. This design keeps one physical packet-driver owner and does
not virtualize a NIC for every DOS VM. A registered IRQ shared with an
unrelated physical device also delays that device while another VM runs.

The source path and ABI change are implemented; full runtime validation of
repeated browser networking while another VM runs is pending.
