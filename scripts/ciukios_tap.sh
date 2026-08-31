#!/usr/bin/env bash
set -euo pipefail

TAP_IF="${CIUKIOS_TAP_IF:-ciukios0}"
HOST_CIDR="${CIUKIOS_TAP_HOST_CIDR:-10.0.2.2/24}"
TAP_USER="${CIUKIOS_TAP_USER:-$(id -un)}"
UPLINK_IF="${CIUKIOS_TAP_UPLINK:-}"
RULE_COMMENT="ciukios-tap-${TAP_IF}"
FORWARD_STATE="/run/ciukios-tap-${TAP_IF}.ip-forward-was-disabled"

usage() {
  cat <<'TXT'
Usage: scripts/ciukios_tap.sh <up|up-nat|down|status>

Creates the isolated Linux TAP endpoint used to reach CiukiOS directly.
After "up", launch QEMU with QEMU_NETWORK_MODE=tap.  The host is 10.0.2.2
and CiukiOS is 10.0.2.15. NETSTART keeps incoming ping active independently
of FTPSRV. "up-nat" additionally enables guest Internet access through the
host's default uplink.

Overrides:
  CIUKIOS_TAP_IF         Interface name (default: ciukios0)
  CIUKIOS_TAP_HOST_CIDR  Host address/prefix (default: 10.0.2.2/24)
  CIUKIOS_TAP_USER       Interface owner (default: current user)
  CIUKIOS_TAP_UPLINK     Internet uplink for up-nat (default: route lookup)
TXT
}

if [[ ! "$TAP_IF" =~ ^[[:alnum:]_.:-]{1,15}$ ]]; then
  echo "[ciukios-tap] ERROR: invalid interface name: $TAP_IF" >&2
  exit 1
fi
if [[ ! "$HOST_CIDR" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}/[0-9]{1,2}$ ]]; then
  echo "[ciukios-tap] ERROR: invalid IPv4 CIDR: $HOST_CIDR" >&2
  exit 1
fi
for command_name in awk grep ip; do
  command -v "$command_name" >/dev/null 2>&1 \
    || { echo "[ciukios-tap] ERROR: missing command: $command_name" >&2; exit 1; }
done

is_tap() {
  ip -d link show dev "$TAP_IF" 2>/dev/null \
    | grep -Eq 'tun[[:space:]]+type[[:space:]]+tap'
}

run_root() {
  if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo -- "$@"
  else
    echo "[ciukios-tap] ERROR: root privileges are required (sudo not found)" >&2
    exit 1
  fi
}

ensure_tap_up() {
  if [[ ! -e "/sys/class/net/$TAP_IF" ]]; then
    run_root ip tuntap add dev "$TAP_IF" mode tap user "$TAP_USER"
    echo "[ciukios-tap] created $TAP_IF for $TAP_USER"
  elif ! is_tap; then
    echo "[ciukios-tap] ERROR: refusing to modify non-TAP interface: $TAP_IF" >&2
    exit 1
  fi
  run_root ip address replace "$HOST_CIDR" dev "$TAP_IF"
  run_root ip link set dev "$TAP_IF" up
}

resolve_network() {
  ip -4 route show dev "$TAP_IF" proto kernel scope link \
    | awk 'NR == 1 { print $1; exit }'
}

resolve_uplink() {
  if [[ -n "$UPLINK_IF" ]]; then
    printf '%s\n' "$UPLINK_IF"
    return
  fi
  ip -4 route show default | awk 'NR == 1 { for (i=1; i<=NF; i++) if ($i == "dev") { print $(i+1); exit } }'
}

require_iptables() {
  command -v iptables >/dev/null 2>&1 \
    || { echo "[ciukios-tap] ERROR: iptables is required for up-nat" >&2; exit 1; }
  command -v sysctl >/dev/null 2>&1 \
    || { echo "[ciukios-tap] ERROR: sysctl is required for up-nat" >&2; exit 1; }
  command -v install >/dev/null 2>&1 \
    || { echo "[ciukios-tap] ERROR: install is required for up-nat state tracking" >&2; exit 1; }
}

iptables_add_once() {
  local table="$1"
  shift
  if ! run_root iptables -w -t "$table" -C "$@" >/dev/null 2>&1; then
    run_root iptables -w -t "$table" -A "$@"
  fi
}

iptables_delete_if_present() {
  local table="$1"
  shift
  if run_root iptables -w -t "$table" -C "$@" >/dev/null 2>&1; then
    run_root iptables -w -t "$table" -D "$@"
  fi
}

