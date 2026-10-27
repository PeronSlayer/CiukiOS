# sec-k06 implementation evidence

Scope: sec-wave3 row 4B. Runtime qualification belongs to the lead; this
worktree runs host tests only. The verification table and integration seams
are recorded in `build/codex/sec-k06-report.md`.

## Primary sources and implementation decisions

- Microsoft [Access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens)
  and [OpenProcessToken](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openprocesstoken):
  token identity belongs to the process. Use a kernel handle holding the frozen
  credential, require process query access and TOKEN_QUERY, deny transfer and
  inheritance, revoke at session close. Do not accept caller SID bytes as rights.
- Microsoft [Desktops](https://learn.microsoft.com/en-us/windows/win32/winstation/desktops):
  messages stay within a desktop. Key named objects and window stations by the
  immutable security session; check the target before copying message/APC data.
- Microsoft [process access rights](https://learn.microsoft.com/en-us/windows/win32/procthread/process-security-and-access-rights),
  [thread access rights](https://learn.microsoft.com/en-us/windows/win32/procthread/thread-security-and-access-rights)
  and [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject):
  priority setters require SET_INFORMATION, queries require QUERY_INFORMATION
  (or the supported limited-query equivalent), and waits require SYNCHRONIZE.
  The inspected wait resolver and priority branches lacked these checks. Apply
  rights before consuming an object or changing scheduler state. Signal-and-wait
  already routes its signal through the object mutation entry point.
- Microsoft [Registry key security](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-key-security-and-access-rights)
  and [registry structure](https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/windows-registry-advanced-users):
  check both logical namespace policy and granted handle access on every call.
  HKCU is the retained user; .DEFAULT remains the protected template. Serialized
  hive files are never an alternative authorization interface. Provisioning a
  personal hive copies template preferences but clears Run/RunOnce and
  RunServices/RunServicesOnce values; a new identity cannot inherit deferred
  commands from the template. H07 exercises the actual protected-slot load,
  profile selection, SID rename and persistence transition.
- Microsoft [Run and RunOnce](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys):
  user and machine queues are distinct. The CiukiOS contract adds durable owned
  jobs, current authorization on replay, and a held uncertain-launch state.
  A SYSTEM broker must never execute a caller command as SYSTEM.
- Microsoft Windows 98 Resource Kit, local read-only extraction
  `/home/peronslayer/Desktop/CiukiOS/build/rskt/wrkc31_1.htm` and
  `wrkc31_4.htm` (registry structure), `wrkc31_2.htm` (Registry Checker):
  System.dat and User.dat are separate, profile-specific data can reside below
  the user's profile, and backup files carry the same sensitive contents.
  CiukiOS retains the CRG/text formats and their durability rules, while its
  real-user contract strengthens backing-file protection beyond Windows 9x.

The inspected baseline used a global named-object list, global legacy service
slots, HKCU through .DEFAULT, direct libreg machine files and an unowned v1
restart journal. These are the enforcement and migration boundaries for 4B.

Restart persistence uses fixed generated file names, SHA-256-bound durable
intents and monotonic journal generations. A committed intent completes its
exact candidate after interruption. Quarantine keeps the original bytes in
content-addressed protected evidence before publishing a consumed marker; it
never assigns them to a user. Retries flush both already-present evidence and
already-marked receipts, because a failed flush without process death can leave
complete but non-durable live bytes. Host cut fixtures cover this case too.
