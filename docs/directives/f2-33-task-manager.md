# Directive f2-33: Task Manager and the process enumeration interface

- **Step:** F2+ (desktop applications). **Contracts:** `execution-abi.md`
  and `posix-subset.md` (amended here: a process enumeration syscall and
  the termination authority), `f2-acceptance.md` (supervisor, process
  groups, crash isolation), `docs/desktop.md`, f2-31 (`libdeskui`); parity
  reference: the 0.8 Task Manager (Applications, Processes, Performance;
  the Virtual Machines tab returns with F3).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-tasks`, from `main` after f2-31 is merged. Files:
  `src/kernel/include/ciuki/abi.h` (syscall `CIUKI_SYS_PROC_LIST = 70`
  with fixed-width records), `src/kernel/proc/syscalls_proc.c`,
  `process.c`/`task.c` (accounting: per-process CPU ticks, pages, threads,
  state, name, parent, group), `supervisor.c` (termination authority
  delegated to the task manager launched from the desktop menu, as the
  desktop's close path and the mixer control handle of F4 do), `sdk/libciuki`
  wrapper, new `apps/tasks/`, `apps/build_apps.py`, `scripts/build_image.py`
  (`/bin/tasks`), host tests, one QEMU case `tasks-qemu-t23`,
  `docs/design/execution-abi.md` and `posix-subset.md` amendments,
  `docs/desktop.md`.

## Scope

1. **Kernel.** `proc_list(buf, size)` returns records (version, pid, ppid,
   group, state, thread count, resident pages, CPU ticks, start tick,
   name ≤ 31) plus one system record (total/idle ticks, usable/free pages,
   kernel heap high-water); readable by any process, copied out with
   validation; a termination request on a non-descendant process requires
   the delegated authority (otherwise `EPERM`), and never targets the
   supervisor or the desktop's own group except through the desktop.
2. **Application.** Tabs Applications (desktop windows with state from the
   desktop through a v2 protocol query added in f2-30's style: title, pid,
   responding; End Task via the desktop's close request; Switch To; New
   Task runs a `/bin` program), Processes (sortable image name, PID, PPID,
   threads, memory, CPU %), Performance (CPU usage with a 60-second
   history from the system record, memory totals, kernel heap); View:
   Update speed High/Normal/Low/Paused and Refresh (F5). The Virtual
   Machines tab is added by F3 (f3-02) and is out of scope here.
3. **Tests.** Host: the record layout and accounting (a fake process set),
   `EPERM` without authority, the UI model and digests; QEMU
   `tasks-qemu-t23`: launch Tasks from the menu, see the demo process
   launched by the script, end it, verify the process disappears and the
   desktop stays alive; `desktop_screen` digests.

## Acceptance by the lead

Host tests; kernel/SDK/app/image build; `tasks-qemu-t23` PASS; `f2-all`
unaffected (new syscall unadvertised to the F2 gate programs).
