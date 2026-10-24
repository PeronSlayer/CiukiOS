# G26C browser integration

The binding scope is G26C in the lead's third-parts directive. The directive
read from the lead worktree contains twelve lines, including its inherited
batch and second-parts rules; it has no further amendment or lead-note section.

## References and decisions

The owner's Windows 98 Resource Kit, chapter 20 (`wrkc20_3.htm`, Explorer bars
and Browse in a New Process), describes retained browsing history and a new
process when a link requests a new window. Chapter 23 (`wrkc23_2.htm`, Control
Panel tools) locates Internet configuration in Control Panel. Chapter 11
(`wrkc11_1.htm`, Spooling) requires queued printing. The text was inspected in
place, without copying private media into tracked files.

* [HTML 4.01 frame targets](https://www.w3.org/TR/html401/types.html#type-frame-target)
  defines `_blank` as a new unnamed window. Retain the G26B URL-only native
  process launcher, exercise its failure and permission boundaries, and do not
  navigate the opener as a fallback when a launch fails.
* [Microsoft URL security zones](https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/platform-apis/ms537183(v=vs.85))
  describes action policies per zone. Preserve the four configurable Web zones,
  restricted-site precedence and exact-host matching. Numeric intranet addresses
  must be valid IPv4, never a prefix such as `10.attacker.example`. Empty hosts
  must not acquire intranet privileges. Only implemented browser actions are
  configurable; this does not claim IE's ActiveX/Java/security-manager breadth.
* [Microsoft file associations](https://learn.microsoft.com/en-us/windows/win32/shell/fa-file-types)
  describes extension/ProgID/command associations. Programs must write G8's
  existing shared HKCR service, not another default-program store. Register
  `.htm`, `.html`, HTTP and HTTPS with quoted arguments. Saved downloads reach
  Files' existing Open With chooser through a constrained desktop request.
* [NetSurf 3.11 pagination source](https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz),
  [cache clone](https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz)
  and [HTML clone](https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz)
  reveal that the generic printer's required cache clone is unimplemented and
  the HTML clone asserts. Do not call them. Render the complete existing layout
  in successive paper-sized clips using the framebuffer plotters. Scale to paper
  width, exclude browser chrome and ignore viewport scroll. Page Setup controls
  paper/orientation/margins; Print controls inclusive range/copies/output.
  This paginates the screen-media layout; print-specific CSS and avoiding a cut
  through a line at a page boundary remain gaps. It neither mutates/reformats the
  live document nor introduces an upstream assertion path.

Layout and plotter globals require rendering on the browser's UI owner after
Print confirmation (the current document at confirmation is printed). A modal
Print dialog holds browser input; no browser object crosses into G31's worker.
The worker subsequently publishes the immutable raw file. One page is admitted
against the existing image ledger, and staging output is bounded by G31's limit.
Cancellation or any render/output failure releases staging; no partial job is
published. No memory limit is raised and no new ABI number is allocated.

Host tests must cover the production printer bridge, settings and failure
cleanup, shared HKCR/default associations, Internet UI actions/history/zones,
new-window launch authorization and full-redraw equivalence of the new UI events.
No QEMU, image build, hardware result or full IE compatibility is claimed.

## Implemented integration boundaries

Programs' Make Default uses `fm_browser_default` over the same HKCR RPC as G8;
`.htm`/`.html` and HTTP/HTTPS open commands quote `%1`. The shared resolver reads
protocol commands, so changing HTTP's registered command also changes URL launch.
Missing service/registration retains the existing NetSurf fallback; malformed or
other failed reads return an error. Desktop URL shortcuts use the ordinary
background resolver. The G8 path-open worker uses selector 4, distinct from
selector 3's explicit launch record (previously both selected 3).

Programs' File Types button opens Files' existing File Types sheet via its
existing FILES_TYPES action. A completed
browser download offers Open, Open With and Close; Open invokes G8's shared
launcher, Open With invokes its program/Browse/Always use chooser. No file opens
automatically. The existing v3 launch is restricted to the authenticated NetSurf
identity, the fixed `files` program and two validated argument forms. Other file
actions, relative paths, dot components, repeated separators and overlong paths
are refused. Saving a longer path still succeeds without an Open shortcut.

G26B already supplies history-day editing, transactional cross-window clear,
restricted/trusted lists and per-zone scripting; these remain connected to the
browser. Zone numeric classification now parses complete IPv4 octets and includes
172.16/12, instead of trusting textual prefixes. Empty/malformed/IPv6 hosts receive
Internet policy. Explicit restricted entries still override trusted/intranet.
Settings apply in new browser processes as the property page states.

Automatic `_blank` GET navigation reuses G26B's core hook and the native URL-only
launch. This change reports delayed launch failures, in addition to immediate
send/length failures. The opener never navigates as a failure fallback. Form POST
transfer, named-window reuse and JavaScript-created windows remain outside that
URL-only process handoff; they require a separate retained request-body design.
No completion claim is made for those paths.
