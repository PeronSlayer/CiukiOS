#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "Usage: $0 SOURCE_SYSTEM.INI SOURCE_MOUSE.INI OUTPUT_DIR" >&2
  exit 2
fi

source_system_ini="$1"
source_mouse_ini="$2"
output_dir="$3"
source_windows_dir="$(dirname -- "$source_system_ini")"
source_default_pif="$source_windows_dir/_DEFAULT.PIF"
source_dosprompt_pif="$source_windows_dir/DOSPRMPT.PIF"
audio_mode="${CIUKIOS_WINDOWS31_AUDIO_MODE:-vsbhda}"

case "$audio_mode" in
  vsbhda|stable|legacy) ;;
  *) echo "[win31-profile] ERROR: CIUKIOS_WINDOWS31_AUDIO_MODE must be vsbhda, stable or legacy" >&2; exit 2 ;;
esac

[[ -s "$source_system_ini" ]] \
  || { echo "[win31-profile] ERROR: missing SYSTEM.INI: $source_system_ini" >&2; exit 1; }
[[ -s "$source_mouse_ini" ]] \
  || { echo "[win31-profile] ERROR: missing MOUSE.INI: $source_mouse_ini" >&2; exit 1; }
[[ -s "$source_default_pif" ]] \
  || { echo "[win31-profile] ERROR: missing _DEFAULT.PIF: $source_default_pif" >&2; exit 1; }
[[ -s "$source_dosprompt_pif" ]] \
  || { echo "[win31-profile] ERROR: missing DOSPRMPT.PIF: $source_dosprompt_pif" >&2; exit 1; }

mkdir -p "$output_dir"
cp -- "$source_system_ini" "$output_dir/SYSTEM.INI"
cp -- "$source_mouse_ini" "$output_dir/MOUSE.INI"
cp -- "$source_default_pif" "$output_dir/_DEFAULT.PIF"
cp -- "$source_dosprompt_pif" "$output_dir/DOSPRMPT.PIF"

# A DOS executable without its own PIF inherits _DEFAULT.PIF.  The installed
# Windows tree fixed conventional memory at 640/128 KiB (limit/required) and
# both EMS and XMS maxima at 1024 KiB.  FFFF in a signed limit means "as much
# as available"; a zero requirement lets a VM start with its actual free
# conventional arena.  Windows can then grow EMS/XMS on demand, without a
# per-program profile.  Parse the extension directory rather than relying on
# fixed extension offsets so equivalent Windows 3.1 PIFs work too.
for generated_pif in "$output_dir/_DEFAULT.PIF" "$output_dir/DOSPRMPT.PIF"; do
  perl -0777pi -e '
    my $signature = "WINDOWS 386 3.0\x00";
    my $header = index($_, $signature);
    die "$ARGV has no WINDOWS 386 3.0 section\n" if $header < 0;
    die "$ARGV has a truncated section header\n" if length($_) < $header + 22;
    my $data = unpack("v", substr($_, $header + 18, 2));
    my $size = unpack("v", substr($_, $header + 20, 2));
    die "$ARGV has an invalid DATA386 section\n"
      if $size < 16 || $data + $size > length($_);
    substr($_, 0x20, 2, pack("v", 640));
    substr($_, 0x22, 2, pack("v", 0));
    substr($_, $data + 0, 2, pack("v", 0xffff));
    substr($_, $data + 2, 2, pack("v", 0));
    substr($_, $data + 8, 2, pack("v", 0xffff));
    substr($_, $data + 12, 2, pack("v", 0xffff));
  ' "$generated_pif"
done

# Patch copies only. The local installed tree remains untouched, and each
# inserted line reuses its anchor's original separator. This historical tree
# contains a valid mixture of CRLF and LF records accepted by Enhanced Mode.
perl -0pi -e '
  s{(?mi)^[ \t]*(?:ActiveAccelerationProfile|HorizontalSensitivity|VerticalSensitivity)[ \t]*=[^\r\n]*(?:\r?\n|\z)}{}g;
  my $count = s{(?mi)^(MouseType[ \t]*=)[^\r\n]*(\r?\n)}
    {$1 . "PS2" . $2
      . "ActiveAccelerationProfile=4" . $2
      . "HorizontalSensitivity=30" . $2
      . "VerticalSensitivity=30" . $2}e;
  die "MOUSE.INI has no MouseType anchor\n" unless $count == 1;
' "$output_dir/MOUSE.INI"

