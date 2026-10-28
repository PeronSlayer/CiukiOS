# sec-firstboot: the first-boot account promotion must succeed on a slow machine

- **Written:** 2026-10-28 19:01 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `xhigh`. Base: main 4fadbd448 or later.
- **Evidence:** the lead's `qemu-t23` boots (TCG Pentium III, icount), serial logs in `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/sec-firstboot/`. On the default KVM profile the same image reaches the desktop by autologon.

## Facts

1. **The kernel generator is seeded late.** On `qemu-t23` it is seeded at about 13.4 s (`[entropy] ... initialised_ms=13466`), after ACCOUNT opens. Until then `getentropy` returns EAGAIN (`docs/design/kernel-entropy.md`).
   - **Original code:** the first promotion attempt (`acct_firstboot_promote` → `acct_service_bootstrap` → `acct_boot_identity`) returned -11 and was never retried. The store stayed unprovisioned and LOGON showed "Logon is not available".
2. **The lead tried two quick fixes. Neither is sufficient:**
   - **f09406018:** retrying `acct_firstboot_promote` in place on the open service. The retry returned -5.
   - **4cba53067 (current main):** on EAGAIN, `acct_service_close` and a full reopen 100 ms later. With this:
     - one boot (`serial-a0b9ed23.log`) promoted the seed and reached the session;
     - the next boot of the same image (`serial-e21a78b0.log`) logged `[owner] store open ... fresh=1` and then `[accounts] open refused; every request is answered with EIO: -5` forever.
   - Repeated -11 results are not logged by 4cba53067. The failing attempt after the OWNER store opened therefore most probably ended on a second EAGAIN, after RESERVE. Confirm this.
3. **Other EAGAIN sources after RESERVE are timing-dependent.**
   - `src/kernel/fs/owner_vfs.c` `owner_vfs_tree_digest` returns EAGAIN when the volume's mutation sequence or generation changes during the walk. Other services write to C: during a slow first boot.
   - `src/kernel/fs/owner.c` (candidate/compact, generation) and the checkpoint code have their own EAGAIN returns.

## Required behaviour

The first boot completes the promotion on every machine of the hardware baseline (`docs/design/hardware-baseline.md`), whatever the order of entropy seeding, OWNER store opening and concurrent volume writes. Windows XP's first boot never fails out-of-box setup because a disk was busy.

1. **Transient refusals are retried.** Every EAGAIN that the promotion can meet (entropy, tree digest, OWNER candidate/generation) is retried, at the step that produced it or by a resume that is proven equivalent.
   - The retry is bounded only by progress, not by a deadline: the system has nothing to show without accounts.
   - It never creates a second machine identity.
   - It never reuses a retired generation.
2. **A promotion interrupted within one session resumes in that session.** It must succeed as it does after a reboot (H15 "cut after OWNER RESERVE"), with the kernel OWNER still holding the open store and any active candidate.
   - Fix the path that now answers EIO, or prove a different design. Contract 3.6 of the user security documents governs the resume rules.
3. **Diagnostics.** Log the failing step and error of each distinct refusal once, for example `[accounts] first-boot step=reserve error=-11`.
   - Log the success line.
   - Log the number of transient retries on success.
   - The lead must be able to tell from the serial log which step waited.
4. **Waiting for entropy is explicit.**
   - Either ACCOUNT does not start the promotion until the generator answers, or an entropy EAGAIN before any durable write is retried in place.
   - Choose one, justify it, and keep `getentropy` non-blocking (the kernel contract).

## Tests

All tests use the production ACCOUNT code and the kernel OWNER of `tests/host/security/system_owner_test.c`:

- entropy unavailable at each entropy call site of the promotion;
- tree-digest EAGAIN injected at each OWNER operation of the first batch;
- an OWNER candidate or generation EAGAIN;
- several consecutive EAGAINs;
- an in-session resume with no OWNER detach.

Each case ends with the seed promoted, one machine SID, the profiles published and the store reopening cleanly at a simulated reboot.

The existing H15 cases and `tests/host/security/adapter_test.c` stay green. Replace or adjust the lead's two quick-fix assertions as your design requires.

## Acceptance

- The new tests pass.
- The security suite and the desktop suite gain no failure. Failures that only LeakSanitizer causes in the sandbox are listed separately.
- The lead verifies with repeated `qemu-t23` first boots.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-sec-firstboot` (branch `wt/sec-firstboot`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Report to `build/codex/sec-firstboot-report.md`. Leave the changes uncommitted.
