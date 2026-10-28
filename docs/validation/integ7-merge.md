# Integration 7 security merge

Directive: `docs/directives/integ7-merge.md`; binding lead decisions U6-1..U6-6
and K8-1..K8-6 in `docs/directives/sec-lead-decisions.md`.

## Research and merge decisions

- [RFC 8018 section 5.2](https://www.rfc-editor.org/rfc/rfc8018.html#section-5.2)
  defines PBKDF2's iterated PRF and XOR construction. Both incoming implementations
  compute the same ACCOUNT verifier. Keep the existing sec-k08 library
  `src/kernel/lib/pbkdf2.c`, remove the duplicate self-test implementation, and
  retain H14's ACCOUNT/hashlib comparison against the retained library.
  U6-6 sets the single check's bound to 8 s; K8-4 makes a median above 1,000 ms
  a warning rather than a failed CPU qualification. The full table totals 599 s.
- Microsoft's [Run and RunOnce registry keys](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)
  documents logon startup and Safe Mode exceptions. Preserve the first-session,
  successful StartUp snapshot and explicit CD Player launch/focus/close predicates
  in normal boots, including `acc-accounts`' autologon check; leave them out of
  shared Safe Mode templates. The read-only Windows 98 Resource Kit
  `wrkc27_2.htm` (Starting Dr. Watson) also describes Startup-folder shortcuts.
- QEMU's upstream [run-state schema](https://raw.githubusercontent.com/qemu/qemu/master/qapi/run-state.json)
  defines host `system_reset` and the RESET event's guest/reason fields. Preserve
  the bounded `power_cut` action and its existing replay tests alongside the
  DOS games' mouse actions. This is simulated abrupt reset evidence, not proof
  of physical disk power-loss behavior.
- Microsoft's [WM_SYSCOMMAND](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-syscommand)
  documents SC_SCREENSAVE and policy-enforced password protection. Preserve
  sec-user-06's saver/secure-attention records and latency checks alongside
  sec-k08's firmware-resume tracking in LOGON.
- U6-4 explicitly keeps `/Programs/sectest` in the image while hiding it from
  Start and Add/Remove Programs. Filter that exact built-in name at the native
  menu presentation/shortcut seeding and Add/Remove inventory paths; retain
  the native launch inventory so direct Run launches still use
  the ordinary executable path. No public ABI or SDK interface changes.

Host verification and parent-by-parent removal justifications are recorded in
`build/codex/integ7-report.md`. Runtime validation and merge completion belong
to the lead; no image, QEMU or hardware evidence is claimed here.
