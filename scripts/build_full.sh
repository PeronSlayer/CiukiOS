#!/usr/bin/env bash
set -euo pipefail

# Default CIUKIOS_ROOT to the repository root (parent of scripts/) when not set externally.
: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if [[ "$(uname -s)" == "Darwin" ]]; then
	# Allow direct invocation on macOS without going through the wrapper entrypoint.
	source "$(cd "$(dirname "${BASH_SOURCE[0]}")/macos" && pwd)/common.sh"
	ciuk_macos_prepare_tools
	ciuk_macos_check_required
	cd "$CIUKIOS_ROOT"
fi

mkdir -p build/full
mkdir -p build/full/obj

BOOT_SRC="src/boot/full_boot.asm"
BOOT_BIN="build/full/obj/full_boot.bin"
STAGE1_SRC="src/boot/full_stage1_loader.asm"
STAGE1_BIN="build/full/obj/full_stage1.bin"
STAGE1_SLOT_BIN="build/full/obj/full_stage1_slot.bin"
STAGE1_LST="build/full/obj/full_stage1.lst"
STAGE2_SRC="src/boot/full_stage2.asm"
STAGE2_BIN="build/full/obj/full_stage2.bin"
STAGE2_MAX_SIZE=512
RUNTIME_SRC="src/runtime/ciukidos.asm"
RUNTIME_BIN="build/full/obj/ciukidos.sys"
# The loader validates CIUKIDOS at 0x0900 and relocates its position-independent
# image to 0x0300.  The bounded image may use the complete interval up to, but
# never including, SYSVARS at 0x0D90.
RUNTIME_MAX_SIZE=$((0xA900))
CIUKIDOS_RUNTIME_SEG=0x0300
CIUKIDOS_ABI_VERSION=2

IMG="${CIUKIOS_FULL_IMG:-build/full/ciukios-full.img}"
TOTAL_SECTORS="${CIUKIOS_FULL_TOTAL_SECTORS:-262144}"
STAGE1_SECTORS=72
STAGE1_SLOT_SIZE=$((STAGE1_SECTORS * 512))
BOOT_LBA_OFFSET="${CIUKIOS_FULL_BOOT_LBA_OFFSET:-0}"
FAT_LBA_OFFSET="${CIUKIOS_FULL_FAT_LBA_OFFSET:-0}"

FAT_SPT=63
FAT_HEADS=16
FAT_SECTORS_PER_CLUSTER=8
FAT_RESERVED_SECTORS=$((1 + STAGE1_SECTORS))
FAT_SECTORS_PER_FAT=128
FAT_COUNT=2
ROOT_ENTRIES=512
ROOT_DIR_SECTORS=$((ROOT_ENTRIES * 32 / 512))
FAT1_LBA=$FAT_RESERVED_SECTORS
FAT2_LBA=$((FAT1_LBA + FAT_SECTORS_PER_FAT))
ROOT_LBA=$((FAT2_LBA + FAT_SECTORS_PER_FAT))
DATA_LBA=$((ROOT_LBA + ROOT_DIR_SECTORS))

COMDEMO_SRC="src/com/comdemo.asm"
COMDEMO_BIN="build/full/obj/comdemo.com"
CIUKRTST_SRC="src/com/ciukrtst.asm"
CIUKRTST_BIN="build/full/obj/ciukrtst.com"
MOUSECB_SRC="src/com/mousecb.asm"
MOUSECB_BIN="build/full/obj/mousecb.com"
CIUKPST_SRC="src/com/ciukpstk.asm"
CIUKPST_BIN="build/full/obj/ciukpst.com"
CIUKPTRM_SRC="src/com/ciukptrm.asm"
CIUKPTRM_BIN="build/full/obj/ciuktrm.exe"
CIUKPCOM_SRC="src/com/ciukpcom.asm"
CIUKPCOM_BIN="build/full/obj/ciukpcom.com"
MZDEMO_SRC="src/com/mzdemo.asm"
MZDEMO_BIN="build/full/obj/mzdemo.exe"
FILEIO_SRC="src/com/fileio.bin.asm"
FILEIO_BIN="build/full/obj/fileio.bin"
DELTEST_SRC="src/com/deltest.bin.asm"
DELTEST_BIN="build/full/obj/deltest.bin"
CIUKEDIT_SRC="src/com/ciukedit.asm"
CIUKEDIT_BIN="build/full/obj/ciukedit.com"
GFXRECT_SRC="src/com/gfxrect.asm"
GFXRECT_BIN="build/full/obj/gfxrect.com"
GFXRECT_MAX_SIZE=1024
GFXSTAR_SRC="src/com/gfxstar.asm"
GFXSTAR_BIN="build/full/obj/gfxstar.com"
GFXSTAR_MAX_SIZE=1024
VIDLEAVE_SRC="src/com/vidleave.asm"
VIDLEAVE_BIN="build/full/obj/vidleave.com"
MOUSE_SRC="src/com/mouse.asm"
MOUSE_BIN="build/full/obj/mouse.com"
CIUKWIN_SRC="src/com/ciukwin.asm"
CIUKWIN_BIN="build/full/obj/ciukwin.com"
CIUKWIN_MAX_SIZE=4096
SETUP_SRC="src/com/setup.asm"
SETUP_BIN="build/full/obj/setup.com"
# Match setup.asm's explicit COM code/data/stack bound. The FAT chain and all
# following payload positions are calculated from the actual file length.
SETUP_MAX_SIZE=$((0xEF00))
SETUP_MAX_CLUSTERS=$(((SETUP_MAX_SIZE + FAT_SECTORS_PER_CLUSTER * 512 - 1) / (FAT_SECTORS_PER_CLUSTER * 512)))
SETUP_MANIFEST_BIN="build/full/obj/setup.mft"
LBA32_TEST_BIN="build/full/obj/lba32.txt"
FORMAT_SRC="src/com/format.asm"
FORMAT_BIN="build/full/obj/format.com"
FORMAT_MAX_CLUSTERS=2
FORMAT_MAX_SIZE=$((FAT_SECTORS_PER_CLUSTER * 512 * FORMAT_MAX_CLUSTERS))
COMMAND_COMPAT_BIN="build/full/obj/command.com"
COMMAND_COMPAT_IMAGE_PATH="${CIUKIOS_COMMAND_COM_IMAGE_PATH:-::COMMAND.COM}"
SHELL_SRC="src/com/shell.asm"
SHELL_BIN="build/full/obj/shell.com"
IPCONFIG_SRC="src/com/ipconfig.asm"
IPCONFIG_BIN="build/full/obj/ipconfig.com"
ICMPD_SRC="src/com/icmpd.asm"
ICMPD_BIN="build/full/obj/icmpd.com"
NETCFG_SRC="src/com/netcfg.asm"
NETCFG_BIN="build/full/obj/netcfg.com"
DRVLOAD_SRC="src/com/drvload.asm"
DRVLOAD_BIN="build/full/obj/drvload.com"
SB16INIT_SRC="src/com/sb16init.asm"
SB16INIT_BIN="build/full/obj/sb16init.com"
AUDIOTST_SRC="src/com/audiotst.asm"
AUDIOTST_BIN="build/full/obj/audiotst.com"
AC97INIT_SRC="src/com/ac97init.asm"
AC97INIT_BIN="build/full/obj/ac97init.com"
AUDIOAUTO_SRC="src/com/audioauto.asm"
AUDIOAUTO_BIN="build/full/obj/audio.com"
AUDIOKEY_SRC="src/com/audiokey.asm"
AUDIOKEY_BIN="build/full/obj/audiokey.com"
SBEMINIT_SRC="src/com/sbeminit.asm"
SBEMINIT_BIN="build/full/obj/sbeminit.com"
DOOMSB_SRC="src/com/doomsb.asm"
DOOMSB_BIN="build/full/obj/doomsb.com"
PMIRQSB_LAUNCH_SRC="src/com/pmirqsb_launch.asm"
PMIRQSB_LAUNCH_BIN="build/full/obj/pmirqsb.com"
PMIRQSB_SRC="src/probes/pmirqsb/pmirqsb.c"
PMIRQSB_BIN="build/full/obj/pmirqsb.le"
DOOMSFX_LAUNCH_SRC="src/com/doomsfx_launch.asm"
DOOMSFX_LAUNCH_BIN="build/full/obj/doomsfx.com"
DOOMSFX_SRC="src/probes/doomsfx/doomsfx.c"
DOOMSFX_BIN="build/full/obj/doomsfx.le"
DOOMVAN_LAUNCH_SRC="src/com/doomvan_launch.asm"
DOOMVAN_LAUNCH_BIN="build/full/obj/doomvan.com"
DOOMVAN_LAUNCH_MZ="build/full/obj/doomvan.exe"
DOOM_LAUNCH_SRC="src/com/doom_launch.asm"
DOOM_LAUNCH_BIN="build/full/obj/doom.com"
DOOM_LAUNCH_MZ="build/full/obj/doom.exe"
WOLF3D_LAUNCH_SRC="src/com/wolf3d_launch.asm"
WOLF3D_LAUNCH_BIN="build/full/obj/wolf3d.com"
VGASETUP_SRC="src/com/vgasetup.asm"
VGASETUP_BIN="build/full/obj/vgasetup.com"
# DOS VBE helpers of the pinned vbesvga.drv release (not its Win16 driver):
# AUXSTACK (a larger stack for the video BIOS, run by the desktop at start),
# AUXCHECK, VIDMODES and MODETEST (used by VGASETUP).
VBESVGA_BUILD_SCRIPT="$CIUKIOS_ROOT/scripts/build_vbesvga_driver.sh"
VBESVGA_OUTPUT_DIR="${CIUKIOS_VBESVGA_OUTPUT_DIR:-$CIUKIOS_ROOT/build/external/video-compat/output}"
VBE_DOS_HELPERS=(AUXSTACK.COM AUXCHECK.COM VIDMODES.COM MODETEST.COM VBESVGA.TXT SOURCE.TXT)
DOS4GW_BIN="${DOS4GW_BIN:-/opt/watcom/binw/dos4gw.exe}"
SPLASH_SRC="misc/CiukiOS_SplashScreen.png"
SPLASH_TOOL="scripts/generate_splash_asset.py"
SPLASH_BIN="build/full/obj/SPLASH.BIN"
SPLASH_MAX_SIZE=49920
SPLASH_EXPECTED_SIZE=49920
DOOM_SRC_DIR="${CIUKIOS_DOOM_SRC_DIR:-$CIUKIOS_ROOT/third_party/Doom}"
DOOM_IMAGE_DIR="${CIUKIOS_DOOM_IMAGE_DIR:-::APPS/DOOM}"
DOOMDATA_IMAGE_DIR="${CIUKIOS_DOOMDATA_IMAGE_DIR:-::DOOMDATA}"
DOOM_AUDIO_PROFILE="${CIUKIOS_DOOM_AUDIO_PROFILE:-sb16}"
DOOMVAN_EXE_OVERRIDDEN="${CIUKIOS_DOOMVAN_EXE+x}"
DOOMVAN_SRC_DIR="${CIUKIOS_DOOMVAN_SRC_DIR:-$CIUKIOS_ROOT/build/external/doom-vanille}"
DOOMVAN_WATCOM_ROOT="${WATCOM:-/opt/watcom}"
DOOMVAN_EXE="${CIUKIOS_DOOMVAN_EXE:-$CIUKIOS_ROOT/build/external/doom-vanille/pcdoom.exe}"
DOOMVAN_IMAGE_DIR="${CIUKIOS_DOOMVAN_IMAGE_DIR:-::APPS/DOOMVAN}"
DOSNAV_SRC_DIR="${CIUKIOS_DOSNAV_SRC_DIR:-$CIUKIOS_ROOT/third_party/DOSNavigator}"
DOSNAV_IMAGE_DIR="${CIUKIOS_DOSNAV_IMAGE_DIR:-::APPS/DOSNAV}"
WOLF3D_SRC_DIR="${CIUKIOS_WOLF3D_SRC_DIR:-$CIUKIOS_ROOT/third_party/WOLF3D}"
WOLF3D_IMAGE_DIR="${CIUKIOS_WOLF3D_IMAGE_DIR:-::APPS/WOLF3D}"
WOLF4GW_BUILD_SCRIPT="$CIUKIOS_ROOT/scripts/build_wolf4gw.sh"
WOLF4GW_OUTPUT_DIR="${CIUKIOS_WOLF4GW_OUTPUT_DIR:-$CIUKIOS_ROOT/build/external/audio-compat/output}"
WOLF4GW_EXE="$WOLF4GW_OUTPUT_DIR/WOLF4GW.EXE"
WOLF4GW_NOTICE="$WOLF4GW_OUTPUT_DIR/WOLF4GW.TXT"
WOLF4GW_MODE="${CIUKIOS_WOLF4GW_MODE:-build}"
COSTA_SRC_DIR="${CIUKIOS_COSTA_SRC_DIR:-$CIUKIOS_ROOT/build/external/costa/v1.8.0}"
COSTA_IMAGE_DIR="${CIUKIOS_COSTA_IMAGE_DIR:-::APPS/COSTA}"
NETWORK_SRC_DIR="${CIUKIOS_NETWORK_SRC_DIR:-$CIUKIOS_ROOT/build/external/network/mtcp-2025-01-10_crynwr-2006-09-02c}"
NETWORK_CONFIG_DIR="${CIUKIOS_NETWORK_CONFIG_DIR:-$CIUKIOS_ROOT/config/network}"
NETWORK_IMAGE_DIR="${CIUKIOS_NETWORK_IMAGE_DIR:-::NET}"
NETWORK_SHARE_IMAGE_DIR="${CIUKIOS_NETWORK_SHARE_IMAGE_DIR:-::SHARE}"
DRIVERS_SRC_DIR="${CIUKIOS_DRIVERS_SRC_DIR:-$CIUKIOS_ROOT/third_party/drivers}"
DRIVERS_IMAGE_DIR="${CIUKIOS_DRIVERS_IMAGE_DIR:-::SYSTEM/DRIVERS}"
DRIVERS_VERIFY_SCRIPT="$CIUKIOS_ROOT/scripts/verify_full_drivers_payload.sh"
CIUKIOS_ALLOW_MISSING_DRIVERS="${CIUKIOS_ALLOW_MISSING_DRIVERS:-0}"
SBEMU_BUILD_SCRIPT="$CIUKIOS_ROOT/scripts/build_vsbhda_fallback.sh"
SBEMU_OUTPUT_DIR="${CIUKIOS_SBEMU_OUTPUT_DIR:-$CIUKIOS_ROOT/build/external/audio-compat/output}"
SBEMU_IMAGE_DIR="${CIUKIOS_SBEMU_IMAGE_DIR:-::SBEMU}"
SBEMU_MODE="${CIUKIOS_SBEMU_MODE:-build}"
CTMOUSE_BIN="${CIUKIOS_CTMOUSE_BIN:-$CIUKIOS_ROOT/assets/drivers/ctmouse/CTMOUSE.EXE}"
CTMOUSE_LICENSE="${CIUKIOS_CTMOUSE_LICENSE:-$CIUKIOS_ROOT/assets/drivers/ctmouse/COPYING}"
STAGE1_SELFTEST_AUTORUN="${CIUKIOS_STAGE1_SELFTEST_AUTORUN:-0}"
STAGE1_RUNTIME_PROBE="${CIUKIOS_STAGE1_RUNTIME_PROBE:-0}"
STAGE1_DEBUG_COMMANDS="${CIUKIOS_STAGE1_DEBUG_COMMANDS:-0}"
TRACE_CHILD_INT21="${CIUKIOS_TRACE_CHILD_INT21:-0}"
TRACE_WIN_INT2F="${CIUKIOS_TRACE_WIN_INT2F:-0}"
TRACE_WIN_MEMORY="${CIUKIOS_TRACE_WIN_MEMORY:-0}"
TRACE_WIN_XMS="${CIUKIOS_TRACE_WIN_XMS:-0}"
STAGE1_BOOT_EXTERNAL_SHELL="${CIUKIOS_STAGE1_BOOT_EXTERNAL_SHELL:-1}"
# Full profile is loader-only: disabling external SHELL.COM exercises the
# deterministic loader-fatal path; no interactive Stage1 fallback is built.
STAGE2_AUTORUN="${CIUKIOS_STAGE2_AUTORUN:-0}"
HARDWARE_VALIDATION_SCREEN="${CIUKIOS_HARDWARE_VALIDATION_SCREEN:-0}"
SETUP_RAW_HDD_DESTRUCTIVE="${CIUKIOS_SETUP_RAW_HDD_DESTRUCTIVE:-0}"
SETUP_LIVE_CD_MODE="${CIUKIOS_SETUP_LIVE_CD_MODE:-0}"
SETUP_FORCE_MEMDISK_SOURCE="${CIUKIOS_SETUP_FORCE_MEMDISK_SOURCE:-0}"
DOS_DEFAULT_DRIVE_INDEX="${CIUKIOS_DOS_DEFAULT_DRIVE_INDEX:-2}"
ENABLE_PS2_MOUSE_INIT="${CIUKIOS_ENABLE_PS2_MOUSE_INIT:-1}"
WOLF_RUNTIME_DIAG="${CIUKIOS_WOLF_RUNTIME_DIAG:-0}"
MTOOLS_TIMEOUT_SEC="${MTOOLS_TIMEOUT_SEC:-20}"
MTOOLS_KILL_AFTER_SEC="${MTOOLS_KILL_AFTER_SEC:-2}"
SETUP_RAW_HDD_INSTALL="${CIUKIOS_SETUP_RAW_HDD_INSTALL:-0}"
SETUP_ATAPI_MIRROR_BLOCKS="${CIUKIOS_SETUP_ATAPI_MIRROR_BLOCKS:-0}"

