"""H13 (sec-k08) source-derived policy census.

derive(root) reads the published selectors from the sources, never from this
file: native syscall numbers (abi.h enum ciuki_syscall and CIUKI_SYS_W32),
call-101 operations (the security_syscall switch), call-99 services and
their sub-actions (the call-99 adapters), the call-101 channel services and
operations (channel.c max_op, the LOGON/OWNER/registry protocol headers),
device commands of call 69 (abi.h/input_abi.h command constants) and the VFS
access bits and policy kinds (fs/security_policy.h).

MAP gives each selector exactly one policy class and the test that covers it
as "file" (the file must name the selector's constant) or "file::marker".
check() reports selectors without a mapping, stale mappings and missing
markers; check_defaults() checks that each dispatcher's default denies.
"""
from pathlib import Path
import re

# ---- derivation ------------------------------------------------------------

def _read(root, name):
    return (Path(root) / name).read_text(errors='replace')


def _block(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def syscalls(root):
    abi = _read(root, 'src/kernel/include/ciuki/abi.h')
    body = _block(abi, 'enum ciuki_syscall {', '};')
    names = re.findall(r'(CIUKI_SYS_\w+)\s*=\s*\d+', body)
    names += re.findall(r'enum ciuki_w32_syscall \{\s*(CIUKI_SYS_W32)\s*=', abi)
    return names


def call101(root):
    sec = _read(root, 'src/kernel/proc/security.c')
    body = _block(sec, 'int security_syscall(struct trap_frame *tf)', '\ndone:')
    body = body[body.index('switch (a.operation)'):]
    return sorted(set(re.findall(r'case (CIUKI_SECURITY_\w+)', body)))


def call99(root):
    sysvc = _read(root, 'src/kernel/proc/syscalls_sysvc.c')
    rec = _read(root, 'src/kernel/proc/syscalls_recovery.c')
    body = _block(sysvc, 'int sysvc_syscall(', '\n}')
    ops = set(re.findall(r'case (CIUKI_RECOVERY_\w+)', body))
    ops |= set(re.findall(r'op [!=]= (CIUKI_RECOVERY_\w+)', rec))
    return sorted(ops)


def call99_actions(root):
    sysvc = _read(root, 'src/kernel/proc/syscalls_sysvc.c')
    jobs = _read(root, 'src/kernel/fs/sysvc_jobs.c')
    out = set(re.findall(r'r\.action [!=]= (CIUKI_SYSTEM_LOG_\w+)', sysvc))
    out |= set(re.findall(r'r\.scope [!=]= (CIUKI_SYSTEM_LOG_\w+)', sysvc))
    out |= set(re.findall(r'r->action [!=]= (CIUKI_DRIVER_PACK_\w+)', sysvc))
    out |= set(re.findall(r'r\.action [!=]= (CIUKI_BOOT_CONFIG_\w+)', sysvc))
    out |= set(re.findall(r'r\.action [!=]= (CIUKI_SYSTEM_SETTING_\w+)', sysvc))
    out |= set(re.findall(r'case (CIUKI_SYSTEM_SETTING_\w+)', jobs))
    out |= set(re.findall(r'job [!=]= (CIUKI_SYSTEM_JOB_\w+)', jobs))
    return sorted(out)


def channel(root):
    """(service, operation) pairs: max_op bounds; servers' names where published."""
    abi = _read(root, 'src/kernel/include/ciuki/abi.h')
    services = {int(v): n for n, v in re.findall(r'#define CIUKI_SECURITY_SERVICE_(AUTH|ACCOUNTS|OWNER|REGISTRY|LOGON|W32SRV) (\d+)u', abi)}
    ch = _read(root, 'src/kernel/proc/channel.c')
    bounds = re.search(r'static const uint32_t max_op\[\]=\{([^}]*)\}', ch).group(1).split(',')
    bounds = [int(b.strip().rstrip('uU'), 0) for b in bounds]
    out = []
    for number, name in sorted(services.items()):
        limit = bounds[number]
        if limit == 0xffffffff:
            if name == 'REGISTRY':
                proto = _read(root, 'sdk/private/registry_protocol.h')
                for block in re.findall(r'enum \{([^}]*)\}', proto):
                    if 'RB_NATIVE_READ' in block or 'RB_CHECKER_LIST' in block:
                        out += [f'REGISTRY:{n}' for n in re.findall(r'(RB_[A-Z0-9_]+)', block)]
            else:
                out.append(f'{name}:framed')     # passthrough bound: no framed operation is served
        else:
            out += [f'{name}:{op}' for op in range(1, limit + 1)]
    return out


DEVCTL = re.compile(r'#define ((?:AUDIO|MIXER|NET|COM|DISK|INPUT|LPT|MEDIA|CIUKI_SETUP|DISPLAY_GET)_\w+)\s+'
                    r'(0x(?:4[1-9a-d][0-9a-fA-F]{2}|434f01[0-9a-fA-F]{2}|4c5001[0-9a-fA-F]{2}|5354[0-9a-fA-F]{4}))u\b')


def devctl(root):
    text = _read(root, 'src/kernel/include/ciuki/abi.h') + _read(root, 'src/kernel/include/ciuki/input_abi.h')
    return sorted(set(n for n, _ in DEVCTL.findall(text)))


def vfs(root):
    h = _read(root, 'src/kernel/fs/security_policy.h')
    access = re.findall(r'(POLICY_[A-Z]+)=\d+u', _block(h, 'enum policy_access {', '};'))
    kinds = re.findall(r'(POLICY_[A-Z]+)', _block(h, 'enum policy_kind {', '};'))
    return access, kinds


def derive(root):
    access, kinds = vfs(root)
    return {'syscall': syscalls(root), 'call101': call101(root), 'call99': call99(root),
            'call99-action': call99_actions(root), 'channel': channel(root), 'devctl': devctl(root),
            'vfs-access': access, 'vfs-kind': kinds}

# ---- policy classes ---------------------------------------------------------
ANY = 'anyone'                         # public or own-process data, bounded copies
OWN = 'own process/session'            # own fds, memory, threads, children
PATH = 'path policy (VFS access check on the resolved object)'
FD = 'retained description rights (checked at open, O(1) warm)'
GRANT = 'session grant (display/input/audio owner of the live session)'
ADMIN = 'administrator (live credential) or SYSTEM'
SYSTEM = 'SYSTEM role only'
LOGON = 'LOGON role only'
SETUP = 'SYSTEM Setup role in Setup boot'
RESERVED = 'reserved: ENOSYS'
DENY = 'deny'

# ---- the map ----------------------------------------------------------------
PROC = 'tests/host/proc/proc_test.c'
FILES = 'tests/host/proc/files_test.c'
FENF = 'tests/host/security/file_enforcement_test.c'
DESK = 'tests/host/proc/desktop_test.c'
CRED = 'tests/host/security/credentials_test.c'
SVC = 'tests/host/security/system_services_test.c'
DEV = 'tests/host/security/devices_test.c'
H13N = 'tests/host/security/system_services_test.c::H13 native'
H13C = 'tests/host/security/registry_adapter_test.c::H13 channels'
POL = 'tests/host/security/owner_policy_test.c'

LWIP = 'tests/host/lwip/'
MAP = {
    'syscall': {
        'CIUKI_SYS_EXIT': (OWN + ' (F0)', PROC + '::task_exit'),
        'CIUKI_SYS_YIELD': (OWN + ' (F0)', 'tests/host/proc/report_routing_test.c::task_yield'),
        'CIUKI_SYS_DEBUG_WRITE': (OWN + ' (F0, bounded copy)', 'tests/host/test_report_routing.py::src/kernel/core/syscall.c'),
        'CIUKI_SYS_PROBE_REPORT': (OWN + ' (F0, controller framing)', 'tests/host/test_report_routing.py::src/kernel/core/syscall.c'),
        'CIUKI_SYS_SLEEP_MS': (OWN + ' (F0, bounded)', 'tests/host/proc/signal_test.c::CIUKI_SYS_SLEEP_MS'),
        'CIUKI_SYS_PROBE_QUERY': (OWN + ' (F0, own test request)', 'tests/host/test_report_routing.py::src/kernel/core/syscall.c'),
        **{f'CIUKI_SYS_RESERVED_{n}': (RESERVED, H13N + '::unknown syscall') for n in range(6, 16)},
        'CIUKI_SYS_SPAWN': (OWN + '; the child inherits, never elevates (k01)', CRED + '::proc_spawn'),
        'CIUKI_SYS_WAITPID': (OWN + ' (children)', PROC + '::proc_waitpid'),
        'CIUKI_SYS_GETPID': (ANY, PROC + '::->pid'),
        'CIUKI_SYS_GETPPID': (ANY, PROC + '::ppid'),
        'CIUKI_SYS_THREAD_CREATE': (OWN, PROC + '::proc_thread_create'),
        'CIUKI_SYS_THREAD_EXIT': (OWN, PROC + '::proc_thread_exit'),
        'CIUKI_SYS_TLS_SET': (OWN, PROC + '::proc_tls_set'),
        'CIUKI_SYS_WAIT_WORD': (OWN + ' (own memory)', PROC + '::proc_wait_word'),
        'CIUKI_SYS_WAKE_WORD': (OWN + ' (own memory)', 'tests/host/native_samples_host.c::CIUKI_SYS_WAKE_WORD'),
        'CIUKI_SYS_MMAP': (OWN + ' (W^X)', PROC + '::ua_mmap'),
        'CIUKI_SYS_MUNMAP': (OWN, PROC + '::ua_munmap'),
        'CIUKI_SYS_MPROTECT': (OWN + ' (W^X)', PROC + '::ua_mprotect'),
        'CIUKI_SYS_BRK': (OWN, PROC + '::ua_brk'),
        'CIUKI_SYS_OPEN': (PATH, FENF),
        'CIUKI_SYS_READ': (FD, FENF),
        'CIUKI_SYS_WRITE': (FD, FENF),
        'CIUKI_SYS_PREAD': (FD, H13N + '::CIUKI_SYS_PREAD'),
        'CIUKI_SYS_PWRITE': (FD, H13N + '::CIUKI_SYS_PWRITE'),
        'CIUKI_SYS_LSEEK64': (FD, FILES),
        'CIUKI_SYS_CLOSE': (OWN, FENF),
        'CIUKI_SYS_FSTAT': (FD, FILES),
        'CIUKI_SYS_STAT': (PATH + ' (read)', H13N + '::CIUKI_SYS_STAT'),
        'CIUKI_SYS_GETDENTS': (FD + ' (filtered listing)', FENF),
        'CIUKI_SYS_MKDIR': (PATH + ' (add)', FENF),
        'CIUKI_SYS_RMDIR': (PATH + ' (remove)', FENF),
        'CIUKI_SYS_RENAME': (PATH + ' (both endpoints)', FENF),
        'CIUKI_SYS_UNLINK': (PATH + ' (remove)', FENF),
        'CIUKI_SYS_DUP': (OWN + '; rights never grow', FILES + '::file_dup'),
        'CIUKI_SYS_DUP2': (OWN + '; rights never grow', FILES + '::dup2'),
        'CIUKI_SYS_FCNTL': (OWN + '; rights never grow', FILES + '::file_fcntl'),
        'CIUKI_SYS_FSYNC': (FD, FILES),
        'CIUKI_SYS_FTRUNCATE': (FD + ' (write)', FENF),
        'CIUKI_SYS_GETCWD': (OWN, FILES),
        'CIUKI_SYS_CHDIR': (PATH + ' (traverse)', H13N + '::CIUKI_SYS_CHDIR'),
        'CIUKI_SYS_CLOCK_GETTIME': (ANY, FILES + '::file_clock'),
        'CIUKI_SYS_NANOSLEEP': (ANY, FILES),
        'CIUKI_SYS_SIGACTION': (OWN, 'tests/host/proc/signal_test.c::sigaction'),
        'CIUKI_SYS_SIGPROCMASK': (OWN, 'tests/host/proc/signal_test.c::proc_sigprocmask'),
        'CIUKI_SYS_KILL': ('own uid/session or administrator; PID 1, SYSTEM and desktop groups protected (k04)', 'tests/host/proc/signal_test.c::proc_signal_kill'),
        'CIUKI_SYS_SIGRETURN': (OWN, 'tests/host/proc/signal_test.c::proc_signal_sigreturn'),
        'CIUKI_SYS_UNAME': (ANY, FILES),
        'CIUKI_SYS_SURFACE_CREATE': (OWN, DESK + '::surface_create'),
        'CIUKI_SYS_SURFACE_MAP': (OWN, DESK + '::surface_create'),
        'CIUKI_SYS_SURFACE_INFO': (OWN, DESK),
        'CIUKI_SYS_PRESENT': (GRANT, DESK + '::present'),
        'CIUKI_SYS_INPUT_READ': (GRANT, DESK + '::input_read'),
        'CIUKI_SYS_CHANNEL_PAIR': (OWN, DESK + '::channel_pair'),
        'CIUKI_SYS_CHANNEL_SEND': (OWN + '; framed service peers retained (k01)', H13C + '::acct_sys_channel_send'),
        'CIUKI_SYS_CHANNEL_RECV': (OWN, DESK + '::channel_recv'),
        'CIUKI_SYS_THREAD_JOIN': (OWN, PROC + '::proc_thread_join'),
        'CIUKI_SYS_THREAD_DETACH': (OWN, PROC + '::proc_thread_detach'),
        'CIUKI_SYS_DISPLAY_INFO': (ANY + ' (copied inventory)', DESK + '::display_info'),
        'CIUKI_SYS_THREAD_KILL': (OWN, 'tests/host/proc/signal_test.c::proc_signal_thread_kill'),
        'CIUKI_SYS_DEVCTL': ('per device command (devctl family)', DEV),
        'CIUKI_SYS_PROC_LIST': (ANY + ' (bounded nonsecret summaries)', PROC + '::proc_list('),
        'CIUKI_SYS_PROC_TERMINATE': ('own uid/session or administrator; PID 1/SYSTEM/desktop group protected', H13N + '::CIUKI_SYS_PROC_TERMINATE'),
        'CIUKI_SYS_DOSVM': ('own session VM: capability, owner key and immutable uid/session (k05)', 'tests/host/dosvm_test.c::CIUKI_DOSVM_'),
        'CIUKI_SYS_POWER': ('actions: live session or SYSTEM logon; POLICY administrator (k04)', 'tests/host/security/power_policy_test.c::power_record_call'),
        'CIUKI_SYS_DISPLAY': (GRANT + '; machine defaults administrator', DESK),
        'CIUKI_SYS_LAUNCH_MARK': ('status anyone; markers own-session compositor/child', 'tests/host/proc/launch_test.c::proc_launch_mark'),
        'CIUKI_SYS_DEVICES': (ANY + ' (copied inventory)', 'tests/host/devices_test.c::devices_query'),
        'CIUKI_SYS_DISPLAY_ACCEL': (GRANT, DESK),
        'CIUKI_SYS_SOCKET': ('own socket/session; no low-port privilege', LWIP + 'stack_test.c'),
        'CIUKI_SYS_BIND': ('own socket/session; no low-port privilege', LWIP + 'amendment.inc'),
        'CIUKI_SYS_CONNECT': ('own socket/session', LWIP + 'amendment.inc'),
        'CIUKI_SYS_LISTEN': ('own socket/session', 'tests/host/win32/offline_test.c'),
        'CIUKI_SYS_ACCEPT': ('own socket/session', LWIP + 'stack_test.c'),
        'CIUKI_SYS_SENDTO': ('own socket/session', LWIP + 'stack_test.c'),
        'CIUKI_SYS_RECVFROM': ('own socket/session', LWIP + 'stack_test.c'),
        'CIUKI_SYS_SHUTDOWN': ('own socket/session', 'tests/host/win32/offline_test.c'),
        'CIUKI_SYS_SOCKNAME': ('own socket/session', LWIP + 'amendment.inc'),
        'CIUKI_SYS_SOCKOPT': ('own socket/session', LWIP + 'amendment.inc'),
        'CIUKI_SYS_POLL': ('own descriptors', LWIP + 'sdk_test.c'),
        'CIUKI_SYS_RESOLVE4': ('own resolver request', LWIP + 'sdk_test.c'),
        'CIUKI_SYS_NETCTL': ('GET anyone; SET/RENEW/RELEASE administrator (k04)', LWIP + 'netctl.inc'),
        'CIUKI_SYS_GETENTROPY': (ANY + '; EAGAIN unseeded', LWIP + 'sdk_test.c'),
        'CIUKI_SYS_W32': ('Win32 personality; per-family checks (k06)', 'tests/host/security/win32_isolation_test.inc::H13 win32'),
        'CIUKI_SYS_TIME': ('GET anyone; SET_ZONE/LOCAL/UTC/ACKNOWLEDGE administrator (k04)', 'tests/host/proc/localtime_test.c::CIUKI_TIME_SET_ZONE'),
        'CIUKI_SYS_FILE_INFO': (PATH + ' (GET read, SET metadata)', H13N + '::CIUKI_SYS_FILE_INFO'),
        'CIUKI_SYS_RECOVERY': ('per service (call99 family)', SVC),
        'CIUKI_SYS_NET_INFO': ('config/stats/routes anyone; sockets filtered by uid/session (k04)', LWIP + 'raw.inc'),
        'CIUKI_SYS_SECURITY': ('per operation (call101 family)', SVC),
    },
    'call101': {
        'CIUKI_SECURITY_QUERY_SELF': (ANY, CRED + '::security_args(1)'),
        'CIUKI_SECURITY_QUERY_PROCESS': ('own uid and session, administrator, or SYSTEM; never SYSTEM targets', H13N + '::CIUKI_SECURITY_QUERY_PROCESS'),
        'CIUKI_SECURITY_QUERY_PEER': ('holder of the live endpoint', CRED + '::security_args(3)'),
        'CIUKI_SECURITY_SESSION_CREATE': (LOGON, H13N + '::CIUKI_SECURITY_SESSION_CREATE'),
        'CIUKI_SECURITY_SPAWN_CREDENTIAL': (LOGON, H13N + '::CIUKI_SECURITY_SPAWN_CREDENTIAL'),
        'CIUKI_SECURITY_SESSION_END': (LOGON, H13N + '::CIUKI_SECURITY_SESSION_END'),
        'CIUKI_SECURITY_SESSION_QUERY': ('own session, LOGON or SYSTEM', CRED + '::security_args(7)'),
        'CIUKI_SECURITY_SESSION_LAUNCH': (LOGON, H13N + '::CIUKI_SECURITY_SESSION_LAUNCH'),
        'CIUKI_SECURITY_LAUNCH_POLL': (LOGON, H13N + '::CIUKI_SECURITY_LAUNCH_POLL'),
        'CIUKI_SECURITY_SECURE_ENTER': (LOGON, H13N + '::CIUKI_SECURITY_SECURE_ENTER'),
        'CIUKI_SECURITY_SECURE_LEAVE': (LOGON + '; one-use cookie', H13N + '::CIUKI_SECURITY_SECURE_LEAVE'),
        'CIUKI_SECURITY_SERVICE_CONNECT': ('fixed role endpoints (security_service_allowed); peer retained', H13C + '::CIUKI_SECURITY_SERVICE_CONNECT'),
        'CIUKI_SECURITY_SERVICE_ACCEPT': ('the registered server role', CRED + '::extended_call(server,13'),
        'CIUKI_SECURITY_SERVICE_CANCEL': ('the requesting endpoint, before first dequeue', CRED + '::extended_call(t,14'),
    },
    'call99': {
        'CIUKI_RECOVERY_STATUS': (ANY + '; authority bit from the credential', DEV),
        'CIUKI_RECOVERY_REPORT': (ANY, DEV),
        'CIUKI_RECOVERY_SET': (ADMIN, DEV),
        'CIUKI_RECOVERY_ENABLE': (ADMIN, DEV),
        'CIUKI_RECOVERY_SCANDISK': (ADMIN + '; system volume through the SYSTEM broker', DEV),
        'CIUKI_RECOVERY_BOOT_CONFIG': (ADMIN, SVC),
        'CIUKI_RECOVERY_LOG': ('per action/scope', SVC),
        'CIUKI_RECOVERY_DRIVER_PACK': (ADMIN, SVC),
        'CIUKI_RECOVERY_SFC_RESTORE': (ADMIN, SVC),
        'CIUKI_RECOVERY_JOB': ('any live session subject or SYSTEM', SVC),
        'CIUKI_RECOVERY_SETTING': (ADMIN, SVC),
    },
    'call99-action': {
        'CIUKI_BOOT_CONFIG_SET': (ADMIN, SVC),
        'CIUKI_SYSTEM_LOG_LIST': ('MACHINE administrator; USER own profile', SVC),
        'CIUKI_SYSTEM_LOG_READ': ('MACHINE administrator; USER own profile', SVC),
        'CIUKI_SYSTEM_LOG_APPEND': ('MACHINE administrator (SCANDISK/SFC only); USER own profile', SVC),
        'CIUKI_SYSTEM_LOG_REPLACE': ('USER own profile; MACHINE never', SVC),
        'CIUKI_SYSTEM_LOG_MACHINE': (ADMIN, SVC),
        'CIUKI_SYSTEM_LOG_USER': ('own profile (kernel-resolved)', SVC),
        'CIUKI_DRIVER_PACK_INSTALL': (ADMIN, SVC),
        'CIUKI_DRIVER_PACK_REMOVE': (ADMIN, SVC),
        'CIUKI_SYSTEM_JOB_STARTUP_REPORT': ('any live session subject or SYSTEM', SVC),
        'CIUKI_SYSTEM_SETTING_SET': (ADMIN, SVC),
        'CIUKI_SYSTEM_SETTING_HWPROFILES': (ADMIN, SVC),
        'CIUKI_SYSTEM_SETTING_PERFORMANCE': (ADMIN, SVC),
        'CIUKI_SYSTEM_SETTING_ACCEL_RETRY': (ADMIN, SVC),
        'CIUKI_SYSTEM_SETTING_CLOCK': (ADMIN + ' (sec-k08, S2-5)', SVC),
        'CIUKI_SYSTEM_SETTING_POWER': (ADMIN + ' (sec-k08, U4-6)', SVC),
    },
}

# Channel services (call-101 SERVICE_CONNECT, framed by channel.c). Connect
# rules: security.c security_service_allowed; per-operation role/authority
# checks are the servers' (ACCOUNT, OWNER, LOGON coordinator, registry broker).
ACC = 'tests/host/security/accounts_test.c'
LGN = 'tests/host/security/logon_service_test.c'
OWN_T = 'tests/host/security/system_owner_test.c'
AUTH_ROLE = 'connect: SYSTEM LOGON/LOGON_UI only; '
ACCOUNTS_ROLE = 'connect: anyone; '
OWNER_ROLE = 'connect: SYSTEM ACCOUNT/Setup only; '
LOGON_ROLE = 'connect: SYSTEM LOGON_UI/ACCOUNT or the session DESKTOP; '
MAP['channel'] = {
    'AUTH:1': (AUTH_ROLE + 'LIST (public names)', 'tests/host/security/fake.c::ACCT_LIST'),
    'AUTH:2': (AUTH_ROLE + 'AUTH with a LOGON challenge', ACC + '::ACCT_AUTH'),
    'AUTH:3': (AUTH_ROLE + 'UNLOCK of the locked session owner', ACC + '::ACCT_UNLOCK'),
    'AUTH:4': (AUTH_ROLE + 'ENROLL_VERIFY', ACC + '::ACCT_ENROLL_VERIFY'),
    'AUTH:5': (AUTH_ROLE + 'BEGIN/CHALLENGE (LOGON relay)', 'tests/host/security/round2_test.c::ACCT_CHALLENGE'),
    'AUTH:6': (AUTH_ROLE + 'AUTOLOGON (ready tuple only)', ACC + '::ACCT_AUTOLOGON'),
    'AUTH:7': (AUTH_ROLE + 'ENROLL_PLAN', 'tests/host/security/round2_test.c::ACCT_ENROLL_PLAN'),
    'AUTH:8': (AUTH_ROLE + 'ENROLL_SOURCE (LOGON)', 'tests/host/security/account_controls_test.inc::ENROLL_SOURCE'),
    'ACCOUNTS:1': (ACCOUNTS_ROLE + 'LOOKUP (public)', 'tests/host/security/fake.c::ACCT_LOOKUP'),
    'ACCOUNTS:2': (ACCOUNTS_ROLE + 'own PASSWORD with the old one', ACC + '::ACCT_PASSWORD'),
    'ACCOUNTS:3': (ACCOUNTS_ROLE + 'CREATE: administrator', ACC + '::h13_accounts'),
    'ACCOUNTS:4': (ACCOUNTS_ROLE + 'UPDATE: administrator; own picture only for others (U4-5)', ACC + '::h13_accounts'),
    'ACCOUNTS:5': (ACCOUNTS_ROLE + 'DELETE: administrator, never self/built-in', ACC + '::h13_accounts'),
    'ACCOUNTS:6': (ACCOUNTS_ROLE + 'LIST (filtered)', 'tests/host/security/fake.c::ACCT_LIST'),
    'ACCOUNTS:7': (ACCOUNTS_ROLE + 'STATUS of the caller\'s own operation id', 'tests/host/security/round2_test.c::ACCT_STATUS'),
    'ACCOUNTS:8': (ACCOUNTS_ROLE + 'PROFILE: own, administrator, LOGON', ACC + '::ACCT_PROFILE'),
    'ACCOUNTS:9': (ACCOUNTS_ROLE + 'RESERVE (durable operation id)', 'tests/host/security/fake.c::ACCT_RESERVE'),
    **{f'OWNER:{n}': (OWNER_ROLE + name, POL + '::' + name) for n, name in enumerate(
        ('OWNER_PREPARE', 'OWNER_COMMIT', 'OWNER_ABORT', 'OWNER_STATUS', 'OWNER_CHECKPOINT', 'OWNER_COMPACT',
         'OWNER_RESERVE'), 1)},
    **{f'OWNER:{n}': (OWNER_ROLE + name + ' (ACCOUNT\'s binding queries)', OWN_T + '::query operations') for n, name in enumerate(
        ('OWNER_IDENTITY', 'OWNER_GENERATION', 'OWNER_BINDING', 'OWNER_ROOT_OCCUPIED', 'OWNER_SNAPSHOT',
         'OWNER_RECOVER_ORPHANS', 'OWNER_CHECKPOINT_FLOOR'), 8)},
    'LOGON:1': (LOGON_ROLE + 'AUTH_OUTCOME: ACCOUNT only', LGN + '::AUTH_OUTCOME'),
    'LOGON:2': (LOGON_ROLE + 'LOCK_EVENT: LOGON_UI only', LGN + '::LGN_OP_LOCK_EVENT'),
    'LOGON:3': (LOGON_ROLE + 'POWER_EVENT: LOGON_UI only', LGN + '::LGN_OP_POWER_EVENT'),
    'LOGON:4': (LOGON_ROLE + 'BEGIN_AUTH: LOGON_UI only', LGN + '::LGN_OP_BEGIN_AUTH'),
    'LOGON:5': (LOGON_ROLE + 'SESSION_LOCK: same-session DESKTOP only', LGN + '::LGN_OP_SESSION_LOCK'),
    'LOGON:6': (LOGON_ROLE + 'SUBMIT_AUTH: LOGON_UI only', LGN + '::LGN_OP_SUBMIT_AUTH'),
    'LOGON:7': (LOGON_ROLE + 'ENROLL_PLAN: LOGON_UI only', 'tests/host/security/logon_ui_test.c::LGN_OP_ENROLL_PLAN'),
    'LOGON:8': (LOGON_ROLE + 'UI_STATE: LOGON_UI only', LGN + '::LGN_OP_UI_STATE'),
    'LOGON:9': (LOGON_ROLE + 'SESSION_EVENT: same-session DESKTOP only', LGN + '::LGN_OP_SESSION_EVENT'),
    'LOGON:10': (LOGON_ROLE + 'ENROLL_SOURCE relay: LOGON_UI only', 'tests/host/security/logon_ui_test.c::LGN_OP_ENROLL_SOURCE'),
    'REGISTRY:RB_NATIVE_READ': ('connect: anyone; machine roots readable, Users root the caller\'s own', 'tests/host/security/registry_adapter_test.c::RB_NATIVE_READ'),
    'REGISTRY:RB_NATIVE_PAGE': ('anyone (as READ)', 'tests/host/security/win32_broker_test.c::RB_NATIVE'),
    'REGISTRY:RB_NATIVE_BEGIN': (ADMIN + '; never the Users root', H13C + '::RB_NATIVE_BEGIN'),
    'REGISTRY:RB_NATIVE_PART': (ADMIN + '; never the Users root', H13C + '::RB_NATIVE_PART'),
    'REGISTRY:RB_NATIVE_COMMIT': (ADMIN + ', rechecked at commit', H13C + '::RB_NATIVE_COMMIT'),
    'REGISTRY:RB_NATIVE_ABORT': ('own staged stream', 'tests/host/security/registry_adapter_test.c::RB_NATIVE_BEGIN'),
    'REGISTRY:RB_MACHINE_ID': (ANY + ' (machine SID, volume, account generation)', 'tests/host/win32/wine/h1_transport.c::RB_MACHINE_ID'),
    'REGISTRY:RB_WIN32': ('HKCU own hive; HKLM writes administrator; HKU other users denied (k06)', 'tests/host/security/win32_broker_test.c::RB_WIN32'),
    'REGISTRY:RB_WIN32_BEGIN': ('as RB_WIN32 (staged)', 'tests/host/win32/service_transport_test.c::RB_WIN32_COMMIT'),
    'REGISTRY:RB_WIN32_PART': ('as RB_WIN32 (staged)', 'tests/host/win32/service_transport_test.c::RB_WIN32_COMMIT'),
    'REGISTRY:RB_WIN32_COMMIT': ('as RB_WIN32', 'tests/host/win32/service_transport_test.c::RB_WIN32_COMMIT'),
    'REGISTRY:RB_WIN32_READ': ('own staged reply', 'tests/host/win32/service_transport_test.c::RB_WIN32_READ'),
    'REGISTRY:RB_CHECKER_LIST': (ADMIN, 'tests/host/security/registry_checker_test.c::RB_CHECKER_LIST'),
    'REGISTRY:RB_CHECKER_BACKUP': (ADMIN, 'tests/host/security/registry_checker_test.c::RB_CHECKER_BACKUP'),
    'REGISTRY:RB_CHECKER_RESTORE': (ADMIN + '; applied at the broker\'s next start', 'tests/host/security/registry_checker_test.c::RB_CHECKER_RESTORE'),
    'REGISTRY:RB_CHECKER_FIX': (ADMIN, 'tests/host/security/registry_checker_test.c::FIX'),
    'REGISTRY:RB_CHECKER_DAILY': ('any session (SYSTEM job, once a day)', 'tests/host/security/registry_checker_test.c::RB_CHECKER_DAILY'),
    'REGISTRY:RB_CHECKER_STATUS': ('any session', 'tests/host/security/registry_checker_test.c::RB_CHECKER_STATUS'),
    'W32SRV:framed': ('connect: own session only (same credential); no framed operation is served', H13C + '::CIUKI_SECURITY_SERVICE_W32SRV'),
}

# Device commands (call 69). Rights on the open device fd are always required.
AUD = 'tests/host/audio/audio_test.c'
MED = DEV
MAP['devctl'] = {
    **{n: ('own stream (audio fd); capture exclusive', AUD + '::' + n) for n in (
        'AUDIO_GET_CAPS', 'AUDIO_SET_FORMAT', 'AUDIO_GET_STATUS', 'AUDIO_SET_GAIN', 'AUDIO_DRAIN', 'AUDIO_RESET',
        'AUDIO_REFILL_READ', 'AUDIO_GET_PRODUCER', 'AUDIO_CAPTURE_CAPS', 'AUDIO_CAPTURE_START', 'AUDIO_CAPTURE_ACQUIRE',
        'AUDIO_CAPTURE_RELEASE', 'AUDIO_SET_SOURCE', 'AUDIO_MIDI_WRITE')},
    'AUDIO_GET_CAPS': ('own stream (audio fd)', 'tests/host/desktop/sound_runtime_test.c::AUDIO_GET_CAPS'),
    'AUDIO_CAPTURE_READ': ('exclusive capture owner', 'tests/host/audio/voice_test.inc::AUDIO_CAPTURE_READ'),
    'AUDIO_CAPTURE_STOP': ('exclusive capture owner', 'tests/host/audio/voice_test.inc::AUDIO_CAPTURE_STOP'),
    'MIXER_GET_MASTER': (ANY + ' (mixer fd)', AUD + '::MIXER_GET_MASTER'),
    'MIXER_GET_DIAGNOSTICS': (ANY + ' (mixer fd)', AUD + '::MIXER_GET_DIAGNOSTICS'),
    'MIXER_REFRESH_MASTER': (ANY + ' (mixer fd)', AUD + '::MIXER_REFRESH_MASTER'),
    'MIXER_SET_MASTER': ('designated desktop or SYSTEM with the control fd; session value not persisted to the machine (k04)', AUD + '::MIXER_SET_MASTER'),
    'MIXER_GET_LINE': (ANY + ' (mixer fd)', AUD + '::ciuki_mixer_line'),
    'MIXER_SET_LINE': ('designated desktop or SYSTEM with the control fd (k04)', AUD + '::ciuki_mixer_line'),
    'NET_GET_STATE': (ANY + ' (net fd, copied state)', FILES + '::NET_GET_STATE'),
    'NET_QUERY_DRIVER_PACK': (ANY + ' (net fd, copied qualification)', H13N + '::NET_QUERY_DRIVER_PACK'),
    **{n: ('own-session port owner (COM fd, generation)', 'tests/host/com_test.c::' + n) for n in (
        'COM_GET_OWNER', 'COM_GET_STATUS', 'COM_SET_LINE', 'COM_SET_MODEM')},
    'COM_GET_LINE': ('own-session port owner (COM fd, generation)', 'tests/host/dosvm_uart_seams.inc::COM_GET_LINE'),
    'COM_SET_MOUSE': (ADMIN + ' (machine input binding, k04)', 'tests/host/com_test.c::COM_SET_MOUSE'),
    'DISK_GET_INFO': (ANY + ' (copied geometry)', 'tests/host/libdisk_test.c::DISK_GET_INFO'),
    'DISK_LEASE_STATE': (ANY + ' (bounded lease state)', 'tests/host/proc/disk_test.c::DISK_LEASE_STATE'),
    'DISK_LEASE_ACQUIRE': (SETUP, DEV + '::DISK_LEASE_ACQUIRE'),
    'DISK_LEASE_RELEASE': ('current lease holder', DEV + '::DISK_LEASE_RELEASE'),
    'DISK_FLUSH': ('current lease holder', 'tests/host/proc/disk_test.c::DISK_FLUSH'),
    'DISK_PUBLISH': (SETUP, 'tests/host/proc/disk_test.c::DISK_PUBLISH'),
    'DISK_SET_PHASE': (SETUP, 'tests/host/fs/test_format.c::DISK_SET_PHASE'),
    'DISK_RECHECK': (SETUP, 'tests/host/libdisk_test.c::DISK_RECHECK'),
    'DISK_PARTITION_ACQUIRE': (ADMIN + ' on a non-system disk', DEV + '::DISK_PARTITION_ACQUIRE'),
    'INPUT_HID_GET_CAPS': (GRANT + ' (input owner)', 'tests/host/usb/hid_abi_test.c::INPUT_HID_GET_CAPS'),
    'INPUT_HID_GET_STATE': (GRANT + ' (input owner)', 'tests/host/usb/hid_abi_test.c::INPUT_HID_GET_STATE'),
    'INPUT_HID_CALIBRATE': (GRANT + '; persistent machine calibration unsupported for users', 'tests/host/security/input_devices_test.c::INPUT_HID_CALIBRATE'),
    'INPUT_JOY_CONTROL': (GRANT + '; persistent mutation EOPNOTSUPP for users', 'tests/host/security/input_devices_test.c::INPUT_JOY_CONTROL'),
    'INPUT_KEYBOARD_SET': (GRANT + ' (transient keyboard state)', DEV + '::INPUT_KEYBOARD_SET'),
    'LPT_GET_CONFIG': ('own-session port owner (LPT fd)', 'tests/host/lpt_test.c::lpt_devctl'),
    'LPT_GET_STATUS': ('own-session port owner (LPT fd)', FILES + '::LPT_GET_STATUS'),
    'LPT_SET_CONFIG': ('writable port owner', 'tests/host/lpt_test.c::LPT_SET_CONFIG'),
    'LPT_INIT': ('writable port owner', 'tests/host/lpt_test.c::lpt_devctl'),
    **{n: (ANY + ' (copied media inventory)', FILES + '::' + n) for n in (
        'MEDIA_GET_STATE', 'MEDIA_GET_VOLUME', 'MEDIA_GET_BLOCKERS', 'MEDIA_GET_LABEL', 'MEDIA_CD_POSITION')},
    'MEDIA_GET_SPACE': (ANY + ' (copied media inventory)', 'tests/host/fs/test_storage_namespace.c::MEDIA_GET_SPACE'),
    'MEDIA_GET_NTFS_STATUS': (ANY + ' (copied mount state)', DEV + '::MEDIA_GET_NTFS_STATUS'),
    'MEDIA_GET_FORMAT': (ANY + ' (format progress)', H13N + '::MEDIA_GET_FORMAT'),
    'MEDIA_GET_CONVERT': (ANY + ' (conversion progress)', H13N + '::MEDIA_GET_CONVERT'),
    'MEDIA_CD_STATE': (ANY + ' (copied state)', DEV + '::MEDIA_CD_STATE'),
    'MEDIA_CD_INFO': (ANY + ' (copied state)', 'tests/host/atapi_test.inc::MEDIA_CD_INFO'),
    'MEDIA_CD_TOC': (ANY + ' (copied state)', 'tests/host/atapi_test.inc::MEDIA_CD_TOC'),
    **{n: ('live local interactive credential or SYSTEM (Guest admitted)', 'tests/host/atapi_test.inc::' + n) for n in (
        'MEDIA_CD_READ_AUDIO', 'MEDIA_CD_TRAY', 'MEDIA_CD_PLAY', 'MEDIA_CD_PAUSE')},
    'MEDIA_SAFE_REMOVE': ('live local interactive credential or SYSTEM (Guest admitted)', DEV + '::MEDIA_SAFE_REMOVE'),
    'MEDIA_FLOPPY_LOAD': ('live local interactive credential or SYSTEM', 'tests/host/fs/test_storage_namespace.c::MEDIA_FLOPPY_LOAD'),
    'MEDIA_SET_LABEL': ('Guest denied; Users removable; administrator fixed/system', DEV + '::MEDIA_SET_LABEL'),
    'MEDIA_FORMAT': (ADMIN + '; system volume through the SYSTEM broker', DEV + '::MEDIA_FORMAT'),
    'MEDIA_CONVERT': (ADMIN + '; system volume through the SYSTEM broker', DEV + '::MEDIA_CONVERT'),
    'MEDIA_DEFRAG_ANALYZE': (ADMIN + '; system volume through the SYSTEM broker', DEV + '::MEDIA_DEFRAG_ANALYZE'),
    'MEDIA_DEFRAG_STEP': (ADMIN + '; system volume through the SYSTEM broker', DEV + '::MEDIA_DEFRAG_STEP'),
    'MEDIA_FLOPPY_READ': (ADMIN + ' (raw floppy snapshot)', DEV + '::MEDIA_FLOPPY_READ'),
    'MEDIA_FLOPPY_WRITE': (ADMIN + ' (raw floppy snapshot)', DEV + '::MEDIA_FLOPPY_WRITE'),
    'CIUKI_SETUP_FAT_QUERY': (SETUP + '; READ on the retained object', FILES + '::CIUKI_SETUP_FAT_QUERY'),
    'CIUKI_SETUP_FAT_ATTRIB': (SETUP + '; metadata on the retained object', FILES + '::CIUKI_SETUP_FAT_ATTRIB'),
    'CIUKI_SETUP_CLOCK_SET': (SETUP, 'tests/host/libsetup_test.c::CIUKI_SETUP_CLOCK_SET'),
    'CIUKI_SETUP_NET_QUERY': (SETUP, DEV + '::CIUKI_SETUP_NET_QUERY'),
    'DISPLAY_GET_MONITOR': (GRANT + ' (display fd)', 'tests/host/display_test.c::display_monitor_query'),
    'DISPLAY_GET_REFRESH': (GRANT + ' (display fd)', 'tests/host/display_test.c::display_refresh_query'),
}

# VFS: the access bits every adapter (native, Win32, DOS) asks policy_check
# for, and the label kinds. Unknown bits and kinds deny every subject (H13).
MAP['vfs-access'] = {
    'POLICY_READ': ('read data/list', POL + '::POLICY_READ'),
    'POLICY_EXEC': ('execute/map executable', 'tests/host/security/owner_names_test.c::POLICY_EXEC'),
    'POLICY_TRAVERSE': ('traverse a directory', 'tests/host/security/installer_store_test.c::POLICY_TRAVERSE'),
    'POLICY_WRITE': ('write/truncate data', POL + '::POLICY_WRITE'),
    'POLICY_ADD': ('create an entry in a directory', POL + '::POLICY_ADD'),
    'POLICY_REMOVE': ('delete an entry', POL + '::POLICY_REMOVE'),
    'POLICY_RENAME': ('rename/move an entry', POL + '::POLICY_RENAME'),
    'POLICY_META': ('attributes and times', POL + '::POLICY_META'),
}
MAP['vfs-kind'] = {
    'POLICY_PUBLIC': ('everyone reads; administrators write', H13N + '::POLICY_PUBLIC'),
    'POLICY_ROOT': ('volume root: Standard users add creator-owned entries (Q7); Guest none', H13N + '::POLICY_ROOT'),
    'POLICY_PROGRAM': ('everyone reads; administrators write', H13N + '::POLICY_PROGRAM'),
    'POLICY_SECRET': (SYSTEM, H13N + '::POLICY_SECRET'),
    'POLICY_SEALED': ('everyone reads; SYSTEM writes', H13N + '::POLICY_SEALED'),
    'POLICY_PROFILES': ('profile container: everyone lists, only SYSTEM changes it', H13N + '::POLICY_PROFILES'),
    'POLICY_PROFILE': ('owner and administrators', H13N + '::POLICY_PROFILE'),
    'POLICY_SHARED': ('Users and administrators; Guest read-only', H13N + '::POLICY_SHARED'),
    'POLICY_CREATOR': ('creator-owner full control, others read/execute (Q7)', H13N + '::POLICY_CREATOR'),
    'POLICY_REMOVABLE': ('Users read/write; Guest read-only', H13N + '::POLICY_REMOVABLE'),
    'POLICY_READONLY': ('read-only for everyone (EROFS)', H13N + '::POLICY_READONLY'),
    'POLICY_DENY': (DENY + ' (every subject, SYSTEM included)', H13N + '::POLICY_DENY'),
}

# ---- checks -----------------------------------------------------------------

def _reference(root, ref, selector):
    """"file" names the selector; "file::m1::m2" names every marker."""
    path, *markers = ref.split('::')
    file = Path(root) / path
    if not file.is_file():
        return f'{selector}: test file {path} does not exist'
    text = file.read_text(errors='replace')
    for marker in markers or [selector.split(':')[-1]]:
        if marker not in text:
            return f'{selector}: {path} does not contain "{marker}"'
    return None


def check(root, derived):
    problems = []
    for family, selectors in derived.items():
        table = MAP.get(family, {})
        seen = set()
        for s in selectors:
            if s in seen:
                problems.append(f'{family}: {s} derived twice')
            seen.add(s)
            if s not in table:
                problems.append(f'{family}: {s} has no policy/test mapping')
                continue
            policy, ref = table[s]
            if not policy:
                problems.append(f'{family}: {s} has no policy class')
            p = _reference(root, ref, s)
            if p:
                problems.append(f'{family}: {p}')
        for s in table:
            if s not in seen:
                problems.append(f'{family}: stale mapping {s} (no longer in the sources)')
    for family in MAP:
        if family not in derived:
            problems.append(f'{family}: mapped family not derived')
    return problems


def check_defaults(root):
    """Each dispatcher's default branch denies; kernel and server bounds agree."""
    r = lambda n: _read(root, n)
    need = [
        ('src/kernel/core/syscall.c', 'r = file_syscall(tf) ? (int32_t)tf->eax : desktop_fd_syscall(tf) ? (int32_t)tf->eax : proc_syscall(tf);',
         'native default routes file -> desktop fd -> process'),
        ('src/kernel/proc/syscalls_proc.c', 'default: return -ENOSYS;', 'proc_syscall default ENOSYS'),
        ('src/kernel/proc/security.c', 'if (a.operation < 1 || a.operation > 14) return -EINVAL;', 'call 101 range'),
        ('src/kernel/proc/security.c', 'if (input.link.service<1 || input.link.service>6) return -EINVAL;', 'call 101 service range'),
        ('src/kernel/proc/syscalls_sysvc.c', 'default: return 0;', 'call 99 SYSTEM services: unknown op not serviced'),
        ('src/kernel/proc/syscalls_recovery.c', 'if (op != CIUKI_RECOVERY_SET && op != CIUKI_RECOVERY_ENABLE) return -EINVAL;', 'call 99 remaining ops EINVAL'),
        ('src/kernel/fs/sysvc_jobs.c', 'default: return -EINVAL;', 'SETTING unknown'),
        ('src/kernel/fs/sysvc_jobs.c', 'if (job != CIUKI_SYSTEM_JOB_STARTUP_REPORT) return -EINVAL;', 'JOB unknown'),
        ('src/kernel/proc/syscalls_sysvc.c', '} else e = -EINVAL;', 'LOG unknown action'),
        ('src/kernel/proc/channel.c', 'if (!h[4] || h[4]>max_op[c->service]) return -EOPNOTSUPP;', 'channel max_op'),
        ('src/kernel/proc/syscalls_file.c', 'file_fd_object(p, (int32_t)tf->ebx) ? -ENOTTY : -EBADF;', 'devctl default'),
        ('src/kernel/drivers/media_dev.c', 'default: return -ENOTTY;', 'media devctl default'),
        ('src/kernel/drivers/net_dev.c', 'if (op != NET_GET_STATE) return -ENOTTY;', 'net devctl default'),
        ('src/kernel/fs/security_policy.c', 'if (!s || !access || (access&~POLICY_ALL)) return -EACCES;', 'VFS unknown access bits'),
        ('src/kernel/fs/security_policy.c', 'if (l->kind>POLICY_DENY) return -EACCES;', 'VFS unknown label kind'),
        ('apps/registry/broker.c', 'if(op!=RB_NATIVE_READ&&op!=RB_NATIVE_PAGE&&op!=RB_NATIVE_COMMIT)return -EOPNOTSUPP;', 'broker native unknown'),
        ('apps/registry/broker.c', '    return -EOPNOTSUPP;\n}\nint rb_exchange(', 'broker Win32 stream unknown'),
        ('apps/logon/coordinator.c', 'if (!l->operation || l->operation>LGN_OP_ENROLL_SOURCE) { (void)lgn_reply(g,i,-EOPNOTSUPP,NULL,0); return; }', 'LOGON unknown op'),
        ('apps/accounts/protocol.c', 'if(!op || (service==1?op>8:op>9)) return -EOPNOTSUPP;', 'ACCOUNT unknown op'),
    ]
    problems = [f'{n}: missing "{t[:70]}" ({why})' for n, t, why in need if t not in r(n)]
    # The kernel's max_op and each server's own bound name the same last operation.
    bounds = {s.split(':')[0]: 0 for s in channel(root)}
    for s in channel(root):
        name, _, op = s.partition(':')
        if op.isdigit():
            bounds[name] = max(bounds[name], int(op))
    lgn = int(re.search(r'#define LGN_OP_ENROLL_SOURCE (\d+)u', r('apps/logon/logon_proto.h')).group(1))
    owner_last = re.search(r'OWNER_RECOVER_ORPHANS, OWNER_CHECKPOINT_FLOOR', r('src/kernel/fs/owner_system.h'))
    if bounds.get('AUTH') != 8 or bounds.get('ACCOUNTS') != 9:
        problems.append(f'channel.c max_op AUTH/ACCOUNTS {bounds.get("AUTH")}/{bounds.get("ACCOUNTS")} differ from protocol.c 8/9')
    if bounds.get('LOGON') != lgn:
        problems.append(f'channel.c max_op LOGON {bounds.get("LOGON")} differs from LGN_OP_ENROLL_SOURCE {lgn}')
    if bounds.get('OWNER') != 14 or not owner_last:
        problems.append('channel.c max_op OWNER differs from the OWNER query table (14)')
    if len(call101(root)) != 14:
        problems.append(f'security_syscall serves {len(call101(root))} operations, its range check says 14')
    return problems
