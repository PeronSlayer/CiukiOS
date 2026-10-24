# Terminal

Open **Programs → Terminal** for CiukiOS's native command prompt. The Terminal
is a resizable monospace grid, initially 80×25, with 1000 retained output lines.
Its working directory starts at `C:\Documents`. Commands are case insensitive.

| Command | Use |
| --- | --- |
| `DIR [path]`, `ls [path]` | List files, directories, or mounted drives in Computer. |
| `CD [path]`, `cd [path]` | Show or change the working directory. `..` goes up. |
| `C:`, `D:` | Go to that drive's root; additional drives must be mounted. |
| `COPY source destination`, `cp` | Copy a file or directory. Wildcard sources need a destination folder. |
| `MOVE source destination`, `mv` | Move a file or directory, including between mounted drives. |
| `REN source destination` | Rename one item on its current volume. |
| `DEL files`, `rm files` | Delete files. Does not recursively delete directories. |
| `MD directory`, `mkdir` | Create one directory. |
| `RD directory`, `rmdir` | Remove an empty directory. |
| `TYPE files`, `cat files` | Print file contents. |
| `CLS`, `clear` | Clear the grid and scrollback. |
| `ECHO text` | Print text. |
| `VER`, `HELP`, `EXIT` | Show the system version, command help, or close Terminal. |
| `START program [file]` | Ask the desktop to launch a graphical native program. |
| `NET` | Show the network driver, MAC address, carrier and driver state. |
| `AUDIO` | Show left/right master volume and mute. |
| `AUDIO volume n`, `AUDIO mute`, `AUDIO unmute` | Request a master volume change; `n` is 0–100. |
| `TASKS`, `TASKS KILL pid` | List processes or request End Task. System processes are protected. |
| `DISPLAY` | Show the current display mode and available modes. |
| `MEM` | Show usable/free physical memory and kernel heap usage. |
| `DATE`, `TIME` | Show the current date or time. |
| `SHUTDOWN`, `REBOOT` | Confirm, then ask the desktop to close programs and flush storage before powering off or restarting. |
| `LUA script.lua [args]`, `script.lua [args]` | Run Lua in this Terminal. |
| `LUA` | Enter interactive Lua; `.exit` returns to the native prompt. |
| `RUN program [args]` | Prints `not available in this release`: the native SDK has no pipes yet. |
| `REG` | Reserved for the registry directive, f2-47. |
| `SCANREG [/BACKUP] [/COMMENT=text] [/FIX]` | Registry Checker: check the registry roots, `/FIX` restores a damaged root from the newest good backup, `/BACKUP` writes `C:\SYSTEM\SYSBCKUP\RB00n.CRB` (five kept). |
| `SCANREG /RESTORE [n]` | List the backups by date, or restore backup `n` (only after a Command prompt only start), then `REBOOT`. |
| `SCANDISK [d:] [/A] [/THOROUGH] [/AUTOFIX]` | Check a drive (default: the current one) and print the ScanDisk report; `/AUTOFIX` schedules the repair for the next start. |
| `SFC`, `SFC /UPDATE file`, `SFC /RESTORE file source` | System File Checker: list changed and deleted system files; keep a new version; restore the original from a source folder. |
| `WIN` | After a Command prompt only start: start the normal desktop. |

Paths may be relative, absolute POSIX paths, or drive paths such as
`C:\Documents\notes.txt`. `C:` maps to `/`; `D:` through `Z:` map to `/mnt/d`
through `/mnt/z`. `CD Computer` selects the virtual Computer namespace and
`DIR` there lists the mounted drives. `..` at a drive root stays at that root.
Drive-relative paths like `C:notes.txt` are unsupported.

Quote paths containing spaces. `*` matches any name suffix and `?` matches one
character in the last path component; matching is case insensitive. `*.*`
includes names without extensions. COPY, MOVE and REN refuse existing targets.
Directories copied by COPY/MOVE include their contents. A failed multi-item
command may have completed earlier items; read its error before retrying.

```text
CD C:\Documents
DIR *.txt
COPY "my notes.txt" backup.txt
MD Backup
MOVE backup.txt Backup
START ciuknote "my notes.txt"
AUDIO volume 70
```

Tab completes the current path, extending its common prefix when several names
match. Up/Down recall commands. Page Up/Down scroll the output; typing follows
the prompt again. Drag across output to select it, then press Ctrl+C to copy.
Ctrl+V pastes printable text into the input line. Right-click the grid for Copy,
Clear and Help. Input and the grid use printable ASCII; other output bytes are
shown as `?`. History retains 200 commands in `/Documents/.terminal_history`
and is saved after each submitted line.

The desktop acknowledges START, AUDIO changes and TASKS KILL asynchronously.
“Requested” means the request was queued; a refusal is printed when the desktop
replies. START accepts graphical programs; console Lua runs with LUA. DISPLAY,
DATE and TIME do not change system settings. NET's carrier status describes the
cable link, without asserting Internet connectivity.

Lua uses the existing pinned Lua 5.4.8 port, linked into the same app. Standard
`print` and `term.print` write to the grid. `term.run` returns a command's output
text and status (zero or negative errno); it does not also print that output.
For example:

```lua
local text, status = term.run("DIR C:\\Documents")
assert(status == 0, text)
term.print(text)
print("Argument:", arg[1])
```

`arg[0]` is the script's absolute path, with arguments from `arg[1]`. Interactive
Lua keeps variables between lines, prints expression results and accepts
continuation lines for unfinished chunks. Ctrl+C interrupts Lua execution;
blocking library calls return before the instruction hook can observe it.
Lua file I/O uses the real filesystem and current directory. Standard `io`
file handles retain the port's behavior; use `print` or `term.print` for grid
output. Nested Lua invocation through `term.run`, EXIT and power confirmations
inside an output capture are refused. Captures and script input have a 1 MiB
limit. Pipelines, redirection and batch files are outside this release.