mtools_ensure_dir() {
	local image="$1"
	local dir_path="$2"
	local out rc

	if mdir -i "$image" "$dir_path" >/dev/null 2>&1; then
		echo "[build-full] mtools mkdir: already exists (verified): $dir_path"
		return 0
	fi

	set +e
	out="$(timeout --kill-after="${MTOOLS_KILL_AFTER_SEC}s" "${MTOOLS_TIMEOUT_SEC}s" mmd -i "$image" "$dir_path" 2>&1)"
	rc=$?
	set -e

	if [[ $rc -eq 0 ]]; then
		echo "[build-full] mtools mkdir: created: $dir_path"
		return 0
	fi

	# Benign case: command returned non-zero but directory is confirmed present.
	if mdir -i "$image" "$dir_path" >/dev/null 2>&1; then
		echo "[build-full] mtools mkdir: non-zero rc=$rc but directory is present (verified): $dir_path"
		if [[ -n "$out" ]]; then
			echo "[build-full] mtools mkdir detail: $out"
		fi
		return 0
	fi

	if [[ $rc -eq 124 ]]; then
		echo "[build-full] ERROR: mtools mkdir timed out creating $dir_path (timeout=${MTOOLS_TIMEOUT_SEC}s kill-after=${MTOOLS_KILL_AFTER_SEC}s)" >&2
	else
		echo "[build-full] ERROR: mtools mkdir failed creating $dir_path (rc=$rc)" >&2
	fi
	if [[ -n "$out" ]]; then
		echo "[build-full] mtools mkdir output: $out" >&2
	fi
	exit 1
}



for f in "$BOOT_SRC" "$STAGE1_SRC" "$STAGE2_SRC" "$RUNTIME_SRC" "$COMDEMO_SRC" "$CIUKRTST_SRC" "$MOUSECB_SRC" "$CIUKPST_SRC" "$CIUKPTRM_SRC" "$CIUKPCOM_SRC" "$MZDEMO_SRC" "$FILEIO_SRC" "$DELTEST_SRC" "$CIUKEDIT_SRC" "$GFXRECT_SRC" "$GFXSTAR_SRC" "$VIDLEAVE_SRC" "$MOUSE_SRC" "$CIUKWIN_SRC" "$SETUP_SRC" "$FORMAT_SRC" "$SHELL_SRC" "$DRVLOAD_SRC" "$SB16INIT_SRC" "$AUDIOTST_SRC" "$AC97INIT_SRC" "$AUDIOAUTO_SRC" "$AUDIOKEY_SRC" "$DOOMSB_SRC" "$PMIRQSB_LAUNCH_SRC" "$PMIRQSB_SRC" "$DOOMSFX_LAUNCH_SRC" "$DOOMSFX_SRC" "$DOOMVAN_LAUNCH_SRC" "$DOOM_LAUNCH_SRC" "$WOLF3D_LAUNCH_SRC" "$VGASETUP_SRC" "$VBESVGA_BUILD_SCRIPT"; do
	if [[ ! -f "$f" ]]; then
		echo "[build-full] ERROR: source not found: $f" >&2
		exit 1
	fi
done

if [[ ! -f "$SPLASH_SRC" ]]; then
	echo "[build-full] ERROR: source not found: $SPLASH_SRC" >&2
	exit 1
fi

if [[ ! -f "$SPLASH_TOOL" ]]; then
	echo "[build-full] ERROR: splash generator not found: $SPLASH_TOOL" >&2
	exit 1
fi

if [[ -z "$DOOMVAN_EXE_OVERRIDDEN" \
	&& -f "$DOOMVAN_SRC_DIR/pcdoom.wpj" \
	&& -f "$DOOMVAN_SRC_DIR/pcdoom.tgt" \
	&& -x "$DOOMVAN_WATCOM_ROOT/binl64/wcl386" ]]; then
	echo "[build-full] rebuilding doom-vanille with its required Watcom ABI flags"
	DOOM_VANILLE_SRC="$DOOMVAN_SRC_DIR" WATCOM="$DOOMVAN_WATCOM_ROOT" \
		bash scripts/build_doom_vanille_probe.sh
fi

case "$WOLF4GW_MODE" in
	build)
		[[ -x "$WOLF4GW_BUILD_SCRIPT" ]] \
			|| { echo "[build-full] ERROR missing Wolf4GW build script: $WOLF4GW_BUILD_SCRIPT" >&2; exit 1; }
		bash "$WOLF4GW_BUILD_SCRIPT"
		;;
	reuse)
		[[ -s "$WOLF4GW_EXE" ]] \
			|| { echo "[build-full] ERROR reusable Wolf4GW binary missing: $WOLF4GW_EXE" >&2; exit 1; }
		;;
	off) ;;
	*)
		echo "[build-full] ERROR: CIUKIOS_WOLF4GW_MODE must be build, reuse or off" >&2
		exit 1
		;;
esac

echo "[build-full] assembling full stage0 boot sector"
nasm -f bin "$BOOT_SRC" \
	-D BOOT_LBA_OFFSET="$BOOT_LBA_OFFSET" \
	-D FAT_TOTAL_SECTORS="$TOTAL_SECTORS" \
	-o "$BOOT_BIN"

BOOT_SIZE="$(stat -c%s "$BOOT_BIN")"
if [[ "$BOOT_SIZE" -ne 512 ]]; then
	echo "[build-full] ERROR: boot sector size is $BOOT_SIZE bytes (expected 512)" >&2
	exit 1
fi

echo "[build-full] assembling stage1 payload for full profile (FAT16)"
nasm -f bin "$STAGE1_SRC" \
	-D FAT_SPT="$FAT_SPT" \
	-D FAT_HEADS="$FAT_HEADS" \
	-D FAT_RESERVED_SECTORS="$FAT_RESERVED_SECTORS" \
	-D FAT_SECTORS_PER_CLUSTER="$FAT_SECTORS_PER_CLUSTER" \
	-D FAT_SECTORS_PER_FAT="$FAT_SECTORS_PER_FAT" \
	-D FAT_ROOT_DIR_SECTORS="$ROOT_DIR_SECTORS" \
	-D FAT_TYPE=16 \
	-D FAT_TOTAL_SECTORS="$TOTAL_SECTORS" \
	-D FAT_LBA_OFFSET="$FAT_LBA_OFFSET" \
	-D STAGE1_SELFTEST_AUTORUN="$STAGE1_SELFTEST_AUTORUN" \
	-D STAGE1_RUNTIME_PROBE="$STAGE1_RUNTIME_PROBE" \
	-D STAGE1_DEBUG_COMMANDS="$STAGE1_DEBUG_COMMANDS" \
	-D TRACE_CHILD_INT21="$TRACE_CHILD_INT21" \
	-D TRACE_WIN_INT2F="$TRACE_WIN_INT2F" \
	-D TRACE_WIN_MEMORY="$TRACE_WIN_MEMORY" \
	-D TRACE_WIN_XMS="$TRACE_WIN_XMS" \
	-D STAGE1_BOOT_EXTERNAL_SHELL="$STAGE1_BOOT_EXTERNAL_SHELL" \
	-D STAGE2_AUTORUN="$STAGE2_AUTORUN" \
	-D HARDWARE_VALIDATION_SCREEN="$HARDWARE_VALIDATION_SCREEN" \
	-D DOS_DEFAULT_DRIVE_INDEX="$DOS_DEFAULT_DRIVE_INDEX" \
	-D ENABLE_PS2_MOUSE_INIT="$ENABLE_PS2_MOUSE_INIT" \
	-D WOLF_RUNTIME_DIAG="$WOLF_RUNTIME_DIAG" \
	-l "$STAGE1_LST" -o "$STAGE1_BIN"

STAGE1_SIZE="$(stat -c%s "$STAGE1_BIN")"
if [[ "$STAGE1_SIZE" -gt 4096 ]]; then
	echo "[build-full] ERROR: loader-only Stage1 is $STAGE1_SIZE bytes (max 4096 before CIUKIDOS at 0x0900)" >&2
	exit 1
fi
if [[ "$STAGE1_SIZE" -gt "$STAGE1_SLOT_SIZE" ]]; then
	echo "[build-full] ERROR: stage1 payload is $STAGE1_SIZE bytes (max $STAGE1_SLOT_SIZE)" >&2
	exit 1
fi

# SETUP.COM patches the installed default drive by rewriting the imm8 of
# "mov byte [dos_default_drive], DOS_DEFAULT_DRIVE_INDEX" inside the cloned
# stage1. Locate that byte from the listing so the patch target follows any
# stage1 layout change instead of relying on a hardcoded LBA/offset.
DEFAULT_DRIVE_IMM_ADDR_HEX="$(awk '/mov byte \[loader_default_drive\], DOS_DEFAULT_DRIVE_INDEX/ {print $2; exit}' "$STAGE1_LST")"
if [[ -z "$DEFAULT_DRIVE_IMM_ADDR_HEX" ]]; then
	echo "[build-full] ERROR: loader_default_drive patch site not found in $STAGE1_LST" >&2
	exit 1
