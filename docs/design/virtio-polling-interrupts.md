# Virtio polling and shared PCI interrupts

The native GPU and web-worker RNG use polling and set the virtqueue
`VIRTQ_AVAIL_F_NO_INTERRUPT` flag. That flag is an optimization, not a transport
interrupt mask. GPU configuration changes can also assert INTx independently
of used-ring notifications. The GPU maps its ISR capability but previously
never read it. In the Linux profile, GPU and NE2000 share IRQ10; an unhandled
GPU interrupt can consequently enter the unrelated packet driver's handler.

The GPU and RNG polling drivers now disable their own PCI INTx delivery while active,
preserving the device's original PCI command word for teardown. This masks
only that PCI function, not the shared PIC line needed by the NIC. Used-ring
polling continues normally. Both clear the transport ISR status while polling;
monitor hotplug requires separate configuration handling.

Sources reviewed before this change:

- [Virtio 1.2, available-ring suppression and PCI ISR capability](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html):
  notification suppression is advisory; reading ISR status deasserts INTx,
  and standard PCI masking applies to the function's interrupt.
- [Linux PCI command definitions](https://github.com/torvalds/linux/blob/master/include/uapi/linux/pci_regs.h):
  `PCI_COMMAND_INTX_DISABLE` is bit 10, separate from memory/I/O decode and
  bus-master enable.

The browser integration snapshot showed physical IRQ10 in service with IRQ12
pending. It does not identify the GPU as the source: this correction follows
the polling-driver contract, while NIC ownership and PIC forwarding are
investigated separately. Runtime validation is pending.