perl -0pi -e '
  s{(?msi)^[ \t]*\[(?:sndblst|adlib)\.drv\][^\r\n]*(?:\r?\n)(?:(?!^[ \t]*\[).)*(?=^[ \t]*\[|\z)}{}g;
  s{(?mi)^[ \t]*(?:device[ \t]*=[ \t]*(?:vsbd\.386|vadlibd\.386)|DMABufferSize[ \t]*=[^\r\n]*|wave[ \t]*=[^\r\n]*|midi[ \t]*=[^\r\n]*)(?:\r?\n|\z)}{}g;

  # Preserve the separator boundary used by the installed Enhanced Mode
  # configuration before the local LF-only tail begins.
  my $boundary = s{(?mi)^(device[ \t]*=[ \t]*\*vmcpd[ \t]*)\r?\n}{$1 . "\r\n"}e;
  die "SYSTEM.INI has no vmcpd boundary\n" unless $boundary == 1;
' "$output_dir/SYSTEM.INI"

# Keep the installed profile on the Windows 3.1 VGA driver.  A VBE BIOS is
# not sufficient proof that its protected-mode or linear-framebuffer path is
# safe: several period chipsets expose modes which hang when Windows switches
# into them.  The source-built VBE driver remains available through explicit
# VGASETUP profiles generated below, but a failed experiment can always be
# recovered by selecting SYSTEM.VGA from the CiukiOS shell.
perl -0pi -e '
  s{(?mi)^(386grabber[ \t]*=)[^\r\n]*}{$1 . "vga.3gr"}e;
  s{(?mi)^(display\.drv[ \t]*=)[^\r\n]*}{$1 . "vga.drv"}e;
  s{(?mi)^(display[ \t]*=)[^\r\n]*}{$1 . "*vddvga"}e;
  s{(?mi)^[ \t]*WindowUpdateTime[ \t]*=[^\r\n]*(?:\r?\n|\z)}{}g;
  s{(?msi)^[ \t]*\[vbesvga\.drv\][^\r\n]*(?:\r?\n)(?:(?!^[ \t]*\[).)*(?=^[ \t]*\[|\z)}{}g;
  s{(?mi)^[ \t]*dci[ \t]*=[^\r\n]*(?:\r?\n|\z)}{}g;
' "$output_dir/SYSTEM.INI"

if [[ "$audio_mode" == "legacy" ]]; then
perl -0pi -e '
  my $devices = s{(?mi)^(device[ \t]*=[ \t]*\*cdpscsi[ \t]*)(\r?\n)}
    {$1 . $2 . "device=vsbd.386" . $2 . "device=vadlibd.386" . $2}e;
  die "SYSTEM.INI has no cdpscsi device anchor\n" unless $devices == 1;

  my $dma = s{(?mi)^(MaxPagingFileSize[ \t]*=[^\r\n]*)(\r?\n)}
    {$1 . $2 . "DMABufferSize=64" . $2}e;
  die "SYSTEM.INI has no paging-size anchor\n" unless $dma == 1;

  my $drivers = s{(?mi)^(midimapper[ \t]*=[ \t]*midimap\.drv[ \t]*)(\r?\n)}
    {$1 . $2 . "wave=sndblst2.drv" . $2 . "midi=msadlib.drv" . $2}e;
  die "SYSTEM.INI has no multimedia-driver anchor\n" unless $drivers == 1;

  my ($eol) = /(?mi)^\[drivers\](\r?\n)/;
  die "SYSTEM.INI has no drivers section\n" unless defined $eol;
  s{(?:\r?\n)+\z}{};
  $_ .= $eol . $eol
      . "[sndblst.drv]" . $eol
      . "port=220" . $eol
      . "int=7" . $eol
      . "dmachannel=1" . $eol
      . "verifyint=0" . $eol
      . "nowarning=1" . $eol . $eol
      . "[adlib.drv]" . $eol
      . "port=388" . $eol;
' "$output_dir/SYSTEM.INI"
elif [[ "$audio_mode" == "vsbhda" ]]; then
perl -0pi -e '
  # VSBHDA16 supplies the SB/AdLib ports while Windows is a Standard Mode
  # child. Do not load VSBD.386/VADLIBD.386: those Enhanced Mode VxDs would
  # install a second virtualizer over HDPMI and are the crash-prone path.
  my $drivers = s{(?mi)^(midimapper[ \t]*=[ \t]*midimap\.drv[ \t]*)(\r?\n)}
    {$1 . $2 . "wave=sndblst2.drv" . $2 . "midi=msadlib.drv" . $2}e;
  die "SYSTEM.INI has no multimedia-driver anchor\n" unless $drivers == 1;

  my ($eol) = /(?mi)^\[drivers\](\r?\n)/;
  die "SYSTEM.INI has no drivers section\n" unless defined $eol;
  s{(?:\r?\n)+\z}{};
  $_ .= $eol . $eol
      . "[sndblst.drv]" . $eol
      . "port=220" . $eol
      . "int=7" . $eol
      . "dmachannel=1" . $eol
      . "verifyint=0" . $eol
      . "nowarning=1" . $eol . $eol
      . "[adlib.drv]" . $eol
      . "port=388" . $eol;
' "$output_dir/SYSTEM.INI"
else
perl -0pi -e '
  my $driver = s{(?mi)^(midimapper[ \t]*=[ \t]*midimap\.drv[ \t]*)(\r?\n)}
    {$1 . $2 . "wave=speaker.drv" . $2}e;
  die "SYSTEM.INI has no multimedia-driver anchor\n" unless $driver == 1;

  my ($eol) = /(?mi)^\[drivers\](\r?\n)/;
  die "SYSTEM.INI has no drivers section\n" unless defined $eol;
  s{(?:\r?\n)+\z}{};
  $_ .= $eol . $eol
      . "[speaker.drv]" . $eol
      . "CPU Speed=300" . $eol
      . "Volume=500" . $eol
      . "Version=774" . $eol
      . "Enhanced=1" . $eol
      . "Max seconds=3" . $eol
      . "Leave interrupts enabled=1" . $eol;
' "$output_dir/SYSTEM.INI"
fi

# Preserve a byte-for-byte recovery profile and derive two conservative VBE
# profiles from it. Prefer the linear framebuffer so the driver can retain
# and double-buffer pixels across GDI resizes. The native VGA profile remains
# available through VGASETUP SAFE when a BIOS cannot support this path.
cp -- "$output_dir/SYSTEM.INI" "$output_dir/SYSTEM.VGA"

generate_vbe_profile() {
  local width="$1"
  local height="$2"
  local target="$3"
  cp -- "$output_dir/SYSTEM.VGA" "$target"
  CIUKIOS_VBE_WIDTH="$width" CIUKIOS_VBE_HEIGHT="$height" perl -0pi -e '
    s{(?mi)^(386grabber[ \t]*=)[^\r\n]*}{$1 . "vbevmdib.3gr"}e;
    s{(?mi)^(display\.drv[ \t]*=)[^\r\n]*}{$1 . "vbesvga.drv"}e;
    s{(?mi)^(display[ \t]*=)[^\r\n]*}{$1 . "vddvbe.386"}e;
    my ($eol) = /(?mi)^\[boot\][ \t]*(\r?\n)/;
    die "SYSTEM.INI has no boot section\n" unless defined $eol;
    my $timer = s{(?mi)^(device[ \t]*=[ \t]*\*vmcpd[ \t]*)(\r?\n)}
      {$1 . $2 . "WindowUpdateTime=15" . $2}e;
    die "SYSTEM.INI has no vmcpd video-timing anchor\n" unless $timer == 1;
    my $dci = s{(?mi)^(\[drivers\][ \t]*)(\r?\n)}
      {$1 . $2 . "dci=display" . $2}e;
    die "SYSTEM.INI has no drivers section\n" unless $dci == 1;
    s{(?:\r?\n)+\z}{};
    $_ .= $eol . $eol
        . "[VBESVGA.DRV]" . $eol
        . "Width=$ENV{CIUKIOS_VBE_WIDTH}" . $eol
        . "Height=$ENV{CIUKIOS_VBE_HEIGHT}" . $eol
        . "Depth=8" . $eol
        . "fontsize=small" . $eol
        . "SwapBuffersInterval=16" . $eol
        . "PreferBankedModes=0" . $eol
        . "Allow3ByteMode=0" . $eol
        . "BounceOnModeset=1" . $eol;
  ' "$target"
}

generate_vbe_profile 800 600 "$output_dir/SYSTEM.800"
generate_vbe_profile 1024 768 "$output_dir/SYSTEM.102"

for expected in \
  '386grabber=vga.3gr' \
  'display.drv=vga.drv' \
  'display=*vddvga'; do
  tr -d '\r' < "$output_dir/SYSTEM.INI" | grep -Fxq -- "$expected" \
    || { echo "[win31-profile] ERROR: missing safe VGA setting: $expected" >&2; exit 1; }
done

if tr -d '\r' < "$output_dir/SYSTEM.INI" \
    | grep -Eiq '^(display\.drv=vbesvga\.drv|display=vddvbe\.386|\[VBESVGA\.DRV\])'; then
  echo "[win31-profile] ERROR: VBE setting leaked into default safe profile" >&2
  exit 1
fi

for profile_spec in 'SYSTEM.800:800:600' 'SYSTEM.102:1024:768'; do
  IFS=: read -r profile_name profile_width profile_height <<< "$profile_spec"
  for expected in \
    '386grabber=vbevmdib.3gr' 'display.drv=vbesvga.drv' \
    'display=vddvbe.386' '[VBESVGA.DRV]' \
    "Width=$profile_width" "Height=$profile_height" \
    'Depth=8' 'PreferBankedModes=0' 'SwapBuffersInterval=16' 'Allow3ByteMode=0'; do
    tr -d '\r' < "$output_dir/$profile_name" | grep -Fxq -- "$expected" \
      || { echo "[win31-profile] ERROR: $profile_name missing $expected" >&2; exit 1; }
  done
done

for expected in \
  'ActiveAccelerationProfile=4' \
  'HorizontalSensitivity=30' \
  'VerticalSensitivity=30'; do
  tr -d '\r' < "$output_dir/MOUSE.INI" | grep -Fxq -- "$expected" \
    || { echo "[win31-profile] ERROR: missing generated mouse setting: $expected" >&2; exit 1; }
done

if [[ "$audio_mode" == "legacy" ]]; then
  for expected in \
    'device=vsbd.386' 'device=vadlibd.386' \
    'wave=sndblst2.drv' 'midi=msadlib.drv' \
    'port=220' 'int=7' 'dmachannel=1' 'verifyint=0' 'port=388'; do
    tr -d '\r' < "$output_dir/SYSTEM.INI" | grep -Fxq -- "$expected" \
      || { echo "[win31-profile] ERROR: missing generated audio setting: $expected" >&2; exit 1; }
  done
elif [[ "$audio_mode" == "vsbhda" ]]; then
  for expected in \
    'wave=sndblst2.drv' 'midi=msadlib.drv' \
    '[sndblst.drv]' 'port=220' 'int=7' 'dmachannel=1' 'verifyint=0' \
    '[adlib.drv]' 'port=388'; do
    tr -d '\r' < "$output_dir/SYSTEM.INI" | grep -Fxq -- "$expected" \
      || { echo "[win31-profile] ERROR: missing VSBHDA setting: $expected" >&2; exit 1; }
  done
  if tr -d '\r' < "$output_dir/SYSTEM.INI" \
      | grep -Eiq '^(device=(vsbd|vadlibd)\.386|DMABufferSize=)'; then
    echo "[win31-profile] ERROR: Enhanced Mode audio VxD leaked into VSBHDA profile" >&2
    exit 1
  fi
else
  for expected in \
    'wave=speaker.drv' '[speaker.drv]' \
    'CPU Speed=300' 'Enhanced=1' 'Max seconds=3' 'Leave interrupts enabled=1'; do
    tr -d '\r' < "$output_dir/SYSTEM.INI" | grep -Fxq -- "$expected" \
      || { echo "[win31-profile] ERROR: missing stable speaker setting: $expected" >&2; exit 1; }
  done
  if tr -d '\r' < "$output_dir/SYSTEM.INI" \
      | grep -Eiq '^(device=(vsbd|vadlibd)\.386|wave=sndblst2\.drv|midi=msadlib\.drv|DMABufferSize=)'; then
    echo "[win31-profile] ERROR: unstable legacy audio setting survived stable profile" >&2
    exit 1
  fi
fi

for generated_pif in "$output_dir/_DEFAULT.PIF" "$output_dir/DOSPRMPT.PIF"; do
  perl -0e '
    local $/;
    $_ = <>;
    my $header = index($_, "WINDOWS 386 3.0\x00");
    die "$ARGV has no WINDOWS 386 3.0 section\n" if $header < 0;
    my $data = unpack("v", substr($_, $header + 18, 2));
    my ($max_conv, $req_conv) = unpack("v2", substr($_, $data, 4));
    my ($max_ems, $req_ems, $max_xms, $req_xms) =
      unpack("v4", substr($_, $data + 8, 8));
    die "$ARGV still limits DOS memory (conv=$max_conv/$req_conv "
      . "EMS=$max_ems/$req_ems XMS=$max_xms/$req_xms KiB)\n"
      unless $max_conv == 0xffff && $req_conv == 0
        && $max_ems == 0xffff && $req_ems == 0
        && $max_xms == 0xffff && $req_xms == 0;
  ' "$generated_pif"
done

if [[ "$audio_mode" == "legacy" ]]; then
  echo "[win31-profile] configured safe VGA default, optional buffered VBE, linear PS/2, native SB/AdLib and dynamic DOS memory"
elif [[ "$audio_mode" == "vsbhda" ]]; then
  echo "[win31-profile] configured safe VGA default, Standard Mode transient VSBHDA16 SB/AdLib and dynamic DOS memory"
else
  echo "[win31-profile] configured safe VGA default, optional buffered VBE, linear PS/2, interrupt-safe PC-speaker WAV fallback and dynamic DOS memory"
fi