fi
# C6 06 <addr16> <imm8>: the immediate is the 5th byte of the instruction.
DEFAULT_DRIVE_IMM_OFF=$((16#$DEFAULT_DRIVE_IMM_ADDR_HEX + 4))
DEFAULT_DRIVE_IMM_BYTE="$(od -An -tu1 -j "$DEFAULT_DRIVE_IMM_OFF" -N1 "$STAGE1_BIN" | tr -d ' ')"
if [[ "$DEFAULT_DRIVE_IMM_BYTE" != "$DOS_DEFAULT_DRIVE_INDEX" ]]; then
	echo "[build-full] ERROR: stage1 default-drive imm byte is $DEFAULT_DRIVE_IMM_BYTE at offset $DEFAULT_DRIVE_IMM_OFF (expected $DOS_DEFAULT_DRIVE_INDEX)" >&2
	exit 1
fi
RAW_STAGE1_PATCH_LBA=$((BOOT_LBA_OFFSET + 1 + DEFAULT_DRIVE_IMM_OFF / 512))
RAW_STAGE1_PATCH_OFF=$((DEFAULT_DRIVE_IMM_OFF % 512))
echo "[build-full] stage1 default-drive patch site: LBA=$RAW_STAGE1_PATCH_LBA off=$RAW_STAGE1_PATCH_OFF"

echo "[build-full] preparing stage1 slot (${STAGE1_SECTORS} sectors)"
dd if=/dev/zero of="$STAGE1_SLOT_BIN" bs=512 count="$STAGE1_SECTORS" status=none
dd if="$STAGE1_BIN" of="$STAGE1_SLOT_BIN" conv=notrunc status=none

echo "[build-full] assembling application payloads"
nasm -f bin src/com/hwdetect.asm -o build/full/obj/hwdetect.com
nasm -f bin src/com/loaddrv.asm -o build/full/obj/loaddrv.com
nasm -f bin assets/drivers/samples/hello/hello.asm -o build/full/obj/hello.com
nasm -f bin src/com/inputinit.asm -o build/full/obj/inputini.com
nasm -f bin src/com/dos_window_runtime.asm -o build/full/obj/doswin.drv
nasm -f bin src/com/window_game_launch.asm -o build/full/obj/dwin.com
nasm -f bin src/com/window_game_launch.asm -D WINDOW_WOLF=1 -o build/full/obj/wwin.com
python3 scripts/build_doom_window.py --output build/full/obj/doom-window
python3 scripts/build_wolf_window.py --output build/full/obj/wolf-window
nasm -f bin "$STAGE2_SRC" -o "$STAGE2_BIN"
nasm -f bin "$RUNTIME_SRC" \
	-D FAT_SPT="$FAT_SPT" \
	-D FAT_HEADS="$FAT_HEADS" \
	-D FAT_RESERVED_SECTORS="$FAT_RESERVED_SECTORS" \
	-D FAT_SECTORS_PER_CLUSTER="$FAT_SECTORS_PER_CLUSTER" \
	-D FAT_SECTORS_PER_FAT="$FAT_SECTORS_PER_FAT" \
	-D FAT_ROOT_DIR_SECTORS="$ROOT_DIR_SECTORS" \
	-D FAT_TYPE=16 \
	-D FAT_TOTAL_SECTORS="$TOTAL_SECTORS" \
	-D FAT_LBA_OFFSET="$FAT_LBA_OFFSET" \
	-D STAGE1_SELFTEST_AUTORUN="$STAGE1_SELFTEST_AUTORUN" \
	-D STAGE1_RUNTIME_PROBE="$STAGE1_RUNTIME_PROBE" \
	-D STAGE1_DEBUG_COMMANDS="$STAGE1_DEBUG_COMMANDS" \
	-D TRACE_CHILD_INT21="$TRACE_CHILD_INT21" \
	-D TRACE_WIN_INT2F="$TRACE_WIN_INT2F" \
	-D TRACE_WIN_MEMORY="$TRACE_WIN_MEMORY" \
	-D TRACE_WIN_XMS="$TRACE_WIN_XMS" \
	-D STAGE1_BOOT_EXTERNAL_SHELL="$STAGE1_BOOT_EXTERNAL_SHELL" \
	-D STAGE2_AUTORUN=0 \
	-D HARDWARE_VALIDATION_SCREEN="$HARDWARE_VALIDATION_SCREEN" \
	-D DOS_DEFAULT_DRIVE_INDEX="$DOS_DEFAULT_DRIVE_INDEX" \
	-D ENABLE_PS2_MOUSE_INIT="$ENABLE_PS2_MOUSE_INIT" \
	-D WOLF_RUNTIME_DIAG="$WOLF_RUNTIME_DIAG" \
	-l build/full/obj/ciukidos.lst -o "$RUNTIME_BIN"
nasm -f bin "$COMDEMO_SRC" -o "$COMDEMO_BIN"
bash scripts/build_media.sh build/full/obj
nasm -f bin "$CIUKRTST_SRC" -D CIUKIDOS_RUNTIME_SEG="$CIUKIDOS_RUNTIME_SEG" -D CIUKIDOS_ABI_VERSION="$CIUKIDOS_ABI_VERSION" -o "$CIUKRTST_BIN"
nasm -f bin "$MOUSECB_SRC" -o "$MOUSECB_BIN"
nasm -f bin "$CIUKPST_SRC" -D CIUKIDOS_RUNTIME_SEG="$CIUKIDOS_RUNTIME_SEG" -D CIUKIDOS_ABI_VERSION="$CIUKIDOS_ABI_VERSION" -o "$CIUKPST_BIN"
nasm -f bin "$CIUKPTRM_SRC" -D CIUKIDOS_RUNTIME_SEG="$CIUKIDOS_RUNTIME_SEG" -D CIUKIDOS_ABI_VERSION="$CIUKIDOS_ABI_VERSION" -o "$CIUKPTRM_BIN"
nasm -f bin "$CIUKPCOM_SRC" -o "$CIUKPCOM_BIN"
nasm -f bin "$MZDEMO_SRC"  -o "$MZDEMO_BIN"
nasm -f bin "$FILEIO_SRC"  -o "$FILEIO_BIN"
nasm -f bin "$DELTEST_SRC" -o "$DELTEST_BIN"
nasm -f bin "$CIUKEDIT_SRC" -o "$CIUKEDIT_BIN"
nasm -f bin "$GFXRECT_SRC" -o "$GFXRECT_BIN"
nasm -f bin "$GFXSTAR_SRC" -o "$GFXSTAR_BIN"
nasm -f bin "$VIDLEAVE_SRC" -o "$VIDLEAVE_BIN"
nasm -f bin "$MOUSE_SRC" -o "$MOUSE_BIN"
nasm -f bin "$CIUKWIN_SRC" -o "$CIUKWIN_BIN"
# Runtime logo bytes must originate from the owner's exact approved portrait.
python3 scripts/build_ciuki_logo.py --check
# Runtime system icons are reproducible conversions of the pinned Tango release.
python3 scripts/build_ui_icons.py --check
nasm -f bin "$SETUP_SRC" -D SETUP_ENABLE_RAW_HDD_INSTALL="$SETUP_RAW_HDD_INSTALL" -D SETUP_ENABLE_RAW_HDD_DESTRUCTIVE="$SETUP_RAW_HDD_DESTRUCTIVE" -D SETUP_LIVE_CD_MODE="$SETUP_LIVE_CD_MODE" -D SETUP_FORCE_MEMDISK_SOURCE="$SETUP_FORCE_MEMDISK_SOURCE" -D ATAPI_MIRROR_BLOCKS="$SETUP_ATAPI_MIRROR_BLOCKS" -D RAW_HDD_PARTITION_SECTORS="$TOTAL_SECTORS" -D RAW_STAGE1_DEFAULT_DRIVE_PATCH_LBA="$RAW_STAGE1_PATCH_LBA" -D RAW_STAGE1_DEFAULT_DRIVE_PATCH_OFF="$RAW_STAGE1_PATCH_OFF" -o "$SETUP_BIN"
nasm -f bin "$FORMAT_SRC" -D MBR_PARTITION_SECTORS="$TOTAL_SECTORS" -o "$FORMAT_BIN"
nasm -f bin "$SHELL_SRC" -D COMMAND_COMPAT=1 -o "$COMMAND_COMPAT_BIN"
nasm -f bin "$SHELL_SRC" -o "$SHELL_BIN"
nasm -f bin "$IPCONFIG_SRC" -o "$IPCONFIG_BIN"
nasm -f bin "$ICMPD_SRC" -o "$ICMPD_BIN"
nasm -f bin "$NETCFG_SRC" -o "$NETCFG_BIN"
nasm -f bin "$DRVLOAD_SRC" -o "$DRVLOAD_BIN"
nasm -f bin "$SB16INIT_SRC" -o "$SB16INIT_BIN"
nasm -f bin "$AUDIOTST_SRC" -o "$AUDIOTST_BIN"
nasm -f bin "$AC97INIT_SRC" -o "$AC97INIT_BIN"
nasm -f bin "$AC97INIT_SRC" -D BOOT_SOUND=1 -o build/full/obj/bootsnd.com
nasm -f bin "$AC97INIT_SRC" -D UI_SFX_DRIVER=1 -o build/full/obj/sfx.drv
bash scripts/build_media_driver.sh build/full/obj/media-driver
python3 scripts/build_system_sounds.py --output build/full/obj/system-sounds
python3 scripts/generate_startup_sound.py build/full/obj
nasm -f bin "$SB16INIT_SRC" -D SB_STARTUP=1 -o build/full/obj/sbstart.com
nasm -f bin "$AUDIOAUTO_SRC" -o "$AUDIOAUTO_BIN"
nasm -f bin "$AUDIOKEY_SRC" -o "$AUDIOKEY_BIN"
nasm -f bin "$SBEMINIT_SRC" -o "$SBEMINIT_BIN"
nasm -f bin "$DOOMSB_SRC" -o "$DOOMSB_BIN"
nasm -f bin "$PMIRQSB_LAUNCH_SRC" -o "$PMIRQSB_LAUNCH_BIN"
nasm -f bin "$DOOMSFX_LAUNCH_SRC" -o "$DOOMSFX_LAUNCH_BIN"
nasm -f bin "$DOOMVAN_LAUNCH_SRC" -o "$DOOMVAN_LAUNCH_BIN"
nasm -f bin -D LAUNCHER_MZ=1 "$DOOMVAN_LAUNCH_SRC" -o "$DOOMVAN_LAUNCH_MZ"
nasm -f bin src/com/doom_safe.asm -o build/full/obj/doomsafe.com
nasm -f bin "$DOOM_LAUNCH_SRC" -o "$DOOM_LAUNCH_BIN"
nasm -f bin -D LAUNCHER_MZ=1 "$DOOM_LAUNCH_SRC" -o "$DOOM_LAUNCH_MZ"
nasm -f bin "$WOLF3D_LAUNCH_SRC" -o "$WOLF3D_LAUNCH_BIN"
nasm -f bin "$VGASETUP_SRC" -o "$VGASETUP_BIN"
if bash scripts/build_pmirqsb_dos4gw.sh; then
	echo "[build-full] PMIRQSB protected-mode probe built"
else
	pmirqsb_rc=$?
	if [[ $pmirqsb_rc -eq 2 ]]; then
		echo "[build-full] PMIRQSB protected-mode probe skipped (OpenWatcom unavailable)"
	else
		exit "$pmirqsb_rc"
	fi
fi
if bash scripts/build_doomsfx_dos4gw.sh; then
	echo "[build-full] DOOMSFX controlled audio lane built"
else
	doomsfx_rc=$?
	if [[ $doomsfx_rc -eq 2 ]]; then
		echo "[build-full] DOOMSFX controlled audio lane skipped (OpenWatcom unavailable)"
	else
		exit "$doomsfx_rc"
	fi
fi

echo "[build-full] generating splash asset"
if ! command -v python3 >/dev/null 2>&1; then
	echo "[build-full] ERROR: python3 is required to generate SPLASH.BIN" >&2
	exit 1
fi

if ! python3 "$SPLASH_TOOL" "$SPLASH_SRC" "$SPLASH_BIN"; then
	echo "[build-full] ERROR: failed to generate SPLASH.BIN (requires python3 + Pillow)" >&2
	exit 1
fi

STAGE2_SIZE="$(stat -c%s "$STAGE2_BIN")"
RUNTIME_SIZE="$(stat -c%s "$RUNTIME_BIN")"
if [[ "$STAGE2_SIZE" -gt "$STAGE2_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: stage2 payload is $STAGE2_SIZE bytes (max $STAGE2_MAX_SIZE)" >&2
	exit 1
fi

if [[ "$RUNTIME_SIZE" -gt "$RUNTIME_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: CIUKIDOS runtime is $RUNTIME_SIZE bytes (max $RUNTIME_MAX_SIZE)" >&2
	exit 1
fi

COMDEMO_SIZE="$(stat -c%s "$COMDEMO_BIN")"
MZDEMO_SIZE="$(stat -c%s  "$MZDEMO_BIN")"
FILEIO_SIZE="$(stat -c%s  "$FILEIO_BIN")"
DELTEST_SIZE="$(stat -c%s "$DELTEST_BIN")"
CIUKEDIT_SIZE="$(stat -c%s "$CIUKEDIT_BIN")"
GFXRECT_SIZE="$(stat -c%s "$GFXRECT_BIN")"
GFXSTAR_SIZE="$(stat -c%s "$GFXSTAR_BIN")"
CIUKWIN_SIZE="$(stat -c%s "$CIUKWIN_BIN")"
SETUP_SIZE="$(stat -c%s "$SETUP_BIN")"
FORMAT_SIZE="$(stat -c%s "$FORMAT_BIN")"
DOOMSFX_SIZE="$(stat -c%s "$DOOMSFX_BIN")"
DOS4GW_SIZE="$(stat -c%s "$DOS4GW_BIN")"
SPLASH_SIZE="$(stat -c%s "$SPLASH_BIN")"
SPLASH_SECTORS=$(((SPLASH_SIZE + 511) / 512))
CLUSTER_SIZE_BYTES=$((FAT_SECTORS_PER_CLUSTER * 512))
SPLASH_CLUSTERS=$(((SPLASH_SIZE + CLUSTER_SIZE_BYTES - 1) / CLUSTER_SIZE_BYTES))
STAGE2_SECTORS=$(((STAGE2_SIZE + 511) / 512))
COMDEMO_SECTORS=$(((COMDEMO_SIZE + 511) / 512))
MZDEMO_SECTORS=$(((MZDEMO_SIZE + 511) / 512))
FILEIO_SECTORS=$(((FILEIO_SIZE + 511) / 512))
DELTEST_SECTORS=$(((DELTEST_SIZE + 511) / 512))
CIUKEDIT_SECTORS=$(((CIUKEDIT_SIZE + 511) / 512))
CIUKEDIT_CLUSTERS=$(((CIUKEDIT_SIZE + CLUSTER_SIZE_BYTES - 1) / CLUSTER_SIZE_BYTES))
GFXRECT_SECTORS=$(((GFXRECT_SIZE + 511) / 512))
GFXSTAR_SECTORS=$(((GFXSTAR_SIZE + 511) / 512))
CIUKWIN_SECTORS=$(((CIUKWIN_SIZE + 511) / 512))
SETUP_SECTORS=$(((SETUP_SIZE + 511) / 512))
SETUP_CLUSTERS=$(((SETUP_SIZE + CLUSTER_SIZE_BYTES - 1) / CLUSTER_SIZE_BYTES))
FORMAT_SECTORS=$(((FORMAT_SIZE + 511) / 512))
FORMAT_CLUSTERS=$(((FORMAT_SIZE + CLUSTER_SIZE_BYTES - 1) / CLUSTER_SIZE_BYTES))
DOOMSFX_SECTORS=$(((DOOMSFX_SIZE + 511) / 512))
DOOMSFX_CLUSTERS=$(((DOOMSFX_SIZE + CLUSTER_SIZE_BYTES - 1) / CLUSTER_SIZE_BYTES))
DOS4GW_SECTORS=$(((DOS4GW_SIZE + 511) / 512))
DOS4GW_CLUSTERS=$(((DOS4GW_SIZE + CLUSTER_SIZE_BYTES - 1) / CLUSTER_SIZE_BYTES))

if [[ "$GFXRECT_SIZE" -gt "$GFXRECT_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: GFXRECT payload is $GFXRECT_SIZE bytes (max $GFXRECT_MAX_SIZE)" >&2
	exit 1
fi

if [[ "$GFXSTAR_SIZE" -gt "$GFXSTAR_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: GFXSTAR payload is $GFXSTAR_SIZE bytes (max $GFXSTAR_MAX_SIZE)" >&2
	exit 1
fi

if [[ "$CIUKWIN_SIZE" -gt "$CIUKWIN_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: CIUKWIN payload is $CIUKWIN_SIZE bytes (max $CIUKWIN_MAX_SIZE)" >&2
	exit 1
fi

if [[ "$SETUP_SIZE" -gt "$SETUP_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: SETUP payload is $SETUP_SIZE bytes (max $SETUP_MAX_SIZE)" >&2
	exit 1
fi

if (( SETUP_CLUSTERS < 1 || SETUP_CLUSTERS > SETUP_MAX_CLUSTERS )); then
	echo "[build-full] ERROR: SETUP cluster span is invalid: $SETUP_CLUSTERS (max $SETUP_MAX_CLUSTERS)" >&2
	exit 1
fi

if [[ "$FORMAT_SIZE" -gt "$FORMAT_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: FORMAT payload is $FORMAT_SIZE bytes (max $FORMAT_MAX_SIZE)" >&2
	exit 1
fi

if (( FORMAT_CLUSTERS < 1 || FORMAT_CLUSTERS > FORMAT_MAX_CLUSTERS )); then
	echo "[build-full] ERROR: FORMAT cluster span is invalid: $FORMAT_CLUSTERS (max $FORMAT_MAX_CLUSTERS)" >&2
	exit 1
fi

echo "[build-full] generating setup payload manifest"
printf 'SMF1' > "$SETUP_MANIFEST_BIN"
printf '\x09' >> "$SETUP_MANIFEST_BIN"
printf '\x01\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x01\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x02\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x02\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x02\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x03\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x03\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x03\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x03\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"

SETUP_MANIFEST_SIZE="$(stat -c%s "$SETUP_MANIFEST_BIN")"
SETUP_MANIFEST_SECTORS=$(((SETUP_MANIFEST_SIZE + 511) / 512))
SETUP_MANIFEST_CLUSTERS=$(((SETUP_MANIFEST_SIZE + CLUSTER_SIZE_BYTES - 1) / CLUSTER_SIZE_BYTES))

if (( SETUP_MANIFEST_CLUSTERS != 1 )); then
	echo "[build-full] ERROR: setup manifest must fit in one cluster ($SETUP_MANIFEST_SIZE bytes)" >&2
	exit 1
fi

echo "[build-full] sector map: STAGE2=$STAGE2_SECTORS COMDEMO=$COMDEMO_SECTORS MZDEMO=$MZDEMO_SECTORS FILEIO=$FILEIO_SECTORS DELTEST=$DELTEST_SECTORS CIUKEDIT=$CIUKEDIT_SECTORS GFXRECT=$GFXRECT_SECTORS GFXSTAR=$GFXSTAR_SECTORS CIUKWIN=$CIUKWIN_SECTORS SETUP=$SETUP_SECTORS SETUP_MFT=$SETUP_MANIFEST_SECTORS FORMAT=$FORMAT_SECTORS DOOMSFX=$DOOMSFX_SECTORS DOS4GW=$DOS4GW_SECTORS SPLASH=$SPLASH_SECTORS"

# Permanent, tiny compatibility fixture placed beyond the 16-bit LBA boundary.
# It lets the raw-HDD and El Torito hard-disk-emulation gates prove the EDD/CHS32
# fallback using only payloads produced by this repository.
printf '%s\r\n' '[LBA32] HIGH-LBA READ PASS' > "$LBA32_TEST_BIN"
LBA32_TEST_SIZE="$(stat -c%s "$LBA32_TEST_BIN")"

if [[ "$SPLASH_SIZE" -ne "$SPLASH_EXPECTED_SIZE" ]]; then
	echo "[build-full] ERROR: SPLASH.BIN size is $SPLASH_SIZE bytes (expected $SPLASH_EXPECTED_SIZE)" >&2
	exit 1
fi

if [[ "$SPLASH_SIZE" -gt "$SPLASH_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: SPLASH.BIN is $SPLASH_SIZE bytes (max $SPLASH_MAX_SIZE)" >&2
	exit 1
fi

if [[ "$FILEIO_SIZE" -le 512 ]]; then
	echo "[build-full] ERROR: FILEIO payload must span >1 cluster ($FILEIO_SIZE bytes)" >&2
	exit 1
fi

# FAT16 directory/cluster map with contiguous SYSTEM/SPLASH chain.
# App payloads remain single-cluster except CIUKEDIT.COM, SETUP.COM, and the
# other explicitly chained payloads below.
ROOT_SYSTEM_CLUSTER=2
ROOT_APPS_CLUSTER=3
SYSTEM_STAGE2_CLUSTER=4
SYSTEM_SPLASH_CLUSTER=5
SYSTEM_SPLASH_LAST_CLUSTER=$((SYSTEM_SPLASH_CLUSTER + SPLASH_CLUSTERS - 1))
APPS_COMDEMO_CLUSTER=$((SYSTEM_SPLASH_LAST_CLUSTER + 1))
APPS_MZDEMO_CLUSTER=$((APPS_COMDEMO_CLUSTER + 1))
APPS_FILEIO_CLUSTER=$((APPS_MZDEMO_CLUSTER + 1))
APPS_DELTEST_CLUSTER=$((APPS_FILEIO_CLUSTER + 1))
APPS_CIUKEDIT_CLUSTER=$((APPS_DELTEST_CLUSTER + 1))
APPS_CIUKEDIT_LAST_CLUSTER=$((APPS_CIUKEDIT_CLUSTER + CIUKEDIT_CLUSTERS - 1))
APPS_GFXRECT_CLUSTER=$((APPS_CIUKEDIT_LAST_CLUSTER + 1))
APPS_GFXSTAR_CLUSTER=$((APPS_GFXRECT_CLUSTER + 1))
APPS_SETUP_CLUSTER=$((APPS_GFXSTAR_CLUSTER + 1))
APPS_SETUP_LAST_CLUSTER=$((APPS_SETUP_CLUSTER + SETUP_CLUSTERS - 1))
APPS_SETUP_MANIFEST_CLUSTER=$((APPS_SETUP_LAST_CLUSTER + 1))
APPS_CIUKWIN_CLUSTER=$((APPS_SETUP_MANIFEST_CLUSTER + 1))
APPS_FORMAT_CLUSTER=$((APPS_CIUKWIN_CLUSTER + 1))
APPS_FORMAT_LAST_CLUSTER=$((APPS_FORMAT_CLUSTER + FORMAT_CLUSTERS - 1))
ROOT_DOS4GW_CLUSTER=$((APPS_FORMAT_LAST_CLUSTER + 1))
ROOT_DOS4GW_LAST_CLUSTER=$((ROOT_DOS4GW_CLUSTER + DOS4GW_CLUSTERS - 1))
ROOT_DOOMSFX_CLUSTER=$((ROOT_DOS4GW_LAST_CLUSTER + 1))
ROOT_DOOMSFX_LAST_CLUSTER=$((ROOT_DOOMSFX_CLUSTER + DOOMSFX_CLUSTERS - 1))
LBA32_TEST_CLUSTER=8194
LBA32_TEST_LBA=$((DATA_LBA + ((LBA32_TEST_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER)))

if (( LBA32_TEST_LBA <= 65535 )); then
	echo "[build-full] ERROR: LBA32 fixture must live beyond LBA 65535 (got $LBA32_TEST_LBA)" >&2
	exit 1
fi

if (( SPLASH_CLUSTERS < 1 )); then
	echo "[build-full] ERROR: SPLASH.BIN requires an invalid cluster count ($SPLASH_CLUSTERS)" >&2
	exit 1
fi

if (( ROOT_DOOMSFX_LAST_CLUSTER >= 256 )); then
	echo "[build-full] ERROR: FAT16 layout exceeds first FAT sector entry range" >&2
	exit 1
fi

for sectors in \
	"$STAGE2_SECTORS" \
	"$COMDEMO_SECTORS" \
	"$MZDEMO_SECTORS" \
	"$FILEIO_SECTORS" \
	"$DELTEST_SECTORS" \
	"$GFXRECT_SECTORS" \
	"$GFXSTAR_SECTORS" \
	"$CIUKWIN_SECTORS" \
	"$SETUP_MANIFEST_SECTORS"; do
	if (( sectors > FAT_SECTORS_PER_CLUSTER )); then
		echo "[build-full] ERROR: payload exceeds single FAT16 cluster (${FAT_SECTORS_PER_CLUSTER} sectors)" >&2
		exit 1
	fi
done

FAT_SECTOR_BIN="build/full/obj/fat16_sector.bin"
dd if=/dev/zero of="$FAT_SECTOR_BIN" bs=1 count=512 status=none

fat16_set_entry() {
	local index="$1" value="$2"
	local offset=$((index * 2))
	printf "$(printf '\\x%02x\\x%02x' $((value & 0xFF)) $(((value >> 8) & 0xFF)))" \
		| dd of="$FAT_SECTOR_BIN" bs=1 seek="$offset" conv=notrunc status=none
}

fat16_set_contiguous_chain() {
	local start="$1" count="$2"
	local cluster="$start"
	local remaining="$count"

	if (( remaining <= 0 )); then
		return
	fi

	while (( remaining > 1 )); do
		fat16_set_entry "$cluster" "$((cluster + 1))"
		cluster=$((cluster + 1))
		remaining=$((remaining - 1))
	done

	fat16_set_entry "$cluster" 0xFFFF
}

fat16_set_entry 0 0xFFF8
fat16_set_entry 1 0xFFFF
fat16_set_entry "$ROOT_SYSTEM_CLUSTER" 0xFFFF
fat16_set_entry "$ROOT_APPS_CLUSTER" 0xFFFF
fat16_set_entry "$SYSTEM_STAGE2_CLUSTER" 0xFFFF
fat16_set_contiguous_chain "$SYSTEM_SPLASH_CLUSTER" "$SPLASH_CLUSTERS"
fat16_set_entry "$APPS_COMDEMO_CLUSTER" 0xFFFF
fat16_set_entry "$APPS_MZDEMO_CLUSTER" 0xFFFF
fat16_set_entry "$APPS_FILEIO_CLUSTER" 0xFFFF
fat16_set_entry "$APPS_DELTEST_CLUSTER" 0xFFFF
fat16_set_contiguous_chain "$APPS_CIUKEDIT_CLUSTER" "$CIUKEDIT_CLUSTERS"
fat16_set_entry "$APPS_GFXRECT_CLUSTER" 0xFFFF
fat16_set_entry "$APPS_GFXSTAR_CLUSTER" 0xFFFF
fat16_set_contiguous_chain "$APPS_SETUP_CLUSTER" "$SETUP_CLUSTERS"
fat16_set_entry "$APPS_SETUP_MANIFEST_CLUSTER" 0xFFFF
fat16_set_entry "$APPS_CIUKWIN_CLUSTER" 0xFFFF
fat16_set_contiguous_chain "$APPS_FORMAT_CLUSTER" "$FORMAT_CLUSTERS"
fat16_set_contiguous_chain "$ROOT_DOS4GW_CLUSTER" "$DOS4GW_CLUSTERS"
fat16_set_contiguous_chain "$ROOT_DOOMSFX_CLUSTER" "$DOOMSFX_CLUSTERS"

make_entry() {
	local out="$1" name="$2" attr="$3" cluster="$4" size="$5"
	dd if=/dev/zero of="$out" bs=1 count=32 status=none
	printf '%s' "$name" | dd of="$out" bs=1 seek=0 conv=notrunc status=none
	printf "$(printf '\\x%02x' "$attr")" | dd of="$out" bs=1 seek=11 conv=notrunc status=none
	printf "$(printf '\\x%02x\\x%02x' $((cluster & 0xFF)) $(((cluster >> 8) & 0xFF)))" \
		| dd of="$out" bs=1 seek=26 conv=notrunc status=none
	printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
		$((size & 0xFF)) $(((size >> 8) & 0xFF)) \
		$(((size >> 16) & 0xFF)) $(((size >> 24) & 0xFF)))" \
		| dd of="$out" bs=1 seek=28 conv=notrunc status=none
}

ROOT_ENTRY_SYSTEM="build/full/obj/root_system.bin"
ROOT_ENTRY_APPS="build/full/obj/root_apps.bin"
ROOT_ENTRY_DOS4GW="build/full/obj/root_dos4gw.bin"
ROOT_ENTRY_DOOMSFX="build/full/obj/root_doomsfx.bin"
ROOT_ENTRY_LBA32="build/full/obj/root_lba32.bin"
DIR_ENTRY_DOT_SYSTEM="build/full/obj/dir_dot_system.bin"
DIR_ENTRY_DOTDOT_ROOT="build/full/obj/dir_dotdot_root.bin"
DIR_ENTRY_DOT_APPS="build/full/obj/dir_dot_apps.bin"
DIR_ENTRY_STAGE2="build/full/obj/dir_stage2.bin"
DIR_ENTRY_SPLASH="build/full/obj/dir_splash.bin"
DIR_ENTRY_COMDEMO="build/full/obj/dir_comdemo.bin"
DIR_ENTRY_MZDEMO="build/full/obj/dir_mzdemo.bin"
DIR_ENTRY_FILEIO="build/full/obj/dir_fileio.bin"
DIR_ENTRY_DELTEST="build/full/obj/dir_deltest.bin"
DIR_ENTRY_CIUKEDIT="build/full/obj/dir_ciukedit.bin"
DIR_ENTRY_GFXRECT="build/full/obj/dir_gfxrect.bin"
DIR_ENTRY_GFXSTAR="build/full/obj/dir_gfxstar.bin"
DIR_ENTRY_CIUKWIN="build/full/obj/dir_ciukwin.bin"
DIR_ENTRY_SETUP="build/full/obj/dir_setup.bin"
DIR_ENTRY_SETUP_MFT="build/full/obj/dir_setup_mft.bin"
DIR_ENTRY_SETUP_MFT_ALT="build/full/obj/dir_setup_mft_alt.bin"
DIR_ENTRY_FORMAT="build/full/obj/dir_format.bin"
SYSTEM_DIR_CLUSTER_BIN="build/full/obj/system_dir_cluster.bin"
APPS_DIR_CLUSTER_BIN="build/full/obj/apps_dir_cluster.bin"

make_entry "$ROOT_ENTRY_SYSTEM" 'SYSTEM     ' 0x10 "$ROOT_SYSTEM_CLUSTER" 0
make_entry "$ROOT_ENTRY_APPS"   'APPS       ' 0x10 "$ROOT_APPS_CLUSTER" 0
make_entry "$ROOT_ENTRY_DOS4GW" 'DOS4GW  EXE' 0x20 "$ROOT_DOS4GW_CLUSTER" "$DOS4GW_SIZE"
make_entry "$ROOT_ENTRY_DOOMSFX" 'DOOMSFX EXE' 0x20 "$ROOT_DOOMSFX_CLUSTER" "$DOOMSFX_SIZE"
make_entry "$ROOT_ENTRY_LBA32" 'LBA32   TXT' 0x20 "$LBA32_TEST_CLUSTER" "$LBA32_TEST_SIZE"

make_entry "$DIR_ENTRY_DOT_SYSTEM" '.          ' 0x10 "$ROOT_SYSTEM_CLUSTER" 0
make_entry "$DIR_ENTRY_DOTDOT_ROOT" '..         ' 0x10 0 0
make_entry "$DIR_ENTRY_DOT_APPS" '.          ' 0x10 "$ROOT_APPS_CLUSTER" 0

make_entry "$DIR_ENTRY_STAGE2"   'STAGE2  BIN' 0x20 "$SYSTEM_STAGE2_CLUSTER" "$STAGE2_SIZE"
make_entry "$DIR_ENTRY_SPLASH"   'SPLASH  BIN' 0x20 "$SYSTEM_SPLASH_CLUSTER" "$SPLASH_SIZE"
make_entry "$DIR_ENTRY_COMDEMO"  'COMDEMO COM' 0x20 "$APPS_COMDEMO_CLUSTER" "$COMDEMO_SIZE"
make_entry "$DIR_ENTRY_MZDEMO"   'MZDEMO  EXE' 0x20 "$APPS_MZDEMO_CLUSTER" "$MZDEMO_SIZE"
make_entry "$DIR_ENTRY_FILEIO"   'FILEIO  BIN' 0x20 "$APPS_FILEIO_CLUSTER" "$FILEIO_SIZE"
make_entry "$DIR_ENTRY_DELTEST"  'DELTEST BIN' 0x20 "$APPS_DELTEST_CLUSTER" "$DELTEST_SIZE"
make_entry "$DIR_ENTRY_CIUKEDIT" 'CIUKEDITCOM' 0x20 "$APPS_CIUKEDIT_CLUSTER" "$CIUKEDIT_SIZE"
make_entry "$DIR_ENTRY_GFXRECT"  'GFXRECT COM' 0x20 "$APPS_GFXRECT_CLUSTER" "$GFXRECT_SIZE"
make_entry "$DIR_ENTRY_GFXSTAR"  'GFXSTAR COM' 0x20 "$APPS_GFXSTAR_CLUSTER" "$GFXSTAR_SIZE"
make_entry "$DIR_ENTRY_SETUP"    'SETUP   COM' 0x20 "$APPS_SETUP_CLUSTER" "$SETUP_SIZE"
make_entry "$DIR_ENTRY_SETUP_MFT" 'SETUPMFTBIN' 0x20 "$APPS_SETUP_MANIFEST_CLUSTER" "$SETUP_MANIFEST_SIZE"
# FAT has no hard links. Reserve an empty directory slot for the legacy name;
# mcopy below allocates a separate chain instead of cross-linking SETUPMFT.BIN.
make_entry "$DIR_ENTRY_SETUP_MFT_ALT" 'MANIFST BIN' 0x20 0 0
make_entry "$DIR_ENTRY_CIUKWIN"  'CIUKWIN COM' 0x20 "$APPS_CIUKWIN_CLUSTER" "$CIUKWIN_SIZE"
make_entry "$DIR_ENTRY_FORMAT"   'FORMAT  COM' 0x20 "$APPS_FORMAT_CLUSTER" "$FORMAT_SIZE"

dd if=/dev/zero of="$SYSTEM_DIR_CLUSTER_BIN" bs=512 count="$FAT_SECTORS_PER_CLUSTER" status=none
dd if="$DIR_ENTRY_DOT_SYSTEM" of="$SYSTEM_DIR_CLUSTER_BIN" bs=1 seek=0 conv=notrunc status=none
dd if="$DIR_ENTRY_DOTDOT_ROOT" of="$SYSTEM_DIR_CLUSTER_BIN" bs=1 seek=32 conv=notrunc status=none
dd if="$DIR_ENTRY_STAGE2" of="$SYSTEM_DIR_CLUSTER_BIN" bs=1 seek=64 conv=notrunc status=none
dd if="$DIR_ENTRY_SPLASH" of="$SYSTEM_DIR_CLUSTER_BIN" bs=1 seek=96 conv=notrunc status=none

dd if=/dev/zero of="$APPS_DIR_CLUSTER_BIN" bs=512 count="$FAT_SECTORS_PER_CLUSTER" status=none
dd if="$DIR_ENTRY_DOT_APPS" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=0 conv=notrunc status=none
dd if="$DIR_ENTRY_DOTDOT_ROOT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=32 conv=notrunc status=none
dd if="$DIR_ENTRY_COMDEMO" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=64 conv=notrunc status=none
dd if="$DIR_ENTRY_MZDEMO" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=96 conv=notrunc status=none
dd if="$DIR_ENTRY_FILEIO" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=128 conv=notrunc status=none
dd if="$DIR_ENTRY_DELTEST" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=160 conv=notrunc status=none
dd if="$DIR_ENTRY_CIUKEDIT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=192 conv=notrunc status=none
dd if="$DIR_ENTRY_GFXRECT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=224 conv=notrunc status=none
dd if="$DIR_ENTRY_GFXSTAR" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=256 conv=notrunc status=none
dd if="$DIR_ENTRY_SETUP" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=288 conv=notrunc status=none
dd if="$DIR_ENTRY_SETUP_MFT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=320 conv=notrunc status=none
dd if="$DIR_ENTRY_SETUP_MFT_ALT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=352 conv=notrunc status=none
dd if="$DIR_ENTRY_CIUKWIN" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=384 conv=notrunc status=none
dd if="$DIR_ENTRY_FORMAT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=416 conv=notrunc status=none

echo "[build-full] creating FAT16 image (${TOTAL_SECTORS} sectors)"
dd if=/dev/zero of="$IMG" bs=512 count="$TOTAL_SECTORS" status=none
dd if="$BOOT_BIN"           of="$IMG" bs=512 count=1                seek=0           conv=notrunc status=none
dd if="$STAGE1_SLOT_BIN"    of="$IMG" bs=512 count="$STAGE1_SECTORS" seek=1          conv=notrunc status=none
dd if="$FAT_SECTOR_BIN"     of="$IMG" bs=512 count=1                seek="$FAT1_LBA" conv=notrunc status=none
dd if="$FAT_SECTOR_BIN"     of="$IMG" bs=512 count=1                seek="$FAT2_LBA" conv=notrunc status=none
dd if="$ROOT_ENTRY_SYSTEM" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 0)) conv=notrunc status=none
dd if="$ROOT_ENTRY_APPS" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 32)) conv=notrunc status=none
dd if="$ROOT_ENTRY_DOS4GW" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 64)) conv=notrunc status=none
dd if="$ROOT_ENTRY_DOOMSFX" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 96)) conv=notrunc status=none
dd if="$ROOT_ENTRY_LBA32" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 128)) conv=notrunc status=none

# The deterministic high-LBA fixture lives outside the first FAT sector used by
# the compact static layout above, so write its EOF entry to both FAT copies.
printf '\377\377' | dd of="$IMG" bs=1 seek=$((FAT1_LBA * 512 + LBA32_TEST_CLUSTER * 2)) conv=notrunc status=none
printf '\377\377' | dd of="$IMG" bs=1 seek=$((FAT2_LBA * 512 + LBA32_TEST_CLUSTER * 2)) conv=notrunc status=none
dd if="$LBA32_TEST_BIN" of="$IMG" bs=512 seek="$LBA32_TEST_LBA" count=1 conv=notrunc status=none

dd if="$SYSTEM_DIR_CLUSTER_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((ROOT_SYSTEM_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$FAT_SECTORS_PER_CLUSTER" conv=notrunc status=none
dd if="$APPS_DIR_CLUSTER_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((ROOT_APPS_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$FAT_SECTORS_PER_CLUSTER" conv=notrunc status=none

dd if="$STAGE2_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((SYSTEM_STAGE2_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$STAGE2_SECTORS" conv=notrunc status=none
dd if="$SPLASH_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((SYSTEM_SPLASH_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$SPLASH_SECTORS" conv=notrunc status=none
dd if="$COMDEMO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_COMDEMO_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$COMDEMO_SECTORS" conv=notrunc status=none
dd if="$MZDEMO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_MZDEMO_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$MZDEMO_SECTORS" conv=notrunc status=none
dd if="$FILEIO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_FILEIO_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$FILEIO_SECTORS" conv=notrunc status=none
dd if="$DELTEST_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_DELTEST_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$DELTEST_SECTORS" conv=notrunc status=none
dd if="$CIUKEDIT_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_CIUKEDIT_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$CIUKEDIT_SECTORS" conv=notrunc status=none
dd if="$GFXRECT_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_GFXRECT_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$GFXRECT_SECTORS" conv=notrunc status=none
dd if="$GFXSTAR_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_GFXSTAR_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$GFXSTAR_SECTORS" conv=notrunc status=none
dd if="$SETUP_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_SETUP_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$SETUP_SECTORS" conv=notrunc status=none
dd if="$SETUP_MANIFEST_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_SETUP_MANIFEST_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$SETUP_MANIFEST_SECTORS" conv=notrunc status=none
dd if="$CIUKWIN_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_CIUKWIN_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$CIUKWIN_SECTORS" conv=notrunc status=none
dd if="$FORMAT_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_FORMAT_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$FORMAT_SECTORS" conv=notrunc status=none
dd if="$DOS4GW_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((ROOT_DOS4GW_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$DOS4GW_SECTORS" conv=notrunc status=none
dd if="$DOOMSFX_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((ROOT_DOOMSFX_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$DOOMSFX_SECTORS" conv=notrunc status=none

if ! command -v mcopy >/dev/null 2>&1; then
	echo "[build-full] ERROR: mcopy is required to inject the COMMAND.COM compatibility shell" >&2
	exit 1
fi

if ! command -v mmd >/dev/null 2>&1 || ! command -v mdir >/dev/null 2>&1 || ! command -v mlabel >/dev/null 2>&1; then
	echo "[build-full] ERROR: mtools mmd/mdir/mlabel are required to prepare the FAT16 volume" >&2
	exit 1
fi

mcopy -o -i "$IMG" "$SETUP_MANIFEST_BIN" ::APPS/MANIFST.BIN
mlabel -i "$IMG" ::CIUKIOSFULL

echo "[build-full] injecting functional COMMAND.COM compatibility shell to $COMMAND_COMPAT_IMAGE_PATH"
mcopy -o -i "$IMG" "$COMMAND_COMPAT_BIN" "$COMMAND_COMPAT_IMAGE_PATH"

echo "[build-full] injecting external shell prototype to ::SYSTEM/SHELL.COM"
mcopy -o -i "$IMG" "$SHELL_BIN" ::SYSTEM/SHELL.COM
mcopy -o -i "$IMG" build/full/obj/doswin.drv ::SYSTEM/DOSWIN.DRV
mcopy -o -i "$IMG" build/full/obj/dwin.com build/full/obj/wwin.com ::APPS/
mcopy -o -i "$IMG" build/full/obj/doom-window/DOOMWIN.EXE build/full/obj/wolf-window/WOLFWIN.EXE ::APPS/
mcopy -o -i "$IMG" "$DOS4GW_BIN" ::APPS/DOS4GW.EXE
mtools_ensure_dir "$IMG" ::SYSTEM/GAMES
mcopy -o -i "$IMG" third_party/doomgeneric/LICENSE ::SYSTEM/GAMES/DOOMGPL.TXT
mcopy -o -i "$IMG" third_party/wolf4sdl/COPYING ::SYSTEM/GAMES/WOLFGPL.TXT
mcopy -o -i "$IMG" src/ports/README.TXT ::SYSTEM/GAMES/README.TXT
python3 scripts/package_window_game_sources.py --output build/full/obj/gamesrc.tgz
mcopy -o -i "$IMG" build/full/obj/gamesrc.tgz ::SYSTEM/GAMES/GAMESRC.TGZ
if [[ "${CIUKIOS_INCLUDE_DOS_WINDOW_PROBES:-0}" == "1" ]]; then
    mcopy -o -i "$IMG" build/full/obj/doom-window/CGSMOKE.EXE ::APPS/CGSMOKE.EXE
    bash src/probes/doswindow/build.sh build/full/obj/doswindow
    for window_probe in DWBIOST.COM DWBIOST.EXE DWBIOSCH.COM; do
        mcopy -o -i "$IMG" "build/full/obj/doswindow/$window_probe" "::APPS/$window_probe"
    done
fi
if [[ "${CIUKIOS_VM_WINDOW:-1}" == "1" ]]; then
    # VM manager for the desktop's DOS windows (C:\VM), part of every build:
    # Jemm386 V86 monitor + JLOAD + CVSESSION (VGA model, keyboard/mouse/
    # SB16/OPL on AC'97), VMSTART (run by the desktop at boot), DPMIRUN
    # (each window's launcher and DPMI host) and VMFORK (a program in a DOS
    # VM of its own). DOS/4GW programs use the
    # patched HDPMI packaged in ::SBEMU. CIUKIOS_VM_WINDOW=0 leaves it out.
    [[ "$SBEMU_MODE" != "off" ]] \
        || { echo "[build-full] ERROR: CIUKIOS_VM_WINDOW=1 needs the patched HDPMI in ::SBEMU (CIUKIOS_SBEMU_MODE=build or reuse)" >&2; exit 1; }
    VM_WINDOW_DIR="${CIUKIOS_VM_WINDOW_DIR:-$CIUKIOS_ROOT/build/full/obj/vm-window}"
    echo "[build-full] building the DOS-window VM session into $VM_WINDOW_DIR"
    bash scripts/build_jemm_monitor.sh --output "$VM_WINDOW_DIR/jemm" \
        --ciukios-device-query --ciukios-vm-scheduler
    vm_jemm="$VM_WINDOW_DIR/jemm/$(cat "$VM_WINDOW_DIR/jemm/CURRENT")"
    rm -rf "$VM_WINDOW_DIR/session"
    bash scripts/build_vm_session.sh --output "$VM_WINDOW_DIR/session"
    nasm -f bin src/com/dpmirun.asm -o "$VM_WINDOW_DIR/DPMIRUN.COM"
    nasm -f bin src/com/vmstart.asm -o "$VM_WINDOW_DIR/VMSTART.COM"
    nasm -f bin src/com/vmfork.asm -o "$VM_WINDOW_DIR/VMFORK.COM"
    mtools_ensure_dir "$IMG" ::VM
    mcopy -o -i "$IMG" "$vm_jemm/JEMM386.EXE" "$vm_jemm/JLOAD.EXE" ::VM/
    mcopy -o -i "$IMG" "$VM_WINDOW_DIR/session/CVSESSION.DLL" ::VM/CVSESS.DLL
    mcopy -o -i "$IMG" "$VM_WINDOW_DIR/DPMIRUN.COM" "$VM_WINDOW_DIR/VMSTART.COM" \
        "$VM_WINDOW_DIR/VMFORK.COM" ::VM/
    mcopy -o -i "$IMG" config/vm-window/README.TXT ::VM/README.TXT
    mcopy -o -i "$IMG" "$vm_jemm/ARTISTIC.TXT" ::VM/JEMM.TXT
    mcopy -o -i "$IMG" "$vm_jemm/JLOAD-LICENSE.TXT" ::VM/JLOAD.TXT
    mcopy -o -i "$IMG" "$vm_jemm/CIUKIOS-MODIFICATIONS.TXT" ::VM/JEMMMODS.TXT
fi
echo "[build-full] injecting official system icons and credits to ::SYSTEM/UI"
mtools_ensure_dir "$IMG" ::SYSTEM/UI
python3 scripts/build_desktop_assets.py
mcopy -o -i "$IMG" assets/desktop/DESKTOP.DAT ::SYSTEM/UI/DESKTOP.DAT
# Desktop applications (Files, Notepad, Tasks): modules hosted by SHELL.COM.
bash scripts/build_apps.sh build/full/obj/apps
mtools_ensure_dir "$IMG" ::SYSTEM/APPS
for app_module in build/full/obj/apps/*.APP; do
	mcopy -o -i "$IMG" "$app_module" ::SYSTEM/APPS/
done
mcopy -o -i "$IMG" assets/icons/native/ICONS.DAT ::SYSTEM/UI/ICONS.DAT
mcopy -o -i "$IMG" assets/icons/CREDITS.TXT ::SYSTEM/UI/CREDITS.TXT
mcopy -o -i "$IMG" assets/icons/upstream/COPYING ::SYSTEM/UI/TANGO.TXT
mcopy -o -i "$IMG" assets/icons/upstream/AUTHORS ::SYSTEM/UI/AUTHORS.TXT
wallpaper_args=(--output build/full/obj/wallpapers)
if [[ "${CIUKIOS_PERSONAL_WALLPAPERS:-0}" == "1" ]]; then
    wallpaper_args+=(--include-user-wallpapers)
fi
python3 scripts/build_wallpapers.py "${wallpaper_args[@]}"
mcopy -o -i "$IMG" build/full/obj/wallpapers/WALLS.DAT ::SYSTEM/UI/WALLS.DAT
echo "[build-full] injecting the desktop fonts to ::SYSTEM/FONTS"
python3 scripts/build_fonts.py --check
mtools_ensure_dir "$IMG" ::SYSTEM/FONTS
for font_file in assets/fonts/native/*.CFN; do
	mcopy -o -i "$IMG" "$font_file" ::SYSTEM/FONTS/
done
mcopy -o -i "$IMG" assets/fonts/LICENSES.TXT ::SYSTEM/FONTS/LICENSES.TXT
mtools_ensure_dir "$IMG" ::DESKTOP
mtools_ensure_dir "$IMG" ::PROGRAMS
mtools_ensure_dir "$IMG" ::SYSTEM/CONFIG
mtools_ensure_dir "$IMG" ::SYSTEM/TEST
echo "[build-full] injecting CiukiOS software OpenGL prototype"
bash scripts/build_opengl.sh build/full/obj/gl
mtools_ensure_dir "$IMG" ::SYSTEM/GL
mtools_ensure_dir "$IMG" ::PROGRAMS/CiukGL
for gl_file in TINYGL.LIB GL.H CIUKGL.H LICENSE.TXT; do
    mcopy -o -i "$IMG" "build/full/obj/gl/$gl_file" "::SYSTEM/GL/$gl_file"
done
mcopy -o -i "$IMG" src/probes/gl/README.TXT ::SYSTEM/GL/README.TXT
mcopy -o -i "$IMG" build/full/obj/gl/GLDEMO.EXE ::PROGRAMS/CiukGL/GLDEMO.EXE
mcopy -o -i "$IMG" src/probes/gl/README.TXT ::PROGRAMS/CiukGL/README.TXT
mcopy -o -i "$IMG" assets/wallpapers/LICENSE.txt ::SYSTEM/UI/WALLLIC.TXT
mcopy -o -i "$IMG" assets/wallpapers/README.md ::SYSTEM/UI/WALLINFO.TXT
for wallpaper_file in build/full/obj/wallpapers/WALL[0-9][0-9].CWP; do
    [[ -f "$wallpaper_file" ]] || continue
    mcopy -o -i "$IMG" "$wallpaper_file" ::SYSTEM/UI/
done
printf 'LIVE' > build/full/obj/STARTUP.CFG
mcopy -o -i "$IMG" build/full/obj/STARTUP.CFG ::SYSTEM/STARTUP.CFG
mcopy -o -i "$IMG" src/com/setup_font.LICENSE ::APPS/SETUPFNT.TXT

echo "[build-full] injecting removable-media reader to ::APPS/MEDIA.COM"
mcopy -o -i "$IMG" build/full/obj/media.com ::APPS/MEDIA.COM
mcopy -o -i "$IMG" src/com/media.txt ::APPS/MEDIA.TXT

echo "[build-full] injecting CIUKIDOS kernel to ::SYSTEM/CIUKIDOS.SYS"
mcopy -o -i "$IMG" "$RUNTIME_BIN" ::SYSTEM/CIUKIDOS.SYS

# Long file names: the kernel's resident LFN extension, built against this
# kernel's listing (it calls the kernel's disk routines); the shell loads it.
echo "[build-full] injecting the long file name extension to ::SYSTEM/LFN.COM"
bash scripts/build_lfn.sh build/full/obj/ciukidos.lst build/full/obj/lfn
mcopy -o -i "$IMG" build/full/obj/lfn/LFN.COM ::SYSTEM/LFN.COM

echo "[build-full] injecting CIUKIDOS ownership probe to ::APPS/CIUKRTST.COM"
mcopy -o -i "$IMG" "$CIUKRTST_BIN" ::APPS/CIUKRTST.COM

echo "[build-full] injecting INT 33h callback probe to ::APPS/MOUSECB.COM"
mcopy -o -i "$IMG" "$MOUSECB_BIN" ::APPS/MOUSECB.COM

echo "[build-full] injecting CIUKIDOS nested process probes to ::APPS"
mcopy -o -i "$IMG" "$CIUKPST_BIN" ::APPS/CIUKPST.COM
mcopy -o -i "$IMG" "$CIUKPTRM_BIN" ::APPS/CIUKTRM.EXE
mcopy -o -i "$IMG" "$CIUKPCOM_BIN" ::APPS/CIUKPCOM.COM

echo "[build-full] injecting DRVLOAD.COM helper to ${DRIVERS_IMAGE_DIR%/}/DRVLOAD.COM"
mtools_ensure_dir "$IMG" "$DRIVERS_IMAGE_DIR"
mcopy -o -i "$IMG" "$DRVLOAD_BIN" "${DRIVERS_IMAGE_DIR%/}/DRVLOAD.COM"

echo "[build-full] injecting MOUSE.COM helper to ::SYSTEM/MOUSE.COM"
mcopy -o -i "$IMG" "$MOUSE_BIN" ::SYSTEM/MOUSE.COM

echo "[build-full] shell-only profile: external desktop payload injection disabled"
if [[ -d "$DOOM_SRC_DIR" ]]; then
    if ! command -v mmd >/dev/null 2>&1 || ! command -v mdir >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
	    echo "[build-full] ERROR: DOOM source present but mtools (mmd/mdir/mcopy) is missing" >&2
        exit 1
    fi

    echo "[build-full] injecting local Doom payload from $DOOM_SRC_DIR to $DOOM_IMAGE_DIR"
	mtools_ensure_dir "$IMG" "$DOOM_IMAGE_DIR"

    shopt -s nullglob dotglob
    doom_items=("$DOOM_SRC_DIR"/*)
    shopt -u nullglob dotglob
    if (( ${#doom_items[@]} > 0 )); then
        mcopy -s -o -i "$IMG" "${doom_items[@]}" "$DOOM_IMAGE_DIR/"
        echo "[build-full] creating DOOM runtime data directory at $DOOMDATA_IMAGE_DIR"
        mtools_ensure_dir "$IMG" "$DOOMDATA_IMAGE_DIR"
        if [[ -f "$DOOM_SRC_DIR/DEFAULT.CFG" ]]; then
            doom_cfg_out="build/full/obj/doom-default.cfg"
            cp "$DOOM_SRC_DIR/DEFAULT.CFG" "$doom_cfg_out"
            case "$DOOM_AUDIO_PROFILE" in
                adlib-pcspeaker)
                    sed -i -E 's/^snd_channels[[:space:]].*/snd_channels		8/; s/^snd_musicdevice[[:space:]].*/snd_musicdevice		3/; s/^snd_sfxdevice[[:space:]].*/snd_sfxdevice		1/; s/^snd_mport[[:space:]].*/snd_mport		-1/' "$doom_cfg_out"
                    echo "[build-full] DOOM audio profile: adlib-pcspeaker (OPL2 music + stable PC-speaker SFX)"
                    ;;
                music-only)
                    sed -i -E 's/^snd_channels[[:space:]].*/snd_channels		0/; s/^snd_musicdevice[[:space:]].*/snd_musicdevice		3/; s/^snd_sfxdevice[[:space:]].*/snd_sfxdevice		0/; s/^snd_sbirq[[:space:]].*/snd_sbirq		7/; s/^snd_sbdma[[:space:]].*/snd_sbdma		1/' "$doom_cfg_out"
                    echo "[build-full] DOOM audio profile: music-only (SFX disabled)"
                    ;;
                pcspeaker-sfx)
                    sed -i -E 's/^snd_channels[[:space:]].*/snd_channels		8/; s/^snd_musicdevice[[:space:]].*/snd_musicdevice		0/; s/^snd_sfxdevice[[:space:]].*/snd_sfxdevice		1/; s/^snd_mport[[:space:]].*/snd_mport		-1/' "$doom_cfg_out"
                    echo "[build-full] DOOM audio profile: pcspeaker-sfx (PC speaker SFX enabled, music disabled)"
                    ;;
                sb16)
                    if [[ -f "$DOOM_SRC_DIR/SB16/DEFAULT.CFG" ]]; then
                        cp "$DOOM_SRC_DIR/SB16/DEFAULT.CFG" "$doom_cfg_out"
                    fi
                    sed -i -E 's/^snd_channels[[:space:]].*/snd_channels		8/; s/^snd_musicdevice[[:space:]].*/snd_musicdevice		3/; s/^snd_sfxdevice[[:space:]].*/snd_sfxdevice		3/; s/^snd_sbport[[:space:]].*/snd_sbport		544/; s/^snd_sbirq[[:space:]].*/snd_sbirq		7/; s/^snd_sbdma[[:space:]].*/snd_sbdma		1/; s/^snd_mport[[:space:]].*/snd_mport		816/' "$doom_cfg_out"
                    echo "[build-full] DOOM audio profile: sb16 (SFX enabled)"
                    ;;
                sb16-sfx)
                    if [[ -f "$DOOM_SRC_DIR/SB16/DEFAULT.CFG" ]]; then
                        cp "$DOOM_SRC_DIR/SB16/DEFAULT.CFG" "$doom_cfg_out"
                    fi
                    sed -i -E 's/^snd_channels[[:space:]].*/snd_channels		8/; s/^snd_musicdevice[[:space:]].*/snd_musicdevice		0/; s/^snd_sfxdevice[[:space:]].*/snd_sfxdevice		3/; s/^snd_sbport[[:space:]].*/snd_sbport		544/; s/^snd_sbirq[[:space:]].*/snd_sbirq		7/; s/^snd_sbdma[[:space:]].*/snd_sbdma		1/; s/^snd_mport[[:space:]].*/snd_mport		-1/' "$doom_cfg_out"
                    echo "[build-full] DOOM audio profile: sb16-sfx (SFX enabled, music disabled)"
                    ;;
                *)
                    echo "[build-full] ERROR: unsupported DOOM audio profile: $DOOM_AUDIO_PROFILE (expected adlib-pcspeaker, music-only, pcspeaker-sfx, sb16 or sb16-sfx)" >&2
                    exit 1
                    ;;
            esac
            echo "[build-full] injecting Doom DEFAULT.CFG to $DOOM_IMAGE_DIR and $DOOMDATA_IMAGE_DIR"
            mcopy -o -i "$IMG" "$doom_cfg_out" "$DOOM_IMAGE_DIR/DEFAULT.CFG"
            mcopy -o -i "$IMG" "$doom_cfg_out" "$DOOMDATA_IMAGE_DIR/DEFAULT.CFG"
        fi
        if [[ -f "$DOOM_SRC_DIR/SB16/DEFAULT.CFG" ]]; then
            doomsb_cfg_out="build/full/obj/doomsb-default.cfg"
            cp "$DOOM_SRC_DIR/SB16/DEFAULT.CFG" "$doomsb_cfg_out"
            sed -i -E 's/^snd_channels[[:space:]].*/snd_channels		8/; s/^snd_musicdevice[[:space:]].*/snd_musicdevice		0/; s/^snd_sfxdevice[[:space:]].*/snd_sfxdevice		3/; s/^snd_sbport[[:space:]].*/snd_sbport		544/; s/^snd_sbirq[[:space:]].*/snd_sbirq		7/; s/^snd_sbdma[[:space:]].*/snd_sbdma		1/; s/^snd_mport[[:space:]].*/snd_mport		-1/' "$doomsb_cfg_out"
            echo "[build-full] injecting DOOMSB.CFG helper profile to $DOOM_IMAGE_DIR"
            mcopy -o -i "$IMG" "$doomsb_cfg_out" "$DOOM_IMAGE_DIR/DOOMSB.CFG"
        fi
        echo "[build-full] injecting DOOMSB.COM helper to $DOOM_IMAGE_DIR"
        mcopy -o -i "$IMG" "$DOOMSB_BIN" "$DOOM_IMAGE_DIR/DOOMSB.COM"
        # Preserve the engine under a private name; explicit .EXE launches
        # must not silently bypass the transient PCI-audio driver.
        [[ -s "$DOOM_SRC_DIR/DOOM.EXE" ]] || { echo '[build-full] ERROR missing Doom engine' >&2; exit 1; }
        mcopy -o -i "$IMG" "$DOOM_SRC_DIR/DOOM.EXE" "$DOOM_IMAGE_DIR/DOOMCORE.EXE"
        mcopy -o -i "$IMG" "$DOOM_LAUNCH_MZ" "$DOOM_IMAGE_DIR/DOOM.EXE"
        echo "[build-full] injecting DOOM.COM/EXE transient AC97 launchers to $DOOM_IMAGE_DIR"
        mcopy -o -i "$IMG" "$DOOM_LAUNCH_BIN" "$DOOM_IMAGE_DIR/DOOM.COM"
        mcopy -o -i "$IMG" build/full/obj/doomsafe.com "$DOOM_IMAGE_DIR/DOOMSAFE.COM"
    else
        echo "[build-full] WARN: Doom source directory is empty: $DOOM_SRC_DIR" >&2
    fi
else
    echo "[build-full] doom payload not found at $DOOM_SRC_DIR (skipped)"
fi

if [[ -f "$DOOMVAN_EXE" && -d "$DOOM_SRC_DIR" ]]; then
	if ! command -v mmd >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
		echo "[build-full] ERROR: DOOMVAN package requested but mtools (mmd/mcopy) is missing" >&2
		exit 1
	fi
	echo "[build-full] injecting doom-vanille probe from $DOOMVAN_EXE to $DOOMVAN_IMAGE_DIR"
	mtools_ensure_dir "$IMG" "$DOOMVAN_IMAGE_DIR"
	mcopy -o -i "$IMG" "$DOOMVAN_LAUNCH_BIN" "$DOOMVAN_IMAGE_DIR/DOOMVAN.COM"
	mcopy -o -i "$IMG" "$DOOMVAN_EXE" "$DOOMVAN_IMAGE_DIR/PCDMCORE.EXE"
	for doomvan_entry in DOOM.EXE DOOMVAN.EXE PCDOOM.EXE; do
		mcopy -o -i "$IMG" "$DOOMVAN_LAUNCH_MZ" "$DOOMVAN_IMAGE_DIR/$doomvan_entry"
	done
	if [[ -f "$DOS4GW_BIN" ]]; then
		mcopy -o -i "$IMG" "$DOS4GW_BIN" "$DOOMVAN_IMAGE_DIR/DOS4GW.EXE"
	fi
	if [[ -f "$DOOM_SRC_DIR/DOOM.WAD" ]]; then
		mcopy -o -i "$IMG" "$DOOM_SRC_DIR/DOOM.WAD" "$DOOMVAN_IMAGE_DIR/DOOM.WAD"
	fi
	if [[ -f "$DOOM_SRC_DIR/DEFAULT.CFG" ]]; then
		doomvan_cfg_out="build/full/obj/doomvan-default.cfg"
		cp "$DOOM_SRC_DIR/DEFAULT.CFG" "$doomvan_cfg_out"
		sed -i -E 's/^sfx_volume[[:space:]].*/sfx_volume\t\t15/; s/^music_volume[[:space:]].*/music_volume\t\t15/' "$doomvan_cfg_out"
		sed -i -E 's/^snd_channels[[:space:]].*/snd_channels		8/; s/^snd_musicdevice[[:space:]].*/snd_musicdevice		3/; s/^snd_sfxdevice[[:space:]].*/snd_sfxdevice		3/; s/^snd_sbport[[:space:]].*/snd_sbport		544/; s/^snd_sbirq[[:space:]].*/snd_sbirq		7/; s/^snd_sbdma[[:space:]].*/snd_sbdma		1/; s/^snd_mport[[:space:]].*/snd_mport		816/' "$doomvan_cfg_out"
		echo "[build-full] injecting doom-vanille transient AC97/SB16 profile to $DOOMVAN_IMAGE_DIR"
		mcopy -o -i "$IMG" "$doomvan_cfg_out" "$DOOMVAN_IMAGE_DIR/DEFAULT.CFG"
	fi
else
	echo "[build-full] doom-vanille probe executable not found at $DOOMVAN_EXE (skipped)"
fi

if [[ -d "$DOSNAV_SRC_DIR" ]]; then
	if ! command -v mmd >/dev/null 2>&1 || ! command -v mdir >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
		echo "[build-full] ERROR: DOSNavigator source present but mtools (mmd/mdir/mcopy) is missing" >&2
		exit 1
	fi

	echo "[build-full] injecting DOSNavigator payload from $DOSNAV_SRC_DIR to $DOSNAV_IMAGE_DIR"
	mtools_ensure_dir "$IMG" "$DOSNAV_IMAGE_DIR"

	shopt -s nullglob dotglob
	dosnav_items=("$DOSNAV_SRC_DIR"/*)
	shopt -u nullglob dotglob
	if (( ${#dosnav_items[@]} > 0 )); then
		mcopy -s -o -i "$IMG" "${dosnav_items[@]}" "$DOSNAV_IMAGE_DIR/"
	else
		echo "[build-full] WARN: DOSNavigator source directory is empty: $DOSNAV_SRC_DIR" >&2
	fi
else
	echo "[build-full] DOSNavigator payload not found at $DOSNAV_SRC_DIR (skipped)"
fi

if [[ -d "$WOLF3D_SRC_DIR" ]]; then
	if ! command -v mmd >/dev/null 2>&1 || ! command -v mdir >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
		echo "[build-full] ERROR: WOLF3D source present but mtools (mmd/mdir/mcopy) is missing" >&2
		exit 1
	fi

	echo "[build-full] injecting WOLF3D payload from $WOLF3D_SRC_DIR to $WOLF3D_IMAGE_DIR"
	mtools_ensure_dir "$IMG" "$WOLF3D_IMAGE_DIR"

	wolf3d_payload_dir="build/full/obj/wolf3d-payload"
	rm -rf "$wolf3d_payload_dir"
	mkdir -p "$wolf3d_payload_dir"
	cp -a "$WOLF3D_SRC_DIR"/. "$wolf3d_payload_dir"/
	if [[ -f "$wolf3d_payload_dir/WOLF3D.EXE" ]]; then
		python3 - "$wolf3d_payload_dir/WOLF3D.EXE" <<PY
from pathlib import Path
import sys

path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
offset = 0x2092C
expected = bytes.fromhex("8b0ef89583c1")
old_patched = bytes.fromhex("eb44f89583c1")
patched = bytes.fromhex("bada03eceb40")
current = bytes(data[offset:offset + 6])
if current == expected:
    data[offset:offset + 6] = patched
    path.write_bytes(data)
    print("[build-full] patched WOLF3D page flip with VGA attribute-controller reset")
elif current == old_patched:
    data[offset:offset + 6] = patched
    path.write_bytes(data)
    print("[build-full] upgraded WOLF3D page-flip patch with VGA attribute-controller reset")
elif current == patched:
    print("[build-full] WOLF3D page-flip patch already present")
else:
    raise SystemExit(f"[build-full] ERROR: unexpected WOLF3D.EXE bytes at 0x{offset:x}: {current.hex()}")
PY
	fi
	if [[ -f "$wolf3d_payload_dir/CONFIG.WL6" ]]; then
		if [[ "$(stat -c%s "$wolf3d_payload_dir/CONFIG.WL6")" -lt 468 ]]; then
			echo "[build-full] ERROR: WOLF3D CONFIG.WL6 is too small for audio profile" >&2
			exit 1
		fi
		# Wolf4GW stores SoundMode, MusicMode and DigiMode as 16-bit enums.
		# Select AdLib SFX/music plus Sound Blaster digital audio; VSBHDA maps
		# those ports to the detected AC97/HDA/PCI controller for one process.
		printf '\x02\x00\x01\x00\x03\x00' \
			| dd of="$wolf3d_payload_dir/CONFIG.WL6" bs=1 seek=$((0x1CE)) count=6 conv=notrunc status=none
		echo "[build-full] WOLF3D profile: transient AC97 Sound Blaster + AdLib"
	fi

	shopt -s nullglob dotglob
	wolf3d_items=("$wolf3d_payload_dir"/*)
	shopt -u nullglob dotglob
	if (( ${#wolf3d_items[@]} > 0 )); then
		mcopy -s -o -i "$IMG" "${wolf3d_items[@]}" "$WOLF3D_IMAGE_DIR/"
	else
		echo "[build-full] WARN: WOLF3D source directory is empty: $WOLF3D_SRC_DIR" >&2
	fi
	if [[ "$WOLF4GW_MODE" != "off" && -s "$WOLF4GW_EXE" ]]; then
		mcopy -o -i "$IMG" "$WOLF3D_LAUNCH_BIN" "$WOLF3D_IMAGE_DIR/WOLF3D.COM"
		mcopy -o -i "$IMG" "$WOLF4GW_EXE" "$WOLF3D_IMAGE_DIR/WOLF4GW.EXE"
		[[ ! -s "$WOLF4GW_NOTICE" ]] || mcopy -o -i "$IMG" "$WOLF4GW_NOTICE" "$WOLF3D_IMAGE_DIR/WOLF4GW.TXT"
		if [[ -s "$DOS4GW_BIN" ]]; then
			mcopy -o -i "$IMG" "$DOS4GW_BIN" "$WOLF3D_IMAGE_DIR/DOS4GW.EXE"
		else
			echo "[build-full] ERROR: Wolf4GW requires DOS4GW at $DOS4GW_BIN" >&2
			exit 1
		fi
		echo "[build-full] WOLF3D.COM selects transient VSBHDA AC97 audio; WOLF3D.EXE remains the 16-bit fallback"
	fi
	# Regression fixture for INT 21h AH=4Bh relative multi-component subpath exec:
	# ship a genuine MZDEMO.EXE inside \APPS\WOLF3D so 'run WOLF3D\MZDEMO.EXE' from
	# \APPS can be validated deterministically (see qemu_test_full_shell_com.sh).
	mcopy -o -i "$IMG" "$MZDEMO_BIN" "$WOLF3D_IMAGE_DIR/MZDEMO.EXE"
else
	echo "[build-full] WOLF3D payload not found at $WOLF3D_SRC_DIR (skipped)"
fi

if [[ -d "$COSTA_SRC_DIR" ]]; then
	for costa_required in COSTA.EXE DESKTOP.EXE DATA/FONTDATA.BSV DATA/FONTINFO.BSV LICENSE; do
		if [[ ! -s "$COSTA_SRC_DIR/$costa_required" ]]; then
			echo "[build-full] ERROR: incomplete Costa payload, missing: $COSTA_SRC_DIR/$costa_required" >&2
			exit 1
		fi
	done
	if ! command -v mmd >/dev/null 2>&1 || ! command -v mdir >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
		echo "[build-full] ERROR: Costa source present but mtools (mmd/mdir/mcopy) is missing" >&2
		exit 1
	fi

	echo "[build-full] injecting Costa payload from $COSTA_SRC_DIR to $COSTA_IMAGE_DIR"
	mtools_ensure_dir "$IMG" "$COSTA_IMAGE_DIR"
	shopt -s nullglob dotglob
	costa_items=("$COSTA_SRC_DIR"/*)
	shopt -u nullglob dotglob
	if (( ${#costa_items[@]} == 0 )); then
		echo "[build-full] ERROR: Costa source directory is empty: $COSTA_SRC_DIR" >&2
		exit 1
	fi
	mcopy -s -o -i "$IMG" "${costa_items[@]}" "$COSTA_IMAGE_DIR/"
else
	echo "[build-full] Costa payload not found at $COSTA_SRC_DIR (run scripts/fetch_costa.sh to install it)"
fi

if [[ -d "$NETWORK_SRC_DIR" ]]; then
	if ! command -v mmd >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
		echo "[build-full] ERROR: network payload present but mtools (mmd/mcopy) is missing" >&2
		exit 1
	fi

	network_required_files=(
		"MTCP/dhcp.exe"
		"MTCP/ftp.exe"
		"MTCP/ftpsrv.exe"
		"MTCP/ping.exe"
		"MTCP/htget.exe"
		"MTCP/pkttool.exe"
		"MTCP/COPYING.TXT"
		"MTCP/SOURCES.ZIP"
		"PACKET/NE2000.COM"
		"PACKET/PKTCHK.COM"
		"PACKET/GPL.DOC"
		"PACKET/SOURCES.ZIP"
	)
	for network_required_file in "${network_required_files[@]}"; do
		if [[ ! -s "$NETWORK_SRC_DIR/$network_required_file" ]]; then
			echo "[build-full] ERROR: incomplete network payload: missing $NETWORK_SRC_DIR/$network_required_file" >&2
			exit 1
		fi
	done
	for network_config_file in MTCP.CFG FTPPASS.TXT README.TXT; do
		if [[ ! -s "$NETWORK_CONFIG_DIR/$network_config_file" ]]; then
			echo "[build-full] ERROR: missing network configuration: $NETWORK_CONFIG_DIR/$network_config_file" >&2
			exit 1
		fi
	done

	echo "[build-full] injecting mTCP/Crynwr network payload to $NETWORK_IMAGE_DIR"
	mtools_ensure_dir "$IMG" "$NETWORK_IMAGE_DIR"
	mtools_ensure_dir "$IMG" "$NETWORK_IMAGE_DIR/SOURCE"
	mtools_ensure_dir "$IMG" "$NETWORK_SHARE_IMAGE_DIR"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/PACKET/NE2000.COM" "$NETWORK_IMAGE_DIR/NE2000.COM"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/PACKET/PKTCHK.COM" "$NETWORK_IMAGE_DIR/PKTCHK.COM"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/MTCP/dhcp.exe" "$NETWORK_IMAGE_DIR/DHCP.EXE"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/MTCP/ftp.exe" "$NETWORK_IMAGE_DIR/FTP.EXE"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/MTCP/ftpsrv.exe" "$NETWORK_IMAGE_DIR/FTPSRV.EXE"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/MTCP/ping.exe" "$NETWORK_IMAGE_DIR/PING.EXE"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/MTCP/htget.exe" "$NETWORK_IMAGE_DIR/HTGET.EXE"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/MTCP/pkttool.exe" "$NETWORK_IMAGE_DIR/PKTTOOL.EXE"
	mcopy -o -i "$IMG" "$IPCONFIG_BIN" "$NETWORK_IMAGE_DIR/IPCONFIG.COM"
	mcopy -o -i "$IMG" "$ICMPD_BIN" "$NETWORK_IMAGE_DIR/ICMPD.COM"
	mcopy -o -i "$IMG" "$ICMPD_BIN" "$NETWORK_IMAGE_DIR/NETSTART.COM"
	mcopy -o -i "$IMG" "$NETCFG_BIN" "$NETWORK_IMAGE_DIR/NETCFG.COM"
	mcopy -o -i "$IMG" "$NETWORK_CONFIG_DIR/MTCP.CFG" "$NETWORK_IMAGE_DIR/MTCP.CFG"
	mcopy -o -i "$IMG" "$NETWORK_CONFIG_DIR/FTPPASS.TXT" "$NETWORK_IMAGE_DIR/FTPPASS.TXT"
	mcopy -o -i "$IMG" "$NETWORK_CONFIG_DIR/README.TXT" "$NETWORK_IMAGE_DIR/README.TXT"
	mcopy -o -i "$IMG" "$NETWORK_CONFIG_DIR/README.TXT" "$NETWORK_SHARE_IMAGE_DIR/README.TXT"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/MTCP/COPYING.TXT" "$NETWORK_IMAGE_DIR/MTCP-GPL.TXT"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/PACKET/GPL.DOC" "$NETWORK_IMAGE_DIR/CRYNWR-GPL.TXT"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/MTCP/SOURCES.ZIP" "$NETWORK_IMAGE_DIR/SOURCE/MTCP-SRC.ZIP"
	mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/PACKET/SOURCES.ZIP" "$NETWORK_IMAGE_DIR/SOURCE/CRYNWR-SRC.ZIP"
else
	echo "[build-full] network payload not found at $NETWORK_SRC_DIR (run scripts/fetch_network_stack.sh)"
fi

echo "[build-full] injecting SB16INIT.COM helper to ${DRIVERS_IMAGE_DIR%/}/SB16INIT.COM"
mtools_ensure_dir "$IMG" "$DRIVERS_IMAGE_DIR"
mcopy -o -i "$IMG" "$SB16INIT_BIN" "${DRIVERS_IMAGE_DIR%/}/SB16INIT.COM"
echo "[build-full] injecting AUDIOTST.COM helper to ${DRIVERS_IMAGE_DIR%/}/AUDIOTST.COM"
mcopy -o -i "$IMG" "$AUDIOTST_BIN" "${DRIVERS_IMAGE_DIR%/}/AUDIOTST.COM"
echo "[build-full] injecting AC97INIT.COM helper to ${DRIVERS_IMAGE_DIR%/}/AC97INIT.COM"
mcopy -o -i "$IMG" "$AC97INIT_BIN" "${DRIVERS_IMAGE_DIR%/}/AC97INIT.COM"
mcopy -o -i "$IMG" build/full/obj/bootsnd.com "${DRIVERS_IMAGE_DIR%/}/SOUND.COM"
mcopy -o -i "$IMG" build/full/obj/sbstart.com "${DRIVERS_IMAGE_DIR%/}/SBSTART.COM"
mcopy -o -i "$IMG" build/full/obj/bootsnd.com ::SYSTEM/BOOTSND.COM
mcopy -o -i "$IMG" build/full/obj/sfx.drv "${DRIVERS_IMAGE_DIR%/}/SFX.DRV"
mcopy -o -i "$IMG" build/full/obj/media-driver/media.drv ::SYSTEM/MEDIA.DRV
mtools_ensure_dir "$IMG" ::SYSTEM/SOUNDS
for system_sound_file in build/full/obj/system-sounds/*.PCM build/full/obj/system-sounds/*.SB build/full/obj/system-sounds/*.TXT; do
    mcopy -o -i "$IMG" "$system_sound_file" ::SYSTEM/SOUNDS/
done
mcopy -o -i "$IMG" build/full/obj/BOOT.PCM ::SYSTEM/BOOT.PCM
echo "[build-full] injecting AUDIO.COM hardware dispatcher to ${DRIVERS_IMAGE_DIR%/}/AUDIO.COM"
mcopy -o -i "$IMG" "$AUDIOAUTO_BIN" "${DRIVERS_IMAGE_DIR%/}/AUDIO.COM"
echo "[build-full] injecting AUDIOKEY.COM helper to ${DRIVERS_IMAGE_DIR%/}/AUDIOKEY.COM"
mcopy -o -i "$IMG" "$AUDIOKEY_BIN" "${DRIVERS_IMAGE_DIR%/}/AUDIOKEY.COM"
echo "[build-full] injecting AKEY.COM alias to ${DRIVERS_IMAGE_DIR%/}/AKEY.COM"
mcopy -o -i "$IMG" "$AUDIOKEY_BIN" "${DRIVERS_IMAGE_DIR%/}/AKEY.COM"
echo "[build-full] injecting VGASETUP.COM display manager"
# The desktop keeps its resolution profile in C:\SYSTEM\VIDEO\DISPLAY.CFG.
mtools_ensure_dir "$IMG" ::SYSTEM/VIDEO
echo "[build-full] building the VBE DOS helpers into ::SYSTEM/VIDEO"
VBESVGA_OUTPUT_DIR="$VBESVGA_OUTPUT_DIR" bash "$VBESVGA_BUILD_SCRIPT"
for vbe_helper in "${VBE_DOS_HELPERS[@]}"; do
	[[ -s "$VBESVGA_OUTPUT_DIR/$vbe_helper" ]] \
		|| { echo "[build-full] ERROR: VBE DOS helper missing: $vbe_helper" >&2; exit 1; }
	mcopy -o -i "$IMG" "$VBESVGA_OUTPUT_DIR/$vbe_helper" "::SYSTEM/VIDEO/$vbe_helper"
done
# The desktop starts AUXSTACK through AUXSTART, which keeps its banner off
# the boot splash (COM1 only on a graphics screen).
nasm -f bin src/com/auxstart.asm -o build/full/obj/auxstart.com
mcopy -o -i "$IMG" build/full/obj/auxstart.com ::SYSTEM/VIDEO/AUXSTART.COM
mcopy -o -i "$IMG" "$VGASETUP_BIN" "${DRIVERS_IMAGE_DIR%/}/VGASETUP.COM"

if [[ -f "$PMIRQSB_BIN" ]]; then
	echo "[build-full] injecting PMIRQSB.COM launcher to ${DRIVERS_IMAGE_DIR%/}/PMIRQSB.COM"
	mcopy -o -i "$IMG" "$PMIRQSB_LAUNCH_BIN" "${DRIVERS_IMAGE_DIR%/}/PMIRQSB.COM"
	echo "[build-full] injecting PMIRQSB.LE DOS/4GW probe to ${DRIVERS_IMAGE_DIR%/}/PMIRQSB.LE"
	mcopy -o -i "$IMG" "$PMIRQSB_BIN" "${DRIVERS_IMAGE_DIR%/}/PMIRQSB.LE"
	if [[ -f "$DOS4GW_BIN" ]]; then
		echo "[build-full] injecting DOS4GW.EXE runtime to ${DRIVERS_IMAGE_DIR%/}/DOS4GW.EXE"
		mcopy -o -i "$IMG" "$DOS4GW_BIN" "${DRIVERS_IMAGE_DIR%/}/DOS4GW.EXE"
	fi
fi
if [[ -f "$DOOMSFX_BIN" ]]; then
	DOOMAUD_IMAGE_DIR="::APPS/DOOMAUD"
	mtools_ensure_dir "$IMG" "$DOOMAUD_IMAGE_DIR"
	echo "[build-full] injecting DOOMSFX.COM launcher to ${DOOMAUD_IMAGE_DIR%/}/DOOMSFX.COM"
	mcopy -o -i "$IMG" "$DOOMSFX_LAUNCH_BIN" "${DOOMAUD_IMAGE_DIR%/}/DOOMSFX.COM"
	echo "[build-full] injecting DOOMSFX.LE controlled audio lane to ${DOOMAUD_IMAGE_DIR%/}/DOOMSFX.LE"
	mcopy -o -i "$IMG" "$DOOMSFX_BIN" "${DOOMAUD_IMAGE_DIR%/}/DOOMSFX.LE"
fi

# Desktop launchers for the DOS games used by the QEMU compatibility lanes.
# Keep the binaries and their data in their existing paths so game data and
# historical test commands still resolve; the launchers give users one folder.
mtools_ensure_dir "$IMG" "::DESKTOP/TestGames"
test_game_specs=(
	"0:DOOM.COM:APPS/DOOM/DOOMCORE.EXE"
	"1:DOOMVAN.COM:APPS/DOOMVAN/PCDMCORE.EXE"
	"2:WOLF3D.COM:APPS/WOLF3D/WOLF3D.EXE"
)
for test_game_spec in "${test_game_specs[@]}"; do
	IFS=: read -r test_game_kind test_game_name test_game_target <<<"$test_game_spec"
	if mdir -i "$IMG" "::$test_game_target" >/dev/null 2>&1; then
		nasm -f bin -D "GAME_KIND=$test_game_kind" src/com/testgame_launch.asm \
			-o "build/full/obj/$test_game_name"
		mcopy -o -i "$IMG" "build/full/obj/$test_game_name" "::DESKTOP/TestGames/$test_game_name"
	fi
done

if [[ ! -d "$DRIVERS_SRC_DIR" ]]; then
	if [[ "$CIUKIOS_ALLOW_MISSING_DRIVERS" == "1" ]]; then
		echo "[build-full] WARN: drivers payload not found at $DRIVERS_SRC_DIR (skipped by CIUKIOS_ALLOW_MISSING_DRIVERS=1)" >&2
	else
		echo "[build-full] ERROR: drivers source directory not found: $DRIVERS_SRC_DIR (set CIUKIOS_ALLOW_MISSING_DRIVERS=1 to skip intentionally)" >&2
		exit 1
	fi
else
	shopt -s nullglob dotglob
	drivers_items=("$DRIVERS_SRC_DIR"/*)
	shopt -u nullglob dotglob

	if (( ${#drivers_items[@]} == 0 )); then
		if [[ "$CIUKIOS_ALLOW_MISSING_DRIVERS" == "1" ]]; then
			echo "[build-full] WARN: drivers source directory is empty: $DRIVERS_SRC_DIR (skipped by CIUKIOS_ALLOW_MISSING_DRIVERS=1)" >&2
		else
			echo "[build-full] ERROR: drivers source directory is empty: $DRIVERS_SRC_DIR (set CIUKIOS_ALLOW_MISSING_DRIVERS=1 to skip intentionally)" >&2
			exit 1
		fi
	else
		if ! command -v mmd >/dev/null 2>&1 || ! command -v mdir >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
			echo "[build-full] ERROR: drivers source present but mtools (mmd/mdir/mcopy) is missing" >&2
			exit 1
		fi

		if [[ ! -f "$DRIVERS_VERIFY_SCRIPT" ]]; then
			echo "[build-full] ERROR: drivers verifier not found: $DRIVERS_VERIFY_SCRIPT" >&2
			exit 1
		fi

		echo "[build-full] injecting local drivers payload from $DRIVERS_SRC_DIR to $DRIVERS_IMAGE_DIR"
		mtools_ensure_dir "$IMG" "$DRIVERS_IMAGE_DIR"
		mcopy -s -o -i "$IMG" "${drivers_items[@]}" "$DRIVERS_IMAGE_DIR/"

		if [[ ! -s "$CTMOUSE_BIN" || ! -s "$CTMOUSE_LICENSE" ]]; then
			echo "[build-full] ERROR: GPL CuteMouse payload is incomplete: $CTMOUSE_BIN / $CTMOUSE_LICENSE" >&2
			exit 1
		fi
		echo "[build-full] injecting GPL CuteMouse to ${DRIVERS_IMAGE_DIR%/}/CTMOUSE.EXE"
		mcopy -o -i "$IMG" "$CTMOUSE_BIN" "${DRIVERS_IMAGE_DIR%/}/CTMOUSE.EXE"
		mcopy -o -i "$IMG" "$CTMOUSE_LICENSE" "${DRIVERS_IMAGE_DIR%/}/CTMOUSE.GPL"

		if ! IMG="$IMG" DRIVERS_SRC_DIR="$DRIVERS_SRC_DIR" DRIVERS_IMAGE_DIR="$DRIVERS_IMAGE_DIR" \
			bash "$DRIVERS_VERIFY_SCRIPT"; then
			echo "[build-full] ERROR: drivers payload verification failed" >&2
			exit 1
		fi
	fi
fi

# Keep repository payload verification exact: project-owned probes are added
# only after the third-party driver tree has been copied and compared.
echo "[build-full] injecting VIDLEAVE.COM video-restore probe to ${DRIVERS_IMAGE_DIR%/}/VIDLEAVE.COM"
mcopy -o -i "$IMG" "$VIDLEAVE_BIN" "${DRIVERS_IMAGE_DIR%/}/VIDLEAVE.COM"
echo "[build-full] injecting SBEMINIT.COM protected-mode PCI audio loader"
mcopy -o -i "$IMG" "$SBEMINIT_BIN" "${DRIVERS_IMAGE_DIR%/}/SBEMINIT.COM"
python3 scripts/package_driver_catalog.py "$IMG" --video "$VBESVGA_OUTPUT_DIR"

case "$SBEMU_MODE" in
	build)
		[[ -x "$SBEMU_BUILD_SCRIPT" ]] \
			|| { echo "[build-full] ERROR: missing SBEMU build script: $SBEMU_BUILD_SCRIPT" >&2; exit 1; }
		bash "$SBEMU_BUILD_SCRIPT"
		;;
	reuse) ;;
	off) ;;
	*)
		echo "[build-full] ERROR: CIUKIOS_SBEMU_MODE must be build, reuse or off" >&2
		exit 1
		;;
esac
if [[ "$SBEMU_MODE" != "off" ]]; then
	for sbemu_file in VSBHDA.EXE VSBHDA16.EXE SNDCARD.DRV HDPMI32I.EXE HDPMI16I.EXE HDPMI.TXT VSBHDA.GPL VSBHDA.TXT DOSXVS.EXE DOSXVS.TXT; do
		[[ -s "$SBEMU_OUTPUT_DIR/$sbemu_file" ]] \
			|| { echo "[build-full] ERROR: incomplete SBEMU fallback output: $SBEMU_OUTPUT_DIR/$sbemu_file" >&2; exit 1; }
	done
	mtools_ensure_dir "$IMG" "$SBEMU_IMAGE_DIR"
	mtools_ensure_dir "$IMG" ::DRIVERS/AUDIO
	for sbemu_file in VSBHDA.EXE VSBHDA16.EXE SNDCARD.DRV HDPMI32I.EXE HDPMI16I.EXE HDPMI.TXT VSBHDA.GPL VSBHDA.TXT DOSXVS.EXE DOSXVS.TXT; do
		mcopy -o -i "$IMG" "$SBEMU_OUTPUT_DIR/$sbemu_file" "${SBEMU_IMAGE_DIR%/}/$sbemu_file"
		mcopy -o -i "$IMG" "$SBEMU_OUTPUT_DIR/$sbemu_file" "::DRIVERS/AUDIO/$sbemu_file"
	done
	echo "[build-full] transient universal AC97/HDA/PCI audio stack ready under $SBEMU_IMAGE_DIR"
fi

if [[ -d "$NETWORK_SRC_DIR/PACKET" ]]; then
    mtools_ensure_dir "$IMG" ::DRIVERS/NET
    for network_driver_file in NE2000.COM PKTCHK.COM GPL.DOC SOURCES.ZIP; do
        mcopy -o -i "$IMG" "$NETWORK_SRC_DIR/PACKET/$network_driver_file" "::DRIVERS/NET/$network_driver_file"
    done
fi



README_OPTIONAL_PAYLOADS="Optional payloads: third_party/drivers is injected under SYSTEM/DRIVERS when available at build time"
if [[ -d "$DOSNAV_SRC_DIR" ]]; then
	README_OPTIONAL_PAYLOADS+=", third_party/DOSNavigator is copied under APPS/DOSNAV when present at build time"
fi

if [[ -d "$WOLF3D_SRC_DIR" ]]; then
	README_OPTIONAL_PAYLOADS+=", third_party/WOLF3D is copied under APPS/WOLF3D when present at build time"
fi

if [[ -d "$NETWORK_SRC_DIR" ]]; then
	README_OPTIONAL_PAYLOADS+=", mTCP/Crynwr networking is available under NET with an FTP sandbox at SHARE"
fi


cat > build/full/README.txt << TXT
CiukiOS Legacy v2 - Full profile (FAT16 baseline)

Image: ciukios-full.img (128MB)
State: BIOS stage0 -> stage1 with full DOS runtime and FAT16 file I/O
Filesystem: FAT16 (SPT=63 Heads=16 128MB) with root directories SYSTEM/APPS
Boot path: stage0 at LBA0, stage1 payload in sectors 2-$((STAGE1_SECTORS + 1))
Data: cluster 2=SYSTEM dir, 3=APPS dir, 4=SYSTEM/STAGE2, ${SYSTEM_SPLASH_CLUSTER}-${SYSTEM_SPLASH_LAST_CLUSTER}=SYSTEM/SPLASH
Data: cluster ${APPS_COMDEMO_CLUSTER}=APPS/COMDEMO, ${APPS_MZDEMO_CLUSTER}=APPS/MZDEMO, ${APPS_FILEIO_CLUSTER}=APPS/FILEIO, ${APPS_DELTEST_CLUSTER}=APPS/DELTEST, ${APPS_CIUKEDIT_CLUSTER}-${APPS_CIUKEDIT_LAST_CLUSTER}=APPS/CIUKEDIT, ${APPS_GFXRECT_CLUSTER}=APPS/GFXRECT, ${APPS_GFXSTAR_CLUSTER}=APPS/GFXSTAR, ${APPS_SETUP_CLUSTER}-${APPS_SETUP_LAST_CLUSTER}=APPS/SETUP, ${APPS_SETUP_MANIFEST_CLUSTER}=APPS/SETUPMFT.BIN, ${APPS_CIUKWIN_CLUSTER}=APPS/CIUKWIN
${README_OPTIONAL_PAYLOADS}
TXT

echo "[build-full] done: $IMG"
