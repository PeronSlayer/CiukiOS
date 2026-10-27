# sec-wave3: k03, k04, k05, k06 and user-02 in parallel

- **Written:** 2026-10-27 12:20 by the lead. Base: branch `wt/sec-integ` (main 6a9abd114 plus sec-k01, sec-user-01, sec-int-01 and sec-k02).
- **Owner decisions 2026-10-27:** the whole user-security programme is completed now; the next hardware image waits for it. Steps whose file areas are disjoint run in parallel.
- **Binding specification:** `docs/design/user-security-contract.md` (model) and `docs/design/user-security-kernel.md` (kernel), especially "Implementation plan" (line ~1767), its table rows 3A, 3B, 4A, 4B and 4C, "Host acceptance commands" and the sections those rows cite; `docs/directives/sec-lead-decisions.md`; the earlier directives `sec-k01.md`, `sec-user-01.md`, `sec-k02.md` and their reports under `build/lead-scratch/` in the main checkout (`sec-*-final.md`).

## Steps and owners

| Step | Implementer | Worktree / branch | Contract row |
| --- | --- | --- | --- |
| sec-k03: every file, handle and mapping entry point enforces the retained subject | Claude agent | `CiukiOS-wt-sec-k03` / `wt/sec-k03` | 3A |
| sec-k04: device and administrative authority | Codex gpt-6.1-sol xhigh | `CiukiOS-wt-sec-k04` / `wt/sec-k04` | 3B |
| sec-k05: DOS VMs inherit the subject; per-user DOS saves | Claude agent | `CiukiOS-wt-sec-k05` / `wt/sec-k05` | 4A |
| sec-k06: Win32 isolation and the SYSTEM registry broker | Codex gpt-6.1-sol xhigh | `CiukiOS-wt-sec-k06` / `wt/sec-k06` | 4B |
| sec-user-02: Setup (mandatory custom administrator, CiukiAdministrator password), ready images with password-less CiukiUser, G32 migration and repair | Claude agent | `CiukiOS-wt-sec-user02` / `wt/sec-user02` | 4C |

The contract orders k05 and k06 after k03 and user-02 after k04. Here they run at the same time: code against the interfaces the contract and the integrated k01/k02 code define, and where you need a k03/k04 function that does not exist yet, call it through a narrowly named declaration you list in your report (the lead wires it at integration). Never edit a file area that the table gives to another row; shared syscall tables, ABI headers (`abi.h`, `process.h`), `scripts/test/host_kernel_tests.sh` and runner registration: add your entries in one contiguous, commented block and list them, so the lead can merge the five branches.

## Common acceptance

- The row's acceptance criteria and its host evidence from "Host acceptance commands".
- Games and installers keep working for the logged-on administrator and for a standard user where the contract allows it: the Win32 installer suites (`tests/host/win32/installers`, including the Quake III installer run to its exit), the DOS VM suites, the desktop suites, and the kernel suite stay green.
- New `abi.h` records get `abi_layout` rows (one field per line); `scripts/gen_api_reference.py --check` passes.
- Constant `kmalloc`/`kzalloc` sizes above 2040 bytes do not compile (fx-r3dalloc); allocate larger state from pages.

Report to `build/codex/<step>-report.md` in your worktree. Leave changes uncommitted.
