#!/usr/bin/env bash
set -euo pipefail

DRY_RUN=0
VALIDATE_ONLY=0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_DIR=""

usage() {
	cat <<'EOF'
Usage: scripts/setup_prepare_artifacts.sh [options]

Validate and prepare Phase 3.5 setup bootstrap artifacts.

Options:
  --dry-run          Print actions without writing files
  --validate-only    Validate required setup files and exit
  --root <path>      Override repository root
  --output-dir <dir> Override output directory (default: <root>/build/setup)
  -h, --help         Show this help and exit

Examples:
  scripts/setup_prepare_artifacts.sh --dry-run
  scripts/setup_prepare_artifacts.sh --validate-only
  scripts/setup_prepare_artifacts.sh --output-dir build/setup-bootstrap
EOF
}

relative_path() {
	local path="$1"
	case "$path" in
		"${REPO_ROOT}"/*) printf '%s\n' "${path#"${REPO_ROOT}/"}" ;;
		*) printf '%s\n' "$path" ;;
	esac
}

log() {
	printf '[setup-bootstrap] %s\n' "$1"
}

run_cmd() {
	if (( DRY_RUN )); then
		printf '[dry-run] '
		printf '%q ' "$@"
		printf '\n'
	else
		"$@"
	fi
}

require_value() {
	local flag="$1"
	local value="${2:-}"
	if [[ -z "$value" ]]; then
		echo "Error: ${flag} requires a value" >&2
		exit 1
	fi
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--dry-run)
			DRY_RUN=1
			shift
			;;
		--validate-only)
			VALIDATE_ONLY=1
			shift
			;;
		--root)
			require_value "$1" "${2:-}"
			REPO_ROOT="$2"
			shift 2
			;;
		--output-dir)
			require_value "$1" "${2:-}"
			OUTPUT_DIR="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Error: unknown argument '$1'" >&2
			usage
			exit 1
			;;
	esac
done

if [[ ! -d "$REPO_ROOT" ]]; then
	echo "Error: repository root not found: $REPO_ROOT" >&2
	exit 1
fi

REPO_ROOT="$(cd "$REPO_ROOT" && pwd)"
if [[ -z "$OUTPUT_DIR" ]]; then
	OUTPUT_DIR="$REPO_ROOT/build/setup"
elif [[ "$OUTPUT_DIR" != /* ]]; then
	OUTPUT_DIR="$REPO_ROOT/$OUTPUT_DIR"
fi

validate_required_files() {
	local required_files
	required_files=(
		"$REPO_ROOT/setup/README.md"
		"$REPO_ROOT/setup/SETUP_COM_MVP_CHECKLIST.md"
	)

	local missing=0
	local file
	for file in "${required_files[@]}"; do
		if [[ -f "$file" ]]; then
			log "OK: found $(relative_path "$file")"
		else
			log "ERROR: missing $(relative_path "$file")"
			missing=1
		fi
	done

	if (( missing )); then
		return 2
	fi

	return 0
}

write_manifest() {
	local manifest_path="$OUTPUT_DIR/artifacts/setup-bootstrap-manifest.txt"
	local timestamp
	local revision="unknown"
	timestamp="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

	if command -v git >/dev/null 2>&1; then
		if git -C "$REPO_ROOT" rev-parse --short HEAD >/dev/null 2>&1; then
			revision="$(git -C "$REPO_ROOT" rev-parse --short HEAD)"
		fi
	fi

	if (( DRY_RUN )); then
		log "DRY-RUN: would write $(relative_path "$manifest_path")"
		return 0
	fi

	{
		printf 'phase=3.5\n'
		printf 'stream=setup-bootstrap\n'
		printf 'created_at_utc=%s\n' "$timestamp"
		printf 'git_revision=%s\n' "$revision"
		printf 'repo_root=%s\n' "$REPO_ROOT"
		printf 'setup_readme=%s\n' "$(relative_path "$REPO_ROOT/setup/README.md")"
		printf 'setup_checklist=%s\n' "$(relative_path "$REPO_ROOT/setup/SETUP_COM_MVP_CHECKLIST.md")"
	} > "$manifest_path"

	log "Wrote $(relative_path "$manifest_path")"
}

write_setup_file_list() {
	local list_path="$OUTPUT_DIR/artifacts/setup-file-list.txt"

	if (( DRY_RUN )); then
		log "DRY-RUN: would write $(relative_path "$list_path")"
		return 0
	fi

	{
		printf '# Setup stream tracked files\n'
		while IFS= read -r path; do
			printf '%s\n' "${path#"${REPO_ROOT}/"}"
		done < <(find "$REPO_ROOT/setup" -type f | LC_ALL=C sort)
	} > "$list_path"

	log "Wrote $(relative_path "$list_path")"
}

main() {
	log "Validating setup bootstrap inputs"
	if ! validate_required_files; then
		log "Validation failed"
		exit 2
	fi

	if (( VALIDATE_ONLY )); then
		log "Validation completed successfully"
		exit 0
	fi

	log "Preparing output directory: $(relative_path "$OUTPUT_DIR")"
	run_cmd mkdir -p "$OUTPUT_DIR" "$OUTPUT_DIR/artifacts" "$OUTPUT_DIR/logs"

	write_manifest
	write_setup_file_list

	log "Done"
}

main "$@"