enable_nat() {
  local network uplink
  require_iptables
  network="$(resolve_network)"
  uplink="$(resolve_uplink)"
  if [[ -z "$network" || -z "$uplink" || ! -e "/sys/class/net/$uplink" ]]; then
    echo "[ciukios-tap] ERROR: cannot resolve TAP subnet or Internet uplink" >&2
    exit 1
  fi
  if [[ "$(sysctl -n net.ipv4.ip_forward)" == "0" && ! -e "$FORWARD_STATE" ]]; then
    run_root install -m 600 /dev/null "$FORWARD_STATE"
  fi
  run_root sysctl -q -w net.ipv4.ip_forward=1
  iptables_add_once nat POSTROUTING -s "$network" -o "$uplink" \
    -m comment --comment "$RULE_COMMENT" -j MASQUERADE
  iptables_add_once filter FORWARD -i "$TAP_IF" -o "$uplink" -s "$network" \
    -m comment --comment "$RULE_COMMENT" -j ACCEPT
  iptables_add_once filter FORWARD -i "$uplink" -o "$TAP_IF" -d "$network" \
    -m conntrack --ctstate RELATED,ESTABLISHED \
    -m comment --comment "$RULE_COMMENT" -j ACCEPT
  echo "[ciukios-tap] Internet NAT enabled: subnet=$network uplink=$uplink"
}

disable_nat() {
  local network uplink
  command -v iptables >/dev/null 2>&1 || return 0
  [[ -e "/sys/class/net/$TAP_IF" ]] || return 0
  network="$(resolve_network)"
  uplink="$(resolve_uplink)"
  [[ -n "$network" && -n "$uplink" ]] || return 0
  iptables_delete_if_present nat POSTROUTING -s "$network" -o "$uplink" \
    -m comment --comment "$RULE_COMMENT" -j MASQUERADE
  iptables_delete_if_present filter FORWARD -i "$TAP_IF" -o "$uplink" -s "$network" \
    -m comment --comment "$RULE_COMMENT" -j ACCEPT
  iptables_delete_if_present filter FORWARD -i "$uplink" -o "$TAP_IF" -d "$network" \
    -m conntrack --ctstate RELATED,ESTABLISHED \
    -m comment --comment "$RULE_COMMENT" -j ACCEPT
  if [[ -e "$FORWARD_STATE" ]]; then
    run_root sysctl -q -w net.ipv4.ip_forward=0
    run_root rm -f -- "$FORWARD_STATE"
    echo "[ciukios-tap] restored net.ipv4.ip_forward=0"
  fi
}

case "${1:-}" in
  up)
    ensure_tap_up
    echo "[ciukios-tap] ready: $TAP_IF host=$HOST_CIDR guest=10.0.2.15"
    echo "[ciukios-tap] run: QEMU_NETWORK_MODE=tap bash scripts/build_run_full.sh"
    ;;
  up-nat)
    ensure_tap_up
    enable_nat
    echo "[ciukios-tap] ready: $TAP_IF host=$HOST_CIDR guest=10.0.2.15"
    echo "[ciukios-tap] run: bash scripts/build_run_full.sh --tap"
    ;;
  down)
    if [[ -e "/sys/class/net/$TAP_IF" ]]; then
      if ! is_tap; then
        echo "[ciukios-tap] ERROR: refusing to delete non-TAP interface: $TAP_IF" >&2
        exit 1
      fi
      disable_nat
      run_root ip link delete dev "$TAP_IF"
      echo "[ciukios-tap] removed $TAP_IF (recreate it with: $0 up)"
    else
      echo "[ciukios-tap] already absent: $TAP_IF"
    fi
    ;;
  status)
    if [[ ! -e "/sys/class/net/$TAP_IF" ]]; then
      echo "[ciukios-tap] absent: $TAP_IF"
      exit 1
    fi
    if ! is_tap; then
      echo "[ciukios-tap] ERROR: interface is not TAP: $TAP_IF" >&2
      exit 1
    fi
    ip -brief address show dev "$TAP_IF"
    if command -v iptables >/dev/null 2>&1 \
      && run_root iptables -w -t nat -S POSTROUTING 2>/dev/null | grep -Fq -- "$RULE_COMMENT"; then
      echo "[ciukios-tap] Internet NAT: enabled"
    else
      echo "[ciukios-tap] Internet NAT: disabled"
    fi
    ;;
  -h|--help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
